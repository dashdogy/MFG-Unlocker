// DXGI interface signatures follow the public Windows API. Architecture was
// informed by ReShade 6.8.0 (Patrick Mours, BSD-3-Clause OR MIT): create first,
// then own a proxy with an explicit lifetime. No ReShade code is linked.
#include "overlay_dxgi_proxy.h"
#include "overlay_native.h"
#include "overlay_dx12.h"
#include "overlay_dx11.h"
#include "overlay_graphics_api.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "optiscaler_host.h"
#include "vsync_control.h"
#include "latency_probe.h"
#include "latency_probe_ring.h"
#include <d3d12.h>
#include <intrin.h>
#include <mutex>
#if MFG_UNLOCK_XESS
#include "overlay_back_buffers.h"
#include "xess/xess_mod.h"
#include "xess/d3d12_host.h"
#endif
#include <atomic>
#include <climits>
#include <new>
#include <array>
#include <string>
#include <vector>

namespace single_overlay::proxy {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<uint64_t> factories{0}, chains{0}, calls{0}, skipped{0};
std::atomic<uint32_t> liveFactories{0}, liveChains{0};
struct Capacity {
    std::atomic<uint32_t>& live;
    bool held=false;
    explicit Capacity(std::atomic<uint32_t>& value):live(value) {
        uint32_t count=live.load(std::memory_order_relaxed);
        while(count<64)if(live.compare_exchange_weak(count,count+1,std::memory_order_acq_rel)){held=true;break;}
    }
    ~Capacity(){if(held)--live;}
    void Transfer(){held=false;}
};
thread_local uint32_t creationDepth = 0;
struct Creation {
    bool outer = creationDepth++ == 0 && !gInsideOverlay && !native::InsideLoader();
    ~Creation() { --creationDepth; }
};
// A swapchain creation through a factory proxy, whose return gets
// the menu; a frame generator's output chain created inside it does not.
thread_local uint32_t swapchainCreations = 0;
struct SwapchainCreation : Creation {
    SwapchainCreation() { if (outer) ++swapchainCreations; }
    ~SwapchainCreation() { if (outer) --swapchainCreations; }
};
template<class T> struct Lease {
    T* object;
    explicit Lease(T* p) noexcept : object(p) { object->AddRef(); }
    ~Lease() { object->Release(); }
};
constexpr GUID factoryId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x01}};
constexpr GUID chainId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x02}};
constexpr IID factoryIids[] = {__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),__uuidof(IDXGIFactory2),
    __uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),__uuidof(IDXGIFactory6),__uuidof(IDXGIFactory7)};
constexpr IID chainIids[] = {__uuidof(IDXGISwapChain),__uuidof(IDXGISwapChain1),__uuidof(IDXGISwapChain2),
    __uuidof(IDXGISwapChain3),__uuidof(IDXGISwapChain4)};
constexpr size_t factoryLast[] = {11,13,24,25,27,28,29,31};
constexpr size_t chainLast[] = {17,28,35,39,40};
template<size_t N> int Version(REFIID iid, const IID (&known)[N]) noexcept {
    for (size_t i=0;i<N;++i) if (iid == known[i]) return static_cast<int>(i);
    return -1;
}

// Cache only interfaces actually requested by the application. Querying every
// factory revision speculatively can itself replace a native table after an
// existing overlay has initialized it. Each published interface owns a stable
// reference and is never changed while proxy methods can access it.
template<class Base,size_t N> class InterfaceCache {
    Base* root;
    const unsigned rootVersion;
    std::array<std::atomic<Base*>,N> values{};
public:
    InterfaceCache(Base* p,unsigned v):root(p),rootVersion(v){values[v].store(p);}
    ~InterfaceCache(){Clear();}
    // xemfglive.1: a moved chain's object (owned by the proxy's `original`, as the first root).
    void Rebase(Base* p) noexcept {Clear();root=p;values[rootVersion].store(p);}
    void Clear() noexcept {
        for(size_t i=0;i<N;++i){auto* p=values[i].exchange(nullptr);if(p&&i!=rootVersion)p->Release();}
    }
    Base* Find(unsigned v) const noexcept {
        for(size_t i=v;i<N;++i)if(auto* p=values[i].load(std::memory_order_acquire))return p;
        return nullptr;
    }
    bool Acquire(unsigned v,REFIID iid,size_t last) noexcept {
        if(Find(v))return true;
        void* queried=nullptr;
        const HRESULT hr=root->QueryInterface(iid,&queried);
        if(hr!=S_OK||!queried){if(queried)static_cast<IUnknown*>(queried)->Release();return false;}
        auto* pointer=static_cast<Base*>(queried);
        if(!native::PinInterface(pointer,last)){pointer->Release();return false;}
        Base* expected=nullptr;
        if(!values[v].compare_exchange_strong(expected,pointer,std::memory_order_acq_rel))pointer->Release();
        return true;
    }
    template<class T> T* As(unsigned version) const noexcept {return static_cast<T*>(Find(version));}
};
template<class T> bool AlreadyWrapped(T* object, REFIID iid) noexcept {
    ComPtr<IUnknown> own;
    return object->QueryInterface(iid, reinterpret_cast<void**>(own.GetAddressOf())) == S_OK && own;
}
// Which layers a swap-chain creation passed through. "name+0xRVA"
// for an address inside a module image, "(none)" outside one.
void ModuleAt(const void* address,wchar_t* text,size_t count) noexcept {
    HMODULE module=nullptr;
    if (!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&module)||!module) { swprintf_s(text,count,L"(none)"); return; }
    wchar_t path[1024]{};
    const DWORD n=GetModuleFileNameW(module,path,1024);
    const wchar_t* leaf=n&&n<1024?wcsrchr(path,L'\\'):nullptr;
    swprintf_s(text,count,L"%s+0x%llX",leaf?leaf+1:L"(unnamed)",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module)));
}
std::atomic<uint64_t> creationsLogged{0};
constexpr uint64_t kCreationLines=16;
void LogSwapchainCreation(const char* method,void* caller,IUnknown* below,HRESULT hr,IUnknown* chain,bool outer) noexcept {
    const uint64_t line=creationsLogged.fetch_add(1,std::memory_order_relaxed)+1;
    if (line>kCreationLines) return;
    wchar_t from[160]{},factory[224]{},created[224]{},text[900]{};
    ModuleAt(caller,from,160);
    Describe(below,factory,224);
    if (SUCCEEDED(hr)) Describe(chain,created,224); else swprintf_s(created,224,L"(failed)");
    swprintf_s(text,L"MFG_PROXY_UI swapchain creation %llu: method=%hs hr=0x%08lX outer=%u caller=%s factoryBelow={%s} chain={%s}",
        static_cast<unsigned long long>(line),method,static_cast<unsigned long>(hr),static_cast<unsigned>(outer),from,factory,created);
    single_module::Log(text);
}

void WrapSwapchain(IDXGISwapChain** result, IUnknown* queue, IDXGIFactory* parent) noexcept;
void WrapStreamlineBase(IDXGISwapChain** result, IUnknown* queue) noexcept;
#if MFG_UNLOCK_XESS
bool WrapHostSwapchain(IDXGISwapChain** result, IUnknown* queue, IDXGIFactory* parent) noexcept;
#endif
class FactoryProxy final : public IDXGIFactory7 {
public:
    ComPtr<IDXGIFactory> original;
    InterfaceCache<IDXGIFactory,8> interfaces;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    // menuout.1: the factory Streamline's proxy wraps (UnderStreamline).
    bool streamlineBase = false;
    FactoryProxy(ComPtr<IDXGIFactory>&& base, unsigned v) noexcept : original(std::move(base)),interfaces(original.Get(),v),version(v) {
        ++factories;
    }
    ~FactoryProxy() { --liveFactories; }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        Lease lease(this);
        const int v=Version(iid,factoryIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==factoryId||(v>=0&&interfaces.Acquire(unsigned(v),iid,factoryLast[v]))) {
            *output=static_cast<IDXGIFactory7*>(this); AddRef(); return S_OK;
        }
        // Preserve unknown extension contracts. Only supported DXGI interfaces
        // are advertised by the proxy; no private object layouts are inferred.
        return original->QueryInterface(iid,output);
    }
    // The game's creation gets the menu on what it returns; beneath Streamline,
    // every chain Streamline or DLSS-G creates here inside the game's creation.
    void Wrap(const Creation& creation,IDXGISwapChain** out,IUnknown* q) noexcept {
        if (streamlineBase) { if (!gInsideOverlay) WrapStreamlineBase(out,q); }
        else if (creation.outer) WrapSwapchain(out,q,this);
    }
    template<class T> T* Revision(unsigned v) noexcept {
        // Witcher 3 calls newer methods on its IDXGIFactory creation pointer
        // without QueryInterface. Acquire only the revision actually used;
        // never assume the root pointer exposes that revision's vtable.
        return interfaces.Acquire(v,factoryIids[v],factoryLast[v])?interfaces.As<T>(v):nullptr;
    }
#if MFG_UNLOCK_XESS
    // XeMFG standalone: the host creates the game's chain on XeFG. The menu's proxy must wrap it (its presents and
    // release are the host's frames and teardown), else the host gives it up and the chain is created natively.
    bool HostCreate(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1& d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,
        IDXGIOutput* o,void* caller,IDXGISwapChain** out,HRESULT& hr) noexcept {
        IDXGISwapChain1* created=nullptr;
        // Asked while off as well: the host notes a chain created before the switch (restart needed).
        if (!xess_d3d12::CreateSwapChain(q,w,&d,f,o,caller,&created,hr)||!created) return false;
        IDXGISwapChain* chain=created;
        if (!WrapHostSwapchain(&chain,q,this)) { xess_d3d12::Abandon(created); return false; }
        *out=chain;
        return true;
    }
    // xemfglive.1: a native chain for a moving proxy, through the layers beneath this factory, with no menu proxy.
    HRESULT CreateNative(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1& d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,
        IDXGISwapChain1** out) noexcept {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target||!out) return E_NOINTERFACE;
        const HRESULT hr=target->CreateSwapChainForHwnd(q,w,&d,f,nullptr,out);
        LogSwapchainCreation("CreateSwapChainForHwnd (XeMFG live move)",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        return hr;
    }
