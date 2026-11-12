#include "dlss5_d3d11.h"
#include "dlss5_d3d11_policy.h"
#include "dlss5_auto.h"
#if !defined(MFG_DLSS5_TESTING)
#include "single_module.h"
#endif
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace dlss5::d3d11 {
namespace {
// The bridge; taken before dlss5.cpp's lock (dlss5::shared, dlss5::*Evaluate), never after it, and never held
// across the game's own NGX call.
std::mutex gMutex;
// The view Read() takes under dlss5.cpp's lock: atomics only.
std::atomic<uint32_t> gTracked{0},gRefusal{0},gInitFailure{0};
std::atomic<uint64_t> gPresentTick{0},gProcessed{0};
// DirectProvider's application id; the bridge device's NGX data folder is under %TEMP%.
constexpr unsigned long long kApplicationId=0x24480451ull;
// Recordings in flight on the bridge queue; the next waits up to kListWaitMs for the oldest, else the frame is left.
constexpr uint32_t kLists=4;
constexpr DWORD kListWaitMs=250;
void Log(const wchar_t* message) noexcept {shared::Log(message);}
// System32's D3D12CreateDevice, resolved at run time (no d3d12 import); the tests link d3d12 themselves.
PFN_D3D12_CREATE_DEVICE CreateDeviceEntry() noexcept {
#if defined(MFG_DLSS5_TESTING)
    return &D3D12CreateDevice;
#else
    return reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(single_module::LoadSystemModule(L"d3d12.dll"),"D3D12CreateDevice"));
#endif
}
bool View(shared::D3D11View& view) noexcept {
    view.tracked=gTracked.load(std::memory_order_acquire)!=0;
    view.refusal=gRefusal.load(std::memory_order_acquire);
    view.initFailure=gInitFailure.load(std::memory_order_acquire);
    const uint64_t tick=gPresentTick.load(std::memory_order_acquire);
    view.stageUnavailable=tick && GetTickCount64()-tick<1000;
    return true;
}
void Register() noexcept {shared::SetD3D11View(&View);}
// Why the game's evaluations are left as they are: published at once, logged once per change of the reason.
void Refuse(Refusal why,const wchar_t* detail) noexcept {
    Register();
    const uint32_t value=static_cast<uint32_t>(why);
    if(gRefusal.exchange(value,std::memory_order_acq_rel)==value)return;
    wchar_t message[700]{};
    swprintf_s(message,L"DLSS 5 DirectX 11 bridge leaves the frame as is (refusal %u): %.600ls",value,detail);
    Log(message);
}
void Accept() noexcept {
    if(gRefusal.exchange(0,std::memory_order_acq_rel))Log(L"DLSS 5 DirectX 11 bridge: evaluations run through it again");
}

// One recording of the bridge queue; its allocator is reset once the fence passed `done`.
struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t done=0;
};
// The bridge's D3D12 device on the game's adapter, its queue and the fence both APIs signal in turn. Made on the
// first owned evaluation and kept, with its NGX initialization, for the process lifetime (NGX is never shut down on
// it). A failed NGX initialization (initFailure) keeps the bridge off for the process.
struct Core {
    bool tried=false,ready=false;
    uint32_t initFailure=0;
    LUID luid{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    // The fence's NT handle, for every game D3D11 device that opens it.
    HANDLE shared=nullptr,event=nullptr;
    uint64_t value=0;
    std::array<List,kLists> lists{};
    uint32_t next=0;
} gCore;
// A texture of the bridge device shared with the game's D3D11 device (whole, one mip). Rests in COMMON between
// submissions, so either API may use it once the other's fence passed.
struct Shared {
    ComPtr<ID3D12Resource> d3d12;
    ComPtr<ID3D11Texture2D> d3d11;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t width=0,height=0;
    explicit operator bool() const noexcept {return d3d12 && d3d11;}
};
struct DepthPass {
    ComPtr<ID3D11Texture2D> copy;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11Resource> targetResource;
};
// The game's D3D11 device; `refused` keeps a refusal of it so it is not retried per frame.
struct GameDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11ComputeShader> convert;
    Refusal refused=Refusal::eNone;
    const wchar_t* reason=L"";
} gDevice;
// One tracked game DLSS feature's shared textures, at its own sizes (a scope and the main view never recreate each
// other's), retired with that feature's release.
struct Set {
    uintptr_t handle=0;
    // color: Before upscaling's input; output: After upscaling's; processed: DLSS 5's result for Before upscaling.
    Shared color,output,depth,motion,exposure,processed;
    DepthPass depthPass;
    // The fence value after the last bridge-queue work that used this set.
    uint64_t last=0;
};
// dlss5.cpp tracks at most 8 DLSS features in all.
std::array<Set,8> gSets;
// Bridge-device textures the GPU may still use, released once the fence passed `after`.
struct Retired { ComPtr<ID3D12Resource> resource; uint64_t after=0; };
std::vector<Retired> gRetired;

