// Exercise the production DXGI proxy, including its real interface ownership
// checks. Only the renderer, coordinator and logging endpoints are stubbed.
#include "overlay_dxgi_proxy.cpp"
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>

namespace single_module { void Log(const wchar_t*) noexcept {} bool OverlayAllowed() noexcept { return true; } }
namespace optiscaler_host { bool Detected() noexcept { return false; } }
// latmon.2: the latency monitor never records here (its recorder is latency_probe.cpp).
namespace latency_probe {
bool Recording() noexcept { return false; }
int64_t Now() noexcept { return 0; }
void GamePresent(const void*,int64_t,int64_t,HRESULT,UINT,UINT,uint16_t) noexcept {}
void OutputPresent(const void*,IDXGISwapChain*,int64_t,int64_t,HRESULT,UINT,UINT,uint16_t) noexcept {}
}
namespace single_overlay::install {
void FactoryReady() noexcept {}
void D3D11Ready() noexcept {}
bool PrepareInput() noexcept { return false; }
}
namespace single_overlay::dx12 {
Session* CreateSession(IDXGISwapChain*,IUnknown*,HMODULE,bool,bool) noexcept { return nullptr; }
bool WindowSession(HWND) noexcept { return false; }
bool OutputSwapchainWanted() noexcept { return false; }
bool RetireSession(Session*) noexcept { return true; }
void DestroySession(Session*) noexcept {}
bool BeginPresent(Session*,UINT,bool) noexcept { return false; }
bool IsPrincipal(Session*) noexcept { return false; }
void EndPresent(Session*,HRESULT,UINT) noexcept {}
bool BeginResize(Session*) noexcept { return false; }
void EndResize(Session*,HRESULT) noexcept {}
void ObserveColorSpace(Session*,DXGI_COLOR_SPACE_TYPE) noexcept {}
void VerifyResizeQueues(Session*,UINT,IUnknown* const*) noexcept {}
}
namespace single_overlay::dx11 {
Session* CreateSession(IDXGISwapChain*,IUnknown*) noexcept { return nullptr; }
void DestroySession(Session*) noexcept {}
bool BeginPresent(Session*,UINT,bool) noexcept { return false; }
void EndPresent(Session*,HRESULT,UINT) noexcept {}
bool BeginResize(Session*) noexcept { return true; }
void EndResize(Session*,HRESULT) noexcept {}
void ObserveColorSpace(Session*,DXGI_COLOR_SPACE_TYPE) noexcept {}
}
namespace vsync_control {
Submission BeforeApplicationPresent(uint32_t interval,uint32_t,bool) noexcept {
    Submission result{}; result.interval=result.originalInterval=interval; return result;
}
void AfterApplicationPresent(const Submission&,HRESULT) noexcept {}
}

namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned revisions[]{2,2,2,1,1,2,2,2,2,2,2,2,2,3,4,4,5,6,7,7};
constexpr IID iids[]{__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),__uuidof(IDXGIFactory2),
    __uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),
    __uuidof(IDXGIFactory6),__uuidof(IDXGIFactory7)};
bool okay=true;
void Check(bool value,const char* label) {
    std::printf("%s %s\n",value?"PASS":"FAIL",label); std::fflush(stdout); okay&=value;
}

