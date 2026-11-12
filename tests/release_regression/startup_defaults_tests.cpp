#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include "ui_status_json.h"
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode=argv[1];
    const auto config=std::filesystem::current_path()/L"startup.json";
    const auto status=std::filesystem::current_path()/L"startup.status.json";
    SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH",config.c_str());
    SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH",(std::filesystem::current_path()/L"startup.status.json").c_str());
    SetEnvironmentVariableW(L"RTX_MFG_ACTIVE_MULTIPLIER",mode==L"environment"?L"5":nullptr);
    std::string saved;
    if(mode==L"legacy") saved=R"({"mode":"fixed","multiplier":4})";
    else if(mode==L"fixed") saved=R"({"followGame":false,"mode":"fixed","multiplier":3})";
    else if(mode==L"dynamic") saved=R"({"followGame":false,"mode":"dynamic","multiplier":4,"dynamicTargetFrameRate":120})";
    else if(mode==L"follow") saved=R"({"followGame":true,"mode":"follow","multiplier":2})";
    else if(mode!=L"clean"&&mode!=L"environment") return 2;
    if(std::filesystem::exists(config)) return 2; // Runner supplies an empty directory.
    if(!saved.empty()) std::ofstream(config)<<saved;
    if(!LoadLibraryW(argv[2])) return 1;
    std::string result;
    for(unsigned attempt=0;attempt<240;++attempt)
    {
        std::ifstream file(status); result={std::istreambuf_iterator<char>(file),{}};
        if(ui_status_json::CompleteObject(result)) break;
        Sleep(25);
    }
    unsigned failures=0;
    auto check=[&](bool value,const char* text){printf("%s %s\n",value?"PASS":"FAIL",text);if(!value)++failures;};
    auto has=[&](const char* key,const char* value){return result.find(std::string("\"")+key+"\":"+value)!=std::string::npos;};
    check(ui_status_json::CompleteObject(result),"fresh DLL publishes complete startup status");
    const bool follow=mode==L"clean"||mode==L"follow";
    check(has("followGame",follow?"true":"false"),"startup control preserves default or explicit follow intent");
    check(has("mode",follow?"\"follow\"":mode==L"dynamic"?"\"dynamic\"":"\"fixed\""),"startup mode matches requested semantics");
    const char* multiplier=mode==L"fixed"?"3":mode==L"environment"?"5":mode==L"legacy"||mode==L"dynamic"?"4":"2";
    check(has("multiplier",multiplier),"saved or explicit multiplier is retained");
    if(mode==L"dynamic") check(has("dynamicTargetFrameRate","120"),"saved Dynamic target is retained");
    if(saved.empty()) check(!std::filesystem::exists(config),"default startup does not create an override file");
    else { std::ifstream file(config); std::string after{std::istreambuf_iterator<char>(file),{}};check(after==saved,"existing settings remain byte-identical"); }
    printf("COMPLETE startup defaults failures=%u\n",failures);
    return failures?1:0;
}