void Retire(Shared& texture,uint64_t after) noexcept {
    if(texture.d3d12){
        try{gRetired.push_back({texture.d3d12,after});}
        catch(...){(void)texture.d3d12.Detach();} // kept for the process rather than released early
    }
    texture=Shared{};
}
void CollectRetired() noexcept {
    if(gRetired.empty() || !gCore.fence)return;
    const uint64_t completed=gCore.fence->GetCompletedValue();
    std::erase_if(gRetired,[completed](const Retired& entry){return entry.after<=completed;});
}
bool Holds(const Set& set) noexcept {
    return set.color || set.output || set.depth || set.motion || set.exposure || set.processed || set.depthPass.copy;
}
// The set's textures retire (released once the fence passed its last work); its handle stays.
void ReleaseTextures(Set& set) noexcept {
    for(Shared* texture:{&set.color,&set.output,&set.depth,&set.motion,&set.exposure,&set.processed})Retire(*texture,set.last);
    set.depthPass=DepthPass{};
}
// The game feature `handle`'s set, made on its first evaluation (null when all are taken).
Set* SetOf(const NVSDK_NGX_Handle* handle) noexcept {
    const uintptr_t key=reinterpret_cast<uintptr_t>(handle);
    for(Set& set:gSets)if(set.handle==key)return &set;
    for(Set& set:gSets)if(!set.handle){set.handle=key;return &set;}
    return nullptr;
}
void RetireSet(const NVSDK_NGX_Handle* handle) noexcept {
    const uintptr_t key=reinterpret_cast<uintptr_t>(handle);
    for(Set& set:gSets)if(set.handle==key){ReleaseTextures(set);set=Set{};}
}
// Every set's textures retire (they were opened on that device) and the references to the game's device go.
void DropDevice() noexcept {
    for(Set& set:gSets)ReleaseTextures(set);
    gDevice=GameDevice{};
}

// The game's resource under `key`: ID3D11Resource*, or void* as some engines store it (`typed` says which).
ID3D11Resource* GameResource(const NVSDK_NGX_Parameter* params,const char* key,bool* typed=nullptr) noexcept {
    __try {
        ID3D11Resource* value=nullptr;void* raw=nullptr;
        if(params->Get(key,&value)==NVSDK_NGX_Result_Success && value){if(typed)*typed=true;return value;}
        if(params->Get(key,&raw)==NVSDK_NGX_Result_Success && raw){if(typed)*typed=false;return static_cast<ID3D11Resource*>(raw);}
    }__except(EXCEPTION_EXECUTE_HANDLER){}
    return nullptr;
}
// Writes the game's resource `key` with the type it was stored with, and reads it back.
bool SetGameResource(const NVSDK_NGX_Parameter* params,const char* key,ID3D11Resource* value,bool typed) noexcept {
    __try {
        auto* writable=const_cast<NVSDK_NGX_Parameter*>(params);
        if(typed)writable->Set(key,value);
        else writable->Set(key,static_cast<void*>(value));
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    return GameResource(params,key)==value;
}
ID3D11Device* OwnerOf(ID3D11Resource* resource) noexcept {
    __try {ID3D11Device* owner=nullptr;resource->GetDevice(&owner);return owner;}
    __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
// A single-sampled 2D texture of `device`; the bridge copies mip 0 of its first slice.
bool Describe(ID3D11Resource* resource,ID3D11Device* device,D3D11_TEXTURE2D_DESC& desc) noexcept {
    if(!resource || !device)return false;
    __try {
        D3D11_RESOURCE_DIMENSION dimension=D3D11_RESOURCE_DIMENSION_UNKNOWN;resource->GetType(&dimension);
        if(dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D)return false;
        ID3D11Device* owner=nullptr;resource->GetDevice(&owner);
        const bool same=owner==device;if(owner)owner->Release();
        if(!same)return false;
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        return desc.Width && desc.Height && desc.SampleDesc.Count==1;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
NVSDK_NGX_Result CallInit(InitFn init,const wchar_t* path,ID3D12Device* device) noexcept {
    __try {return init(kApplicationId,path,device,NVSDK_NGX_Version_API,nullptr);}
    __except(EXCEPTION_EXECUTE_HANDLER){return static_cast<NVSDK_NGX_Result>(kProviderFault);}
}
bool AdapterOf(ID3D11Device* device,LUID& luid) noexcept {
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))
        || FAILED(adapter->GetDesc(&desc)))return false;
    luid=desc.AdapterLuid;return true;
}
bool SameLuid(const LUID& left,const LUID& right) noexcept {return left.LowPart==right.LowPart && left.HighPart==right.HighPart;}
std::wstring DataPath() {
    wchar_t temp[MAX_PATH+1]{};
    const DWORD length=GetTempPathW(MAX_PATH+1,temp);
    const std::wstring base=length && length<=MAX_PATH?std::wstring(temp,length):std::wstring(L".\\");
    const std::wstring folder=base+L"MFGUnlocker-DLSS5";
    return CreateDirectoryW(folder.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS ? folder : base;
}
// The bridge device: made on the first call (with its NGX initialization), then only checked to sit on the same adapter.
bool CoreFor(const Api& api,ID3D11Device* device) {
    LUID luid{};
    if(!AdapterOf(device,luid)){Refuse(Refusal::eDevice,L"the game's DirectX 11 device names no DXGI adapter");return false;}
    if(gCore.tried){
        // A failed NGX initialization is published as its state (View), not as a refusal.
        if(gCore.initFailure)return false;
        if(!gCore.ready){Refuse(Refusal::eDevice,L"the bridge's DirectX 12 device is unavailable (its refusal was logged)");return false;}
        if(!SameLuid(luid,gCore.luid)){
            Refuse(Refusal::eDevice,L"the game's DirectX 11 device is on another adapter than the bridge's DirectX 12 device");return false;
        }
        return true;
    }
    gCore.tried=true;gCore.luid=luid;
    const wchar_t* step=nullptr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)))
        || FAILED(adapter->GetDesc1(&desc)))step=L"adapter";
    else if(desc.VendorId!=0x10DE || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))step=L"adapter is not NVIDIA";
    else if(const auto create=CreateDeviceEntry();!create)step=L"D3D12CreateDevice entry";
    else if(FAILED(create(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&gCore.device))))step=L"D3D12CreateDevice";
    else if(gCore.device->GetNodeCount()!=1)step=L"multi-node device";
    else {
        D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if(FAILED(gCore.device->CreateCommandQueue(&queue,IID_PPV_ARGS(&gCore.queue))))step=L"queue";
        else if(FAILED(gCore.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&gCore.fence)))
            || FAILED(gCore.device->CreateSharedHandle(gCore.fence.Get(),nullptr,GENERIC_ALL,nullptr,&gCore.shared)))step=L"shared fence";
        else if(!(gCore.event=CreateEventW(nullptr,FALSE,FALSE,nullptr)))step=L"event";
        else for(auto& entry:gCore.lists)
            if(FAILED(gCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&entry.allocator)))
                || FAILED(gCore.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,entry.allocator.Get(),nullptr,IID_PPV_ARGS(&entry.list)))
                || FAILED(entry.list->Close())){step=L"command lists";break;}
    }
    if(step){
        wchar_t message[400]{};
        swprintf_s(message,L"DLSS 5 DirectX 11 bridge: its DirectX 12 device was refused: step=%ls adapter=%.128ls",step,desc.Description);
        Log(message);
        Refuse(Refusal::eDevice,L"the bridge's DirectX 12 device was refused (logged before)");return false;
    }
    // NGX on the bridge device through the runtime's own Init_Ext (this device is never prepared as the game's). If the
    // native DLSSNR create fails, dlss5.cpp falls back to the provider beside the game (DirectFallbackAllowed).
    const InitFn init=api.init?api.init(api.runtime):nullptr;
    const std::wstring path=DataPath();
    const NVSDK_NGX_Result result=init?CallInit(init,path.c_str(),gCore.device.Get()):NVSDK_NGX_Result_FAIL_NotImplemented;
    wchar_t message[1500]{};
    swprintf_s(message,L"DLSS 5 DirectX 11 bridge: DirectX 12 device on %.128ls (LUID %08X:%08X), NGX D3D12 initialization 0x%08X%ls data=%.1000ls",
        desc.Description,static_cast<unsigned>(luid.HighPart),luid.LowPart,static_cast<unsigned>(result),
        init?L"":L" (the runtime exports no NVSDK_NGX_D3D12_Init_Ext)",path.c_str());
    Log(message);
    if(InitBlocks(static_cast<uint32_t>(result))){
        // Fail closed for the process: the native create and the direct provider both need NGX on this device. The
        // state follows the result (0xBAD0000C: Driver update required); an undocumented 0 is kept as a platform error.
        gCore.initFailure=static_cast<uint32_t>(result)?static_cast<uint32_t>(result)
            :static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_PlatformError);
        gInitFailure.store(gCore.initFailure,std::memory_order_release);Register();
        Log(L"DLSS 5 DirectX 11 bridge: NGX did not initialize on the bridge device, so DLSS 5 stays off in this DirectX 11 game");
        shared::Failure(CreateFailureState(gCore.initFailure),gCore.initFailure);
        return false;
    }
    gCore.ready=true;return true;
}
bool TypedLoadStore(DXGI_FORMAT format) noexcept {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return SUCCEEDED(gCore.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))
        && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}
