#include "common.h"
#include <thread>
#include <atomic>
extern "C" __declspec(dllimport) HRESULT WINAPI EngineCreateFactory(unsigned,REFIID,void**);
extern "C" __declspec(dllimport) FARPROC WINAPI EngineLookup(HMODULE,LPCSTR);
extern "C" __declspec(dllimport) bool WINAPI EngineInstallForeignFactory();
using namespace harness;
using Create=HRESULT(WINAPI*)(unsigned,REFIID,void**);
using Lookup=FARPROC(WINAPI*)(HMODULE,LPCSTR);
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc!=3)return 2;
    const std::wstring mode=argv[1];
    auto native=SystemModule(L"dxgi.dll");
    const auto factory=GetProcAddress(native,"CreateDXGIFactory1");
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    const auto tick=GetProcAddress(kernel,"GetTickCount64");
    EntryBytes entry;entry.Capture(reinterpret_cast<void*>(factory));
    // Imported renderer entry is captured before loading the candidate. The
    // candidate must cover its IAT without a main-executable resolver call.
    Create create=&EngineCreateFactory;
    Lookup lookup=&EngineLookup;
    if(mode==L"engine-conflict" && !Check(EngineInstallForeignFactory(),"foreign renderer factory chain installed"))return 1;
    HMODULE candidate=LoadCandidate(argv[2]);
    if(!Check(candidate!=nullptr,"candidate loads"))return 1;
    const bool excluded=mode==L"engine-middleware" || mode==L"engine-external";
    if(mode==L"engine-late" || excluded) {
        const wchar_t* path=mode==L"engine-middleware"?L"sl.enginefixture.dll":
            mode==L"engine-external"?L"..\\ExternalEngineFixture.dll":L"LateEngineFixture.dll";
        auto module=LoadLibraryExW(path,nullptr,0);
        if(!Check(module!=nullptr,"additional renderer image loads"))return 1;
        create=reinterpret_cast<Create>(GetProcAddress(module,"EngineCreateFactory"));
        lookup=reinterpret_cast<Lookup>(GetProcAddress(module,"EngineLookup"));
        if(!Check(create&&lookup,"renderer exports resolved before first factory call"))return 1;
    }
    if(!Check(lookup(kernel,"GetTickCount64")==tick,"unrelated renderer resolver result preserved"))return 1;
    SetLastError(0);
    Check(lookup(kernel,"MfgMissingExportForRegression")==nullptr && GetLastError()==ERROR_PROC_NOT_FOUND,
        "failed resolver preserves null result and native last error");
    if(mode==L"engine-vulkan-resolver") {
        HMODULE vulkan=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!Check(vulkan!=nullptr,"system Vulkan loader available")) return 1;
        // The executable's system resolver is also intercepted. Use a direct
        // export lookup taken through its original system entry as the control.
        auto systemLookup=reinterpret_cast<Lookup>(lookup(kernel,"GetProcAddress"));
        if(!Check(systemLookup!=nullptr,"system resolver available")) return 1;
        const auto original=systemLookup(vulkan,"vkGetInstanceProcAddr");
        const auto resolved=lookup(vulkan,"vkGetInstanceProcAddr");
        Check(original && resolved && resolved!=original,"renderer Vulkan resolver supplies a typed gateway");
        const auto unchanged=systemLookup(vulkan,"vkEnumerateInstanceExtensionProperties");
        Check(unchanged && lookup(vulkan,"vkEnumerateInstanceExtensionProperties")==unchanged,
            "unwrapped Vulkan export stays native");
        return gFailures?1:0;
    }
    if(excluded || mode==L"engine-conflict") {
        const auto before=Status(candidate).factoryWrappersCreated;
        ComPtr<IDXGIFactory1> result;
        Check(SUCCEEDED(create(1,IID_PPV_ARGS(&result))),"excluded or foreign factory forwards successfully");
        Check(Status(candidate).factoryWrappersCreated==before,"excluded or foreign factory remains unwrapped");
        if(excluded)Check(lookup(native,"CreateDXGIFactory1")==factory,"excluded resolver retains original factory address");
    } else {
        const auto before=Status(candidate).factoryWrappersCreated;
        ComPtr<IDXGIFactory1> result;
        if(mode==L"engine-dynamic") {
            using Factory=HRESULT(WINAPI*)(REFIID,void**);
            auto resolved=reinterpret_cast<Factory>(lookup(native,"CreateDXGIFactory1"));
            Check(reinterpret_cast<FARPROC>(resolved)!=factory,"renderer resolver supplies a typed factory gateway");
            Check(resolved && SUCCEEDED(resolved(IID_PPV_ARGS(&result))),"renderer dynamic factory succeeds");
        } else Check(SUCCEEDED(create(1,IID_PPV_ARGS(&result))),"renderer imported factory succeeds");
        Check(Status(candidate).factoryWrappersCreated==before+1,"renderer factory wrapped before returning to application");
        Check(Status(candidate).overlayInstallState==2,"renderer-only factory activates overlay");
        ComPtr<IDXGIFactory2> v2;
        Check(result && SUCCEEDED(result.As(&v2)),"factory QueryInterface preserves public revisions");
        ComPtr<IDXGIFactory> v0;
        Check(SUCCEEDED(create(0,IID_PPV_ARGS(&v0))),"CreateDXGIFactory ABI preserved");
        ComPtr<IDXGIFactory2> second;
        Check(SUCCEEDED(create(2,IID_PPV_ARGS(&second))),"CreateDXGIFactory2 ABI preserved");
        std::atomic<bool> transparent{true};
        std::thread threads[4];
        for(auto& t:threads)t=std::thread([&]{for(unsigned i=0;i<1000;++i)if(lookup(kernel,"GetTickCount64")!=tick)transparent=false;});
        for(auto& t:threads)t.join();
        Check(transparent,"concurrent unrelated renderer resolutions remain transparent");
    }
    Check(entry.Unchanged(),"system DXGI factory entry bytes unchanged");
    return gFailures?1:0;
}
