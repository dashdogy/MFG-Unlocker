#include "dlss5.h"
#include "dlss5_auto.h"
#include "dlss5_settings.h"
#include "frame_gen_target.h"
#include "overlay_native.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <map>
#include <variant>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
struct Params final:NVSDK_NGX_Parameter {
 using Value=std::variant<unsigned long long,float,double,unsigned int,int,ID3D11Resource*,ID3D12Resource*,void*>;
 std::map<std::string,Value> values;
 template<class T> void Put(const char* key,T value){values[key]=value;}
 template<class T> NVSDK_NGX_Result Fetch(const char* key,T* out)const {
  const auto found=values.find(key);if(found==values.end())return NVSDK_NGX_Result_Fail;
  const auto* value=std::get_if<T>(&found->second);if(!value)return NVSDK_NGX_Result_Fail;
  *out=*value;return NVSDK_NGX_Result_Success;
 }
#define PARAM(T) void Set(const char*k,T v)override{Put(k,v);} NVSDK_NGX_Result Get(const char*k,T*out)const override{return Fetch(k,out);}
 PARAM(unsigned long long) PARAM(float) PARAM(double) PARAM(unsigned int) PARAM(int) PARAM(ID3D11Resource*) PARAM(ID3D12Resource*) PARAM(void*)
#undef PARAM
 void Reset()override{values.clear();}
};
unsigned creates=0,evaluates=0,releases=0,allocates=0,destroys=0;
bool failCreate=false,failEvaluate=false,failRelease=false;
NVSDK_NGX_Result createFailure=NVSDK_NGX_Result_FAIL_FeatureNotFound;
// Every line DLSS 5 logged, and how many contain `text`.
std::vector<std::wstring> logs;
void Capture(const wchar_t* line){logs.emplace_back(line);}
size_t Logged(const wchar_t* text){size_t n=0;for(const auto& line:logs)n+=line.find(text)!=std::wstring::npos;return n;}
float lastIntensity=-1,lastStructure=-1;int lastStyle=-1,lastPreset=-1;unsigned lastWidth=0;
// When set, the network's output is this image (in COPY_SOURCE) instead of its input.
ID3D12Resource* providerSource=nullptr;
// The depth, motion vectors and controls the provider's last evaluation read.
void *lastDepth=nullptr,*lastMotion=nullptr,*lastOutput=nullptr;int lastDepthWidth=0,lastMotionWidth=0,lastReset=-1,lastUi=-1;float lastMotionScale=0;
// The network input of the provider's last evaluation (hdrbridge.1 reads it back).
void* lastColor=nullptr;
// autorect.1: the Color and Output subrects of the provider's last evaluation (the network's rect).
int lastColorRectWidth=0,lastColorRectHeight=0,lastOutputRectWidth=0,lastOutputRectHeight=0;
// The game's reconstruction input at the moment its evaluation runs.
void* gameColorDuringEvaluate=nullptr;
// nrscale.1 (replace-memory): the video memory the provider's creates take and its releases give back, reported to
// DLSS 5 (testing::SetVideoMemory) while memoryBudget is set.
uint64_t memoryUsage=0,memoryBudget=0,memoryPerCreate=0;
void ReportMemory(){dlss5::testing::SetVideoMemory(memoryUsage,memoryBudget);}
NVSDK_NGX_Result NVSDK_CONV Allocate(NVSDK_NGX_Parameter** out){++allocates;*out=new Params;return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV Destroy(NVSDK_NGX_Parameter* params){++destroys;delete static_cast<Params*>(params);return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature feature,const NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out){
 ++creates;CHECK(static_cast<uint32_t>(feature)==18);unsigned width=0;float intensity=0;
 CHECK(params->Get("DLSSNR.Width",&width)==NVSDK_NGX_Result_Success && width>0 && width<=128);lastWidth=width;
 CHECK(params->Get("DLSSNR.Intensity",&intensity)==NVSDK_NGX_Result_Success && std::isfinite(intensity));
 CHECK(params->Get("DLSSNR.Hint.Render.Preset",&lastPreset)==NVSDK_NGX_Result_Success);
 if(failCreate)return createFailure;
 if(memoryBudget){memoryUsage+=memoryPerCreate;ReportMemory();}
 *out=reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t(0x1000+creates));return NVSDK_NGX_Result_Success;
}
void Barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);
}
NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback){
 ++evaluates;if(failEvaluate)return NVSDK_NGX_Result_FAIL_InvalidParameter;
 void* color=nullptr;void* output=nullptr;int reset=0;
 CHECK(params->Get("DLSSNR.Color",&color)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.Output",&output)==NVSDK_NGX_Result_Success && color!=output);
 CHECK(params->Get("DLSSNR.Reset",&reset)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.Intensity",&lastIntensity)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.LocalStructureStrength",&lastStructure)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.Style",&lastStyle)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.Depth",&lastDepth)==NVSDK_NGX_Result_Success&&params->Get("DLSSNR.MVec",&lastMotion)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.DepthSubrectWidth",&lastDepthWidth)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.MVecSubrectWidth",&lastMotionWidth)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.MVecScaleX",&lastMotionScale)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.UICorrection",&lastUi)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.ColorSubrectWidth",&lastColorRectWidth)==NVSDK_NGX_Result_Success
  &&params->Get("DLSSNR.ColorSubrectHeight",&lastColorRectHeight)==NVSDK_NGX_Result_Success);
 CHECK(params->Get("DLSSNR.OutputSubrectWidth",&lastOutputRectWidth)==NVSDK_NGX_Result_Success
  &&params->Get("DLSSNR.OutputSubrectHeight",&lastOutputRectHeight)==NVSDK_NGX_Result_Success);
 // The network's rect stays within the feature (DLSSNR's capacity) and its textures.
 {const auto desc=static_cast<ID3D12Resource*>(output)->GetDesc();
  CHECK(lastOutputRectWidth>0&&lastOutputRectHeight>0&&uint64_t(lastOutputRectWidth)<=desc.Width&&UINT(lastOutputRectHeight)<=desc.Height);}
 lastOutput=output;lastReset=reset;lastColor=color;
 if(evaluates==1)CHECK(reset==1);
 auto* src=static_cast<ID3D12Resource*>(color);auto* dst=static_cast<ID3D12Resource*>(output);
 if(providerSource){
  Barrier(cmd,dst,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=providerSource;to.pResource=dst;
  const auto desc=dst->GetDesc();const D3D12_BOX box{0,0,0,static_cast<UINT>(desc.Width),desc.Height,1};
  cmd->CopyTextureRegion(&to,0,0,0,&from,&box);
  Barrier(cmd,dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return NVSDK_NGX_Result_Success;
 }
 Barrier(cmd,src,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
 Barrier(cmd,dst,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
 cmd->CopyResource(dst,src);
 Barrier(cmd,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Barrier(cmd,dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle*){
 ++releases;if(failRelease)return NVSDK_NGX_Result_FAIL_PlatformError;
 if(memoryBudget){memoryUsage-=memoryPerCreate;ReportMemory();}
 return NVSDK_NGX_Result_Success;
}
// The lifecycle cases pin feature extents: Resolution Scale Auto off unless a case turns it on.
dlss5::Settings On(dlss5::Stage stage=dlss5::Stage::eUpscaled){dlss5::Settings s{};s.enabled=true;s.stage=static_cast<uint32_t>(stage);s.scaleAuto=false;return s;}
void Policies(){
 using namespace dlss5;
 Settings wild{};wild.stage=7;wild.resources=9;wild.intensity=2.0f;wild.structure=std::numeric_limits<float>::quiet_NaN();
 wild.scale=500.0f;wild.passes=40;wild.model=5;wild.preset=3;wild.exposure=-1.0f;wild.diffuseWhite=10.0f;
 const Settings fixed=Sanitized(wild);
 CHECK(fixed.stage==0&&fixed.resources==1&&fixed.intensity==1.0f&&fixed.structure==1.0f&&fixed.scale==200.0f);
 CHECK(fixed.passes==kMaxPasses&&fixed.model==0&&fixed.preset==0&&fixed.exposure==0.0f&&fixed.diffuseWhite==kMinDiffuseWhite);
 Settings a=On(),b=On();b.intensity=0.3f;b.structure=0.1f;b.characterMask=false;b.uiCorrection=2;b.encoding=3;
 CHECK(SameFeature(a,b));b.model=1;CHECK(!SameFeature(a,b));b=a;b.passes=2;CHECK(!SameFeature(a,b));
 b=a;b.scale=50;CHECK(!SameFeature(a,b));b=a;b.preset=2;CHECK(!SameFeature(a,b));b=a;b.stage=1;CHECK(!SameFeature(a,b));
 uint32_t w=0,h=0;CHECK(ProcessingExtent(3840,2160,100,w,h)&&w==3840&&h==2160);
 CHECK(ProcessingExtent(3840,2160,50,w,h)&&w==1920&&h==1080);CHECK(ProcessingExtent(3840,2160,0,w,h)&&w==960&&h==540);
 CHECK(ProcessingExtent(1000,1000,200,w,h)&&w==2000);CHECK(ProcessingExtent(10000,100,200,w,h)&&w==kMaxExtent);
 CHECK(!ProcessingExtent(0,10,100,w,h));
 Settings white=On();CHECK(DiffuseWhite(white)==100.0f&&WhiteScale(white)==1.0f);
 white.encoding=3;CHECK(DiffuseWhite(white)==203.0f);white.diffuseWhite=300;CHECK(DiffuseWhite(white)==203.0f);
 white.diffuseWhiteOverride=true;CHECK(DiffuseWhite(white)==300.0f&&std::abs(WhiteScale(white)-80.0f/300.0f)<1e-6f);
 white.encoding=0;CHECK(DiffuseWhite(white)==100.0f);
 CHECK(UiCorrectionValue(On())==0);Settings ui=On();ui.uiCorrection=2;CHECK(UiCorrectionValue(ui)==1);ui.uiCorrection=1;CHECK(UiCorrectionValue(ui)==0);
 // Present stage: Encoding Auto from the swapchain, UI Correction Auto, inputs.
 CHECK(PresentEncoding(10,1)==Encoding::eScrgb&&PresentEncoding(24,12)==Encoding::eHdr10&&PresentEncoding(24,0)==Encoding::eSrgb);
 CHECK(PresentEncoding(28,0)==Encoding::eSrgb&&PresentEncoding(87,0)==Encoding::eSrgb);
 Settings atPresent=On(Stage::ePresent);
 CHECK(ResolvedEncoding(atPresent,Encoding::eHdr10)==Encoding::eHdr10&&DiffuseWhite(atPresent,Encoding::eScrgb)==203.0f);
 CHECK(WhiteScale(atPresent,Encoding::eSrgb)==1.0f&&std::abs(WhiteScale(atPresent,Encoding::eHdr10)-80.0f/203.0f)<1e-6f);
 atPresent.encoding=1;CHECK(ResolvedEncoding(atPresent,Encoding::eHdr10)==Encoding::eLinear);atPresent.encoding=0;
 CHECK(UiCorrectionValue(atPresent,Stage::ePresent)==(kPresentAutoUiCorrection?1:0)&&UiCorrectionValue(atPresent,Stage::eUpscaled)==0);
 atPresent.uiCorrection=1;CHECK(UiCorrectionValue(atPresent,Stage::ePresent)==0);
 atPresent.uiCorrection=2;CHECK(UiCorrectionValue(atPresent,Stage::eRender)==1&&UiCorrectionValue(atPresent,Stage::ePresent)==1);
 Settings inputs=On(Stage::ePresent);
 CHECK(ChoosePresentInputs(inputs,true)==PresentInputs::eCaptured&&ChoosePresentInputs(inputs,false)==PresentInputs::eNeutral);
 inputs.resources=0;CHECK(ChoosePresentInputs(inputs,true)==PresentInputs::eNeutral);
 inputs.resources=2;CHECK(ChoosePresentInputs(inputs,true)==PresentInputs::eCaptured&&ChoosePresentInputs(inputs,false)==PresentInputs::eSkip);
 CHECK(CaptureCurrent(5,5)&&CaptureCurrent(6,5)&&!CaptureCurrent(7,5)&&!CaptureCurrent(4,5)&&!CaptureCurrent(3,0));
 // Block Hudless clears every HUD-less tag's resource in the forwarded copy.
 struct Tag{uint32_t type;void* resource;};Tag tags[]{{2,&checks},{1,&checks},{2,nullptr},{2,&checks}};
 CHECK(WithholdTags(tags,4,2)==2&&!tags[0].resource&&tags[1].resource&&!tags[2].resource&&!tags[3].resource);
 CHECK(WithholdTags(tags,4,2)==0);
 CHECK(CreateFailureState(0xBAD0000B)==State::eUnsupported);CHECK(CreateFailureState(kResultOutOfDate)==State::eDriverUpdate);
 // The control file codec: defaults, legacy keys, ranges, round trip.
 Settings parsed{};CHECK(settings_json::Parse("{\"multiplier\":2}",parsed)&&parsed==Settings{});
 CHECK(settings_json::Parse("{\"dlss5\":true,\"dlss5Intensity\":37,\"dlss5Quality\":1}",parsed));
 CHECK(parsed.enabled&&std::abs(parsed.intensity-0.37f)<1e-6f&&parsed.scale==85.0f&&parsed.stage==0&&!parsed.scaleAuto&&!parsed.adaptiveExposure);
 CHECK(settings_json::Parse("{\"dlss5OverallIntensity\":0.5,\"dlss5Intensity\":90,\"dlss5ResolutionScale\":60,\"dlss5Quality\":1}",parsed));
 CHECK(parsed.intensity==0.5f&&parsed.scale==60.0f);
 const Settings beforeBad=parsed;
 for(const char* bad:{"{\"dlss5\":\"true\"}","{\"dlss5Model\":3}","{\"dlss5PassCount\":0}","{\"dlss5OverallIntensity\":1.5}",
     "{\"dlss5HookStage\":4}","{\"dlss5ResolutionScaleAuto\":1}","{\"dlss5Intensity\":101}","{\"dlss5Model\":1.5}","{\"dlss5DiffuseWhite\":40}","{\"dlss5Exposure\":nan}"}){
  Settings attempt=beforeBad;CHECK(!settings_json::Parse(bad,attempt));CHECK(attempt==beforeBad);
 }
 Settings custom=On(Stage::eRender);custom.model=2;custom.structure=0.25f;custom.tone=0.6f;custom.characterMask=false;custom.passes=3;
 custom.scale=62.5f;custom.domain=1;custom.downsample=2;custom.upsample=1;custom.reconstruction=1;custom.preset=2;custom.signal=1;
 custom.encoding=4;custom.diffuseWhite=250;custom.diffuseWhiteOverride=true;custom.blockHudless=false;custom.exposure=2.5f;custom.exposureMode=2;
 custom.resources=2;custom.uiCorrection=2;custom.intensity=0.8f;custom.scaleAuto=false;custom.adaptiveExposure=false;
 std::string file="{\"multiplier\":2,\"dlss5Intensity\":44,\"dlss5Quality\":1,\"dlss5Model\":1}";
 CHECK(settings_json::Write(file,custom));
 CHECK(file.find("dlss5Quality")==std::string::npos&&file.find("\"dlss5Intensity\"")==std::string::npos);
 Settings back{};CHECK(settings_json::Parse(file,back)&&back==custom);
 CHECK(settings_json::Write(file,custom)&&settings_json::Parse(file,back)&&back==custom);
 const std::string status="{"+settings_json::StatusFields(custom).substr(1)+"}";
 CHECK(ui_status_json::CompleteObject(status));back={};CHECK(settings_json::Parse(status,back)&&back==custom);
 // A generation 1 config (no dlss5DefaultsVersion) moves an old default look to the new one and Auto at its old
 // default (on, up to 100 %) to off; custom strengths, other Auto maxima and marked configs stay as saved. Adaptive
 // exposure goes off for generations 1 and 2; a generation 3 file keeps it as saved.
 Settings migrated{};
 CHECK(settings_json::Parse("{\"dlss5\":true,\"dlss5AdaptiveExposure\":true,\"dlss5OverallIntensity\":1,\"dlss5Model\":0,\"dlss5StructureIntensity\":0.7,\"dlss5ToneIntensity\":0.3,"
     "\"dlss5CharacterIntensity\":0.5,\"dlss5ResolutionScale\":100,\"dlss5ResolutionScaleAuto\":true,\"dlss5LaterStructure\":0.7,"
     "\"dlss5LaterTone\":0.3,\"dlss5LaterCharacter\":0.5}",migrated));
 CHECK(migrated.enabled&&MatchingLook(migrated)==1&&migrated.structure==1.0f&&migrated.tone==1.0f&&migrated.character==-1.0f);
 CHECK(!migrated.scaleAuto&&migrated.scale==100.0f&&migrated.laterStructure==1.0f&&migrated.laterTone==1.0f&&migrated.laterCharacter==-1.0f);
 CHECK(migrated.defaultsVersion==kDefaultsVersion&&kDefaultsVersion==3&&!migrated.adaptiveExposure);
 migrated={};CHECK(settings_json::Parse("{\"dlss5OverallIntensity\":0.5,\"dlss5StructureIntensity\":0.7,\"dlss5ToneIntensity\":0.3,"
     "\"dlss5CharacterIntensity\":0.5}",migrated)&&MatchingLook(migrated)==0);
 migrated={};CHECK(settings_json::Parse("{\"dlss5Model\":1,\"dlss5StructureIntensity\":1,\"dlss5ToneIntensity\":1,"
     "\"dlss5CharacterIntensity\":0.8}",migrated)&&MatchingLook(migrated)==2);
 migrated={};CHECK(settings_json::Parse("{\"dlss5StructureIntensity\":0.6,\"dlss5ToneIntensity\":0.3,\"dlss5CharacterIntensity\":0.5,"
     "\"dlss5ResolutionScale\":85,\"dlss5ResolutionScaleAuto\":true}",migrated));
 CHECK(migrated.structure==0.6f&&migrated.tone==0.3f&&migrated.character==0.5f&&migrated.scaleAuto&&migrated.scale==85.0f);
 migrated={};CHECK(settings_json::Parse("{\"dlss5DefaultsVersion\":2,\"dlss5StructureIntensity\":0.7,\"dlss5ToneIntensity\":0.3,"
     "\"dlss5CharacterIntensity\":0.5,\"dlss5ResolutionScaleAuto\":true,\"dlss5AdaptiveExposure\":true}",migrated));
 CHECK(migrated.structure==0.7f&&migrated.tone==0.3f&&migrated.character==0.5f&&migrated.scaleAuto);
 CHECK(!migrated.adaptiveExposure&&migrated.defaultsVersion==3);
 migrated={};CHECK(settings_json::Parse("{\"dlss5DefaultsVersion\":3,\"dlss5AdaptiveExposure\":true}",migrated));
 CHECK(migrated.adaptiveExposure&&migrated.defaultsVersion==3);
 migrated={};CHECK(settings_json::Parse("{\"dlss5DefaultsVersion\":2}",migrated)&&!migrated.adaptiveExposure&&migrated==Settings{});
 // A later generation still parses (no whole-config refusal), and writing marks the file.
 migrated={};CHECK(settings_json::Parse("{\"dlss5DefaultsVersion\":7}",migrated)&&migrated.defaultsVersion==7);
 std::string marked="{\"multiplier\":2}";
 CHECK(settings_json::Write(marked,Settings{})&&marked.find("\"dlss5DefaultsVersion\":3")!=std::string::npos);
 // Frames for each stage.
 dlss_sr::CreateKeys keys{};keys.present=dlss_sr::CreateKeys::eRequired|dlss_sr::CreateKeys::eCreateFlags;
 keys.width=64;keys.height=32;keys.outWidth=128;keys.outHeight=64;keys.createFlags=kMotionLowResFlag|kDepthInvertedFlag;
 Source source{};source.output=1;source.color=4;source.depth=2;source.motion=3;source.colorX=5;source.colorY=6;Frame f{};
 CHECK(Build(source,keys,Stage::eUpscaled,true,f));CHECK(f.depthRect.width==64&&f.motionRect.width==64&&f.width==128&&f.color==1);
 CHECK(f.reset==1&&f.depthInverted==1);
 CHECK(Build(source,keys,Stage::eRender,false,f));CHECK(f.color==4&&f.width==64&&f.height==32&&(f.colorRect==Rect{5,6,64,32}));
 source.color=0;CHECK(!Build(source,keys,Stage::eRender,false,f));CHECK(Build(source,keys,Stage::eUpscaled,false,f));source.color=4;
 keys.createFlags=0;CHECK(Build(source,keys,Stage::eUpscaled,false,f));CHECK(f.motionRect.width==128);
 // A render subrect beyond the Create size is refused, not clamped: DLSS SR reads such a subrect whole. One pixel over
 // is the game's own rounding of its render size (GitHub issue 160, 3414 against 3413) and is taken as it is.
 InputsWhy why{};
 source.renderWidth=65;CHECK(Build(source,keys,Stage::eUpscaled,false,f,&why));CHECK(f.depthRect.width==65);
 source.renderWidth=66;CHECK(!Build(source,keys,Stage::eUpscaled,false,f,&why));CHECK(why.gap==InputGap::eRenderSize);source.renderWidth=0;
 source.renderHeight=33;CHECK(Build(source,keys,Stage::eUpscaled,false,f,&why));CHECK(f.depthRect.height==33);
 source.renderHeight=34;CHECK(!Build(source,keys,Stage::eUpscaled,false,f,&why));CHECK(why.gap==InputGap::eRenderSize);
 source.renderWidth=64;source.renderHeight=32;CHECK(Build(source,keys,Stage::eUpscaled,false,f,&why));source.renderWidth=source.renderHeight=0;
 source.motionScaleX=std::numeric_limits<float>::infinity();CHECK(!Build(source,keys,Stage::eUpscaled,false,f,&why));
 CHECK(why.gap==InputGap::eMotionScale);source.motionScaleX=1;
 source.output=0;CHECK(!Build(source,keys,Stage::eUpscaled,false,f,&why));CHECK(why.gap==InputGap::eResource&&!strcmp(why.key,"Output"));
 source.output=1;
 source.depth=0;CHECK(!Build(source,keys,Stage::eRender,false,f,&why));CHECK(why.gap==InputGap::eResource&&!strcmp(why.key,"Depth"));
 source.depth=2;
 { dlss_sr::CreateKeys partial=keys;partial.present=dlss_sr::CreateKeys::eCreateFlags;
   CHECK(!Build(source,partial,Stage::eUpscaled,false,f,&why));CHECK(why.gap==InputGap::eCreateKeys); }
 CHECK(RectFits({3,4,5,6},8,10));CHECK(!RectFits({3,4,6,6},8,10));CHECK(!RectFits({-1,0,5,6},8,10));
 Flight flight;CHECK(flight.Drained(0));CHECK(!flight.Record(0));CHECK(flight.Record(10));
 CHECK(!flight.Drained(99999));CHECK(!flight.CanRecord(20));CHECK(flight.CanRecord(10));
 CHECK(flight.Signaled(10,1,true));CHECK(!flight.Drained(0));CHECK(flight.Drained(1));CHECK(!flight.Drained(UINT64_MAX));
 CHECK(flight.Record(20));CHECK(!flight.Signaled(20,3,true));CHECK(!flight.Drained(99999));CHECK(!flight.CanRecord(20));
 Params params;Settings writer=On();writer.intensity=0.5f;writer.preset=2;writer.model=1;writer.characterMask=false;
 CHECK(WriteCreate(&params,128,64,writer));float value=0;int number=-1;unsigned extent=0;
 CHECK(params.Get("DLSSNR.Intensity",&value)==NVSDK_NGX_Result_Success&&value==0.5f);
 CHECK(params.Get("DLSSNR.Hint.Render.Preset",&number)==NVSDK_NGX_Result_Success&&number==2);
 CHECK(params.Get("DLSSNR.Style",&number)==NVSDK_NGX_Result_Success&&number==1);
 CHECK(params.Get("DLSSNR.UseAutoMask",&number)==NVSDK_NGX_Result_Success&&number==0);
 CHECK(params.Get("DLSSNR.Width",&extent)==NVSDK_NGX_Result_Success&&extent==128);
 CHECK(params.Get("DLSSNR.ScalingRatio",&value)==NVSDK_NGX_Result_Success&&value==1.0f);
 writer.structure=0.2f;CHECK(WriteEvaluate(&params,f,writer));int rect=0;CHECK(params.Get(keys::kOutputKeys.rect[2],&rect)==NVSDK_NGX_Result_Success);
 CHECK(params.Get("DLSSNR.LocalStructureStrength",&value)==NVSDK_NGX_Result_Success&&value==0.2f);
 // dlss5adv.1: later passes, the Advanced ranges, Boost tolerance and the new keys.
 Settings later=On();later.passes=3;later.model=0;later.intensity=0.9f;later.structure=0.7f;later.tone=0.3f;later.character=0.5f;
 later.laterModel=2;later.laterIntensity=0.4f;later.laterStructure=1.6f;later.laterTone=0.0f;later.laterCharacter=-1.0f;
 CHECK(PassSettings(later,1)==later&&LaterModel(later)==0);
 later.laterCustom=true;CHECK(PassSettings(later,0)==later);
 const Settings second=PassSettings(later,1);
 CHECK(second.model==2&&second.intensity==0.4f&&second.structure==1.6f&&second.tone==0.0f&&second.character==-1.0f&&second.passes==3);
 CHECK(PassSettings(later,2).model==2&&LaterModel(later)==2);
 Settings sameLook=later;sameLook.laterIntensity=0.1f;sameLook.laterTone=1.5f;CHECK(SameFeature(later,sameLook));
 Settings otherModel=later;otherModel.laterModel=1;CHECK(!SameFeature(later,otherModel));
 otherModel.passes=1;Settings onePass=later;onePass.passes=1;CHECK(SameFeature(onePass,otherModel));
 CHECK(WritePassControls(&params,later,1));
 CHECK(params.Get("DLSSNR.Style",&number)==NVSDK_NGX_Result_Success&&number==2);
 CHECK(params.Get("DLSSNR.LocalStructureStrength",&value)==NVSDK_NGX_Result_Success&&value==1.6f);
 CHECK(params.Get("DLSSNR.SkinStructureStrength",&value)==NVSDK_NGX_Result_Success&&value==-1.0f);
 CHECK(WritePassControls(&params,later,0)&&params.Get("DLSSNR.Style",&number)==NVSDK_NGX_Result_Success&&number==0);
 Settings wide{};wide.structure=1.8f;wide.tone=2.5f;wide.character=-0.4f;wide.laterCharacter=-3.0f;wide.blend=-1.0f;wide.color=9.0f;
 wide.highlightLimit=0.5f;wide.haloDark=2.0f;wide.haloRadius=9;wide.upsample=4;wide.reconstruction=3;wide.supersample=2;
 wide.knee=0.99f;wide.adaptiveTarget=0.01f;wide.boostTolerance=3;wide.debugView=4;wide.laterModel=3;
 const Settings clamped=Sanitized(wide);
 CHECK(clamped.structure==1.8f&&clamped.tone==kMaxStrength&&clamped.character==kMinCharacter&&clamped.laterCharacter==kMinCharacter);
 CHECK(clamped.blend==0.0f&&clamped.color==kMaxColor&&clamped.highlightLimit==kMinHighlightLimit&&clamped.haloDark==1.0f);
 CHECK(clamped.haloRadius==kMaxHaloRadius&&clamped.upsample==0&&clamped.reconstruction==0&&clamped.supersample==0);
 {Settings ratio{};ratio.reconstruction=2;CHECK(Sanitized(ratio).reconstruction==0);
  Settings saved{};saved.reconstruction=1;saved.intensity=0.4f;
  CHECK(settings_json::Parse("{\"dlss5ScaleReconstruction\":2,\"dlss5OverallIntensity\":0.6}",saved));
  CHECK(saved.reconstruction==0&&saved.intensity==0.6f);}
 CHECK(clamped.knee==kMaxKnee&&clamped.adaptiveTarget==kMinAdaptiveTarget&&clamped.boostTolerance==1&&clamped.debugView==0&&clamped.laterModel==0);
 Settings halfRate{};halfRate.boost=true;
 CHECK(kHalfRateOffered&&Sanitized(halfRate).boost);
 // Its look model grid: 16-texel cells up to about 1080 lines, a multiple of 16 above, partial cells counted.
 BoostGrid grid{};
 CHECK(BoostGridFor(1920,1080,grid)&&grid.cell==16&&grid.bins==kBoostBins&&grid.cellsX==120&&grid.cellsY==68);
 CHECK(BoostGridFor(2560,1440,grid)&&grid.cell==16&&grid.cellsX==160&&grid.cellsY==90);
 CHECK(BoostGridFor(3840,2160,grid)&&grid.cell==32&&grid.cellsX==120&&grid.cellsY==68);
 CHECK(BoostGridFor(1280,720,grid)&&grid.cell==16&&grid.cellsX==80&&grid.cellsY==45);
 CHECK(BoostGridFor(kMaxExtent,kMaxExtent,grid)&&grid.cell%16==0&&grid.cellsX*6<=kMaxExtent&&grid.cellsY*grid.bins<=kMaxExtent);
 CHECK(!BoostGridFor(0,1080,grid)&&!BoostGridFor(1920,kMaxExtent+1,grid));
 CHECK(BoostThresholds(1).stableBelow==0.02f&&BoostThresholds(1).unstableAbove==0.06f);
 CHECK(BoostThresholds(0).unstableAbove<BoostThresholds(1).unstableAbove&&BoostThresholds(2).stableBelow>BoostThresholds(1).stableBelow);
 Settings advanced=custom;advanced.laterCustom=true;advanced.laterModel=1;advanced.laterIntensity=0.6f;advanced.laterStructure=1.25f;
 advanced.laterTone=0.1f;advanced.laterCharacter=-1.0f;advanced.blend=1.4f;advanced.color=0.25f;advanced.highlightGuard=true;
 advanced.highlightLimit=3.5f;advanced.haloDark=0.75f;advanced.haloBright=0.2f;advanced.haloRadius=3;advanced.supersample=1;
 advanced.upsample=3;advanced.reconstruction=1;advanced.knee=0.65f;advanced.adaptiveTarget=0.3f;advanced.boostTolerance=2;
 advanced.debugView=3;advanced.compareSwap=true;advanced.structure=2.0f;advanced.character=-1.0f;
 std::string advancedFile="{\"multiplier\":2}";CHECK(settings_json::Write(advancedFile,advanced));
 back={};CHECK(settings_json::Parse(advancedFile,back)&&back==advanced);
 const std::string advancedStatus="{"+settings_json::StatusFields(advanced).substr(1)+"}";
 CHECK(ui_status_json::CompleteObject(advancedStatus));back={};CHECK(settings_json::Parse(advancedStatus,back)&&back==advanced);
 for(const char* bad:{"{\"dlss5UpsampleFilter\":4}","{\"dlss5ScaleReconstruction\":3}","{\"dlss5BlendStrength\":2.5}",
     "{\"dlss5HaloRadius\":0}","{\"dlss5HdrKnee\":0.2}","{\"dlss5LaterIntensity\":1.5}","{\"dlss5DebugView\":4}","{\"dlss5CharacterIntensity\":-2}"}){
  Settings attempt=advanced;CHECK(!settings_json::Parse(bad,attempt));CHECK(attempt==advanced);
 }
}
struct Gpu {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
 ComPtr<ID3D12GraphicsCommandList> cmd;ComPtr<ID3D12Fence> fence;uint64_t tick=0;HANDLE event=nullptr;
 Gpu(){
  ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
  CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));CHECK(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))));
  D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};CHECK(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue))));
  CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
  CHECK(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd))));
  CHECK(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(event);
 }
 ~Gpu(){if(event)CloseHandle(event);}
 void Submit(){
  CHECK(SUCCEEDED(cmd->Close()));ID3D12CommandList* lists[]{cmd.Get()};queue->ExecuteCommandLists(1,lists);
  CHECK(SUCCEEDED(queue->Signal(fence.Get(),++tick)));CHECK(SUCCEEDED(fence->SetEventOnCompletion(tick,event)));
  CHECK(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);CHECK(device->GetDeviceRemovedReason()==S_OK);
  CHECK(SUCCEEDED(allocator->Reset()));CHECK(SUCCEEDED(cmd->Reset(allocator.Get(),nullptr)));
 }
 ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,UINT16 mips=1){
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=64;d.DepthOrArraySize=d.SampleDesc.Count=1;d.MipLevels=mips;
  d.Format=format;d.Flags=flags;D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};ComPtr<ID3D12Resource> result;
  CHECK(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result))));return result;
 }
 ComPtr<ID3D12Resource> Buffer(UINT64 size,D3D12_HEAP_TYPE type){
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{type};ComPtr<ID3D12Resource> result;
  CHECK(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result))));return result;
 }
};
const uint16_t kPixel[]{0x3400,0x3C00,0x4400,0x3800}; // .25,1,4,.5: unclipped HDR + alpha
struct Images {
 Gpu& gpu;ComPtr<ID3D12Resource> upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
 explicit Images(Gpu& g,ID3D12Resource* like):gpu(g){
  auto desc=like->GetDesc();UINT64 bytes=0;gpu.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
  upload=gpu.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);readback=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
  unsigned char* mapped=nullptr;CHECK(SUCCEEDED(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)memcpy(mapped+y*footprint.Footprint.RowPitch+x*8,kPixel,8);
  upload->Unmap(0,nullptr);
 }
 // Fills texture (in COPY_DEST) with kPixel, leaving it in `after`.
 void Fill(ID3D12Resource* texture,D3D12_RESOURCE_STATES after){
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
  dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  gpu.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(gpu.cmd.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,after);gpu.Submit();
 }
 // Every texel of texture (in `state`) still equals kPixel.
 bool Unchanged(ID3D12Resource* texture,D3D12_RESOURCE_STATES state){
  Barrier(gpu.cmd.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
  gpu.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(gpu.cmd.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);gpu.Submit();
  unsigned char* mapped=nullptr;CHECK(SUCCEEDED(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
  bool same=true;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
   uint16_t result[4]{};memcpy(result,mapped+y*footprint.Footprint.RowPitch+x*8,8);
   if(same && memcmp(result,kPixel,8))std::fprintf(stderr,"texel %u,%u = %04X %04X %04X %04X\n",x,y,result[0],result[1],result[2],result[3]);
   same&=!memcmp(result,kPixel,8);
  }
  readback->Unmap(0,nullptr);return same;
 }
};
std::vector<unsigned char> Texels(Gpu& gpu,ID3D12Resource* texture,D3D12_RESOURCE_STATES state,unsigned texel);
float FromHalfBits(uint16_t h){ // normal halves only
 const int exponent=(h>>10)&31;const float mantissa=1.0f+(h&1023)/1024.0f;
 return (h&0x8000?-1.0f:1.0f)*(exponent?std::ldexp(mantissa,exponent-15):0.0f);
}
void Runtime(const std::string& mode){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 // The game's depth-stencil buffer as UE5 hands it to DLSS: typeless D32S8, no
 // typed shader view of ours (the provider alone reads depth).
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());CHECK(keys.createFlags==3);
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x100);
 GameCreated(1,handle,keys);Configure(On());
 // An optimal-settings query may overwrite the game's shared creation scalars.
 // The NR stage must use the dimensions captured for this exact source handle.
 game.Set("Width",1u);game.Set("Height",1u);game.Set("OutWidth",2u);game.Set("OutHeight",2u);
 failCreate=mode=="create-failure";failEvaluate=mode=="evaluate-failure";failRelease=mode=="release-failure";
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);};
 run();CHECK(creates==1&&evaluates==0);
 if(failCreate){gpu.Submit();CHECK(Read().state==State::eNotInstalled);run();CHECK(creates==1);BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(allocates==destroys);return;}
 BeforeRelease(1,handle);CHECK(releases==0);AfterRelease(1,handle,NVSDK_NGX_Result_Fail);CHECK(releases==0);
 gpu.Submit();game.Set("DLSS.Output.Subrect.Base.X",-1);run();CHECK(evaluates==0&&Read().state==State::eMissingInputs);
 run();CHECK(Logged(L"DLSS 5 inputs unavailable: DLSS.Output.Subrect.Base.X holds a negative value")==1);
 game.Set("DLSS.Output.Subrect.Base.X",0u);run();
 if(failEvaluate){CHECK(evaluates==1);CHECK(Read().state==State::eEvaluateFailed);gpu.Submit();
 failEvaluate=false;Settings retry=On();retry.intensity=0.71f;Configure(retry);run();CHECK(creates==2&&releases==1);gpu.Submit();run();CHECK(evaluates==2);gpu.Submit();BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==2&&allocates==destroys);return;}
 CHECK(evaluates==1);gpu.Submit();
 if(failRelease){BeforeRelease(1,handle);CHECK(releases==1);AfterRelease(1,handle,NVSDK_NGX_Result_Success);testing::Collect();CHECK(releases==1&&destroys==0);return;}
 // A render subrect beyond the Create size (Streamline 2.7.32 with empty tag
 // extents passes the color resource's size): refused and logged once, and
 // the stage runs again once the subrect fits.
 {
  game.Set("DLSS.Render.Subrect.Dimensions.Width",128u);game.Set("DLSS.Render.Subrect.Dimensions.Height",64u);
  const unsigned beforeSubrect=evaluates;
  for(unsigned n=0;n<3;++n){run();gpu.Submit();}
  CHECK(evaluates==beforeSubrect&&Read().state==State::eMissingInputs);
  CHECK(Logged(L"DLSS 5 inputs unavailable: render subrect 128x64 exceeds the DLSS Create size 64x64 (output 64x64)")==1);
  game.Set("DLSS.Render.Subrect.Dimensions.Width",64u);run();CHECK(evaluates==beforeSubrect+1&&creates==1);gpu.Submit();
  // Logged again after an evaluation ran.
  game.Set("DLSS.Render.Subrect.Dimensions.Width",128u);run();run();game.Set("DLSS.Render.Subrect.Dimensions.Width",64u);
  CHECK(evaluates==beforeSubrect+1&&Logged(L"render subrect 128x64 exceeds the DLSS Create size 64x64")==2);
 }
 // A mipped output (Control Resonant's HDR target has a full chain) runs on
 // mip 0, with only that subresource moved out of UNORDERED_ACCESS and back.
 {
  auto mipped=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,7);
  game.Set("Output",mipped.Get());const unsigned beforeMipped=evaluates;
  run();gpu.Submit();CHECK(evaluates==beforeMipped+1&&Read().state==State::eRunning);
  CHECK(Logged(L"DLSS 5: output has 7 mips, processing mip 0 only")==1);
  run();gpu.Submit();CHECK(evaluates==beforeMipped+2);
  CHECK(Logged(L"DLSS 5: output has 7 mips, processing mip 0 only")==1);
  game.Set("Output",output.Get());
 }
 // Provider pass-through makes the production HDR encode/decode a no-op, in
 // every Scaling Domain and NR Signal Encoding (RGB residual reconstruction).
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 for(uint32_t domain=0;domain<3;++domain)for(uint32_t signal=0;signal<3;++signal){
  Settings bridge=On();bridge.domain=domain;bridge.signal=signal;bridge.exposure=2.0f;Configure(bridge);
  const unsigned before=evaluates;run();CHECK(evaluates==before+1&&creates==1);gpu.Submit();
  CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 }
 // halfgrid.1: Half rate. The network evaluation fits the look model to its input and output; the Carry
 // evaluation between applies it to its own input. On the same input (refilled, zero motion), Carry gives the
 // network evaluation's result: the image itself with a pass-through network, and another image's change, in
 // each channel, with a network that outputs that image (the float fit's rounding aside, under 1 %). The
 // network runs on every other evaluation of the same feature.
 {
  const auto fill=[&](ID3D12Resource* texture,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after,const uint16_t* value,unsigned bytes){
   auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size=0;
   gpu.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&size);
   auto upload=gpu.Buffer(size,D3D12_HEAP_TYPE_UPLOAD);unsigned char* mapped=nullptr;
   CHECK(SUCCEEDED(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
   for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)memcpy(mapped+y*footprint.Footprint.RowPitch+x*bytes,value,bytes);
   upload->Unmap(0,nullptr);
   if(before!=D3D12_RESOURCE_STATE_COPY_DEST)Barrier(gpu.cmd.Get(),texture,before,D3D12_RESOURCE_STATE_COPY_DEST);
   D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   src.PlacedFootprint=footprint;dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   gpu.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(gpu.cmd.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,after);gpu.Submit();
  };
  const uint16_t still[4]{};fill(motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,still,4);
  // The other network output, in the network signal: .3, .6, .2, 1.
  const uint16_t other[4]{0x34CD,0x38CD,0x3266,0x3C00};
  auto otherNetwork=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
  fill(otherNetwork.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE,other,8);
  const auto refill=[&]{Barrier(gpu.cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
   images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);};
  const auto close=[](const std::vector<unsigned char>& a,const std::vector<unsigned char>& b,bool report=true){
   bool same=a.size()==64*64*8&&b.size()==a.size();
   for(size_t n=0;same && n<64*64*4;++n){
    uint16_t x=0,y=0;memcpy(&x,&a[n*2],2);memcpy(&y,&b[n*2],2);const float fx=FromHalfBits(x),fy=FromHalfBits(y);
    same=std::abs(fx-fy)<=0.01f*std::max(std::abs(fx),std::abs(fy))+1e-4f;
    if(!same && report)std::fprintf(stderr,"Half rate texel %zu channel %zu: %04X vs %04X\n",n/4,n%4,x,y);
   }
   return same;
  };
  Settings half=On();half.boost=true;Configure(half);
  const unsigned beforeHalf=evaluates;
  std::vector<unsigned char> original;
  for(ID3D12Resource* source:{static_cast<ID3D12Resource*>(nullptr),otherNetwork.Get()}){
   providerSource=source;
   refill();if(original.empty())original=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
   const unsigned beforeNetwork=evaluates;run();gpu.Submit();CHECK(evaluates==beforeNetwork+1);
   const auto network=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
   refill();run();gpu.Submit();CHECK(evaluates==beforeNetwork+1);
   const auto carried=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
   CHECK(close(network,carried));
   CHECK(source ? !close(network,original,false) : close(network,original));
  }
  providerSource=nullptr;
  CHECK(evaluates==beforeHalf+2&&creates==1&&Read().state==State::eRunning);
  CHECK(Logged(L"DLSS 5 Boost: the network runs on every other evaluation; the frames between rebuild its look from their own image (extent 64x64")==1);
  CHECK(Logged(L"model 4x4 cells of 16, 8 bins")==1&&Logged(L"DLSS 5 Boost unavailable")==0);
  refill();
 }
 Configure(On());
 const unsigned beforeRing=evaluates;
 for(unsigned n=0;n<8;++n)run();CHECK(evaluates==beforeRing+8);run();CHECK(evaluates==beforeRing+8);
 BeforeRelease(2,handle);CHECK(releases==0);gpu.Submit();
 // Image controls apply to the next evaluation of the same feature.
 Settings live=On();live.intensity=0.7f;live.structure=0.2f;live.characterMask=false;live.uiCorrection=2;Configure(live);
 run();CHECK(creates==1&&releases==0&&std::abs(lastIntensity-0.7f)<1e-6f&&std::abs(lastStructure-0.2f)<1e-6f);gpu.Submit();
 const unsigned stableEvaluations=evaluates;
 // autoscale.1: the model goes to a replacement created beside the live feature,
 // which keeps evaluating with its own model until the replacement's create
 // executed, so no evaluation goes unprocessed.
 Settings model=live;model.model=1;Configure(model);run();
 CHECK(creates==2&&releases==0&&evaluates==stableEvaluations+1&&lastStyle==0&&Read().pending);
 // Changed again before it is ready: that replacement goes, another is created.
 Settings scaled=model;scaled.scale=50;Configure(scaled);run();
 CHECK(creates==3&&releases==0&&lastWidth==32&&evaluates==stableEvaluations+2&&lastStyle==0);gpu.Submit();
 // Its create executed: it takes over with fresh history; the first replacement is released.
 run();CHECK(creates==3&&releases==1&&evaluates==stableEvaluations+3&&lastStyle==1&&lastReset==1&&!Read().pending);gpu.Submit();
 // The old feature goes once its last list executed.
 run();CHECK(releases==2&&evaluates==stableEvaluations+4&&lastReset==0);gpu.Submit();
 CHECK(Logged(L"DLSS 5 feature replaced without a gap")==1);
 // 50 %: down to 32x32 and back up, still exact for a pass-through network.
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 // dlss5adv.1: the Advanced decode controls on the same feature. A pass-through
 // network changes nothing, so Blend and Color strength, the highlight guard,
 // both halo reductions, Edge-aware upsampling, RGB residual reconstruction and another
 // HDR knee must leave the HDR image exact.
 {
  Settings advanced=scaled;advanced.blend=1.7f;advanced.color=0.3f;advanced.highlightGuard=true;advanced.highlightLimit=1.5f;
  advanced.haloDark=0.6f;advanced.haloBright=0.4f;advanced.haloRadius=3;advanced.upsample=3;advanced.reconstruction=0;advanced.knee=0.6f;
  advanced.laterCustom=true;advanced.laterTone=0.0f;
  const unsigned beforeAdvanced=evaluates;Configure(advanced);run();gpu.Submit();run();gpu.Submit();
  CHECK(evaluates==beforeAdvanced+2&&creates==3&&releases==2);
  CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
  // Debug view Change: mid grey (0.5 in a gamma-2.2 view of diffuse white, 1 here
  // without Adaptive exposure) where nothing changed.
  Settings view=advanced;view.debugView=3;view.adaptiveExposure=false;Configure(view);run();gpu.Submit();
  const auto grey=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);bool flat=grey.size()==64*64*8;
  for(size_t n=0;flat && n<64*64;++n){
   uint16_t h[4]{};memcpy(h,&grey[n*8],8);
   flat=h[0]==h[1]&&h[1]==h[2]&&std::abs(FromHalfBits(h[0])-std::pow(0.5f,2.2f))<0.01f&&h[3]==kPixel[3];
   if(!flat)std::fprintf(stderr,"change view texel %zu = %04X %04X %04X %04X\n",n,h[0],h[1],h[2],h[3]);
  }
  CHECK(flat);
  Barrier(gpu.cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
  images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Configure(scaled);
 }
 BeforeRelease(2,handle);AfterRelease(2,handle,NVSDK_NGX_Result_Success);CHECK(releases==2);
 // Chained passes: one feature per pass, every pass evaluated.
 Settings passes=scaled;passes.scale=100;passes.passes=3;passes.preset=2;Configure(passes);
 const unsigned beforeChain=evaluates;run();CHECK(creates==6&&releases==2&&lastPreset==2&&evaluates==beforeChain+1);gpu.Submit();
 const unsigned beforePasses=evaluates;run();CHECK(evaluates==beforePasses+3);gpu.Submit();
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 Settings off=passes;off.enabled=false;Configure(off);run();CHECK(Read().state==State::eOff);CHECK(evaluates==beforePasses+3);
 // Off releases the features once their work executed, and shows no cost.
 gpu.Submit();run();CHECK(releases==6&&Read().live==0&&Read().gpuMs<0.0f&&Read().boostNetworkMs<0.0f&&evaluates==beforePasses+3);
 // Render stage: our processed copy is the reconstruction input for exactly one call.
 Settings render=passes;render.enabled=true;render.stage=static_cast<uint32_t>(Stage::eRender);render.passes=1;Configure(render);
 auto evaluateRender=[&]{
  const auto swap=BeforeEvaluate(1,api,gpu.cmd.Get(),handle,&game);
  ID3D12Resource* during=nullptr;game.Get("Color",&during);gameColorDuringEvaluate=during;
  AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success,swap);
  ID3D12Resource* after=nullptr;CHECK(game.Get("Color",&after)==NVSDK_NGX_Result_Success&&after==color.Get());
  return swap;
 };
 auto swap=evaluateRender();CHECK(swap.slot==SIZE_MAX&&creates==7&&releases==6);gpu.Submit();
 const unsigned beforeRender=evaluates;swap=evaluateRender();
 CHECK(swap.slot!=SIZE_MAX&&swap.typed&&gameColorDuringEvaluate!=color.Get()&&evaluates==beforeRender+1);gpu.Submit();
 CHECK(images.Unchanged(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
 // RGBA16F motion vectors (Cyberpunk 2077) go to the provider as they are.
 {
  auto motion4=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  game.Set("MotionVectors",motion4.Get());const unsigned beforeRgba=evaluates;
  swap=evaluateRender();CHECK(swap.slot!=SIZE_MAX&&evaluates==beforeRgba+1&&lastMotion==motion4.Get());gpu.Submit();
  game.Set("MotionVectors",motion.Get());
 }
 // The Upscaled stage is not run while the Render stage is selected.
 const unsigned beforeUpscaled=evaluates;run();CHECK(evaluates==beforeUpscaled);gpu.Submit();
 Configure(passes);
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(allocates==destroys);
 // Reused numerical handle starts a fresh source generation.
 const unsigned beforeReuse=creates;GameCreated(1,handle,keys);run();CHECK(creates==beforeReuse+3);gpu.Submit();
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==creates&&allocates==destroys);
 // DLSS 5 Auto: Hook Method Auto runs after upscaling; Resolution Scale Auto
 // starts at the stage extent's level and never above the slider; the stage's
 // GPU time and luminance come back once its descriptor set completed; the
 // adaptive exposure gain keeps the residual exact.
 GameCreated(1,handle,keys);
 Settings automatic=On(Stage::eAuto);automatic.scaleAuto=true;automatic.scale=50;automatic.adaptiveExposure=true;Configure(automatic);
 const unsigned beforeAuto=creates;run();CHECK(creates==beforeAuto+1&&lastWidth==32);gpu.Submit();
 for(unsigned n=0;n<4;++n){run();gpu.Submit();}
 const auto measured=Read();
 CHECK(measured.stage==static_cast<uint32_t>(Stage::eUpscaled)&&measured.scale==50.0f&&!measured.pending);
 CHECK(std::isfinite(measured.gpuMs)&&measured.gpuMs>=0.0f&&measured.budgetMs>0.0f);
 // The test color's luminance (about 1.06 x diffuse white) asks for a gain toward 0.18.
 CHECK(measured.exposureGain>0.1f&&measured.exposureGain<0.5f);
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 // SDR color keeps a unit gain, and the status reports the gain applied.
 automatic.encoding=static_cast<uint32_t>(Encoding::eSrgb);Configure(automatic);
 run();gpu.Submit();CHECK(Read().exposureGain==1.0f);automatic.encoding=static_cast<uint32_t>(Encoding::eAuto);
 // Auto off: the slider value itself, and a unit gain without adaptive exposure.
 automatic.scaleAuto=false;automatic.scale=100;automatic.adaptiveExposure=false;Configure(automatic);
 run();CHECK(lastWidth==64);gpu.Submit();run();gpu.Submit();CHECK(Read().exposureGain==1.0f);
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==creates&&allocates==destroys);
 // Present is a working Hook Method: no swapchain presented yet, nothing withheld.
 Settings present=passes;present.stage=static_cast<uint32_t>(Stage::ePresent);Configure(present);
 CHECK(PresentStageSelected()&&!BlockHudless()&&Read().state!=State::eStageUnavailable);
 ForeignCreated();CHECK(Read().state==State::eOtherMod);Configure(Settings{});CHECK(Read().state==State::eOff&&!PresentStageSelected());
}
// Bytes of every texel of a 64x64 texture (in `state`).
std::vector<unsigned char> Texels(Gpu& gpu,ID3D12Resource* texture,D3D12_RESOURCE_STATES state,unsigned texel){
 auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
 gpu.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);auto readback=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
 Barrier(gpu.cmd.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
 D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
 gpu.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(gpu.cmd.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);gpu.Submit();
 unsigned char* mapped=nullptr;CHECK(SUCCEEDED(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
 std::vector<unsigned char> out(size_t(desc.Width)*desc.Height*texel);
 for(unsigned y=0;y<desc.Height;++y)memcpy(out.data()+size_t(y)*desc.Width*texel,mapped+size_t(y)*footprint.Footprint.RowPitch,size_t(desc.Width)*texel);
 readback->Unmap(0,nullptr);return out;
}
// Fills a 64x64 texture (in COPY_DEST) texel by texel from `pattern(x,y,out)`, leaving it in `after`.
template<class Pattern> void FillTexels(Gpu& gpu,ID3D12Resource* texture,unsigned texel,D3D12_RESOURCE_STATES after,Pattern pattern){
 auto desc=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
 gpu.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);auto upload=gpu.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
 unsigned char* mapped=nullptr;CHECK(SUCCEEDED(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped))));
 for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)pattern(x,y,mapped+size_t(y)*footprint.Footprint.RowPitch+size_t(x)*texel);
 upload->Unmap(0,nullptr);
 D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
 dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 gpu.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(gpu.cmd.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,after);gpu.Submit();
}
uint16_t Half(float value){ // exact for the small values used here
 uint32_t bits=0;memcpy(&bits,&value,4);const uint32_t sign=(bits>>16)&0x8000;const int exponent=int((bits>>23)&0xFF)-127+15;
 return static_cast<uint16_t>(value==0?sign:sign|(exponent<<10)|((bits>>13)&0x3FF));
}
// The Present stage on WARP: the swapchain image (render target, no UAV, in
// PRESENT) is processed on the presentation queue with a pass-through provider.
// ReShade 6.8.0's IID_UnwrappedObject and Streamline's base-interface IID.
constexpr GUID kReShadeUnwrap={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
constexpr GUID kStreamlineBase={0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
// A device object of a wrapper layer: only its reference count is ever used.
// With `real`, it answers Streamline's base-interface IID with that device.
struct WrapperDevice:IUnknown{
 ULONG refs=1;ID3D12Device* real=nullptr;
 WrapperDevice()=default;
 explicit WrapperDevice(ID3D12Device* r):real(r){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override{
  if(real && riid==kStreamlineBase){real->AddRef();*out=real;return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{return --refs;}
};
// A ReShade- or Streamline-style presentation queue: forwards to the real
// queue, reports the wrapper's own device object, and with `unwraps` answers
// `iid` (ReShade's IID_UnwrappedObject, or Streamline's base-interface IID)
// with the real queue.
struct WrapperQueue:ID3D12CommandQueue{
 ID3D12CommandQueue* real;IUnknown* device;bool unwraps;GUID iid;ULONG refs=1;unsigned executes=0;
 WrapperQueue(ID3D12CommandQueue* r,IUnknown* d,bool u,const GUID& i=kReShadeUnwrap):real(r),device(d),unwraps(u),iid(i){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override{
  if(unwraps && riid==iid){real->AddRef();*out=real;return S_OK;}
  if(riid==__uuidof(IUnknown)||riid==__uuidof(ID3D12Object)||riid==__uuidof(ID3D12DeviceChild)||riid==__uuidof(ID3D12Pageable)
     ||riid==__uuidof(ID3D12CommandQueue)){AddRef();*out=this;return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{return --refs;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g,UINT* n,void* d) override{return real->GetPrivateData(g,n,d);}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g,UINT n,const void* d) override{return real->SetPrivateData(g,n,d);}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,const IUnknown* d) override{return real->SetPrivateDataInterface(g,d);}
 HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override{return real->SetName(n);}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** out) override{device->AddRef();*out=device;return S_OK;}
 void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r,UINT n,const D3D12_TILED_RESOURCE_COORDINATE* c,const D3D12_TILE_REGION_SIZE* z,
  ID3D12Heap* h,UINT m,const D3D12_TILE_RANGE_FLAGS* f,const UINT* o,const UINT* t,D3D12_TILE_MAPPING_FLAGS x) override{real->UpdateTileMappings(r,n,c,z,h,m,f,o,t,x);}
 void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d,const D3D12_TILED_RESOURCE_COORDINATE* dc,ID3D12Resource* s,
  const D3D12_TILED_RESOURCE_COORDINATE* sc,const D3D12_TILE_REGION_SIZE* z,D3D12_TILE_MAPPING_FLAGS f) override{real->CopyTileMappings(d,dc,s,sc,z,f);}
 void STDMETHODCALLTYPE ExecuteCommandLists(UINT n,ID3D12CommandList* const* l) override{++executes;real->ExecuteCommandLists(n,l);}
 void STDMETHODCALLTYPE SetMarker(UINT m,const void* d,UINT n) override{real->SetMarker(m,d,n);}
 void STDMETHODCALLTYPE BeginEvent(UINT m,const void* d,UINT n) override{real->BeginEvent(m,d,n);}
 void STDMETHODCALLTYPE EndEvent() override{real->EndEvent();}
 HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* f,UINT64 v) override{return real->Signal(f,v);}
 HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f,UINT64 v) override{return real->Wait(f,v);}
 HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f) override{return real->GetTimestampFrequency(f);}
 HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g,UINT64* c) override{return real->GetClockCalibration(g,c);}
 D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override{return real->GetDesc();}
};
void PresentRuntime(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto image=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
 Images images(gpu,image.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 images.Fill(image.Get(),D3D12_RESOURCE_STATE_PRESENT);
 // UE5's depth-stencil depth (typeless D32S8) cleared to 0.25, and motion vectors (x/16, -1-y/16).
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 {
  ComPtr<ID3D12DescriptorHeap> dsv;D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1};
  CHECK(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&dsv))));
  D3D12_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D32_FLOAT_S8X24_UINT,D3D12_DSV_DIMENSION_TEXTURE2D};
  gpu.device->CreateDepthStencilView(depth.Get(),&view,dsv->GetCPUDescriptorHandleForHeapStart());
  gpu.cmd->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,0.25f,7,0,nullptr);
  Barrier(gpu.cmd.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);gpu.Submit();
 }
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 const auto motionAt=[](unsigned x,unsigned y,unsigned char* out){const uint16_t v[2]{Half(x/16.0f),Half(-1.0f-y/16.0f)};memcpy(out,v,4);};
 FillTexels(gpu,motion.Get(),4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,motionAt);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 game.Set("MV.Scale.X",2.0f);game.Set("MV.Scale.Y",-1.0f);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x200);
 GameCreated(1,handle,keys);
 Settings settings=On(Stage::ePresent);Configure(settings);CHECK(PresentStageSelected()&&!BlockHudless());
 PresentTarget target{gpu.queue.Get(),image.Get(),1/*DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709*/,0x5000};
 auto evaluate=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);gpu.Submit();};
 auto present=[&](const PresentTarget& t){Present(t);gpu.Submit();};
 // Before any DLSS evaluation there is no NGX route to create the feature through.
 present(target);CHECK(creates==0&&Read().state==State::eWaitingForDlss&&!BlockHudless());
 // The DLSS evaluation records the route and a copy of its depth and motion vectors;
 // the next present creates the feature on our list.
 evaluate();CHECK(creates==0&&evaluates==0);
 present(target);CHECK(creates==1&&evaluates==0&&lastWidth==64);
 CHECK(images.Unchanged(image.Get(),D3D12_RESOURCE_STATE_PRESENT));
 // Optional with this frame's DLSS resources: the provider reads our copies.
 // (Read before the GPU wait: WARP's first run of the shaders can exceed the
 // one-second Running window.)
 evaluate();Present(target);const auto snapshot=Read();gpu.Submit();
 CHECK(evaluates==1&&lastReset==1&&lastOutput!=image.Get()&&BlockHudless());
 CHECK(lastDepth&&lastDepth!=depth.Get()&&lastMotion&&lastMotion!=motion.Get()&&lastDepthWidth==64&&lastMotionWidth==64&&lastMotionScale==2.0f);
 CHECK(lastUi==(kPresentAutoUiCorrection?1:0));
 CHECK(snapshot.state==State::eRunning&&snapshot.stage==3&&snapshot.route==1&&snapshot.width==64&&snapshot.live==1);
 // Exact pass-through of the scRGB image (Encoding Auto from the FP16 swapchain).
 CHECK(images.Unchanged(image.Get(),D3D12_RESOURCE_STATE_PRESENT));
 {
  const auto copiedDepth=Texels(gpu,static_cast<ID3D12Resource*>(lastDepth),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,4);
  bool depthSame=true;for(size_t i=0;i<copiedDepth.size();i+=4){float d=0;memcpy(&d,&copiedDepth[i],4);depthSame&=d==0.25f;}
  CHECK(copiedDepth.size()==64*64*4&&depthSame);
  const auto copiedMotion=Texels(gpu,static_cast<ID3D12Resource*>(lastMotion),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,4);
  bool motionSame=copiedMotion.size()==64*64*4;
  for(unsigned y=0;y<64 && motionSame;++y)for(unsigned x=0;x<64;++x){unsigned char want[4]{};motionAt(x,y,want);motionSame&=!memcmp(want,&copiedMotion[(size_t(y)*64+x)*4],4);}
  CHECK(motionSame);
 }
 // A second present without a new DLSS evaluation still uses that frame's copy.
 present(target);CHECK(evaluates==2&&lastReset==0&&lastMotionWidth==64);
 // 50 %: a replacement is created beside the live feature, which processes this
 // present; the next present switches to it with fresh history, and the old one
 // goes once its list executed. Down and back up is still exact.
 settings.scale=50;Configure(settings);
 evaluate();present(target);CHECK(creates==2&&releases==0&&lastWidth==32&&evaluates==3&&lastReset==0);
 evaluate();present(target);CHECK(evaluates==4&&lastReset==1&&releases==0);
 evaluate();present(target);CHECK(evaluates==5&&lastReset==0&&releases==1);
 CHECK(images.Unchanged(image.Get(),D3D12_RESOURCE_STATE_PRESENT));
 // Never: neutral inputs (zero motion and depth at the network extent) although DLSS ran.
 settings.resources=static_cast<uint32_t>(Resources::eNever);Configure(settings);
 evaluate();present(target);CHECK(evaluates==6&&lastReset==1&&lastDepthWidth==32&&lastMotionWidth==32&&lastMotionScale==1.0f);
 {
  const auto zeroDepth=Texels(gpu,static_cast<ID3D12Resource*>(lastDepth),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,4);
  const auto zeroMotion=Texels(gpu,static_cast<ID3D12Resource*>(lastMotion),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,4);
  bool zero=!zeroDepth.empty()&&!zeroMotion.empty();for(auto b:zeroDepth)zero&=b==0;for(auto b:zeroMotion)zero&=b==0;CHECK(zero);
 }
 CHECK(images.Unchanged(image.Get(),D3D12_RESOURCE_STATE_PRESENT));
 // Required without a current DLSS evaluation: the image is left alone and the
 // HUD-less tag is no longer withheld.
 settings.resources=static_cast<uint32_t>(Resources::eRequired);Configure(settings);
 present(target);present(target);CHECK(evaluates==6&&Read().state==State::eMissingInputs&&!BlockHudless());
 evaluate();present(target);CHECK(evaluates==7&&lastReset==1&&lastMotionWidth==64&&BlockHudless());
 // Block Hudless off: processed, but the tag passes.
 settings.blockHudless=false;Configure(settings);CHECK(!BlockHudless());
 evaluate();present(target);CHECK(evaluates==8&&!BlockHudless());
 // An 8-bit swapchain (sRGB, Encoding Auto) of another layer: a new image copy, exact.
 auto image8=gpu.Texture(DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
 const auto pixel8=[](unsigned x,unsigned y,unsigned char* out){out[0]=static_cast<unsigned char>(x*4);out[1]=static_cast<unsigned char>(y*4);out[2]=static_cast<unsigned char>(255-x);out[3]=200;};
 FillTexels(gpu,image8.Get(),4,D3D12_RESOURCE_STATE_PRESENT,pixel8);
 PresentTarget target8{gpu.queue.Get(),image8.Get(),0/*DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709*/,0x6000};
 evaluate();present(target8);CHECK(evaluates==9&&lastReset==1&&creates==2);
 {
  const auto texels=Texels(gpu,image8.Get(),D3D12_RESOURCE_STATE_PRESENT,4);bool same=texels.size()==64*64*4;
  for(unsigned y=0;y<64 && same;++y)for(unsigned x=0;x<64;++x){unsigned char want[4]{};pixel8(x,y,want);
   if(memcmp(want,&texels[(size_t(y)*64+x)*4],4)){std::fprintf(stderr,"8-bit texel %u,%u\n",x,y);same=false;break;}}
  CHECK(same);
 }
 // A network that changes the image: the change reaches the swapchain image.
 auto changedNetwork=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 images.Fill(changedNetwork.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);providerSource=changedNetwork.Get();
 evaluate();present(target8);providerSource=nullptr;CHECK(evaluates==10);
 {
  const auto texels=Texels(gpu,image8.Get(),D3D12_RESOURCE_STATE_PRESENT,4);unsigned changed=0;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){unsigned char want[4]{};pixel8(x,y,want);
   changed+=memcmp(want,&texels[(size_t(y)*64+x)*4],3)!=0;CHECK(want[3]==texels[(size_t(y)*64+x)*4+3]);}
  CHECK(changed>64*64/2);
 }
 // Compare, Split view at 50 %: the original left of column 32, a light grey
 // divider on columns 31-32 (dark edges on 30 and 33), the processed image to
 // its right; the same feature, no recreation.
 {
  Settings split=settings;split.compare=true;split.compareSplit=50.0f;Configure(split);
  // A fresh frame: the image still holds the previous present's processed output.
  Barrier(gpu.cmd.Get(),image8.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
  FillTexels(gpu,image8.Get(),4,D3D12_RESOURCE_STATE_PRESENT,pixel8);
  providerSource=changedNetwork.Get();evaluate();present(target8);providerSource=nullptr;CHECK(evaluates==11&&creates==2);
  const auto texels=Texels(gpu,image8.Get(),D3D12_RESOURCE_STATE_PRESENT,4);unsigned left=0,right=0;bool divider=true;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
   unsigned char want[4]{};pixel8(x,y,want);const unsigned char* got=&texels[(size_t(y)*64+x)*4];
   if(x<30)left+=memcmp(want,got,3)==0;
   else if(x>33)right+=memcmp(want,got,3)!=0;
   else if(x==31||x==32)divider&=got[0]==255&&got[1]==255&&got[2]==255;
   else divider&=got[0]<8&&got[1]<8&&got[2]<8; // black edges on 30 and 33
   CHECK(want[3]==got[3]);
  }
  if(left!=30*64 || right<=30*64/2 || !divider)std::fprintf(stderr,"split: left=%u right=%u divider=%d\n",left,right,divider?1:0);
  CHECK(left==30*64&&right>30*64/2&&divider);
  // The overlay's "DLSS 5 Off / On" pills read the applied split from this evaluation.
  const auto view=LastCompare();CHECK(view.split==0.5f&&!view.held&&view.tick!=0);
  Configure(settings);
 }
 // A secondary view (DLSS output 32x32) evaluated after the main one: the copy
 // whose output matches the image is used.
 Params secondary=game;secondary.Set("Width",32u);secondary.Set("Height",32u);secondary.Set("OutWidth",32u);secondary.Set("OutHeight",32u);
 const auto* handle2=reinterpret_cast<const NVSDK_NGX_Handle*>(0x300);GameCreated(1,handle2,ReadCreate(&secondary));
 AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);
 AfterEvaluate(1,api,gpu.cmd.Get(),handle2,&secondary,NVSDK_NGX_Result_Success);gpu.Submit();
 present(target8);CHECK(evaluates==12&&lastMotionWidth==64&&lastDepthWidth==64);
 BeforeRelease(1,handle2);AfterRelease(1,handle2,NVSDK_NGX_Result_Success);
 // RGBA16F motion vectors (Cyberpunk 2077): captured as RG16F, value for value.
 {
  auto motion4=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
  const auto motion4At=[](unsigned x,unsigned y,unsigned char* out){
   const uint16_t v[4]{Half(x/16.0f),Half(-1.0f-y/16.0f),Half(0.0f),Half(1.0f)};memcpy(out,v,8);};
  FillTexels(gpu,motion4.Get(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,motion4At);
  game.Set("MotionVectors",motion4.Get());const unsigned beforeRgba=evaluates;
  evaluate();present(target8);CHECK(evaluates==beforeRgba+1&&lastMotion&&lastMotion!=motion4.Get()&&lastMotionWidth==64);
  const auto copied=Texels(gpu,static_cast<ID3D12Resource*>(lastMotion),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,4);
  bool same=copied.size()==64*64*4;
  for(unsigned y=0;y<64 && same;++y)for(unsigned x=0;x<64;++x){unsigned char want[4]{};motionAt(x,y,want);same&=!memcmp(want,&copied[(size_t(y)*64+x)*4],4);}
  CHECK(same);
  game.Set("MotionVectors",motion.Get());
 }
 // A wrapper such as ReShade presents through its own queue and device objects
 // while NGX holds the device underneath: the list goes to the queue underneath
 // (never through the wrapper); without the unwrap contract it is refused.
 {
  WrapperDevice wrapperDevice,streamlineDevice(gpu.device.Get());
  WrapperQueue wrapped(gpu.queue.Get(),&wrapperDevice,true),opaque(gpu.queue.Get(),&wrapperDevice,false);
  WrapperQueue streamline(gpu.queue.Get(),&streamlineDevice,true,kStreamlineBase);
  const unsigned beforeWrapped=evaluates;
  evaluate();present(PresentTarget{&wrapped,image8.Get(),0,0x6000});
  CHECK(evaluates==beforeWrapped+1&&wrapped.executes==0&&Read().state==State::eRunning);
  evaluate();present(PresentTarget{&opaque,image8.Get(),0,0x6000});CHECK(evaluates==beforeWrapped+1&&opaque.executes==0);
  evaluate();present(target8);CHECK(evaluates==beforeWrapped+2);
  // Streamline's proxy queue (its base-interface IID) over the device NGX holds.
  evaluate();present(PresentTarget{&streamline,image8.Get(),0,0x6000});
  CHECK(evaluates==beforeWrapped+3&&streamline.executes==0&&Read().state==State::eRunning);
  CHECK(wrapped.refs==1&&opaque.refs==1&&streamline.refs==1&&wrapperDevice.refs==1&&streamlineDevice.refs==1);
 }
 // hudlatch.1: Block Hudless ends with its chain. Draining another owner leaves it; draining the stage's owner (a
 // resize, or the chain moved to XeFG, whose layer has no Present stage) gives frame generation the HUD-less tag
 // back until a present is processed again.
 settings.blockHudless=true;Configure(settings);
 evaluate();present(target8);CHECK(BlockHudless());
 CHECK(DrainPresent(0x5000)&&BlockHudless()&&Logged(L"its swapchain was released or resized")==0);
 CHECK(DrainPresent(0x6000)&&!BlockHudless()&&Logged(L"its swapchain was released or resized")==1);
 CHECK(DrainPresent(0x6000)&&!BlockHudless()&&Logged(L"its swapchain was released or resized")==1);
 evaluate();present(target8);CHECK(BlockHudless());
 // The swapchain's work drains before its buffers go; another owner is untouched.
 CHECK(DrainPresent(0x6000)&&DrainPresent(0x5000));
 // Another Hook Method releases the Present feature once its work completed.
 settings.stage=static_cast<uint32_t>(Stage::eUpscaled);Configure(settings);CHECK(!PresentStageSelected()&&!BlockHudless());
 testing::Collect();CHECK(releases==creates&&creates==2);
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(allocates==destroys);
 Configure(Settings{});CHECK(Read().state==State::eOff&&Read().live==0);
}
// A fence of another device (`device`), wrapping nothing.
struct OtherFence:ID3D12Fence{
 IUnknown* device;ULONG refs=1;
 explicit OtherFence(IUnknown* d):device(d){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override{
  if(riid==__uuidof(IUnknown)||riid==__uuidof(ID3D12Object)||riid==__uuidof(ID3D12DeviceChild)||riid==__uuidof(ID3D12Pageable)
     ||riid==__uuidof(ID3D12Fence)){AddRef();*out=this;return S_OK;}
  *out=nullptr;return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{return --refs;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** out) override{device->AddRef();*out=device;return S_OK;}
 UINT64 STDMETHODCALLTYPE GetCompletedValue() override{return 0;}
 HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64,HANDLE) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Signal(UINT64) override{return E_NOTIMPL;}
};
// Makes the vtable page of `object` executable for its lifetime, as a vtable
// hook may leave it: the overlay's unwrap contract then refuses the object.
struct ExecutableVtable{
 void* table;DWORD previous=0;bool changed=false;
 explicit ExecutableVtable(IUnknown* object):table(*reinterpret_cast<void**>(object)){
  changed=VirtualProtect(table,3*sizeof(void*),PAGE_EXECUTE_READWRITE,&previous)!=FALSE;
 }
 ~ExecutableVtable(){DWORD ignored=0;if(changed)VirtualProtect(table,3*sizeof(void*),previous,&ignored);}
};
// Monster Hunter Wilds (Streamline 2.7.32): the presentation queue is
// Streamline's proxy, which answers Streamline's base-interface IID with the
// native queue and whose GetDevice gives Streamline's proxy device, over the
// native device NGX holds. The native NGX create is refused as on Ada
// (0xBAD0000B, the direct provider is next) and the list holding that attempt
// is the first submission. Our list and fences were created on that native
// device, so they go to the native queue as they are, also when objects of the
// native device do not pass the unwrap contract.
void PresentStreamline(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto image=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
 auto depth=gpu.Texture(DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 Images images(gpu,image.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 images.Fill(image.Get(),D3D12_RESOURCE_STATE_PRESENT);
 FillTexels(gpu,depth.Get(),4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,[](unsigned,unsigned,unsigned char* out){const float d=0.5f;memcpy(out,&d,4);});
 FillTexels(gpu,motion.Get(),4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,[](unsigned,unsigned,unsigned char* out){memset(out,0,4);});
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x700);
 GameCreated(1,handle,ReadCreate(&game));Configure(On(Stage::ePresent));
 WrapperDevice proxyDevice(gpu.device.Get());
 WrapperQueue proxy(gpu.queue.Get(),&proxyDevice,true,kStreamlineBase);
 const PresentTarget target{&proxy,image.Get(),1,0x7000};
 failCreate=true;createFailure=static_cast<NVSDK_NGX_Result>(0xBAD0000B);
 testing::SetProviderPath(L"C:\\RTXMFG-test-missing\\nvngx_dlssnr.dll");
 {
  ExecutableVtable hooked(gpu.cmd.Get());
  CHECK(hooked.changed&&!single_overlay::native::PinInterface(gpu.cmd.Get(),2));
  // The refusal names the object, the unwrap result, the vtable's owner and both devices.
  const std::wstring refusal=testing::NativeRefusal(gpu.cmd.Get(),gpu.device.Get());
  CHECK(refusal.rfind(L"object ",0)==0&&refusal.find(L"unwrap=refused (vtable not pinnable)")!=std::wstring::npos);
  CHECK(refusal.find(L"D3D12Core.dll+0x")!=std::wstring::npos&&refusal.find(L"object device=")!=std::wstring::npos);
  CHECK(refusal.find(L"queue device=")!=std::wstring::npos);
  AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);gpu.Submit();
  // The first present attempts the native create (refused) on our list, which reaches the native queue.
  Present(target);gpu.Submit();
  CHECK(creates==1&&proxy.executes==0&&Read().state==State::eWaitingForDlss&&Read().failures==0);
  CHECK(Logged(L"queue wrapped")==1&&Logged(L"NGX device=")==1&&Logged(L"submitting ours on the native queue")==1);
  CHECK(Logged(L"native NGX create returned 0xBAD0000B; the next evaluation uses the direct provider")==1);
  // The next present takes the direct route (only once that submission completed), again on the native queue.
  AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);gpu.Submit();
  Present(target);gpu.Submit();
  CHECK(proxy.executes==0&&Logged(L"direct provider open failed")==1&&Read().state==State::eUnsupported);
  CHECK(Logged(L"Present stage refused")==0&&DrainPresent(0x7000));
 }
 CHECK(single_overlay::native::PinInterface(gpu.cmd.Get(),2));
 // Accepted objects, and a fence of another device named with both devices.
 CHECK(testing::NativeRefusal(gpu.fence.Get(),gpu.device.Get()).empty()&&testing::NativeRefusal(gpu.cmd.Get(),gpu.device.Get()).empty());
 {
  // (D3D12 gives every device of one adapter the same object, so the other device is a stand-in.)
  WrapperDevice otherDevice;OtherFence other{&otherDevice};
  const std::wstring refusal=testing::NativeRefusal(&other,gpu.device.Get());
  wchar_t devices[100]{};swprintf_s(devices,L"object device=%p queue device=%p",static_cast<void*>(&otherDevice),static_cast<void*>(gpu.device.Get()));
  CHECK(refusal.find(L"unwrap=ok")!=std::wstring::npos&&refusal.find(devices)!=std::wstring::npos);
  CHECK(other.refs==1&&otherDevice.refs==1);
 }
 CHECK(proxy.refs==1&&proxyDevice.refs==1);
 Configure(Settings{});testing::Collect();BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);
 CHECK(allocates==destroys);
}
// Every slot of the 8-entry table taken: the next DLSS feature is not tracked,
// logged once until a slot frees and the table fills again.
// Witcher 3: our own hair runtime holds the queue table's ExecuteCommandLists
// slot (witcher_dots). The layer here is code of this module, as that hook is
// of ours. Without its forward DLSS 5 refuses our code; with it DLSS 5 hooks
// the runtime method behind the layer and its fences follow the game's
// submissions through it.
using QueueExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
QueueExecuteFn layerRuntime=nullptr;unsigned layerCalls=0;
void STDMETHODCALLTYPE LayerExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){++layerCalls;layerRuntime(queue,count,lists);}
void* LayerForward(ID3D12CommandQueue* queue) noexcept {
 return (*reinterpret_cast<void***>(queue))[10]==reinterpret_cast<void*>(&LayerExecute)?reinterpret_cast<void*>(layerRuntime):nullptr;
}
void SetQueueSlot(void** slot,void* value){
 DWORD protection=0,ignored=0;CHECK(VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection));
 *slot=value;CHECK(VirtualProtect(slot,sizeof(void*),protection,&ignored));
}
void OwnQueueLayer(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 void** const slot=*reinterpret_cast<void***>(gpu.queue.Get())+10;
 layerRuntime=reinterpret_cast<QueueExecuteFn>(*slot);SetQueueSlot(slot,reinterpret_cast<void*>(&LayerExecute));
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x100);
 GameCreated(1,handle,keys);Configure(On());
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);};
 run();CHECK(creates==0&&Read().state==State::eUnsupported);
 // GitHub issue 160: the refusal names the part that failed (here the entry is in this executable, as our own code).
 CHECK(Logged(L"DLSS 5 setup refused: step=queue submission hook (own module) owner=")==1&&Logged(L"DLSS 5 queue submission hook: owner=")==0);
 SetQueueForward(&LayerForward);
 Settings retry=On();retry.intensity=0.71f;Configure(retry);run();CHECK(creates==1&&evaluates==0);
 CHECK(Logged(L"DLSS 5 queue submission hook: owner=")==1&&Logged(L" (behind our own table hook)")==1);
 const unsigned before=layerCalls;gpu.Submit();CHECK(layerCalls==before+1);
 run();CHECK(evaluates==1);gpu.Submit();
 run();CHECK(evaluates==2&&creates==1);gpu.Submit();
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==1&&allocates==destroys);
 SetQueueSlot(slot,reinterpret_cast<void*>(layerRuntime));
}
// autoscale.1: a replacement that cannot be created beside the live feature
// leaves that feature evaluating; the slot then replaces in place (once the work
// completed, nothing evaluated until the new create executed).
void ReplaceFailure(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x400);
 GameCreated(1,handle,keys);Configure(On());
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);};
 run();CHECK(creates==1&&evaluates==0);gpu.Submit();
 run();CHECK(evaluates==1);gpu.Submit();
 failCreate=true;Settings model=On();model.model=1;Configure(model);
 run();CHECK(creates==2&&evaluates==2&&lastStyle==0&&Read().state==State::eRunning);gpu.Submit();
 CHECK(Logged(L"DLSS 5 could not create a replacement beside the live feature")==1);
 failCreate=false;run();CHECK(creates==3&&releases==1&&evaluates==2);gpu.Submit();
 run();CHECK(evaluates==3&&lastStyle==1&&lastReset==1);gpu.Submit();
 // In place from now on: the next change also leaves one evaluation out.
 Settings scaled=model;scaled.scale=50;Configure(scaled);run();CHECK(creates==4&&releases==2&&evaluates==3);gpu.Submit();
 run();CHECK(evaluates==4&&lastWidth==32);gpu.Submit();
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==3&&allocates==destroys);
}
// nrscale.1: a replacement with no room beside the live feature in the video memory
// budget replaces it in place (one evaluation left out, said once); with room again
// it goes beside. Each create takes 300 MB; the create log gives the memory.
void ReplaceMemory(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x600);
 constexpr uint64_t MB=1ull<<20;
 memoryUsage=1000*MB;memoryBudget=1500*MB;memoryPerCreate=300*MB;ReportMemory();
 GameCreated(1,handle,keys);Configure(On());
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);};
 run();CHECK(creates==1&&evaluates==0);gpu.Submit();
 CHECK(Logged(L"; video memory 1000 -> 1300 MB of a 1500 MB budget")==1);
 run();CHECK(evaluates==1);gpu.Submit();
 Settings model=On();model.model=1;Configure(model);
 run();CHECK(creates==2&&releases==1&&evaluates==1);gpu.Submit();
 CHECK(Logged(L"DLSS 5 replaces its feature in place: 1300 MB of video memory in use and about 300 MB for a create at 64x64 exceed the 1500 MB budget")==1);
 run();CHECK(evaluates==2&&lastStyle==1&&lastReset==1);gpu.Submit();
 CHECK(Logged(L"replaced without a gap")==0);
 // Room again: the replacement goes beside, the live feature evaluating until it is ready.
 memoryBudget=4000*MB;ReportMemory();
 Settings scaled=model;scaled.scale=50;Configure(scaled);
 run();CHECK(creates==3&&releases==1&&evaluates==3&&lastStyle==1);gpu.Submit();
 run();CHECK(evaluates==4&&lastWidth==32);gpu.Submit();
 CHECK(Logged(L"replaced without a gap")==1&&Logged(L"replaces its feature in place")==1);
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==3&&allocates==destroys);
 memoryBudget=0;testing::SetVideoMemory(0,0);
}
// autorect.1: Resolution Scale Auto creates its feature once, at the slider's level, and an Auto step moves the
// network's rect within it: no create, the provider's Color and Output subrects at the level's extent, the history
// restarted once, the status at the level. Half rate keeps its look model textures across steps. The slider stays
// Auto's ceiling: lowering it is a new, smaller feature beside the live one.
void AutoCapacity(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());
 images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 images.Fill(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x700);
 Settings automatic=On(Stage::eAuto);automatic.scaleAuto=true;automatic.scale=100;automatic.adaptiveExposure=false;
 testing::SetAutoLevel(0);
 GameCreated(1,handle,keys);Configure(automatic);
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);};
 const auto rect=[](int width,int height){
  return lastColorRectWidth==width&&lastColorRectHeight==height&&lastOutputRectWidth==width&&lastOutputRectHeight==height;
 };
 run();CHECK(creates==1&&evaluates==0&&lastWidth==64);gpu.Submit();
 run();CHECK(evaluates==1&&rect(64,64)&&lastReset==1);gpu.Submit();
 run();CHECK(evaluates==2&&lastReset==0);gpu.Submit();
 testing::SetAutoLevel(2);
 run();CHECK(creates==1&&evaluates==3&&rect(48,48)&&lastReset==1);gpu.Submit();
 run();CHECK(evaluates==4&&rect(48,48)&&lastReset==0);gpu.Submit();
 {const auto view=Read();
  CHECK(view.scale==75.0f&&view.extentWidth==48&&view.extentHeight==48&&!view.pending&&view.live==1&&view.creates==1);}
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 testing::SetAutoLevel(1);
 run();CHECK(creates==1&&rect(54,54)&&lastReset==1);gpu.Submit();
 testing::SetAutoLevel(0);
 run();CHECK(creates==1&&rect(64,64)&&lastReset==1&&Read().scale==100.0f&&Read().extentWidth==64);gpu.Submit();
 CHECK(images.Unchanged(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
 CHECK(Logged(L"DLSS 5 feature created")==1&&Logged(L"replaced without a gap")==0&&releases==0);
 // Half rate across a step: its textures hold every level's look model grid (64x64: 4x4 cells, 48x48: 3x3), so
 // the step keeps them; the step runs the network on the new rect, and Carry follows on it.
 automatic.boost=true;Configure(automatic);
 const unsigned beforeBoost=evaluates;
 run();CHECK(evaluates==beforeBoost+1);gpu.Submit();
 run();CHECK(evaluates==beforeBoost+1);gpu.Submit();
 testing::SetAutoLevel(2);
 run();CHECK(evaluates==beforeBoost+2&&rect(48,48)&&lastReset==1);gpu.Submit();
 run();CHECK(evaluates==beforeBoost+2);gpu.Submit();
 CHECK(Logged(L"DLSS 5 Boost: the network runs on every other evaluation")==1&&Logged(L"DLSS 5 Boost unavailable")==0&&creates==1);
 // autofps.1: a Dynamic frame generation target reaches Auto's budget (the status reports the one StepScale steps
 // with): 120 FPS with 2 to 4 frames, as DLSS-G publishes it; once stopped, the 30 % rule again; XeMFG's report as
 // well (dlss5-auto-policy checks the budget itself, e.g. none for a target out of reach).
 {
  frame_gen_target::Publish(frame_gen_target::Source::eDlssg,true,120.0f,2,4);
  run();gpu.Submit();run();gpu.Submit();
  const auto followed=Read();
  CHECK(followed.budgetMs==auto_mode::DynamicBudgetMs(followed.frameMs,followed.gpuMs,120.0f,2,4));
  CHECK(Logged(L"DLSS 5 Auto follows Dynamic frame generation's target: 120 FPS with 2 to 4 frames per rendered frame")==1);
  frame_gen_target::Publish(frame_gen_target::Source::eDlssg,false,0.0f,0,0);
  const auto plain=Read();
  CHECK(plain.budgetMs==auto_mode::BudgetMs(plain.frameMs,plain.gpuMs));
  frame_gen_target::Publish(frame_gen_target::Source::eXemfg,true,90.0f,1,3);
  const auto xemfg=Read();
  CHECK(xemfg.budgetMs==auto_mode::DynamicBudgetMs(xemfg.frameMs,xemfg.gpuMs,90.0f,1,3));
  frame_gen_target::Publish(frame_gen_target::Source::eXemfg,false,0.0f,0,0);
  run();gpu.Submit();
  CHECK(Logged(L"DLSS 5 Auto no longer follows a Dynamic frame generation target")==1&&creates==1);
 }
 // The slider at 75 %: a 48x48 feature beside the live one, which runs at 75 % of its 64x64 until then.
 testing::SetAutoLevel(SIZE_MAX);
 automatic.boost=false;automatic.scale=75;Configure(automatic);
 const unsigned beforeSlider=evaluates;
 run();CHECK(creates==2&&lastWidth==48&&evaluates==beforeSlider+1&&rect(48,48));gpu.Submit();
 run();CHECK(evaluates==beforeSlider+2&&rect(48,48)&&lastReset==1);gpu.Submit();
 CHECK(Logged(L"replaced without a gap")==1&&!Read().pending&&Read().scale==75.0f);
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);testing::Collect();
 CHECK(releases==creates&&allocates==destroys);
}
// hdrbridge.1: the HDR bridge keeps hue. Unbounded color (an IsHDR DLSS feature
// after upscaling) above the knee goes to the network scaled along its ray, and the
// network's change comes back by the texel's own ratio k, one factor for R, G and B.
// With a network that adds an even step to its signal, the decoded texel is the
// stage texel plus k times the change the network made to the SDR color it saw, so
// its hue moves as that SDR color's does. A per-channel shoulder turns the same
// step into a red cast on lit skin (R/G 1.47 -> 1.60 at +0.01).
double SrgbDecode(double c){c=std::clamp(c,0.0,1.0);return c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4);}
double OkHue(const double c[3],double* chroma){ // OkLab hue in degrees, linear BT.709
 const double m[3][3]{{0.4122214708,0.5363325363,0.0514459929},{0.2119034982,0.6806995451,0.1073969566},{0.0883024619,0.2817188376,0.6299787005}};
 double lms[3]{};for(int i=0;i<3;++i)lms[i]=std::cbrt(m[i][0]*c[0]+m[i][1]*c[1]+m[i][2]*c[2]);
 const double a=1.9779984951*lms[0]-2.4285922050*lms[1]+0.4505937099*lms[2],b=0.0259040371*lms[0]+0.7827717662*lms[1]-0.8086757660*lms[2];
 *chroma=std::hypot(a,b);return std::atan2(b,a)*57.29577951308232;
}
uint16_t HalfNearest(double value){ // round to nearest even; normal halves only
 const float f=static_cast<float>(value);uint32_t bits=0;memcpy(&bits,&f,4);
 uint32_t h=((bits>>16)&0x8000)|(uint32_t(int((bits>>23)&0xFF)-127+15)<<10)|((bits>>13)&0x3FF);
 const uint32_t rest=bits&0x1FFF;if(rest>0x1000||(rest==0x1000&&(h&1)))++h;
 return static_cast<uint16_t>(h);
}
void HdrBridge(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 // Bands of 16 rows, 1 = diffuse white: lit skin, bright skin and warm light above the knee (0.8), mid skin below it.
 const float bands[4][3]{{1.10f,0.75f,0.55f},{1.40f,0.95f,0.70f},{2.0f,1.2f,0.4f},{0.60f,0.42f,0.31f}};
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto color=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 const auto stage=[&](unsigned,unsigned y,unsigned char* out){
  const uint16_t h[4]{Half(bands[y/16][0]),Half(bands[y/16][1]),Half(bands[y/16][2]),0x3C00};memcpy(out,h,8);
 };
 FillTexels(gpu,output.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,stage);
 FillTexels(gpu,color.Get(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,stage);
 Params game;game.Set("Output",output.Get());game.Set("Color",color.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x500);
 GameCreated(1,handle,keys);Settings bridge=On();bridge.adaptiveExposure=false;Configure(bridge);
 auto run=[&]{AfterEvaluate(1,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success);gpu.Submit();};
 run();CHECK(creates==1&&evaluates==0);
 // A pass-through network leaves the HDR image exact; the network input it saw is read back.
 run();CHECK(evaluates==1);
 const auto exact=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
 for(unsigned y=0;y<64;++y){
  uint16_t h[4]{};memcpy(h,&exact[(size_t(y)*64+5)*8],8);
  CHECK(h[0]==Half(bands[y/16][0])&&h[1]==Half(bands[y/16][1])&&h[2]==Half(bands[y/16][2]));
 }
 auto* input=static_cast<ID3D12Resource*>(lastColor);CHECK(input);
 const auto inputDesc=input->GetDesc();CHECK(inputDesc.Width==64&&inputDesc.Height==64&&inputDesc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT);
 const auto seen=Texels(gpu,input,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
 const auto ratio=[](double peak){return peak>0.8?peak/(0.8+0.2*(1-std::exp(-(peak-0.8)/0.2))):1.0;};
 for(unsigned band=0;band<4;++band){
  // The network sees the texel along its ray: the same channel ratios, the brightest channel on the shoulder.
  uint16_t h[4]{};memcpy(h,&seen[((size_t(band)*16+8)*64+32)*8],8);
  double base[3]{},sdr[3]{},peak=0;
  for(int c=0;c<3;++c){base[c]=FromHalfBits(Half(bands[band][c]));sdr[c]=SrgbDecode(FromHalfBits(h[c]));peak=std::max(peak,base[c]);}
  const double k=ratio(peak);
  for(int c=0;c<3;++c)CHECK(std::abs(sdr[c]*k-base[c])<=2e-3*peak);
 }
 for(const double step:{0.01,-0.01,0.03}){
  // The network's output: its input plus an even step on all three channels of the sRGB signal.
  auto network=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<unsigned char> changed(seen.size());
  for(size_t n=0;n<64*64;++n){
   uint16_t h[4]{};memcpy(h,&seen[n*8],8);
   for(int c=0;c<3;++c)h[c]=HalfNearest(std::clamp(FromHalfBits(h[c])+step,1e-3,1.0));
   memcpy(&changed[n*8],h,8);
  }
  FillTexels(gpu,network.Get(),8,D3D12_RESOURCE_STATE_COPY_SOURCE,[&](unsigned x,unsigned y,unsigned char* out){memcpy(out,&changed[(size_t(y)*64+x)*8],8);});
  Barrier(gpu.cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
  FillTexels(gpu,output.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,stage);
  providerSource=network.Get();const unsigned before=evaluates;run();providerSource=nullptr;CHECK(evaluates==before+1);
  const auto result=Texels(gpu,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
  for(unsigned band=0;band<4;++band){
   const size_t at=(size_t(band)*16+8)*64+32;
   uint16_t h[4]{},in[4]{},out[4]{};memcpy(h,&result[at*8],8);memcpy(in,&seen[at*8],8);memcpy(out,&changed[at*8],8);
   double base[3]{},got[3]{},sdr[3]{},sdrAfter[3]{},peak=0;
   for(int c=0;c<3;++c){
    base[c]=FromHalfBits(Half(bands[band][c]));got[c]=FromHalfBits(h[c]);peak=std::max(peak,base[c]);
    sdr[c]=SrgbDecode(FromHalfBits(in[c]));sdrAfter[c]=SrgbDecode(FromHalfBits(out[c]));
   }
   const double k=ratio(peak);
   // Every channel: the stage texel plus k times the network's change (RGBA16F output rounding aside).
   for(int c=0;c<3;++c)CHECK(std::abs(got[c]-(base[c]+k*(sdrAfter[c]-sdr[c])))<=2e-3*peak);
   double c0=0,c1=0,s0=0,s1=0;
   const double dh=std::remainder(OkHue(got,&c1)-OkHue(base,&c0),360.0),dhs=std::remainder(OkHue(sdrAfter,&s1)-OkHue(sdr,&s0),360.0);
   std::printf("hdr bridge step %+.2f band %u k %.3f: R/G %.3f -> %.3f, hue %+.2f (SDR %+.2f), chroma x%.3f (SDR x%.3f)\n",step,band,k,
    base[0]/base[1],got[0]/got[1],dh,dhs,c1/c0,s1/s0);
   CHECK(std::abs(dh-dhs)<=0.25&&std::abs(c1/c0-s1/s0)<=0.01);
   // Lit skin keeps its hue and R/G (no red cast).
   if(band==0&&std::abs(step)<0.02)CHECK(std::abs(dh)<=0.5&&std::abs((got[0]/got[1])/(base[0]/base[1])-1)<=0.01);
  }
 }
 BeforeRelease(1,handle);AfterRelease(1,handle,NVSDK_NGX_Result_Success);CHECK(releases==1&&allocates==destroys);
}
// dx11nr.1: on the DirectX 11 route, native NGX's 0xBAD0000C (driver out of date) fails closed: Driver update
// required, the direct provider is never tried (not after a settings change either). Route 1 keeps its fallback.
void D3D11OutOfDate(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 Params game;game.Set("Output",output.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 // A provider beside the game: without the rule, the next evaluation would try it.
 testing::SetProviderPath(L"C:\\RTXMFG-test-missing\\nvngx_dlssnr.dll");
 failCreate=true;createFailure=NVSDK_NGX_Result_FAIL_OutOfDate;
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x100);
 GameCreated(kD3D11Route,handle,keys);Configure(On());
 auto run=[&](const NVSDK_NGX_Handle* which,size_t route){AfterEvaluate(route,api,gpu.cmd.Get(),which,&game,NVSDK_NGX_Result_Success);gpu.Submit();};
 run(handle,kD3D11Route);
 CHECK(creates==1&&Read().state==State::eDriverUpdate&&Read().result==0xBAD0000Cu);
 // The bridge asks before any cross-API work: nothing to do for these settings.
 CHECK(!shared::CanEvaluate(kD3D11Route,handle,&game,Stage::eUpscaled)&&Read().state==State::eDriverUpdate);
 CHECK(Logged(L"the driver is out of date, so the DirectX 11 bridge does not use the direct provider")==1);
 CHECK(Logged(L"DLSS 5 direct provider")==0);
 // A settings change retries the slot: refused again, without a create or the direct provider.
 Settings retry=On();retry.intensity=0.71f;Configure(retry);
 CHECK(shared::CanEvaluate(kD3D11Route,handle,&game,Stage::eUpscaled));
 run(handle,kD3D11Route);run(handle,kD3D11Route);
 CHECK(creates==1&&Read().state==State::eDriverUpdate&&Logged(L"DLSS 5 direct provider")==0);
 CHECK(!shared::CanEvaluate(kD3D11Route,handle,&game,Stage::eUpscaled));
 BeforeRelease(kD3D11Route,handle);AfterRelease(kD3D11Route,handle,NVSDK_NGX_Result_Success);testing::Collect();
 CHECK(allocates==destroys);
 // The same result on a D3D12 route keeps dlss5.cpp's fallback in this build.
 const auto* other=reinterpret_cast<const NVSDK_NGX_Handle*>(0x200);
 GameCreated(1,other,keys);Configure(On());run(other,1);
 CHECK(creates==2&&Logged(L"the next evaluation uses the direct provider")==1);
 BeforeRelease(1,other);AfterRelease(1,other,NVSDK_NGX_Result_Success);testing::Collect();CHECK(allocates==destroys);
}
// dx11nr.1: the bridge's stage calls carry the settings revision it resolved its stage for, so a switch to Present in
// between records nothing on its list; CanEvaluate accepts a healthy slot and refuses missing inputs; ReleaseRoute
// releases the route's feature (Present on DirectX 11) once its work completed.
void D3D11Stage(){
 using namespace dlss5;Gpu gpu;testing::AllowSoftwareDevice();
 auto output=gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
 auto depth=gpu.Texture(DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto motion=gpu.Texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Images images(gpu,output.Get());images.Fill(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 Params game;game.Set("Output",output.Get());game.Set("Depth",depth.Get());game.Set("MotionVectors",motion.Get());
 game.Set("Width",64u);game.Set("Height",64u);game.Set("OutWidth",64u);game.Set("OutHeight",64u);game.Set("DLSS.Feature.Create.Flags",3);
 const auto keys=ReadCreate(&game);CHECK(keys.Complete());
 Api api{GetModuleHandleW(nullptr),&Create,&Evaluate,&Release,&Allocate,&Destroy};
 const auto* handle=reinterpret_cast<const NVSDK_NGX_Handle*>(0x100);
 GameCreated(kD3D11Route,handle,keys);Configure(On());
 uint64_t revision=0;shared::Wanted(revision);
 CHECK(shared::CanEvaluate(kD3D11Route,handle,&game,Stage::eUpscaled)&&shared::CanEvaluate(kD3D11Route,handle,nullptr,Stage::eUpscaled));
 CHECK(!shared::CanEvaluate(kD3D11Route,reinterpret_cast<const NVSDK_NGX_Handle*>(0x999),&game,Stage::eUpscaled));
 Params missing=game;missing.values.erase("Depth");
 CHECK(!shared::CanEvaluate(kD3D11Route,handle,&missing,Stage::eUpscaled)&&Read().state==State::eMissingInputs);
 // Present chosen after the bridge read its settings: neither stage call records.
 Configure(On(Stage::ePresent));
 AfterEvaluate(kD3D11Route,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success,Substitution{},revision);
 CHECK(BeforeEvaluate(kD3D11Route,api,gpu.cmd.Get(),handle,&game,revision).slot==SIZE_MAX);
 CHECK(creates==0&&evaluates==0);gpu.Submit();
 // Back at After upscaling, at its own revision: the feature is created, then evaluates.
 Configure(On());shared::Wanted(revision);
 auto run=[&]{AfterEvaluate(kD3D11Route,api,gpu.cmd.Get(),handle,&game,NVSDK_NGX_Result_Success,Substitution{},revision);gpu.Submit();};
 run();CHECK(creates==1);run();CHECK(evaluates==1&&Read().d3d11);
 // Present on DirectX 11: the bridge releases the route's feature as Off does.
 Configure(On(Stage::ePresent));shared::ReleaseRoute(kD3D11Route);
 CHECK(releases==1&&Read().live==0&&Logged(L"DLSS 5: a feature its Hook Method does not run on was released")==1);
 BeforeRelease(kD3D11Route,handle);AfterRelease(kD3D11Route,handle,NVSDK_NGX_Result_Success);testing::Collect();CHECK(allocates==destroys);
}
void Slots(){
 using namespace dlss5;
 dlss_sr::CreateKeys keys{};keys.present=dlss_sr::CreateKeys::eRequired|dlss_sr::CreateKeys::eCreateFlags;
 keys.width=keys.height=keys.outWidth=keys.outHeight=64;
 const auto handle=[](uintptr_t n){return reinterpret_cast<const NVSDK_NGX_Handle*>(0x1000*n);};
 for(uintptr_t n=1;n<=8;++n)GameCreated(1,handle(n),keys);
 CHECK(Logged(L"slots are in use")==0);
 GameCreated(1,handle(9),keys);GameCreated(1,handle(10),keys);
 CHECK(Logged(L"DLSS 5 inputs unavailable: all 8 DLSS feature slots are in use, so this DLSS feature is not tracked")==1);
 BeforeRelease(1,handle(1));AfterRelease(1,handle(1),NVSDK_NGX_Result_Success);
 GameCreated(1,handle(11),keys);CHECK(Logged(L"slots are in use")==1);
 GameCreated(1,handle(12),keys);CHECK(Logged(L"slots are in use")==2);
 for(uintptr_t n=2;n<=11;++n){BeforeRelease(1,handle(n));AfterRelease(1,handle(n),NVSDK_NGX_Result_Success);}
 // nrouter.1: a slot belongs to its route; OptiScaler's calls own theirs until the release begins.
 GameCreated(kOptiScalerRoute,handle(20),keys);
 CHECK(Owns(kOptiScalerRoute,handle(20)) && !Owns(1,handle(20)) && !Owns(kOptiScalerRoute,handle(21)));
 BeforeRelease(kOptiScalerRoute,handle(20));CHECK(!Owns(kOptiScalerRoute,handle(20)));
 AfterRelease(kOptiScalerRoute,handle(20),NVSDK_NGX_Result_Success);CHECK(!Owns(kOptiScalerRoute,handle(20)));
}
int main(int argc,char**argv){
 dlss5::SetLogCallback(&Capture);
 Policies();const std::string mode=argc>1?argv[1]:"";
 if(mode=="present")PresentRuntime();
 else if(mode=="present-streamline")PresentStreamline();
 else if(mode=="slots")Slots();
 else if(mode=="own-queue-layer")OwnQueueLayer();
 else if(mode=="replace-failure")ReplaceFailure();
 else if(mode=="replace-memory")ReplaceMemory();
 else if(mode=="auto-capacity")AutoCapacity();
 else if(mode=="hdr-bridge")HdrBridge();
 else if(mode=="d3d11-out-of-date")D3D11OutOfDate();
 else if(mode=="d3d11-stage")D3D11Stage();
 else Runtime(mode);
 std::printf("DLSS5 checks=%u PASS\n",checks);
}