#endif
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* q,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out) override {
        Lease lease(this); SwapchainCreation creation;
#if MFG_UNLOCK_XESS
        if (creation.outer&&!streamlineBase&&d&&out) {
            DXGI_SWAP_CHAIN_DESC1 d1{d->BufferDesc.Width,d->BufferDesc.Height,d->BufferDesc.Format,FALSE,d->SampleDesc,
                d->BufferUsage,d->BufferCount,DXGI_SCALING_STRETCH,d->SwapEffect,DXGI_ALPHA_MODE_UNSPECIFIED,d->Flags};
            DXGI_SWAP_CHAIN_FULLSCREEN_DESC f{d->BufferDesc.RefreshRate,d->BufferDesc.ScanlineOrdering,d->BufferDesc.Scaling,d->Windowed};
            HRESULT hr=E_FAIL;
            if (HostCreate(q,d->OutputWindow,d1,&f,nullptr,_ReturnAddress(),out,hr)) return hr;
        }
#endif
        const HRESULT hr=original->CreateSwapChain(q,d,out);
        LogSwapchainCreation("CreateSwapChain",_ReturnAddress(),original.Get(),hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out) Wrap(creation,out,q);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
#if MFG_UNLOCK_XESS
        if (creation.outer&&!streamlineBase&&d&&out) {
            HRESULT hr=E_FAIL;
            if (HostCreate(q,w,*d,f,o,_ReturnAddress(),reinterpret_cast<IDXGISwapChain**>(out),hr)) return hr;
        }
#endif
        const HRESULT hr=target->CreateSwapChainForHwnd(q,w,d,f,o,out);
        LogSwapchainCreation("CreateSwapChainForHwnd",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out) Wrap(creation,reinterpret_cast<IDXGISwapChain**>(out),q);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* q,IUnknown* w,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
        const HRESULT hr=target->CreateSwapChainForCoreWindow(q,w,d,o,out);
        LogSwapchainCreation("CreateSwapChainForCoreWindow",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out) Wrap(creation,reinterpret_cast<IDXGISwapChain**>(out),q);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* q,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); SwapchainCreation creation;
        auto* target=Revision<IDXGIFactory2>(2);
        if (!target) return E_NOINTERFACE;
        const HRESULT hr=target->CreateSwapChainForComposition(q,d,o,out);
        LogSwapchainCreation("CreateSwapChainForComposition",_ReturnAddress(),target,hr,out?*out:nullptr,creation.outer);
        if (SUCCEEDED(hr)&&out&&*out) Wrap(creation,reinterpret_cast<IDXGISwapChain**>(out),q);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetParent(riid,ppParent); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->EnumAdapters(Adapter,ppAdapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->MakeWindowAssociation(WindowHandle,Flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetWindowAssociation(pWindowHandle); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->CreateSoftwareAdapter(Module,ppAdapter); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory1>(1); return target?target->EnumAdapters1(Adapter,ppAdapter):E_NOINTERFACE; }
    BOOL STDMETHODCALLTYPE IsCurrent() override { Lease lease(this); auto* target=Revision<IDXGIFactory1>(1); return target?target->IsCurrent():FALSE; }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->IsWindowedStereoEnabled():FALSE; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->GetSharedResourceAdapterLuid(hResource,pLuid):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterStereoStatusWindow(WindowHandle,wMsg,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterStereoStatusEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); if(target)target->UnregisterStereoStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterOcclusionStatusWindow(WindowHandle,wMsg,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); return target?target->RegisterOcclusionStatusEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory2>(2); if(target)target->UnregisterOcclusionStatus(dwCookie); }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { Lease lease(this); auto* target=Revision<IDXGIFactory3>(3); return target?target->GetCreationFlags():0; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory4>(4); return target?target->EnumAdapterByLuid(AdapterLuid,riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory4>(4); return target?target->EnumWarpAdapter(riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { Lease lease(this); auto* target=Revision<IDXGIFactory5>(5); return target?target->CheckFeatureSupport(Feature,pFeatureSupportData,FeatureSupportDataSize):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void **ppvAdapter) override { Lease lease(this); auto* target=Revision<IDXGIFactory6>(6); return target?target->EnumAdapterByGpuPreference(Adapter,GpuPreference,riid,ppvAdapter):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory7>(7); return target?target->RegisterAdaptersChangedEvent(hEvent,pdwCookie):E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override { Lease lease(this); auto* target=Revision<IDXGIFactory7>(7); return target?target->UnregisterAdaptersChangedEvent(dwCookie):E_NOINTERFACE; }
};

#if MFG_UNLOCK_XESS
// xemfglive.2: the latency waitable a game takes from a movable chain is the proxy's own semaphore. The bridge waits on
// the current chain's waitable and releases the game's semaphore once per frame that chain lets through, so the wait
// survives moves; DXGI's own handle is wait-only and nothing could signal it once its chain is gone.
class LatencyBridge {
    HANDLE thread_=nullptr,stop_=nullptr,from_=nullptr,to_=nullptr;
    static DWORD WINAPI Run(void* raw) {
        auto* self=static_cast<LatencyBridge*>(raw);
        const HANDLE waits[2]={self->stop_,self->from_};
        for (;;) {
            if (WaitForMultipleObjects(2,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+1) break;
            // Over the game's maximum the release fails harmlessly; anything else ends the bridge.
            if (!ReleaseSemaphore(self->to_,1,nullptr)&&GetLastError()!=ERROR_TOO_MANY_POSTS) break;
        }
        return 0;
    }
public:
    // Takes both handles when it starts (Stop closes them); on false they stay the caller's.
    bool Start(HANDLE from,HANDLE to) noexcept {
        Stop();
        if (!from||!to) return false;
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        from_=from; to_=to;
        thread_=stop_?CreateThread(nullptr,0,Run,this,0,nullptr):nullptr;
        // The game waits on the far side of this thread every frame: it wakes before the game's other work.
        if (thread_) { SetThreadPriority(thread_,THREAD_PRIORITY_HIGHEST); return true; }
        from_=to_=nullptr;
        Stop();
        return false;
    }
    void Stop() noexcept {
        if (thread_) { SetEvent(stop_); WaitForSingleObject(thread_,INFINITE); CloseHandle(thread_); thread_=nullptr; }
        for (HANDLE* handle:{&stop_,&from_,&to_}) if (*handle) { CloseHandle(*handle); *handle=nullptr; }
    }
    ~LatencyBridge(){Stop();}
};
bool SameObject(IUnknown* a,IUnknown* b) noexcept {
    if (!a||!b) return false;
    ComPtr<IUnknown> left,right;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&left)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&right)))&&left==right;
}
HANDLE DuplicateOwn(HANDLE handle) noexcept {
    HANDLE copy=nullptr;
    return handle&&DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)?copy:nullptr;
}
constexpr LONG kWaitableMaximum=DXGI_MAX_SWAP_CHAIN_BUFFERS;
// xemfglive.3/.6: a move at a Present waits this long for the game's DLSS-G to be Off and its feature released.
constexpr uint64_t kDlssgOffWaitMs=5000;
// xemfglive.9: Streamline frees DLSS-G's released buffers (about 1 GB at 4K) only in its own Present bookkeeping, a few
// frames late, and XeFG's chain bypasses Streamline, so the native chain presents this many frames after the release.
constexpr uint32_t kStreamlineCollectPresents=12;
constexpr uint64_t kMoveVramLogDelayMs=3000;
#endif
class SwapchainProxy;
struct PresentCall {
    SwapchainProxy* object;
    PresentCall* previous;
    bool outer=true;
    static thread_local PresentCall* current;
    explicit PresentCall(SwapchainProxy* p) : object(p),previous(current) {
        for (auto* call=previous;call;call=call->previous) if(call->object==p) outer=false;
        current=this;
    }
    ~PresentCall(){current=previous;}
};
thread_local PresentCall* PresentCall::current=nullptr;

