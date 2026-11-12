#include "ngx_mfg_gate.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <thread>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
static unsigned checks=0, failures=0;
static bool Check(bool ok,const char* text) { ++checks; if(!ok)++failures; printf("%s %s\n",ok?"PASS":"FAIL",text); fflush(stdout); return ok; }
int wmain(int argc,wchar_t** argv) {
    if(argc<2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    HMODULE provider=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!Check(provider!=nullptr,"unmodified real provider loads"))return 2;
    Check(!ngx_mfg_gate::Patch(nullptr,true).patched,"null owner rejected");
    Check(!ngx_mfg_gate::Patch(GetModuleHandleW(nullptr),true).patched,"non-provider owner rejected");
    // GitHub issue 149: a supported provider before 310.7 (AION 2's 310.6.0.0) has no count/index check to restore,
    // and RTX 40 holds the game at 2X with it. Run with an archived 310.1-310.6 provider and "missing".
    if(argc==3 && !wcscmp(argv[2],L"missing")) {
        Check(!ngx_mfg_gate::Missing(nullptr)&&!ngx_mfg_gate::Missing(GetModuleHandleW(nullptr)),"null and non-provider owners are not reported");
        Check(!ngx_mfg_gate::Find(provider).match&&ngx_mfg_gate::Missing(provider),"provider without the count/index check is reported missing");
        Check(!ngx_mfg_gate::Patch(provider,true).candidate&&!ngx_mfg_gate::Ready(provider),"nothing is published on it");
        printf("PROVIDER missing\n");
        printf("COMPLETE gate checks=%u failures=%u\n",checks,failures);return failures?1:0;
    }
    const auto site=ngx_mfg_gate::Find(provider);
    if(!Check(site.match&&site.functionBegin,"unique gate, aligned branch, decoded target and unwind bounds established"))return 2;
    Check(!ngx_mfg_gate::Missing(provider),"provider with the count/index check is not reported missing");
    printf("PROVIDER gateRva=0x%llX functionRva=0x%llX\n",
        static_cast<unsigned long long>(site.match-reinterpret_cast<uint8_t*>(provider)),
        static_cast<unsigned long long>(site.functionBegin-reinterpret_cast<uintptr_t>(provider)));
    std::array<uint8_t,13> original{}; memcpy(original.data(),site.match,original.size());
    Check(!ngx_mfg_gate::Patch(provider,false).patched&&!ngx_mfg_gate::Ready(provider)
        && !memcmp(original.data(),site.match,original.size()),"missing Ada adapter proof leaves provider unchanged");
    if(argc==3 && !wcscmp(argv[2],L"foreign")) {
        const auto changed=protected_pointer::ReplaceProtectedBytes(reinterpret_cast<uintptr_t>(site.match+2),
            ngx_mfg_gate::kOriginal.data(),ngx_mfg_gate::kReplacement.data(),2,
            PAGE_EXECUTE_READ,&VirtualProtect,&FlushInstructionCache,PAGE_EXECUTE_WRITECOPY);
        Check(changed.disposition==protected_pointer::PublishDisposition::ePublishedRestored,"foreign-writer fixture established");
        Check(!ngx_mfg_gate::Patch(provider,true).patched&&!ngx_mfg_gate::Ready(provider),"foreign replacement cannot claim publication ownership");
        printf("COMPLETE gate checks=%u failures=%u\n",checks,failures);return failures?1:0;
    }
    // Invoke only the proven CPU validation routine in this disposable process.
    // This is not CreateFeature, EvaluateFeature or generated-frame proof.
    const auto* start=reinterpret_cast<const uint8_t*>(site.functionBegin);
    constexpr uint8_t prefix[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x44,0x8b,0x81};
    if(!Check(!memcmp(start,prefix,sizeof(prefix)),"validator argument layout matches the decoded routine")) return 2;
    uint32_t countOffset=0; memcpy(&countOffset,start+sizeof(prefix),4);
    if(!Check(countOffset>=8&&countOffset+16<2048,"test-owned input fields fit the bounded buffer"))return 2;
    using Validate=uint32_t(__fastcall*)(void*,bool);
    const auto validate=reinterpret_cast<Validate>(site.functionBegin);
    std::array<uint8_t,2048> input{};
    auto run=[&](uint32_t count,uint32_t index,bool support){
        input.fill(0);memcpy(input.data()+countOffset,&count,4);memcpy(input.data()+countOffset-4,&index,4);
        return validate(input.data(),support);
    };
    for(uint32_t count=1;count<=5;++count)
        Check(run(count,1,false)==(count==1?1u:0xbad00005u),"native Ada validator accepts 2x and rejects higher counts");
    const bool dllMode=argc==4&&!wcscmp(argv[2],L"dll");
    ngx_mfg_gate::Result result{};
    if(dllMode) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;ComPtr<ID3D12Device> device;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 2;
        HMODULE cuda=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        using Init=int(WINAPI*)(unsigned);using Count=int(WINAPI*)(int*);using Luid=int(WINAPI*)(char*,unsigned*,int);
        auto init=cuda?reinterpret_cast<Init>(GetProcAddress(cuda,"cuInit")):nullptr;
        auto getCount=cuda?reinterpret_cast<Count>(GetProcAddress(cuda,"cuDeviceGetCount")):nullptr;
        auto getLuid=cuda?reinterpret_cast<Luid>(GetProcAddress(cuda,"cuDeviceGetLuid")):nullptr;
        int count=0;if(!init||!getCount||!getLuid||init(0)||getCount(&count)||count!=1)return 2;
        LUID cudaLuid{};unsigned nodeMask=0;if(getLuid(reinterpret_cast<char*>(&cudaLuid),&nodeMask,0))return 2;
        for(UINT i=0;!device;++i) {
            ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
            if(desc.VendorId==0x10de&&desc.AdapterLuid.LowPart==cudaLuid.LowPart&&desc.AdapterLuid.HighPart==cudaLuid.HighPart)
                D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
        }
        if(!Check(device!=nullptr,"physical NVIDIA D3D12 device created for DLL adapter validation"))return 2;
        HMODULE dll=LoadLibraryExW(argv[3],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
        const auto fixturePath=std::filesystem::path(exe).parent_path()/L"sl.interposer.dll";
        HMODULE interposer=LoadLibraryExW(fixturePath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        using SetDevice=uint32_t(*)(void*);
        auto setup=interposer?reinterpret_cast<SetDevice>(GetProcAddress(interposer,"slSetD3DDevice")):nullptr;
        if(!Check(dll&&setup&&setup(device.Get())==0,"exact normal DLL observes the real device through the existing public setup boundary"))return 2;
        result={true,!memcmp(site.match+2,ngx_mfg_gate::kReplacement.data(),2),site.match};
    } else result=ngx_mfg_gate::Patch(provider,true);
    DWORD protection=0; protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(site.match),protection,13);
    printf("PUBLISH candidate=%u patched=%u eligible=%u protection=0x%X bytes=%02X%02X\n",result.candidate,result.patched,
        dlssg_provider_policy::IsSupportedRetainedProvider(provider),protection,site.match[2],site.match[3]);
    if(!Check(result.patched&&(dllMode||ngx_mfg_gate::Ready(provider)),"normal publisher restores the provider MFG validation path"))return 1;
    Check(!memcmp(site.match,original.data(),2)&&!memcmp(site.match+4,original.data()+4,9),"only the two branch-opcode bytes changed");
    Check(protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(site.match),PAGE_EXECUTE_READ,13),"provider executable protection restored");
    for(uint32_t count=1;count<=5;++count) for(uint32_t index=1;index<=count;++index) {
        const auto code=run(count,index,false);
        float time=0;memcpy(&time,input.data()+countOffset+4,4);
        printf("VALIDATE multiplier=%u index=%u result=0x%08X position=%.6f\n",count+1,index,code,time);
        Check(code==1&&std::abs(time-float(index)/float(count+1))<1e-6f,"2x through 6x retain the correct ordered temporal fractions");
    }
    Check(run(0,1,false)==0xbad00005u,"zero count remains invalid");
    Check(run(6,1,false)==0xbad00005u,"count beyond the provider maximum remains invalid");
    Check(run(2,0,false)==0xbad00005u,"zero index remains invalid");
    Check(run(2,3,false)==0xbad00005u,"out-of-range index remains invalid");
    Check(run(1,1,false)==1,"unchanged 2x semantics remain accepted");
    if(!dllMode) {
        std::atomic<bool> concurrent{true};std::vector<std::thread> threads;
        for(unsigned i=0;i<8;++i)threads.emplace_back([&]{if(!ngx_mfg_gate::Patch(provider,true).patched)concurrent=false;});
        for(auto& thread:threads)thread.join();
        Check(concurrent&&ngx_mfg_gate::Ready(provider),"repeat and concurrent callers reuse the owned publication");
    }
    printf("COMPLETE gate checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
