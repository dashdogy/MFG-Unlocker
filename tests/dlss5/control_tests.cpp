#include "build_variant.h"
#include "ui_status_json.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;
unsigned checks=0;
void Check(bool condition,const char* message){++checks;std::printf("%s %s\n",condition?"PASS":"FAIL",message);std::fflush(stdout);if(!condition)std::exit(1);}
std::string Read(const std::filesystem::path& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
bool Has(const std::string&s,const char* key,const char* value){return s.find(std::string("\"")+key+"\":"+value)!=std::string::npos;}
std::string Wait(const std::filesystem::path& path,const char* key,const char* value){
 const auto start=GetTickCount64();std::string text;
 do{text=Read(path);if(ui_status_json::CompleteObject(text)&&Has(text,key,value))return text;Sleep(25);}while(GetTickCount64()-start<7000);
 return text;
}
void Write(const std::filesystem::path& path,const std::string& text){std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<text;file.close();Check(Read(path)==text,"exact config written");}
int wmain(int argc,wchar_t**argv){
 if(argc!=2)return 2;
 const auto folder=std::filesystem::current_path()/L"control-test-work";std::filesystem::create_directories(folder);
 const auto config=folder/L"config.json",status=folder/L"status.json",dll=folder/L"winmm.dll";
 Check(CopyFileW(argv[1],dll.c_str(),FALSE)!=FALSE,"fresh candidate copied to its isolated proxy path");
 // A previous run's status must not satisfy this run's waits.
 std::error_code removed;std::filesystem::remove(status,removed);Check(!std::filesystem::exists(status),"no stale status file");
 SetEnvironmentVariableW(MFG_CONFIG_ENV_W,config.c_str());SetEnvironmentVariableW(MFG_STATUS_ENV_W,status.c_str());
 Write(config,"{\"multiplier\":2,\"followGame\":true}");
 HMODULE module=LoadLibraryW(dll.c_str());Check(module!=nullptr,"exact unified DLL loads");
 auto text=Wait(status,"dlss5OverallIntensity","1");Check(ui_status_json::CompleteObject(text)&&Has(text,"version",MFG_STATUS_VERSION),"new status protocol is complete");
 Check(Has(text,"dlss5","false")&&Has(text,"dlss5State","0")&&Has(text,"dlss5ResolutionScale","100")&&Has(text,"dlss5HookStage","0")&&Has(text,"dlss5ResolutionScaleAuto","false")&&Has(text,"dlss5AdaptiveExposure","false")
     &&Has(text,"dlss5Model","0")&&Has(text,"dlss5StructureIntensity","1")&&Has(text,"dlss5ToneIntensity","1")&&Has(text,"dlss5CharacterIntensity","-1")
     &&Has(text,"dlss5CharacterMask","true")&&Has(text,"dlss5UseExposureValue","1")&&Has(text,"dlss5DefaultsVersion","3")
     &&Has(text,"dlss5CompareSplitView","false")&&Has(text,"dlss5CompareSplit","50")&&Has(text,"dlss5CompareKey","0")&&Has(text,"dlss5FrameMs","0.000"),
     "old configs default to DLSS 5 off with the provider's look and Resolution Scale Auto off");
 // dx11nr.1: no DirectX 11 DLSS feature, no bridge refusal.
 Check(Has(text,"dlss5D3D11","false")&&Has(text,"dlss5D3D11Refusal","0"),"the DirectX 11 bridge reports nothing without a DirectX 11 game");
 Write(config,"{\"multiplier\":2,\"followGame\":true,\"dlss5\":true,\"dlss5Intensity\":37,\"dlss5Quality\":1,\"futureOption\":17}");
 text=Wait(status,"dlss5OverallIntensity","0.37");
 Check(Has(text,"dlss5","true")&&Has(text,"dlss5ResolutionScale","85")&&Has(text,"dlss5State","1"),"pre-dlss5.2 intent reaches the backend while waiting for real DLSS");
 Check(Has(text,"dlss5Evaluations","0"),"config changes invent no provider evaluation");
 const std::string saved="{\"multiplier\":2,\"followGame\":true,\"dlss5\":true,\"dlss5HookStage\":1,\"dlss5Model\":2,\"dlss5PassCount\":3,"
     "\"dlss5StructureIntensity\":0.5,\"dlss5Preset\":2,\"dlss5Exposure\":2.5,\"dlss5Encoding\":4,\"dlss5DiffuseWhite\":250,\"dlss5DiffuseWhiteOverride\":true,"
     "\"dlss5CompareSplitView\":true,\"dlss5CompareSplit\":30,\"dlss5CompareKey\":112}";
 Write(config,saved);text=Wait(status,"dlss5PassCount","3");
 Check(Has(text,"dlss5HookStage","1")&&Has(text,"dlss5Model","2")&&Has(text,"dlss5StructureIntensity","0.5")&&Has(text,"dlss5Preset","2")
     &&Has(text,"dlss5Exposure","2.5")&&Has(text,"dlss5Encoding","4")&&Has(text,"dlss5DiffuseWhite","250")&&Has(text,"dlss5DiffuseWhiteOverride","true")
     &&Has(text,"dlss5CompareSplitView","true")&&Has(text,"dlss5CompareSplit","30")&&Has(text,"dlss5CompareKey","112"),
     "every saved DLSS 5 setting reaches the backend");
 for(const auto* bad:{"\"dlss5\":\"true\",\"dlss5PassCount\":4","\"dlss5Model\":3","\"dlss5PassCount\":11","\"dlss5OverallIntensity\":1.5","\"dlss5Intensity\":101"}){
  Write(config,std::string("{\"multiplier\":2,")+bad+"}");Sleep(600);text=Read(status);
  Check(ui_status_json::CompleteObject(text)&&Has(text,"dlss5PassCount","3")&&Has(text,"dlss5Model","2"),"malformed or out-of-range NR settings preserve last valid intent");
 }
 Write(config,"{\"multiplier\":2,\"dlss5\":false,\"dlss5Model\":1}");text=Wait(status,"dlss5Model","1");
 Check(Has(text,"dlss5","false")&&Has(text,"dlss5State","0"),"off applies without a game or generated frame");
 // A generation 1 config with the old default look and Auto reaches the backend on the new defaults.
 Write(config,"{\"multiplier\":2,\"dlss5\":true,\"dlss5Model\":0,\"dlss5OverallIntensity\":1,\"dlss5StructureIntensity\":0.7,"
     "\"dlss5ToneIntensity\":0.3,\"dlss5CharacterIntensity\":0.5,\"dlss5ResolutionScale\":100,\"dlss5ResolutionScaleAuto\":true}");
 text=Wait(status,"dlss5","true");
 Check(Has(text,"dlss5StructureIntensity","1")&&Has(text,"dlss5ToneIntensity","1")&&Has(text,"dlss5CharacterIntensity","-1")
     &&Has(text,"dlss5ResolutionScaleAuto","false")&&Has(text,"dlss5DefaultsVersion","3"),"an old default look and Auto move to the new defaults");
 // A generation 2 file's Adaptive exposure (its old default, on) reaches the backend off; a
 // generation 3 file's stays as saved.
 Write(config,"{\"multiplier\":2,\"dlss5\":true,\"dlss5DefaultsVersion\":2,\"dlss5AdaptiveExposure\":true,\"dlss5Model\":2}");
 text=Wait(status,"dlss5Model","2");
 Check(Has(text,"dlss5AdaptiveExposure","false")&&Has(text,"dlss5DefaultsVersion","3"),"generation 2 Adaptive exposure moves to off");
 Write(config,"{\"multiplier\":2,\"dlss5\":true,\"dlss5DefaultsVersion\":3,\"dlss5AdaptiveExposure\":true,\"dlss5Model\":1}");
 text=Wait(status,"dlss5Model","1");
 Check(Has(text,"dlss5AdaptiveExposure","true")&&Has(text,"dlss5DefaultsVersion","3"),"generation 3 Adaptive exposure stays as saved");
 std::printf("DLSS5 exact-DLL control checks=%u PASS\n",checks);
}