class SwapchainProxy final : public IDXGISwapChain4 {
public:
    ComPtr<IDXGISwapChain> original;
    InterfaceCache<IDXGISwapChain,5> interfaces;
    ComPtr<IDXGIFactory> parent;
    dx12::Session* renderer;
    // dx11menu.1: a DirectX 11 swapchain's menu session (renderer is null then).
    dx11::Session* renderer11 = nullptr;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    // XeSS provider swapchains retire with their FG context; other chains
    // never set a provider generation and never retire early.
    std::mutex methodMutex;
    ComPtr<IDXGISwapChain> providerChain;
    uint64_t providerGeneration = 0;
    uint32_t activeMethods = 0;
    bool providerRetired = false;
    // XeFG's native output chain under OptiScaler (generation =
    // its context's): the menu only, XeFG's sync intervals unchanged.
    bool outputLayer = false;
    // menuout.1: also an output layer, DLSS-G's chain beneath Streamline.
    bool streamlineBase = false;
#if MFG_UNLOCK_XESS
    // XeFG's proxy chain of the XeMFG standalone host (xess/d3d12_host.h).
    bool xemfgHost = false;
    // xemfglive.1 (xess/d3d12_host.h Move): what moving the game's D3D12 chain to XeFG or back needs. `moving` refuses
    // other calls meanwhile; without ownWaitable (xemfglive.2), a game that took the chain's own waitable pins it.
    ComPtr<IUnknown> creationQueue;
    HWND window = nullptr;
    uint32_t askedGeneration = 0;
    bool moving = false;
    bool colorSpaceSet = false;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool hdrSet = false;
    DXGI_HDR_METADATA_TYPE hdrType = DXGI_HDR_METADATA_TYPE_NONE;
    std::vector<uint8_t> hdrData;
    UINT maxLatency = 0;
    HANDLE ownWaitable = nullptr;    // the proxy's semaphore; games get duplicates of it
    bool forwardedWaitable = false;  // a game took the chain's own waitable (no ownWaitable)
    HANDLE presentRelease = nullptr; // ours, of ownWaitable, released after each Present
    LatencyBridge bridge;
    // xemfglive.3: the proxy's own back buffers, and a move pending for a later Present (MoveAtPresent).
    BackBuffers backBuffers;
    bool presentMove = false;
    uint64_t presentMoveStart = 0;
    // xemfglive.9: native Presents since DLSS-G's release; when video memory is logged again (0: not due).
    uint32_t collectPresents = 0;
    uint64_t vramLogAt = 0;
#endif
    struct Method {
        SwapchainProxy* object;
        bool valid;
        explicit Method(SwapchainProxy* p) : object(p) {
            object->AddRef();
            std::lock_guard lock(object->methodMutex);
            valid = !object->providerRetired;
#if MFG_UNLOCK_XESS
            valid = valid && !object->moving;
#endif
            if (valid) ++object->activeMethods;
        }
        ~Method() {
            if (valid) { std::lock_guard lock(object->methodMutex); --object->activeMethods; }
            object->Release();
        }
    };
    bool RetireProvider(uint64_t generation) noexcept {
        {
            std::lock_guard lock(methodMutex);
            if (!generation || generation != providerGeneration || providerRetired || activeMethods) return false;
            // Never wait for a method which may be acquiring the XeSS context
            // lock held by this destroy caller. Reserve against every new call.
            providerRetired = true;
        }
        if (!dx12::RetireSession(renderer)) {
            std::lock_guard lock(methodMutex);
            providerRetired = false;
            return false;
        }
        dx12::DestroySession(renderer); renderer = nullptr;
        interfaces.Clear(); original.Reset(); parent.Reset(); providerChain.Reset();
        return true;
    }
    // XeFG's output chain loses its menu before XeFG's destruction,
    // which releases this chain as its last user; until then it keeps
    // forwarding. New methods wait on the lock while the session retires.
    bool RetireRenderer(uint64_t generation) noexcept {
        std::lock_guard lock(methodMutex);
        if (!outputLayer || !generation || generation != providerGeneration || providerRetired || activeMethods) return false;
        if (!renderer) return true;
        if (!dx12::RetireSession(renderer)) return false;
        dx12::DestroySession(renderer); renderer = nullptr;
        return true;
    }
    SwapchainProxy(ComPtr<IDXGISwapChain>&& base,IDXGIFactory* factory,dx12::Session* session,unsigned v) noexcept
        : original(std::move(base)),interfaces(original.Get(),v),parent(factory),renderer(session),version(v) { ++chains; }
    ~SwapchainProxy() {
        dx12::DestroySession(renderer);
        dx11::DestroySession(renderer11);
#if MFG_UNLOCK_XESS
        IDXGISwapChain* const host = xemfgHost ? original.Get() : nullptr;   // identity only after the Reset
        bridge.Stop();
        backBuffers.Destroy();   // its copies finish first; it lets go of the chain's buffers before the chain goes
        if (presentRelease) CloseHandle(presentRelease);
        if (ownWaitable) CloseHandle(ownWaitable);
#endif
        // No proxy/renderer locks are held while the existing chain executes
        // its final Release and any nested Steam/Streamline callbacks.
        interfaces.Clear(); original.Reset(); parent.Reset(); --liveChains;
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::ChainReleased(host);
#endif
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        Method lease(this);
        if (!lease.valid) {
            // The inert shell still has ordinary COM identity and can release
            // safely after SDK teardown; it never touches the retired provider.
            if (iid == __uuidof(IUnknown) || iid == chainId) {
                *output = static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
            }
            return E_NOINTERFACE;
        }
        const int v=Version(iid,chainIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)
            ||iid==chainId||(v>=0&&interfaces.Acquire(unsigned(v),iid,chainLast[v]))) {
            *output=static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
        }
        return original->QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** output) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        return parent ? parent->QueryInterface(iid,output) : original->GetParent(iid,output);
    }
    // The optional V-Sync Off override applies to the application's own
    // top-level Presents only; nested calls and DirectX 11 games keep theirs.
    vsync_control::Submission AdjustSync(const PresentCall& call,UINT sync,UINT flags) noexcept {
        if (!call.outer || outputLayer || renderer11) { vsync_control::Submission same{}; same.interval=same.originalInterval=sync; return same; }
        return vsync_control::BeforeApplicationPresent(sync,flags,dx12::IsPrincipal(renderer));
    }
    // latmon.1/.3: the latency monitor (latency_probe.h) times this chain's Presents.
    uint16_t ProbeRole(const PresentCall& call) const noexcept {
        uint16_t role=0;
        if (streamlineBase) role|=latency_probe::kRoleStreamlineBase;
        else if (outputLayer) role|=latency_probe::kRoleXefgOutput;
#if MFG_UNLOCK_XESS
        if (xemfgHost) role|=latency_probe::kRoleXemfgHost;
#endif
        if (providerChain) role|=latency_probe::kRoleProvider;
        if (renderer11) role|=latency_probe::kRoleD3D11;
        if (!call.outer) role|=latency_probe::kRoleNested;
        return role;
    }
    // After this Present's own work, so the probe's reads never delay the code around the call it timed.
    void ProbePresent(const PresentCall& call,int64_t entry,int64_t returned,HRESULT hr,UINT sync,UINT flags) noexcept {
        if (outputLayer) latency_probe::OutputPresent(this,original.Get(),entry,returned,hr,sync,flags,ProbeRole(call));
        else latency_probe::GamePresent(this,entry,returned,hr,sync,flags,ProbeRole(call));
    }
#if MFG_UNLOCK_XESS
    // xemfglive.3: the game's frame into the chain's current buffer, before DLSS 5 and the menu; UINT_MAX: nothing.
    UINT CopyGameFrame(bool outer,UINT flags) noexcept {
        if (!outer||(flags&DXGI_PRESENT_TEST)||!backBuffers.Active()) return UINT_MAX;
        auto* chain3=interfaces.As<IDXGISwapChain3>(3);
        if (!chain3) return UINT_MAX;
        const UINT before=chain3->GetCurrentBackBufferIndex();
        backBuffers.Copy(chain3);
        return before;
    }
    void GameFramePresented(UINT before) noexcept {
        if (before==UINT_MAX) return;
        if (auto* chain3=interfaces.As<IDXGISwapChain3>(3)) backBuffers.Presented(before,chain3->GetCurrentBackBufferIndex());
    }
#endif
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
        const bool probe=!(flags & DXGI_PRESENT_TEST)&&latency_probe::Recording();
        const int64_t probeEntry=probe?latency_probe::Now():0;
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; PresentCall call(this);
#if MFG_UNLOCK_XESS
        if (call.outer && providerChain && !(flags & DXGI_PRESENT_TEST))
            xess_mod::PreparePresent(providerChain.Get(), providerGeneration);
        const UINT chainBefore=CopyGameFrame(call.outer,flags);
#endif
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,false);
        const bool entered11=call.outer&&dx11::BeginPresent(renderer11,flags,false);
        const auto vsync=AdjustSync(call,sync,flags);
#if MFG_UNLOCK_XESS
        const bool host=xemfgHost&&call.outer&&!(flags & DXGI_PRESENT_TEST);
        if (host) xess_d3d12::BeforePresent(original.Get());
#endif
        const HRESULT hr=original->Present(vsync.interval,flags);
        const int64_t probeReturn=probe?latency_probe::Now():0;
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::AfterPresent(original.Get(),hr);
        GameFramePresented(chainBefore);
        if (call.outer&&!(flags & DXGI_PRESENT_TEST)) { if (presentRelease) ReleaseSemaphore(presentRelease,1,nullptr); AskMove(); }
#endif
        vsync_control::AfterApplicationPresent(vsync,hr);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        if (entered11) dx11::EndPresent(renderer11,hr,flags);
#if MFG_UNLOCK_XESS
        if (call.outer&&!(flags & DXGI_PRESENT_TEST)&&SUCCEEDED(hr)) MoveAtPresent();
#endif
        if (probe) ProbePresent(call,probeEntry,probeReturn,hr,vsync.interval,flags);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override {
        const bool probe=!(flags & DXGI_PRESENT_TEST)&&latency_probe::Recording();
        const int64_t probeEntry=probe?latency_probe::Now():0;
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; PresentCall call(this);
#if MFG_UNLOCK_XESS
        if (call.outer && providerChain && !(flags & DXGI_PRESENT_TEST))
            xess_mod::PreparePresent(providerChain.Get(), providerGeneration);
        const UINT chainBefore=CopyGameFrame(call.outer,flags);
#endif
        const bool partial=params&&(params->DirtyRectsCount||params->pScrollRect||params->pScrollOffset);
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,partial);
        const bool entered11=call.outer&&dx11::BeginPresent(renderer11,flags,partial);
        const auto vsync=AdjustSync(call,sync,flags);
#if MFG_UNLOCK_XESS
        const bool host=xemfgHost&&call.outer&&!(flags & DXGI_PRESENT_TEST);
        if (host) xess_d3d12::BeforePresent(original.Get());
#endif
        const HRESULT hr=interfaces.As<IDXGISwapChain1>(1)->Present1(vsync.interval,flags,params);
        const int64_t probeReturn=probe?latency_probe::Now():0;
#if MFG_UNLOCK_XESS
        if (host) xess_d3d12::AfterPresent(original.Get(),hr);
        GameFramePresented(chainBefore);
        if (call.outer&&!(flags & DXGI_PRESENT_TEST)) { if (presentRelease) ReleaseSemaphore(presentRelease,1,nullptr); AskMove(); }
#endif
        vsync_control::AfterApplicationPresent(vsync,hr);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        if (entered11) dx11::EndPresent(renderer11,hr,flags);
#if MFG_UNLOCK_XESS
        if (call.outer&&!(flags & DXGI_PRESENT_TEST)&&SUCCEEDED(hr)) MoveAtPresent();
#endif
        if (probe) ProbePresent(call,probeEntry,probeReturn,hr,vsync.interval,flags);
        return hr;
    }
    // dx11menu.1: the DirectX 11 session's BeginResize only refuses while one of its Presents is in flight.
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
#if MFG_UNLOCK_XESS
        if (backBuffers.Active()) return ResizeProxied(count,w,h,format,flags,nullptr,nullptr);
        HRESULT moved=S_OK;
        if (MoveOnResize(count,w,h,format,flags,nullptr,nullptr,moved)) return moved;
