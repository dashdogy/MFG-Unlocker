
#include "dlss5.h"
#include "dlss5_auto.h"
#include "dlss5_d3d11_policy.h"
#include "dlss5_stage_common.h"
#include "overlay_native.h"
#include "physical_key.h"
#include <MinHook.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
using Microsoft::WRL::ComPtr;
namespace dlss5 {
namespace {
std::recursive_mutex gMutex;
// Settings are written by the control thread and read once per evaluation.
std::mutex gSettingsMutex;
Settings gWanted{};
uint64_t gRevision=1;
std::atomic<LogCallback> gLog{nullptr};
#if defined(MFG_DLSS5_TESTING)
bool gSoftwareTest=false;
std::wstring gProviderTestPath;
// nrscale.1: the video memory a case reports instead of DXGI's (testing::SetVideoMemory); a zero budget: DXGI's.
std::atomic<uint64_t> gTestUsage{0},gTestBudget{0};
// autorect.1: Resolution Scale Auto's level a case pins (testing::SetAutoLevel); SIZE_MAX: the controller's.
std::atomic<size_t> gTestAutoLevel{SIZE_MAX};
#endif
bool gForeign=false;
// The provider-ownership snapshot, taken once per process before the first native create, so a
// provider NGX mapped for our own first native attempt never counts as another loader's for a later or reset slot.
bool gProviderChecked=false,gProviderForeign=false;
uint64_t gNativeAttempts=0;
uint64_t gCreates=0,gEvaluations=0,gFailures=0;
State gState=State::eOff;
uint32_t gResult=0;
uint64_t gLastEvaluate=0;
DirectProvider gDirect;
HMODULE gDirectRuntime=nullptr;
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
ExecuteFn gExecute=nullptr;
void* gExecuteTarget=nullptr;
std::atomic<QueueForwardFn> gQueueForward{nullptr};
// One descriptor set per live recording: encode (4 SRV + 1 UAV), decode (4 SRV
// + 1 UAV) and four more tables: the Present stage's two neutral-input copies,
// or Boost's compose/copy, fit/carry, solve and hold.
constexpr uint32_t kSetDescriptors=30,kSets=8;
// A provider feature and everything sized to its processing extent. A slot
// evaluates one (its own base); a Resolution Scale or settings change creates
// the next one beside it while the live one keeps evaluating (Settle).
struct Feature : common::FeatureCore {
    // encoded: the network input; ping/pong: pass outputs.
    ComPtr<ID3D12Resource> encoded, ping, pong;
    ComPtr<ID3D12DescriptorHeap> heap;
    // Boost: boostSums and boostModel hold the last network run's look model
    // (dlss5_policy.h BoostGrid; its least-squares sums, then 3 RGBA32F texels
    // per cell and bin), boostChange what the model does not explain (fine
    // detail) with the input's luminance in alpha (extent, network signal);
    // boostMotion a Carry evaluation's motion vectors and boostTwoFrame the
    // vectors over both evaluations for the next network pass (motion size, RG32F).
    ComPtr<ID3D12Resource> boostChange,boostSums,boostModel,boostMotion,boostTwoFrame;
    BoostGrid boostGrid{};
    bool boostHeld=false,boostSkipped=false;
    // The Present stage's neutral depth and motion vectors at the extent.
    ComPtr<ID3D12Resource> neutralDepth,neutralMotion;
    bool neutralFilled=false;
    // Anything the provider or the GPU may still use.
    bool Holds() const noexcept {return passes || params || encoded || heap || boostChange || neutralDepth;}
};
// A replaced feature, released once the slot's fence passed `after` (every
// list that used it executed).
struct Retired { Feature feature; uint64_t after=0; };
// Slots stay owned across failed releases and outstanding/discarded command lists.
// Exhaustion disables new NR work rather than evicting an in-use feature.
struct Slot : Feature, common::SlotState {
    Api api{};
    ComPtr<ID3D12Device> device;
    // The Render stage's processed reconstruction input (the game color's size).
    ComPtr<ID3D12Resource> substitute;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12GraphicsCommandList> recording;
    ComPtr<ID3D12CommandQueue> lastQueue;
    Flight flight{};
    uint32_t descriptor=0;
    std::array<uint64_t,kSets> descriptorFence{};
    // The replacement being created (ready once the fence passed
    // nextReady), the features it replaced, and whether this slot replaces its
    // feature in place instead (a replacement could not be created beside it).
    Feature next;
    uint64_t nextReady=0;
    std::array<Retired,3> retired{};
    bool replaceRefused=false;
    // nrscale.1: the adapter's video memory, the latest create's share of it, and
    // whether the change under way replaces the feature in place (RoomFor).
    ComPtr<IDXGIAdapter3> memory;
    bool memoryTried=false,inPlace=false;
    uint64_t createBytes=0;
    uint32_t createWidth=0,createHeight=0;
    // vmemlog.1: the last "DLSS 5 video memory" line (tick; local and non-local bytes).
    ULONGLONG memoryLogTick=0;
    uint64_t memoryLogLocal=0,memoryLogSystem=0;
    // Per descriptor set: two timestamps around the stage and the meter's
    // float2 (log2 luminance sum, samples), read back once the set's fence passed.
    ComPtr<ID3D12QueryHeap> timing;
    ComPtr<ID3D12Resource> meter,readback;
    const uint8_t* measurements=nullptr;
    bool measurementsTried=false;
    std::array<bool,kSets> measured{};
    // autorect.1: the Resolution Scale each set measured; only the controller's
    // own level feeds Auto (not a set recorded before a step).
    std::array<float,kSets> measuredScale{};
    uint64_t timestampFrequency=0;
    bool boostLogged=false;
    // Per descriptor set: 1 a Boost network evaluation, 2 a Carry one, 0 neither.
    // gpuMs averages the latest of each, so it is the cost per evaluation.
    std::array<uint8_t,kSets> measuredStep{};
    float boostNetworkMs=-1.0f,boostCarryMs=-1.0f;
};
constexpr uint32_t kMeasurementBytes=32;
std::array<Slot,8> gSlots;
std::array<std::atomic<uintptr_t>,8> gPendingCommands{};
struct Pipeline {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> encode,decode,copy,meter,hold,carry,compose,fit,solve;
    UINT descriptorSize=0;
} gPipeline;
// The NGX entry points and device of the latest DLSS SR/RR evaluation while
// the Present stage is selected: its feature is created through them, on
// command lists of that device.
Api gNgxApi{};
ComPtr<ID3D12Device> gNgxDevice;
// Use DLSS resources: depth and motion vectors of recent DLSS evaluations,
// copied on the game's command list into textures of ours. A set is rewritten
// only after its previous copy and every Present-stage read of it completed
// (their fences), and read only once the copy's submission is known.
struct CaptureSet {
    ComPtr<ID3D12Resource> depth,motion;
    ComPtr<ID3D12Fence> writer,reader;
    const Slot* source=nullptr;
    // writer value after the copy, reader value after the last read, the
    // capture's order and the Present-stage present it belongs to.
    uint64_t written=0,read=0,sequence=0,at=0;
    Rect depthRect{},motionRect{};
    float motionScaleX=1.0f,motionScaleY=1.0f;
    int32_t depthInverted=0;
    // The DLSS output extent of the evaluation (a secondary view's differs
    // from the swapchain's).
    uint32_t outWidth=0,outHeight=0;
};
constexpr uint32_t kCaptureDescriptors=10;
struct Captures {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<CaptureSet,3> sets;
    uint64_t sequence=0;
} gCaptures;
// Present stage: one feature for the principal swapchain, recorded on our
// own command list and submitted to the application's presentation queue
// before its Present reaches the next layer (Streamline DLSS-G).
struct PresentStage {
    Slot slot;
    uintptr_t owner=0;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::array<ComPtr<ID3D12CommandAllocator>,kSets> allocators;
    std::array<uint64_t,kSets> allocatorFence{};
    // Our copy of the swapchain image (its format, UAV); the neutral depth and
    // motion vectors at the extent belong to the slot's feature.
    ComPtr<ID3D12Resource> color;
    PresentInputs inputs=PresentInputs::eNeutral;
    // Logged once: how the presentation queue and NGX's device are layered
    // when a wrapper such as ReShade sits between them.
    bool layersLogged=false;
} gPresent;
// Present-stage presents so far; a capture records the one it belongs to.
uint64_t gPresents=0;
const wchar_t* gPresentRefusal=nullptr;
// Hook Method Present picked (gPresentChosen) and also on (gPresentSelected);
// the last present processed its image with Block Hudless on.
std::atomic<bool> gPresentChosen{false},gPresentSelected{false},gHudlessBlocked{false};
// Compare as the last evaluation applied it (LastCompare).
std::atomic<float> gCompareShown{0.0f};
std::atomic<bool> gCompareHeld{false},gCompareSwapped{false};
std::atomic<uint64_t> gCompareTick{0};
std::atomic<uint64_t> gOverlayDividerTick{0};
std::atomic<shared::LiveFn> gVulkanLive{nullptr};
std::atomic<shared::D3D11ViewFn> gD3D11View{nullptr};
// Encode: stage color -> network signal at the processing extent.
// Decode: network output -> stage color (RGB residual or processed image).
// Bridge: decoded linear color x exposure, scaled so 1 = diffuse white, then
// a shoulder above 0.8 on the brightest channel (only for unbounded color; the
// color is scaled along its ray, so hue and saturation stay) and the NR signal
// encoding. Decode carries the network's change back by the texel's own ratio.
// Resampling filters each tap in the Scaling Domain.
const char kShader[]=R"(
Texture2D<float4> a:register(t0);
Texture2D<float4> b:register(t1);
Texture2D<float4> ex:register(t2);
Texture2D<float4> original:register(t3);
RWTexture2D<float4> dst:register(u0);
cbuffer C:register(b0) {
 uint2 origin; uint2 size; uint2 extent;
 uint encoding, signal, domain, filter, reconstruction, flags;
 float whiteScale, exposure, preExposure, diffuseWhite;
 float adaptiveGain, split, knee, blendStrength;
 float colorStrength, highlightLimit, haloDark, haloBright;
 float haloRadius; uint debugView; float pad1, pad2;
};
RWStructuredBuffer<float2> meter:register(u1);
static const float3x3 k2020to709={1.660491,-0.587641,-0.072850,-0.124550,1.132900,-0.008349,-0.018151,-0.100579,1.118730};
static const float3x3 k709to2020={0.627404,0.329283,0.043313,0.069097,0.919540,0.011362,0.016391,0.088013,0.895595};
float3 PqEncode(float3 y){
 const float m1=0.1593017578125,m2=78.84375,c1=0.8359375,c2=18.8515625,c3=18.6875;
 float3 p=pow(max(y,0),m1);return pow((c1+c2*p)/(1+c3*p),m2);
}
float3 PqDecode(float3 e){
 const float m1=0.1593017578125,m2=78.84375,c1=0.8359375,c2=18.8515625,c3=18.6875;
 float3 p=pow(saturate(e),1.0/m2);return pow(max(p-c1,0)/(c2-c3*p),1.0/m1);
}
float3 SrgbEncode(float3 c){c=saturate(c);return c<=0.0031308?c*12.92:1.055*pow(c,1.0/2.4)-0.055;}
float3 SrgbDecode(float3 c){c=saturate(c);return c<=0.04045?c/12.92:pow((c+0.055)/1.055,2.4);}
// Single-exit helpers: fxc miscompiles early returns from branches here
// (X4000, uninitialized results).
float3 DecodeInput(float3 c){
 float3 r=c;
 if(encoding==2)r=SrgbDecode(c);
 else if(encoding==3)r=mul(k2020to709,PqDecode(c)*(10000.0/80.0));
 else if(encoding==5)r=sign(c)*pow(abs(c),2.2);
 return r;
}
float3 EncodeOutput(float3 c){
 float3 r=c;
 if(encoding==2)r=SrgbEncode(c);
 else if(encoding==3)r=PqEncode(mul(k709to2020,c)*(80.0/10000.0));
 else if(encoding==5)r=sign(c)*pow(abs(c),1.0/2.2);
 return r;
}
float3 ToDomain(float3 c){
 float3 r=c;
 if(domain==0)r=sign(c)*pow(abs(c),1.0/2.2);
 else if(domain==1)r=sign(c)*PqEncode(abs(c)*(80.0/10000.0));
 return r;
}
float3 FromDomain(float3 d){
 float3 r=d;
 if(domain==0)r=sign(d)*pow(abs(d),2.2);
 else if(domain==1)r=sign(d)*PqDecode(abs(d))*(10000.0/80.0);
 return r;
}
float Shoulder(float m){return knee+(1-knee)*(1-exp(-(m-knee)/(1-knee)));}
// hdrbridge.1: how much the bridge compresses an exposed, white-scaled color:
// its brightest channel over that channel's shoulder, one factor for R, G and B
// (1 at or below the knee and for bounded color).
float Compression(float3 r){
 float m=min(max(max(r.r,r.g),max(r.b,0)),65504);
 float k=1;
 if((flags&1)&&m>knee)k=m/Shoulder(m);
 return k;
}
float3 ToSdr(float3 r){
 r=max(r,0);
 float3 s=saturate(r);
 float k=Compression(r);
 if(flags&1)s=k>1?min(r,65504)/k:r;
 return s;
}
float3 ToSignal(float3 s){
 float w=diffuseWhite/10000.0;
 float3 r=s;
 if(signal==0)r=SrgbEncode(s);
 else if(signal==1)r=PqEncode(s*w)/PqEncode(w.xxx).x;
 return r;
}
float3 FromSignal(float3 v){
 float w=diffuseWhite/10000.0;
 v=saturate(v);
 float3 r=v;
 if(signal==0)r=SrgbDecode(v);
 else if(signal==1)r=PqDecode(v*PqEncode(w.xxx).x)/w;
 return r;
}
// Use Exposure Value: the game's exposure texture x scale / pre-exposure
// (pre-exposed color without a texture is already exposed), x Exposure.
float ExposureBase(){
 float game=1;uint mode=(flags>>3)&3;
 if(mode!=0&&(flags&4)){
  float e=ex.Load(int3(0,0,0)).r;
  if(preExposure>0)e/=preExposure;
  if(!(e>0)||!isfinite(e))e=1;
  game=mode==2?1/e:e;
 }
 float f=exposure*game;return f>0&&isfinite(f)?f:0;
}
// x the adaptive exposure gain (1 when off).
float ExposureFactor(){float f=ExposureBase()*adaptiveGain;return f>0&&isfinite(f)?f:0;}
float3 SourceTap(int2 p,float f){
 p=clamp(p,int2(origin),int2(origin+size)-1);
 return ToDomain(DecodeInput(a.Load(int3(p,0)).rgb)*f);
}
// Bounded color clips; unbounded color stays compressed, and Decode expands
// the network's change by the texel's Compression.
float3 NetworkLinear(Texture2D<float4> t,int2 p){
 p=clamp(p,0,int2(extent)-1);
 float3 s=FromSignal(t.Load(int3(p,0)).rgb);
 if(!(flags&1))s=saturate(s);
 return ToDomain(s/whiteScale);
}
[numthreads(8,8,1)] void Encode(uint3 id:SV_DispatchThreadID){
 if(id.x>=extent.x||id.y>=extent.y)return;
 float f=ExposureFactor();
 float3 d=0;
 if(all(extent==size))d=SourceTap(int2(origin+id.xy),f);
 else{
  float2 ratio=float2(size)/float2(extent);
  if(filter==2)d=SourceTap(int2(origin)+int2((id.xy+0.5)*ratio),f);
  else if(filter==0&&all(ratio>1)){
   // Area: every source texel the footprint covers, weighted by overlap.
   float2 lo=id.xy*ratio,hi=(id.xy+1)*ratio;float3 sum=0;float total=0;
   int2 first=int2(floor(lo)),last=int2(ceil(hi))-1;
   last=min(last,first+7);
   for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x){
    float2 cover=saturate(min(hi,float2(x+1,y+1))-max(lo,float2(x,y)));float w=cover.x*cover.y;
    sum+=SourceTap(int2(origin)+int2(x,y),f)*w;total+=w;
   }
   d=sum/max(total,1e-6);
  }else{
   float2 s=(id.xy+0.5)*ratio-0.5;int2 i=int2(floor(s));float2 t=s-i;
   d=lerp(lerp(SourceTap(int2(origin)+i,f),SourceTap(int2(origin)+i+int2(1,0),f),t.x),
          lerp(SourceTap(int2(origin)+i+int2(0,1),f),SourceTap(int2(origin)+i+int2(1,1),f),t.x),t.y);
  }
 }
 dst[id.xy]=float4(ToSignal(ToSdr(FromDomain(d)*whiteScale)),1);
}
float4 CatmullRom(float t){
 float t2=t*t,t3=t2*t;
 return float4(-0.5*t3+t2-0.5*t,1.5*t3-2.5*t2+1,-1.5*t3+2*t2+0.5*t,0.5*t3-0.5*t2);
}
void Upsample(float2 s,out float3 inD,out float3 nrD){
 int2 i=int2(floor(s));float2 t=s-i;
 inD=0;nrD=0;
 if(filter==2){int2 p=int2(round(s));inD=NetworkLinear(a,p);nrD=NetworkLinear(b,p);}
 else if(filter==0){
  inD=lerp(lerp(NetworkLinear(a,i),NetworkLinear(a,i+int2(1,0)),t.x),lerp(NetworkLinear(a,i+int2(0,1)),NetworkLinear(a,i+int2(1,1)),t.x),t.y);
  nrD=lerp(lerp(NetworkLinear(b,i),NetworkLinear(b,i+int2(1,0)),t.x),lerp(NetworkLinear(b,i+int2(0,1)),NetworkLinear(b,i+int2(1,1)),t.x),t.y);
 }else{
  float4 wx=CatmullRom(t.x),wy=CatmullRom(t.y);
  [unroll]for(int y=0;y<4;++y)[unroll]for(int x=0;x<4;++x){
   float w=wx[x]*wy[y];int2 p=i+int2(x-1,y-1);
   inD+=NetworkLinear(a,p)*w;nrD+=NetworkLinear(b,p)*w;
  }
 }
}
float Luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
// Upsample Filter Edge-aware: the bilinear taps, each weighted by how many
// stops its network input lies from the full-resolution image (guide, exposed
// linear, compressed as the network sees it) there, so the change stays on its
// side of an edge.
void UpsampleGuided(float2 s,float3 guide,out float3 inD,out float3 nrD){
 int2 i=int2(floor(s));float2 t=s-i;
 float3 sumIn=0,sumNr=0;float total=0;float yg=Luma(max(guide,0));
 [loop]for(int y=0;y<2;++y)[loop]for(int x=0;x<2;++x){
  int2 p=i+int2(x,y);
  float3 tapIn=NetworkLinear(a,p),tapNr=NetworkLinear(b,p);
  float stops=abs(log2((Luma(max(FromDomain(tapIn),0))+0.01)/(yg+0.01)))/0.6;
  float w=(x?t.x:1-t.x)*(y?t.y:1-t.y)*(exp(-0.5*stops*stops)+1e-4);
  sumIn+=tapIn*w;sumNr+=tapNr*w;total+=w;
 }
 inD=sumIn/total;nrD=sumNr/total;
}
// Supersampling Filter Area: every network texel the output pixel covers,
// weighted by overlap (Resolution Scale above 100 %).
void AreaDown(uint2 id,out float3 inD,out float3 nrD){
 float2 ratio=float2(extent)/float2(size);
 float2 lo=float2(id)*ratio,hi=float2(id+1)*ratio;
 int2 first=int2(floor(lo)),last=min(int2(ceil(hi))-1,first+7);
 float3 sumIn=0,sumNr=0;float total=0;
 for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x){
  float2 cover=saturate(min(hi,float2(x+1,y+1))-max(lo,float2(x,y)));float w=cover.x*cover.y;
  sumIn+=NetworkLinear(a,int2(x,y))*w;sumNr+=NetworkLinear(b,int2(x,y))*w;total+=w;
 }
 inD=sumIn/max(total,1e-6);nrD=sumNr/max(total,1e-6);
}
// Halo reduction: the fine part of the network's change (the change less its
// mean over a ring haloRadius network texels out) is taken back where it
// darkens beside a brighter surface (haloDark) or brightens beside a darker one
// (haloBright). g: the exposure factor over the texel's Compression; white:
// diffuse white in stage units.
float3 HaloCorrection(uint2 id,float3 inL,float3 nrL,float g,float white){
 float2 c=(float2(id)+0.5)*float2(extent)/float2(size);
 float3 sum=nrL-inL;float yc=Luma(inL),hi=yc,lo=yc;
 // Eight directions, 45 degrees apart.
 [loop]for(int k=0;k<8;++k){
  float angle=k*0.785398;
  int2 p=int2(floor(c+float2(cos(angle),sin(angle))*haloRadius));
  float3 ti=FromDomain(NetworkLinear(a,p))/g,tn=FromDomain(NetworkLinear(b,p))/g;
  sum+=tn-ti;float y=Luma(ti);hi=max(hi,y);lo=min(lo,y);
 }
 float3 fine=(nrL-inL)-sum/9;
 float e=0.01*white;
 float nearBright=saturate(((hi+e)/(max(yc,0)+e)-1.25)*2),nearDark=saturate(((max(yc,0)+e)/(max(lo,0)+e)-1.25)*2);
 return -haloDark*nearBright*min(fine,0)-haloBright*nearDark*max(fine,0);
}
[numthreads(8,8,1)] void Decode(uint3 id:SV_DispatchThreadID){
 if(id.x<size.x&&id.y<size.y){
  uint2 at=id.xy+origin;
  float4 source=(flags&2)?dst[at]:original.Load(int3(at,0));
  float f=ExposureFactor();
  float g=max(f,1e-20);
  // Diffuse white in the stage's linear units.
  float white=1/(whiteScale*g);
  float3 base=DecodeInput(source.rgb);
  // The network's change in R, G and B goes back by this texel's own ratio.
  float k=Compression(base*g*whiteScale),gk=k>1?g/k:g;
  float3 inD=0,nrD=0;
  float2 s=(id.xy+0.5)*float2(extent)/float2(size)-0.5;
  if(all(extent==size)){inD=NetworkLinear(a,int2(id.xy));nrD=NetworkLinear(b,int2(id.xy));}
  else if((flags&256)&&all(extent>size))AreaDown(id.xy,inD,nrD);
  else if(filter==3)UpsampleGuided(s,base*gk,inD,nrD);
  else Upsample(s,inD,nrD);
  float3 inL=FromDomain(inD)/gk,nrL=FromDomain(nrD)/gk;
  // Bound an invalid network output; keep unclipped HDR detail.
  float3 bound=max(abs(base),1.0);
  float3 result=base+clamp(nrL-inL,-bound,bound);
  if(reconstruction==1)result=nrL;
  else if(reconstruction==2)result=base*clamp((max(nrL,0)+0.01*white)/(max(inL,0)+0.01*white),0,8);
  // Blend controls at their defaults leave the result untouched (bit for bit).
  float3 r=result;
  if(haloDark+haloBright>0)r+=HaloCorrection(id.xy,inL,nrL,gk,white);
  // Colour strength: 0 keeps the game's hue and takes only the brightness change.
  if(colorStrength!=1){
   float yb=Luma(base),yr=Luma(r);
   float3 tone=yb>1e-6*white?base*clamp(yr/yb,0,16):base+(yr-yb);
   r=tone+(r-tone)*colorStrength;
  }
  if(blendStrength!=1)r=base+(r-base)*blendStrength;
  // Highlight guard: no brighter than highlightLimit times the original luminance (2 % of white at least).
  if(highlightLimit>0){
   float cap=highlightLimit*max(Luma(base),0.02*white),yr=Luma(r);
   if(yr>cap)r*=cap/yr;
  }
  // Debug views: the network's input or output as it sees them (SDR, 1 = diffuse
  // white), or mid grey where unchanged with 8x the change in a gamma-2.2 view.
  if(debugView==1)r=ToSdr(inL/white)*white;
  else if(debugView==2)r=ToSdr(nrL/white)*white;
  else if(debugView==3){
   float3 v=0.5+8*(sign(r)*pow(abs(r/white),1/2.2)-sign(base)*pow(abs(base/white),1/2.2));
   r=pow(saturate(v),2.2)*white;
  }
  float4 written=source;
  if(f>0&&all(isfinite(r)))written=float4(EncodeOutput(r),source.a);
  // Compare: the original left of the split (right of it with Swap sides), and a divider on it.
  bool left=float(id.x)<split;
  if((flags&32)&&((flags&128)?!left:left))written=source;
  // White core (2 px at 1080p, 3 at 4K; 200 nits in HDR) in 1 px black edges.
  if(flags&64){
   float d=abs(float(id.x)+0.5-split),core=max(1.0,float(size.x)/2560.0),edge=1.0;
   if(d<core)written=float4(EncodeOutput((float3)((flags&1)?2.5:1.0)),source.a);
   else if(d<core+edge)written=float4(EncodeOutput((float3)0.0),source.a);
  }
  dst[at]=written;
 }
}
// Adaptive exposure meter: log2 luminance of the exposed, white-scaled stage
// color on a 64x64 grid, without the gain it feeds; one group.
groupshared float2 partial[256];
[numthreads(16,16,1)] void Meter(uint3 id:SV_GroupThreadID){
 uint index=id.y*16+id.x;
 float f=ExposureBase();
 float2 total=0;
 for(uint y=0;y<4;++y)for(uint x=0;x<4;++x){
  float2 cell=float2(id.xy*4+uint2(x,y))+0.5;
  int2 p=clamp(int2(origin)+int2(cell*float2(size)/64.0),int2(origin),int2(origin+size)-1);
  float3 rel=max(DecodeInput(a.Load(int3(p,0)).rgb)*f*whiteScale,0);
  float lum=dot(rel,float3(0.2126,0.7152,0.0722));
  if(lum>1e-6)total+=float2(log2(clamp(lum,1.0/16384.0,16384.0)),1);
 }
 partial[index]=total;
 GroupMemoryBarrierWithGroupSync();
 for(uint stride=128;stride>0;stride>>=1){
  if(index<stride)partial[index]+=partial[index+stride];
  GroupMemoryBarrierWithGroupSync();
 }
 if(index==0)meter[0]=partial[0];
}
// Present stage: `size` texels of a (from `origin`) to dst; a null a writes zeros.
[numthreads(8,8,1)] void Copy(uint3 id:SV_DispatchThreadID){
 if(id.x<size.x&&id.y<size.y)dst[id.xy]=a.Load(int3(origin+id.xy,0));
}
)";
// Boost: the stage tables' t0..t3 and u0 with constants of its own. On a
// network run, Fit and Solve fit the look model (dlss5_policy.h BoostGrid) to
// the network's input and output, and Hold keeps what the model does not
// explain. On the evaluation between, Carry applies the model to that
// evaluation's own input, so edges and animation always come from the current
// image, and adds the held fine detail from where each texel was wherever its
// 3x3 luminance still matches (a surface that only moved).
// Fit: t0 the network input, t1 its output -> the model's sums.
// Solve: t0 the sums -> the model.
// Hold: t0 the network input, t1 its output, t2 the model -> the detail and luminance.
// Carry: t0 this evaluation's network input, t1 the held detail, t2 the model,
// t3 the game motion vectors -> the network output's stand-in.
// Compose: t0 the game motion vectors, t1 the Carry evaluation's (ours, at
// 0,0) -> the motion over both evaluations, in the game's units.
const char kBoostShader[]=R"(
Texture2D<float4> t0s:register(t0);
Texture2D<float4> t1s:register(t1);
Texture2D<float4> t2s:register(t2);
Texture2D<float4> t3s:register(t3);
RWTexture2D<float4> dst:register(u0);
cbuffer B:register(b0) {
 int2 motionOrigin; uint2 motionSize;
 uint2 extent; float2 motionScale;
 float stableBelow, unstableAbove; uint cell; uint bins;
 uint2 cells; float lambda; uint pad0;
 uint4 pad1;
};
// Where extent texel e was in the previous evaluation (pixel centers), from the game's motion vectors.
float2 Source(uint2 e,Texture2D<float4> motion){
 float2 at=(float2(e)+0.5)*float2(motionSize)/float2(extent);
 int2 m=motionOrigin+clamp(int2(at),0,int2(motionSize)-1);
 return float2(e)+0.5+motion.Load(int3(m,0)).xy*motionScale*float2(extent)/float2(motionSize);
}
bool Inside(float2 q){return all(isfinite(q))&&all(q>=0)&&all(q<float2(extent));}
float Luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
float Bin(float3 c){return saturate(Luma(c))*float(bins-1);}
float4 Bilinear(Texture2D<float4> t,float2 q){
 float2 s=q-0.5;int2 i=int2(floor(s));float2 f=s-i;int2 hi=int2(extent)-1;
 float4 a=t.Load(int3(clamp(i,0,hi),0)),b=t.Load(int3(clamp(i+int2(1,0),0,hi),0));
 float4 c=t.Load(int3(clamp(i+int2(0,1),0,hi),0)),d=t.Load(int3(clamp(i+int2(1,1),0,hi),0));
 return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
// The look model: per cell (cell x cell texels) and luminance bin, an affine map of the network input
// to its output, fitted on a network evaluation (Fit, Solve) and applied to any later input (Look).
// Fit: t0 the network input, t1 its output -> u0 the sums of each cell and bin, 6 texels each:
// weight, x (3), x x' (6), x y' (9), y (3); each texel adds to the two bins nearest its luminance.
groupshared float sums[256*22];
[numthreads(16,16,1)] void Fit(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,uint index:SV_GroupIndex){
 const uint per=max(cell/16,1);
 for(uint b=0;b<bins;++b){
  float s[22];
  [unroll]for(uint k=0;k<22;++k)s[k]=0;
  for(uint py=0;py<per;++py)for(uint px=0;px<per;++px){
   const uint2 p=group.xy*cell+local.xy+16*uint2(px,py);
   if(p.x<extent.x&&p.y<extent.y){
    const float3 x=t0s.Load(int3(p,0)).rgb,y=t1s.Load(int3(p,0)).rgb;
    const float w=saturate(1-abs(Bin(x)-float(b)));
    s[0]+=w;
    s[1]+=w*x.r;s[2]+=w*x.g;s[3]+=w*x.b;
    s[4]+=w*x.r*x.r;s[5]+=w*x.r*x.g;s[6]+=w*x.r*x.b;s[7]+=w*x.g*x.g;s[8]+=w*x.g*x.b;s[9]+=w*x.b*x.b;
    s[10]+=w*x.r*y.r;s[11]+=w*x.r*y.g;s[12]+=w*x.r*y.b;
    s[13]+=w*x.g*y.r;s[14]+=w*x.g*y.g;s[15]+=w*x.g*y.b;
    s[16]+=w*x.b*y.r;s[17]+=w*x.b*y.g;s[18]+=w*x.b*y.b;
    s[19]+=w*y.r;s[20]+=w*y.g;s[21]+=w*y.b;
   }
  }
  [unroll]for(uint k2=0;k2<22;++k2)sums[index*22+k2]=s[k2];
  GroupMemoryBarrierWithGroupSync();
  for(uint stride=128;stride>0;stride>>=1){
   if(index<stride){[unroll]for(uint k3=0;k3<22;++k3)sums[index*22+k3]+=sums[(index+stride)*22+k3];}
   GroupMemoryBarrierWithGroupSync();
  }
  if(index<6){
   float4 v;
   v.x=sums[index*4];v.y=sums[index*4+1];
   v.z=index*4+2<22?sums[index*4+2]:0;v.w=index*4+3<22?sums[index*4+3]:0;
   dst[uint2(group.x*6+index,group.y*bins+b)]=v;
  }
  GroupMemoryBarrierWithGroupSync();
 }
}
// Solve: t0 the sums -> u0 the model, 3 texels per cell and bin (output r, g, b: the weights of the
// input's r, g, b and an offset). The sums are blurred [1 2 1] over the neighbouring cells first;
// lambda pulls each map toward the plain offset (the identity plus the mean change), so flat or
// empty cells and bins keep the tone change without inventing contrast.
[numthreads(8,8,1)] void Solve(uint3 id:SV_DispatchThreadID){
 if(id.x>=cells.x||id.y>=cells.y*bins)return;
 const int2 c=int2(id.x,id.y/bins);const uint b=id.y%bins;
 float s[24];
 [unroll]for(uint k=0;k<24;++k)s[k]=0;
 for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
  const int2 n=c+int2(dx,dy);
  if(all(n>=0)&&all(n<int2(cells))){
   const float w=float((2-abs(dx))*(2-abs(dy)));
   [unroll]for(uint m=0;m<6;++m){
    const float4 v=t0s.Load(int3(n.x*6+m,n.y*bins+b,0))*w;
    s[m*4]+=v.x;s[m*4+1]+=v.y;s[m*4+2]+=v.z;s[m*4+3]+=v.w;
   }
  }
 }
 float4 model[3]={float4(1,0,0,0),float4(0,1,0,0),float4(0,0,1,0)};
 const float n=s[0];
 if(n>1e-3){
  const float3 mx=float3(s[1],s[2],s[3])/n,my=float3(s[19],s[20],s[21])/n;
  float3x3 cxx=float3x3(s[4],s[5],s[6],s[5],s[7],s[8],s[6],s[8],s[9])/n;
  float3x3 cxy=float3x3(s[10],s[11],s[12],s[13],s[14],s[15],s[16],s[17],s[18])/n;
  [unroll]for(uint i=0;i<3;++i)[unroll]for(uint j=0;j<3;++j){
   cxx[i][j]-=mx[i]*mx[j];cxy[i][j]-=mx[i]*my[j];
   if(i==j){cxx[i][j]+=lambda;cxy[i][j]+=lambda;}
  }
  // cxx is symmetric positive definite (lambda > 0): its inverse by cofactors.
  const float3 r0=cxx[0],r1=cxx[1],r2=cxx[2];
  const float3 k0=cross(r1,r2),k1=cross(r2,r0),k2=cross(r0,r1);
  const float det=dot(r0,k0);
  if(det>1e-12){
   const float3x3 inverse=transpose(float3x3(k0,k1,k2))/det;
   const float3x3 a=mul(inverse,cxy);
   [unroll]for(uint o=0;o<3;++o){
    const float3 weights=float3(a[0][o],a[1][o],a[2][o]);
    model[o]=float4(weights,my[o]-dot(mx,weights));
   }
  }
 }
 [unroll]for(uint o2=0;o2<3;++o2)dst[uint2(id.x*3+o2,id.y)]=model[o2];
}
// The model (t2s) applied to input x, with the cells around extent position q (pixel centers):
// bilinear between cells, linear between the luminance bins of x.
float3 Look(float3 x,float2 q){
 float2 g=clamp(q/float(cell)-0.5,0,float2(cells)-1);
 const int2 i0=int2(floor(g));const float2 f=g-i0;const int2 i1=min(i0+1,int2(cells)-1);
 const float z=Bin(x);const uint z0=uint(floor(z));const uint z1=min(z0+1,bins-1);const float fz=z-z0;
 float4 m0=0,m1=0,m2=0;
 [unroll]for(uint corner=0;corner<8;++corner){
  const bool bx=(corner&1)!=0,by=(corner&2)!=0,bz=(corner&4)!=0;
  const int2 at=int2(bx?i1.x:i0.x,by?i1.y:i0.y);const uint zb=bz?z1:z0;
  const float w=(bx?f.x:1-f.x)*(by?f.y:1-f.y)*(bz?fz:1-fz);
  const int row=at.y*int(bins)+int(zb);
  m0+=w*t2s.Load(int3(at.x*3,row,0));m1+=w*t2s.Load(int3(at.x*3+1,row,0));m2+=w*t2s.Load(int3(at.x*3+2,row,0));
 }
 const float4 x1=float4(x,1);
 return float3(dot(m0,x1),dot(m1,x1),dot(m2,x1));
}
// Hold: t0 the network input, t1 its output, t2 the model -> what the model does not explain
// (fine detail) and the input's luminance, for the next evaluation's Carry.
[numthreads(8,8,1)] void Hold(uint3 id:SV_DispatchThreadID){
 if(id.x>=extent.x||id.y>=extent.y)return;
 const float3 x=t0s.Load(int3(id.xy,0)).rgb;
 dst[id.xy]=float4(t1s.Load(int3(id.xy,0)).rgb-Look(x,float2(id.xy)+0.5),Luma(x));
}
// Carry: t0 this evaluation's network input, t1 the held detail, t2 the model, t3 the game motion
// vectors -> the network output's stand-in: the model (from where each texel was) applied to this
// input, plus the held detail where this input's 3x3 luminance matches where it came from.
[numthreads(8,8,1)] void Carry(uint3 id:SV_DispatchThreadID){
 if(id.x>=extent.x||id.y>=extent.y)return;
 const float3 now=t0s.Load(int3(id.xy,0)).rgb;
 const float2 source=Source(id.xy,t3s);
 const bool inside=Inside(source);
 const float2 q=inside?source:float2(id.xy)+0.5;
 float d=0;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  const int2 p=clamp(int2(id.xy)+int2(x,y),0,int2(extent)-1);
  d=max(d,abs(Luma(t0s.Load(int3(p,0)).rgb)-Bilinear(t1s,q+float2(x,y)).a));
 }
 const float keep=inside?saturate(1-(d-stableBelow)/max(unstableAbove-stableBelow,1e-6)):0;
 dst[id.xy]=float4(Look(now,q)+Bilinear(t1s,q).rgb*keep,1);
}
[numthreads(8,8,1)] void Compose(uint3 id:SV_DispatchThreadID){
 if(id.x>=motionSize.x||id.y>=motionSize.y)return;
 float2 now=t0s.Load(int3(motionOrigin+int2(id.xy),0)).xy;
 float2 q=float2(id.xy)+0.5+now*motionScale;
 float2 before=now;
 if(all(isfinite(q))&&all(q>=0)&&all(q<float2(motionSize)))before=t1s.Load(int3(int2(q),0)).xy;
 dst[id.xy]=float4(now+before,0,0);
}
)";
void Log(const wchar_t* message) noexcept { if(auto fn=gLog.load()) fn(message); }
void Failure(State state,uint32_t result) noexcept {
    gState=state;gResult=result;++gFailures;
    wchar_t message[180]{};
    swprintf_s(message,L"DLSS 5 unavailable: state=%u result=0x%08X",static_cast<unsigned>(state),result);
    Log(message);
}
void SlotFailure(Slot& slot,State state,uint32_t result) noexcept {
    slot.failed=true;slot.failureRevision=slot.revision;slot.failureState=state;slot.failureResult=result;Failure(state,result);
}
Settings Wanted(uint64_t& revision) noexcept {std::lock_guard lock(gSettingsMutex);revision=gRevision;return gWanted;}
Slot* Find(size_t route,uintptr_t game) noexcept {
    if(!game)return nullptr;
    for(auto& slot:gSlots) if(slot.occupied && !slot.retiring && slot.route==route && slot.game==game)return &slot;
    return nullptr;
}
void Transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&barrier);
}
// The game's output, which can carry a mip chain: only mip 0 is ours to move.
void TransitionMip0(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,0,before,after};cmd->ResourceBarrier(1,&barrier);
}
void Uav(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=resource;cmd->ResourceBarrier(1,&barrier);
}
bool Complete(const Slot& slot) noexcept {
    return slot.flight.Drained(slot.fence ? slot.fence->GetCompletedValue() : 0);
}
// The list fenced with `value` was submitted and executed (0: nothing to wait for).
bool Passed(const Slot& slot,uint64_t value) noexcept {
    if(!value)return true;
    const uint64_t completed=slot.fence ? slot.fence->GetCompletedValue() : 0;
    return !slot.flight.failed && slot.flight.submitted>=value && completed!=UINT64_MAX && completed>=value;
}
// The fence value after which nothing recorded so far is outstanding: the
// list being recorded when there is one, else the last one submitted.
uint64_t Recorded(const Slot& slot) noexcept {
    return slot.flight.recording ? slot.flight.Next() : slot.flight.submitted;
}
// Releases a feature's passes and parameters; its textures go with it. A
// refused release fails the slot, which keeps what is left.
bool ReleaseFeature(Slot& slot,Feature& feature) noexcept {
    while(feature.passes){
        auto*& handle=feature.handles[feature.passes-1];
        const auto result=slot.direct ? gDirect.Release(handle) : slot.api.release(handle);
        if(result!=NVSDK_NGX_Result_Success) {slot.cleanupFailed=true;SlotFailure(slot,State::eEvaluateFailed,static_cast<uint32_t>(result));return false;}
        handle=nullptr;--feature.passes;
    }
    if(feature.params) {
        if(!slot.api.destroy || slot.api.destroy(feature.params)!=NVSDK_NGX_Result_Success) {slot.cleanupFailed=true;SlotFailure(slot,State::eEvaluateFailed,NVSDK_NGX_Result_FAIL_InvalidParameter);return false;}
        feature.params=nullptr;
    }
    feature=Feature{};return true;
}
// Any feature of the slot: live, being created, or replaced and not yet released.
bool Outstanding(const Slot& slot) noexcept {
    if(slot.Holds() || slot.next.Holds())return true;
    for(const auto& entry:slot.retired)if(entry.feature.Holds())return true;
    return false;
}
// Hands `feature` over to be released once everything recorded so far
// executed. False while every retired entry is still taken.
bool Retire(Slot& slot,Feature& feature) noexcept {
    if(!feature.Holds()){feature=Feature{};return true;}
    for(auto& entry:slot.retired)if(!entry.feature.Holds()){
        entry.feature=std::move(feature);entry.after=Recorded(slot);feature=Feature{};return true;
    }
    return false;
}
// Releases the retired features whose lists executed.
void Collect(Slot& slot) noexcept {
    for(auto& entry:slot.retired)
        if(!slot.cleanupFailed && entry.feature.Holds() && Passed(slot,entry.after) && !ReleaseFeature(slot,entry.feature))return;
}
void Destroy(Slot& slot) noexcept {
    if(!Complete(slot) || slot.cleanupFailed)return;
    for(auto& entry:slot.retired)if(!ReleaseFeature(slot,entry.feature))return;
    if(!ReleaseFeature(slot,slot.next))return;
    slot.nextReady=0;
    if(!ReleaseFeature(slot,slot))return;
    slot.substitute.Reset();
    slot.measuredStep={};slot.boostNetworkMs=slot.boostCarryMs=-1.0f;
    slot.descriptorFence={};slot.measured={};
}
void ReleasePresentStage() noexcept;
void LogVideoMemory(Slot& slot,const wchar_t* why,bool force) noexcept;
void Pump() noexcept {
    for(auto& slot:gSlots) if(slot.occupied && slot.retiring && !slot.game && Complete(slot)) {
        Destroy(slot);
        if(!Outstanding(slot)) slot=Slot{};
    }
    // Hook Method Off releases the features once their work executed.
    uint64_t revision=0;
    if(!Wanted(revision).enabled) for(auto& slot:gSlots) if(slot.occupied && !slot.retiring && Outstanding(slot)) {
        Collect(slot);if(!Complete(slot))continue;
        Destroy(slot);
        if(!Outstanding(slot)){slot.gpuMs=-1.0f;Log(L"DLSS 5 off: its feature was released");LogVideoMemory(slot,L"released",true);}
    }
    if(!gPresentSelected.load(std::memory_order_acquire))ReleasePresentStage();
}
// typed: the game stored the value as ID3D12Resource* (else void*).
bool Resource(const NVSDK_NGX_Parameter* params,const char* key,uintptr_t& out,bool* typed=nullptr) noexcept {
    __try {
        ID3D12Resource* value=nullptr;void* raw=nullptr;
        if(params->Get(key,&value)==NVSDK_NGX_Result_Success && value) {out=reinterpret_cast<uintptr_t>(value);if(typed)*typed=true;return true;}
        if(params->Get(key,&raw)==NVSDK_NGX_Result_Success && raw) {out=reinterpret_cast<uintptr_t>(raw);if(typed)*typed=false;return true;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
bool SetResource(const NVSDK_NGX_Parameter* params,const char* key,uintptr_t value,bool typed) noexcept {
    __try {
        auto* writable=const_cast<NVSDK_NGX_Parameter*>(params);
        if(typed)writable->Set(key,reinterpret_cast<ID3D12Resource*>(value));
        else writable->Set(key,reinterpret_cast<void*>(value));
        uintptr_t check=0;bool checkTyped=false;
        return Resource(params,key,check,&checkTyped) && check==value;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool Unsigned(const NVSDK_NGX_Parameter* params,const char* key,uint32_t& out) noexcept {
    __try {
        unsigned int u=0;int i=0;
        if(params->Get(key,&u)==NVSDK_NGX_Result_Success) {out=u;return true;}
        if(params->Get(key,&i)==NVSDK_NGX_Result_Success && i>=0) {out=static_cast<uint32_t>(i);return true;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
bool OptionalUnsigned(const NVSDK_NGX_Parameter* params,const char* key,uint32_t& out) noexcept {
    __try {
        unsigned int value=0;int signedValue=0;
        if(params->Get(key,&value)==NVSDK_NGX_Result_Success){out=value;return true;}
        if(params->Get(key,&signedValue)==NVSDK_NGX_Result_Success){if(signedValue<0)return false;out=static_cast<uint32_t>(signedValue);}
        return true; // SDK default for an absent optional key
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Float(const NVSDK_NGX_Parameter* params,const char* key,float& out) noexcept {
    __try { float value=0;if(params->Get(key,&value)==NVSDK_NGX_Result_Success)out=value; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
// The evaluation as `frame`. When it cannot feed DLSSNR, `why` says why and
// `source` holds what was read.
bool ReadFrame(const NVSDK_NGX_Parameter* params,const dlss_sr::CreateKeys& keys,Stage stage,Frame& frame,
    InputsWhy& why,Source& source) noexcept {
    source={};why={};
    if(!params){why={InputGap::eResource,"parameter"};return false;}
    if(!(keys.present & dlss_sr::CreateKeys::eCreateFlags)){why={InputGap::eCreateKeys,"DLSS.Feature.Create.Flags"};return false;}
    if(!Resource(params,"Depth",source.depth)){why={InputGap::eResource,"Depth"};return false;}
    if(!Resource(params,"MotionVectors",source.motion)){why={InputGap::eResource,"MotionVectors"};return false;}
    if(stage==Stage::eRender ? !Resource(params,"Color",source.color) : !Resource(params,"Output",source.output)){
        why={InputGap::eResource,stage==Stage::eRender?"Color":"Output"};return false;
    }
    Resource(params,"ExposureTexture",source.exposure);
    const std::pair<const char*,uint32_t*> subrects[]{{"DLSS.Output.Subrect.Base.X",&source.outputX},
        {"DLSS.Output.Subrect.Base.Y",&source.outputY},{"DLSS.Input.Color.Subrect.Base.X",&source.colorX},
        {"DLSS.Input.Color.Subrect.Base.Y",&source.colorY},{"DLSS.Input.Depth.Subrect.Base.X",&source.depthX},
        {"DLSS.Input.Depth.Subrect.Base.Y",&source.depthY},{"DLSS.Input.MV.Subrect.Base.X",&source.motionX},
        {"DLSS.Input.MV.Subrect.Base.Y",&source.motionY},{"DLSS.Render.Subrect.Dimensions.Width",&source.renderWidth},
        {"DLSS.Render.Subrect.Dimensions.Height",&source.renderHeight}};
    for(const auto& [key,value]:subrects)if(!OptionalUnsigned(params,key,*value)){why={InputGap::eSubrectKey,key};return false;}
    Float(params,"MV.Scale.X",source.motionScaleX);Float(params,"MV.Scale.Y",source.motionScaleY);
    float exposureScale=1.0f;Float(params,"DLSS.Exposure.Scale",exposureScale);
    Float(params,"DLSS.Pre.Exposure",source.preExposure);
    // The pre-exposure divides; the exposure scale multiplies the texture value.
    if(std::isfinite(exposureScale) && exposureScale>0.0f && source.preExposure>0.0f)source.preExposure/=exposureScale;
    else if(std::isfinite(exposureScale) && exposureScale>0.0f && exposureScale!=1.0f)source.preExposure=1.0f/exposureScale;
    uint32_t reset=0;Unsigned(params,"Reset",reset);source.reset=reset!=0;
    return Build(source,keys,stage,false,frame,&why);
}
// Why DLSS 5 has no inputs, logged once until an evaluation runs again or the
// reason changes (gCaptureGap: the Present stage's copy of DLSS's depth and
// motion vectors, until a capture succeeds). Callers set State::eMissingInputs.
wchar_t gInputsGap[512]{},gCaptureGap[512]{};
constexpr wchar_t kSlotsFull[]=L"all 8 DLSS feature slots are in use, so this DLSS feature is not tracked";
static_assert(std::tuple_size_v<decltype(gSlots)> ==8);
void InputsUnavailable(const wchar_t* reason,wchar_t (&last)[512]=gInputsGap) noexcept {
    if(!wcscmp(last,reason))return;
    wcsncpy_s(last,reason,_TRUNCATE);
    wchar_t message[600]{};swprintf_s(message,L"DLSS 5 inputs unavailable: %.512ls",reason);Log(message);
}
void InputsUnavailable(const wchar_t* context,const InputsWhy& why,const Source& source,const dlss_sr::CreateKeys& keys,
    wchar_t (&last)[512]=gInputsGap) noexcept {
    wchar_t reason[512]{};
    const uint32_t renderWidth=source.renderWidth?source.renderWidth:keys.width,renderHeight=source.renderHeight?source.renderHeight:keys.height;
    switch(why.gap){
    case InputGap::eRenderSize:
        swprintf_s(reason,L"%lsrender subrect %ux%u exceeds the DLSS Create size %ux%u (output %ux%u); DLSS SR reads such a subrect whole, so it is not clamped",
            context,renderWidth,renderHeight,keys.width,keys.height,keys.outWidth,keys.outHeight);break;
    case InputGap::eCreateKeys:
        swprintf_s(reason,L"%lsthe DLSS Create gave no usable %.80hs (keys 0x%X, %ux%u -> %ux%u)",context,why.key,
            static_cast<unsigned>(keys.present),keys.width,keys.height,keys.outWidth,keys.outHeight);break;
    case InputGap::eResource:swprintf_s(reason,L"%lsthe DLSS evaluation has no %.80hs resource",context,why.key);break;
    case InputGap::eSubrectKey:swprintf_s(reason,L"%ls%.80hs holds a negative value",context,why.key);break;
    case InputGap::eRange:
        swprintf_s(reason,L"%lsa subrect size or offset is beyond INT_MAX (render %ux%u, Create %ux%u -> %ux%u)",context,
            renderWidth,renderHeight,keys.width,keys.height,keys.outWidth,keys.outHeight);break;
    case InputGap::eMotionScale:swprintf_s(reason,L"%lsMV.Scale is not finite",context);break;
    default:return;
    }
    InputsUnavailable(reason,last);
}
// A typed view format for a game resource's format (SRV), or UNKNOWN.
DXGI_FORMAT ViewFormat(DXGI_FORMAT format) noexcept {
    switch(format){
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
        return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// A view of a game depth buffer's depth (plane 0) for our Present-stage copy, or UNKNOWN.
DXGI_FORMAT DepthViewFormat(DXGI_FORMAT format) noexcept {
    switch(format){
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// viewed: we create a shader view of it (stage color, exposure), so its format
// needs a typed view. The provider alone reads depth and motion vectors, whose
// formats (e.g. a depth-stencil typeless depth buffer) it handles itself.
// anyMips: the stage color may carry a mip chain; our views and barriers only touch mip 0.
bool TextureOnDevice(ID3D12Resource* resource,ID3D12Device* device,const Rect& rect,D3D12_RESOURCE_DESC& desc,bool viewed,bool anyMips=false) noexcept {
    __try {
        ID3D12Device* owner=nullptr;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owner))) || !owner)return false;
        const bool same=owner==device;owner->Release();if(!same)return false;
        desc=resource->GetDesc();
        return desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize==1
            && (anyMips || desc.MipLevels==1) && desc.SampleDesc.Count==1 && !(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            && RectFits(rect,desc.Width,desc.Height) && (!viewed || ViewFormat(desc.Format)!=DXGI_FORMAT_UNKNOWN);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
// Logs a refused input once per change of what was refused.
void InputsRefused(const wchar_t* what,const D3D12_RESOURCE_DESC& desc,const Rect& rect) noexcept {
    static uint64_t last=0;
    const uint64_t key=(uint64_t(reinterpret_cast<uintptr_t>(what))<<16)^desc.Format^(uint64_t(desc.Flags)<<8);
    if(key==last)return;
    last=key;
    wchar_t message[400]{};
    swprintf_s(message,L"DLSS 5 inputs refused: %ls format=%u flags=0x%X size=%llux%u mips=%u samples=%u rect=%d,%d %dx%d",
        what,static_cast<unsigned>(desc.Format),static_cast<unsigned>(desc.Flags),static_cast<unsigned long long>(desc.Width),
        desc.Height,desc.MipLevels,desc.SampleDesc.Count,rect.x,rect.y,rect.width,rect.height);
    Log(message);
}
bool TypedLoadStore(ID3D12Device* device,DXGI_FORMAT format) noexcept {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))
        && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}
// The stage color must be readable; the Upscaled output is also written in
// place. A game exposure texture that cannot be read is treated as absent.
// Motion vectors are read as .xy: two- or four-channel 16/32-bit float views
// (Cyberpunk 2077 hands DLSS RGBA16F). UNKNOWN for anything else.
DXGI_FORMAT MotionCaptureFormat(DXGI_FORMAT view) noexcept {
    switch(view){
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool ValidateResources(const Slot& slot,Frame& frame,Stage stage,D3D12_RESOURCE_DESC& color) noexcept {
    if(!slot.device)return false;
    D3D12_RESOURCE_DESC depth{},motion{};
    if(!TextureOnDevice(reinterpret_cast<ID3D12Resource*>(frame.color),slot.device.Get(),frame.colorRect,color,true,true)){
        InputsRefused(stage==Stage::eRender?L"color":L"output",color,frame.colorRect);return false;
    }
    if(color.MipLevels>1){
        static std::atomic<bool> noted{false};
        if(!noted.exchange(true)){
            wchar_t message[160]{};
            swprintf_s(message,L"DLSS 5: %ls has %u mips, processing mip 0 only",stage==Stage::eRender?L"color":L"output",color.MipLevels);
            Log(message);
        }
    }
    if(!TextureOnDevice(reinterpret_cast<ID3D12Resource*>(frame.depth),slot.device.Get(),frame.depthRect,depth,false)){
        InputsRefused(L"depth",depth,frame.depthRect);return false;
    }
    if(!TextureOnDevice(reinterpret_cast<ID3D12Resource*>(frame.motion),slot.device.Get(),frame.motionRect,motion,false)){
        InputsRefused(L"motion vectors",motion,frame.motionRect);return false;
    }
    if(MotionCaptureFormat(ViewFormat(motion.Format))==DXGI_FORMAT_UNKNOWN){
        InputsRefused(L"motion vector format",motion,frame.motionRect);return false;
    }
    if(stage==Stage::eUpscaled){
        const DXGI_FORMAT format=ViewFormat(color.Format);
        if(!(color.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || !TypedLoadStore(slot.device.Get(),format)){
            InputsRefused(L"output UAV format",color,frame.colorRect);return false;
        }
    }
    if(frame.exposure){
        D3D12_RESOURCE_DESC exposure{};
        if(!TextureOnDevice(reinterpret_cast<ID3D12Resource*>(frame.exposure),slot.device.Get(),{0,0,1,1},exposure,true))frame.exposure=0;
    }
    return true;
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    // Provider-internal queues never wait on the NR control lock. An exact
    // application command list with our recorded work must obtain its fence,
    // including when the status worker currently holds that lock.
    bool pending=false;
    for(const auto& item:gPendingCommands){
        const uintptr_t command=item.load(std::memory_order_acquire);if(!command)continue;
        for(UINT i=0;i<count;++i)if(reinterpret_cast<uintptr_t>(lists[i])==command)pending=true;
    }
    if(!pending){gExecute(queue,count,lists);return;}
    std::lock_guard lock(gMutex);
    std::array<bool,8> used{};
    for(size_t n=0;n<gSlots.size();++n) {
        auto& slot=gSlots[n];if(!slot.occupied || !slot.flight.recording)continue;
        for(UINT i=0;i<count;++i) if(reinterpret_cast<uintptr_t>(lists[i])==slot.flight.recording)used[n]=true;
        if(used[n] && slot.lastQueue && slot.lastQueue.Get()!=queue && slot.flight.submitted)
            if(FAILED(queue->Wait(slot.fence.Get(),slot.flight.submitted)))slot.flight.failed=true;
    }
    gExecute(queue,count,lists);
    for(size_t n=0;n<gSlots.size();++n)if(used[n]) {
        auto& slot=gSlots[n];const auto value=slot.flight.Next();
        const bool success=value && !slot.flight.failed && SUCCEEDED(queue->Signal(slot.fence.Get(),value));
        slot.flight.Signaled(slot.flight.recording,value,success);
        if(success) {gPendingCommands[n].store(0,std::memory_order_release);slot.recording.Reset();slot.lastQueue=queue;}
        else {SlotFailure(slot,State::eEvaluateFailed,kProviderFault);}
    }
}
// The game submits through the same object layering as a queue created from
// the device that owns its DLSS command list: the D3D12 runtime (system or
// the game's Agility SDK) or a wrapping layer such as ReShade. Only that
// layer's ExecuteCommandLists sees the command list pointers NGX was given, so
// it is the one hooked. It must be code in a loaded image other than ours, and
// that image is pinned for the process once the hook exists. Where the slot
// holds our own table hook (Witcher 3 hair, witcher_dots), the runtime method
// that hook forwards to is hooked: every submission still reaches it with the
// same command list pointers.
// `detail` says which part failed, with MinHook's status and the entry's first bytes, so an entry another hook
// already rewrote can be told from a pin or protection failure.
bool PrepareQueue(ID3D12Device* device,std::wstring& owner,std::wstring& detail) noexcept {
    ComPtr<ID3D12CommandQueue> probe;D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if(FAILED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&probe)))){detail=L"probe queue";return false;}
    void* target=(*reinterpret_cast<void***>(probe.Get()))[10];
    bool behindOwn=false;
    if(const auto forward=gQueueForward.load(std::memory_order_acquire))
        if(void* inner=forward(probe.Get())){target=inner;behindOwn=true;}
    if(gExecuteTarget){if(target!=gExecuteTarget)detail=L"another queue type already hooked";return target==gExecuteTarget;}
    MEMORY_BASIC_INFORMATION memory{};
    if(VirtualQuery(target,&memory,sizeof(memory))!=sizeof(memory) || memory.Type!=MEM_IMAGE || !memory.AllocationBase)
        {detail=L"entry is not in a loaded image";return false;}
    HMODULE module=nullptr,self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&module) || module!=static_cast<HMODULE>(memory.AllocationBase))
        {detail=L"entry module";return false;}
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(module,path,32768);
    if(!length || length>=32768){detail=L"entry module path";return false;}
    owner.assign(path,length);
    const auto refused=[&](const wchar_t* part,MH_STATUS status) noexcept {
        wchar_t text[160]{};
        const auto* bytes=static_cast<const unsigned char*>(target);
        int at=swprintf_s(text,L"%ls (%hs) entry bytes",part,MH_StatusToString(status));
        const uintptr_t end=reinterpret_cast<uintptr_t>(memory.BaseAddress)+memory.RegionSize;
        const int readable=static_cast<int>((std::min<uintptr_t>)(16,end-reinterpret_cast<uintptr_t>(target)));
        for(int i=0;i<readable && at>0 && at<150;++i)at+=swprintf_s(text+at,std::size(text)-at,L" %02X",bytes[i]);
        detail=text;
        return false;
    };
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Execute),&self) || self==module){detail=L"own module";return false;}
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return refused(L"MinHook initialize",init);
    const auto made=MH_CreateHook(target,reinterpret_cast<void*>(&Execute),reinterpret_cast<void**>(&gExecute));
    if(made!=MH_OK)return refused(L"MinHook create",made);
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(target),&pinned) || pinned!=module){
        MH_RemoveHook(target);gExecute=nullptr;detail=L"owner pin";return false;
    }
    if(const auto enabled=MH_EnableHook(target);enabled!=MH_OK){
        refused(L"MinHook enable",enabled);
        MH_RemoveHook(target);gExecute=nullptr;return false;
    }
    gExecuteTarget=target;
    wchar_t message[2048]{};
    swprintf_s(message,L"DLSS 5 queue submission hook: owner=%.1800ls%ls",owner.c_str(),behindOwn?L" (behind our own table hook)":L"");
    Log(message);
    return true;
}
constexpr UINT kConstants=28;
bool PipelineFor(ID3D12Device* device) noexcept {
    if(gPipeline.device)return gPipeline.device.Get()==device;
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[4]{};
    for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,kConstants};
    // The meter's float2 of the current descriptor set (u1).
    params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[3].Descriptor={1,0};
    D3D12_ROOT_SIGNATURE_DESC root{4,params};ComPtr<ID3DBlob> binary,error;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&binary,&error))
        || FAILED(device->CreateRootSignature(0,binary->GetBufferPointer(),binary->GetBufferSize(),IID_PPV_ARGS(&gPipeline.root))))return false;
    const char* const entries[]{"Encode","Decode","Copy","Meter","Hold","Carry","Compose","Fit","Solve"};
    ComPtr<ID3D12PipelineState>* const states[]{&gPipeline.encode,&gPipeline.decode,&gPipeline.copy,&gPipeline.meter,
        &gPipeline.hold,&gPipeline.carry,&gPipeline.compose,&gPipeline.fit,&gPipeline.solve};
    static_assert(std::size(entries)==std::size(states));
    for(unsigned i=0;i<std::size(entries);++i){
        const char* const source=i<4?kShader:kBoostShader;
        const size_t length=i<4?sizeof(kShader)-1:sizeof(kBoostShader)-1;
        if(FAILED(D3DCompile(source,length,"MFGUnlocker DLSS 5",nullptr,nullptr,entries[i],"cs_5_1",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&binary,&error))){
            if(error){
                wchar_t message[1024]{};
                swprintf_s(message,L"DLSS 5 shader compile failed: %.900hs",static_cast<const char*>(error->GetBufferPointer()));
                Log(message);
            }
            gPipeline.root.Reset();return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=gPipeline.root.Get();desc.CS={binary->GetBufferPointer(),binary->GetBufferSize()};
        if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(states[i]->ReleaseAndGetAddressOf())))){gPipeline.root.Reset();return false;}
    }
    gPipeline.descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gPipeline.device=device;return true;
}
bool Nvidia(ID3D12Device* device) noexcept {
#if defined(MFG_DLSS5_TESTING)
    if(gSoftwareTest)return true;
#endif
    const LUID wanted=device->GetAdapterLuid();ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(wanted,IID_PPV_ARGS(&adapter))))return false;
    DXGI_ADAPTER_DESC1 desc{};return SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId==0x10DE && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE);
}
// nrscale.1: the process's local video memory in use and its budget on the
// slot's adapter, in bytes. False when DXGI cannot tell (asked once).
bool VideoMemory(Slot& slot,uint64_t& usage,uint64_t& budget) noexcept {
#if defined(MFG_DLSS5_TESTING)
    if(gTestBudget){usage=gTestUsage;budget=gTestBudget;return true;}
#endif
    if(!slot.memoryTried){
        slot.memoryTried=true;ComPtr<IDXGIFactory4> factory;
        if(slot.device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            factory->EnumAdapterByLuid(slot.device->GetAdapterLuid(),IID_PPV_ARGS(&slot.memory));
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if(!slot.memory || FAILED(slot.memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)))return false;
    usage=local.CurrentUsage;budget=local.Budget;return true;
}
// vmemlog.1: the process's local and non-local video memory (system memory the GPU reads over PCIe), logged as "DLSS 5
// video memory (why)" at each create and release (force), and while a feature runs at most every 2 s when either moved
// by 64 MB or more, otherwise every 30 s. Diagnostics only; nothing acts on it.
void LogVideoMemory(Slot& slot,const wchar_t* why,bool force) noexcept {
#if defined(MFG_DLSS5_TESTING)
    if(gTestBudget)return;
#endif
    const ULONGLONG now=GetTickCount64();
    if(!force && slot.memoryLogTick && now-slot.memoryLogTick<2000)return;
    uint64_t usage=0,budget=0;
    if(!VideoMemory(slot,usage,budget) || !slot.memory)return;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonLocal{};
    if(FAILED(slot.memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))
        || FAILED(slot.memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonLocal)))return;
    const auto moved=[](uint64_t a,uint64_t b){return (a>b?a-b:b-a)>=(64ull<<20);};
    if(!force && slot.memoryLogTick && now-slot.memoryLogTick<30000
        && !moved(local.CurrentUsage,slot.memoryLogLocal) && !moved(nonLocal.CurrentUsage,slot.memoryLogSystem))return;
    slot.memoryLogTick=now;slot.memoryLogLocal=local.CurrentUsage;slot.memoryLogSystem=nonLocal.CurrentUsage;
    wchar_t message[240]{};
    swprintf_s(message,L"DLSS 5 video memory (%ls): local %llu MB of a %llu MB budget, non-local %llu MB of a %llu MB budget",
        why,local.CurrentUsage>>20,local.Budget>>20,nonLocal.CurrentUsage>>20,nonLocal.Budget>>20);
    Log(message);
}
std::wstring ProviderPath() {
#if defined(MFG_DLSS5_TESTING)
    if(!gProviderTestPath.empty())return gProviderTestPath;
#endif
    wchar_t path[32768]{};
    for(unsigned i=0;i<2;++i) {
        HMODULE module=nullptr;
        if(i && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ProviderPath),&module))continue;
        const DWORD length=GetModuleFileNameW(module,path,32768);if(!length || length>=32768)continue;
        std::wstring full(path,length);const auto at=full.find_last_of(L"\\/");if(at==std::wstring::npos)continue;
        full.resize(at+1);full+=L"nvngx_dlssnr.dll";
        const DWORD attributes=GetFileAttributesW(full.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY))return full;
    }
    return {};
}
// A game slot's recording is fenced by the queue hook (Execute); the Present
// stage submits and signals its own list.
void Record(Slot& slot,ID3D12GraphicsCommandList* cmd) noexcept {
    if(slot.flight.Record(reinterpret_cast<uintptr_t>(cmd))){
        slot.recording=cmd;
        for(size_t n=0;n<gSlots.size();++n)
            if(&gSlots[n]==&slot)gPendingCommands[n].store(reinterpret_cast<uintptr_t>(cmd),std::memory_order_release);
    }
    else {SlotFailure(slot,State::eEvaluateFailed,kProviderFault);}
}
bool MakeTextureOf(ID3D12Device* device,uint32_t width,uint32_t height,DXGI_FORMAT format,D3D12_RESOURCE_STATES state,
    ComPtr<ID3D12Resource>& texture) noexcept {
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,
        nullptr,IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));
}
bool MakeTexture(ID3D12Device* device,uint32_t width,uint32_t height,ComPtr<ID3D12Resource>& texture) noexcept {
    return MakeTextureOf(device,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,texture);
}
bool MakeTextures(Slot& slot,uint32_t width,uint32_t height,uint32_t passes) noexcept {
    if(!MakeTexture(slot.device.Get(),width,height,slot.encoded) || !MakeTexture(slot.device.Get(),width,height,slot.ping)
        || (passes>1 && !MakeTexture(slot.device.Get(),width,height,slot.pong)))return false;
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kSetDescriptors*kSets,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    return SUCCEEDED(slot.device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&slot.heap)));
}
// format: the view's format; UNKNOWN takes ViewFormat of the resource's.
void Srv(ID3D12Device* device,D3D12_CPU_DESCRIPTOR_HANDLE at,ID3D12Resource* resource,DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;desc.Texture2D.MipLevels=1;
    desc.Format=!resource ? DXGI_FORMAT_R16G16B16A16_FLOAT : format!=DXGI_FORMAT_UNKNOWN ? format : ViewFormat(resource->GetDesc().Format);
    device->CreateShaderResourceView(resource,&desc,at);
}
void UavView(ID3D12Device* device,D3D12_CPU_DESCRIPTOR_HANDLE at,ID3D12Resource* resource) noexcept {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};desc.Format=ViewFormat(resource->GetDesc().Format);desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource,nullptr,&desc,at);
}
// Writes one table with a view format per SRV (UNKNOWN: ViewFormat of the resource's).
void ViewsOf(ID3D12Device* device,ID3D12DescriptorHeap* heap,uint32_t first,ID3D12Resource* const (&t)[4],
    const DXGI_FORMAT (&formats)[4],ID3D12Resource* u0) noexcept {
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const auto step=gPipeline.descriptorSize;
    cpu.ptr+=size_t(first)*step;
    for(unsigned i=0;i<4;++i)Srv(device,{cpu.ptr+size_t(i)*step},t[i],formats[i]);
    UavView(device,{cpu.ptr+size_t(4)*step},u0);
}
// Writes one table (t0..t3, u0) at descriptor `first` of `heap`.
void Views(ID3D12Device* device,ID3D12DescriptorHeap* heap,uint32_t first,ID3D12Resource* t0,ID3D12Resource* t1,
    ID3D12Resource* t2,ID3D12Resource* t3,ID3D12Resource* u0,DXGI_FORMAT t0Format=DXGI_FORMAT_UNKNOWN) noexcept {
    ViewsOf(device,heap,first,{t0,t1,t2,t3},{t0Format,DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN},u0);
}
// Writes one table at descriptor `first` of the slot's current set.
void Table(Slot& slot,uint32_t first,ID3D12Resource* t0,ID3D12Resource* t1,ID3D12Resource* t2,ID3D12Resource* t3,ID3D12Resource* u0) noexcept {
    Views(slot.device.Get(),slot.heap.Get(),slot.descriptor*kSetDescriptors+first,t0,t1,t2,t3,u0);
}
// The shader's constant buffer; the Vulkan stage pushes the same layout (dlss5.h).
using Constants=shared::PassConstants;
static_assert(sizeof(Constants)==kConstants*4);
// Binds the table at descriptor `first` of `heap` and runs `kernel` over x by y.
void Run(ID3D12GraphicsCommandList* cmd,ID3D12DescriptorHeap* heap,uint32_t first,ID3D12PipelineState* kernel,
    const Constants& constants,uint32_t x,uint32_t y,D3D12_GPU_VIRTUAL_ADDRESS meter=0) noexcept {
    ID3D12DescriptorHeap* heaps[]{heap};cmd->SetDescriptorHeaps(1,heaps);
    cmd->SetComputeRootSignature(gPipeline.root.Get());cmd->SetPipelineState(kernel);
    auto gpu=heap->GetGPUDescriptorHandleForHeapStart();const auto step=gPipeline.descriptorSize;
    gpu.ptr+=size_t(first)*step;
    cmd->SetComputeRootDescriptorTable(0,{gpu.ptr});
    cmd->SetComputeRootDescriptorTable(1,{gpu.ptr+size_t(4)*step});
    cmd->SetComputeRoot32BitConstants(2,kConstants,&constants,0);
    // The meter runs as one group; the image kernels as 8x8 tiles.
    if(meter){cmd->SetComputeRootUnorderedAccessView(3,meter);cmd->Dispatch(1,1,1);}
    else cmd->Dispatch((x+7)/8,(y+7)/8,1);
}
void Dispatch(Slot& slot,ID3D12GraphicsCommandList* cmd,const Constants& constants,bool decode) noexcept {
    Run(cmd,slot.heap.Get(),slot.descriptor*kSetDescriptors+(decode?5:0),decode?gPipeline.decode.Get():gPipeline.encode.Get(),
        constants,decode?constants.width:constants.extentWidth,decode?constants.height:constants.extentHeight);
}
// Copies `rect` of `source` (viewed as `format`) to `target` at 0,0 with the
// table at descriptor `first`; a null source fills zeros. `target` is in
// UNORDERED_ACCESS.
void CopyTexels(ID3D12GraphicsCommandList* cmd,ID3D12Device* device,ID3D12DescriptorHeap* heap,uint32_t first,
    ID3D12Resource* source,DXGI_FORMAT format,ID3D12Resource* target,const Rect& rect) noexcept {
    Views(device,heap,first,source,nullptr,nullptr,nullptr,target,format);
    Constants constants{};constants.originX=static_cast<uint32_t>(rect.x);constants.originY=static_cast<uint32_t>(rect.y);
    constants.width=static_cast<uint32_t>(rect.width);constants.height=static_cast<uint32_t>(rect.height);
    Run(cmd,heap,first,gPipeline.copy.Get(),constants,constants.width,constants.height);
}
// kBoostShader's constant buffer.
struct BoostConstants {
    int32_t motionX=0,motionY=0;uint32_t motionWidth=0,motionHeight=0;
    uint32_t extentWidth=0,extentHeight=0;float motionScaleX=1.0f,motionScaleY=1.0f;
    // Detail: how far (network signal, 0..1) a texel's 3x3 luminance may move
    // from where it came from and still keep the carried detail, and where it
    // has lost all of it (BoostThresholds).
    float stableBelow=0.02f,unstableAbove=0.06f;
    // The look model's grid (dlss5_policy.h BoostGrid) and its pull toward the identity.
    uint32_t cell=0,bins=0,cellsX=0,cellsY=0;float lambda=kBoostLambda;uint32_t pad[13]{};
};
static_assert(sizeof(BoostConstants)==kConstants*4);
void Run(ID3D12GraphicsCommandList* cmd,ID3D12DescriptorHeap* heap,uint32_t first,ID3D12PipelineState* kernel,
    const BoostConstants& constants,uint32_t x,uint32_t y) noexcept {
    Constants raw{};std::memcpy(&raw,&constants,sizeof(raw));
    Run(cmd,heap,first,kernel,raw,x,y);
}
BoostConstants BoostConstantsFor(const Slot& slot,const Frame& frame,const Settings& wanted) noexcept {
    BoostConstants c{};
    const BoostThreshold threshold=BoostThresholds(wanted.boostTolerance);
    c.stableBelow=threshold.stableBelow;c.unstableAbove=threshold.unstableAbove;
    c.motionX=frame.motionRect.x;c.motionY=frame.motionRect.y;
    c.motionWidth=static_cast<uint32_t>(frame.motionRect.width);c.motionHeight=static_cast<uint32_t>(frame.motionRect.height);
    c.extentWidth=common::ShownWidth(slot);c.extentHeight=common::ShownHeight(slot);
    c.motionScaleX=frame.motionScaleX;c.motionScaleY=frame.motionScaleY;
    c.cell=slot.boostGrid.cell;c.bins=slot.boostGrid.bins;c.cellsX=slot.boostGrid.cellsX;c.cellsY=slot.boostGrid.cellsY;
    return c;
}
// Boost: this evaluation runs the network, or Carry stands in for it after a
// network evaluation. Off without a shader view of the game's motion vectors,
// or while a texture of the wrong size could still be in use; each reason is
// logged once per slot.
enum class BoostStep { eOff, eNetwork, eCarry };
BoostStep BoostFor(Slot& slot,const Frame& frame,const Settings& wanted,DXGI_FORMAT& motionView) noexcept {
    const auto off=[&slot](const wchar_t* reason){
        slot.boostHeld=slot.boostSkipped=false;
        if(reason && !slot.boostLogged){
            wchar_t message[200]{};swprintf_s(message,L"DLSS 5 Boost unavailable: %ls",reason);Log(message);slot.boostLogged=true;
        }
        return BoostStep::eOff;
    };
    // The Present stage has captured or neutral inputs and its own use of descriptors 10-19.
    if(!wanted.boost || wanted.stage==static_cast<uint32_t>(Stage::ePresent))return off(nullptr);
    // ValidateResources already checked the motion vectors on this device.
    const D3D12_RESOURCE_DESC motion=reinterpret_cast<ID3D12Resource*>(frame.motion)->GetDesc();
    motionView=ViewFormat(motion.Format);
    if(MotionCaptureFormat(motionView)==DXGI_FORMAT_UNKNOWN)return off(L"the game's motion vectors have no shader view format");
    // autorect.1: the look model's grid follows the network's rect; under Resolution
    // Scale Auto its textures hold every level's grid, so an Auto step keeps them.
    BoostGrid grid{};
    if(!BoostGridFor(common::ShownWidth(slot),common::ShownHeight(slot),grid))return off(L"the extent has no look model grid");
    BoostGrid sizes=grid,levels{};
    if(wanted.scaleAuto && auto_mode::BoostGridCapacity(slot.width,slot.height,auto_mode::CeilingLevel(slot.applied.scale),levels)){
        sizes.cellsX=std::max(sizes.cellsX,levels.cellsX);sizes.cellsY=std::max(sizes.cellsY,levels.cellsY);
    }
    // The motion textures take the resource's size, so a moving render subrect keeps them.
    const uint32_t motionWidth=static_cast<uint32_t>(motion.Width),motionHeight=motion.Height;
    const auto sized=[](const ComPtr<ID3D12Resource>& texture,uint32_t w,uint32_t h){
        const auto desc=texture->GetDesc();return desc.Width==w && desc.Height==h;
    };
    const auto reset=[&slot]{
        slot.boostChange.Reset();slot.boostSums.Reset();slot.boostModel.Reset();slot.boostMotion.Reset();slot.boostTwoFrame.Reset();
    };
    const bool stale=(slot.boostChange && !sized(slot.boostChange,slot.extentWidth,slot.extentHeight))
        || (slot.boostSums && !sized(slot.boostSums,sizes.cellsX*6,sizes.cellsY*sizes.bins))
        || (slot.boostModel && !sized(slot.boostModel,sizes.cellsX*3,sizes.cellsY*sizes.bins))
        || (slot.boostMotion && !sized(slot.boostMotion,motionWidth,motionHeight))
        || (slot.boostTwoFrame && !sized(slot.boostTwoFrame,motionWidth,motionHeight));
    if(stale){
        if(!Complete(slot))return off(nullptr);
        reset();
    }
    if(!slot.boostChange){
        slot.boostHeld=slot.boostSkipped=false;
        constexpr auto kSums=DXGI_FORMAT_R32G32B32A32_FLOAT;constexpr auto kUav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(!MakeTexture(slot.device.Get(),slot.extentWidth,slot.extentHeight,slot.boostChange)
            || !MakeTextureOf(slot.device.Get(),sizes.cellsX*6,sizes.cellsY*sizes.bins,kSums,kUav,slot.boostSums)
            || !MakeTextureOf(slot.device.Get(),sizes.cellsX*3,sizes.cellsY*sizes.bins,kSums,kUav,slot.boostModel)
            || !MakeTextureOf(slot.device.Get(),motionWidth,motionHeight,DXGI_FORMAT_R32G32_FLOAT,kUav,slot.boostMotion)
            || !MakeTextureOf(slot.device.Get(),motionWidth,motionHeight,DXGI_FORMAT_R32G32_FLOAT,kUav,slot.boostTwoFrame)){
            reset();
            return off(L"its textures could not be created");
        }
        wchar_t message[260]{};
        swprintf_s(message,L"DLSS 5 Boost: the network runs on every other evaluation; the frames between rebuild its look from their own image (extent %ux%u, motion %ux%u view %u, model %ux%u cells of %u, %u bins)",
            common::ShownWidth(slot),common::ShownHeight(slot),motionWidth,motionHeight,static_cast<unsigned>(motionView),grid.cellsX,grid.cellsY,grid.cell,grid.bins);
        Log(message);
    }
    // A rect that moved restarted the history (Activate), so this evaluation runs the network on the new grid.
    slot.boostGrid=grid;
    return slot.boostHeld && !slot.boostSkipped && !frame.reset ? BoostStep::eCarry : BoostStep::eNetwork;
}
// GPU timestamps and the luminance meter for each descriptor set. Without
// them Auto keeps its starting Resolution Scale and a unit exposure gain.
bool MeasurementsFor(Slot& slot) noexcept {
    if(slot.measurements)return true;
    if(!slot.device || slot.measurementsTried)return false;
    slot.measurementsTried=true;
    D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,kSets*2,0};
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT},cpu{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC meter=buffer;meter.Width=kSets*sizeof(float)*2;meter.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC readback=buffer;readback.Width=kSets*kMeasurementBytes;
    void* mapped=nullptr;D3D12_RANGE read{0,kSets*kMeasurementBytes};
    if(FAILED(slot.device->CreateQueryHeap(&queries,IID_PPV_ARGS(&slot.timing)))
        || FAILED(slot.device->CreateCommittedResource(&gpu,D3D12_HEAP_FLAG_NONE,&meter,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&slot.meter)))
        || FAILED(slot.device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&readback,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot.readback)))
        || FAILED(slot.readback->Map(0,&read,&mapped)) || !mapped){
        slot.timing.Reset();slot.meter.Reset();slot.readback.Reset();
        Log(L"DLSS 5 Auto measurements unavailable: Resolution Scale and exposure keep their starting values");
        return false;
    }
    slot.measurements=static_cast<const uint8_t*>(mapped);return true;
}
// What a live feature should have been created with for `wanted` (no seeding).
// autorect.1: Resolution Scale Auto's feature is its capacity, whatever its level.
Settings Expected(const Slot&,Settings wanted) noexcept {
    if(wanted.stage==static_cast<uint32_t>(Stage::eAuto))wanted.stage=static_cast<uint32_t>(auto_mode::AutoStage());
    if(wanted.scaleAuto)wanted.scale=auto_mode::CapacityScale(wanted.scale);
    return wanted;
}
#if defined(MFG_DLSS5_TESTING)
// testing::SetAutoLevel: Auto's level as a case pinned it, whatever the controller measured.
void PinAutoLevel(Slot& slot,const Settings& wanted) noexcept {
    const size_t pinned=gTestAutoLevel.load();
    if(pinned!=SIZE_MAX && wanted.scaleAuto && (!slot.scalingSeeded || slot.scaling.level!=pinned)){
        slot.scaling.Reset(pinned);slot.scalingSeeded=true;
    }
}
#endif
Settings EffectiveFor(Slot& slot,const Settings& wanted,Stage stage,uint32_t width,uint32_t height) noexcept {
#if defined(MFG_DLSS5_TESTING)
    PinAutoLevel(slot,wanted);
#endif
    return common::Effective(slot,wanted,stage,width,height);
}
// autorect.1: after Harvest, so that an Auto step applies to this evaluation.
void ActivateFor(Slot& slot,const Settings& wanted,const Settings& run) noexcept {
#if defined(MFG_DLSS5_TESTING)
    PinAutoLevel(slot,wanted);
#endif
    common::Activate(slot,slot,wanted,run);
}
void LogCost(const wchar_t* api,shared::CostLog& log,uint32_t stage,uint32_t width,uint32_t height,float gpuMs,double frameMs,
    uint64_t evaluations) noexcept {
    if(gpuMs<0.0f)return;
    const ULONGLONG now=GetTickCount64();
    const bool first=log.ms<0.0f,moved=!first && std::fabs(gpuMs-log.ms)>0.25f*log.ms;
    if(!first && !(moved && now-log.tick>=1000) && now-log.tick<30000)return;
    log.ms=gpuMs;log.tick=now;
    wchar_t message[300]{};
    swprintf_s(message,L"DLSS 5%ls cost: stage=%u extent=%ux%u gpu=%.2f ms frame=%.2f ms (%.0f%% of each rendered frame) evaluations=%llu",
        api,stage,width,height,gpuMs,frameMs,frameMs>0.0?100.0*gpuMs/frameMs:0.0,static_cast<unsigned long long>(evaluations));
    Log(message);
}
// Measurements whose descriptor sets completed feed Auto: the mean GPU time of
// the stage and the pooled luminance, over the time since the previous feed.
void Harvest(Slot& slot,const Settings& wanted,uint64_t completed) noexcept {
    if(!slot.measurements)return;
    if(!slot.timestampFrequency && slot.lastQueue){
        UINT64 frequency=0;
        if(SUCCEEDED(slot.lastQueue->GetTimestampFrequency(&frequency)))slot.timestampFrequency=frequency;
    }
    double ms=0.0;uint32_t timed=0,paired=0;float sum=0.0f,samples=0.0f;
    bool otherLevel=false;
    for(size_t n=0;n<kSets;++n){
        if(!slot.measured[n] || slot.descriptorFence[n]>completed)continue;
        slot.measured[n]=false;
        uint64_t stamps[2]{};float meter[2]{};
        std::memcpy(stamps,slot.measurements+n*kMeasurementBytes,sizeof(stamps));
        std::memcpy(meter,slot.measurements+n*kMeasurementBytes+16,sizeof(meter));
        if(slot.timestampFrequency && stamps[1]>stamps[0]){
            otherLevel|=slot.measuredScale[n]!=slot.scaling.Scale();
            const float stage=static_cast<float>(double(stamps[1]-stamps[0])*1000.0/double(slot.timestampFrequency));
            if(slot.measuredStep[n]==1){slot.boostNetworkMs=stage;++paired;}
            else if(slot.measuredStep[n]==2){slot.boostCarryMs=stage;++paired;}
            else{ms+=stage;++timed;}
        }
        if(std::isfinite(meter[0]) && std::isfinite(meter[1]) && meter[1]>0.0f){sum+=meter[0];samples+=meter[1];}
    }
    if(!timed && !paired && samples<=0.0f)return;
    const double seconds=slot.frameSeconds;slot.frameSeconds=0.0;
    if(timed || paired){
        // Boost: the mean of the latest network and Carry evaluations (one of
        // them alone until both were measured); otherwise this feed's mean.
        if(timed){slot.gpuMs=static_cast<float>(ms/timed);slot.boostNetworkMs=slot.boostCarryMs=-1.0f;}
        else if(slot.boostNetworkMs>=0.0f && slot.boostCarryMs>=0.0f)slot.gpuMs=(slot.boostNetworkMs+slot.boostCarryMs)*0.5f;
        else slot.gpuMs=std::max(slot.boostNetworkMs,slot.boostCarryMs);
        LogCost(L"",slot.cost,slot.applied.stage,common::ShownWidth(slot),common::ShownHeight(slot),slot.gpuMs,slot.clock.averageMs,gEvaluations);
        LogVideoMemory(slot,L"running",false);
        common::StepScale(slot,wanted,seconds,otherLevel,Log,
            L"DLSS 5 Auto resolution scale: %.0f%% -> %.0f%% (stage %.2f ms, budget %.2f ms), within the feature",
            L"DLSS 5 Auto resolution scale holds its %.0f%% quality floor (stage %.2f ms, budget %.2f ms)");
    }
    if(samples>0.0f && wanted.adaptiveExposure)slot.exposure.Update(sum/samples,seconds,wanted.adaptiveTarget);
}
// Device, queue hook, pipeline and fence for this slot (once per source).
bool Setup(Slot& slot,const Api& api,ID3D12GraphicsCommandList* cmd) noexcept {
    if(slot.device && slot.api.Complete()){
        if(slot.api.runtime!=api.runtime || slot.api.create!=api.create || slot.api.evaluate!=api.evaluate || slot.api.release!=api.release){
            SlotFailure(slot,State::eUnsupported,NVSDK_NGX_Result_FAIL_InvalidParameter);return false;
        }
        return true;
    }
    const wchar_t* step=nullptr;std::wstring owner,detail;
    if(!api.Complete())step=L"NGX entry points incomplete";
    else if(FAILED(cmd->GetDevice(IID_PPV_ARGS(&slot.device))))step=L"command list device";
    else if(!Nvidia(slot.device.Get()))step=L"adapter is not NVIDIA";
    else if(cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)step=L"command list is not DIRECT";
    else if(slot.device->GetNodeCount()!=1)step=L"multi-node device";
    else if(!PrepareQueue(slot.device.Get(),owner,detail))step=L"queue submission hook";
    else if(!PipelineFor(slot.device.Get()))step=L"compute pipeline";
    else if(FAILED(slot.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&slot.fence))))step=L"fence";
    if(step){
        wchar_t message[2048]{};
        swprintf_s(message,L"DLSS 5 setup refused: step=%ls%ls%ls%ls%.1700ls",step,detail.empty()?L"":L" (",detail.c_str(),
            detail.empty()?L"":L")",owner.empty()?L"":(L" owner="+owner).c_str());
        Log(message);
        SlotFailure(slot,State::eUnsupported,NVSDK_NGX_Result_FAIL_FeatureNotSupported);return false;
    }
    slot.api=api;return true;
}
// Creates the feature's passes on `cmd`. The first evaluation waits for this
// submission's fence. The log gives the time the provider's create calls
// took on the calling (the game's render) thread.
void CreateFeature(Slot& slot,ID3D12GraphicsCommandList* cmd,const Frame& frame,const Settings& wanted,uint32_t extentWidth,uint32_t extentHeight,
    const wchar_t* waiting=L"waiting for queue fence") noexcept {
    if(slot.params){if(slot.api.destroy(slot.params)!=NVSDK_NGX_Result_Success){slot.cleanupFailed=true;SlotFailure(slot,State::eCreateFailed,NVSDK_NGX_Result_FAIL_InvalidParameter);return;}slot.params=nullptr;}
    const std::wstring path=ProviderPath();
    if(!slot.nativeAttempted){
        if(!gProviderChecked){
            gProviderChecked=true;
            const HMODULE loaded=GetModuleHandleW(L"nvngx_dlssnr.dll");
            gProviderForeign=loaded && !gDirect.Module();
            if(gProviderForeign)common::LogProviderLoaded(Log,L"",loaded,path,gNativeAttempts);
        }
        slot.directOwnershipDenied=gProviderForeign;
    }
    if(slot.nativeAttempted && !slot.nativeSucceeded && slot.directOwnershipDenied){
        if(!slot.ownershipLogged){
            slot.ownershipLogged=true;
            wchar_t message[200]{};
            swprintf_s(message,L"DLSS 5 direct provider refused: nvngx_dlssnr.dll is held by another loader; native NGX create returned 0x%08X",
                slot.nativeResult);
            Log(message);
        }
        gState=State::eOtherMod;return;
    }
    // dx11nr.1: the DirectX 11 bridge stays off the direct provider once native NGX reported an outdated driver.
    if(slot.nativeAttempted && !slot.nativeSucceeded && !d3d11::DirectFallbackAllowed(slot.route,slot.nativeResult)){
        SlotFailure(slot,State::eDriverUpdate,slot.nativeResult);return;
    }
    // nrscale.1: the video memory before the create (the log, and RoomFor's estimate of the next replacement).
    uint64_t usageBefore=0,budget=0;
    const bool measured=VideoMemory(slot,usageBefore,budget);
    if(slot.api.allocate(&slot.params)!=NVSDK_NGX_Result_Success || !slot.params
        || !WriteCreate(slot.params,extentWidth,extentHeight,wanted)) {
        SlotFailure(slot,State::eCreateFailed,NVSDK_NGX_Result_FAIL_InvalidParameter);return;
    }
    NVSDK_NGX_Result result=NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const wchar_t* directStep=L"";
    bool route=true;
    if(!slot.nativeAttempted || slot.nativeSucceeded){slot.nativeAttempted=true;slot.direct=false;++gNativeAttempts;}
    else {
        slot.direct=true;route=false;
        if(!path.empty() && (!gDirectRuntime || gDirectRuntime==slot.api.runtime) && gDirect.Open(path.c_str(),slot.api.runtime)){
            common::LogDirectIdentity(Log,L"",gDirect.Module(),path);
            gDirectRuntime=slot.api.runtime;result=gDirect.Initialize(slot.device.Get(),slot.params);directStep=L"initialize";
            if(result==NVSDK_NGX_Result_Success){route=true;directStep=L"create";}
        }else if(!path.empty()){result=NVSDK_NGX_Result_FAIL_PlatformError;directStep=L"open";}
    }
    LARGE_INTEGER frequency{},start{},stop{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(uint32_t pass=0;route && pass<wanted.passes;++pass){
        NVSDK_NGX_Handle* handle=nullptr;
        // Each pass is created with its own Model and strengths (PassSettings).
        if(!WritePassControls(slot.params,wanted,pass)){result=NVSDK_NGX_Result_FAIL_InvalidParameter;break;}
        result=slot.direct ? gDirect.Create(cmd,slot.params,&handle)
            : slot.api.create(cmd,static_cast<NVSDK_NGX_Feature>(kFeature),slot.params,&handle);
        if(handle){slot.handles[slot.passes++]=handle;}
        if(result!=NVSDK_NGX_Result_Success || !handle)break;
    }
    QueryPerformanceCounter(&stop);
    const double createMs=frequency.QuadPart?double(stop.QuadPart-start.QuadPart)*1000.0/double(frequency.QuadPart):0.0;
    Record(slot,cmd);gResult=static_cast<uint32_t>(result);
    if(!slot.direct)slot.nativeResult=gResult;
    if(result!=NVSDK_NGX_Result_Success || slot.passes!=wanted.passes){
        wchar_t message[2048]{};
        const bool fallback=d3d11::DirectFallbackAllowed(slot.route,gResult);
        if(slot.direct)swprintf_s(message,L"DLSS 5 direct provider %ls failed: result=0x%08X passes=%u/%u createMs=%.1f provider=%.1700ls",
            directStep,gResult,slot.passes,wanted.passes,createMs,path.c_str());
        else swprintf_s(message,L"DLSS 5 native NGX create returned 0x%08X%ls%ls (createMs=%.1f)",gResult,
            path.empty()?L"; no provider beside the game for the direct route"
                :fallback?L"; the next evaluation uses the direct provider ":L"; the driver is out of date, so the DirectX 11 bridge does not use the direct provider ",
            path.substr(0,1700).c_str(),createMs);
        Log(message);
        if(slot.passes){SlotFailure(slot,State::eCreateFailed,result==NVSDK_NGX_Result_Success?NVSDK_NGX_Result_FAIL_OutOfGPUMemory:gResult);return;}
        if(slot.direct || path.empty() || !fallback){SlotFailure(slot,CreateFailureState(gResult),gResult);}
        else gState=State::eWaitingForDlss;
        return;
    }
    if(!slot.direct)slot.nativeSucceeded=true;
    slot.applied=wanted;slot.width=frame.width;slot.height=frame.height;slot.extentWidth=extentWidth;slot.extentHeight=extentHeight;
    slot.createdReady=false;gCreates+=slot.passes;
    if(!MakeTextures(slot,extentWidth,extentHeight,slot.passes)){SlotFailure(slot,State::eCreateFailed,NVSDK_NGX_Result_FAIL_OutOfGPUMemory);return;}
    gState=State::eWaitingForDlss;
    // nrscale.1: what the create took, kept while it is measurable (a create
    // in place may start before the old feature's memory came back).
    uint64_t usageAfter=0;
    wchar_t memory[160]{};
    if(measured && VideoMemory(slot,usageAfter,budget)){
        if(usageAfter>usageBefore){slot.createBytes=usageAfter-usageBefore;slot.createWidth=extentWidth;slot.createHeight=extentHeight;}
        swprintf_s(memory,L"; video memory %llu -> %llu MB of a %llu MB budget",usageBefore>>20,usageAfter>>20,budget>>20);
    }
    slot.inPlace=false;
    wchar_t message[2048]{};
    swprintf_s(message,L"DLSS 5 feature created: route=%ls stage=%u size=%ux%u extent=%ux%u passes=%u preset=%u model=%u later=%ls%u create=%.1f ms provider=%.1400ls%ls; %.200ls",
        slot.direct?L"direct":L"native-ngx",wanted.stage,slot.width,slot.height,extentWidth,extentHeight,slot.passes,wanted.preset,wanted.model,
        wanted.laterCustom?L"custom/":L"same/",LaterModel(wanted),createMs,path.empty()?L"NGX-managed":path.c_str(),memory,waiting);
    Log(message);
    LogVideoMemory(slot,L"create",true);
}
// nrscale.1: whether a replacement at this extent fits beside the live feature (else it replaces it in place).
bool RoomFor(Slot& slot,uint32_t extentWidth,uint32_t extentHeight) noexcept {
    if(slot.inPlace)return false;
    uint64_t usage=0,budget=0;
    if(!VideoMemory(slot,usage,budget))return true;
    const uint64_t bytes=ReplacementBytes(slot.createBytes,slot.createWidth,slot.createHeight,extentWidth,extentHeight);
    if(RoomBeside(usage,budget,bytes))return true;
    slot.inPlace=true;
    wchar_t message[320]{};
    swprintf_s(message,L"DLSS 5 replaces its feature in place: %llu MB of video memory in use and about %llu MB for a create at %ux%u exceed the %llu MB budget; "
        L"frames go unprocessed until the new create executed",usage>>20,bytes>>20,extentWidth,extentHeight,budget>>20);
    Log(message);
    return false;
}
// CreateFeature, with the Present stage's neutral inputs at the extent.
void Create(Slot& slot,ID3D12GraphicsCommandList* cmd,const Frame& frame,const Settings& run,uint32_t extentWidth,uint32_t extentHeight,
    bool present,const wchar_t* waiting=L"waiting for queue fence") noexcept {
    CreateFeature(slot,cmd,frame,run,extentWidth,extentHeight,waiting);
    if(present && slot.HasFeature() && !slot.failed
        && (!MakeTextureOf(slot.device.Get(),extentWidth,extentHeight,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,slot.neutralDepth)
            || !MakeTextureOf(slot.device.Get(),extentWidth,extentHeight,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,slot.neutralMotion)))
        SlotFailure(slot,State::eCreateFailed,NVSDK_NGX_Result_FAIL_OutOfGPUMemory);
}
// The live feature for `run` at this extent, on `cmd`: kept, created or
// replaced. True when a feature evaluates on `cmd` now; `run` is then what it
// evaluates with.
// A change of Resolution Scale (Auto's steps too) or of the
// feature's settings creates the replacement beside the live feature, which
// keeps evaluating with what it was created with. Once the replacement's
// create executed (its fence) it takes over with fresh history, and the old
// feature is released once every list that used it executed, so no frame goes
// unprocessed. The feature is replaced in place (once its work completed,
// frames unprocessed until the new create executed) when it is not ready yet,
// the stage or its size changed, no replacement could be created beside it
// (then for the rest of the slot's life), or (nrscale.1) a replacement has no
// room beside it in the video memory budget (RoomFor).
bool Settle(Slot& slot,ID3D12GraphicsCommandList* cmd,const Frame& frame,Settings& run,uint32_t extentWidth,uint32_t extentHeight,
    bool present) noexcept {
    Collect(slot);
    const auto differs=[&](const Feature& feature){
        return feature.width!=frame.width || feature.height!=frame.height || feature.extentWidth!=extentWidth
            || feature.extentHeight!=extentHeight || !SameFeature(feature.applied,run);
    };
    if(slot.HasFeature() && !differs(slot)){
        // The replacement of a change that was undone goes.
        if(slot.next.Holds() && Retire(slot,slot.next))slot.nextReady=0;
        slot.inPlace=false;
        return true;
    }
    const bool beside=slot.HasFeature() && !slot.replaceRefused && slot.createdReady && slot.applied.stage==run.stage
        && slot.width==frame.width && slot.height==frame.height && (slot.next.Holds() || RoomFor(slot,extentWidth,extentHeight));
    if(!beside){
        if(Outstanding(slot)){
            if(!Complete(slot))return false;
            Destroy(slot);if(Outstanding(slot))return false;
        }
        if(!Complete(slot))return false;
        Create(slot,cmd,frame,run,extentWidth,extentHeight,present);
        return false;
    }
    const Settings live=Keeping(run,slot.applied);
    // A replacement for settings that changed again since goes first.
    if(slot.next.Holds() && differs(slot.next)){
        if(!Retire(slot,slot.next)){run=live;return true;}
        slot.nextReady=0;
    }
    if(!slot.next.Holds()){
        std::swap(static_cast<Feature&>(slot),slot.next);
        Create(slot,cmd,frame,run,extentWidth,extentHeight,present,L"the live feature evaluates until this queue fence");
        const bool made=!slot.failed && slot.passes==run.passes && slot.heap;
        std::swap(static_cast<Feature&>(slot),slot.next);
        slot.nextReady=Recorded(slot);
        if(!made){
            // The live feature stays; Destroy takes what was made with it.
            slot.failed=false;slot.replaceRefused=true;
            Log(L"DLSS 5 could not create a replacement beside the live feature; its changes replace it in place from now on");
        }
        run=live;return true;
    }
    // The live feature moves to the retired ones (a free entry first).
    if(!Passed(slot,slot.nextReady) || !Retire(slot,slot)){run=live;return true;}
    static_cast<Feature&>(slot)=std::move(slot.next);slot.next=Feature{};slot.nextReady=0;
    slot.createdReady=true;slot.resetHistory=true;
    wchar_t message[200]{};
    swprintf_s(message,L"DLSS 5 feature replaced without a gap: extent=%ux%u passes=%u model=%u; the previous one is released once its work completed",
        slot.extentWidth,slot.extentHeight,slot.passes,slot.applied.model);
    Log(message);
    return true;
}
// One immutable descriptor set per live recording, recycled only after
// its exact queue fence. Textures execute in queue order; cross-queue
// submissions wait for the preceding feature fence in Execute().
bool ReserveDescriptors(Slot& slot,const Settings& wanted) noexcept {
    const uint64_t completed=slot.fence->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    Harvest(slot,wanted,completed);
    size_t available=slot.descriptorFence.size();
    for(size_t n=0;n<slot.descriptorFence.size();++n)
        if(slot.descriptorFence[n]<=completed){available=n;break;}
    if(available==slot.descriptorFence.size()){slot.resetHistory=true;return false;}
    slot.descriptor=static_cast<uint32_t>(available);
    slot.descriptorFence[available]=slot.flight.Next();
    return true;
}
// A failed slot retries only after a settings change, once its work and
// cleanup completed. False while it stays failed; with `publish`, its failure
// is the state meanwhile.
bool Recovered(Slot& slot,bool publish=true) noexcept {
    if(!slot.failed)return true;
    if(publish){gState=slot.failureState;gResult=slot.failureResult;}
    if(slot.revision==slot.failureRevision || slot.cleanupFailed || slot.flight.failed || (slot.direct && gDirect.Faulted()) || !Complete(slot))return false;
    Destroy(slot);if(Outstanding(slot))return false;
    slot.failed=false;return true;
}
// Readies the slot on `cmd`: setup, failure retry, inputs, feature (re)creation
// and a free descriptor set. True when the feature evaluates on `cmd` now.
bool Ready(Slot& slot,const Api& api,ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Parameter* params,
    const Settings& wanted,uint64_t revision,Stage stage,Frame& frame,D3D12_RESOURCE_DESC& color,Settings& run) noexcept {
    slot.revision=revision;
    common::Tick(slot);
    if(gForeign){gState=State::eOtherMod;return false;}
    if(!Recovered(slot))return false;
    if(!slot.flight.CanRecord(reinterpret_cast<uintptr_t>(cmd)))return false;
    InputsWhy why{};Source source{};
    if(!ReadFrame(params,slot.keys,stage,frame,why,source)){
        InputsUnavailable(L"",why,source,slot.keys);slot.resetHistory=true;gState=State::eMissingInputs;return false;
    }
    if(!Setup(slot,api,cmd))return false;
    MeasurementsFor(slot);
    if(!ValidateResources(slot,frame,stage,color)){gState=State::eMissingInputs;return false;}
    run=EffectiveFor(slot,wanted,stage,frame.width,frame.height);
    uint32_t extentWidth=0,extentHeight=0;
    if(!ProcessingExtent(frame.width,frame.height,run.scale,extentWidth,extentHeight)){
        wchar_t reason[160]{};swprintf_s(reason,L"the stage extent %ux%u is beyond the %u texture limit",frame.width,frame.height,kMaxExtent);
        InputsUnavailable(reason);gState=State::eMissingInputs;return false;
    }
    if(!Settle(slot,cmd,frame,run,extentWidth,extentHeight,false))return false;
    if(!slot.createdReady){if(!Complete(slot))return false;slot.createdReady=true;slot.resetHistory=true;}
    if(!ReserveDescriptors(slot,wanted))return false;
    ActivateFor(slot,wanted,run);
    return true;
}
// dx11nr.1 (shared::CanEvaluate): whether Ready could let the slot work at `stage` now, without recording or retrying;
// a refusal publishes its state as Ready does.
bool Possible(Slot& slot,const NVSDK_NGX_Parameter* params,Stage stage) noexcept {
    uint64_t revision=0;Wanted(revision);
    if(slot.failed && (revision==slot.failureRevision || slot.cleanupFailed || slot.flight.failed || (slot.direct && gDirect.Faulted())
            || !Complete(slot))){
        gState=slot.failureState;gResult=slot.failureResult;return false;
    }
    if(slot.nativeAttempted && !slot.nativeSucceeded && slot.directOwnershipDenied){gState=State::eOtherMod;return false;}
    if(!params)return true;
    InputsWhy why{};Source source{};Frame frame{};
    if(!ReadFrame(params,slot.keys,stage,frame,why,source)){
        InputsUnavailable(L"",why,source,slot.keys);slot.resetHistory=true;gState=State::eMissingInputs;return false;
    }
    D3D12_RESOURCE_DESC color{};
    if(slot.device && !ValidateResources(slot,frame,stage,color)){gState=State::eMissingInputs;return false;}
    return true;
}
Constants PassConstantsFor(const Frame& frame,const Settings& wanted,uint32_t extentWidth,uint32_t extentHeight,Encoding automatic,
    bool unbounded,float adaptiveGain,bool decode,bool inPlace) noexcept {
    Constants c{};
    c.originX=static_cast<uint32_t>(frame.colorRect.x);c.originY=static_cast<uint32_t>(frame.colorRect.y);
    c.width=frame.width;c.height=frame.height;c.extentWidth=extentWidth;c.extentHeight=extentHeight;
    const auto encoding=ResolvedEncoding(wanted,automatic);
    c.encoding=static_cast<uint32_t>(encoding);c.signal=wanted.signal;c.domain=wanted.domain;
    c.filter=decode?wanted.upsample:wanted.downsample;c.reconstruction=wanted.reconstruction;
    c.flags=(unbounded?1u:0u)|(inPlace?2u:0u)|(frame.exposure?4u:0u)|((wanted.exposureMode&3u)<<3);
    c.whiteScale=WhiteScale(wanted,automatic);c.exposure=wanted.exposure;c.preExposure=frame.preExposure;
    c.diffuseWhite=DiffuseWhite(wanted,automatic);
    c.adaptiveGain=adaptiveGain;
    // The HDR bridge's knee: Encode and Decode must use the same one.
    c.knee=wanted.knee;
    if(decode){
        // Blend, halos and the debug view; flag 256: Supersampling Filter Area.
        c.blendStrength=wanted.blend;c.colorStrength=wanted.color;
        c.highlightLimit=wanted.highlightGuard?wanted.highlightLimit:0.0f;
        c.haloDark=wanted.haloDark;c.haloBright=wanted.haloBright;c.haloRadius=static_cast<float>(wanted.haloRadius);
        c.debugView=wanted.debugView;
        if(wanted.supersample==static_cast<uint32_t>(SupersampleFilter::eArea))c.flags|=256u;
        // Compare: the original left of `split` (flag 32; right of it with Swap
        // sides, flag 128), everywhere while the compare key is held; Split view
        // draws its divider (flag 64), except Before upscaling, where the
        // upscaler would take it for image content.
        const bool held=wanted.compareKey && PhysicalKeyDown(static_cast<int>(wanted.compareKey)) && ProcessInForeground();
        if(held){c.flags|=32u;c.split=static_cast<float>(frame.width);}
        else if(wanted.compare){
            c.flags|=32u|(wanted.compareSwap?128u:0u);c.split=std::round(static_cast<float>(frame.width)*wanted.compareSplit/100.0f);
            // The overlay's divider over the finished image replaces this one.
            const bool overlay=GetTickCount64()-gOverlayDividerTick.load(std::memory_order_acquire)<500;
            if(!overlay && wanted.stage!=static_cast<uint32_t>(Stage::eRender))c.flags|=64u;
        }
        gCompareShown.store(held?1.0f:wanted.compare?wanted.compareSplit/100.0f:0.0f,std::memory_order_relaxed);
        gCompareHeld.store(held,std::memory_order_relaxed);
        gCompareSwapped.store(!held && wanted.compare && wanted.compareSwap,std::memory_order_relaxed);
        gCompareTick.store(GetTickCount64(),std::memory_order_release);
    }
    return c;
}
// autorect.1: the network's rect (Activate) is the shaders' extent; the
// textures keep the processing extent, and every kernel stays within the rect.
Constants MakeConstants(const Slot& slot,const Frame& frame,const Settings& wanted,bool decode,bool inPlace) noexcept {
    return PassConstantsFor(frame,wanted,common::ShownWidth(slot),common::ShownHeight(slot),slot.automatic,common::Unbounded(slot,wanted),
        common::AdaptiveGain(slot,wanted),decode,inPlace);
}
// Encode, the network passes and decode into `destination`. The stage color
// is read in NON_PIXEL_SHADER_RESOURCE; the network textures rest in UNORDERED_ACCESS.
// Upscaled: destination is the color itself (in place, its UNORDERED_ACCESS
// state restored). Render: destination is our substitute, color stays as is.
bool RecordNeural(Slot& slot,ID3D12GraphicsCommandList* cmd,Frame frame,const Settings& wanted,ID3D12Resource* destination) noexcept {
    auto* color=reinterpret_cast<ID3D12Resource*>(frame.color);
    auto* exposure=reinterpret_cast<ID3D12Resource*>(frame.exposure);
    const bool inPlace=destination==color;
    ID3D12Resource* outputs[2]{slot.ping.Get(),slot.pong.Get()};
    ID3D12Resource* final=outputs[(slot.passes-1)%2];
    Table(slot,0,color,nullptr,exposure,nullptr,slot.encoded.Get());
    Table(slot,5,slot.encoded.Get(),final,exposure,inPlace?nullptr:color,destination);
    if(slot.resetHistory)frame.reset=1;
    DXGI_FORMAT motionView=DXGI_FORMAT_UNKNOWN;
    const BoostStep boost=BoostFor(slot,frame,wanted,motionView);
    auto* motion=reinterpret_cast<ID3D12Resource*>(frame.motion);
    const BoostConstants boosting=boost==BoostStep::eOff?BoostConstants{}:BoostConstantsFor(slot,frame,wanted);
    const uint32_t set=slot.descriptor;
    const bool measuring=slot.measurements && slot.timing && slot.meter;
    if(measuring)cmd->EndQuery(slot.timing.Get(),D3D12_QUERY_TYPE_TIMESTAMP,set*2);
    if(inPlace)TransitionMip0(cmd,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const Constants encode=MakeConstants(slot,frame,wanted,false,inPlace);
    Dispatch(slot,cmd,encode,false);
    if(measuring){
        // The encode table (t0: the stage color) and this set's meter float2.
        Run(cmd,slot.heap.Get(),set*kSetDescriptors,gPipeline.meter.Get(),encode,1,1,
            slot.meter->GetGPUVirtualAddress()+uint64_t(set)*sizeof(float)*2);
    }
    Transition(cmd,slot.encoded.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    NVSDK_NGX_Result result=NVSDK_NGX_Result_Success;
    // Pass n writes ping/pong n%2 (UNORDERED_ACCESS) and pass n+1 reads it
    // (NON_PIXEL_SHADER_RESOURCE); readable[] is each texture's current state.
    bool readable[2]{};
    // Boost tables: descriptors 10, 15, 20 and 25 of this set, free outside the
    // Present stage (where Boost is off). Its textures rest in UNORDERED_ACCESS.
    constexpr auto kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;constexpr auto kWrite=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const uint32_t boostFirst=set*kSetDescriptors+10;
    constexpr DXGI_FORMAT kOwn=DXGI_FORMAT_UNKNOWN;
    const uint32_t activeWidth=common::ShownWidth(slot),activeHeight=common::ShownHeight(slot);
    Frame network=frame;
    if(boost==BoostStep::eCarry){
        // This evaluation's motion vectors, for the next network pass's two-frame vectors.
        CopyTexels(cmd,slot.device.Get(),slot.heap.Get(),boostFirst,motion,motionView,slot.boostMotion.Get(),frame.motionRect);
        Uav(cmd,slot.boostMotion.Get());
        // The held look model on this input, plus the held detail where the surface only moved.
        Transition(cmd,slot.boostChange.Get(),kWrite,kRead);Transition(cmd,slot.boostModel.Get(),kWrite,kRead);
        ViewsOf(slot.device.Get(),slot.heap.Get(),boostFirst+5,{slot.encoded.Get(),slot.boostChange.Get(),slot.boostModel.Get(),motion},
            {kOwn,kOwn,kOwn,motionView},final);
        Run(cmd,slot.heap.Get(),boostFirst+5,gPipeline.carry.Get(),boosting,activeWidth,activeHeight);
        Transition(cmd,slot.boostChange.Get(),kRead,kWrite);Transition(cmd,slot.boostModel.Get(),kRead,kWrite);
        Transition(cmd,final,kWrite,kRead);readable[(slot.passes-1)%2]=true;
        slot.boostSkipped=true;
    }else if(boost==BoostStep::eNetwork && slot.boostSkipped){
        // The network last ran two evaluations ago: give it the motion over both.
        Transition(cmd,slot.boostMotion.Get(),kWrite,kRead);
        ViewsOf(slot.device.Get(),slot.heap.Get(),boostFirst,{motion,slot.boostMotion.Get(),nullptr,nullptr},
            {motionView,DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN},slot.boostTwoFrame.Get());
        Run(cmd,slot.heap.Get(),boostFirst,gPipeline.compose.Get(),boosting,boosting.motionWidth,boosting.motionHeight);
        Transition(cmd,slot.boostMotion.Get(),kRead,kWrite);
        Transition(cmd,slot.boostTwoFrame.Get(),kWrite,kRead);
        network.motion=reinterpret_cast<uintptr_t>(slot.boostTwoFrame.Get());
        network.motionRect={0,0,frame.motionRect.width,frame.motionRect.height};
    }
    for(uint32_t pass=0;boost!=BoostStep::eCarry && pass<slot.passes;++pass){
        ID3D12Resource* target=outputs[pass%2];
        if(readable[pass%2]){Transition(cmd,target,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);readable[pass%2]=false;}
        Frame neural=network;
        neural.color=reinterpret_cast<uintptr_t>(pass==0?slot.encoded.Get():outputs[(pass-1)%2]);
        neural.output=reinterpret_cast<uintptr_t>(target);
        neural.colorRect=neural.outputRect={0,0,static_cast<int32_t>(activeWidth),static_cast<int32_t>(activeHeight)};
        WriteEvaluate(slot.params,neural,PassSettings(wanted,pass));
        result=slot.direct?gDirect.Evaluate(cmd,slot.handles[pass],slot.params):slot.api.evaluate(cmd,slot.handles[pass],slot.params,nullptr);
        if(result!=NVSDK_NGX_Result_Success)break;
        Transition(cmd,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);readable[pass%2]=true;
    }
    if(network.motion!=frame.motion)Transition(cmd,slot.boostTwoFrame.Get(),kRead,kWrite);
    if(boost==BoostStep::eNetwork){
        slot.boostSkipped=false;slot.boostHeld=false;
        if(result==NVSDK_NGX_Result_Success){
            // The network's output stays exact. Fit the look model to this run's input and
            // output (one 16x16 group per cell), solve it per cell and bin, and hold what it
            // does not explain for the next evaluation's Carry.
            ViewsOf(slot.device.Get(),slot.heap.Get(),boostFirst+5,{slot.encoded.Get(),final,nullptr,nullptr},
                {kOwn,kOwn,kOwn,kOwn},slot.boostSums.Get());
            Run(cmd,slot.heap.Get(),boostFirst+5,gPipeline.fit.Get(),boosting,boosting.cellsX*8,boosting.cellsY*8);
            Transition(cmd,slot.boostSums.Get(),kWrite,kRead);
            ViewsOf(slot.device.Get(),slot.heap.Get(),boostFirst+10,{slot.boostSums.Get(),nullptr,nullptr,nullptr},
                {kOwn,kOwn,kOwn,kOwn},slot.boostModel.Get());
            Run(cmd,slot.heap.Get(),boostFirst+10,gPipeline.solve.Get(),boosting,boosting.cellsX,boosting.cellsY*boosting.bins);
            Transition(cmd,slot.boostSums.Get(),kRead,kWrite);Transition(cmd,slot.boostModel.Get(),kWrite,kRead);
            ViewsOf(slot.device.Get(),slot.heap.Get(),boostFirst+15,{slot.encoded.Get(),final,slot.boostModel.Get(),nullptr},
                {kOwn,kOwn,kOwn,kOwn},slot.boostChange.Get());
            Run(cmd,slot.heap.Get(),boostFirst+15,gPipeline.hold.Get(),boosting,activeWidth,activeHeight);
            Transition(cmd,slot.boostModel.Get(),kRead,kWrite);
            Uav(cmd,slot.boostChange.Get());slot.boostHeld=true;
        }
    }
    if(inPlace)TransitionMip0(cmd,color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(result==NVSDK_NGX_Result_Success){
        Dispatch(slot,cmd,MakeConstants(slot,frame,wanted,true,inPlace),true);
        Uav(cmd,destination);slot.resetHistory=false;++gEvaluations;gLastEvaluate=GetTickCount64();gState=State::eRunning;
    }else{SlotFailure(slot,State::eEvaluateFailed,static_cast<uint32_t>(result));}
    // Back to resting states: the pass outputs and the input.
    for(unsigned n=0;n<2;++n)
        if(readable[n])Transition(cmd,outputs[n],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd,slot.encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(measuring && result==NVSDK_NGX_Result_Success){
        cmd->EndQuery(slot.timing.Get(),D3D12_QUERY_TYPE_TIMESTAMP,set*2+1);
        cmd->ResolveQueryData(slot.timing.Get(),D3D12_QUERY_TYPE_TIMESTAMP,set*2,2,slot.readback.Get(),uint64_t(set)*kMeasurementBytes);
        // The meter buffer was promoted to UNORDERED_ACCESS by the meter's write.
        Transition(cmd,slot.meter.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(slot.readback.Get(),uint64_t(set)*kMeasurementBytes+16,slot.meter.Get(),uint64_t(set)*sizeof(float)*2,sizeof(float)*2);
        Transition(cmd,slot.meter.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.measured[set]=true;slot.measuredScale[set]=common::ShownScale(slot);
        slot.measuredStep[set]=static_cast<uint8_t>(boost==BoostStep::eNetwork?1:boost==BoostStep::eCarry?2:0);
    }
    Record(slot,cmd);gResult=static_cast<uint32_t>(result);
    return result==NVSDK_NGX_Result_Success;
}
// The Render stage's substitute input: the game color's size, our format.
bool SubstituteFor(Slot& slot,const D3D12_RESOURCE_DESC& color) noexcept {
    if(slot.substitute){
        const auto desc=slot.substitute->GetDesc();
        if(desc.Width==color.Width && desc.Height==color.Height)return true;
        if(!Complete(slot))return false;
        slot.substitute.Reset();
    }
    return MakeTexture(slot.device.Get(),static_cast<uint32_t>(color.Width),color.Height,slot.substitute)
        && TypedLoadStore(slot.device.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
}
// The fence reached `value` (0: nothing outstanding). A removed device never
// counts as executed.
bool Executed(ID3D12Fence* fence,uint64_t value) noexcept {
    if(!value)return true;
    const uint64_t completed=fence?fence->GetCompletedValue():0;
    return completed!=UINT64_MAX && completed>=value;
}
// Free for a new copy: its previous copy and every read of it executed.
bool CaptureIdle(const CaptureSet& set) noexcept {
    return Executed(set.writer.Get(),set.written) && Executed(set.reader.Get(),set.read);
}
// The copy's game submission happened, so its fence value will be signaled.
bool CaptureSubmitted(const CaptureSet& set) noexcept {
    if(!set.sequence)return false;
    if(Executed(set.writer.Get(),set.written))return true;
    return set.source && set.source->fence.Get()==set.writer.Get() && set.source->flight.submitted>=set.written;
}
// Present stage, after the game's DLSS SR/RR evaluation: the NGX route of the
// Present-stage feature and, with Use DLSS resources, a copy of this
// evaluation's depth and motion vectors on its command list. Their copies rest
// in NON_PIXEL_SHADER_RESOURCE.
void CaptureForPresent(Slot& slot,const Api& api,ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Parameter* params,
    const Settings& wanted,uint64_t revision) noexcept {
    slot.revision=revision;
    if(gForeign || !Recovered(slot,false))return;
    // This source's Render/Upscaled feature goes once its work completed.
    if(Outstanding(slot)){if(!Complete(slot))return;Destroy(slot);if(Outstanding(slot))return;}
    if(!Setup(slot,api,cmd))return;
    gNgxApi=slot.api;gNgxDevice=slot.device;
    if(wanted.resources==static_cast<uint32_t>(Resources::eNever) || !slot.flight.CanRecord(reinterpret_cast<uintptr_t>(cmd)))return;
    Frame frame{};InputsWhy why{};Source source{};
    if(!ReadFrame(params,slot.keys,Stage::eUpscaled,frame,why,source)){
        InputsUnavailable(L"Present-stage capture: ",why,source,slot.keys,gCaptureGap);return;
    }
    auto* depth=reinterpret_cast<ID3D12Resource*>(frame.depth);auto* motion=reinterpret_cast<ID3D12Resource*>(frame.motion);
    D3D12_RESOURCE_DESC depthDesc{},motionDesc{};
    if(!TextureOnDevice(depth,slot.device.Get(),frame.depthRect,depthDesc,false) || DepthViewFormat(depthDesc.Format)==DXGI_FORMAT_UNKNOWN){
        InputsRefused(L"Present-stage depth",depthDesc,frame.depthRect);return;
    }
    // The copy reads the game's view (.xy of an RGBA source) into a two-channel texture of ours.
    const DXGI_FORMAT motionView=TextureOnDevice(motion,slot.device.Get(),frame.motionRect,motionDesc,false)
        ? ViewFormat(motionDesc.Format) : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT motionFormat=MotionCaptureFormat(motionView);
    if(motionFormat==DXGI_FORMAT_UNKNOWN){
        InputsRefused(L"Present-stage motion vectors",motionDesc,frame.motionRect);return;
    }
    auto& captures=gCaptures;
    if(captures.device && captures.device.Get()!=slot.device.Get())return;
    if(!captures.heap){
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,static_cast<UINT>(kCaptureDescriptors*captures.sets.size()),
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if(FAILED(slot.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&captures.heap))))return;
        captures.device=slot.device;
    }
    // The oldest idle set, never the newest capture: the next present may read it.
    uint64_t newest=0;
    for(const auto& set:captures.sets)newest=std::max(newest,set.sequence);
    size_t chosen=captures.sets.size();
    for(size_t n=0;n<captures.sets.size();++n){
        const auto& set=captures.sets[n];
        if((set.sequence && set.sequence==newest) || !CaptureIdle(set))continue;
        if(chosen==captures.sets.size() || set.sequence<captures.sets[chosen].sequence)chosen=n;
    }
    if(chosen==captures.sets.size())return;
    auto& set=captures.sets[chosen];
    set.sequence=0;set.written=set.read=0;
    const auto width=[](const Rect& r){return static_cast<uint32_t>(r.width);};
    const auto height=[](const Rect& r){return static_cast<uint32_t>(r.height);};
    const auto fits=[](const ComPtr<ID3D12Resource>& texture,uint32_t w,uint32_t h,DXGI_FORMAT format){
        if(!texture)return false;
        const auto desc=texture->GetDesc();return desc.Width==w && desc.Height==h && desc.Format==format;
    };
    constexpr auto kRest=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if((!fits(set.depth,width(frame.depthRect),height(frame.depthRect),DXGI_FORMAT_R32_FLOAT)
            && !MakeTextureOf(slot.device.Get(),width(frame.depthRect),height(frame.depthRect),DXGI_FORMAT_R32_FLOAT,kRest,set.depth))
        || (!fits(set.motion,width(frame.motionRect),height(frame.motionRect),motionFormat)
            && !MakeTextureOf(slot.device.Get(),width(frame.motionRect),height(frame.motionRect),motionFormat,kRest,set.motion))){
        set.depth.Reset();set.motion.Reset();return;
    }
    const uint32_t first=static_cast<uint32_t>(chosen)*kCaptureDescriptors;
    Transition(cmd,set.depth.Get(),kRest,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd,set.motion.Get(),kRest,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CopyTexels(cmd,slot.device.Get(),captures.heap.Get(),first,depth,DepthViewFormat(depthDesc.Format),set.depth.Get(),frame.depthRect);
    CopyTexels(cmd,slot.device.Get(),captures.heap.Get(),first+5,motion,motionView,set.motion.Get(),frame.motionRect);
    Transition(cmd,set.depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kRest);
    Transition(cmd,set.motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,kRest);
    Record(slot,cmd);
    set.writer=slot.fence;set.written=slot.flight.Next();set.source=&slot;set.reader.Reset();
    set.depthRect={0,0,frame.depthRect.width,frame.depthRect.height};
    set.motionRect={0,0,frame.motionRect.width,frame.motionRect.height};
    set.motionScaleX=frame.motionScaleX;set.motionScaleY=frame.motionScaleY;set.depthInverted=frame.depthInverted;
    set.outWidth=slot.keys.outWidth;set.outHeight=slot.keys.outHeight;
    set.at=gPresents+1;set.sequence=++captures.sequence;gCaptureGap[0]=0;
}
// Logs a Present-stage refusal once per reason and detail.
bool PresentRefused(const wchar_t* step,const wchar_t* detail=L"") noexcept {
    static wchar_t lastDetail[1600]{};
    if(gPresentRefusal!=step || wcscmp(lastDetail,detail)){
        gPresentRefusal=step;wcsncpy_s(lastDetail,detail,_TRUNCATE);
        wchar_t message[1800]{};
        swprintf_s(message,L"DLSS 5 Present stage refused: %ls%ls%.1600ls",step,*detail?L": ":L"",detail);Log(message);
    }
    gState=State::eStageUnavailable;return false;
}
// The swapchain images the Present stage processes: one 2D subresource in a
// format with a UAV of the same format (8-bit, RGB10A2 or RGBA16F).
bool PresentImage(ID3D12Resource* buffer,D3D12_RESOURCE_DESC& desc) noexcept {
    __try {desc=buffer->GetDesc();} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    return desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize==1 && desc.MipLevels==1
        && desc.SampleDesc.Count==1 && desc.Width && desc.Width<=kMaxExtent && desc.Height && desc.Height<=kMaxExtent
        && (desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM
            || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM);
}
// Drops the Present-stage feature, image copy and idle captures once their
// work completed (the Hook Method is no longer Present).
void ReleasePresentStage() noexcept {
    auto& stage=gPresent;auto& slot=stage.slot;
    if(Outstanding(slot) || stage.color){
        if(!Complete(slot))return;
        Destroy(slot);if(Outstanding(slot))return;
        stage.color.Reset();
    }
    for(auto& set:gCaptures.sets)if(set.depth && CaptureIdle(set))set=CaptureSet{};
}
// Records this present's work on our list (recording): Setup, the image copy,
// feature (re)creation, or the evaluation with the image copied in and out.
// True when the network processed the image.
bool RecordPresent(PresentStage& stage,const PresentTarget& target,const Settings& wanted,const D3D12_RESOURCE_DESC& image,
    ID3D12Device* device,const CaptureSet* capture) noexcept {
    auto& slot=stage.slot;auto* list=stage.list.Get();
    if(!Setup(slot,gNgxApi,list) || !slot.flight.CanRecord(reinterpret_cast<uintptr_t>(list)))return false;
    MeasurementsFor(slot);common::Tick(slot);
    const uint32_t width=static_cast<uint32_t>(image.Width),height=image.Height;
    Settings run=EffectiveFor(slot,wanted,Stage::ePresent,width,height);
    uint32_t extentWidth=0,extentHeight=0;
    if(!ProcessingExtent(width,height,run.scale,extentWidth,extentHeight)){
        wchar_t reason[160]{};swprintf_s(reason,L"the swapchain image %ux%u is beyond the %u texture limit",width,height,kMaxExtent);
        InputsUnavailable(reason);gState=State::eMissingInputs;return false;
    }
    // Encoding Auto follows the swapchain; linear color of an FP16 or HDR10
    // swapchain is unbounded.
    const Encoding automatic=PresentEncoding(image.Format,target.colorSpace);
    if(automatic!=slot.automatic){slot.automatic=automatic;slot.resetHistory=true;}
    slot.keys.createFlags=image.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || automatic==Encoding::eHdr10 ? kHdrFlag : 0;
    if(stage.color){
        const auto desc=stage.color->GetDesc();
        if(desc.Width!=image.Width || desc.Height!=image.Height || desc.Format!=image.Format){
            if(!Complete(slot))return false;
            stage.color.Reset();
        }
    }
    if(!stage.color && !MakeTextureOf(device,width,height,image.Format,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,stage.color)){
        SlotFailure(slot,State::eCreateFailed,NVSDK_NGX_Result_FAIL_OutOfGPUMemory);return false;
    }
    Frame frame{};
    frame.color=frame.output=reinterpret_cast<uintptr_t>(stage.color.Get());
    frame.width=width;frame.height=height;
    frame.colorRect=frame.outputRect={0,0,static_cast<int32_t>(width),static_cast<int32_t>(height)};
    if(!Settle(slot,list,frame,run,extentWidth,extentHeight,true))return false;
    if(!slot.createdReady){if(!Complete(slot))return false;slot.createdReady=true;slot.resetHistory=true;}
    if(!ReserveDescriptors(slot,wanted))return false;
    ActivateFor(slot,wanted,run);
    if(capture){
        frame.depth=reinterpret_cast<uintptr_t>(capture->depth.Get());frame.motion=reinterpret_cast<uintptr_t>(capture->motion.Get());
        frame.depthRect=capture->depthRect;frame.motionRect=capture->motionRect;
        frame.motionScaleX=capture->motionScaleX;frame.motionScaleY=capture->motionScaleY;frame.depthInverted=capture->depthInverted;
    }else{
        // Neutral inputs: zero motion and constant (zero) depth, filled over the processing extent once
        // and handed over at the network's rect (autorect.1: Auto's level within it).
        const Rect extent{0,0,static_cast<int32_t>(slot.extentWidth),static_cast<int32_t>(slot.extentHeight)};
        const Rect active{0,0,static_cast<int32_t>(common::ShownWidth(slot)),static_cast<int32_t>(common::ShownHeight(slot))};
        if(!slot.neutralFilled){
            const uint32_t first=slot.descriptor*kSetDescriptors+10;
            CopyTexels(list,device,slot.heap.Get(),first,nullptr,DXGI_FORMAT_UNKNOWN,slot.neutralDepth.Get(),extent);
            CopyTexels(list,device,slot.heap.Get(),first+5,nullptr,DXGI_FORMAT_UNKNOWN,slot.neutralMotion.Get(),extent);
            Transition(list,slot.neutralDepth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(list,slot.neutralMotion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            slot.neutralFilled=true;
        }
        frame.depth=reinterpret_cast<uintptr_t>(slot.neutralDepth.Get());frame.motion=reinterpret_cast<uintptr_t>(slot.neutralMotion.Get());
        frame.depthRect=frame.motionRect=active;
    }
    // The image (PRESENT) into our copy, processed in place there, and back.
    auto* buffer=target.buffer;auto* color=stage.color.Get();
    Transition(list,buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(color,buffer);
    Transition(list,color,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool evaluated=RecordNeural(slot,list,frame,run,color);
    if(evaluated){
        Transition(list,buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(buffer,color);
        Transition(list,color,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list,buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
    }else Transition(list,buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    return evaluated;
}
// The image module holding `address` and the page's protection, or what
// VirtualQuery reports when no image holds it.
void Owner(const void* address,wchar_t* out,size_t size) noexcept {
    MEMORY_BASIC_INFORMATION memory{};HMODULE module=nullptr;wchar_t path[1024]{};
    if(VirtualQuery(address,&memory,sizeof(memory))!=sizeof(memory)){swprintf_s(out,size,L"unreadable");return;}
    const DWORD length=memory.Type==MEM_IMAGE && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        |GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&module) ? GetModuleFileNameW(module,path,1024) : 0;
    if(length && length<1024)swprintf_s(out,size,L"%ls+0x%llX protect=0x%X",path,static_cast<unsigned long long>(
        static_cast<const char*>(address)-reinterpret_cast<const char*>(module)),static_cast<unsigned>(memory.Protect));
    else swprintf_s(out,size,L"no image (type=0x%X) protect=0x%X",static_cast<unsigned>(memory.Type),static_cast<unsigned>(memory.Protect));
}
// The object underneath `object` (itself when nothing wraps it) on exactly
// `device`: what a native queue may be handed. Otherwise `refusal` names the
// object, the unwrap result, its vtable's owner and both devices.
template<class T> ComPtr<T> NativeOn(const wchar_t* what,T* object,ID3D12Device* device,wchar_t (&refusal)[1600]) noexcept {
    ComPtr<T> inner;ComPtr<ID3D12Device> owner;
    const bool unwrapped=object && single_overlay::native::Unwrap(object,inner);
    if(unwrapped && SUCCEEDED(inner->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get()==device)return inner;
    if(!object){swprintf_s(refusal,L"no %ls",what);return {};}
    if(!unwrapped)object->GetDevice(IID_PPV_ARGS(&owner));
    wchar_t vtable[1200]{};Owner(*reinterpret_cast<void* const*>(object),vtable,std::size(vtable));
    swprintf_s(refusal,L"%ls %p unwrap=%ls inner=%p vtable=%.1200ls object device=%p queue device=%p",what,static_cast<void*>(object),
        unwrapped?L"ok":single_overlay::native::PinInterface(object,2)?L"refused":L"refused (vtable not pinnable)",
        static_cast<void*>(inner.Get()),vtable,static_cast<void*>(owner.Get()),static_cast<void*>(device));
    return {};
}
// Closes our list and, when anything was recorded, submits it to the
// presentation queue (after the capture copy it reads) and fences it. A list
// that cannot be submitted exactly as recorded is never executed, and the
// stage stays failed.
bool SubmitPresent(PresentStage& stage,ID3D12CommandQueue* queue,bool native,size_t allocator,CaptureSet* read) noexcept {
    auto& slot=stage.slot;
    const auto list=reinterpret_cast<uintptr_t>(stage.list.Get());
    const bool closed=SUCCEEDED(stage.list->Close());
    if(slot.flight.recording!=list)return closed;
    // native: NGX holds a wrapper device, so the native queue gets the native
    // list and fences underneath ours.
    ComPtr<ID3D12Device> queueDevice;
    ComPtr<ID3D12GraphicsCommandList> submitList=stage.list;
    ComPtr<ID3D12Fence> fence=slot.fence,writer=read?read->writer:nullptr;
    if(native){
        wchar_t refusal[1600]{L"the presentation queue reports no device"};
        if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice)))){
            submitList=NativeOn(L"command list",stage.list.Get(),queueDevice.Get(),refusal);
            fence=submitList?NativeOn(L"fence",slot.fence.Get(),queueDevice.Get(),refusal):nullptr;
            if(fence && read)writer=NativeOn(L"capture fence",read->writer.Get(),queueDevice.Get(),refusal);
        }else submitList=nullptr;
        if(!submitList || !fence || (read && !writer)){
            slot.flight.failed=true;PresentRefused(L"no native object underneath ours",refusal);
            SlotFailure(slot,State::eStageUnavailable,kProviderFault);return false;
        }
    }
    const bool waited=closed && (!read || Executed(read->writer.Get(),read->written) || SUCCEEDED(queue->Wait(writer.Get(),read->written)));
    if(!waited){slot.flight.failed=true;SlotFailure(slot,State::eEvaluateFailed,kProviderFault);return false;}
    ID3D12CommandList* lists[]{submitList.Get()};
    queue->ExecuteCommandLists(1,lists);
    const uint64_t value=slot.flight.Next();
    const bool signaled=value && SUCCEEDED(queue->Signal(fence.Get(),value));
    slot.flight.Signaled(list,value,signaled);
    if(!signaled){SlotFailure(slot,State::eEvaluateFailed,kProviderFault);return false;}
    stage.allocatorFence[allocator]=value;
    if(read){read->reader=slot.fence;read->read=value;}
    slot.lastQueue=queue;
    return true;
}
// One present at the Present stage. True when its image was processed.
bool ProcessPresent(PresentStage& stage,const PresentTarget& target,const Settings& wanted,uint64_t revision) noexcept {
    auto& slot=stage.slot;
    slot.revision=revision;
    if(gForeign){gState=State::eOtherMod;return false;}
    if(!gNgxApi.Complete() || !gNgxDevice){gState=State::eWaitingForDlss;return false;}
    // Our list is recorded on the device layer NGX and the provider were given
    // and runs on the queue the application presents with. A wrapper such as
    // ReShade or Streamline can sit between the two: its queue over the device
    // NGX holds, or the native queue under the wrapper device NGX holds. Then
    // the work goes to the native queue underneath the presentation queue (the
    // same hardware queue, submitted before the Present is forwarded), found
    // with the wrappers' public unwrap contracts, only if both lead to the same
    // native device object. When NGX holds that native device itself, our list
    // and fences were created on it and go as they are; only a wrapped NGX
    // device needs the native objects underneath ours.
    ComPtr<ID3D12Device> device=gNgxDevice,queueDevice;
    ComPtr<ID3D12CommandQueue> nativeQueue;
    ID3D12CommandQueue* queue=target.queue;
    bool native=false;
    if(FAILED(target.queue->GetDevice(IID_PPV_ARGS(&queueDevice))))
        return PresentRefused(L"the presentation queue does not belong to the device DLSS runs on");
    if(queueDevice.Get()!=device.Get()){
        ComPtr<ID3D12Device> nativeNgx,nativeQueueDevice;
        const bool queueUnwrapped=single_overlay::native::Unwrap(target.queue,nativeQueue);
        const bool ngxUnwrapped=single_overlay::native::Unwrap(device.Get(),nativeNgx);
        const bool same=queueUnwrapped && ngxUnwrapped && SUCCEEDED(nativeQueue->GetDevice(IID_PPV_ARGS(&nativeQueueDevice)))
            && nativeQueueDevice.Get()==nativeNgx.Get();
        const bool ngxNative=ngxUnwrapped && nativeNgx.Get()==device.Get();
        if(!stage.layersLogged){
            stage.layersLogged=true;
            wchar_t message[400]{};
            swprintf_s(message,L"DLSS 5 Present stage layers: queue device=%p (queue %ls, native device=%p); NGX device=%p (%ls, native=%p); %ls",
                static_cast<void*>(queueDevice.Get()),!queueUnwrapped?L"unwrap failed":nativeQueue.Get()==target.queue?L"native":L"wrapped",
                static_cast<void*>(nativeQueueDevice.Get()),static_cast<void*>(device.Get()),
                !ngxUnwrapped?L"unwrap failed":ngxNative?L"native":L"wrapped",static_cast<void*>(nativeNgx.Get()),
                !same?L"different devices":ngxNative?L"submitting ours on the native queue":L"submitting the native objects underneath ours");
            Log(message);
        }
        if(!same){
            wchar_t devices[200]{};
            swprintf_s(devices,L"queue device=%p (native %p), NGX device=%p (native %p)",static_cast<void*>(queueDevice.Get()),
                static_cast<void*>(nativeQueueDevice.Get()),static_cast<void*>(device.Get()),static_cast<void*>(nativeNgx.Get()));
            return PresentRefused(L"the presentation queue does not belong to the device DLSS runs on",devices);
        }
        queue=nativeQueue.Get();native=!ngxNative;
    }
    if(slot.device && slot.device.Get()!=device.Get())return PresentRefused(L"the DLSS device changed");
    if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return PresentRefused(L"the presentation queue is not DIRECT");
    D3D12_RESOURCE_DESC image{};
    if(!PresentImage(target.buffer,image))return PresentRefused(L"swapchain image layout or format");
    if(!TypedLoadStore(device.Get(),image.Format))return PresentRefused(L"no typed UAV load/store for the swapchain format");
    if(!Recovered(slot))return false;
    // Use DLSS resources: the newest submitted capture of this frame or the one
    // before, preferring the view whose DLSS output matches the image.
    CaptureSet* capture=nullptr;
    if(wanted.resources!=static_cast<uint32_t>(Resources::eNever) && gCaptures.device.Get()==device.Get())
        for(auto& set:gCaptures.sets){
            if(!CaptureSubmitted(set) || !CaptureCurrent(gPresents,set.at))continue;
            const auto matches=[&image](const CaptureSet& c){return c.outWidth==image.Width && c.outHeight==image.Height;};
            if(!capture || (matches(set) && !matches(*capture))
                || (matches(set)==matches(*capture) && set.sequence>capture->sequence))capture=&set;
        }
    const PresentInputs inputs=ChoosePresentInputs(wanted,capture!=nullptr);
    if(inputs==PresentInputs::eSkip){
        InputsUnavailable(L"Use DLSS resources is Required and no DLSS evaluation of this or the previous frame was captured");
        slot.resetHistory=true;gState=State::eMissingInputs;return false;
    }
    if(inputs!=PresentInputs::eCaptured)capture=nullptr;
    if(inputs!=stage.inputs){stage.inputs=inputs;slot.resetHistory=true;}
    if(!stage.list){
        for(auto& allocator:stage.allocators)
            if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()))))
                return PresentRefused(L"command allocators");
        if(FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,stage.allocators[0].Get(),nullptr,IID_PPV_ARGS(&stage.list)))
            || FAILED(stage.list->Close())){stage.list.Reset();return PresentRefused(L"command list");}
        stage.allocatorFence={};
    }
    // One allocator per fenced submission, reused once that submission executed.
    size_t allocator=stage.allocators.size();
    for(size_t n=0;n<stage.allocators.size();++n)
        if(Executed(slot.fence.Get(),stage.allocatorFence[n])){allocator=n;break;}
    if(allocator==stage.allocators.size()){slot.resetHistory=true;return false;}
    if(FAILED(stage.allocators[allocator]->Reset()) || FAILED(stage.list->Reset(stage.allocators[allocator].Get(),nullptr)))
        return PresentRefused(L"command list reset");
    const bool processed=RecordPresent(stage,target,wanted,image,device.Get(),capture);
    return SubmitPresent(stage,queue,native,allocator,processed?capture:nullptr) && processed;
}
}
void Configure(Settings settings) noexcept {
    settings=Sanitized(settings);
    const bool present=settings.stage==static_cast<uint32_t>(Stage::ePresent);
    gPresentChosen.store(present,std::memory_order_release);
    gPresentSelected.store(present && settings.enabled,std::memory_order_release);
    if(!present || !settings.enabled || !settings.blockHudless)gHudlessBlocked.store(false,std::memory_order_release);
    std::lock_guard lock(gSettingsMutex);
    if(settings==gWanted)return;
    gWanted=settings;++gRevision;
}
Settings Requested() noexcept {std::lock_guard lock(gSettingsMutex);return gWanted;}
void SetLogCallback(LogCallback callback) noexcept {gLog.store(callback);}
void SetQueueForward(QueueForwardFn forward) noexcept {gQueueForward.store(forward,std::memory_order_release);}
dlss_sr::CreateKeys ReadCreate(const NVSDK_NGX_Parameter* params) noexcept {
    dlss_sr::CreateKeys keys{};if(!params)return keys;
    const char* names[]{"Width","Height","OutWidth","OutHeight","DLSS.Feature.Create.Flags"};
    uint32_t values[5]{};
    for(unsigned i=0;i<5;++i)if(Unsigned(params,names[i],values[i]))keys.present|=i==4?dlss_sr::CreateKeys::eCreateFlags:1u<<i;
    keys.width=values[0];keys.height=values[1];keys.outWidth=values[2];keys.outHeight=values[3];keys.createFlags=static_cast<int32_t>(values[4]);
    return keys;
}
void GameCreated(size_t route,const NVSDK_NGX_Handle* handle,const dlss_sr::CreateKeys& keys) noexcept {
    if(!handle)return;
    std::lock_guard lock(gMutex);Pump();
    // A duplicate numerical handle cannot overwrite an older owned generation.
    if(Find(route,reinterpret_cast<uintptr_t>(handle)))return;
    for(auto& slot:gSlots)if(!slot.occupied){
        slot.occupied=true;slot.route=route;slot.game=reinterpret_cast<uintptr_t>(handle);slot.keys=keys;
        if(!wcscmp(gInputsGap,kSlotsFull))gInputsGap[0]=0;
        return;
    }
    InputsUnavailable(kSlotsFull);
    gState=State::eMissingInputs;gResult=NVSDK_NGX_Result_FAIL_OutOfGPUMemory;++gFailures;
}
bool Owns(size_t route,const NVSDK_NGX_Handle* game) noexcept {
    std::lock_guard lock(gMutex);return Find(route,reinterpret_cast<uintptr_t>(game))!=nullptr;
}
// Which caller and NGX runtime, once per process.
void ForeignCreated(const void* caller,HMODULE runtime) noexcept {
    bool first=false;
    {std::lock_guard lock(gMutex);first=!gForeign;gForeign=true;gState=State::eOtherMod;}
    if(!first)return;
    wchar_t where[1200]{},path[1024]{};
    if(caller)Owner(caller,where,1200);else wcscpy_s(where,L"-");
    const DWORD length=runtime?GetModuleFileNameW(runtime,path,1024):0;
    if(!length || length>=1024)wcscpy_s(path,L"-");
    wchar_t message[2400]{};
    swprintf_s(message,L"DLSS 5 stopped: NGX feature %u was created by another caller (caller=%ls runtime=%ls)",kFeature,where,path);
    Log(message);
}
Substitution BeforeEvaluate(size_t route,const Api& api,ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Handle* game,
    const NVSDK_NGX_Parameter* params,uint64_t expectedRevision) noexcept {
    Substitution substitution{};
    if(!cmd || !game || !params)return substitution;
    try {
        std::lock_guard lock(gMutex);Pump();
        Slot* found=Find(route,reinterpret_cast<uintptr_t>(game));if(!found)return substitution;
        Slot& slot=*found;uint64_t revision=0;const Settings wanted=Wanted(revision);
        if(expectedRevision && revision!=expectedRevision)return substitution;
        if(!wanted.enabled || wanted.stage!=static_cast<uint32_t>(Stage::eRender))return substitution;
        Frame frame{};D3D12_RESOURCE_DESC color{};Settings run{};
        if(!Ready(slot,api,cmd,params,wanted,revision,Stage::eRender,frame,color,run))return substitution;
        bool typed=false;uintptr_t original=0;
        if(!Resource(params,"Color",original,&typed) || original!=frame.color || !SubstituteFor(slot,color)){
            InputsUnavailable(L"the Before upscaling stage could not substitute the game's Color input");
            slot.descriptorFence[slot.descriptor]=0;gState=State::eMissingInputs;return substitution;
        }
        if(!RecordNeural(slot,cmd,frame,run,slot.substitute.Get()))return substitution;
        gInputsGap[0]=0;
        Transition(cmd,slot.substitute.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(!SetResource(params,"Color",reinterpret_cast<uintptr_t>(slot.substitute.Get()),typed)){
            SetResource(params,"Color",original,typed);
            Transition(cmd,slot.substitute.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            SlotFailure(slot,State::eEvaluateFailed,NVSDK_NGX_Result_FAIL_InvalidParameter);return substitution;
        }
        substitution.slot=static_cast<size_t>(&slot-gSlots.data());substitution.original=original;substitution.typed=typed;
    }catch(...){std::lock_guard lock(gMutex);Failure(State::eEvaluateFailed,kProviderFault);}
    return substitution;
}
void AfterEvaluate(size_t route,const Api& api,ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Handle* game,
    const NVSDK_NGX_Parameter* params,NVSDK_NGX_Result gameResult,const Substitution& substitution,uint64_t expectedRevision) noexcept {
    try {
        std::lock_guard lock(gMutex);
        if(substitution.slot<gSlots.size()){
            // The game's own input goes back before anything else can read it.
            auto& slot=gSlots[substitution.slot];
            SetResource(params,"Color",substitution.original,substitution.typed);
            if(slot.substitute && cmd){
                Transition(cmd,slot.substitute.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Record(slot,cmd);
            }
        }
        if(gameResult!=NVSDK_NGX_Result_Success || !cmd || !game)return;
        Pump();
        Slot* found=Find(route,reinterpret_cast<uintptr_t>(game));if(!found)return;
        Slot& slot=*found;uint64_t revision=0;const Settings wanted=Wanted(revision);
        // The caller resolved its stage for that revision (the DirectX 11 bridge): nothing of another stage, such as
        // the Present stage's capture, ever records on its list.
        if(expectedRevision && revision!=expectedRevision)return;
        if(!wanted.enabled){slot.resetHistory=true;gState=State::eOff;return;}
        if(wanted.stage==static_cast<uint32_t>(Stage::ePresent)){CaptureForPresent(slot,api,cmd,params,wanted,revision);return;}
        // Hook Method Auto runs here (dlss5_auto.h AutoStage).
        if(wanted.stage!=static_cast<uint32_t>(Stage::eUpscaled) && wanted.stage!=static_cast<uint32_t>(Stage::eAuto))return;
        Frame frame{};D3D12_RESOURCE_DESC color{};Settings run{};
        if(!Ready(slot,api,cmd,params,wanted,revision,auto_mode::AutoStage(),frame,color,run))return;
        if(RecordNeural(slot,cmd,frame,run,reinterpret_cast<ID3D12Resource*>(frame.color)))gInputsGap[0]=0;
    }catch(...){std::lock_guard lock(gMutex);Failure(State::eEvaluateFailed,kProviderFault);}
}
void BeforeRelease(size_t route,const NVSDK_NGX_Handle* game) noexcept {
    std::lock_guard lock(gMutex);
    if(auto* slot=Find(route,reinterpret_cast<uintptr_t>(game))) {
        slot->retiring=true;
        // The game's release boundary does not prove NR completion. The queue
        // fence remains authoritative even if the game releases its SR first.
        if(Complete(*slot))Destroy(*slot);
    }
}
void AfterRelease(size_t route,const NVSDK_NGX_Handle* game,NVSDK_NGX_Result result) noexcept {
    std::lock_guard lock(gMutex);
    for(auto& slot:gSlots)if(slot.occupied && slot.retiring && slot.route==route && slot.game==reinterpret_cast<uintptr_t>(game)){
        if(result==NVSDK_NGX_Result_Success)slot.game=0;else slot.retiring=false;
    }
    Pump();
}
void NoteOverlayDivider() noexcept {gOverlayDividerTick.store(GetTickCount64(),std::memory_order_release);}
CompareView LastCompare() noexcept {
    CompareView view{};view.tick=gCompareTick.load(std::memory_order_acquire);
    view.split=gCompareShown.load(std::memory_order_relaxed);view.held=gCompareHeld.load(std::memory_order_relaxed);
    view.swapped=gCompareSwapped.load(std::memory_order_relaxed);
    return view;
}
bool Running() noexcept {
    std::lock_guard lock(gMutex);return gState==State::eRunning;
}
bool Offered() noexcept {
    if(Requested().enabled)return true;
    try {return !ProviderPath().empty();}catch(...){return false;}
}
Snapshot Read() noexcept {
    std::lock_guard lock(gMutex);Snapshot view{};view.requested=Requested();view.state=gState;view.result=gResult;
    view.creates=gCreates;view.evaluations=gEvaluations;view.failures=gFailures;
    try {view.providerDetected=!ProviderPath().empty() || gDirect.Module();}catch(...){}
    const auto live=[&view](const Slot& slot){
        ++view.live;view.width=slot.width;view.height=slot.height;view.extentWidth=common::ShownWidth(slot);view.extentHeight=common::ShownHeight(slot);
        view.route=slot.direct?2:1;view.stage=slot.applied.stage;view.passes=slot.passes;
        view.pending|=!SameFeature(slot.applied,Expected(slot,view.requested)) || !slot.createdReady;
        view.scale=common::ShownScale(slot);view.gpuMs=slot.gpuMs;view.budgetMs=common::AutoBudgetMs(slot);
        view.scaleFloorHeld|=view.requested.scaleAuto && slot.scalingSeeded && slot.scaling.floorHeld;
        view.boostNetworkMs=slot.boostNetworkMs;
        view.frameMs=static_cast<float>(slot.clock.averageMs);
        view.exposureGain=common::AdaptiveGain(slot,view.requested);
        view.d3d11|=slot.route==kD3D11Route;
    };
    for(const auto& slot:gSlots)if(slot.occupied && slot.HasFeature())live(slot);
    if(gPresent.slot.HasFeature())live(gPresent.slot);
    // A Vulkan feature (its module publishes the view under a leaf lock).
    shared::Live vulkan{};
    const auto vulkanLive=gVulkanLive.load(std::memory_order_acquire);
    if(vulkanLive && vulkanLive(vulkan) && vulkan.live){
        view.live+=vulkan.live;view.width=vulkan.width;view.height=vulkan.height;
        view.extentWidth=vulkan.extentWidth;view.extentHeight=vulkan.extentHeight;view.route=vulkan.route;view.stage=vulkan.stage;
        view.passes=vulkan.passes;view.pending|=vulkan.pending;view.scale=vulkan.scale;view.gpuMs=vulkan.gpuMs;
        view.budgetMs=vulkan.budgetMs;view.frameMs=vulkan.frameMs;view.exposureGain=vulkan.exposureGain;
        view.scaleFloorHeld|=vulkan.scaleFloorHeld;
    }
    const bool present=view.requested.stage==static_cast<uint32_t>(Stage::ePresent);
    // Off measures nothing: no cost left from before while the feature is released.
    if(!view.requested.enabled){view.gpuMs=view.boostNetworkMs=-1.0f;view.budgetMs=0.0f;}
    if(!view.requested.enabled)view.state=State::eOff;
    else if(gForeign)view.state=State::eOtherMod;
    // A Present-stage refusal stays; one left from another Hook Method does not.
    else if(view.state==State::eOff || (view.state==State::eStageUnavailable && !present)
        || (view.state==State::eRunning && GetTickCount64()-gLastEvaluate>1000))view.state=State::eWaitingForDlss;
    // Before upscaling or Present on a Vulkan game has no stage yet.
    if(view.requested.enabled && !gForeign && vulkanLive && vulkan.stageUnavailable)view.state=State::eStageUnavailable;
    // dx11nr.1: the DirectX 11 bridge (its view is lock-free): Present has no stage there, an outdated driver stops
    // it, and a refusal of the game's device keeps DLSS 5 off.
    shared::D3D11View bridge{};
    const auto d3d11View=gD3D11View.load(std::memory_order_acquire);
    if(d3d11View && d3d11View(bridge)){
        view.d3d11|=bridge.tracked;view.d3d11Refusal=bridge.refusal;
        if(view.requested.enabled && !gForeign){
            if(bridge.initFailure){view.state=CreateFailureState(bridge.initFailure);view.result=bridge.initFailure;}
            else if(bridge.stageUnavailable)view.state=State::eStageUnavailable;
            else if(d3d11::Blocking(static_cast<d3d11::Refusal>(bridge.refusal)) && view.state==State::eWaitingForDlss)
                view.state=State::eUnsupported;
        }
    }
    return view;
}
void Present(const PresentTarget& target) noexcept {
    if(!target.queue || !target.buffer || !target.owner)return;
    // hudlatch.1: Block Hudless is written under gMutex, as DrainPresent clears it, so a present that started before
    // its owner's drain cannot set it again afterwards.
    try {
        std::lock_guard lock(gMutex);Pump();
        uint64_t revision=0;const Settings wanted=Wanted(revision);
        bool blocked=false;
        if(wanted.enabled && wanted.stage==static_cast<uint32_t>(Stage::ePresent)){
            ++gPresents;
            auto& stage=gPresent;
            if(stage.owner!=target.owner){stage.owner=target.owner;stage.slot.resetHistory=true;}
            blocked=ProcessPresent(stage,target,wanted,revision) && wanted.blockHudless;
        }
        gHudlessBlocked.store(blocked,std::memory_order_release);
    }catch(...){
        std::lock_guard lock(gMutex);
        gHudlessBlocked.store(false,std::memory_order_release);
        Failure(State::eEvaluateFailed,kProviderFault);
    }
}
bool DrainPresent(uintptr_t owner) noexcept {
    ComPtr<ID3D12Fence> fence;uint64_t value=0;
    {
        std::lock_guard lock(gMutex);
        if(!owner || gPresent.owner!=owner)return true;
        gPresent.owner=0;gPresent.slot.resetHistory=true;
        // hudlatch.1: no present processes the game's image until a chain owns the stage again (a chain moved to
        // XeFG never does), so frame generation gets the HUD-less tag back here.
        if(gHudlessBlocked.exchange(false,std::memory_order_acq_rel))
            Log(L"DLSS 5 Present stage: its swapchain was released or resized; frame generation gets the game's HUD-less tag until a present is processed again");
        fence=gPresent.slot.fence;value=gPresent.slot.flight.submitted;
    }
    if(!fence || fence->GetCompletedValue()>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    const bool done=event && SUCCEEDED(fence->SetEventOnCompletion(value,event)) && WaitForSingleObject(event,3000)==WAIT_OBJECT_0;
    if(event)CloseHandle(event);
    if(!done)Log(L"DLSS 5 Present stage: swapchain work did not complete within 3 s");
    return done;
}
bool PresentStageSelected() noexcept {return gPresentSelected.load(std::memory_order_acquire);}
bool BlockHudless() noexcept {return gHudlessBlocked.load(std::memory_order_acquire);}
namespace shared {
Settings Wanted(uint64_t& revision) noexcept {return dlss5::Wanted(revision);}
void SetState(State state,uint32_t result) noexcept {std::lock_guard lock(gMutex);gState=state;gResult=result;}
void Failure(State state,uint32_t result) noexcept {std::lock_guard lock(gMutex);dlss5::Failure(state,result);}
void Created(uint32_t passes) noexcept {std::lock_guard lock(gMutex);gCreates+=passes;}
void Evaluated() noexcept {std::lock_guard lock(gMutex);++gEvaluations;gLastEvaluate=GetTickCount64();gState=State::eRunning;}
bool Foreign() noexcept {std::lock_guard lock(gMutex);return gForeign;}
void Log(const wchar_t* message) noexcept {dlss5::Log(message);}
std::wstring ProviderPath() {return dlss5::ProviderPath();}
void Cost(CostLog& log,uint32_t stage,uint32_t extentWidth,uint32_t extentHeight,float gpuMs,double frameMs) noexcept {
    uint64_t evaluations=0;
    {std::lock_guard lock(gMutex);evaluations=gEvaluations;}
    dlss5::LogCost(L" Vulkan",log,stage,extentWidth,extentHeight,gpuMs,frameMs,evaluations);
}
bool ReadFrame(const NVSDK_NGX_Parameter* params,const dlss_sr::CreateKeys& keys,Stage stage,Frame& frame,InputsWhy& why,
    Source& source) noexcept {return dlss5::ReadFrame(params,keys,stage,frame,why,source);}
void InputsUnavailable(const InputsWhy& why,const Source& source,const dlss_sr::CreateKeys& keys) noexcept {
    std::lock_guard lock(gMutex);dlss5::InputsUnavailable(L"",why,source,keys);
}
void InputsUnavailable(const wchar_t* reason) noexcept {std::lock_guard lock(gMutex);dlss5::InputsUnavailable(reason);}
void InputsAvailable() noexcept {std::lock_guard lock(gMutex);gInputsGap[0]=0;}
PassConstants Constants(const Frame& frame,const Settings& wanted,uint32_t extentWidth,uint32_t extentHeight,Encoding automatic,
    bool unbounded,float adaptiveGain,bool decode,bool inPlace) noexcept {
    return PassConstantsFor(frame,wanted,extentWidth,extentHeight,automatic,unbounded,adaptiveGain,decode,inPlace);
}
void SetVulkanLive(LiveFn fn) noexcept {gVulkanLive.store(fn,std::memory_order_release);}
void SetD3D11View(D3D11ViewFn fn) noexcept {gD3D11View.store(fn,std::memory_order_release);}
uint64_t Evaluations() noexcept {std::lock_guard lock(gMutex);return gEvaluations;}
void ReleaseRoute(size_t route) noexcept {
    std::lock_guard lock(gMutex);Pump();
    for(auto& slot:gSlots)if(slot.occupied && !slot.retiring && slot.route==route && Outstanding(slot)){
        dlss5::Collect(slot);if(!Complete(slot))continue;
        Destroy(slot);
        if(!Outstanding(slot)){slot.gpuMs=-1.0f;slot.resetHistory=true;Log(L"DLSS 5: a feature its Hook Method does not run on was released");}
    }
}
bool CanEvaluate(size_t route,const NVSDK_NGX_Handle* game,const NVSDK_NGX_Parameter* params,Stage stage) noexcept {
    if(!game)return false;
    std::lock_guard lock(gMutex);Pump();
    if(gForeign){gState=State::eOtherMod;return false;}
    Slot* found=Find(route,reinterpret_cast<uintptr_t>(game));
    return found && Possible(*found,params,stage);
}
}
#if defined(MFG_DLSS5_TESTING)
namespace testing {
void AllowSoftwareDevice() noexcept {std::lock_guard lock(gMutex);gSoftwareTest=true;}
void Collect() noexcept {std::lock_guard lock(gMutex);Pump();}
void SetProviderPath(const wchar_t* path) {std::lock_guard lock(gMutex);gProviderTestPath=path?path:L"";}
void SetVideoMemory(uint64_t usage,uint64_t budget) noexcept {gTestUsage=usage;gTestBudget=budget;}
void SetAutoLevel(size_t level) noexcept {gTestAutoLevel=level;}
std::wstring NativeRefusal(ID3D12DeviceChild* object,ID3D12Device* queueDevice) {
    wchar_t refusal[1600]{};
    return NativeOn(L"object",object,queueDevice,refusal) ? std::wstring{} : std::wstring{refusal};
}
}
#endif

}
