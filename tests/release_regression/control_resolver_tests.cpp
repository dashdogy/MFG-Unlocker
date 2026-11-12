#include <Windows.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include "ui_status_json.h"
#include "build_variant.h"
#include "status_transport.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <array>
#include <cstring>
#include <cstdio>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

static bool ok=true;
static bool lockedMode=false;
static void Check(bool value,const char* what){printf("%s %s\n",value?"PASS":"FAIL",what);fflush(stdout);ok&=value;}
template<class T>T Proc(HMODULE m,const char* name){return reinterpret_cast<T>(GetProcAddress(m,name));}
static std::string Read(const std::filesystem::path& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
static bool Has(const std::string& s,const char* key,const char* value){return s.find(std::string("\"")+key+"\":"+value)!=std::string::npos;}
static std::string Wait(const std::filesystem::path& path,const char* key,const char* value){
    const ULONGLONG start=GetTickCount64();std::string s;
    do{
        if(lockedMode){
            auto reader=[](const std::wstring& p,std::string& out){out=Read(p);return !out.empty();};
            auto valid=[](const std::string& text){
                if(!ui_status_json::CompleteObject(text)||!Has(text,"version",MFG_STATUS_VERSION)
                    ||!Has(text,"pid",std::to_string(GetCurrentProcessId()).c_str())
                    ||!Has(text,"processBirth",std::to_string(diagnostic_paths::ProcessBirth()).c_str()))return false;
                const std::string key="\"heartbeat\":";const auto at=text.find(key);if(at==std::string::npos)return false;
                uint64_t stamp=0;auto parsed=std::from_chars(text.data()+at+key.size(),text.data()+text.size(),stamp);
                FILETIME time{};GetSystemTimeAsFileTime(&time);const auto now=((uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime)/10000000ull-11644473600ull;
                return parsed.ec==std::errc{}&&stamp<=now&&now-stamp<=5;
            };
            status_transport::Read(path.wstring(),s,reader,valid);
        }else s=Read(path);
        if(ui_status_json::CompleteObject(s)&&Has(s,key,value))return s;Sleep(25);
    }while(GetTickCount64()-start<7000);
    return s;
}
struct Guarded {
    void* pages=VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    template<class T>T* Copy(const T& value,size_t size){DWORD old=0;VirtualProtect(static_cast<char*>(pages)+4096,4096,PAGE_NOACCESS,&old);auto* p=static_cast<char*>(pages)+4096-size;memcpy(p,&value,size);return reinterpret_cast<T*>(p);}
    ~Guarded(){VirtualFree(pages,0,MEM_RELEASE);}
};
int wmain(int argc,wchar_t** argv){
    if(argc<3)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    auto cwd=std::filesystem::current_path();
    auto status=cwd/L"control.status.json";
    SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH",(cwd/L"control.status.json").c_str());
    SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH",(cwd/L"config.json").c_str());
    std::ofstream(cwd/L"config.json")<<"{\"mode\":\"fixed\",\"multiplier\":4}";
    HMODULE core=LoadLibraryW(argv[2]);Check(core!=nullptr,"fresh integrated DLL loads");if(!core)return 1;
    auto initial=Wait(status,"mainResolverDiscoveryInstalled","true");
    Check(Has(initial,"version",MFG_STATUS_VERSION),"candidate status uses the matching policy protocol");
    Check(Has(initial,"processBirth",std::to_string(diagnostic_paths::ProcessBirth()).c_str()),"snapshot identifies this exact process lifetime");
    Check(Has(initial,"mainResolverDiscoveryInstalled","true")&&Has(initial,"setOptionsSeen","false"),"initial state has no invented FG observation");
    HANDLE statusLock=INVALID_HANDLE_VALUE;std::string lockedContent;
    if(!wcscmp(argv[1],L"control-locked")){
        statusLock=CreateFileW(status.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        Check(statusLock!=INVALID_HANDLE_VALUE,"game-style handle locks the candidate's real status file");
        if(statusLock==INVALID_HANDLE_VALUE)return 1;
        lockedContent=Read(status);lockedMode=true;
    }
    HMODULE interposer=LoadLibraryW(L"sl.interposer.dll");
    HMODULE wrapper=LoadLibraryW(L"ControlWrapperFixture.dll");
    Check(interposer&&wrapper,"controlled interposer and unsigned-layout wrapper fixtures load");if(!interposer||!wrapper)return 1;
    auto lookup=Proc<PFun_slGetFeatureFunction*>(interposer,"slGetFeatureFunction");
    auto mode=Proc<void(*)(unsigned)>(interposer,"FixtureLookupMode");
    auto nativeSet=Proc<PFun_slDLSSGSetOptions*>(wrapper,"FixtureSetOptions");
    auto nativeGet=Proc<PFun_slDLSSGGetState*>(wrapper,"FixtureGetState");
    auto reject=Proc<void(*)(bool)>(wrapper,"FixtureRejectSet");
    auto last=Proc<uintptr_t(*)()>(wrapper,"FixtureLastOptions");
    auto generated=Proc<uint32_t(*)()>(wrapper,"FixtureGenerated");
    if(!lookup||!mode||!nativeSet||!nativeGet||!reject||!last||!generated)return 2;
    std::array<void*,4> entries{reinterpret_cast<void*>(nativeSet),reinterpret_cast<void*>(nativeGet),reinterpret_cast<void*>(GetProcAddress(wrapper,"slGetPluginFunction")),reinterpret_cast<void*>(GetProcAddress(wrapper,"FixtureFreeResources"))};
    std::array<std::array<unsigned char,24>,4> before{};
    for(size_t i=0;i<entries.size();++i)memcpy(before[i].data(),entries[i],24);
    void* ptr=nullptr;
    mode(1);auto result=lookup(sl::kFeatureDLSS_G,"slDLSSGSetOptions",ptr);
    Check(result==sl::Result::eErrorInvalidParameter&&ptr==reinterpret_cast<void*>(nativeSet),"failed lookup with non-null output remains unchanged");
    mode(0);lookup(sl::kFeatureDLSS,"slDLSSGSetOptions",ptr);
    Check(ptr==reinterpret_cast<void*>(nativeSet),"wrong feature cannot publish a DLSS-G route");
    lookup(sl::kFeatureDLSS_G,"unrelatedFunction",ptr);
    Check(ptr==reinterpret_cast<void*>(nativeSet),"unrelated function name remains unchanged");
    mode(2);void* data=nullptr;lookup(sl::kFeatureDLSS_G,"unrelatedFunction",data);
    lookup(sl::kFeatureDLSS_G,"slDLSSGSetOptions",ptr);
    Check(ptr!=nullptr&&ptr==data,"non-executable target is not replaced by a control thunk");
    mode(0);void* set=nullptr;void* get=nullptr;
    lookup(sl::kFeatureDLSS_G,"slDLSSGSetOptions",set);lookup(sl::kFeatureDLSS_G,"slDLSSGGetState",get);
    Check(set&&get&&set!=reinterpret_cast<void*>(nativeSet)&&get!=reinterpret_cast<void*>(nativeGet),"dynamic lookup publishes typed forwarding for an unpatchable wrapper");
    if(!set||!get||set==reinterpret_cast<void*>(nativeSet)||get==reinterpret_cast<void*>(nativeGet))return 1;
    mode(2);lookup(sl::kFeatureDLSS_G,"slDLSSGSetOptions",ptr);
    Check(ptr==data,"an existing route cannot admit a non-executable target");
    mode(3);lookup(sl::kFeatureDLSS_G,"slDLSSGSetOptions",ptr);
    Check(ptr==reinterpret_cast<void*>(GetProcAddress(wrapper,"FixtureAlternateSetOptions")),"changed resolved target cannot replace cached forwarding original");
    mode(0);
    for(size_t i=0;i<entries.size();++i)Check(memcmp(before[i].data(),entries[i],24)==0,"public and generic wrapper executable entries remain untouched");
    if(!set||!get)return 1;
    sl::DLSSGOptions full{};full.structVersion=sl::kStructVersion1;full.mode=sl::DLSSGMode::eOn;full.numFramesToGenerate=1;
    Guarded optionsMemory;auto* options=optionsMemory.Copy(full,offsetof(sl::DLSSGOptions,bReserved15));
    sl::ViewportHandle viewport(0);auto callSet=reinterpret_cast<PFun_slDLSSGSetOptions*>(set);auto callGet=reinterpret_cast<PFun_slDLSSGGetState*>(get);
    Check(callSet(viewport,*options)==sl::Result::eOk,"guard-page V1 options forward successfully");
    Check(last()==reinterpret_cast<uintptr_t>(options)&&generated()==1,"requested 4x cannot mutate unknown wrapper's original 2x options");
    auto on=Wait(status,"appliedFrameGenerationOn","true");
    Check(Has(on,"multiplier","4")&&Has(on,"appliedRevision","0"),"saved 4x request stays unapplied on the unsupported wrapper");
    Check(Has(on,"gameFrameGenerationOn","true")&&Has(on,"setOptionsSeen","true")&&Has(on,"setOptionsAccepted","true"),"game On and accepted On reach UI status");
    Check(Has(on,"activeWrapperObserved","true")&&Has(on,"activeWrapperPatched","false")&&Has(on,"bridgeReady","false")&&Has(on,"universalRouteFailure","2")&&Has(on,"safeMaximumMultiplier","2"),"observation does not authorize MFG or invented capacity");
    sl::DLSSGState fullState{};fullState.structVersion=sl::kStructVersion1;
    Guarded stateMemory;auto* state=stateMemory.Copy(fullState,offsetof(sl::DLSSGState,numFramesToGenerateMax));
    Check(callGet(viewport,*state,nullptr)==sl::Result::eOk&&state->numFramesActuallyPresented==2,"guard-page V1 state preserves native output without reading V2 capacity");
    auto sampled=Wait(status,"getStateSeen","true");Check(Has(sampled,"actualFramesPresented","2"),"native state reaches UI telemetry");
    reject(true);options->mode=sl::DLSSGMode::eOff;
    Check(callSet(viewport,*options)==sl::Result::eErrorInvalidParameter,"native rejected Off result is preserved");
    auto failed=Wait(status,"setOptionsAccepted","false");Check(Has(failed,"gameFrameGenerationOn","false")&&Has(failed,"appliedFrameGenerationOn","true"),"rejected Off changes intent but does not falsely turn accepted FG off");
    reject(false);Check(callSet(viewport,*options)==sl::Result::eOk,"native accepted Off is preserved");
    auto off=Wait(status,"appliedFrameGenerationOn","false");Check(Has(off,"gameFrameGenerationOn","false")&&Has(off,"setOptionsAccepted","true"),"accepted Off reaches UI status");
    Check(FreeLibrary(wrapper)&&GetModuleHandleW(L"ControlWrapperFixture.dll")==wrapper,"published callable owner stays alive after caller releases its handle");
    Check(callSet(viewport,*options)==sl::Result::eOk,"retained forwarding pointer remains callable");
    options->structVersion=6;
    Check(callSet(viewport,*options)==sl::Result::eOk,"unknown options version preserves the native result");
    auto unknown=Wait(status,"universalRouteFailure","5");
    Check(Has(unknown,"bridgeReady","false")&&Has(unknown,"universalRouteFailure","5"),"unknown structure version closes override eligibility");
    // nulltoken.1: a null frame token to slPCLSetMarker (Indiana Jones) reaches Streamline unchanged; the latency
    // monitor's wrap must not read it.
    {
        using PclAbi=sl::Result(*)(uint32_t,const sl::FrameToken*);
        struct TestToken final:sl::FrameToken{
            uint32_t id;
            explicit TestToken(uint32_t value):id(value){}
            operator uint32_t() const override{return id;}
        };
        auto pclCalls=Proc<unsigned(WINAPI*)()>(interposer,"FixturePclCalls");
        auto pclAddress=Proc<void*(WINAPI*)()>(interposer,"FixturePclAddress");
        void* pcl=nullptr;
        const bool wrapped=pclCalls&&pclAddress&&lookup(sl::kFeaturePCL,"slPCLSetMarker",pcl)==sl::Result::eOk
            &&pcl&&pcl!=pclAddress();
        Check(wrapped,"slPCLSetMarker lookup returns the latency monitor's wrap");
        if(wrapped){
            const unsigned before=pclCalls();
            Check(reinterpret_cast<PclAbi>(pcl)(0,nullptr)==sl::Result::eErrorMissingInputParameter&&pclCalls()==before+1,
                "null frame token reaches Streamline unchanged and its answer is returned");
            const TestToken token(7);
            Check(reinterpret_cast<PclAbi>(pcl)(0,&token)==sl::Result::eOk&&pclCalls()==before+2,
                "a valid frame token still forwards");
        }
    }
    if(lockedMode){
        Check(Read(status)==lockedContent,"candidate does not overwrite the game's locked snapshot in place");
        std::string alternate;
        Check(status_transport::ReadMemory(status.wstring(),alternate)&&ui_status_json::CompleteObject(alternate),"exact loaded candidate publishes a complete memory snapshot across module boundaries");
        std::ofstream(cwd/L"config.json")<<"{\"mode\":\"fixed\",\"multiplier\":3}";
        auto updated=Wait(status,"multiplier","3");
        Check(Has(updated,"multiplier","3"),"saved control changes still reach backend and alternate status");
        CloseHandle(statusLock);lockedMode=false;
        auto recovered=Wait(status,"multiplier","3");
        Check(Has(recovered,"multiplier","3")&&recovered!=lockedContent,"primary publication recovers after lock release");
    }
    printf("COMPLETE resolver control regression passed=%u\n",ok?1:0);return ok?0:1;
}
