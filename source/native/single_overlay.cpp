#include "single_overlay.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "overlay_dxgi_proxy.h"
#include "overlay_vulkan.h"
#include "overlay_native.h"
#include "overlay_application_imports.h"
#include "overlay_adapter_parent.h"
#include "overlay_graphics_api.h"
#include "caller_scoped_import.h"
#include "optiscaler_host.h"
#include "witcher_dots/witcher_dots.h"
#include "rtx_hair/universal_hair.h"
#include <d3d12.h>
#include <d3d11.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace single_overlay {
namespace {
// Bindings have process lifetime and are published before a gateway escapes.
// A distinct original entry always gets a distinct typed gateway.
// First call through each gateway: which export it binds and who called it.
// Distinguishes application, middleware and scoped-entry routes in game logs.
void LogFirstGatewayCall(const char* api,FARPROC original,void* caller) noexcept {
    std::unique_ptr<wchar_t[]> storage(new(std::nothrow) wchar_t[2*32768+512]);
    if (!storage) return;
    wchar_t* owner=storage.get();
    wchar_t* from=owner+32768;
    wchar_t* line=from+32768;
    auto leaf=[](const void* address,wchar_t* path,uintptr_t& rva) {
        HMODULE module=nullptr;
        rva=0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),&module) || !module) { wcscpy_s(path,32768,L"(none)"); return path; }
        const DWORD n=GetModuleFileNameW(module,path,32768);
        if (!n || n>=32768) { wcscpy_s(path,32768,L"(unnamed)"); return path; }
        rva=reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module);
        const wchar_t* separator=wcsrchr(path,L'\\');
        return separator?const_cast<wchar_t*>(separator+1):path;
    };
    uintptr_t ownerRva=0,callerRva=0;
    const wchar_t* ownerLeaf=leaf(reinterpret_cast<const void*>(original),owner,ownerRva);
    const wchar_t* callerLeaf=leaf(caller,from,callerRva);
    swprintf_s(line,512,L"MFG_PROXY_UI factory gateway first call api=%hs entry=%s caller=%s+0x%llX",
        api,ownerLeaf,callerLeaf,static_cast<unsigned long long>(callerRva));
    single_module::Log(line);
}
template<class Tag,class Fn> struct Gateways {
    // Keep entry identity separate from a published trampoline. Repeated slInit
    // calls must reuse the same gateway after a scoped entry has been armed.
    inline static std::array<std::atomic<FARPROC>,16> originals{};
    inline static std::array<std::atomic<FARPROC>,16> trampolines{};
    inline static std::array<std::atomic<bool>,16> called{};
    template<size_t I> static FARPROC Original() noexcept {
        if (const auto trampoline=trampolines[I].load(std::memory_order_acquire)) return trampoline;
        return originals[I].load(std::memory_order_acquire);
    }
    template<size_t I> static HRESULT WINAPI Factory(REFIID iid,void** output) {
        if (!called[I].exchange(true,std::memory_order_acq_rel))
            LogFirstGatewayCall(Tag::name,originals[I].load(std::memory_order_acquire),_ReturnAddress());
        return proxy::FactoryCall(reinterpret_cast<proxy::FactoryFn>(Original<I>()),iid,output);
    }
    template<size_t I> static HRESULT WINAPI Factory2(UINT flags,REFIID iid,void** output) {
        if (!called[I].exchange(true,std::memory_order_acq_rel))
            LogFirstGatewayCall(Tag::name,originals[I].load(std::memory_order_acquire),_ReturnAddress());
        return proxy::FactoryCall(reinterpret_cast<proxy::Factory2Fn>(Original<I>()),flags,iid,output);
    }
    template<size_t... I> static auto Entries(std::index_sequence<I...>) {
        if constexpr (std::is_same_v<Fn,proxy::Factory2Fn>) return std::array<FARPROC,sizeof...(I)>{reinterpret_cast<FARPROC>(&Factory2<I>)...};
        else return std::array<FARPROC,sizeof...(I)>{reinterpret_cast<FARPROC>(&Factory<I>)...};
    }
    inline static const auto entries=Entries(std::make_index_sequence<16>{});
    static bool PublishOriginal(void* target,void* trampoline) noexcept {
        if (!target||!trampoline) return false;
        for (size_t i=0;i<originals.size();++i) if (originals[i].load(std::memory_order_acquire)==reinterpret_cast<FARPROC>(target)) {
            FARPROC expected=nullptr;
            return trampolines[i].compare_exchange_strong(expected,reinterpret_cast<FARPROC>(trampoline),std::memory_order_acq_rel)
                || expected==reinterpret_cast<FARPROC>(trampoline);
        }
        return false;
    }
    static FARPROC Bind(FARPROC target) noexcept {
        for (size_t i=0;i<originals.size();++i) {
            if (entries[i]==target) return target;
            FARPROC expected=nullptr;
            if (originals[i].compare_exchange_strong(expected,target,std::memory_order_acq_rel)||expected==target) return entries[i];
        }
        single_module::Log(L"MFG_PROXY_UI factory gateway capacity exhausted; original entry retained");
        return target;
    }
};
struct Factory0 { static constexpr const char* name="CreateDXGIFactory"; };
struct Factory1 { static constexpr const char* name="CreateDXGIFactory1"; };
struct Factory2 { static constexpr const char* name="CreateDXGIFactory2"; };
using DeviceFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
struct DeviceGateways {
    inline static std::array<std::atomic<DeviceFn>,8> originals{};
    template<size_t I> static HRESULT WINAPI Create(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** output) {
        adapter_parent::Observe(adapter);
        if(output)witcher_dots::BeforeDeviceCreate(_ReturnAddress());
        const auto result=originals[I].load(std::memory_order_acquire)(adapter,level,iid,output);
        const DWORD error=GetLastError();
        if(SUCCEEDED(result)&&output&&*output) {
            graphics_api::Note(graphics_api::kD3D12); // dx11menu.1: bookkeeping only
            witcher_dots::ObserveDevice(static_cast<IUnknown*>(*output),_ReturnAddress());
            rtx_hair::universal::ObserveDevice(static_cast<IUnknown*>(*output)); // rtxhair.1 (hairauto.1: on unless turned off)
        }
        SetLastError(error);
        return result;
    }
    template<size_t... I> static auto Entries(std::index_sequence<I...>) {
        return std::array<DeviceFn,sizeof...(I)>{&Create<I>...};
    }
    static FARPROC Bind(FARPROC entry) noexcept {
        static const auto entries=Entries(std::make_index_sequence<8>{});
        const auto target=reinterpret_cast<DeviceFn>(entry);
        for(size_t i=0;i<originals.size();++i) {
            if(entries[i]==target)return entry;
            DeviceFn expected=nullptr;
            if(originals[i].compare_exchange_strong(expected,target,std::memory_order_acq_rel)||expected==target)
                return reinterpret_cast<FARPROC>(entries[i]);
        }
        return entry;
    }
};
// dx11menu.1: a DirectX 11 game's public device creation. D3D11CreateDevice arms the adapter-parent route from the
// returned device's adapter, so a factory taken through IDXGIDevice -> GetAdapter -> GetParent is the menu's proxy;
// D3D11CreateDeviceAndSwapChain also hands its swapchain to the DirectX 11 menu. Beside OptiScaler, or with the menu
// off, both forward only.
bool D3D11DeviceName(const char* name) noexcept {
    return !strcmp(name,"D3D11CreateDevice")||!strcmp(name,"D3D11CreateDeviceAndSwapChain");
}
// What the executable's import directory links, by DLL and by name or ordinal. d3d12.lib binds D3D12CreateDevice by
// ordinal (#101), which slots::VisitImports does not report. Any import from d3d11.dll counts as D3D11, so a dual-API
// executable keeps the runtime signals (graphics_api). Bounded like VisitImports; no delay imports.
struct LinkedApis { bool d3d11=false,d3d12=false,vulkan=false,valid=false; };
LinkedApis ScanLinkedApis(HMODULE image) noexcept {
    LinkedApis linked{};
    if (!image) return linked;
    const auto base=reinterpret_cast<uint8_t*>(image);
    const auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000) return linked;
    const auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return linked;
    const size_t size=nt->OptionalHeader.SizeOfImage;
    const auto valid=[&](size_t rva,size_t bytes){return rva<size&&bytes<=size-rva;};
    const auto text=[&](size_t rva)->const char*{
        if (!valid(rva,1)) return nullptr;
        const auto* s=reinterpret_cast<const char*>(base+rva);
        return memchr(s,0,(std::min)(size_t(4096),size-rva))?s:nullptr;
    };
    const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {linked.valid=true;return linked;}
    if (!valid(dir.VirtualAddress,dir.Size)||dir.Size>1024*sizeof(IMAGE_IMPORT_DESCRIPTOR)) return linked;
    const auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for (size_t i=0;i<dir.Size/sizeof(*imports);++i) {
        const auto& imp=imports[i];
        if (!imp.Name) {linked.valid=true;return linked;}
        const char* dll=text(imp.Name);
        if (!dll) return linked;
        const bool d3d11=!_stricmp(dll,"d3d11.dll"),d3d12=!_stricmp(dll,"d3d12.dll");
        linked.d3d11|=d3d11;
        // Bound slots hold addresses, not names: only the original thunks are read (as VisitImports).
        if (!imp.OriginalFirstThunk) continue;
        const size_t thunks=imp.OriginalFirstThunk;
        for (size_t j=0;j<4096;++j) {
            const size_t rva=thunks+j*sizeof(IMAGE_THUNK_DATA64);
            if (!valid(rva,sizeof(IMAGE_THUNK_DATA64))) return linked;
            const auto value=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+rva)->u1.AddressOfData;
            if (!value) break;
            if (IMAGE_SNAP_BY_ORDINAL64(value)) {
                if (d3d12&&IMAGE_ORDINAL64(value)==101) linked.d3d12=true;
                continue;
            }
            const char* name=text(static_cast<size_t>(value)+2);
            if (!name) return linked;
            if (D3D11DeviceName(name)) linked.d3d11=true;
            else if (!strcmp(name,"D3D12CreateDevice")) linked.d3d12=true;
            else if (!strcmp(name,"vkCreateInstance")) linked.vulkan=true;
        }
    }
    return linked;
}
bool D3D11MenuWanted() noexcept {
    return single_module::OverlayAllowed()&&!optiscaler_host::Detected();
}
struct D3D11Gateways {
    inline static std::array<std::atomic<PFN_D3D11_CREATE_DEVICE>,8> devices{};
    inline static std::array<std::atomic<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>,8> chains{};
    template<size_t I> static HRESULT WINAPI CreateDevice(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,
        UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* level,
        ID3D11DeviceContext** context) {
        const auto result=devices[I].load(std::memory_order_acquire)(adapter,type,software,flags,levels,count,sdk,
            device,level,context);
        const DWORD error=GetLastError();
        if(SUCCEEDED(result)&&device&&*device) {
            graphics_api::Note(graphics_api::kD3D11);
            if(D3D11MenuWanted())adapter_parent::ObserveD3D11(*device);
        }
        SetLastError(error);
        return result;
    }
    template<size_t I> static HRESULT WINAPI CreateDeviceAndSwapChain(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,
        HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,
        IDXGISwapChain** chain,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context) {
        const auto original=chains[I].load(std::memory_order_acquire);
        if(!D3D11MenuWanted())return original(adapter,type,software,flags,levels,count,sdk,desc,chain,device,level,context);
        const auto result=proxy::D3D11ChainCall(original,_ReturnAddress(),adapter,type,software,flags,levels,count,sdk,
            desc,chain,device,level,context);
        const DWORD error=GetLastError();
        if(SUCCEEDED(result)&&((device&&*device)||(chain&&*chain)))graphics_api::Note(graphics_api::kD3D11);
        if(SUCCEEDED(result)&&device&&*device)adapter_parent::ObserveD3D11(*device);
        SetLastError(error);
        return result;
    }
    template<size_t... I> static auto DeviceEntries(std::index_sequence<I...>) {
        return std::array<PFN_D3D11_CREATE_DEVICE,sizeof...(I)>{&CreateDevice<I>...};
    }
    template<size_t... I> static auto ChainEntries(std::index_sequence<I...>) {
        return std::array<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN,sizeof...(I)>{&CreateDeviceAndSwapChain<I>...};
    }
    template<class Fn,size_t N> static FARPROC Bind(std::array<std::atomic<Fn>,N>& originals,const std::array<Fn,N>& entries,
        FARPROC entry) noexcept {
        const auto target=reinterpret_cast<Fn>(entry);
        for(size_t i=0;i<N;++i) {
            if(entries[i]==target)return entry;
            Fn expected=nullptr;
            if(originals[i].compare_exchange_strong(expected,target,std::memory_order_acq_rel)||expected==target)
                return reinterpret_cast<FARPROC>(entries[i]);
        }
        return entry;
    }
    static FARPROC Bind(const char* name,FARPROC entry) noexcept {
        static const auto deviceEntries=DeviceEntries(std::make_index_sequence<8>{});
        static const auto chainEntries=ChainEntries(std::make_index_sequence<8>{});
        return !strcmp(name,"D3D11CreateDevice")?Bind(devices,deviceEntries,entry):Bind(chains,chainEntries,entry);
    }
};
bool PublishFactoryOriginal(const char* name,void* target,void* trampoline) noexcept {
    if (!strcmp(name,"CreateDXGIFactory")) return Gateways<Factory0,proxy::FactoryFn>::PublishOriginal(target,trampoline);
    if (!strcmp(name,"CreateDXGIFactory1")) return Gateways<Factory1,proxy::FactoryFn>::PublishOriginal(target,trampoline);
    if (!strcmp(name,"CreateDXGIFactory2")) return Gateways<Factory2,proxy::Factory2Fn>::PublishOriginal(target,trampoline);
    return false;
}
bool Eligible(HMODULE module,FARPROC original) noexcept {
    // Factory resolution may be called from small-stack engine workers too.
    std::unique_ptr<wchar_t[]> storage(new(std::nothrow) wchar_t[32768]);
    if (!storage) return false;
    wchar_t* path=storage.get();
    wchar_t system[MAX_PATH]{};
    const DWORD count=module?GetModuleFileNameW(module,path,32768):0;
    if (!count||count>=32768||!slots::ImageEntry(module,reinterpret_cast<void*>(original))) return false;
    const wchar_t* leaf=wcsrchr(path,L'\\'); leaf=leaf?leaf+1:path;
    bool eligible=false;
    if (!_wcsicmp(leaf,L"dxgi.dll")) {
        if (!GetSystemDirectoryW(system,MAX_PATH)||wcscat_s(system,L"\\dxgi.dll")) return false;
        eligible=!_wcsicmp(path,system);
        if (!eligible) {
            // Local ReShade DXGI exposes the same public factory ABI. Bind
            // only entries owned by this image; never patch its COM methods.
            eligible=true;
            for (const char* name:{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2",
                                  "ReShadeRegisterAddon","ReShadeUnregisterAddon"})
                eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
        }
    } else if (!_wcsicmp(leaf,L"d3d12.dll")) {
        if (!GetSystemDirectoryW(system,MAX_PATH)||wcscat_s(system,L"\\d3d12.dll")) return false;
        eligible=!_wcsicmp(path,system)
            && GetProcAddress(module,"D3D12CreateDevice")==original;
        if (!eligible && witcher_dots::GameProcess()) {
            // Witcher DOTS must observe the renderer's device even through a
            // local ReShade d3d12.dll (same public ABI); it resolves the native
            // device behind the returned wrapper itself.
            eligible=GetProcAddress(module,"D3D12CreateDevice")==original;
            for (const char* name:{"ReShadeRegisterAddon","ReShadeUnregisterAddon"})
                eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
        }
    } else if (!_wcsicmp(leaf,L"sl.interposer.dll")) {
        // Establish the expected public interposer export family in this exact
        // loaded image. Never bind a shared/private Streamline COM method.
        eligible=true;
        for (const char* name:{"slInit","slSetD3DDevice","CreateDXGIFactory1","CreateDXGIFactory2"})
            eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,name)))&&eligible;
    }
    HMODULE pinned=nullptr;
    return eligible&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(original),&pinned)&&pinned==module;
}
// dx11menu.1: the D3D11 device entries' owners. System32 d3d11.dll; also a local ReShade d3d11.dll and this module
// when it is named d3d11.dll (its d3d11 proxy family), which forward the same public ABI. The entry must be that
// exact image's own export, pinned for the process lifetime.
bool EligibleD3D11(HMODULE module,LPCSTR name,FARPROC original) noexcept {
    std::unique_ptr<wchar_t[]> storage(new(std::nothrow) wchar_t[32768]);
    if (!storage) return false;
    wchar_t* path=storage.get();
    wchar_t system[MAX_PATH]{};
    const DWORD count=module?GetModuleFileNameW(module,path,32768):0;
    if (!count||count>=32768||!slots::ImageEntry(module,reinterpret_cast<void*>(original))
        ||GetProcAddress(module,name)!=original) return false;
    const wchar_t* leaf=wcsrchr(path,L'\\'); leaf=leaf?leaf+1:path;
    if (_wcsicmp(leaf,L"d3d11.dll")||!GetSystemDirectoryW(system,MAX_PATH)||wcscat_s(system,L"\\d3d11.dll")) return false;
    bool eligible=!_wcsicmp(path,system);
    if (!eligible) {
        eligible=true;
        for (const char* entry:{"D3D11CreateDevice","D3D11CreateDeviceAndSwapChain"})
            eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,entry)))&&eligible;
        if (module!=single_module::Self())
            for (const char* entry:{"ReShadeRegisterAddon","ReShadeUnregisterAddon"})
                eligible=slots::ImageEntry(module,reinterpret_cast<void*>(GetProcAddress(module,entry)))&&eligible;
    }
    HMODULE pinned=nullptr;
    return eligible&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(original),&pinned)&&pinned==module;
}
// Where Winds Meet's packed executable resolves the interposer's factory
// exports itself: no import slot or GetProcAddress call reaches us, so only
// Streamline's swapchain is created. Route main-executable callers of this
// interposer's public factory entries through the same typed gateways.
// Streamline, middleware and RTXMFG callers keep the original entry.
void ArmInterposerFactories() noexcept {
    const HMODULE interposer=GetModuleHandleW(L"sl.interposer.dll");
    if (!interposer) return;
    for (const char* name:{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"}) {
        const FARPROC entry=GetProcAddress(interposer,name);
        if (!entry) continue;
        const FARPROC replacement=ResolveProc(interposer,name,entry);
        if (replacement==entry) continue;
        void* trampoline=nullptr;
        if (!caller_scoped_import::PrepareInterposerEntry(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(replacement),name,trampoline)
            || !PublishFactoryOriginal(name,reinterpret_cast<void*>(entry),trampoline)
            || !caller_scoped_import::Activate(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(replacement))) {
            wchar_t line[160]{};
            swprintf_s(line,L"MFG_PROXY_UI interposer-entry inactive symbol=%hs; other factory routes retained",name);
            single_module::Log(line);
        }
    }
}
}
FARPROC ResolveProc(HMODULE module,LPCSTR name,FARPROC original) noexcept {
    if (gInsideOverlay||!single_module::OwnsBackend()||!original||!name||reinterpret_cast<uintptr_t>(name)<=0xffff) return original;
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    return original;
#else
    if (!strcmp(name,"D3D12CreateDevice")) {
        if (!Eligible(module,original)||GetProcAddress(module,name)!=original) return original;
        return DeviceGateways::Bind(original);
    }
    // OptiScaler mode: the device gateway above (Witcher DOTS) stays; factory,
    // Vulkan and input routes exist only for RTXMFG's menu.
    if (!single_module::OverlayAllowed()) {
#if MFG_UNLOCK_OVERLAY_MENU_DRAW
        // vkprov.2: except the Vulkan instance and device lifecycle the XeMFG DLSS-G provider records into, while it
        // is installed (checked there); RTXMFG's menu stays off.
        if (name[0]=='v' && name[1]=='k') return vulkan::ResolveLifecycle(module,name,original);
#endif
        return original;
    }
    if (!strcmp(name,"CreateDXGIFactory")||!strcmp(name,"CreateDXGIFactory1")||!strcmp(name,"CreateDXGIFactory2")) {
        if (!Eligible(module,original)) return original;
        if (!strcmp(name,"CreateDXGIFactory")) return Gateways<Factory0,proxy::FactoryFn>::Bind(original);
        if (!strcmp(name,"CreateDXGIFactory1")) return Gateways<Factory1,proxy::FactoryFn>::Bind(original);
        return Gateways<Factory2,proxy::Factory2Fn>::Bind(original);
    }
    // dx11menu.1: none beside OptiScaler, whose own swap chain wrappers serve a DirectX 11 game, and none in a D3D12
    // or Vulkan application (graphics_api), which keeps its D3D11 entries untouched.
    if (D3D11DeviceName(name)) {
        if (optiscaler_host::Detected()||!graphics_api::D3D11Application()||!EligibleD3D11(module,name,original))
            return original;
        return D3D11Gateways::Bind(name,original);
    }
#if MFG_UNLOCK_OVERLAY_MENU_DRAW
    if (name[0]=='v' && name[1]=='k') return vulkan::Resolve(module,name,original);
    return ResolveBoundedInput(module,name,original);
#else
    return original;
#endif
#endif
}
bool OwnsInterface(void* object) noexcept {
    return object&&!gInsideOverlay&&proxy::Owned(static_cast<IUnknown*>(object));
}
void DescribeInterface(void* object,wchar_t* text,size_t count) noexcept {
    if (!text||!count) return;
    if (gInsideOverlay) { swprintf_s(text,count,L"%p (overlay call)",object); return; }
    proxy::Describe(static_cast<IUnknown*>(object),text,count);
}
bool WrapUpgradedInterface(void** output) noexcept {
    if (gInsideOverlay||!single_module::OverlayAllowed()) return false;
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    (void)output;
    return false;
#else
    return proxy::WrapUpgraded(output);
#endif
}
void ArmFactoryGateway() noexcept {
    static std::atomic_flag publishing=ATOMIC_FLAG_INIT;
    if(publishing.test_and_set(std::memory_order_acquire))return;
    struct ReleasePublication {std::atomic_flag& value;~ReleasePublication(){value.clear(std::memory_order_release);}} release{publishing};
    HMODULE executable=GetModuleHandleW(nullptr);
    slots::Batch batch;
    const bool loaderCallout=native::InsideLoader();
    const bool dotsDevice=witcher_dots::GameProcess();
    size_t deferred=0;
    // dx11menu.1: an executable linking D3D12 or Vulkan creation and no D3D11 creation gets no DirectX 11 menu.
    {
        const LinkedApis apis=ScanLinkedApis(executable);
        const uint32_t linked=!apis.valid||apis.d3d11?0u
            :(apis.d3d12?graphics_api::kD3D12Linked:0u)|(apis.vulkan?graphics_api::kVulkanLinked:0u);
        if (linked&&!(graphics_api::Seen()&linked)) {
            graphics_api::Note(linked);
            single_module::Log(L"MFG_PROXY_UI executable links D3D12 or Vulkan creation and no D3D11 creation: no DirectX 11 menu or D3D11 gateway (step=application_api)");
        }
    }
    const bool inspected=slots::VisitImports(executable,[&](const char* name,void** slot) {
        if (strcmp(name,"CreateDXGIFactory")&&strcmp(name,"CreateDXGIFactory1")&&strcmp(name,"CreateDXGIFactory2")
            && !(dotsDevice&&!strcmp(name,"D3D12CreateDevice"))
            && !D3D11DeviceName(name)
            && !(name[0]=='v'&&name[1]=='k')) return true;
        void* value=protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot));
        HMODULE owner=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(value),&owner)) return false;
        const auto original=reinterpret_cast<FARPROC>(value);
        const auto replacement=ResolveProc(owner,name,original);
        if (original==replacement) return true;
        if(!strcmp(name,"D3D12CreateDevice")||D3D11DeviceName(name)) {
            // DOTS requires this early, aligned import. Do not use the
            // factory-entry fallback for a different public ABI. The D3D11
            // device entries (dx11menu.1) likewise: aligned slots only;
            // GetProcAddress callers reach the same gateways.
            if(!slots::ImageSlot(executable,slot)) {++deferred;return true;}
            return batch.Add(executable,slot,value,reinterpret_cast<void*>(replacement));
        }
        // DllMain retains only aligned data publication. Public-entry fallback
        // is retried synchronously at slInit, before application graphics calls.
        if (loaderCallout && !slots::ImageSlot(executable,slot)) { ++deferred; return true; }
        return batch.Add(executable,slot,value,reinterpret_cast<void*>(replacement),name,&PublishFactoryOriginal);
    });
    if (!inspected||!batch.Publish()) single_module::Log(L"MFG_PROXY_UI application graphics import publication unavailable; dynamic gateways remain available");
    wchar_t line[160]{};
    swprintf_s(line,L"MFG_PROXY_UI armed mainGraphicsImports=%zu graphicsProbes=0 nativeTableWrites=0",batch.published);
    single_module::Log(line);
    if (deferred) single_module::Log(L"MFG_PROXY_UI unaligned graphics imports deferred to the pre-slInit boundary");
    // Entry relays need MinHook; never from a loader callout (DllMain).
    if (!loaderCallout) ArmInterposerFactories();
    application_imports::ArmStartupDependencies();
}
void InstallKnownModules() noexcept {
    if (single_module::OwnsBackend()) install::RequestInstall();
}
void BeforeStreamlineInit(const void* caller) noexcept {
    // Existing early boundary: aligned application imports or caller-scoped
    // public DXGI entries. No graphics creation or native COM table mutation.
    ArmFactoryGateway();
#if !MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    // Dying Light 2 / The Beast: the renderer DLL (rd3d12_x64_rwdi.dll) imports
    // slInit and resolves the interposer's factory exports itself, before this
    // boundary, so no import or resolver gateway sees them. Scope the relays to
    // that verified host as well. Published even if a relay was armed earlier.
    const HMODULE interposer=GetModuleHandleW(L"sl.interposer.dll");
    // OptiScaler UI-delegated mode wraps no factories, so no menu host is needed.
    if (!interposer || !single_module::OwnsBackend() || !single_module::OverlayAllowed()) return;
    HMODULE owner=nullptr;
    if (!caller || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(caller),&owner) || owner==GetModuleHandleW(nullptr)) return;
    const HMODULE host=application_imports::StreamlineHost(caller,interposer);
    const bool published=host && caller_scoped_import::SetInterposerHost(host);
    std::unique_ptr<wchar_t[]> path(new(std::nothrow) wchar_t[32768]);
    if (!path) return;
    const DWORD count=owner?GetModuleFileNameW(owner,path.get(),32768):0;
    if (!count || count>=32768) wcscpy_s(path.get(),32768,L"(unknown)");
    std::unique_ptr<wchar_t[]> line(new(std::nothrow) wchar_t[33000]);
    if (!line) return;
    swprintf_s(line.get(),33000,L"MFG_PROXY_UI interposer-entry host=%s caller=slInit path=%s",
        published?L"published":host?L"conflict":L"rejected",path.get());
    single_module::Log(line.get());
#else
    (void)caller;
#endif
}
}
