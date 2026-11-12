#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <span>

struct ID3D12Device;

namespace xess_mod
{
std::span<const unsigned char> TensorCoreFatbin();
void InstallProviderTensorCores(HMODULE provider, ID3D12Device* device) noexcept;
struct Snapshot
{
    bool importsInstalled = false;
    bool providerLoaded = false;
    bool portReady = false;
    bool initialized = false;
    bool enabled = false;
    bool gameEnabled = false;
    bool composition = false;
    bool compositionManagedByGame = false;
    bool compositionRequested = false;
    bool presentStatusAvailable = false;
    bool providerEnabled = false;
    uint32_t applied = 0;
    uint32_t capacity = 0;
    uint32_t framesPresented = 0;
    uint32_t taggedResources = 0;
    uint32_t taggedFrameId = 0;
    uint32_t lastPresentId = 0;
    int32_t lastResult = 0;
    int32_t lastFrameGenResult = 0;
    std::array<uint64_t, 5> resourceAttempts{};
    std::array<uint64_t, 5> resourceAccepted{};
    std::array<int32_t, 5> resourceResults{};
    uint64_t generation = 0;
    uint64_t appFrames = 0;
    uint64_t resourceCalls = 0;
    uint64_t constantsCalls = 0;
    uint64_t lastStateTick = 0;
    // Untracked XeSS-FG contexts: Intel GPUs, an unprofiled pair, or OptiScaler's own.
    // Kept so the menu can tell the game is using XeSS-FG even without MFG.
    bool nativeEnabled = false;
    uint64_t nativeActivityTick = 0;
    uint32_t runtimeSource = 0; // xess_runtime::Source
    uint32_t adapterVendor = 0; // PCI vendor of the game device at XeSS-FG creation
    uint32_t modelActive = 0; // 1 our port of Intel's MFG model, 2 XeFG's 2x model, 0 none yet
    bool providerFailing = false; // the Nvngx replacement provider's recent batches all failed
    std::string runtimeFailure;
    std::string status = "Waiting for XeSS Frame Generation under OptiScaler.";
    std::string provider;
    std::string adapter;
};

bool InstallImports(HMODULE self) noexcept;
void DiscoverSdkModules() noexcept;
bool PrivateApiHandle(bool fg, void* handle) noexcept;
void InitializeOutsideLoaderLock() noexcept;
std::shared_ptr<const Snapshot> ReadSnapshot() noexcept;
// XeSS MFG runs under OptiScaler only; OptiScaler (its menu, or the game through its
// pass-through) sets the frame count. The control file's xessModel (0 our port, 1 XeFG's 2x model)
// and OptiScaler.ini's [XeFG] XeFGModel or earlier RtxmfgModel (0 auto = the control file's, 1 port, 2 XeFG's 2x model)
// choose the model, once, at the first XeFG creation, so a change applies after a restart.
void ConfigureModel(uint32_t model, uint32_t hostModel = 0) noexcept;
void PreparePresent(void* swapchain, uint64_t generation) noexcept;
// XeFG input fixes for tracked contexts (on by default, read once):
//  - matrix fix: when the game passes XeFG's view and projection in each other's slot
//    (the view slot holds a projection, the projection slot a view; Witcher does), swap them back.
//  - motion vectors (xess/motion_fill.h): 0 the game's own motion vectors, 1 its (2,2)
//    marker filled with camera motion (default; RTXMFG_XESS_MV_FILL=0 selects 0, =2 selects 2),
//    2 all-zero motion (diagnostic).
//  - UI composition: as the game asks (default); RTXMFG_XESS_UI_COMPOSITION=off keeps it off.
//  - HUD-less match (xess/hudless_match.h): 1 the game's HUD-less input converted to the back
//    buffer's encoding before XeFG sees it (default; composition then only for frames it converted),
//    0 the game's own HUD-less input (RTXMFG_XESS_HUDLESS=game).
uint32_t MotionFillDefault() noexcept;
bool UiCompositionOffDefault() noexcept;
uint32_t HudlessMatchDefault() noexcept;
// OptiScaler XeFG output: the SDK gateway marks calls made by OptiScaler's own code.
// Contexts such a call creates stay untracked (none of our XeFG changes apply to them); the
// first one applies the frame-count unlock (host_unlock.h) to the profiled provider pair.
// The gateway passes later host calls on those handles straight to the provider.
struct HostCallScope
{
    explicit HostCallScope(bool host) noexcept;
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    bool previous;
};
// Whether the host-owned XeFG context kept RTXMFG's menu on its native output chain at its last initialization;
// reason = the attach's refusal (a literal) when not.
bool HostOutputAttached(const void* handle, const char** reason) noexcept;
bool HostOwnedHandle(const void* handle, bool fg) noexcept;
// RTXMFG's own Vulkan host (xess/vulkan/vk_host.cpp) loads libxess_fg/libxell itself and drives XeFG on the xvk
// D3D12-on-Vulkan layer. Adopting its pair activates the SDK route on exactly those modules (no OptiScaler needed);
// calls the host makes inside a VulkanHostCallScope then take the OptiScaler host path (port, unlock, pacing owner).
// Returns whether the route is active on that pair.
bool AdoptVulkanHostPair(HMODULE fg, HMODULE ll) noexcept;
// vkprov.1: the XeMFG DLSS-G provider's device (an xvk recorded device). Applies the Vulkan host's frame-count unlock
// and ported model to the active pair and returns its libxess_fg, or null with `reason`.
HMODULE PrepareProvider(ID3D12Device* device, std::string& reason) noexcept;
// pluginprov.2: the network the route published on the pair the providers drive: "port" (our port of Intel's MFG
// model), "2x" (XeFG's own 2x network) or "stock". For the providers' logs.
const char* ProviderModelName() noexcept;
// A provider batch's outcome. OptiScaler's "FG on" comes from the game's request, before the provider runs, so a run of
// failures is what tells the menu that no frames are being generated.
void NoteProviderBatch(bool generated) noexcept;
// The pair the active route drives (OptiScaler's, when it loaded one first), false while inactive.
bool ActiveSdkPair(HMODULE& fg, HMODULE& ll) noexcept;
// Whether the export's code jumps into OptiScaler (its XeLL input took it).
bool HostRedirectedExport(HMODULE module, const char* name) noexcept;
struct VulkanHostCallScope
{
    VulkanHostCallScope() noexcept;
    ~VulkanHostCallScope();
    VulkanHostCallScope(const VulkanHostCallScope&) = delete;
    bool previous;
};
// optisleep.1: Hook_xellSleep's call into libxell on this thread, under the context's lease. Its own sleep point follows
// once the lease is released, so the libxell sleep probe does not sleep inside it (universal_hooks.cpp HostProbeActs).
struct GatewaySleepScope
{
    GatewaySleepScope() noexcept;
    ~GatewaySleepScope();
    GatewaySleepScope(const GatewaySleepScope&) = delete;
    bool previous;
};
// The host's xvk device: driver-specific presentation paths (NVAPI flip metering) never get its queues.
void SetVulkanHostDevice(ID3D12Device* device) noexcept;
// The XeMFG standalone D3D12 host's device (xess/d3d12_host.cpp), told apart for its flip queue and XeFG's native
// buffer count (ConfigureFlipQueue, HostNativeBuffers).
void SetStandaloneHostDevice(ID3D12Device* device) noexcept;
// sapacing.1: the control key xemfgFlipQueue for both XeMFG standalone hosts: 0 auto, 1 on, 2 off. Read at the first
// host initialization, so a restart applies a change; an OptiScaler.ini that sets XeFGFlipQueue still decides.
void SetStandaloneFlipQueue(uint32_t mode) noexcept;
void WriteLog(const char* stage, const std::string& fields) noexcept;
HMODULE Self() noexcept;
bool Active() noexcept;
// The game's own discovered SDK pair, once the hooks are active.
HMODULE NativeFgModule() noexcept;
HMODULE NativeLlModule() noexcept;
// OptiScaler's XeLL input (upstream master 9171bf45 and later) owns the game's XeLL contexts:
// the discovered libxell's create and destroy entries jump into OptiScaler, so none reaches our XeLL hooks.
bool HostOwnsLatency() noexcept;
// A pinned renderer-import wrapper may preserve a selected SDK's real caller
// across its own non-tail GetProcAddress forwarding. Only the two Create names
// qualify; a claimed importer never substitutes for executable caller ownership.
bool ResolveNativeSdkCreate(HMODULE importer, const void* caller, HMODULE module,
    LPCSTR name, FARPROC& result) noexcept;
}