class Fixture final : public IDXGIFactory7 {
public:
    std::atomic<ULONG> refs{1};
    std::array<std::atomic<unsigned>,8> queries{};
    std::array<std::atomic<unsigned>,20> calls{};
    std::atomic<UINT> adapterIndex{0};
    unsigned maximum=7;
    unsigned malformed=0;
    std::barrier<>* rendezvous=nullptr;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        *output=nullptr;
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)) {
            *output=static_cast<IDXGIFactory7*>(this); AddRef(); return S_OK;
        }
        for (unsigned v=0;v<8;++v) if (iid==iids[v]) {
            ++queries[v];
            if (v>maximum) return E_NOINTERFACE;
            if (v==1&&malformed==1) return S_OK; // Broken success with no interface.
            *output=static_cast<IDXGIFactory7*>(this); AddRef();
            if (v==1&&rendezvous) rendezvous->arrive_and_wait();
            if (v==1&&malformed==2) return E_NOINTERFACE; // Failed query with an owned output.
            if (v==1&&malformed==3) return S_FALSE; // Success other than the required S_OK.
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT Track(unsigned method) { ++calls[method]; return E_ABORT; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void**) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT,IDXGIAdapter**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE,IDXGIAdapter**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**) override { return Track(0); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**) override { return Track(1); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**) override { return Track(2); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index,IDXGIAdapter1**) override { adapterIndex=index; return Track(3); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { ++calls[4]; return TRUE; }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { ++calls[5]; return TRUE; }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE,LUID*) override { return Track(6); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND,UINT,DWORD*) override { return Track(7); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE,DWORD*) override { return Track(8); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override { ++calls[9]; }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND,UINT,DWORD*) override { return Track(10); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE,DWORD*) override { return Track(11); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override { ++calls[12]; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { ++calls[13]; return 0x105; }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID,REFIID,void**) override { return Track(14); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID,void**) override { return Track(15); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE,void*,UINT) override { return Track(16); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT,DXGI_GPU_PREFERENCE,REFIID,void**) override { return Track(17); }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE,DWORD*) override { return Track(18); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD) override { return Track(19); }
};
// A layer that forwards every interface to the object beneath it, as Streamline's proxies do.
struct Forwarder final : IUnknown {
    IUnknown* below;
    explicit Forwarder(IUnknown* p) : below(p) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override { return below->QueryInterface(iid,output); }
};
thread_local Fixture* creating=nullptr;
HRESULT WINAPI CreateFixture(REFIID iid,void** output) { return creating->QueryInterface(iid,output); }
ComPtr<IDXGIFactory> Wrap(Fixture& fixture,unsigned version=0) {
    creating=&fixture;
    void* output=nullptr;
    const auto hr=single_overlay::proxy::FactoryCall(&CreateFixture,iids[version],&output);
    creating=nullptr;
    Check(hr==S_OK&&output&&output!=static_cast<IDXGIFactory7*>(&fixture),"fixture factory wrapped through production gateway");
    ComPtr<IDXGIFactory> result; result.Attach(static_cast<IDXGIFactory*>(output)); return result;
}
int64_t Invoke(IDXGIFactory* object,unsigned method) {
    // Intentional direct newer-vtable access reproduces the game, with no QI.
    auto* p=reinterpret_cast<IDXGIFactory7*>(object);
    DWORD cookie=0; LUID luid{}; BOOL support=FALSE;
    switch (method) {
    case 0: return p->CreateSwapChainForHwnd(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);
    case 1: return p->CreateSwapChainForCoreWindow(nullptr,nullptr,nullptr,nullptr,nullptr);
    case 2: return p->CreateSwapChainForComposition(nullptr,nullptr,nullptr,nullptr);
    case 3: return p->EnumAdapters1(37,nullptr);
    case 4: return p->IsCurrent();
    case 5: return p->IsWindowedStereoEnabled();
    case 6: return p->GetSharedResourceAdapterLuid(nullptr,&luid);
    case 7: return p->RegisterStereoStatusWindow(nullptr,19,&cookie);
    case 8: return p->RegisterStereoStatusEvent(nullptr,&cookie);
    case 9: p->UnregisterStereoStatus(23); return 0;
    case 10: return p->RegisterOcclusionStatusWindow(nullptr,29,&cookie);
    case 11: return p->RegisterOcclusionStatusEvent(nullptr,&cookie);
    case 12: p->UnregisterOcclusionStatus(31); return 0;
    case 13: return p->GetCreationFlags();
    case 14: return p->EnumAdapterByLuid(luid,IID_NULL,nullptr);
    case 15: return p->EnumWarpAdapter(IID_NULL,nullptr);
    case 16: return p->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&support,sizeof(support));
    case 17: return p->EnumAdapterByGpuPreference(41,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_NULL,nullptr);
    case 18: return p->RegisterAdaptersChangedEvent(nullptr,&cookie);
    case 19: return p->UnregisterAdaptersChangedEvent(43);
    }
    return E_UNEXPECTED;
}
int64_t Expected(unsigned method,bool available) {
    if (method==4||method==5) return available?TRUE:FALSE;
    if (method==9||method==12) return 0;
    if (method==13) return available?0x105:0;
    return available?E_ABORT:E_NOINTERFACE;
}
bool NoHigherQueries(const Fixture& fixture,unsigned requested=0) {
    for(unsigned v=1;v<8;++v)if(v!=requested&&fixture.queries[v])return false;
    return true;
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    for (unsigned method=0;method<std::size(revisions);++method) {
        std::printf("METHOD %u REVISION %u\n",method,revisions[method]);
        Fixture fixture;
        auto proxy=Wrap(fixture);
        if (!proxy||proxy.Get()==static_cast<IDXGIFactory7*>(&fixture)) return 1;
        Check(NoHigherQueries(fixture),"creation performs no speculative revision queries");
        Check(Invoke(proxy.Get(),method)==Expected(method,true)&&fixture.calls[method]==1,
            "direct newer method forwards once with the underlying return value");
        Check(fixture.queries[revisions[method]]==1&&NoHigherQueries(fixture,revisions[method]),
            "only the method's required revision was queried");
        Check(Invoke(proxy.Get(),method)==Expected(method,true)&&fixture.calls[method]==2
            &&fixture.queries[revisions[method]]==1&&fixture.refs==3,
            "subsequent method call reuses one retained interface reference");
        if (method==3) Check(fixture.adapterIndex==37,"EnumAdapters1 preserves the adapter index");
        proxy.Reset();
        Check(fixture.refs==1,"proxy retirement releases the root and acquired revision");
    }
    {
        Fixture fixture; fixture.maximum=0; auto proxy=Wrap(fixture);
        for (unsigned method=0;method<std::size(revisions);++method)
            Check(Invoke(proxy.Get(),method)==Expected(method,false)&&fixture.calls[method]==0,
                "unsupported revision returns safely without invoking a newer method");
        Check(fixture.refs==2,"rejected revisions retain no references");
        proxy.Reset(); Check(fixture.refs==1,"unsupported factory retires cleanly");
    }
    {
        Fixture fixture; auto proxy=Wrap(fixture);
        Check(Invoke(proxy.Get(),13)==Expected(13,true)&&Invoke(proxy.Get(),3)==Expected(3,true)
            &&fixture.queries[3]==1&&fixture.queries[1]==0,
            "cached higher revision satisfies an inherited method without another query");
        proxy.Reset(); Check(fixture.refs==1,"shared higher revision released once");
    }
    {
        Fixture fixture; auto proxy=Wrap(fixture,7);
        bool forwarded=true;
        for(unsigned method=0;method<std::size(revisions);++method)
            forwarded&=Invoke(proxy.Get(),method)==Expected(method,true)&&fixture.calls[method]==1;
        Check(forwarded&&fixture.queries[7]==1&&NoHigherQueries(fixture,7)&&fixture.refs==2,
            "creation at revision 7 forwards inherited methods without further queries");
        proxy.Reset(); Check(fixture.refs==1,"highest creation revision retires without double release");
    }
    for(unsigned malformed=1;malformed<=3;++malformed) {
        Fixture fixture; fixture.malformed=malformed; auto proxy=Wrap(fixture);
        Check(Invoke(proxy.Get(),3)==E_NOINTERFACE&&fixture.calls[3]==0&&fixture.refs==2,
            "malformed QueryInterface result rejected without leaking its output");
        proxy.Reset(); Check(fixture.refs==1,"malformed query leaves root lifetime intact");
    }
    {
        constexpr unsigned count=8;
        Fixture fixture; std::barrier rendezvous(count); fixture.rendezvous=&rendezvous;
        auto proxy=Wrap(fixture); std::atomic<unsigned> succeeded{0}; std::vector<std::thread> threads;
        for(unsigned i=0;i<count;++i)threads.emplace_back([&]{if(Invoke(proxy.Get(),3)==E_ABORT)++succeeded;});
        for(auto& thread:threads)thread.join();
        fixture.rendezvous=nullptr;
        Check(succeeded==count&&fixture.calls[3]==count&&fixture.queries[1]==count&&fixture.refs==3,
            "concurrent first use publishes one cache entry and releases losing queries");
        proxy.Reset(); Check(fixture.refs==1,"concurrent cache retirement releases exactly one acquired reference");
    }
    {
        // menuout.1: Streamline's proxy over a base proxy forwards our IID and still gets our layer above it;
        // a layer over an ordinary proxy is ours already.
        using namespace single_overlay::proxy;
        Fixture fixture; fixture.AddRef();
        ComPtr<IDXGIFactory> root; root.Attach(static_cast<IDXGIFactory7*>(&fixture));
        ++liveFactories;
        auto* lower=new FactoryProxy(std::move(root),0);
        Forwarder above(static_cast<IDXGIFactory7*>(lower));
        lower->streamlineBase=true;
        Check(!Wrapped(&above,factoryId)&&Wrapped(static_cast<IDXGIFactory7*>(lower),factoryId),
            "a layer over Streamline's base proxy is wrapped; the base proxy itself is not");
        lower->streamlineBase=false;
        Check(Wrapped(&above,factoryId),"a layer over an ordinary proxy counts as wrapped");
        Check(!Wrapped(static_cast<IDXGIFactory7*>(&fixture),factoryId),"a foreign factory is not wrapped");
        lower->Release();
        Check(fixture.refs==1,"base proxy retirement releases its factory");
    }
    MfgSingleModuleStatus status{}; single_overlay::proxy::ReadStatus(status);
    Check(status.liveFactoryWrappers==0&&status.liveSwapchainWrappers==0,"all fixture proxies retired");
    return okay?0:1;
}