#endif
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (!dx11::BeginResize(renderer11)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=original->ResizeBuffers(count,w,h,format,flags);
#if MFG_UNLOCK_XESS
        if (xemfgHost && SUCCEEDED(hr)) xess_d3d12::ChainResized(original.Get()); // pacerfix.1
#endif
        dx12::EndResize(renderer,hr); dx11::EndResize(renderer11,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags,
        const UINT* masks,IUnknown* const* queues) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
#if MFG_UNLOCK_XESS
        if (backBuffers.Active()) return ResizeProxied(count,w,h,format,flags,masks,queues);
        HRESULT moved=S_OK;
        if (MoveOnResize(count,w,h,format,flags,masks,queues,moved)) return moved;
#endif
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (!dx11::BeginResize(renderer11)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->ResizeBuffers1(count,w,h,format,flags,masks,queues);
        if (SUCCEEDED(hr)) dx12::VerifyResizeQueues(renderer,count,queues);
#if MFG_UNLOCK_XESS
        if (xemfgHost && SUCCEEDED(hr)) xess_d3d12::ChainResized(original.Get()); // pacerfix.1
#endif
        dx12::EndResize(renderer,hr); dx11::EndResize(renderer11,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->SetColorSpace1(color);
        if (SUCCEEDED(hr)) { dx12::ObserveColorSpace(renderer,color); dx11::ObserveColorSpace(renderer11,color); }
#if MFG_UNLOCK_XESS
        if (SUCCEEDED(hr)) { colorSpaceSet=true; colorSpace=color; }   // a moved chain gets it again
#endif
        return hr;
    }

#if MFG_UNLOCK_XESS
    // ---- xemfglive.1: live moves (xess/d3d12_host.h Move)
    // A game-created D3D12 chain notes its queue, window and switch generation (it asks to move only when that changes).
    void PrepareMoves(IUnknown* queue) noexcept {
        creationQueue=queue;
        HWND hwnd=nullptr;
        auto* chain1=interfaces.As<IDXGISwapChain1>(1);
        if (chain1&&SUCCEEDED(chain1->GetHwnd(&hwnd))) window=hwnd;
        askedGeneration=xess_d3d12::SwitchGeneration();
        PrepareBackBuffers(queue);
        // xemfglive.2: a waitable chain that could move gets the proxy's own latency semaphore before the game asks.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (!window||!chain1||FAILED(chain1->GetDesc1(&desc))||!(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
            ||!xess_d3d12::Ready()) return;
        auto* chain2=interfaces.As<IDXGISwapChain2>(2);
        HANDLE source=chain2?chain2->GetFrameLatencyWaitableObject():nullptr;
        HANDLE own=source?CreateSemaphoreW(nullptr,0,kWaitableMaximum,nullptr):nullptr;
        HANDLE to=DuplicateOwn(own);
        if (to&&bridge.Start(source,to)) { ownWaitable=own; source=to=own=nullptr; }
        for (HANDLE handle:{source,to,own}) if (handle) CloseHandle(handle);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            single_module::Log(ownWaitable ? L"MFG_PROXY_UI latency waitable: the game's is the proxy's own, bridged from the chain's (XeMFG live moves)"
                : L"MFG_PROXY_UI latency waitable: the proxy's own could not be set up; the game gets the chain's (it cannot move once taken)");
    }
    // xemfglive.3: the proxy's own back buffers before the game asks for any, so a switch moves at a Present.
    void PrepareBackBuffers(IUnknown* queue) noexcept {
        auto* chain1=interfaces.As<IDXGISwapChain1>(1);
        if (!window||!chain1||!queue||!(xemfgHost||MovableNative())||!xess_d3d12::Ready()) return;
        static std::atomic<uint32_t> told{0};
        const BackBufferChoice choice=ChooseBackBuffers();
        wchar_t line[400]{};
        if (!BackBuffersChosen(choice)) {
            if (told.fetch_add(1)==0) {
                swprintf_s(line,L"MFG_PROXY_UI XeMFG live switching: the game keeps the swap chain's own buffers (%ls); a switch waits for the game's resize",
                    ChoiceText(choice));
                single_module::Log(line);
            }
            return;
        }
        std::string why;
        const bool made=backBuffers.Create(chain1,queue,why);
        if (told.fetch_add(1)>=4) return;
        const DXGI_SWAP_CHAIN_DESC1 desc=backBuffers.Desc();
        if (made)
            swprintf_s(line,L"MFG_PROXY_UI XeMFG live switching: the game renders into the proxy's own %u back buffers (%ux%u, format %d%ls; %ls), copied to the swap chain at each Present",
                desc.BufferCount,desc.Width,desc.Height,int(desc.Format),xemfgHost?L", XeFG's chain":L"",ChoiceText(choice));
        else
            swprintf_s(line,L"MFG_PROXY_UI XeMFG live switching: the game keeps the swap chain's own buffers (%hs); a switch waits for the game's resize",
                why.c_str());
        single_module::Log(line);
    }
    bool MovableNative() const noexcept {
        return renderer && !renderer11 && !outputLayer && !streamlineBase && !providerChain;
    }
    // What can be checked before anything is released.
    bool MovePreflight(xess_d3d12::Move move,std::string& refused) noexcept {
        BOOL fullscreen=FALSE;
        if (SUCCEEDED(interfaces.As<IDXGISwapChain>(0)->GetFullscreenState(&fullscreen,nullptr))&&fullscreen) {
            refused="fullscreen_exclusive"; return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(interfaces.As<IDXGISwapChain1>(1)->GetDesc1(&desc))) { refused="chain_desc"; return false; }
        return move!=xess_d3d12::Move::eToXefg||xess_d3d12::LivePreflight(window,desc,refused);
    }
    // After the game's Present: once per switch generation, the game's main chain asks for the resize it moves in.
    void AskMove() noexcept {
        if (!creationQueue||!window) return;
        const uint32_t generation=xess_d3d12::SwitchGeneration();
        if (generation==askedGeneration) return;
        askedGeneration=generation;
        presentMove=false;   // a move still waiting belongs to the switch's previous position
        const auto move=xess_d3d12::WantedMove(xemfgHost);
        if (move==xess_d3d12::Move::eNone) return;
        if (!xemfgHost&&(!MovableNative()||!dx12::IsPrincipal(renderer))) return;
        std::string refused;
        if (!MovePreflight(move,refused)) { xess_d3d12::MoveResult(move,refused.c_str()); return; }
        // xemfglive.6: the game's DLSS-G on this native chain is released exactly before the chain goes to XeFG.
        if (move==xess_d3d12::Move::eToXefg&&!xemfgHost&&xess_d3d12::DlssgEverApplied())
            xess_d3d12::WantDlssgRelease(true);
        // xemfglive.3: the game holds none of the chain's buffers, so the chain moves at a coming Present.
        if (backBuffers.Active()) {
            presentMove=true;
            presentMoveStart=GetTickCount64();
            collectPresents=0;
            xess_d3d12::HoldForMove(generation,true);
            return;
        }
        xess_d3d12::RequestResize(window,generation);
    }
    // xemfglive.3: the move AskMove started, after the game's Present. It waits while the game's DLSS-G is On and gives
    // up after kDlssgOffWaitMs. xemfglive.6/.9: to XeFG it also waits for DLSS-G's exact feature release (on timeout
    // the move goes ahead and DLSS-G stays held until a restart), then kStreamlineCollectPresents more native frames.
    void MoveAtPresent() noexcept {
        using xess_d3d12::Move;
        if (vramLogAt&&GetTickCount64()>=vramLogAt) {
            vramLogAt=0;
            xess_d3d12::LogVideoMemory(creationQueue.Get(),"after_move_3s");
        }
        if (!presentMove) return;
        const Move move=xess_d3d12::WantedMove(xemfgHost);
        if (move==Move::eNone||askedGeneration!=xess_d3d12::SwitchGeneration()||!backBuffers.Active()) { presentMove=false; return; }
        // xemfglive.8: either way the game's DLSS-G is Off at the provider first, so it never stays On through a move.
        const bool expired=GetTickCount64()-presentMoveStart>kDlssgOffWaitMs;
        if (xess_d3d12::DlssgOn()) {
            if (expired) {
                presentMove=false;
                xess_d3d12::MoveResult(move,"dlssg_still_on");
                return;
            }
            xess_d3d12::HoldForMove(askedGeneration,false);
            return;
        }
        if (move==Move::eToXefg&&!xemfgHost&&xess_d3d12::DlssgEverApplied()&&!xess_d3d12::DlssgReleased()&&!expired) {
            xess_d3d12::HoldForMove(askedGeneration,false);
            return;
        }
        if (move==Move::eToXefg&&!xemfgHost&&xess_d3d12::DlssgEverApplied()&&collectPresents<kStreamlineCollectPresents
            &&!expired) {
            if (collectPresents++==0) xess_d3d12::LogVideoMemory(creationQueue.Get(),"dlssg_released");
            xess_d3d12::HoldForMove(askedGeneration,false);
            return;
        }
        presentMove=false;
        std::string refused;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
        MovePreflight(move,refused);
        if (refused.empty()&&FAILED(interfaces.As<IDXGISwapChain1>(1)->GetDesc1(&desc))) refused="chain_desc";
        const bool hasFullscreen=refused.empty()&&SUCCEEDED(interfaces.As<IDXGISwapChain1>(1)->GetFullscreenDesc(&fullscreen));
        if (refused.empty()&&(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)&&!ownWaitable&&forwardedWaitable)
            refused="waitable_forwarded";
        if (refused.empty()) {
            std::lock_guard lock(methodMutex);
            if (activeMethods!=1||providerRetired) refused="concurrent_call"; else moving=true;
        }
        if (!refused.empty()) { xess_d3d12::MoveResult(move,refused.c_str()); return; }
        MovingScope guard{this};
        if (!backBuffers.Drain()) { xess_d3d12::MoveResult(move,"gpu_drain"); return; }
        backBuffers.ForgetChain();
        HRESULT result=S_OK;
        ReplaceChain(move,desc,hasFullscreen?&fullscreen:nullptr,L"at a Present",result);
    }
    // xemfglive.3: the game resizes the proxy's back buffers: they are let go, the chain resizes (or moves) beneath them,
    // and they are made again. On failure, or other queues than the chain's, the game gets the chain's own from then on.
    HRESULT ResizeProxied(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,
        IUnknown* const* queues) noexcept {
        if (!backBuffers.Released()) return DXGI_ERROR_INVALID_CALL;
        if (!backBuffers.Drain()) return DXGI_ERROR_WAS_STILL_DRAWING;
        backBuffers.ForgetChain();
        HRESULT result=S_OK;
        if (!MoveOnResize(count,width,height,format,flags,masks,queues,result)) {
            if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
            result=(masks||queues)?interfaces.As<IDXGISwapChain3>(3)->ResizeBuffers1(count,width,height,format,flags,masks,queues)
                :original->ResizeBuffers(count,width,height,format,flags);
            if (SUCCEEDED(result)&&queues) dx12::VerifyResizeQueues(renderer,count,queues);
            // pacerfix.1: XeFG's chain was resized in place.
            if (xemfgHost && SUCCEEDED(result)) xess_d3d12::ChainResized(original.Get());
            dx12::EndResize(renderer,result);
        }
        if (FAILED(result)) return result;   // the game's buffers stay as they were, as a chain's do
        std::string why;
        auto* chain1=interfaces.As<IDXGISwapChain1>(1);
        if (!SingleQueue(count,masks,queues)) why="resize_queues";
        else if (!chain1) why="chain1";
        else if (backBuffers.Recreate(chain1,why)) return result;
        backBuffers.Destroy();
        wchar_t line[200]{};
        swprintf_s(line,L"MFG_PROXY_UI XeMFG live switching: back buffers off after the game's resize (%hs); the game gets the swap chain's own",
            why.c_str());
        single_module::Log(line);
        return result;
    }
    struct MovingScope {
        SwapchainProxy* proxy;
        ~MovingScope() { std::lock_guard lock(proxy->methodMutex); proxy->moving=false; }
    };
    // ResizeBuffers1's queues, when given, are all the creation queue on one node (what a move re-creates the chain on).
    bool SingleQueue(UINT count,const UINT* masks,IUnknown* const* queues) noexcept {
        if (!masks&&!queues) return true;
        if (!count) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            if (FAILED(interfaces.As<IDXGISwapChain1>(1)->GetDesc1(&desc))) return false;
            count=desc.BufferCount;
        }
        for (UINT i=0;i<count;++i) {
            if (masks&&masks[i]>1) return false;
            if (queues&&!SameObject(queues[i],creationQueue.Get())) return false;
        }
        return true;
    }
    // A native chain from the factory this one came from (a window just released by its old chain may take a moment).
    bool CreateNativeChain(const DXGI_SWAP_CHAIN_DESC1& desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
        ComPtr<IDXGISwapChain1>& created,std::string& reason) noexcept {
        ComPtr<IUnknown> own;
        if (!parent||parent->QueryInterface(factoryId,reinterpret_cast<void**>(own.GetAddressOf()))!=S_OK||!own) {
            reason="native_factory"; return false;
        }
        auto* factory=static_cast<FactoryProxy*>(own.Get());
        HRESULT hr=E_FAIL;
        for (int attempt=0;attempt<4;++attempt) {
            if (attempt) Sleep(16);
            hr=factory->CreateNative(creationQueue.Get(),window,desc,fullscreen,created.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr)&&created) return true;
        }
        char text[48]{};
        sprintf_s(text,"native_create_0x%08lX",static_cast<unsigned long>(hr));
        reason=text;
        return false;
    }
    // The new chain becomes this proxy's; a native one gets a menu session of its own.
    bool Adopt(IDXGISwapChain1* created,bool host) noexcept {
        ComPtr<IDXGISwapChain3> chain3;
        if (!created||!native::PinInterface(created,2)||FAILED(created->QueryInterface(IID_PPV_ARGS(&chain3)))||!chain3
            ||!native::PinInterface(chain3.Get(),chainLast[3])) return false;
        original=chain3;
        interfaces.Rebase(original.Get());
        xemfgHost=host;
        if (!host&&install::PrepareInput()) renderer=dx12::CreateSession(original.Get(),creationQueue.Get());
        return true;
    }
    void ReplayState() noexcept {
        if (maxLatency) if (auto* chain2=interfaces.As<IDXGISwapChain2>(2)) chain2->SetMaximumFrameLatency(maxLatency);
        if (colorSpaceSet) if (auto* chain3=interfaces.As<IDXGISwapChain3>(3))
            if (SUCCEEDED(chain3->SetColorSpace1(colorSpace))) dx12::ObserveColorSpace(renderer,colorSpace);
        if (hdrSet&&interfaces.Acquire(4,chainIids[4],chainLast[4]))
            if (auto* chain4=interfaces.As<IDXGISwapChain4>(4))
                chain4->SetHDRMetaData(hdrType,static_cast<UINT>(hdrData.size()),hdrData.empty()?nullptr:hdrData.data());
    }
    // Inside the game's ResizeBuffers: the real resize runs first (DXGI refuses it while anyone holds the chain's
    // buffers, so success proves the game let them go), then ReplaceChain. True when handled here (`result`).
    bool MoveOnResize(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,
        IUnknown* const* queues,HRESULT& result) noexcept {
        using xess_d3d12::Move;
        if (!creationQueue||!window) return false;
        const Move move=xess_d3d12::WantedMove(xemfgHost);
        if (move==Move::eNone||(!xemfgHost&&!MovableNative())) return false;
        xess_d3d12::MoveAttempt();
        std::string refused;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
        if (!SingleQueue(count,masks,queues)) refused="resize_queues";
        else MovePreflight(move,refused);
        if (refused.empty()&&FAILED(interfaces.As<IDXGISwapChain1>(1)->GetDesc1(&desc))) refused="chain_desc";
        const bool hasFullscreen=refused.empty()&&SUCCEEDED(interfaces.As<IDXGISwapChain1>(1)->GetFullscreenDesc(&fullscreen));
        // The game waits on the chain's own semaphore, which nothing signals once the chain is gone.
        if (refused.empty()&&(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)&&!ownWaitable&&forwardedWaitable)
            refused="waitable_forwarded";
        if (refused.empty()) {
            std::lock_guard lock(methodMutex);
            if (activeMethods!=1||providerRetired) refused="concurrent_call"; else moving=true;
        }
        const auto fail=[&](const char* why) { xess_d3d12::MoveResult(move,why); };
        if (!refused.empty()) { fail(refused.c_str()); return false; }   // the game's resize runs as always
        MovingScope guard{this};
        if (!dx12::BeginResize(renderer)) { fail("menu_drawing"); result=DXGI_ERROR_WAS_STILL_DRAWING; return true; }
        result=(masks||queues)?interfaces.As<IDXGISwapChain3>(3)->ResizeBuffers1(count,width,height,format,flags,masks,queues)
            :original->ResizeBuffers(count,width,height,format,flags);
        if (SUCCEEDED(result)&&queues) dx12::VerifyResizeQueues(renderer,count,queues);
        dx12::EndResize(renderer,result);
        if (FAILED(result)) { fail("resize_failed"); return true; }
        // The chain as resized (a zero size or count kept its own).
        if (FAILED(interfaces.As<IDXGISwapChain1>(1)->GetDesc1(&desc))) { fail("chain_desc"); return true; }
        ReplaceChain(move,desc,hasFullscreen?&fullscreen:nullptr,L"in the game's resize",result);
        return true;
    }
    // The move itself (`moving` set, no one but the menu holding the chain's buffers): a native chain must be ours alone
    // and goes first, then the new one is created on the same window; if that fails, one of the old kind is. `result`
    // becomes S_OK, or DXGI_ERROR_DEVICE_REMOVED when no chain could be created (the proxy is an inert shell then).
    void ReplaceChain(xess_d3d12::Move move,const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,const wchar_t* when,HRESULT& result) noexcept {
        using xess_d3d12::Move;
        const auto fail=[&](const char* why) { xess_d3d12::MoveResult(move,why); };
        xess_d3d12::LogVideoMemory(creationQueue.Get(),"before_move");   // xemfglive.9
        IDXGISwapChain* const oldHost=xemfgHost?original.Get():nullptr;   // identity only after the Reset
        if (!xemfgHost) {
            if (!dx12::RetireSession(renderer)) { fail("menu_busy"); return; }
            dx12::DestroySession(renderer); renderer=nullptr;
            interfaces.Clear();
            original->AddRef();
            if (original->Release()!=1) {   // another holder keeps the chain (and the window): it stays, with its menu
                interfaces.Rebase(original.Get());
                if (install::PrepareInput()) renderer=dx12::CreateSession(original.Get(),creationQueue.Get());
                fail("chain_shared");
                return;
            }
        } else {
            interfaces.Clear();
        }
        bridge.Stop();
        if (presentRelease) { CloseHandle(presentRelease); presentRelease=nullptr; }
        original.Reset();   // the window is free: the native chain is gone, or XeFG's goes with its context
        if (oldHost) xess_d3d12::ChainReleased(oldHost,"live_move");
        else xess_d3d12::NativeChainReleased();   // xemfglive.5: the game's DLSS-G may be held until a restart
        ComPtr<IDXGISwapChain1> created;
        std::string reason;
        bool host=false,retried=false;
        if (move==Move::eToXefg) {
            host=xess_d3d12::CreateSwapChainLive(creationQueue.Get(),window,desc,fullscreenDesc,false,created.GetAddressOf(),reason);
            if (!host) { retried=true; std::string nativeReason; CreateNativeChain(desc,fullscreenDesc,created,nativeReason); }
        } else if (!CreateNativeChain(desc,fullscreenDesc,created,reason)) {
            retried=true;
            std::string xefg;
            host=xess_d3d12::CreateSwapChainLive(creationQueue.Get(),window,desc,fullscreenDesc,true,created.GetAddressOf(),xefg);
        }
        if (!created||!Adopt(created.Get(),host)) {
            if (created&&host) { IDXGISwapChain1* const raw=created.Detach(); xess_d3d12::Abandon(raw); }
            created.Reset();
            {
                std::lock_guard lock(methodMutex);
                providerRetired=true;   // an inert shell from here: the game's calls fail instead of reaching no chain
            }
            reason="recreate_failed"+(reason.empty()?std::string():":"+reason);
            fail(reason.c_str());
            single_module::Log(L"MFG_PROXY_UI XeMFG live move: no chain could be created for the game's window");
            result=DXGI_ERROR_DEVICE_REMOVED;
            return;
        }
        ReplayState();
        if (ownWaitable) {
            // The game keeps waiting on the proxy's semaphore; the bridge now follows the new chain's.
            HANDLE source=(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
                ?interfaces.As<IDXGISwapChain2>(2)->GetFrameLatencyWaitableObject():nullptr;
            HANDLE to=DuplicateOwn(ownWaitable);
            if (!bridge.Start(source,to)) { if (source) CloseHandle(source); presentRelease=to; }
        }
        result=S_OK;
        xess_d3d12::MoveResult(move,retried?(reason.empty()?"recreated":reason.c_str()):nullptr,retried);
        xess_d3d12::LogVideoMemory(creationQueue.Get(),"after_move");   // xemfglive.9
        vramLogAt=GetTickCount64()+kMoveVramLogDelayMs;
        wchar_t line[200]{};
        swprintf_s(line,L"MFG_PROXY_UI XeMFG live move %ls: the game's chain is now %ls%ls",when,host?L"XeFG's":L"native",
            retried?L" (the wanted kind failed)":L"");
        single_module::Log(line);
    }
#endif

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetDevice(riid,ppDevice); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
#if MFG_UNLOCK_XESS
        if (backBuffers.Active()) return backBuffers.Get(Buffer,riid,ppSurface);   // xemfglive.3: the proxy's own
#endif
        return interfaces.As<IDXGISwapChain>(0)->GetBuffer(Buffer,riid,ppSurface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->SetFullscreenState(Fullscreen,pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetFullscreenState(pFullscreen,ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain>(0)->GetLastPresentCount(pLastPresentCount); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetFullscreenDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetCoreWindow(refiid,ppUnk); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { Method lease(this); if (!lease.valid) return 0; return interfaces.As<IDXGISwapChain1>(1)->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetRestrictToOutput(ppRestrictToOutput); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->SetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->SetRotation(Rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain1>(1)->GetRotation(pRotation); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->SetSourceSize(Width,Height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetSourceSize(pWidth,pHeight); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        const HRESULT hr=interfaces.As<IDXGISwapChain2>(2)->SetMaximumFrameLatency(MaxLatency);
#if MFG_UNLOCK_XESS
        if (SUCCEEDED(hr)) maxLatency=MaxLatency;   // a moved chain gets it again
#endif
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetMaximumFrameLatency(pMaxLatency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        Method lease(this); if (!lease.valid) return nullptr;
#if MFG_UNLOCK_XESS
        if (ownWaitable) return DuplicateOwn(ownWaitable);   // xemfglive.2: the proxy's semaphore, which moves keep signalled
#endif
        const HANDLE handle=interfaces.As<IDXGISwapChain2>(2)->GetFrameLatencyWaitableObject();
#if MFG_UNLOCK_XESS
        if (handle) forwardedWaitable=true;
#endif
        return handle;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->SetMatrixTransform(pMatrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain2>(2)->GetMatrixTransform(pMatrix); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
        Method lease(this); if (!lease.valid) return 0;
#if MFG_UNLOCK_XESS
        if (backBuffers.Active()) return backBuffers.Index();   // xemfglive.3: the proxy's own buffers' index
#endif
        return interfaces.As<IDXGISwapChain3>(3)->GetCurrentBackBufferIndex();
    }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL; return interfaces.As<IDXGISwapChain3>(3)->CheckColorSpaceSupport(ColorSpace,pColorSpaceSupport); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override {
        Method lease(this); if (!lease.valid) return DXGI_ERROR_INVALID_CALL;
        // GTA may call this method on the creation-return pointer before it
        // requests revision 4 from our proxy. Acquire that public interface on
        // first use, retaining the exact owning layer and normal cache lifetime.
        // Never assume a revision-3 pointer provides revision-4 methods.
        if (!interfaces.Acquire(4,chainIids[4],chainLast[4])) return E_NOINTERFACE;
        auto* target=interfaces.As<IDXGISwapChain4>(4);
        const HRESULT hr=target?target->SetHDRMetaData(Type,Size,pMetaData):E_NOINTERFACE;
#if MFG_UNLOCK_XESS
        if (SUCCEEDED(hr)&&Size<=4096&&(pMetaData||!Size)) {   // a moved chain gets it again
            try {
                const auto* bytes=static_cast<const uint8_t*>(pMetaData);
                hdrData.assign(bytes,bytes+Size);
                hdrType=Type; hdrSet=true;
            } catch (...) { hdrSet=false; }
        }
#endif
        return hr;
    }
};

// Ours, or a layer that forwards our IID to ours. Streamline's proxy over one
// of our base proxies (menuout.1) forwards it too, and still gets our layer.
bool Wrapped(IUnknown* object,REFIID iid) noexcept {
    ComPtr<IUnknown> own;
    if (object->QueryInterface(iid,reinterpret_cast<void**>(own.GetAddressOf()))!=S_OK||!own) return false;
    const bool base=iid==factoryId?static_cast<FactoryProxy*>(own.Get())->streamlineBase
        :static_cast<SwapchainProxy*>(own.Get())->streamlineBase;
    return !base||own.Get()==object;
}

// The API of a swapchain creation's device argument; anything unknown takes the D3D12 session (dx11menu.1).
enum class CreationApi { eOther, eD3D12, eD3D11, eD3D10 };
CreationApi DeviceApi(IUnknown* queue) noexcept {
    static constexpr GUID kD3D11Device{0xdb6f6ddb,0xac77,0x4e88,{0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40}};
    static constexpr GUID kD3D10Device{0x9b7e4c0f,0x342c,0x4106,{0xa1,0x9f,0x4f,0x27,0x04,0xf6,0x89,0xf0}};
    ComPtr<ID3D12CommandQueue> d3d12;
    if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&d3d12)))) return CreationApi::eD3D12;
    ComPtr<IUnknown> device;
    if (SUCCEEDED(queue->QueryInterface(kD3D11Device,reinterpret_cast<void**>(device.GetAddressOf())))) return CreationApi::eD3D11;
    if (SUCCEEDED(queue->QueryInterface(kD3D10Device,reinterpret_cast<void**>(device.ReleaseAndGetAddressOf())))) return CreationApi::eD3D10;
    return CreationApi::eOther;
}

// dx11menu.1: a DirectX 11 game's swapchain gets the DirectX 11 session (DLSS 5, its keys and the general settings).
// Beside OptiScaler, which wraps DirectX 11 swap chains itself, the chain stays as it is: OptiScaler's menu serves it.
void WrapD3D11Swapchain(IDXGISwapChain** output,IUnknown* device,IDXGIFactory* parent) noexcept {
    if (optiscaler_host::Detected()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            single_module::Log(L"MFG_PROXY_UI DirectX 11 swap chain beside OptiScaler: left to OptiScaler's own swap chain and menu; no MFG Unlocker menu");
        return;
    }
    // A D3D11 window of a D3D12 or Vulkan game (launcher, video, middleware) gets no menu of its own (graphics_api).
    if (!graphics_api::D3D11Application()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            wchar_t line[200]{};
            swprintf_s(line,L"MFG_PROXY_UI DirectX 11 swap chain in a D3D12 or Vulkan application: no DirectX 11 menu (step=application_api seen=0x%X first=0x%X)",
                graphics_api::Seen(),graphics_api::First());
            single_module::Log(line);
        }
        return;
    }
    Capacity capacity(liveChains); if(!capacity.held)return;
    InternalScope internal;
    if (!native::PinInterface(*output,2)||Wrapped(*output,chainId)) return;
    ComPtr<IDXGISwapChain3> chain3;
    if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3))))return;
    ComPtr<IDXGISwapChain> base=chain3;
    constexpr unsigned version=3;
    if(!native::PinInterface(base.Get(),chainLast[version])||slots::Fault("proxy-chain")) return;
    if (!install::PrepareInput()) return;
    auto* session=dx11::CreateSession(base.Get(),device);
    if (!session) return; // Unknown ownership leaves the original object untouched.
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,nullptr,version);
    if (!proxy) { dx11::DestroySession(session); return; }
    proxy->renderer11=session;
    auto* old=*output;
    capacity.Transfer();
    *output=proxy;
    old->Release(); // Transfer only the reference returned by successful creation.
    single_module::Log(L"MFG_PROXY_UI DirectX 11 swapchain proxy attached after existing creation chain completed (menu: DLSS 5 and keys; frame generation and XeMFG need DirectX 12 or Vulkan); nativeTableWrites=0");
}

