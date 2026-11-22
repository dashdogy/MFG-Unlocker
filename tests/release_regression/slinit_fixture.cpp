#include <Windows.h>
#include <sl_core_types.h>
#include <atomic>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <sl_dlss_g.h>
static std::atomic<unsigned> calls{0};
static bool cacheVulkan = false;
static HMODULE vulkanLoader = nullptr;
static FARPROC cachedGipa = nullptr, cachedGdpa = nullptr, cachedCreateInstance = nullptr;
extern "C" __declspec(dllexport) void FixtureEnableVulkan(){cacheVulkan=true;}
extern "C" __declspec(dllexport) FARPROC WINAPI FixtureVulkanProc(const char* name){
    if(!vulkanLoader)return nullptr;
    if(!strcmp(name,"vkGetInstanceProcAddr"))return cachedGipa;
    if(!strcmp(name,"vkGetDeviceProcAddr"))return cachedGdpa;
    if(!strcmp(name,"vkCreateInstance"))return cachedCreateInstance;
    return GetProcAddress(vulkanLoader,name);
}
// Minimal ABI fixture, not an NVIDIA binary. It never loads a plugin/provider.
// The hotpatch build exports an assembly slInit with the real interposer's
// hotpatchable entry (slinit_fixture_hotpatch.asm) that jumps to this body.
#if defined(FIXTURE_HOTPATCH_SLINIT)
extern "C" sl::Result FixtureSlInitBody(const sl::Preferences&,uint64_t) {
#else
extern "C" __declspec(dllexport) sl::Result slInit(const sl::Preferences&,uint64_t) {
#endif
    ++calls;
    if(cacheVulkan){
        vulkanLoader=LoadLibraryW(L"vulkan-1.dll");
        cachedGipa=GetProcAddress(vulkanLoader,"vkGetInstanceProcAddr");
        cachedGdpa=GetProcAddress(vulkanLoader,"vkGetDeviceProcAddr");
        cachedCreateInstance=GetProcAddress(vulkanLoader,"vkCreateInstance");
    }
    return sl::Result::eOk;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureInitCalls(){return calls.load();}
// Exercise the host's intercepted loader return without a feature lookup.
extern "C" __declspec(dllexport) HMODULE WINAPI FixtureLoadProbe(const wchar_t* path){return LoadLibraryW(path);}
// Manual hooking: an identity upgrade. The real interposer returns its own
// proxy; the menu's contract is the same for any returned DXGI object.
static std::atomic<unsigned> upgrades{0};
extern "C" __declspec(dllexport) sl::Result slUpgradeInterface(void** baseInterface){
    if(!baseInterface||!*baseInterface)return sl::Result::eErrorInvalidParameter;
    ++upgrades;
    return sl::Result::eOk;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureUpgradeCalls(){return upgrades.load();}
// Counts calls so the entry is long enough for a relocated entry detour.
// With a probe wrapper set, it also records whether that wrapper's maximum
// clamp was already patched when this call began: sl.dlss_g's
// slOnPluginStartup reads its maximum inside slSetD3DDevice.
static std::atomic<unsigned> deviceSets{0};
static std::atomic<HMODULE> probeWrapper{nullptr};
static std::atomic<int> wrapperAtDeviceSet{-1};
extern "C" __declspec(dllexport) void WINAPI FixtureProbeWrapper(HMODULE wrapper){probeWrapper=wrapper;}
extern "C" __declspec(dllexport) int WINAPI FixtureWrapperAtDeviceSet(){return wrapperAtDeviceSet.load();}
#if defined(FIXTURE_HOTPATCH_SLINIT)
extern "C" sl::Result FixtureSlSetD3DDeviceBody(void*) {
#else
extern "C" __declspec(dllexport) sl::Result slSetD3DDevice(void*) {
#endif
    if(HMODULE wrapper=probeWrapper.load()){
        using ReadFn=int(WINAPI*)();
        auto read=reinterpret_cast<ReadFn>(GetProcAddress(wrapper,"FixtureClampPatched"));
        wrapperAtDeviceSet=read?read():-2;
    }
    ++deviceSets;return sl::Result::eOk;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureDeviceSets(){return deviceSets.load();}
static unsigned lookupMode = 0;
extern "C" __declspec(dllexport) void FixtureLookupMode(unsigned mode){lookupMode=mode;}
// nulltoken.1: Streamline's slPCLSetMarker answers a null frame token with eErrorMissingInputParameter (sl.pcl
// 2.14.16 did in Indiana Jones). Same ABI as PFun_slPCLSetMarker: the reference arrives as this pointer.
static std::atomic<unsigned> pclCalls{0};
static sl::Result FixturePclSetMarker(uint32_t, const sl::FrameToken* frame){
    ++pclCalls;
    return frame ? sl::Result::eOk : sl::Result::eErrorMissingInputParameter;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixturePclCalls(){return pclCalls.load();}
extern "C" __declspec(dllexport) void* WINAPI FixturePclAddress(){return reinterpret_cast<void*>(&FixturePclSetMarker);}
// slshutdown.1: Streamline's slShutdown unloads the plug-ins its plug-in manager loaded (sl.common last). With
// FixtureOwnPlugins the lookup below loads the wrapper with its own reference, as that manager does, and slShutdown
// releases it, recording whether that was the final release (Cyberpunk's exit crash: it was not, we kept sl.dlss_g).
static std::atomic<unsigned> shutdowns{0};
static std::atomic<int> wrapperUnloadedInShutdown{-1};
static bool ownPlugins=false;
static HMODULE ownedWrapper=nullptr;
extern "C" __declspec(dllexport) void WINAPI FixtureOwnPlugins(int own){ownPlugins=own!=0;}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureShutdownCalls(){return shutdowns.load();}
extern "C" __declspec(dllexport) int WINAPI FixtureWrapperUnloadedInShutdown(){return wrapperUnloadedInShutdown.load();}
extern "C" __declspec(dllexport) sl::Result slShutdown(){
    ++shutdowns;
    if(ownedWrapper){
        FreeLibrary(ownedWrapper);ownedWrapper=nullptr;
        wrapperUnloadedInShutdown=GetModuleHandleW(L"ControlWrapperFixture.dll")==nullptr?1:0;
    }
    return sl::Result::eOk;
}
extern "C" __declspec(dllexport) sl::Result slGetFeatureFunction(sl::Feature, const char* name, void*& function){
    if(name&&!strcmp(name,"slPCLSetMarker")){function=reinterpret_cast<void*>(&FixturePclSetMarker);return sl::Result::eOk;}
    HMODULE wrapper=nullptr;
    if(ownPlugins){
        if(!ownedWrapper)ownedWrapper=LoadLibraryW(L"ControlWrapperFixture.dll");
        wrapper=ownedWrapper;
    }else{
        wrapper=GetModuleHandleW(L"ControlWrapperFixture.dll");
        if(!wrapper)wrapper=LoadLibraryW(L"ControlWrapperFixture.dll");
    }
    if(!wrapper)return sl::Result::eErrorNotInitialized;
    function=reinterpret_cast<void*>(GetProcAddress(wrapper,
        name && strcmp(name,"slDLSSGGetState")==0 ? "FixtureGetState" : "FixtureSetOptions"));
    if(lookupMode==2)function=reinterpret_cast<void*>(&lookupMode);
    if(lookupMode==3)function=reinterpret_cast<void*>(GetProcAddress(wrapper,"FixtureAlternateSetOptions"));
    return lookupMode==1 ? sl::Result::eErrorInvalidParameter : sl::Result::eOk;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}

namespace {
HMODULE Dxgi(){wchar_t path[MAX_PATH]{};GetSystemDirectoryW(path,MAX_PATH);wcscat_s(path,L"\\dxgi.dll");return LoadLibraryW(path);}
}
// Private cached resolution inside an interposer. Only the application's
// lookup of these exported factory functions can reach the UI gateway.
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory1(REFIID iid,void** out){
    using Fn=HRESULT(WINAPI*)(REFIID,void**);static auto fn=reinterpret_cast<Fn>(GetProcAddress(Dxgi(),"CreateDXGIFactory1"));return fn(iid,out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory2(UINT flags,REFIID iid,void** out){
    using Fn=HRESULT(WINAPI*)(UINT,REFIID,void**);static auto fn=reinterpret_cast<Fn>(GetProcAddress(Dxgi(),"CreateDXGIFactory2"));return fn(flags,iid,out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory(REFIID iid,void** out){return FixtureFactory1(iid,out);}
static std::atomic<unsigned> deviceCalls{0};
static IUnknown* deviceAdapter=nullptr;
extern "C" __declspec(dllexport) unsigned WINAPI FixtureDeviceCalls(){return deviceCalls.load();}
extern "C" __declspec(dllexport) IUnknown* WINAPI FixtureDeviceAdapter(){return deviceAdapter;}
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureCreateDevice(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out){
    wchar_t path[MAX_PATH]{};GetSystemDirectoryW(path,MAX_PATH);wcscat_s(path,L"\\d3d12.dll");
    using Fn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    static auto fn=reinterpret_cast<Fn>(GetProcAddress(LoadLibraryW(path),"D3D12CreateDevice"));
    ++deviceCalls;deviceAdapter=adapter;return fn(adapter,level,iid,out);
}