// The depth pass: the depth view's first channel into the shared R32_FLOAT texture. Single exit (fxc).
const char kDepthShader[]=R"(
Texture2D<float> source:register(t0);
RWTexture2D<float> target:register(u0);
[numthreads(8,8,1)] void Convert(uint3 id:SV_DispatchThreadID){
 uint width,height;target.GetDimensions(width,height);
 if(id.x<width&&id.y<height)target[id.xy]=source.Load(int3(id.xy,0));
}
)";
ComPtr<ID3DBlob> gDepthCode;
bool gDepthCompiled=false;
bool ConvertShader(ID3D11Device* device,ComPtr<ID3D11ComputeShader>& shader) noexcept {
    if(shader)return true;
    if(!gDepthCompiled){
        gDepthCompiled=true;ComPtr<ID3DBlob> error;
        if(FAILED(D3DCompile(kDepthShader,sizeof(kDepthShader)-1,"MFGUnlocker DLSS 5 DirectX 11 depth",nullptr,nullptr,"Convert","cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&gDepthCode,&error))){
            gDepthCode.Reset();
            wchar_t message[1024]{};
            swprintf_s(message,L"DLSS 5 DirectX 11 depth shader compile failed: %.900hs",error?static_cast<const char*>(error->GetBufferPointer()):"-");
            Log(message);
        }
    }
    return gDepthCode && SUCCEEDED(device->CreateComputeShader(gDepthCode->GetBufferPointer(),gDepthCode->GetBufferSize(),nullptr,&shader));
}
// The context's compute shader (with its class instances), shader resource 0 and unordered access view 0, put back
// as they were when this goes out of scope (the views' hidden counters kept).
struct ComputeState {
    ID3D11DeviceContext* context;
    ID3D11ComputeShader* shader=nullptr;
    ID3D11ClassInstance* instances[256]{};
    UINT instanceCount=256;
    ID3D11ShaderResourceView* resource=nullptr;
    ID3D11UnorderedAccessView* access=nullptr;
    explicit ComputeState(ID3D11DeviceContext* saved) noexcept:context(saved) {
        context->CSGetShader(&shader,instances,&instanceCount);
        context->CSGetShaderResources(0,1,&resource);
        context->CSGetUnorderedAccessViews(0,1,&access);
    }
    ~ComputeState() {
        // Ours go first, so the game's views come back without a read/write conflict between them.
        ID3D11ShaderResourceView* noResource=nullptr;ID3D11UnorderedAccessView* noAccess=nullptr;const UINT keep=UINT(-1);
        context->CSSetShaderResources(0,1,&noResource);
        context->CSSetUnorderedAccessViews(0,1,&noAccess,&keep);
        context->CSSetShader(shader,instanceCount?instances:nullptr,instanceCount);
        context->CSSetShaderResources(0,1,&resource);
        context->CSSetUnorderedAccessViews(0,1,&access,&keep);
        if(shader)shader->Release();
        for(UINT n=0;n<instanceCount && n<256;++n)if(instances[n])instances[n]->Release();
        if(resource)resource->Release();
        if(access)access->Release();
    }
    ComputeState(const ComputeState&)=delete;
    ComputeState& operator=(const ComputeState&)=delete;
};
// The game's predicate is suspended while the bridge works on its context: D3D11 predicates copies and dispatches
// but not fence signals and waits, so a predicated copy would hand DLSS 5 (or the game) a stale texture.
struct Unpredicated {
    ID3D11DeviceContext* context;
    ID3D11Predicate* predicate=nullptr;
    BOOL value=FALSE;
    explicit Unpredicated(ID3D11DeviceContext* suspended) noexcept:context(suspended) {
        context->GetPredication(&predicate,&value);
        if(predicate)context->SetPredication(nullptr,FALSE);
    }
    ~Unpredicated() {
        if(!predicate)return;
        context->SetPredication(predicate,value);
        predicate->Release();
    }
    Unpredicated(const Unpredicated&)=delete;
    Unpredicated& operator=(const Unpredicated&)=delete;
};
// The game's depth (mip 0) as R32_FLOAT in `target`: copied whole into our texture of its typeless family (never
// bound for output, so no binding of the game's conflicts with it), then read through its depth view.
bool RunDepthPass(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11ComputeShader* shader,ID3D11Resource* source,
    const D3D11_TEXTURE2D_DESC& desc,ID3D11Resource* target,DepthPass& pass) noexcept {
    const DepthFormats formats=DepthFormatsFor(desc.Format);
    if(!shader || !target || formats.copy==DXGI_FORMAT_UNKNOWN)return false;
    D3D11_TEXTURE2D_DESC held{};
    if(pass.copy)pass.copy->GetDesc(&held);
    if(!pass.copy || held.Width!=desc.Width || held.Height!=desc.Height || held.Format!=formats.copy){
        pass.copy.Reset();pass.view.Reset();
        D3D11_TEXTURE2D_DESC copy{};copy.Width=desc.Width;copy.Height=desc.Height;copy.MipLevels=1;copy.ArraySize=1;
        copy.Format=formats.copy;copy.SampleDesc.Count=1;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=formats.view;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        if(FAILED(device->CreateTexture2D(&copy,nullptr,&pass.copy)) || FAILED(device->CreateShaderResourceView(pass.copy.Get(),&view,&pass.view))){
            pass.copy.Reset();pass.view.Reset();return false;
        }
    }
    if(pass.targetResource.Get()!=target){
        pass.target.Reset();pass.targetResource=target;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT;view.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
        if(FAILED(device->CreateUnorderedAccessView(target,&view,&pass.target))){pass.targetResource.Reset();return false;}
    }
    context->CopySubresourceRegion(pass.copy.Get(),0,0,0,0,source,0,nullptr);
    ComputeState saved(context);
    ID3D11ShaderResourceView* resource=pass.view.Get();ID3D11UnorderedAccessView* access=pass.target.Get();const UINT keep=UINT(-1);
    context->CSSetShader(shader,nullptr,0);
    context->CSSetShaderResources(0,1,&resource);
    context->CSSetUnorderedAccessViews(0,1,&access,&keep);
    context->Dispatch((desc.Width+7)/8,(desc.Height+7)/8,1);
    return true;
}
bool DeviceFor(ID3D11Device* device) {
    if(gDevice.device.Get()==device){
        if(gDevice.refused!=Refusal::eNone){Refuse(gDevice.refused,gDevice.reason);return false;}
        return true;
    }
    if(gDevice.device){
        Log(L"DLSS 5 DirectX 11 bridge: the game's DirectX 11 device changed; the previous one's shared textures go once their work completed");
        DropDevice();
    }
    gDevice.device=device;
    const auto refuse=[](Refusal why,const wchar_t* reason){gDevice.refused=why;gDevice.reason=reason;Refuse(why,reason);return false;};
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&gDevice.device1))) || FAILED(device->QueryInterface(IID_PPV_ARGS(&gDevice.device5))))
        return refuse(Refusal::eNoSharedFence,L"the game's DirectX 11 device has no ID3D11Device5 (D3D11.4 shared fences)");
    if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)
        return refuse(Refusal::eDevice,L"the game's DirectX 11 device is below feature level 11_0 (no compute pass for its depth)");
    if(FAILED(gDevice.device5->OpenSharedFence(gCore.shared,IID_PPV_ARGS(&gDevice.fence))))
        return refuse(Refusal::eSharing,L"the bridge's shared fence could not be opened on the game's DirectX 11 device");
    if(!ConvertShader(device,gDevice.convert))return refuse(Refusal::eDevice,L"the depth pass's compute shader could not be made");
    return true;
}
// A texture of the bridge device shared with the game's device; its predecessor in this set retires.
// D3D11 opens a D3D12 texture only when it allows render targets and simultaneous access (on WARP, UAV alone or
// render target alone fails OpenSharedResource1 with E_INVALIDARG for R11G11B10/R32F/RG16F).
bool Ensure(Set& set,Shared& texture,uint32_t width,uint32_t height,DXGI_FORMAT format,bool unordered) noexcept {
    if(texture && texture.width==width && texture.height==height && texture.format==format)return true;
    Retire(texture,set.last);
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
        |(unordered?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    HANDLE handle=nullptr;ComPtr<ID3D12Resource> resource;ComPtr<ID3D11Texture2D> view;
    const wchar_t* step=L"CreateCommittedResource";
    HRESULT hr=gCore.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,
        nullptr,IID_PPV_ARGS(&resource));
    if(SUCCEEDED(hr)){step=L"CreateSharedHandle";hr=gCore.device->CreateSharedHandle(resource.Get(),nullptr,GENERIC_ALL,nullptr,&handle);}
    if(SUCCEEDED(hr)){step=L"OpenSharedResource1";hr=gDevice.device1->OpenSharedResource1(handle,IID_PPV_ARGS(&view));}
    if(handle)CloseHandle(handle);
    if(FAILED(hr)){
        static uint64_t last=0;
        const uint64_t key=(uint64_t(static_cast<uint32_t>(hr))<<32)^(uint64_t(format)<<16)^(uint64_t(desc.Flags)<<8)
            ^reinterpret_cast<uintptr_t>(step);
        if(key!=last){
            last=key;
            wchar_t message[300]{};
            swprintf_s(message,L"DLSS 5 DirectX 11 bridge: shared texture failed at %ls hr=0x%08X format=%u flags=0x%X size=%ux%u",
                step,static_cast<unsigned>(hr),static_cast<unsigned>(format),static_cast<unsigned>(desc.Flags),width,height);
            Log(message);
        }
        return false;
    }
    texture.d3d12=resource;texture.d3d11=view;texture.width=width;texture.height=height;texture.format=format;
    return true;
}
// The bridge queue's next recording, open: its previous work completed (waited for up to kListWaitMs).
List* NextList() noexcept {
    List& entry=gCore.lists[gCore.next];
    if(gCore.fence->GetCompletedValue()<entry.done){
        if(SUCCEEDED(gCore.fence->SetEventOnCompletion(entry.done,gCore.event)))WaitForSingleObject(gCore.event,kListWaitMs);
        // An earlier timed-out wait can leave the event set: the fence itself decides.
        if(gCore.fence->GetCompletedValue()<entry.done)return nullptr;
    }
    if(FAILED(entry.allocator->Reset()) || FAILED(entry.list->Reset(entry.allocator.Get(),nullptr)))return nullptr;
    gCore.next=(gCore.next+1)%kLists;
    return &entry;
}
struct Transition { ID3D12Resource* resource; D3D12_RESOURCE_STATES before,after; };
void Barriers(ID3D12GraphicsCommandList* cmd,std::initializer_list<Transition> transitions) noexcept {
    D3D12_RESOURCE_BARRIER barriers[8]{};UINT count=0;
    for(const Transition& transition:transitions){
        if(!transition.resource || transition.before==transition.after || count==8)continue;
        D3D12_RESOURCE_BARRIER& barrier=barriers[count++];barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={transition.resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,transition.before,transition.after};
    }
    if(count)cmd->ResourceBarrier(count,barriers);
}
// One evaluation through the bridge (gMutex held): after the game's call (After upscaling), or before it (Before
// upscaling: `processed` receives DLSS 5's input for the game's call). Nothing crosses the APIs if the stage refuses.
void Process(const Api& api,ID3D11DeviceContext* passed,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,
    Stage stage,uint64_t revision,ComPtr<ID3D11Texture2D>* processed) {
    const bool render=stage==Stage::eRender;
    CollectRetired();
    // A failed slot, another loader's provider or another component's DLSSNR: no cross-API work at all.
    if(!shared::CanEvaluate(kD3D11Route,handle,nullptr,stage))return;
    const auto missing=[](const wchar_t* reason) noexcept {shared::InputsUnavailable(reason);shared::SetState(State::eMissingInputs);};
    ID3D11Resource* color=GameResource(params,render?"Color":"Output");
    ID3D11Resource* depth=GameResource(params,"Depth");
    ID3D11Resource* motion=GameResource(params,"MotionVectors");
    ID3D11Resource* exposure=GameResource(params,"ExposureTexture");
    if(!color || !depth || !motion){
        missing(render?L"the DirectX 11 evaluation has no Color, Depth or MotionVectors resource"
            :L"the DirectX 11 evaluation has no Output, Depth or MotionVectors resource");
        return;
    }
    ComPtr<ID3D11Device> device;device.Attach(OwnerOf(color));
    ComPtr<ID3D11Device> called;passed->GetDevice(&called);
    if(!device){missing(L"the DirectX 11 DLSS output names no device");return;}
    // A layer (ReShade) may wrap the context the game passes: the bridge records on the immediate context of the
    // device its resources belong to, which must sit on the same adapter.
    LUID owner{},caller{};
    if(called.Get()!=device.Get() && (!AdapterOf(called.Get(),caller) || !AdapterOf(device.Get(),owner) || !SameLuid(owner,caller))){
        Refuse(Refusal::eDevice,L"the evaluation's DirectX 11 context and its resources are on different adapters");return;
    }
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    ComPtr<ID3D11DeviceContext4> context4;
    if(!context || FAILED(context.As(&context4))){
        Refuse(Refusal::eNoSharedFence,L"the game's immediate context has no ID3D11DeviceContext4 (D3D11.4 shared fences)");return;
    }
    if(!CoreFor(api,device.Get()))return;
    if(gCore.device->GetDeviceRemovedReason()!=S_OK){gCore.ready=false;Refuse(Refusal::eDevice,L"the bridge's DirectX 12 device was removed");return;}
    if(!DeviceFor(device.Get()))return;
    Set* const owned=SetOf(handle);
    if(!owned){Refuse(Refusal::eSharing,L"every shared texture set of the bridge is taken");return;}
    Set& set=*owned;
    D3D11_TEXTURE2D_DESC colorDesc{},depthDesc{},motionDesc{},exposureDesc{};
    if(!Describe(color,device.Get(),colorDesc) || !Describe(depth,device.Get(),depthDesc) || !Describe(motion,device.Get(),motionDesc)){
        missing(L"a DirectX 11 DLSS input is not a single-sampled 2D texture of the game's device");return;
    }
    const DXGI_FORMAT colorFormat=ColorFormat(colorDesc.Format),motionFormat=MotionFormat(motionDesc.Format);
    const DepthFormats depthFormats=DepthFormatsFor(depthDesc.Format);
    // After upscaling writes the output in place: typed UAV load and store, as the D3D12 stage requires.
    if(colorFormat==DXGI_FORMAT_UNKNOWN || depthFormats.copy==DXGI_FORMAT_UNKNOWN || motionFormat==DXGI_FORMAT_UNKNOWN
        || (!render && !TypedLoadStore(colorFormat))){
        wchar_t reason[300]{};
        swprintf_s(reason,L"the bridge does not share these DirectX 11 formats: %ls %u, depth %u, motion vectors %u",render?L"color":L"output",
            static_cast<unsigned>(colorDesc.Format),static_cast<unsigned>(depthDesc.Format),static_cast<unsigned>(motionDesc.Format));
        missing(reason);return;
    }
    if(exposure && (!Describe(exposure,device.Get(),exposureDesc) || ExposureFormat(exposureDesc.Format)==DXGI_FORMAT_UNKNOWN))exposure=nullptr;
    Shared& stageTexture=render?set.color:set.output;
    const wchar_t* failed=nullptr;
    if(!Ensure(set,stageTexture,colorDesc.Width,colorDesc.Height,colorFormat,!render))failed=render?L"color":L"output";
    else if(!Ensure(set,set.depth,depthDesc.Width,depthDesc.Height,DXGI_FORMAT_R32_FLOAT,true))failed=L"depth";
    else if(!Ensure(set,set.motion,motionDesc.Width,motionDesc.Height,motionFormat,false))failed=L"motion vector";
    else if(exposure && !Ensure(set,set.exposure,1,1,ExposureFormat(exposureDesc.Format),false))failed=L"exposure";
    else if(render && !Ensure(set,set.processed,colorDesc.Width,colorDesc.Height,DXGI_FORMAT_R16G16B16A16_FLOAT,false))failed=L"processed color";
    if(failed){
        wchar_t reason[200]{};
        swprintf_s(reason,L"the shared %ls texture could not be made on the bridge device or opened on the game's",failed);
        Refuse(Refusal::eSharing,reason);return;
    }
    // The D3D12 stage's parameters, checked before any cross-API work so refused inputs cost nothing per frame.
    ID3D12Resource* const exposureResource=exposure?set.exposure.d3d12.Get():nullptr;
    Block block{};CopyScalars(params,block);
    block.Set(render?"Color":"Output",stageTexture.d3d12.Get());
    block.Set("Depth",set.depth.d3d12.Get());
    block.Set("MotionVectors",set.motion.d3d12.Get());
    if(exposureResource)block.Set("ExposureTexture",exposureResource);
    if(!shared::CanEvaluate(kD3D11Route,handle,&block,stage))return;
    List* list=NextList();
    if(!list){
        static uint64_t logged=0;
        if(GetTickCount64()-logged>5000){logged=GetTickCount64();Log(L"DLSS 5 DirectX 11 bridge: its previous DirectX 12 work is still running; this frame is left as is");}
        return;
    }
    // From here every path closes the list; once the D3D12 stage recorded on it, it is submitted.
    ID3D12GraphicsCommandList* cmd=list->list.Get();
    const Unpredicated unpredicated(context.Get());
    // D3D11: the inputs into the shared set, then the fence value the bridge queue waits for.
    context->CopySubresourceRegion(stageTexture.d3d11.Get(),0,0,0,0,color,0,nullptr);
    const bool converted=RunDepthPass(device.Get(),context.Get(),gDevice.convert.Get(),depth,depthDesc,set.depth.d3d11.Get(),set.depthPass);
    context->CopySubresourceRegion(set.motion.d3d11.Get(),0,0,0,0,motion,0,nullptr);
    if(exposure){const D3D11_BOX texel{0,0,0,1,1,1};context->CopySubresourceRegion(set.exposure.d3d11.Get(),0,0,0,0,exposure,0,&texel);}
    const uint64_t ready=gCore.value+1;
    if(!converted || FAILED(context4->Signal(gDevice.fence.Get(),ready))){
        cmd->Close();
        Refuse(Refusal::eSharing,converted?L"the game's immediate context could not signal the shared fence"
            :L"the depth pass could not run on the game's DirectX 11 device");
        return;
    }
    gCore.value=ready;context->Flush();
    if(FAILED(gCore.queue->Wait(gCore.fence.Get(),ready))){
        cmd->Close();gCore.ready=false;Refuse(Refusal::eDevice,L"the bridge queue could not wait for the game's fence");return;
    }
    // D3D12: the shared set in the states the stage expects of a game's NGX inputs, and back to COMMON for D3D11.
    constexpr auto kCommon=D3D12_RESOURCE_STATE_COMMON,kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto stageState=render?kRead:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barriers(cmd,{{stageTexture.d3d12.Get(),kCommon,stageState},{set.depth.d3d12.Get(),kCommon,kRead},{set.motion.d3d12.Get(),kCommon,kRead},
        {exposureResource,kCommon,kRead}});
    const uint64_t evaluations=shared::Evaluations();
    bool substituted=false;
    // The stage calls take this evaluation's revision: after a settings change in between they record nothing.
    if(render){
        const Substitution substitution=dlss5::BeforeEvaluate(kD3D11Route,api.d3d12,cmd,handle,&block,revision);
        // The stage's processed input (NON_PIXEL_SHADER_RESOURCE until AfterEvaluate) into the texture the game reads.
        ID3D12Resource* input=substitution.slot!=SIZE_MAX?block.Resource12("Color"):nullptr;
        if(input && input!=stageTexture.d3d12.Get()){
            Barriers(cmd,{{input,kRead,D3D12_RESOURCE_STATE_COPY_SOURCE},{set.processed.d3d12.Get(),kCommon,D3D12_RESOURCE_STATE_COPY_DEST}});
            D3D12_TEXTURE_COPY_LOCATION to{set.processed.d3d12.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};to.SubresourceIndex=0;
            D3D12_TEXTURE_COPY_LOCATION from{input,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};from.SubresourceIndex=0;
            cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            Barriers(cmd,{{input,D3D12_RESOURCE_STATE_COPY_SOURCE,kRead},{set.processed.d3d12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,kCommon}});
            substituted=true;
        }
        // Our Color back and the stage's texture to its resting state: bookkeeping only (no game result to process).
        dlss5::AfterEvaluate(kD3D11Route,api.d3d12,cmd,handle,&block,NVSDK_NGX_Result_Fail,substitution,revision);
    }else dlss5::AfterEvaluate(kD3D11Route,api.d3d12,cmd,handle,&block,NVSDK_NGX_Result_Success,Substitution{},revision);
    Barriers(cmd,{{stageTexture.d3d12.Get(),stageState,kCommon},{set.depth.d3d12.Get(),kRead,kCommon},{set.motion.d3d12.Get(),kRead,kCommon},
        {exposureResource,kRead,kCommon}});
    if(FAILED(cmd->Close())){
        gCore.ready=false;Refuse(Refusal::eDevice,L"the bridge's DirectX 12 list could not be closed");return;
    }
    // Through the queue submission hook the D3D12 stage installed on this device: its fence proves the stage's work.
    ID3D12CommandList* lists[]{cmd};
    gCore.queue->ExecuteCommandLists(1,lists);
    const uint64_t done=ready+1;
    if(FAILED(gCore.queue->Signal(gCore.fence.Get(),done))){
        gCore.ready=false;Refuse(Refusal::eDevice,L"the bridge queue could not signal the shared fence");return;
    }
    gCore.value=done;list->done=done;set.last=done;
    // D3D11: nothing reads or writes the shared set before the bridge queue finished with it.
    if(FAILED(context4->Wait(gDevice.fence.Get(),done))){
        // Without the GPU wait the next copies could race the bridge queue: wait on the CPU instead, or stop.
        const bool waited=SUCCEEDED(gCore.fence->SetEventOnCompletion(done,gCore.event))
            && WaitForSingleObject(gCore.event,1000)==WAIT_OBJECT_0 && gCore.fence->GetCompletedValue()>=done;
        if(!waited){gCore.ready=false;Refuse(Refusal::eSharing,L"the game's immediate context could not wait for the shared fence");return;}
    }
    Accept();
    if(!render && shared::Evaluations()!=evaluations){
        context->CopySubresourceRegion(color,0,0,0,0,stageTexture.d3d11.Get(),0,nullptr);
        gProcessed.fetch_add(1,std::memory_order_relaxed);
    }
    if(render && substituted && processed){*processed=set.processed.d3d11;gProcessed.fetch_add(1,std::memory_order_relaxed);}
}
void Run(const Api& api,ID3D11DeviceContext* context,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,
    Stage stage,uint64_t revision,ComPtr<ID3D11Texture2D>* processed) noexcept {
    try {
        std::lock_guard lock(gMutex);
        Process(api,context,handle,params,stage,revision,processed);
    }catch(...){
        if(processed)processed->Reset();
        shared::Failure(State::eEvaluateFailed,kProviderFault);
    }
}
// DLSS 5 off, or Hook Method Present, which the bridge does not run: dlss5.cpp releases this route's features once
// their work completed, the bridge its shared textures (the device and its NGX initialization stay).
void Idle(const wchar_t* why) noexcept {
    shared::ReleaseRoute(kD3D11Route);
    std::lock_guard lock(gMutex);
    bool held=false;
    for(Set& set:gSets)if(Holds(set)){ReleaseTextures(set);held=true;}
    if(held){
        wchar_t message[200]{};swprintf_s(message,L"DLSS 5 %ls: the DirectX 11 bridge released its shared textures",why);Log(message);
    }
    CollectRetired();
}
void PresentUnavailable() noexcept {
    Register();
    const uint64_t now=GetTickCount64(),before=gPresentTick.exchange(now,std::memory_order_acq_rel);
    if(!before || now-before>5000)
        Log(L"DLSS 5 on DirectX 11: Hook Method Present is not available in this build (Before upscaling, After upscaling and Auto run); the frame is left as is");
    Idle(L"Present on DirectX 11");
}
}