void WrapSwapchain(IDXGISwapChain** output,IUnknown* queue,IDXGIFactory* parent) noexcept {
    if (!output||!*output||!queue||native::InsideLoader()||liveChains.load()>=64) return;
    const auto api=DeviceApi(queue);
    if (api==CreationApi::eD3D11) { WrapD3D11Swapchain(output,queue,parent); return; }
    if (api==CreationApi::eD3D12) graphics_api::Note(graphics_api::kD3D12); // dx11menu.1: bookkeeping only
    static std::atomic<bool> olderApiLogged{false};
    if (api==CreationApi::eD3D10 && !olderApiLogged.exchange(true))
        single_module::Log(L"MFG_PROXY_UI DirectX 10 swap chain: no menu (it draws on DirectX 11, DirectX 12 and Vulkan swap chains)");
    Capacity capacity(liveChains); if(!capacity.held)return;
    InternalScope internal;
    if (!native::PinInterface(*output,2)||Wrapped(*output,chainId)) return;
    ComPtr<IDXGISwapChain3> chain3;
    if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3))))return;
    ComPtr<IDXGISwapChain> base=chain3;
    constexpr unsigned version=3;
    if(!native::PinInterface(base.Get(),chainLast[version])||slots::Fault("proxy-chain")) return;
    if (!install::PrepareInput()) return;
    auto* session=dx12::CreateSession(base.Get(),queue);
    if (!session) return; // Unknown ownership leaves the original object untouched.
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,session,version);
    if (!proxy) { dx12::DestroySession(session); return; }
