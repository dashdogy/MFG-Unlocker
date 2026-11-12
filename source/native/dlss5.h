#pragma once
#include "dlss5_provider.h"
namespace dlss5 {
void Configure(Settings) noexcept;
Settings Requested() noexcept;
using LogCallback = void (*)(const wchar_t*);
void SetLogCallback(LogCallback) noexcept;
// The runtime method behind a queue's ExecuteCommandLists slot when that slot
// holds a table hook of this module (Witcher 3 hair, witcher_dots); null for
// any other slot. DLSS 5 then hooks that method instead of refusing our code.
using QueueForwardFn = void* (*)(ID3D12CommandQueue*) noexcept;
void SetQueueForward(QueueForwardFn) noexcept;
dlss_sr::CreateKeys ReadCreate(const NVSDK_NGX_Parameter*) noexcept;
// The route key of the game's calls into OptiScaler's own D3D12 NGX exports
// (nrouter.1); the NGX runtime routes are 0..7.
inline constexpr size_t kOptiScalerRoute=64;
// dx11nr.1: the route key of the game's DirectX 11 DLSS calls (the runtime's D3D11 exports), whose DLSS 5 runs on
// the bridge's own D3D12 device (dlss5_d3d11.h).
inline constexpr size_t kD3D11Route=65;
void GameCreated(size_t route,const NVSDK_NGX_Handle*,const dlss_sr::CreateKeys&) noexcept;
bool Owns(size_t route,const NVSDK_NGX_Handle*) noexcept;
// Logs the caller (module+offset) and the NGX runtime once per process.
void ForeignCreated(const void* caller=nullptr, HMODULE runtime=nullptr) noexcept;
// Render stage: the reconstruction's input color is replaced by our processed
// copy for exactly one evaluation; AfterEvaluate puts the game's back.
struct Substitution {
    size_t slot=SIZE_MAX;
    uintptr_t original=0;
    bool typed=false; // stored as ID3D12Resource* rather than void*
};
// expectedRevision (0: any): the settings revision the caller resolved its stage for (the DirectX 11 bridge); after
// a settings change in between, the call records nothing and the next evaluation runs with the new settings.
Substitution BeforeEvaluate(size_t route,const Api&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,uint64_t expectedRevision=0) noexcept;
void AfterEvaluate(size_t route,const Api&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,NVSDK_NGX_Result,const Substitution& =Substitution{},uint64_t expectedRevision=0) noexcept;
void BeforeRelease(size_t route,const NVSDK_NGX_Handle*) noexcept;
void AfterRelease(size_t route,const NVSDK_NGX_Handle*,NVSDK_NGX_Result) noexcept;
// Present stage. The swapchain layer calls Present for each application
// Present before forwarding it: the current image is processed on `queue`, the
// application's presentation queue, before the next layer (Streamline DLSS-G)
// reads it. The feature is created through the NGX route and on the device of
// the game's DLSS SR/RR evaluations.
struct PresentTarget {
    ID3D12CommandQueue* queue=nullptr;
    ID3D12Resource* buffer=nullptr; // the current image, in PRESENT
    uint32_t colorSpace=0;          // DXGI_COLOR_SPACE_TYPE of the swapchain
    uintptr_t owner=0;              // the presenting swapchain layer
};
void Present(const PresentTarget&) noexcept;
// Hook Method Present is on; Present needs no call otherwise.
bool PresentStageSelected() noexcept;
// The owner's images are about to be released (resize or destruction): waits
// up to 3 s for its submitted Present-stage work. False if it did not complete.
// Block Hudless ends with it until the stage processes a present again.
bool DrainPresent(uintptr_t owner) noexcept;
// Block Hudless: the last present of the chain that owns the stage was
// processed, so the game's HUD-less tag must not reach DLSS frame generation.
bool BlockHudless() noexcept;
struct Snapshot {
    Settings requested{};
    State state=State::eOff;
    // route: 1 native NGX, 2 direct provider. stage: the live feature's Hook Method.
    uint32_t result=0, route=0, stage=0, live=0, passes=0;
    // Stage extent and the network's extent (autorect.1: the rect it runs on,
    // Auto's level within the feature's processing extent).
    uint32_t width=0, height=0, extentWidth=0, extentHeight=0;
    bool providerDetected=false, pending=false;
    uint64_t creates=0, evaluations=0, failures=0;
    // DLSS 5 Auto: the network's Resolution Scale (Auto's level), the stage's GPU time
    // (-1 before the first measurement), its budget and the exposure gain.
    float scale=0.0f, gpuMs=-1.0f, budgetMs=0.0f, exposureGain=1.0f;
    // The real frame interval the GPU time is a share of (0 before the first frames).
    float frameMs=0.0f;
    // Boost: a network evaluation's GPU time, while gpuMs is the mean per
    // evaluation (-1 when Boost is not running).
    float boostNetworkMs=-1.0f;
    // Resolution Scale Auto holds its quality floor below the slider while the
    // stage stays over budget: the frame rate pays, not detail.
    bool scaleFloorHeld=false;
    // dx11nr.1: the game's DLSS runs on DirectX 11 and DLSS 5 through the DirectX 11 bridge; d3d11Refusal is why
    // the bridge leaves its evaluations as they are (dlss5_policy.h d3d11::Refusal, 0 none).
    bool d3d11=false;
    uint32_t d3d11Refusal=0;
};
Snapshot Read() noexcept;
// Whether DLSS 5 runs (State::eRunning), without Read()'s provider lookup.
bool Running() noexcept;
// Whether DLSS 5 can run in this process: on in the settings, or its provider (nvngx_dlssnr.dll) beside the game or
// MFG Unlocker, so the menu offers it (Snapshot::providerDetected's file check).
bool Offered() noexcept;
// Compare as the last evaluation applied it: the share of the width showing the
// original image (1 while the compare key is held, 0 with Compare off) and the
// GetTickCount64 time of that evaluation (0 before the first). With `swapped`
// (Swap sides) the original is right of the split instead.
struct CompareView { float split=0.0f; bool held=false; uint64_t tick=0; bool swapped=false; };
CompareView LastCompare() noexcept;
// The overlay drew the divider over the finished image this frame; for the
// next half second the evaluation draws none of its own.
void NoteOverlayDivider() noexcept;
// The Vulkan stage (dlss5_vulkan.cpp) runs on the settings,
// state, counters, input reading and log of this module. Its own lock is taken
// before this module's, never after it; the live view it publishes is read
// under a leaf lock of its own.
namespace shared {
Settings Wanted(uint64_t& revision) noexcept;
void SetState(State state, uint32_t result=0) noexcept;
// Counts and logs a failure ("DLSS 5 unavailable: ...").
void Failure(State state, uint32_t result) noexcept;
void Created(uint32_t passes) noexcept;
// One processed evaluation: Running, for Read() and the menu.
void Evaluated() noexcept;
bool Foreign() noexcept;
void Log(const wchar_t* message) noexcept;
std::wstring ProviderPath();
// "DLSS 5 cost: ..." at a feature's first measurement, then at most every 30 s, or when the cost
// moved by more than 25 % (at most once a second). No UI.
struct CostLog { float ms = -1.0f; unsigned long long tick = 0; };
void Cost(CostLog& log, uint32_t stage, uint32_t extentWidth, uint32_t extentHeight, float gpuMs, double frameMs) noexcept;
// The evaluation's inputs (the D3D12 stage's ReadFrame, API-neutral: resources are read as pointers).
bool ReadFrame(const NVSDK_NGX_Parameter*, const dlss_sr::CreateKeys&, Stage, Frame&, InputsWhy&, Source&) noexcept;
// "DLSS 5 inputs unavailable: ..." once per change of the reason.
void InputsUnavailable(const InputsWhy&, const Source&, const dlss_sr::CreateKeys&) noexcept;
void InputsUnavailable(const wchar_t* reason) noexcept;
void InputsAvailable() noexcept;
// The constants and resolved settings the D3D12 stage's shader passes use, for the same passes on Vulkan.
// With the HDR knee and the Blend, halo and debug settings of the decode
// pass (112 bytes, within Vulkan's guaranteed 128 push-constant bytes).
struct PassConstants {
    uint32_t originX,originY,width,height,extentWidth,extentHeight;
    uint32_t encoding,signal,domain,filter,reconstruction,flags;
    float whiteScale,exposure,preExposure,diffuseWhite;
    float adaptiveGain=1.0f,split=0.0f,knee=0.8f,blendStrength=1.0f;
    float colorStrength=1.0f,highlightLimit=0.0f,haloDark=0.0f,haloBright=0.0f;
    float haloRadius=2.0f;uint32_t debugView=0;float pad[2]{};
};
static_assert(sizeof(PassConstants)==112);
// unbounded: the color is HDR (ResolvedEncoding/IsHDR as the D3D12 stage decides);
// decode adds Compare (flags 32/64) and publishes it for LastCompare.
PassConstants Constants(const Frame&,const Settings&,uint32_t extentWidth,uint32_t extentHeight,Encoding automatic,
    bool unbounded,float adaptiveGain,bool decode,bool inPlace) noexcept;
// What a Vulkan feature reports for Read() while one is live.
struct Live {
    uint32_t live=0, route=0, stage=0, passes=0, width=0, height=0, extentWidth=0, extentHeight=0;
    bool pending=false;
    float scale=0.0f, gpuMs=-1.0f, budgetMs=0.0f, frameMs=0.0f, exposureGain=1.0f;
    // A Vulkan evaluation within the last second had a Hook Method without a Vulkan stage.
    bool stageUnavailable=false;
    // As Snapshot::scaleFloorHeld.
    bool scaleFloorHeld=false;
};
using LiveFn = bool (*)(Live&) noexcept;
void SetVulkanLive(LiveFn) noexcept;
// dx11nr.1: the DirectX 11 bridge (dlss5_d3d11.cpp) runs the D3D12 stage on its own device under the kD3D11Route
// slots; its lock is taken before this module's, never after it, and the view it publishes is read lock-free.
struct D3D11View {
    // A game DirectX 11 DLSS feature is tracked.
    bool tracked=false;
    // A DirectX 11 evaluation within the last second had Hook Method Present, which the bridge does not run.
    bool stageUnavailable=false;
    // NGX's D3D12 initialization of the bridge device failed with this result (0: none); the state follows
    // CreateFailureState (0xBAD0000C: Driver update required).
    uint32_t initFailure=0;
    uint32_t refusal=0;
};
using D3D11ViewFn = bool (*)(D3D11View&) noexcept;
void SetD3D11View(D3D11ViewFn) noexcept;
// Evaluations processed so far (Snapshot::evaluations), without Read()'s provider lookup.
uint64_t Evaluations() noexcept;
// Releases what Hook Method Off and released game features no longer hold, once their work completed, and the
// features of `route`'s slots too (the bridge's pass-through evaluations).
void ReleaseRoute(size_t route) noexcept;
// Whether an evaluation of `route`'s handle would do any work now, without recording: the slot is usable and
// `params` (null: not checked) hold inputs the stage at `stage` accepts. A refusal publishes its state as the
// evaluation would. The bridge asks this before any cross-API work.
bool CanEvaluate(size_t route,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* params,Stage stage) noexcept;
}
#if defined(MFG_DLSS5_TESTING)
namespace testing {void AllowSoftwareDevice() noexcept;void Collect() noexcept;void SetProviderPath(const wchar_t*);
// nrscale.1: the process's video memory in use and its budget as the slots read them (budget 0: DXGI's).
void SetVideoMemory(uint64_t usage,uint64_t budget) noexcept;
// autorect.1: pins Resolution Scale Auto's level (auto_mode::kLevels index) on every slot, as if its controller had
// stepped there; still clamped to the slider's range. SIZE_MAX hands it back to the controller.
void SetAutoLevel(size_t level) noexcept;
// The Present stage's refusal text for `object` on a native queue of `queueDevice` (empty: accepted).
std::wstring NativeRefusal(ID3D12DeviceChild* object,ID3D12Device* queueDevice);}
#endif
}