void GameCreated(const NVSDK_NGX_Handle* handle,const dlss_sr::CreateKeys& keys) noexcept {
    if(!handle)return;
    Register();
    // A duplicate handle keeps its older slot (dlss5::GameCreated) and is counted once.
    const bool already=dlss5::Owns(kD3D11Route,handle);
    dlss5::GameCreated(kD3D11Route,handle,keys);
    if(already || !dlss5::Owns(kD3D11Route,handle))return;
    gTracked.fetch_add(1,std::memory_order_acq_rel);
    uint32_t untracked=static_cast<uint32_t>(Refusal::eUntracked);
    gRefusal.compare_exchange_strong(untracked,0,std::memory_order_acq_rel);
    wchar_t message[200]{};
    swprintf_s(message,L"DLSS 5 DirectX 11 route: the game's DLSS feature %p is tracked (%ux%u -> %ux%u)",
        static_cast<const void*>(handle),keys.width,keys.height,keys.outWidth,keys.outHeight);
    Log(message);
}
void UntrackedCreate() noexcept {
    Refuse(Refusal::eUntracked,L"the game created its DirectX 11 DLSS while another hook covered the NGX runtime's D3D11 entries; "
        L"DLSS 5 does not attach to a feature whose evaluation or release could pass it by");
}
NVSDK_NGX_Result Evaluate(const Api& api,ID3D11DeviceContext* context,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback callback) noexcept {
    if(!api.evaluate)return NVSDK_NGX_Result_FAIL_NotInitialized;
    const auto original=[&]() noexcept {return api.evaluate(context,handle,params,callback);};
    if(!context || !handle || !params)return original();
    if(!dlss5::Owns(kD3D11Route,handle)){
        // Its Create was not seen through the route, and DLSS 5 does not guess its keys.
        if(!gTracked.load(std::memory_order_acquire))
            Refuse(Refusal::eUntracked,L"the game evaluates a DirectX 11 DLSS feature whose creation the route did not see "
                L"(it was created before the NGX runtime's D3D11 entries were hooked); DLSS 5 does not attach to it");
        return original();
    }
    uint64_t revision=0;const Settings wanted=shared::Wanted(revision);
    if(!wanted.enabled){Idle(L"off");return original();}
    const Stage stage=wanted.stage==static_cast<uint32_t>(Stage::eAuto)?auto_mode::AutoStage():static_cast<Stage>(wanted.stage);
    if(stage==Stage::ePresent){PresentUnavailable();return original();}
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){
        Refuse(Refusal::eDeferredContext,L"the game evaluates DLSS on a deferred DirectX 11 context; the bridge records on the immediate context only");
        return original();
    }
    if(!api.d3d12.Complete()){Refuse(Refusal::eDevice,L"the NGX runtime's D3D12 entries for the bridge device are incomplete");return original();}
    if(stage!=Stage::eRender){
        const NVSDK_NGX_Result result=original();
        if(result==NVSDK_NGX_Result_Success)Run(api,context,handle,params,stage,revision,nullptr);
        return result;
    }
    // Before upscaling: DLSS 5's input stands in for the game's Color for this call only.
    ComPtr<ID3D11Texture2D> processed;
    Run(api,context,handle,params,stage,revision,&processed);
    bool typed=false;
    ID3D11Resource* const own=processed?GameResource(params,"Color",&typed):nullptr;
    if(!own || !SetGameResource(params,"Color",processed.Get(),typed)){
        if(own)SetGameResource(params,"Color",own,typed);
        return original();
    }
    const NVSDK_NGX_Result result=original();
    if(!SetGameResource(params,"Color",own,typed))Log(L"DLSS 5 DirectX 11 bridge: the game's Color could not be put back after Before upscaling");
    return result;
}
bool BeforeRelease(const NVSDK_NGX_Handle* handle) noexcept {
    const bool owned=handle && dlss5::Owns(kD3D11Route,handle);
    dlss5::BeforeRelease(kD3D11Route,handle);
    return owned;
}
void AfterRelease(const NVSDK_NGX_Handle* handle,NVSDK_NGX_Result result,bool owned) noexcept {
    dlss5::AfterRelease(kD3D11Route,handle,result);
    if(!owned || result!=NVSDK_NGX_Result_Success)return;
    uint32_t tracked=gTracked.load(std::memory_order_acquire);
    while(tracked && !gTracked.compare_exchange_weak(tracked,tracked-1,std::memory_order_acq_rel)){}
    std::lock_guard lock(gMutex);
    // This feature's shared textures retire; with the game's last DLSS feature, its device references too.
    RetireSet(handle);
    if(tracked==1)DropDevice();
    CollectRetired();
}
#if defined(MFG_DLSS5_TESTING)
namespace testing {
bool ConvertDepth(ID3D11DeviceContext* context,ID3D11Resource* source,ID3D11Resource* target) noexcept {
    if(!context || !source || !target)return false;
    std::lock_guard lock(gMutex);
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{};ComPtr<ID3D11ComputeShader> shader;DepthPass pass{};
    const Unpredicated unpredicated(context); // as the bridge runs it
    return Describe(source,device.Get(),desc) && ConvertShader(device.Get(),shader)
        && RunDepthPass(device.Get(),context,shader.Get(),source,desc,target,pass);
}
uint64_t Processed() noexcept {return gProcessed.load(std::memory_order_acquire);}
uint32_t Refused() noexcept {return gRefusal.load(std::memory_order_acquire);}
size_t Sets() noexcept {
    std::lock_guard lock(gMutex);
    size_t count=0;
    for(const Set& set:gSets)count+=set.handle && Holds(set);
    return count;
}
}
#endif
}