#if MFG_UNLOCK_XESS
    if (api==CreationApi::eD3D12) proxy->PrepareMoves(queue);   // xemfglive.1
#endif
    auto* old=*output;
    capacity.Transfer();
    *output=proxy;
    old->Release(); // Transfer only the reference returned by successful creation.
    single_module::Log(L"MFG_PROXY_UI swapchain proxy attached after existing creation chain completed; nativeTableWrites=0");
}
// menuout.1: a chain on Streamline's base factory, where DLSS-G presents every
// output frame. Its parent stays its own, so no factory taken from it creates
// beneath Streamline.
void WrapStreamlineBase(IDXGISwapChain** output,IUnknown* queue) noexcept {
    if (!output||!*output||!queue||native::InsideLoader()||liveChains.load()>=64) return;
    // DLSS-G presents on D3D12 only; a DirectX 11 game's chain gets its menu above Streamline (dx11menu.1).
    const auto api=DeviceApi(queue);
    if (api==CreationApi::eD3D11||api==CreationApi::eD3D10) return;
    if (api==CreationApi::eD3D12) graphics_api::Note(graphics_api::kD3D12);
    Capacity capacity(liveChains); if(!capacity.held)return;
    InternalScope internal;
    if (!native::PinInterface(*output,2)||AlreadyWrapped(*output,chainId)) return;
    ComPtr<IDXGISwapChain3> chain3;
    if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3))))return;
    ComPtr<IDXGISwapChain> base=chain3;
    if(!native::PinInterface(base.Get(),chainLast[3])||!install::PrepareInput()) return;
    auto* session=dx12::CreateSession(base.Get(),queue,nullptr,true,true);
    if (!session) return; // Streamline keeps the chain it created; the menu stays above it.
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),nullptr,session,3);
    if (!proxy) { dx12::DestroySession(session); return; }
    proxy->outputLayer=true;
    proxy->streamlineBase=true;
    auto* old=*output;
    capacity.Transfer();
    *output=proxy;
    old->Release(); // The creation's reference moves to the proxy.
    single_module::Log(L"MFG_PROXY_UI swapchain proxy attached beneath Streamline (every output frame); nativeTableWrites=0");
}
#if MFG_UNLOCK_XESS
// A proxy over the XeMFG host's chain (XeFG's proxy chain) that carries the host's present calls and the
// game's release. No menu session: XeFG's chain is libxess_fg's object, which the renderer never takes; the menu sits
// on XeFG's output chain instead (AttachOutputSwapchain during the takeover).
bool WrapHostSwapchain(IDXGISwapChain** output,IUnknown* queue,IDXGIFactory* parent) noexcept {
    const auto refuse=[](const wchar_t* step) {
        wchar_t line[128]{}; swprintf_s(line,L"MFG_PROXY_UI XeMFG standalone chain not wrapped: %s",step);
        single_module::Log(line); return false;
    };
    if (!output||!*output) return false;
    if (native::InsideLoader()) return refuse(L"loader_lock");
    Capacity capacity(liveChains); if(!capacity.held) return refuse(L"capacity");
    InternalScope internal;
    // No AlreadyWrapped test: XeFG's proxy passes unknown interfaces to its output chain, which carries our menu
    // proxy (AttachOutputSwapchain), and this chain is the one the host just created.
    if (!native::PinInterface(*output,2)) return refuse(L"pin");
    ComPtr<IDXGISwapChain3> chain3;
    if (FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3)))||!chain3) return refuse(L"swapchain3");
    ComPtr<IDXGISwapChain> base=chain3;
    if (!native::PinInterface(base.Get(),chainLast[3])) return refuse(L"pin_swapchain3");
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,nullptr,3);
    if (!proxy) return refuse(L"allocation");
    proxy->xemfgHost=true;
    proxy->PrepareMoves(queue);   // xemfglive.1: back to a native chain when switched off
    capacity.Transfer();
    auto* old=*output;
    *output=proxy;
    old->Release(); // The creation's reference moves to the proxy.
    single_module::Log(L"MFG_PROXY_UI XeMFG standalone chain attached");
    return true;
}
#endif

