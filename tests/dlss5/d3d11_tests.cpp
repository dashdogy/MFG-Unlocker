// dx11nr.1: the DirectX 11 bridge's device-free decisions (dlss5_d3d11_policy.h) and its depth pass on WARP.
//   Dlss5D3D11Tests formats      typeless->typed color, depth copy/view, motion-vector and exposure formats
//   Dlss5D3D11Tests parameters   the evaluation keys handed to the D3D12 stage, checked against its own ReadFrame
//   Dlss5D3D11Tests out-of-date  the 0xBAD0000C fail-closed decisions
//   Dlss5D3D11Tests depth        R24G8 / R32 / R32G8X24 / R16 / R32_FLOAT depth to R32_FLOAT, compute state restored
#include "dlss5_d3d11.h"
#include "dlss5_d3d11_policy.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#c);std::exit(1);}}while(0)
using namespace dlss5;
using namespace dlss5::d3d11;

void Formats(){
 // Color/output: the typed member of the family, so CopySubresourceRegion is legal both ways.
 const DXGI_FORMAT colors[][2]{
  {DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R16G16B16A16_FLOAT},{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT},
  {DXGI_FORMAT_R32G32B32A32_TYPELESS,DXGI_FORMAT_R32G32B32A32_FLOAT},
  {DXGI_FORMAT_R10G10B10A2_TYPELESS,DXGI_FORMAT_R10G10B10A2_UNORM},{DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM},
  {DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM},{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM},
  {DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM},{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM},
  {DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT}};
 for(const auto& pair:colors)CHECK(ColorFormat(pair[0])==pair[1]);
 // sRGB views (the D3D12 stage refuses them too), integer and single-channel formats are refused.
 for(const DXGI_FORMAT refused:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UINT,
     DXGI_FORMAT_R10G10B10A2_UINT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_UNKNOWN})
  CHECK(ColorFormat(refused)==DXGI_FORMAT_UNKNOWN);
 // Depth: the copy's typeless family and the depth view the pass reads.
 const DXGI_FORMAT depths[][3]{
  {DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
  {DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
  {DXGI_FORMAT_R24_UNORM_X8_TYPELESS,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
  {DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT},
  {DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT},
  {DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT},
  {DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
  {DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
  {DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_UNORM},
  {DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_UNORM},
  {DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_FLOAT}};
 for(const auto& row:depths){const DepthFormats formats=DepthFormatsFor(row[0]);CHECK(formats.copy==row[1]&&formats.view==row[2]);}
 for(const DXGI_FORMAT refused:{DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_UNKNOWN})
  CHECK(DepthFormatsFor(refused).copy==DXGI_FORMAT_UNKNOWN&&DepthFormatsFor(refused).view==DXGI_FORMAT_UNKNOWN);
 // Motion vectors: two- or four-channel 16/32-bit float only (dlss5.cpp MotionCaptureFormat).
 const DXGI_FORMAT motions[][2]{
  {DXGI_FORMAT_R16G16_TYPELESS,DXGI_FORMAT_R16G16_FLOAT},{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16_FLOAT},
  {DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R16G16B16A16_FLOAT},{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT},
  {DXGI_FORMAT_R32G32_TYPELESS,DXGI_FORMAT_R32G32_FLOAT},{DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32_FLOAT},
  {DXGI_FORMAT_R32G32B32A32_TYPELESS,DXGI_FORMAT_R32G32B32A32_FLOAT},{DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT}};
 for(const auto& pair:motions)CHECK(MotionFormat(pair[0])==pair[1]);
 for(const DXGI_FORMAT refused:{DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R16G16_SINT,DXGI_FORMAT_R32_FLOAT,
     DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_UNKNOWN})
  CHECK(MotionFormat(refused)==DXGI_FORMAT_UNKNOWN);
 // Exposure: 1x1 R32/R16 float, else left out.
 CHECK(ExposureFormat(DXGI_FORMAT_R32_TYPELESS)==DXGI_FORMAT_R32_FLOAT&&ExposureFormat(DXGI_FORMAT_R32_FLOAT)==DXGI_FORMAT_R32_FLOAT);
 CHECK(ExposureFormat(DXGI_FORMAT_R16_TYPELESS)==DXGI_FORMAT_R16_FLOAT&&ExposureFormat(DXGI_FORMAT_R16_FLOAT)==DXGI_FORMAT_R16_FLOAT);
 CHECK(ExposureFormat(DXGI_FORMAT_R8_UNORM)==DXGI_FORMAT_UNKNOWN&&ExposureFormat(DXGI_FORMAT_R16G16B16A16_FLOAT)==DXGI_FORMAT_UNKNOWN);
}

// Records every key the D3D12 stage reads; resources answer with a fake pointer, scalars are absent.
struct Spy final:NVSDK_NGX_Parameter {
 mutable std::set<std::string> read;
 NVSDK_NGX_Result Note(const char* key)const{read.insert(key);return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 template<class T> NVSDK_NGX_Result Pointer(const char* key,T** out)const{read.insert(key);*out=reinterpret_cast<T*>(uintptr_t(0x1000));return NVSDK_NGX_Result_Success;}
#define SPY(T) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char*k,T*)const override{return Note(k);}
 SPY(unsigned long long) SPY(float) SPY(double) SPY(unsigned int) SPY(int)
#undef SPY
 void Set(const char*,ID3D11Resource*)override{} void Set(const char*,ID3D12Resource*)override{} void Set(const char*,void*)override{}
 NVSDK_NGX_Result Get(const char*k,ID3D11Resource** out)const override{return Pointer(k,out);}
 NVSDK_NGX_Result Get(const char*k,ID3D12Resource** out)const override{return Pointer(k,out);}
 NVSDK_NGX_Result Get(const char*k,void** out)const override{return Pointer(k,out);}
 void Reset()override{read.clear();}
};
dlss_sr::CreateKeys Keys(){
 dlss_sr::CreateKeys keys{};keys.present=dlss_sr::CreateKeys::eRequired|dlss_sr::CreateKeys::eCreateFlags;
 keys.width=640;keys.height=360;keys.outWidth=1280;keys.outHeight=720;keys.createFlags=kMotionLowResFlag|kDepthInvertedFlag;
 return keys;
}
void Parameters(){
 // Every key ReadFrame reads, at either stage, is a scalar key or a resource key the bridge hands over.
 for(const Stage stage:{Stage::eUpscaled,Stage::eRender}){
  Spy spy;Frame frame{};InputsWhy why{};Source source{};
  shared::ReadFrame(&spy,Keys(),stage,frame,why,source);
  CHECK(spy.read.count("Depth")&&spy.read.count("MotionVectors")&&spy.read.count(stage==Stage::eRender?"Color":"Output"));
  for(const std::string& key:spy.read){
   bool listed=false;
   for(const ScalarKey& scalar:kScalarKeys)listed|=key==scalar.name;
   for(const char* resource:kResourceKeys)listed|=key==resource;
   if(!listed)std::fprintf(stderr,"ReadFrame reads %s, which the bridge does not copy\n",key.c_str());
   CHECK(listed);
  }
  for(const ScalarKey& scalar:kScalarKeys)CHECK(spy.read.count(scalar.name));
 }
 // The game's D3D11 block (a Block stands in for NGX's), with scalars of the types games store.
 auto* const output11=reinterpret_cast<ID3D11Resource*>(uintptr_t(0x1100));
 auto* const depth11=reinterpret_cast<ID3D11Resource*>(uintptr_t(0x1101));
 auto* const motion11=reinterpret_cast<ID3D11Resource*>(uintptr_t(0x1102));
 Block game;
 game.Set("Output",output11);game.Set("Depth",depth11);game.Set("MotionVectors",static_cast<void*>(motion11));
 game.Set("DLSS.Output.Subrect.Base.X",8u);game.Set("DLSS.Output.Subrect.Base.Y",4);
 game.Set("DLSS.Input.Depth.Subrect.Base.X",2u);game.Set("DLSS.Input.Depth.Subrect.Base.Y",3u);
 game.Set("DLSS.Input.MV.Subrect.Base.X",5);game.Set("DLSS.Input.MV.Subrect.Base.Y",6u);
 game.Set("DLSS.Render.Subrect.Dimensions.Width",600u);game.Set("DLSS.Render.Subrect.Dimensions.Height",340u);
 game.Set("MV.Scale.X",-2.0f);game.Set("MV.Scale.Y",3.0f);
 game.Set("DLSS.Pre.Exposure",2.0f);game.Set("DLSS.Exposure.Scale",4.0f);game.Set("Reset",1);
 game.Set("Jitter.Offset.X",0.25f);
 Block ours;CopyScalars(&game,ours);
 // No D3D11 resource reaches the D3D12 stage, and nothing it does not read is copied.
 ID3D11Resource* leaked=nullptr;void* raw=nullptr;float jitter=0.0f;
 CHECK(ours.Get("Output",&leaked)!=NVSDK_NGX_Result_Success&&ours.Get("Depth",&raw)!=NVSDK_NGX_Result_Success);
 CHECK(ours.Get("MotionVectors",&raw)!=NVSDK_NGX_Result_Success&&ours.Get("Jitter.Offset.X",&jitter)!=NVSDK_NGX_Result_Success);
 auto* const output12=reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1200));
 auto* const depth12=reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1201));
 auto* const motion12=reinterpret_cast<ID3D12Resource*>(uintptr_t(0x1202));
 ours.Set("Output",output12);ours.Set("Depth",depth12);ours.Set("MotionVectors",motion12);
 Frame frame{};InputsWhy why{};Source source{};
 CHECK(shared::ReadFrame(&ours,Keys(),Stage::eUpscaled,frame,why,source));
 CHECK(source.output==uintptr_t(output12)&&source.depth==uintptr_t(depth12)&&source.motion==uintptr_t(motion12)&&!source.exposure);
 CHECK(source.outputX==8&&source.outputY==4&&source.depthX==2&&source.depthY==3&&source.motionX==5&&source.motionY==6);
 CHECK(source.renderWidth==600&&source.renderHeight==340);
 CHECK(source.motionScaleX==-2.0f&&source.motionScaleY==3.0f&&source.preExposure==0.5f&&source.reset);
 // The same reads of the game's own block (its resources in D3D12 form) give the same evaluation.
 Block reference=game;
 reference.Set("Output",output12);reference.Set("Depth",depth12);reference.Set("MotionVectors",motion12);
 Frame expected{};Source expectedSource{};
 CHECK(shared::ReadFrame(&reference,Keys(),Stage::eUpscaled,expected,why,expectedSource));
 CHECK(expected.color==frame.color&&expected.output==frame.output&&expected.depth==frame.depth&&expected.motion==frame.motion
  &&expected.exposure==frame.exposure&&expected.colorRect==frame.colorRect&&expected.outputRect==frame.outputRect
  &&expected.depthRect==frame.depthRect&&expected.motionRect==frame.motionRect&&expected.motionScaleX==frame.motionScaleX
  &&expected.motionScaleY==frame.motionScaleY&&expected.preExposure==frame.preExposure&&expected.depthInverted==frame.depthInverted
  &&expected.reset==frame.reset&&expected.width==frame.width&&expected.height==frame.height);
 // A negative subrect key stays negative, so the D3D12 stage refuses it as it would the game's block.
 Block negative;negative.Set("DLSS.Input.MV.Subrect.Base.X",-1);
 Block copied;CopyScalars(&negative,copied);
 copied.Set("Output",output12);copied.Set("Depth",depth12);copied.Set("MotionVectors",motion12);
 CHECK(!shared::ReadFrame(&copied,Keys(),Stage::eUpscaled,frame,why,source)&&why.gap==InputGap::eSubrectKey);
 // Pointer types: a D3D12 resource reads back as one (and as void*), never as a D3D11 resource.
 Block block;ID3D12Resource* typed=nullptr;
 block.Set("Color",output12);
 CHECK(block.Get("Color",&typed)==NVSDK_NGX_Result_Success&&typed==output12&&block.Resource12("Color")==output12);
 CHECK(block.Get("Color",&raw)==NVSDK_NGX_Result_Success&&raw==output12&&block.Get("Color",&leaked)!=NVSDK_NGX_Result_Success);
 block.Set("Color",reinterpret_cast<void*>(uintptr_t(0x5)));
 CHECK(block.Get("Color",&typed)!=NVSDK_NGX_Result_Success&&block.Get("Color",&raw)==NVSDK_NGX_Result_Success);
 // Fixed storage: keys beyond its capacity and names past 63 bytes are dropped.
 Block full;
 for(unsigned n=0;n<Block::kEntries+8;++n)full.Set(("key"+std::to_string(n)).c_str(),n);
 CHECK(full.Count()==Block::kEntries);
 unsigned last=0;CHECK(full.Get(("key"+std::to_string(Block::kEntries-1)).c_str(),&last)==NVSDK_NGX_Result_Success&&last==Block::kEntries-1);
 full.Reset();CHECK(full.Count()==0);
 full.Set(std::string(80,'x').c_str(),1u);CHECK(full.Count()==0);
 CHECK(std::size(kScalarKeys)+std::size(kResourceKeys)<=Block::kEntries);
}

void OutOfDate(){
 CHECK(kD3D11Route>=8&&kD3D11Route!=kOptiScalerRoute);
 // Native NGX's outdated-driver result never leads the bridge to the direct provider.
 CHECK(!DirectFallbackAllowed(kD3D11Route,kResultOutOfDate));
 for(const uint32_t other:{kResultFeatureNotSupported,kResultFeatureNotFound,kResultPlatformError,kResultDenied,0xBAD0000Bu,0xBAD00007u})
  CHECK(DirectFallbackAllowed(kD3D11Route,other));
 // The D3D12 routes keep dlss5.cpp's fallback in this build.
 for(const size_t route:{size_t(0),size_t(7),kOptiScalerRoute})CHECK(DirectFallbackAllowed(route,kResultOutOfDate));
 // Any failure of the bridge device's own NGX initialization keeps the bridge off; its state follows the result.
 // NGX success is 0x1 (NVSDK_NGX_Result_Success), not 0; 0 and any other unknown value block too.
 CHECK(InitBlocks(kResultOutOfDate)&&InitBlocks(0xBAD00007u)&&InitBlocks(kProviderFault)&&InitBlocks(kResultFeatureNotFound)
     &&!InitBlocks(0x1u)&&!InitBlocks(static_cast<uint32_t>(NVSDK_NGX_Result_Success))&&InitBlocks(0));
 CHECK(CreateFailureState(kResultOutOfDate)==State::eDriverUpdate&&CreateFailureState(kResultFeatureNotFound)==State::eNotInstalled);
 CHECK(CreateFailureState(kProviderFault)==State::eCreateFailed&&CreateFailureState(0xBAD00007u)==State::eCreateFailed);
 // Device refusals block DLSS 5; an untracked feature only explains why it waits.
 for(const Refusal refusal:{Refusal::eDeferredContext,Refusal::eNoSharedFence,Refusal::eDevice,Refusal::eSharing})CHECK(Blocking(refusal));
 CHECK(!Blocking(Refusal::eNone)&&!Blocking(Refusal::eUntracked));
}

// The depth pass on WARP: each depth format becomes R32_FLOAT, and the game's compute shader, shader resource 0 and
// unordered access view 0 come back (slot 1 is never touched).
void Depth(){
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
 CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,&device,nullptr,&context)));
 constexpr UINT kSize=37; // not a multiple of the pass's 8x8 groups
 const auto texture=[&](DXGI_FORMAT format,UINT bind,const D3D11_SUBRESOURCE_DATA* data=nullptr,D3D11_USAGE usage=D3D11_USAGE_DEFAULT,UINT cpu=0){
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=kSize;desc.MipLevels=desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;
  desc.Usage=usage;desc.BindFlags=bind;desc.CPUAccessFlags=cpu;ComPtr<ID3D11Texture2D> made;
  CHECK(SUCCEEDED(device->CreateTexture2D(&desc,data,&made)));return made;
 };
 // The game's compute state: its own shader, SRV 0 and 1, UAV 0 and 1.
 const char kGame[]="RWTexture2D<float> o:register(u0);[numthreads(1,1,1)]void Main(uint3 i:SV_DispatchThreadID){o[i.xy]=1;}";
 ComPtr<ID3DBlob> code,error;
 CHECK(SUCCEEDED(D3DCompile(kGame,sizeof(kGame)-1,"game",nullptr,nullptr,"Main","cs_5_0",0,0,&code,&error)));
 ComPtr<ID3D11ComputeShader> gameShader;CHECK(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&gameShader)));
 auto gameTexture=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
 auto gameTexture2=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
 ComPtr<ID3D11ShaderResourceView> gameView,gameView1;ComPtr<ID3D11UnorderedAccessView> gameAccess,gameAccess1;
 CHECK(SUCCEEDED(device->CreateShaderResourceView(gameTexture.Get(),nullptr,&gameView)));
 CHECK(SUCCEEDED(device->CreateUnorderedAccessView(gameTexture2.Get(),nullptr,&gameAccess)));
 auto spare=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
 auto spare2=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
 CHECK(SUCCEEDED(device->CreateShaderResourceView(spare.Get(),nullptr,&gameView1)));
 CHECK(SUCCEEDED(device->CreateUnorderedAccessView(spare2.Get(),nullptr,&gameAccess1)));
 const auto bindGame=[&]{
  ID3D11ShaderResourceView* views[]{gameView.Get(),gameView1.Get()};ID3D11UnorderedAccessView* accesses[]{gameAccess.Get(),gameAccess1.Get()};
  context->CSSetShader(gameShader.Get(),nullptr,0);context->CSSetShaderResources(0,2,views);context->CSSetUnorderedAccessViews(0,2,accesses,nullptr);
 };
 const auto gameBound=[&]{
  ComPtr<ID3D11ComputeShader> shader;ID3D11ShaderResourceView* views[2]{};ID3D11UnorderedAccessView* accesses[2]{};
  context->CSGetShader(&shader,nullptr,nullptr);context->CSGetShaderResources(0,2,views);context->CSGetUnorderedAccessViews(0,2,accesses);
  const bool same=shader.Get()==gameShader.Get()&&views[0]==gameView.Get()&&views[1]==gameView1.Get()
   &&accesses[0]==gameAccess.Get()&&accesses[1]==gameAccess1.Get();
  for(auto* view:views)if(view)view->Release();
  for(auto* access:accesses)if(access)access->Release();
  return same;
 };
 auto target=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
 auto staging=texture(DXGI_FORMAT_R32_FLOAT,0,nullptr,D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ);
 const auto converted=[&](float expected,float tolerance){
  context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
  CHECK(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
  bool close=true;
  for(UINT y=0;y<kSize;++y)for(UINT x=0;x<kSize;++x){
   const float value=reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch)[x];
   const float want=expected>=0.0f?expected:0.1f+0.8f*y/kSize;
   close&=std::fabs(value-want)<=tolerance;
  }
  context->Unmap(staging.Get(),0);return close;
 };
 // Depth-stencil buffers as games bind them (typeless, depth-stencil and shader resource), cleared to a depth.
 struct Case{DXGI_FORMAT resource,view;float depth,tolerance;};
 const Case cases[]{{DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_D24_UNORM_S8_UINT,0.25f,1.0f/16777215.0f},
  {DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,0.75f,0.0f},{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,0.5f,0.0f},
  {DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_D16_UNORM,0.125f,1.0f/65535.0f}};
 for(const Case& test:cases){
  auto depth=texture(test.resource,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
  D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=test.view;dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11DepthStencilView> clear;CHECK(SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&dsv,&clear)));
  context->ClearDepthStencilView(clear.Get(),D3D11_CLEAR_DEPTH,test.depth,0);
  // Also while the game still has it bound as its depth target: the pass reads its own copy.
  context->OMSetRenderTargets(0,nullptr,clear.Get());
  bindGame();
  CHECK(dlss5::d3d11::testing::ConvertDepth(context.Get(),depth.Get(),target.Get()));
  CHECK(gameBound());
  ComPtr<ID3D11DepthStencilView> stillBound;context->OMGetRenderTargets(0,nullptr,&stillBound);CHECK(stillBound.Get()==clear.Get());
  context->OMSetRenderTargets(0,nullptr,nullptr);
  CHECK(converted(test.depth,test.tolerance));
 }
 // Depth kept in a color texture (R32_FLOAT, no depth binding), a vertical gradient.
 std::vector<float> gradient(size_t(kSize)*kSize);
 for(UINT y=0;y<kSize;++y)for(UINT x=0;x<kSize;++x)gradient[size_t(y)*kSize+x]=0.1f+0.8f*y/kSize;
 const D3D11_SUBRESOURCE_DATA data{gradient.data(),static_cast<UINT>(kSize*sizeof(float)),0};
 auto color=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,&data);
 bindGame();CHECK(dlss5::d3d11::testing::ConvertDepth(context.Get(),color.Get(),target.Get()));CHECK(gameBound());CHECK(converted(-1.0f,0.0f));
 // A predicate of the game's: the pass's copy and dispatch still run, and the predicate comes back.
 {
  D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;
  CHECK(SUCCEEDED(device->CreatePredicate(&query,&predicate)));
  context->Begin(predicate.Get());context->End(predicate.Get());
  BOOL passed=TRUE;
  for(unsigned n=0;n<100000&&context->GetData(predicate.Get(),&passed,sizeof(passed),0)==S_FALSE;++n)Sleep(0);
  for(const BOOL value:{FALSE,TRUE}){
   bindGame();context->SetPredication(predicate.Get(),value);
   CHECK(dlss5::d3d11::testing::ConvertDepth(context.Get(),color.Get(),target.Get()));
   ComPtr<ID3D11Predicate> after;BOOL afterValue=!value;context->GetPredication(&after,&afterValue);
   CHECK(after.Get()==predicate.Get()&&afterValue==value&&gameBound());
   context->SetPredication(nullptr,FALSE);
   CHECK(converted(-1.0f,0.0f));
  }
 }
 context->CSSetShader(nullptr,nullptr,0);CHECK(dlss5::d3d11::testing::ConvertDepth(context.Get(),color.Get(),target.Get()));
 ComPtr<ID3D11ComputeShader> none;context->CSGetShader(&none,nullptr,nullptr);CHECK(!none);
 // A format the pass does not read is refused before anything is bound.
 auto refused=texture(DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
 bindGame();CHECK(!dlss5::d3d11::testing::ConvertDepth(context.Get(),refused.Get(),target.Get()));CHECK(gameBound());
}

int main(int argc,char** argv){
 const std::string mode=argc>1?argv[1]:"";
 if(mode=="formats")Formats();
 else if(mode=="parameters")Parameters();
 else if(mode=="out-of-date")OutOfDate();
 else if(mode=="depth")Depth();
 else{std::fprintf(stderr,"usage: Dlss5D3D11Tests formats|parameters|out-of-date|depth\n");return 2;}
 std::printf("DLSS5 D3D11 %s checks=%u PASS\n",mode.c_str(),checks);
 return 0;
}
