#include "common.h"
#include <fstream>
using namespace harness;
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;
using Lookup=FARPROC(WINAPI*)(HMODULE,LPCSTR);
struct Probe {
    HMODULE kernel=nullptr,dxgi=nullptr;
    FARPROC expectedTick=nullptr,expectedFactory=nullptr;
    const wchar_t* renderer=nullptr;
    bool nativeOkay=false,rendererOkay=false,errorOkay=false;
    DWORD rendererLoadError=0;
    bool lookupFound=false;
    uintptr_t stackSpan=0;
};
DWORD WINAPI ResolveOnSmallStack(void* argument) {
    auto& probe=*static_cast<Probe*>(argument);
    ULONG_PTR low=0,high=0;GetCurrentThreadStackLimits(&low,&high);
    probe.stackSpan=high-low;
    // This successful, unrelated export follows the exact crashing main-IAT
    // resolver path. Do not catch stack overflow: the broken DLL must fail.
    probe.nativeOkay=GetProcAddress(probe.kernel,"GetTickCount64")==probe.expectedTick;
    SetLastError(0);
    probe.errorOkay=GetProcAddress(probe.kernel,"MfgMissingExportForRegression")==nullptr
        &&GetLastError()==ERROR_PROC_NOT_FOUND;
    const auto renderer=LoadLibraryExW(probe.renderer,nullptr,0);
    probe.rendererLoadError=renderer?ERROR_SUCCESS:GetLastError();
    const auto lookup=reinterpret_cast<Lookup>(GetProcAddress(renderer,"EngineLookup"));
    probe.lookupFound=lookup!=nullptr;
    if(lookup) {
        probe.rendererOkay=lookup(probe.kernel,"GetTickCount64")==probe.expectedTick
            &&lookup(probe.dxgi,"CreateDXGIFactory1")!=probe.expectedFactory;
        for(unsigned i=0;i<64;++i)
            probe.rendererOkay&=lookup(probe.kernel,"GetTickCount64")==probe.expectedTick;
    }
    return probe.nativeOkay&&probe.rendererOkay&&probe.errorOkay?0:1;
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc!=3)return 2;
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    const auto dxgi=SystemModule(L"dxgi.dll");
    const auto tick=GetProcAddress(kernel,"GetTickCount64");
    const auto factory=GetProcAddress(dxgi,"CreateDXGIFactory1");
    if(!Check(LoadCandidate(argv[2])!=nullptr,"exact candidate loads"))return 1;
    // Wait for publication into this executable's resolver IAT before starting
    // the limited-stack worker; otherwise this test could bypass the regression.
    bool ready=false;
    for(unsigned i=0;i<200&&!ready;++i) {
        std::ifstream file("RTXMFG-Universal.status.json");
        const std::string status{std::istreambuf_iterator<char>(file),{}};
        file.close();
        ready=status.find("\"mainResolverDiscoveryInstalled\":true")!=std::string::npos
            &&status.find("\"pid\":"+std::to_string(GetCurrentProcessId())+",")!=std::string::npos;
        if(!ready)Sleep(25);
    }
    if(!Check(ready,"main resolver hook is active before small-stack calls"))return 1;
    for(const SIZE_T reserve:{SIZE_T(64*1024),SIZE_T(128*1024)}) {
        const auto* name=reserve==64*1024?L"StackRenderer64.dll":L"StackRenderer128.dll";
        if(!Check(CopyFileW(L"EngineFactoryFixture.dll",name,FALSE)!=FALSE,"fresh local renderer fixture copied"))return 1;
        Probe probe{kernel,dxgi,tick,factory,name};
        HANDLE thread=CreateThread(nullptr,reserve,&ResolveOnSmallStack,&probe,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
        if(!Check(thread!=nullptr,"game-like worker created with bounded stack reservation"))return 1;
        const DWORD wait=WaitForSingleObject(thread,10000);
        if(wait!=WAIT_OBJECT_0){Check(false,"worker returns without hanging");TerminateProcess(GetCurrentProcess(),2);}
        DWORD exit=1;GetExitCodeThread(thread,&exit);CloseHandle(thread);
        printf("NOTE requestedStack=%zu committedStack=%zu nativeOkay=%u rendererLoadError=%lu lookupFound=%u\n",reserve,probe.stackSpan,probe.nativeOkay,probe.rendererLoadError,probe.lookupFound);
        Check(probe.stackSpan>0&&probe.stackSpan<=reserve,"worker uses the requested small stack");
        Check(probe.nativeOkay,"successful native export resolves without stack overflow");
        Check(probe.errorOkay,"missing native export retains its result and last error");
        Check(probe.rendererOkay,"late renderer discovery and its resolver work on the small stack");
    }
    return gFailures?1:0;
}