void WrapFactory(REFIID iid,void** output) noexcept {
    if (!output||!*output||!single_module::OverlayAllowed()||Version(iid,factoryIids)<0||liveFactories.load()>=64) return;
    Capacity capacity(liveFactories); if(!capacity.held)return;
    InternalScope internal;
    auto* returned=static_cast<IUnknown*>(*output);
    if (!native::PinInterface(returned,2)||Wrapped(returned,factoryId)) return;
    const unsigned version=static_cast<unsigned>(Version(iid,factoryIids));
    ComPtr<IDXGIFactory> base=static_cast<IDXGIFactory*>(returned);
    if(!native::PinInterface(base.Get(),factoryLast[version])||slots::Fault("proxy-factory")) return;
    auto* proxy=new(std::nothrow) FactoryProxy(std::move(base),version);
    if (!proxy) return;
    capacity.Transfer();
    *output=static_cast<IDXGIFactory7*>(proxy);
    returned->Release();
    gDxgiHooked.store(true);
    install::FactoryReady();
}
// [Overlay] MenuOnOutputFrames=0 in RTX40MFG-UI.ini (beside the game) keeps
// the menu above Streamline, on the game's frames.
bool MenuOnOutputFrames() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,MAX_PATH);
    wchar_t* slash=length&&length<MAX_PATH?wcsrchr(path,L'\\'):nullptr;
    if (!slash||wcscpy_s(slash+1,MAX_PATH-static_cast<size_t>(slash+1-path),L"RTX40MFG-UI.ini")) return true;
    return GetPrivateProfileIntW(L"Overlay",L"MenuOnOutputFrames",1,path)!=0;
}
// menuout.1: DLSS-G's chain is created on the factory Streamline's proxy wraps, so our proxy goes there through
// Streamline's public calls only (base IID, then slUpgradeInterface); anything unexpected keeps the returned proxy.
// Not beside OptiScaler: its frame generation sits beneath Streamline's base.
void Skipped(unsigned reason,const wchar_t* text,const void* where=nullptr) noexcept {
    static std::array<std::atomic<uint32_t>,9> logged{};
    if (reason>=logged.size()||logged[reason].fetch_add(1,std::memory_order_relaxed)>=3) return;
    wchar_t module[160]{},line[400]{};
    if (where) ModuleAt(where,module,160);
    swprintf_s(line,L"MFG_PROXY_UI factory beneath Streamline skipped: %s%s%s; the menu stays on the game's frames",
        text,where?L" at ":L"",module);
    single_module::Log(line);
}
void UnderStreamline(REFIID iid,void** output) noexcept {
    static const bool wanted=MenuOnOutputFrames();
    const HMODULE interposer=GetModuleHandleW(L"sl.interposer.dll");
    if (!output||!*output||!interposer||!single_module::OverlayAllowed()) return;
    const int version=Version(iid,factoryIids);
    if (!wanted) return Skipped(0,L"[Overlay] MenuOnOutputFrames=0");
    if (version<0) return Skipped(1,L"the interface asked for is no factory revision");
    if (optiscaler_host::Detected()) return Skipped(2,L"OptiScaler");
    if (native::InsideLoader()) return Skipped(3,L"loader lock");
    InternalScope internal;
    using UpgradeFn=int(*)(void**); // sl::Result slUpgradeInterface(void** baseInterface), eOk = 0
    const auto upgrade=reinterpret_cast<UpgradeFn>(GetProcAddress(interposer,"slUpgradeInterface"));
    if (!upgrade||!slots::ImageEntry(interposer,reinterpret_cast<void*>(upgrade)))
        return Skipped(4,L"slUpgradeInterface export",reinterpret_cast<void*>(upgrade));
    auto* returned=static_cast<IUnknown*>(*output);
    const bool pinned=native::PinInterface(returned,2);
    if (!pinned||!slots::ImageSlot(interposer,*reinterpret_cast<void***>(returned)))
        return Skipped(5,L"the factory returned is not Streamline's proxy",pinned?(*reinterpret_cast<void***>(returned))[0]:nullptr);
    ComPtr<IUnknown> wrapped;
    if (returned->QueryInterface(native::kStreamlineBase,reinterpret_cast<void**>(wrapped.GetAddressOf()))!=S_OK||!wrapped)
        return Skipped(6,L"Streamline's base query");
    // menuout.2: the base is whatever Streamline wraps; ours only forwards to it.
    void** const baseTable=*reinterpret_cast<void***>(wrapped.Get());
    if (!native::PinInterface(wrapped.Get(),factoryLast[version])||AlreadyWrapped(wrapped.Get(),factoryId))
        return Skipped(7,L"Streamline's base",baseTable);
    const bool systemBase=native::SystemDxgiObject(wrapped.Get(),factoryLast[version]);
    Capacity capacity(liveFactories); if(!capacity.held) return Skipped(8,L"factory capacity");
    // Streamline keeps the revision the game asked for as its base.
    ComPtr<IDXGIFactory> root;
    root.Attach(static_cast<IDXGIFactory*>(wrapped.Detach()));
    auto* lower=new(std::nothrow) FactoryProxy(std::move(root),static_cast<unsigned>(version));
    if (!lower) return;
    lower->streamlineBase=true;
    capacity.Transfer();
    IUnknown* const ours=static_cast<IDXGIFactory7*>(lower);
    void* upgraded=ours;
    const int result=upgrade(&upgraded);
    auto* proxy=static_cast<IUnknown*>(upgraded);
    ComPtr<IUnknown> base;
    void* typed=nullptr;
    const bool placed=result==0&&proxy&&proxy!=ours&&native::PinInterface(proxy,2)
        &&slots::ImageSlot(interposer,*reinterpret_cast<void***>(proxy))
        &&proxy->QueryInterface(native::kStreamlineBase,reinterpret_cast<void**>(base.GetAddressOf()))==S_OK&&base.Get()==ours
        &&proxy->QueryInterface(iid,&typed)==S_OK&&typed;
    base.Reset();
    if (!placed) {
        if (typed) static_cast<IUnknown*>(typed)->Release();
        if (proxy&&proxy!=ours) proxy->Release(); // drops its reference on ours
        lower->Release();
        static std::atomic<bool> refusedLogged{false};
        if (!refusedLogged.exchange(true)) {
            wchar_t line[160]{};
            swprintf_s(line,L"MFG_PROXY_UI factory beneath Streamline refused (slUpgradeInterface=%d); the menu stays on the game's frames",result);
            single_module::Log(line);
        }
        return;
    }
    proxy->Release(); // typed keeps the new proxy; the new proxy keeps ours
    lower->Release();
    *output=typed;
    returned->Release(); // The game's reference moves to the new Streamline proxy.
    static std::atomic<uint32_t> placedLogged{0};
    if (placedLogged.fetch_add(1,std::memory_order_relaxed)<4) {
        wchar_t module[160]{},line[320]{};
        ModuleAt(baseTable,module,160);
        swprintf_s(line,L"MFG_PROXY_UI factory beneath Streamline: Streamline's proxy now wraps ours (slUpgradeInterface) base=%s system=%u; nativeTableWrites=0",
            module,static_cast<unsigned>(systemBase));
        single_module::Log(line);
    }
}
}
HRESULT FactoryCall(FactoryFn original,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(iid,output);
    if (SUCCEEDED(hr)&&creation.outer) { UnderStreamline(iid,output); WrapFactory(iid,output); }
    else if (!creation.outer) ++skipped;
    return hr;
}
HRESULT FactoryCall(Factory2Fn original,UINT flags,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(flags,iid,output);
    if (SUCCEEDED(hr)&&creation.outer) { UnderStreamline(iid,output); WrapFactory(iid,output); }
    else if (!creation.outer) ++skipped;
    return hr;
}
void ReadStatus(MfgSingleModuleStatus& status) noexcept {
    status.factoryGatewayCalls=calls.load();
    status.factoryWrappersCreated=factories.load();
    status.swapchainWrappersCreated=chains.load();
    status.liveFactoryWrappers=liveFactories.load();
    status.liveSwapchainWrappers=liveChains.load();
    status.internalFactoryCallsSkipped=skipped.load();
}
HRESULT ParentFactoryCall(ParentFn original,IDXGIObject* object,REFIID iid,void** output) noexcept {
    Creation creation;
    const HRESULT hr=original(object,iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output);
    return hr;
}
HRESULT D3D11ChainCall(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN original,void* caller,IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** chain,ID3D11Device** device,D3D_FEATURE_LEVEL* level,
    ID3D11DeviceContext** context) noexcept {
    Creation creation;
    const HRESULT hr=original(adapter,type,software,flags,levels,count,sdk,desc,chain,device,level,context);
    if (FAILED(hr)||!creation.outer||!chain||!*chain) return hr;
    const DWORD error=GetLastError();
    LogSwapchainCreation("D3D11CreateDeviceAndSwapChain",caller,nullptr,hr,*chain,true);
    // The device as the application holds it; when it asked for none, the one the swapchain reports.
    ComPtr<ID3D11Device> reported;
    IUnknown* creator=device&&*device?static_cast<IUnknown*>(*device):nullptr;
    if (!creator&&!native::InsideLoader()) {
        InternalScope internal;
        if (native::PinInterface(*chain,2)&&SUCCEEDED((*chain)->GetDevice(IID_PPV_ARGS(&reported)))&&reported)
            creator=reported.Get();
    }
    const wchar_t* step=!creator?L"device":native::InsideLoader()?L"loader_lock":liveChains.load()>=64?L"capacity"
        :DeviceApi(creator)!=CreationApi::eD3D11?L"device_api":nullptr;
    if (!step) {
        IDXGISwapChain* const returned=*chain;
        WrapD3D11Swapchain(chain,creator,nullptr); // its own refusals log their step (dx11::CreateSession)
        if (*chain!=returned) {
            gDxgiHooked.store(true);
            install::D3D11Ready();
            // The chain's parent becomes the menu's factory proxy: a chain the game creates again from GetParent
            // (an MSAA or fullscreen change) keeps the menu. If that factory is not wrapped, GetParent stays native.
            auto* proxy=static_cast<SwapchainProxy*>(*chain);
            void* factory=nullptr;
            {
                InternalScope internal;
                if (FAILED(proxy->original->GetParent(__uuidof(IDXGIFactory),&factory))) factory=nullptr;
            }
            void* const native=factory;
            if (factory) WrapFactory(__uuidof(IDXGIFactory),&factory);
            if (factory&&factory!=native) proxy->parent.Attach(static_cast<IDXGIFactory7*>(factory));
            else if (factory) static_cast<IUnknown*>(factory)->Release();
        }
    } else {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1,std::memory_order_relaxed)<3) {
            wchar_t line[160]{};
            swprintf_s(line,L"MFG_PROXY_UI D3D11CreateDeviceAndSwapChain swap chain left unwrapped (step=%ls)",step);
            single_module::Log(line);
        }
    }
    SetLastError(error);
    return hr;
}
bool Owned(IUnknown* object) noexcept {
    if (!object||!native::PinInterface(object,2)) return false;
    InternalScope internal;
    return AlreadyWrapped(object,factoryId)||AlreadyWrapped(object,chainId);
}
void Describe(IUnknown* object,wchar_t* text,size_t count) noexcept {
    if (!object) { swprintf_s(text,count,L"null"); return; }
    if (native::InsideLoader()) { swprintf_s(text,count,L"%p (loader lock)",static_cast<void*>(object)); return; }
    // The pin checks the table and its first entries before any call.
    if (!native::PinInterface(object,2)) { swprintf_s(text,count,L"%p unreadable-table",static_cast<void*>(object)); return; }
    InternalScope internal;
    const bool ours=AlreadyWrapped(object,factoryId)||AlreadyWrapped(object,chainId);
    struct Kind { IID iid; const char* name; };
    static const Kind kinds[]={{__uuidof(IDXGIFactory),"factory"},{__uuidof(IDXGISwapChain),"swapchain"},
        {__uuidof(ID3D12Device),"d3d12-device"},{__uuidof(ID3D12CommandQueue),"d3d12-queue"},{__uuidof(IDXGIAdapter),"adapter"}};
    const char* kind="other";
    for (const auto& candidate:kinds) {
        void* typed=nullptr;
        const HRESULT hr=object->QueryInterface(candidate.iid,&typed);
        if (typed) static_cast<IUnknown*>(typed)->Release();
        if (hr==S_OK&&typed) { kind=candidate.name; break; }
    }
    wchar_t table[160]{};
    ModuleAt((*reinterpret_cast<void***>(object))[0],table,160);
    swprintf_s(text,count,L"%p %hs ours=%u queryInterface=%s",static_cast<void*>(object),kind,static_cast<unsigned>(ours),table);
}
bool WrapUpgraded(void** output) noexcept {
    if (!output||!*output||native::InsideLoader()) return false;
    auto* upgraded=static_cast<IUnknown*>(*output);
    if (!native::PinInterface(upgraded,2)||Owned(upgraded)) return false;
    {
        // A D3D12 swapchain reports its device, never its presentation queue,
        // and the renderer needs that queue. Leave upgraded swapchains alone;
        // Streamline's manual-hooking flow upgrades the factory first.
        InternalScope internal;
        ComPtr<IDXGISwapChain> chain;
        if (SUCCEEDED(upgraded->QueryInterface(IID_PPV_ARGS(&chain)))&&chain) {
            static std::atomic_flag logged=ATOMIC_FLAG_INIT;
            if (!logged.test_and_set())
                single_module::Log(L"MFG_PROXY_UI upgraded swapchain left unwrapped: its presentation queue is not discoverable");
            return false;
        }
    }
    // The application keeps using its pointer at whatever revision it holds,
    // so the proxy's root is the highest revision the upgraded factory has.
    for (int version=7;version>=0;--version) {
        void* typed=nullptr;
        {
            InternalScope internal;
            if (upgraded->QueryInterface(factoryIids[version],&typed)!=S_OK||!typed) {
                if (typed) static_cast<IUnknown*>(typed)->Release();
                continue;
            }
        }
        void* wrapped=typed;
        WrapFactory(factoryIids[version],&wrapped);
        if (wrapped==typed) { static_cast<IUnknown*>(typed)->Release(); return false; }
        *output=wrapped;
        upgraded->Release(); // The application's reference moves to the proxy.
        return true;
    }
    return false;
}
}

namespace single_overlay::dx12 {
using Microsoft::WRL::ComPtr;
bool AttachProviderSwapchain(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
    HMODULE provider, uint64_t generation, IDXGISwapChain** output) noexcept
{
    if (!output) return false;
    *output = nullptr;
    if (!chain || !queue || !provider || !generation || native::InsideLoader()) return false;
    proxy::Capacity capacity(proxy::liveChains);
    if (!capacity.held) return false;
    InternalScope internal;
    // Only the verified provider's own swapchain can take this route.
    if (!native::PinInterface(chain, 2)) return false;
    ComPtr<IDXGISwapChain3> version3;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&version3)))
        || !native::PinInterface(version3.Get(), 39)
        || !slots::ImageSlot(provider, *reinterpret_cast<void***>(version3.Get()))
        || !slots::ImageEntry(provider, (*reinterpret_cast<void***>(version3.Get()))[8])) return false;
    // OptiScaler mode keeps the provider wrapper for XeSS present preparation
    // (input fixes and UI composition) but creates no menu session or input hooks.
    const bool menu = single_module::OverlayAllowed();
    if (menu && !install::PrepareInput()) return false;
    auto* session = menu ? CreateSession(version3.Get(), queue, provider) : nullptr;
    if (menu && !session) return false;
    ComPtr<IDXGISwapChain> base = version3;
    auto* wrapper = new(std::nothrow) proxy::SwapchainProxy(std::move(base), nullptr, session, 3);
    if (!wrapper) { DestroySession(session); return false; }
    wrapper->providerGeneration = generation;
    wrapper->providerChain = chain;
    capacity.Transfer();
    *output = wrapper;
    single_module::Log(L"MFG_PROXY_UI XeSS provider swapchain attached with exact generation and queue");
    return true;
}
const char* AttachOutputSwapchain(IDXGISwapChain1** chain, IUnknown* queue, HWND window, uint64_t generation,
    IDXGISwapChain** keep) noexcept
{
    if (keep) *keep = nullptr;
    if (!chain || !*chain || !queue || !generation || !keep) return "arguments";
    if (!OutputSwapchainWanted()) return "ui_mode";
    if (native::InsideLoader()) return "loader_lock";
    // The factory route wraps what the game's creation returns:
    // one menu per window, never this chain as well. XeMFG
    // standalone's takeover runs inside the factory route, and its game chain
    // gets no menu (WrapHostSwapchain), so the menu goes here.
#if MFG_UNLOCK_XESS
    if (proxy::swapchainCreations && !xess_d3d12::InsideTakeover()) return "factory_route";
#else
    if (proxy::swapchainCreations) return "factory_route";
#endif
    if (WindowSession(window)) return "window_session";
    proxy::Capacity capacity(proxy::liveChains);
    if (!capacity.held) return "capacity";
    InternalScope internal;
    ComPtr<IDXGISwapChain3> version3;
    if (!native::PinInterface(*chain, 2) || proxy::AlreadyWrapped(*chain, proxy::chainId)
        || FAILED((*chain)->QueryInterface(IID_PPV_ARGS(&version3)))
        || !native::PinInterface(version3.Get(), proxy::chainLast[3])) return "swapchain_interface";
    if (!install::PrepareInput()) return "input_unavailable";
    // Device, queue and LUID identity as for every session.
    auto* session = CreateSession(version3.Get(), queue, nullptr, true);
    if (!session) return "session_refused";
    ComPtr<IDXGISwapChain> base = version3;
    auto* wrapper = new(std::nothrow) proxy::SwapchainProxy(std::move(base), nullptr, session, 3);
    if (!wrapper) { DestroySession(session); return "allocation"; }
    wrapper->providerGeneration = generation;
    wrapper->outputLayer = true;
    capacity.Transfer();
    auto* created = *chain;
    *chain = wrapper; // XeFG's reference moves to the proxy.
    created->Release();
    wrapper->AddRef();
    *keep = wrapper;
    single_module::Log(L"MFG_PROXY_UI XeFG output swapchain attached (OptiScaler host route) with exact generation and queue");
    return nullptr;
}
bool AboveStreamlineBase(IUnknown* chain) noexcept
{
    if (!chain) return false;
    ComPtr<IUnknown> base, owned;
    if (chain->QueryInterface(native::kStreamlineBase, reinterpret_cast<void**>(base.GetAddressOf())) != S_OK || !base
        || !native::PinInterface(base.Get(), 2)
        || base->QueryInterface(proxy::chainId, reinterpret_cast<void**>(owned.GetAddressOf())) != S_OK || !owned) return false;
    return static_cast<proxy::SwapchainProxy*>(owned.Get())->streamlineBase;
}
bool RetireOutputSwapchain(IDXGISwapChain* chain, uint64_t generation) noexcept
{
    if (!chain || !generation) return false;
    ComPtr<IUnknown> owned;
    if (chain->QueryInterface(proxy::chainId, reinterpret_cast<void**>(owned.GetAddressOf())) != S_OK || !owned) return false;
    return static_cast<proxy::SwapchainProxy*>(owned.Get())->RetireRenderer(generation);
}
bool DetachProviderSwapchain(IDXGISwapChain* chain, uint64_t generation) noexcept
{
    if (!chain || !generation) return false;
    ComPtr<IUnknown> owned;
    if (chain->QueryInterface(proxy::chainId, reinterpret_cast<void**>(owned.GetAddressOf())) != S_OK || !owned) return false;
    return static_cast<proxy::SwapchainProxy*>(owned.Get())->RetireProvider(generation);
}
}
