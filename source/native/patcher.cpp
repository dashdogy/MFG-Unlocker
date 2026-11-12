#include "single_module.h"
#include "single_overlay.h"
#include "overlay_application_imports.h"
#include "overlay_slots.h"
#include "caller_scoped_import.h"
#include "shared.h"
#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ui_status_json.h"
#include "ui_input_coherence.h"
#include "xess/xess_mod.h"
#include "xess/cuda/cuda_route.h"
#include "xess/vulkan/vk_host.h"
#include "xess/vulkan/vk_provider.h"
#include "xess/d3d12_host.h"
#include "xess/sdk_pacing.h"
#include "xess/ngx_provider.h"
#include "xess/ngx_provider_policy.h"
#include "ampere_backend.h"
#include "ampere_provider_arch.h"
#include "adapter_discovery.h"
#include "ngx_initialization.h"
#include "ngx_runtime_dispatch.h"
#include "ampere_gpu.h"
#include "ampere_policy.h"
#include "gpu_dispatch.h"
#include "gpu_name_override.h"
#include "ui_host.h"
#include "runtime_log_setting.h"
#include "gpu_selection_policy.h"
#include "midpoint_fix.h"
#include "early_provider_load.h"
namespace gpu_backend = gpu_dispatch;
inline bool UseAmpere() noexcept { return gpu_dispatch::UsesTranslatedProgram(); }
// DLSS-G presets. Families whose unmodified provider gets this
// process's own cached-reader, endpoint and NvAPI hooks (dlssg_preset): RTX 40
// and RTX 50, whose DLSS-G provider runs its own program.
inline bool NativeDlssgPresetGpu() noexcept { return gpu_dispatch::UsesNativeProgram(); }
// Families whose DLSS-G, SR and RR presets can switch while the game runs: the
// native families, and RTX 30/20, whose translated route owns its preset reader.
inline bool LivePresetGpu() noexcept { return NativeDlssgPresetGpu() || UseAmpere(); }

#include "dlssg_provider_policy.h"
#include "dlssg_provider_selection.h"
#include "ampere_status_fields.h"
#include "ampere_flip_metering.h"
#include "ngx_mfg_gate.h"
#include "ada_gate_hold.h"
#include "reflex_control.h"
#include "latency_probe.h"
#include "latency_probe_ring.h"
#include "vsync_control.h"
#include "optiscaler_host.h"
#include "optiscaler_menu_tab.h"
#include "optiscaler_private_streamline_policy.h"
#include "ngx_runtime_policy.h"
#include "dlssg_preset.h"
#include "dlss_sr.h"
#include "dlss5.h"
#include "dlss5_vulkan.h"
#include "dlss5_settings.h"
#include "frame_gen_target.h"
#include "dlssg_preset_policy.h"
#include "dlssg_preset_switch_policy.h"
#include "present_counter.h"
#include "entry_detour.h"
#include "protected_pointer.h"
#include "nvidia_mfg_policy.h"
#include "streamline_ota_policy.h"
#include "streamline_state_policy.h"
#include "temporal_interval_trace.h"
#include "fault_capture.h"
#include "session_diagnostics.h"
#include "universal_route_policy.h"
#include "universal_wrapper_profile.h"
#include "vulkan_capability.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <winternl.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <share.h>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Stable prefix of sl::VulkanInfo v1-v3. The reference ABI is pointer-sized,
// so the universal core can observe the active VkPhysicalDevice without
// depending on Vulkan SDK headers or copying Streamline's helper-only types.
struct VulkanInfoPrefix
{
    sl::BaseStructure* next = nullptr;
    sl::StructType structType{};
    size_t structVersion = 0;
    void* device = nullptr;
    void* instance = nullptr;
    void* physicalDevice = nullptr;
};

using PFun_slSetVulkanInfoAbi = sl::Result(
    const VulkanInfoPrefix& info);

static_assert(offsetof(VulkanInfoPrefix, device) == 32);
static_assert(offsetof(VulkanInfoPrefix, physicalDevice) == 48);

// vkCreateDevice (VkResult; VkPhysicalDevice, create info, allocator, VkDevice*).
using PFun_vkCreateDeviceAbi = int32_t WINAPI(void* physicalDevice, const void* createInfo,
    const void* allocator, void** device);
constexpr int32_t kVkErrorInitializationFailed = -3;

FILE* gLog = nullptr;
std::atomic<bool> gDesiredFollowGame{true};
std::atomic<uint32_t> gDesiredMultiplier{2u};
std::atomic<bool> gDesiredDynamicMode{false};
std::atomic<uint32_t> gDynamicTargetFrameRate{0};
std::atomic<uint32_t> gDlssgPresetRequested{2};
std::atomic<uint32_t> gVsyncMode{0};
std::atomic<uint32_t> gReflexFrameLimitFps{0};
std::mutex gControlSnapshotMutex;
std::atomic<bool> gDynamicExperimental56{false};
std::atomic<bool> gGeneratedOnlyDebug{false};
std::atomic<uint64_t> gDesiredRevision{0};
std::atomic<uint64_t> gAppliedRevision{0};
std::atomic<uint64_t> gAttemptedRevision{0};
std::atomic<uint64_t> gLastAttemptTick{0};
std::atomic<bool> gControlReady{false};
std::atomic<PFun_slGetFeatureFunction*> gOriginalGetFeatureFunction{nullptr};
std::atomic<PFun_slSetD3DDevice*> gOriginalSetD3DDevice{nullptr};
std::atomic<PFun_slSetVulkanInfoAbi*> gOriginalSetVulkanInfo{nullptr};
std::atomic<PFun_slSetTag*> gOriginalSetTag{nullptr};
std::atomic<PFun_slSetTagForFrame*> gOriginalSetTagForFrame{nullptr};
std::atomic<PFun_slSetConstants*> gOriginalSetConstants{nullptr};   // XeMFG standalone
std::atomic<PFun_slInit*> gOriginalSlInit{nullptr};
entry_detour::Handle gSlInitEntryHandle{};
std::atomic<PFun_slUpgradeInterface*> gOriginalSlUpgradeInterface{nullptr};
entry_detour::Handle gSlUpgradeInterfaceEntryHandle{};
std::atomic<bool> gSlUpgradeInterfaceResolverFallbackActive{false};
std::atomic<uint64_t> gSlUpgradeInterfaceCalls{0};
std::atomic<uint64_t> gSlUpgradeInterfaceWrapped{0};
std::atomic<PFun_slSetD3DDevice*> gOriginalSlSetD3DDeviceEntry{nullptr};
entry_detour::Handle gSlSetD3DDeviceEntryHandle{};
// RTX 30 systems: the interposer's Vulkan device setup entries.
std::atomic<PFun_slSetVulkanInfoAbi*> gOriginalSlSetVulkanInfoEntry{nullptr};
entry_detour::Handle gSlSetVulkanInfoEntryHandle{};
std::atomic<PFun_vkCreateDeviceAbi*> gOriginalSlVulkanCreateDevice{nullptr};
entry_detour::Handle gSlVulkanCreateDeviceEntryHandle{};
std::atomic<bool> gSlInitInstalledAtLoad{false};
std::atomic<uint64_t> gSlSetD3DDeviceCalls{0};
// OptiScaler's private interposer keeps its own slSetD3DDevice original; it is
// never forwarded into the game's instance (see HookOptiScalerSlSetD3DDevice).
std::atomic<PFun_slSetD3DDevice*> gOriginalOptiScalerSlSetD3DDevice{nullptr};
entry_detour::Handle gOptiScalerSlSetD3DDeviceEntryHandle{};
std::atomic<uint64_t> gOptiScalerSlSetD3DDeviceCalls{0};
// Hooked from the loader notification, before OptiScaler can call its slInit;
// whether the RTX 30 declaration was armed then; whether a private call has
// taken the RTX 30 startup (optiscaler_private_streamline_policy.h).
std::atomic<bool> gOptiScalerSlSetD3DDeviceAtLoad{false};
std::atomic<bool> gOptiScalerSlSetD3DDeviceArmedAtLoad{false};
std::atomic<bool> gOptiScalerPrivateAmpereAdmitted{false};
// The game's sl.interposer.dll; the Streamline entry hooks serve only it.
std::atomic<HMODULE> gHostInterposer{nullptr};
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE (WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
std::atomic<GetProcAddressFn> gOriginalSlCommonGetProcAddress{nullptr};
std::atomic<GetProcAddressFn> gOriginalMainGetProcAddress{nullptr};
std::atomic<LoadLibraryAFn> gOriginalStreamlineLoadLibraryA{nullptr};
std::atomic<LoadLibraryWFn> gOriginalStreamlineLoadLibraryW{nullptr};
std::atomic<LoadLibraryExAFn> gOriginalStreamlineLoadLibraryExA{nullptr};
std::atomic<LoadLibraryExWFn> gOriginalStreamlineLoadLibraryExW{nullptr};
std::atomic<bool> gSlCommonResolverDiscoveryInstalled{false};
std::atomic<bool> gStreamlineLoaderDiscoveryInstalled{false};
std::atomic<uint64_t> gStreamlineLoaderDiscoveryCalls{0};
std::atomic<uintptr_t> gRemixRuntimeBase{0};
std::atomic<bool> gMainResolverDiscoveryInstalled{false};
std::atomic<const char*> gMainResolverImportRoute{nullptr};   // apisetgpa.1: where the hook went in, for the log
std::atomic<bool> gSlInitIatFallbackInstalled{false};
std::atomic<bool> gSlInitResolverFallbackActive{false};
std::atomic<uint64_t> gSlInitCalls{0};
std::atomic<uint64_t> gSlInitFlagsBefore{0};
std::atomic<uint64_t> gSlInitFlagsAfter{0};
std::atomic<bool> gOtaPreferencesForced{false};
std::atomic<bool> gDownloadedStreamlinePluginsForced{false};
std::atomic<bool> gOtaProviderPreflightSupported{false};
std::atomic<bool> gOtaForceSuppressed{false};
std::atomic<bool> gFullStreamlineOtaRequested{false};
std::atomic<bool> gFullStreamlineOtaEligible{false};
std::atomic<bool> gNvidiaCompatibilityResolved{false};
std::atomic<uint32_t> gNvidiaCompatibilityTier{0};
std::atomic<int32_t> gNvidiaProfileStatus{-1};
std::atomic<uint32_t> gStreamlineHostVersionMajor{0};
std::atomic<uint32_t> gStreamlineHostVersionMinor{0};
std::atomic<uint32_t> gStreamlineHostVersionBuild{0};
std::atomic<uint32_t> gStreamlineHostVersionPrivate{0};
std::atomic<bool> gSetOptionsHookExposed{false};
std::atomic<bool> gGetStateHookExposed{false};
std::atomic<bool> gSetOptionsSeen{false};
std::atomic<bool> gGetStateSeen{false};
// This records the host's unadjusted request.  Keep it separate from the mode
// accepted after applying the mod control: an accepted mod-generated Off must
// not erase the host's continuing On intent.
std::atomic<bool> gGameFrameGenerationOn{false};
std::atomic<uint32_t> gGameFrameGenerationViewport{UINT32_MAX};
std::atomic<bool> gAppliedFrameGenerationOn{false};
std::atomic<uint64_t> gPresentationLifecycleEpoch{1};
std::atomic<int32_t> gLastSetOptionsResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<int32_t> gLastGetStateResult{static_cast<int32_t>(sl::Result::eErrorNotInitialized)};
std::atomic<bool> gAppliedDynamicMode{false};
std::atomic<uint32_t> gAppliedMultiplier{0};
std::atomic<float> gAppliedDynamicTargetFrameRate{0.0f};
std::atomic<bool> gAppliedDynamicTargetValid{false};
// autofps.1: the most presented frames per real frame the applied Dynamic options may use (SafeMaximumMultiplier
// when they were applied), for DLSS 5 Auto (frame_gen_target.h); 0 when the applied options are not Dynamic.
std::atomic<uint32_t> gAppliedDynamicMaxFrames{0};
std::atomic<bool> gAppliedDynamicExperimental56{false};
std::atomic<bool> gAppliedGeneratedOnlyDebug{false};
std::atomic<uint32_t> gActualFramesPresented{0};
std::atomic<uint32_t> gNumFramesToGenerateMax{0};
std::atomic<uint32_t> gDlssgStatus{0};
std::atomic<bool> gDynamicMfgSupported{false};
std::atomic<bool> gDynamicMfgCapabilityKnown{false};
std::atomic<bool> gFgVsyncSupportKnown{false};
std::atomic<bool> gFgVsyncSupported{false};
std::atomic<uint64_t> gStateSampleTick{0};
std::atomic<uint64_t> gSetOptionsCalls{0};
// admitoff.1: RTX 30/20 options (sets and state queries) built as Off because
// startup admission is unavailable while the game asked On, Dynamic or Auto.
std::atomic<uint64_t> gAmpereAdmissionHeldRequests{0};
std::atomic<uint64_t> gSetOptionsResolverFallbackCalls{0};
std::atomic<uint64_t> gGetStateCalls{0};
std::atomic<uint64_t> gGetStateResolverFallbackCalls{0};
std::atomic<uint64_t> gLiveReapplyCount{0};
// RTX 30 DLSS-G wrapper NVAPI view (AmpereWrapperNvapiView).
using NvapiQueryInterfaceFn = void*(__cdecl*)(uint32_t);
std::array<std::atomic<HMODULE>, 4> gAmpereFilteredWrappers{};
std::atomic<NvapiQueryInterfaceFn> gWrapperNvapiQuery{nullptr};
std::atomic<uint64_t> gWrapperNvapiFilters{0}, gFlipMeteringHidden{0};
std::atomic<bool> gWrapperNvapiFilterInstalled{false}, gAmpereWrapperResolverRouted{false};
// Flip-metering path: the wrapper's SetFlipConfig, forwarded through ObservedSetFlipConfig.
using NvapiSetFlipConfigFn = int(__cdecl*)(void*, void*, void*, void*);
std::atomic<NvapiSetFlipConfigFn> gWrapperSetFlipConfig{nullptr};
std::atomic<uint64_t> gFlipMeteringForwarded{0};
// A registered wrapper predates the V2 generation: the flip-metering path does not apply.
std::atomic<bool> gFlipForwardBlocked{false};
std::atomic<uint64_t> gNotInitializedRetryCount{0};
std::atomic<uint64_t> gNgxCreateCalls{0};
std::atomic<uint64_t> gNgxFrameGenerationCreateCalls{0};
std::atomic<uint64_t> gNgxEvaluateCalls{0};
using NgxDispatchRoute = universal_route_policy::NgxDispatchRoute;
enum class NgxGraphicsApi : uint32_t
{
    eUnknown = 0,
    eD3D12 = 1,
    eVulkan = 2,
};
enum class NgxProviderSelectionSource : uint32_t
{
    eNone = 0,
    eProviderEntry = 1,
    eRuntimeCaller = 2,
    eRuntimeUniqueCandidate = 3,
    eRuntimeDispatchTable = 4,
};
std::atomic<uint32_t> gActiveNgxDispatchRoute{
    static_cast<uint32_t>(NgxDispatchRoute::ePending)};
std::atomic<uint32_t> gActiveNgxGraphicsApi{
    static_cast<uint32_t>(NgxGraphicsApi::eUnknown)};
std::atomic<bool> gVulkanAdapterVerified{false};
std::atomic<uint64_t> gActiveNgxCreateHandle{0};
std::atomic<uint64_t> gActiveNgxEvaluateHandle{0};
std::atomic<uintptr_t> gActiveNgxProviderBase{0};
std::atomic<uint64_t> gActiveNgxProviderGeneration{0};
std::atomic<uint32_t> gActiveNgxSelectionSource{0};
std::atomic<bool> gProviderChangedAfterCreate{false};
std::atomic<int32_t> gLastNgxCreateResult{0};
std::atomic<bool> gFrameGenerationCreateObserved{false};
// GitHub issue 149: RTX 40 holds frame generation at 2X while the provider in use has no count/index check to
// restore (ada_gate_hold.h: decided by the provider a frame-generation Create uses, not the latest inspected).
ada_gate_hold::Hold gAdaGateHold;
std::atomic<bool> gFirstCreateMidpointReady{false};
std::atomic<bool> gBackportReadyAtCreate{false};
std::atomic<bool> gPipelineMayPredateDetour{false};
std::atomic<bool> gRestartRequired{false};
// Live DLSS-G preset switching. The phase and start tick change only
// under gStreamlineCallMutex; the phase is read lock-free by status.
std::atomic<uint32_t> gPresetSwitchPhase{
    static_cast<uint32_t>(dlssg_preset_switch::Phase::eIdle)};
uint64_t gPresetSwitchStartTick = 0;
// The saved request whose switch failed; a restart applies it.
std::atomic<uint32_t> gPresetSwitchFailedRequest{UINT32_MAX};
std::atomic<bool> gDllNotificationRegistered{false};
early_provider_load::Tracker<> gOutputPullMaskLoads;
struct OutputPullMaskInitEvidence
{
    entry_detour::Handle handle{};
    HMODULE owner = nullptr;
    uint64_t generation = 0;
};
std::mutex gOutputPullMaskInitMutex;
OutputPullMaskInitEvidence gOutputPullMaskSlInit;
OutputPullMaskInitEvidence gOutputPullMaskVulkanInit;
thread_local entry_detour::Snapshot gAmpereNativeInitBoundary{};
thread_local uint64_t gAmpereNativeInitTicket = 0;
std::atomic<bool> gModuleInventoryDirty{true};
// A host can shut Streamline down and initialize it again before it has a
// device (Cyberpunk 2077 calls slInit twice at startup). A plug-in we retain
// across that slShutdown is never unloaded, so the next slInit reuses its
// first-session state: NVIDIA's plug-ins skip slOnPluginLoad once loaded and
// then start on the previous session's freed parameter interface. Work which
// retains a Streamline plug-in (entry detours, pins) therefore waits until the
// host's session has a device: slSetD3DDevice, slSetVulkanInfo or the first
// slGetFeatureFunction, which Streamline only allows after the device is set.
std::atomic<bool> gStreamlinePluginRetention{false};
std::atomic<bool> gLiveHookInstalled{false};
std::atomic<bool> gSetOptionsResolverFallbackActive{false};
std::atomic<bool> gGetStateResolverFallbackActive{false};
std::atomic<bool> gUiTagHookInstalled{false};
std::atomic<uint32_t> gLoadedWrapperCandidates{0};
std::atomic<uint32_t> gPatchedWrapperCandidates{0};
std::atomic<uint32_t> gLoadedNgxCandidates{0};
std::atomic<uint32_t> gPatchedNgxCandidates{0};
std::atomic<uint32_t> gWrapperRouteBits{0};
std::atomic<uint32_t> gNgxRouteBits{0};
std::atomic<bool> gActiveWrapperObserved{false};
std::atomic<bool> gActiveWrapperPatched{false};
std::atomic<uintptr_t> gActiveWrapperBase{0};
std::atomic<uint32_t> gWrapperCompiledMaximumGeneratedFrames{0};
std::atomic<uint32_t> gSafeMaximumMultiplier{2};
std::atomic<bool> gActiveWrapperUsesNvidiaOta{false};
std::atomic<uint32_t> gActiveWrapperVersionMajor{0};
std::atomic<uint32_t> gActiveWrapperVersionMinor{0};
std::atomic<uint32_t> gActiveWrapperVersionBuild{0};
std::atomic<uint32_t> gActiveWrapperVersionPrivate{0};
std::atomic<uint32_t> gLastOptionsViewport{UINT32_MAX};
std::atomic<uint32_t> gGameOptionsStructVersion{0};
std::atomic<uint32_t> gGameColorWidth{0};
std::atomic<uint32_t> gGameColorHeight{0};
std::atomic<uint32_t> gGameHudlessBufferFormat{0};
std::atomic<uint32_t> gGameUiBufferFormat{0};
std::atomic<bool> gGameUiRecompositionEnabled{false};
std::atomic<bool> gAppliedUiRecompositionEnabled{false};
std::atomic<bool> gAppliedUiRecompositionForced{false};
std::atomic<uint64_t> gSetTagCalls{0};
std::atomic<uint64_t> gSetTagForFrameCalls{0};
std::atomic<uint32_t> gRealFpsMilli{0};
// 0 unavailable, 1 callback, 2 DXGI, 3 estimate (unused), 4 DLSS-G presented count.
std::atomic<uint32_t> gFpsOutputSource{0};
constexpr uint32_t kFpsOutputDlssgPresented = 4;
std::atomic<uint32_t> gDlssFpsMilli{0};
std::atomic<uint32_t> gFpsSampleWindowMs{0};
std::atomic<uint64_t> gFpsSampleTick{0};
std::atomic<uint64_t> gFpsOutputPresentTick{0};
// Every present the telemetry sampled, for "presents since" after a GPU driver fault.
std::atomic<uint64_t> gTelemetryPresents{0};
std::atomic<bool> gLogReady{false};
std::mutex gStreamlineCallMutex;
std::mutex gLastOptionsMutex;
std::mutex gModuleMutex;
std::mutex gImportPublicationMutex;
std::mutex gActiveControlRouteMutex;
std::mutex gUiTagMutex;
std::once_flag gNvidiaCompatibilityOnce;
std::wstring gConfigPath;
std::wstring gStatusPath;
std::wstring gExecutableDirectory;
std::wstring gExecutablePath;
std::array<wchar_t, 32768> gExecutablePathBuffer{};
std::string gNvidiaProfileName;

constexpr uint32_t kRouteLocal = 1u;
constexpr uint32_t kRouteExternal = 2u;
constexpr uint32_t kMinimumMultiplier = 2u;
constexpr uint32_t kMaximumMultiplier = 6u;
constexpr uint64_t kNotInitializedRetryDelayMs = 500;

uint64_t PackEntryHandle(entry_detour::Handle handle) noexcept
{
    return (static_cast<uint64_t>(handle.serial) << 32)
        | static_cast<uint64_t>(handle.slot);
}

entry_detour::Handle UnpackEntryHandle(uint64_t value) noexcept
{
    if (value == 0)
        return {};
    return {static_cast<uint32_t>(value),
        static_cast<uint32_t>(value >> 32)};
}

struct ControlConfig
{
    bool followGame = true;
    uint32_t multiplier = 2u;
    bool dynamic = false;
    uint32_t dynamicTargetFrameRate = 0;
    // latency.1: the game / driver preset by default (was B, whose forced UI recomposition costs about 1.5 ms of GPU
    // time per 4x batch). A saved B without "dlssgDefaults" (written before latency.1, mostly the old default) reads as
    // game / driver; the menu writes the marker with any preset it saves.
    uint32_t dlssgPreset = dlssg_preset::kGameOrDriver;
    // DLSS Super Resolution preset (0 game/driver, 10..13 J..M)
    // and Ray Reconstruction preset (0 game/driver, 4..6 D..F).
    uint32_t dlssSrPreset = 0;
    uint32_t dlssRrPreset = 0;
    dlss5::Settings dlss5{};
    uint32_t vsyncMode = 0;
    uint32_t reflexFrameLimitFps = 0;
    // XeSS MFG runs under OptiScaler only; its menu owns the frame count. The model
    // XeFG's extra frames use: 0 our port of Intel's MFG model, 1 XeFG's own 2x model, read at
    // OptiScaler's first XeFG creation (a restart applies a change). OptiScaler.ini [XeFG]
    // XeFGModel (or the earlier RtxmfgModel) comes first. Older XeSS keys in a saved file are ignored.
    uint32_t xessModel = 0;
    // RTXMFG's own XeMFG takeover on Vulkan games, harness builds only (MFG_UNLOCK_XEFG_VK_STANDALONE); products run
    // XeMFG on Vulkan as OptiScaler's XeFG output (MfgXeFGVkGetApi) and ignore these. xessVulkanFrames is its frame
    // count (2-6); without it the fixed multiplier applies (Follow = 2X, the game has no FG of its own).
    bool xessVulkan = false;
    uint32_t xessVulkanFrames = 0;
    // XeMFG standalone, RTXMFG as the XeFG host without OptiScaler (xess/d3d12_host.h, and the
    // Vulkan host above as the product's), off by default; xemfgFrames is its frame count (2-6, 0 = the fixed
    // multiplier, Follow = 2X). Takeover happens at a swap chain's creation: switching on applies after a restart.
    bool xemfgStandalone = false;
    uint32_t xemfgFrames = 0;
    // XeMFG standalone's Dynamic MFG (xess/xemfg_dynamic.h), up to xemfgFrames: its target (0 = the FPS
    // limit, else just under the refresh rate), rounding (0 at most, 1 nearest, 2 at least), 1X allowed, "Use target as
    // cap". Applies live.
    bool xemfgDynamic = false;
    uint32_t xemfgTargetFps = 0;
    uint32_t xemfgRounding = 1;
    bool xemfgAllow1x = true;
    bool xemfgTargetAsCap = false;
    // sapacing.1 (V1.7.1): XeMFG standalone's flip queue (xess_mod.h SetStandaloneFlipQueue), no menu row: 0 auto (on for
    // the D3D12 host, off for the Vulkan host), 1 on, 2 off. A restart applies a change.
    uint32_t xemfgFlipQueue = 0;
    // latency.1: low-latency pacing for XeMFG on D3D12 (xess/sdk_pacing.h SetLatencyPacing): 0 auto (on), 1 on, 2 off.
    // Applies live, under OptiScaler too.
    uint32_t latencyPacing = 0;
    bool dynamicExperimental56 = false;
    bool generatedOnlyDebug = false;
    bool intervalLogging = gpu_dispatch::UsesNativeProgram() || UseAmpere();
    // When OptiScaler is detected (optiscaler_host::UiMode): 0 = "shared": its
    // menu owns frame generation (Follow, no overrides) and RTXMFG's menu offers
    // the DLSS presets and DLSS 5; 1 = "rtxmfg": RTXMFG's own UI and controls;
    // 2 = "optiscaler": no RTXMFG menu, everything delegated;
    // 3 = absent: optiscaler_host::DefaultUiMode() (hidden with RTXMFG's tab
    // once OptiScaler handed its plugin API, else shared; hidden for a
    // ForzaTech executable).
    uint32_t optiScalerUi = 3;
};

struct ControlSnapshot
{
    ControlConfig control{};
    uint64_t revision = 0;
};

struct LastGameOptions
{
    sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions options{};
    bool valid = false;
};

struct RetainedFeatureIdentity
{
    uintptr_t handle = 0;
    uintptr_t runtime = 0;
    uint64_t runtimeGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uint64_t publication = 0;
    uint64_t adapterLuid = 0;
    uint32_t generatedFrameCapacity = 0;
    uint64_t lifetime = 0;
    uint64_t createAttemptEpoch = 0;

    explicit operator bool() const noexcept
    {
        return handle != 0 && runtime != 0 && runtimeGeneration != 0
            && provider != 0 && providerGeneration != 0 && wrapper != 0
            && wrapperGeneration != 0 && publication != 0
            && generatedFrameCapacity != 0 && lifetime != 0;
    }
};

struct AcceptedOffEvidence
{
    RetainedFeatureIdentity feature{};
    uint32_t routeSlot = UINT32_MAX;
    uint32_t viewport = UINT32_MAX;
    uint64_t revision = 0;
};

using PFun_slSetDataInternal = sl::Result(
    const sl::BaseStructure* inputs, sl::CommandBuffer* commandBuffer);
using PFun_slGetDataInternal = sl::Result(
    const sl::BaseStructure* inputs, sl::BaseStructure* outputs,
    sl::CommandBuffer* commandBuffer);

using ControlEntryPath = universal_route_policy::Path;
using UniversalRouteFailure = universal_route_policy::Failure;

struct ModuleRecord
{
    HMODULE module = nullptr;
    std::wstring path;
    uint64_t generation = 0;
    uint64_t freshLoadToken = 0;
    uint32_t controlRouteSlot = UINT32_MAX;
    bool wrapperExport = false;
    bool wrapperCandidate = false;
    bool wrapperPatched = false;
    uint32_t wrapperCompiledMaximumGeneratedFrames = 0;
    bool ngxExport = false;
    bool ngxD3D12Export = false;
    bool ngxVulkanExport = false;
    bool ngxCandidate = false;
    bool ngxPatched = false;
    bool ngxTemporalPatched = false;
    bool ngxRuntimeExport = false;
    bool ngxRuntimeD3D12Export = false;
    bool ngxRuntimeVulkanExport = false;
    bool createResolverDiscoveryHooked = false;
    bool inventoryLogged = false;
};

struct FileVersion
{
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    uint32_t privatePart = 0;
};

constexpr size_t kControlRouteCapacity = 16;

struct ControlRouteRecord
{
    std::atomic<bool> claimed{false};
    HMODULE wrapper = nullptr;
    uint64_t generation = 0;
    std::wstring path;
    FileVersion version{};
    std::atomic<bool> wrapperPatched{false};
    std::atomic<uint32_t> compiledMaximumGeneratedFrames{0};

    entry_detour::Handle publicSetHandle{};
    entry_detour::Handle publicGetHandle{};
    entry_detour::Handle internalSetHandle{};
    entry_detour::Handle internalGetHandle{};
    entry_detour::Handle freeResourcesHandle{};
    std::atomic<PFun_slDLSSGSetOptions*> publicSetOriginal{nullptr};
    std::atomic<PFun_slDLSSGGetState*> publicGetOriginal{nullptr};
    std::atomic<PFun_slSetDataInternal*> internalSetOriginal{nullptr};
    std::atomic<PFun_slGetDataInternal*> internalGetOriginal{nullptr};
    std::atomic<PFun_slFreeResources*> freeResourcesOriginal{nullptr};
    std::atomic<bool> publicSetResolverFallback{false};
    std::atomic<bool> publicGetResolverFallback{false};
    std::atomic<bool> lifecycleInstallAttempted{false};

    std::atomic<uint32_t> activeSetterPath{
        static_cast<uint32_t>(ControlEntryPath::eNone)};
    std::atomic<uint32_t> activeStatePath{
        static_cast<uint32_t>(ControlEntryPath::eNone)};
    std::atomic<uint64_t> setterCalls{0};
    std::atomic<uint64_t> stateCalls{0};
    std::atomic<uint64_t> lastCallTick{0};
    std::atomic<uint64_t> lastCallRevision{0};
    std::atomic<uint64_t> lastAcceptedRevision{0};
    std::atomic<bool> structureCompatible{true};
    std::atomic<bool> frameGenerationOffAccepted{false};
    std::atomic<bool> releaseObserved{false};
    std::atomic<uint64_t> releaseCalls{0};
};

using UiInputSnapshot = ui_input_coherence::Snapshot;

LastGameOptions gLastGameOptions;
RetainedFeatureIdentity gRetainedFrameGenerationFeature;
RetainedFeatureIdentity gFreshRecreatedFrameGenerationFeature;
AcceptedOffEvidence gAcceptedOffEvidence;
std::mutex gFrameGenerationLifetimeMutex;
std::atomic<uint64_t> gFrameGenerationCreateAttemptEpoch{0};
std::vector<ModuleRecord> gModuleRecords;
std::array<ControlRouteRecord, kControlRouteCapacity> gControlRoutes{};
ui_input_coherence::Tracker gUiInputTracker;
std::mutex gControlRouteMutex;
std::atomic<uint64_t> gNextModuleGeneration{1};
std::atomic<uint32_t> gActiveControlRouteSlot{UINT32_MAX};
std::atomic<uint32_t> gUniversalRouteFailure{
    static_cast<uint32_t>(UniversalRouteFailure::eNoActiveRoute)};
std::atomic<DWORD> gInternalControlBypassTlsIndex{TLS_OUT_OF_INDEXES};
LARGE_INTEGER gFpsCounterFrequency{};
LARGE_INTEGER gFpsWindowStart{};
uint64_t gFpsWindowOutputFrames = 0;
bool gFpsWindowOutputAvailable = false;
frame_telemetry::PresentCounter gFpsPresentCounter;
// Every recorded GetState samples DLSS-G's presented count (gDlssgPresentedMutex);
// the window copy is gFpsTelemetryMutex's.
std::mutex gDlssgPresentedMutex;
frame_telemetry::DlssgPresentedCounter gDlssgPresented;
frame_telemetry::DlssgPresentedTotals gFpsWindowDlssgPresented;
std::array<uint64_t, temporal_interval_trace::kFirstSampleHandleCapacity>
    gFpsWindowFirstSamples{};
bool gFpsTelemetryActive = false;
LARGE_INTEGER gIntervalFpsWindowStart{};
std::array<uint64_t, temporal_interval_trace::kFirstSampleHandleCapacity>
    gIntervalFpsWindowFirstSamples{};
std::mutex gFpsTelemetryMutex;

HMODULE ModuleFromAddress(const void* address);
std::wstring LoadedModulePath(HMODULE module);
bool UsesNvidiaOtaCache(const std::wstring& path);
void RecomputeModuleStateLocked();
void ObserveAmpereFeatureLifetime(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept;
void EnsureAmpereFeatureLifetimeObserver() noexcept;
ControlRouteRecord* ActiveControlRoute() noexcept;
const char* ControlPathName(ControlEntryPath path) noexcept;
void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept;
bool ActiveSetterCovered(const ControlRouteRecord& route) noexcept;
bool SetterEntryCovered(const ControlRouteRecord& route) noexcept;
bool StateEntryCovered(const ControlRouteRecord& route) noexcept;
bool TryInstallSetOptionsEntryDetour(HMODULE wrapper, void* resolvedTarget);
bool TryInstallGetStateEntryDetour(HMODULE wrapper, void* resolvedTarget);
uint32_t EnsureControlRoute(HMODULE wrapper, const std::wstring& path,
    uint64_t generation, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames);
bool InstallControlRouteEntries(uint32_t routeSlot);
bool InstallControlRouteLifecycleEntry(uint32_t routeSlot);
bool TryInstallSlInitEntryDetour(HMODULE interposer, void* resolvedTarget);
bool TryInstallSlUpgradeInterfaceEntryDetour(HMODULE interposer, void* resolvedTarget);
bool TryInstallSlSetD3DDeviceEntryDetour(HMODULE interposer);
bool TryInstallSlVulkanEntryDetours(HMODULE interposer);
bool TryInstallOptiScalerSlSetD3DDeviceEntryDetour(HMODULE interposer, const std::wstring& path);
void* MappedExportAddress(HMODULE module, const char* expected) noexcept;
void WINAPI BeforeNgxD3D12EvaluateFeature(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle, const void*) noexcept;
void WINAPI BeforeNgxRuntimeD3D12EvaluateFeature(void*, uintptr_t, const void*, void*, uintptr_t, uintptr_t, entry_detour::Handle, const void*) noexcept;
bool TryInstallNgxCreateEntryDetour(HMODULE provider, const std::wstring& path,
    uint64_t generation);
bool TryInstallNgxEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeCreateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanCreateEntryDetours(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeVulkanCreateEntryDetours(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation);
bool TryInstallNgxVulkanAdapterEntryDetours(
    HMODULE module, const std::wstring& path, uint64_t generation);
bool InstallSlCommonResolverDiscovery(
    HMODULE module, const std::wstring& path);
bool InstallStreamlineLoaderDiscovery(
    HMODULE module, const std::wstring& path);
FARPROC AmpereWrapperNvapiView(HMODULE module, LPCSTR name, FARPROC resolved, const void* caller) noexcept;
ModuleRecord InspectLoadedModule(HMODULE, const std::wstring&);
bool InspectAlreadyLoadedModules();
ampere_gpu::PreparationBoundary ResolveAmperePreparationBoundary(HMODULE, uint64_t) noexcept;
bool AmperePreparationStillCurrent(HMODULE, const ampere_gpu::PreparationBoundary&) noexcept;
void PrepareNgxInitialization(void* device, const entry_detour::Snapshot& entry,
    const void* caller, bool firstInit) noexcept;
bool OutputPullMaskEarlyInitProven(HMODULE provider, NgxGraphicsApi api) noexcept;
void ObserveOutputPullMaskSlInitResult(
    const entry_detour::Snapshot& before, bool succeeded) noexcept;
void ObserveOutputPullMaskVulkanInit(entry_detour::Handle handle) noexcept;

sl::Result InvokeAmpereDeviceSetup(PFun_slSetD3DDevice* original, void* device)
{
    sl::Result result = sl::Result::eErrorNotInitialized;
    ampere_backend::BeginStartup();
    __try
    {
        InspectAlreadyLoadedModules();
        result = original(device);
        InspectAlreadyLoadedModules();
    }
    __finally { ampere_backend::EndStartup(result == sl::Result::eOk); }
    return result;
}

sl::Result InvokeAmpereInit(PFun_slInit* original, const sl::Preferences& preferences, uint64_t version)
{
    ampere_backend::BeginInit();
    __try { return original(preferences, version); }
    __finally { ampere_backend::EndInit(); }
}

// RTX 30 Vulkan hosts start the Streamline plug-ins in slSetVulkanInfo (manual
// hooking) or inside the interposer's own vkCreateDevice. Either call is the
// startup bracket slSetD3DDevice is for D3D12.
sl::Result InvokeAmpereVulkanSetup(PFun_slSetVulkanInfoAbi* original, const VulkanInfoPrefix& info)
{
    sl::Result result = sl::Result::eErrorNotInitialized;
    ampere_backend::BeginStartup();
    __try
    {
        InspectAlreadyLoadedModules();
        result = original(info);
        InspectAlreadyLoadedModules();
    }
    __finally { ampere_backend::EndStartup(result == sl::Result::eOk); }
    return result;
}

int32_t InvokeAmpereVulkanDeviceCreation(PFun_vkCreateDeviceAbi* original, void* physicalDevice,
    const void* createInfo, const void* allocator, void** device)
{
    int32_t result = kVkErrorInitializationFailed;
    ampere_backend::BeginStartup();
    __try
    {
        InspectAlreadyLoadedModules();
        result = original(physicalDevice, createInfo, allocator, device);
        // The interposer created the device on the verified adapter and then
        // started its plug-ins on it; that VkDevice is the host's FG device.
        if (result == 0 && device)
            ampere_backend::ObserveVulkanDevice(*device, true);
        InspectAlreadyLoadedModules();
    }
    __finally { ampere_backend::EndStartup(result == 0); }
    return result;
}

sl::Result HookSlDLSSGGetState(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options);
ModuleRecord InspectLoadedModule(
    HMODULE module, const std::wstring& suppliedPath);
FileVersion ReadFileVersion(const std::wstring& path);
uint64_t ModuleGeneration(HMODULE module) noexcept;

// A beta's lines carry local time; `stamped` lines were stamped when they were queued.
void WriteLogLine(const wchar_t* message, bool stamped)
{
    OutputDebugStringW(L"[MfgUnlock] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
    if (gLog)
    {
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
        if (!stamped)
        {
            wchar_t stamp[session_diagnostics::kStampChars];
            session_diagnostics::Stamp(stamp);
            fwprintf_s(gLog, L"%s%s\n", stamp, message);
            fflush(gLog);
            return;
        }
#else
        (void)stamped;
#endif
        fwprintf_s(gLog, L"%s\n", message);
        fflush(gLog);
    }
}

// 4096: the startup banner's base= list passes 2048 characters, and a shorter buffer would truncate its
// session-diagnostics label and output fields.
void Log(const wchar_t* format, ...)
{
    wchar_t message[4096]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    WriteLogLine(message, false);
}

// Hook-state lines repeat on every module rescan; a beta writes each distinct line once.
void LogOnChange(const wchar_t* format, ...)
{
    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    if (!session_diagnostics::FirstOccurrence(message))
        return;
#endif
    WriteLogLine(message, false);
}

void MidpointLog(const wchar_t* message)
{
    Log(L"%s", message ? message : L"");
}

// The sink for lines queued by single_module::Log.
void QueuedLog(const wchar_t* line)
{
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    WriteLogLine(line ? line : L"", true);
#else
    MidpointLog(line);
#endif
}

void LogAmpereArchitectureDetection(bool armed) noexcept
{
    static std::atomic<bool> logged{false};
    if (!gLogReady.load(std::memory_order_acquire) || logged.exchange(true, std::memory_order_acq_rel))
        return;
    const auto declaration = ampere_provider_arch::Read();
    // nvapiRedirected: ids another layer's NVAPI factory detour replaced
    // (bit 2 = GetArchInfo under OptiScaler), taken from NVIDIA's own factory.
    Log(L"RTX 30 DLSS-G declaration: armed=%d gpuArchitecture=0x%X implementation=0x%X nvapiRedirected=0x%X",
        armed, declaration.gpuArchitecture, declaration.gpuImplementation, declaration.nvapiRedirected);
}

int32_t NvidiaProfileStatus(
    const nvidia_mfg_policy::ProfileQuery& query) noexcept
{
    if (query.initializeStatus != 0)
        return query.initializeStatus;
    if (query.createSessionStatus != 0)
        return query.createSessionStatus;
    if (query.loadSettingsStatus != 0)
        return query.loadSettingsStatus;
    if (query.findApplicationStatus != 0)
        return query.findApplicationStatus;
    return query.getProfileStatus;
}

void ResolveNvidiaCompatibilityPolicy()
{
    std::call_once(gNvidiaCompatibilityOnce, [] {
        const nvidia_mfg_policy::ProfileQuery query =
            nvidia_mfg_policy::IdentifyExecutable(gExecutablePath.c_str());
        gNvidiaProfileName = nvidia_mfg_policy::NormalizeTitle(
            query.profileName);
        gNvidiaProfileStatus.store(
            NvidiaProfileStatus(query), std::memory_order_relaxed);
        gNvidiaCompatibilityTier.store(
            static_cast<uint32_t>(query.tier), std::memory_order_relaxed);
        gNvidiaCompatibilityResolved.store(
            query.getProfileStatus == 0 && !query.profileName.empty(),
            std::memory_order_release);
    });
}

nvidia_mfg_policy::CapacityDecision CurrentCapacityDecision() noexcept
{
    const bool activeObserved =
        gActiveWrapperObserved.load(std::memory_order_acquire);
    const bool wrapperPatched = activeObserved
        && gActiveWrapperPatched.load(std::memory_order_acquire);
    return nvidia_mfg_policy::DecideCapacity(
        static_cast<nvidia_mfg_policy::Tier>(
            gNvidiaCompatibilityTier.load(std::memory_order_acquire)),
        wrapperPatched,
        activeObserved
            ? gWrapperCompiledMaximumGeneratedFrames.load(
                std::memory_order_acquire)
            : 0u);
}

// rtx30ngx.1: on RTX 30/20, an Nvngx replacement provider (MFGUnlocker's XeMFG provider, loaded by OptiScaler)
// owns DLSS-G frame generation and NVIDIA's provider never created a FrameGeneration feature
// (ampere_policy::ReplacementOwnsFrameGeneration). Logged once each way.
bool AmpereReplacementOwnsFrameGeneration() noexcept
{
    if (!UseAmpere())
        return false;
    const bool owns = ampere_policy::ReplacementOwnsFrameGeneration(ngx_provider::Offered(),
        gNgxFrameGenerationCreateCalls.load(std::memory_order_acquire), ampere_backend::CreatedFeatures());
    static std::atomic<int> logged{-1};
    const int state = owns ? 1 : 0;
    if (logged.load(std::memory_order_relaxed) != state
        && logged.exchange(state, std::memory_order_acq_rel) != state)
    {
        if (owns)
            Log(L"RTX 30/20 DLSS-G options follow the Nvngx replacement provider (MFGUnlocker XeMFG, up to %ux): "
                L"NVIDIA's provider created no FrameGeneration feature, so no Ampere ring applies",
                static_cast<unsigned>(ngx_provider::kMaxGenerated + 1u));
        else if (ngx_provider::Offered())
            Log(L"RTX 30/20 DLSS-G options follow the certified Ampere ring: NVIDIA's provider created a "
                L"FrameGeneration feature (ngx=%llu ampere=%llu)",
                static_cast<unsigned long long>(gNgxFrameGenerationCreateCalls.load(std::memory_order_acquire)),
                static_cast<unsigned long long>(ampere_backend::CreatedFeatures()));
    }
    return owns;
}

// RTX 30/20 whose DLSS-G options and live re-enables go through the Ampere policy (the certified ring, startup
// admission, the retained translated feature). Not while a replacement provider owns frame generation: then the
// options follow the game as on RTX 40.
bool AmpereOptionsApply() noexcept
{
    return UseAmpere() && !AmpereReplacementOwnsFrameGeneration();
}

// GitHub issue 149 (AION 2, RTX 4070 SUPER, the game's DLSS-G 310.6.0.0): the provider has no count/index check to
// restore, so the route can never become ready, yet the wrapper's own clamp was lifted and the game's 3X went through
// to a provider that does not take it: the picture froze. While the provider RTX 40 runs lacks the check, the game's
// count is held at one generated frame and its reported maximum at one, so it runs and offers 2X. Which provider that
// is: the one a frame-generation Create uses, else (before any Create) one without the check only while no supported
// provider with it has been inspected (ada_gate_hold.h; beta.3 let any later inspection of a game's old bundled
// provider set the hold mid-session, beside NVIDIA's patched OTA provider).
bool AdaProviderWithoutGate() noexcept
{
    return gpu_dispatch::IsAda() && gAdaGateHold.Held();
}

void NoteAdaProviderGate(bool missing, const std::wstring& path, ada_gate_hold::Source source) noexcept
{
    switch (gAdaGateHold.Note(missing, source))
    {
    case ada_gate_hold::Change::eHeld:
        Log(L"RTX 40: this DLSS-G provider has no multi-frame count/index check to restore (DLSS-G before 310.7); "
            L"frame generation is held at 2X with it: %s", path.c_str());
        break;
    case ada_gate_hold::Change::eLifted:
        Log(L"RTX 40: frame generation above 2X is allowed again: %s DLSS-G provider has the multi-frame count/index check: %s",
            source == ada_gate_hold::Source::eCreate ? L"the frame-generation Create's" : L"a loaded", path.c_str());
        break;
    case ada_gate_hold::Change::eNone:
        break;
    }
}

uint32_t SafeMaximumMultiplier() noexcept
{
    // RTX 30/20: the certified ring's N+1 (6x only for a full ring), or the replacement provider's own maximum.
    uint32_t runtimeMaximum = UseAmpere()
        ? ampere_policy::SafeMaximumMultiplier(AmpereReplacementOwnsFrameGeneration(), ngx_provider::kMaxGenerated,
            ampere_backend::Ready(), ampere_backend::StartupMaximumGeneratedFrames(),
            ampere_backend::CertifiedMaximumGeneratedFrames())
        : gpu_dispatch::UsesNativeProgram() ? std::clamp(
            CurrentCapacityDecision().effectiveMaximumMultiplier,
            kMinimumMultiplier, kMaximumMultiplier) : 2u;
    if (AdaProviderWithoutGate())
        runtimeMaximum = 2u;
    // RTX 50's unpatched wrapper clamps the count to the provider's own
    // maximum as well; offer no mode above the one the provider reports.
    if (gpu_dispatch::IsBlackwell())
        if (const uint32_t reported = gNumFramesToGenerateMax.load(std::memory_order_acquire))
            runtimeMaximum = std::min(runtimeMaximum,
                std::clamp(reported + 1u, kMinimumMultiplier, kMaximumMultiplier));
    const uint32_t maximum = nvidia_mfg_policy::LimitToPolicy(
        static_cast<nvidia_mfg_policy::Tier>(
            gNvidiaCompatibilityTier.load(std::memory_order_acquire)), runtimeMaximum);
    gSafeMaximumMultiplier.store(maximum, std::memory_order_release);
    return maximum;
}

uint32_t AmpereAdvertisedMaximum() noexcept
{
    // Allocation evidence (certified maximum) arrives only after Create; the
    // pre-Create maximum follows the wrapper's startup capacity and the title's
    // NVIDIA ceiling, the same ceiling SafeMaximumMultiplier applies later.
    // Once a Create certified a smaller ring, no more than it holds.
    return ampere_policy::AdvertisedGeneratedFrames(ampere_backend::StartupMaximumGeneratedFrames(),
        nvidia_mfg_policy::LimitToPolicy(static_cast<nvidia_mfg_policy::Tier>(
            gNvidiaCompatibilityTier.load(std::memory_order_acquire)), ampere_policy::kMaximumGeneratedFrames + 1u),
        ampere_backend::RingMaximumGeneratedFrames());
}

uint32_t EffectiveMultiplier(const ControlConfig& control) noexcept
{
    if (UseAmpere())
    {
        return std::clamp(control.multiplier, 1u, SafeMaximumMultiplier());
    }
    else
    {
        return std::clamp(control.multiplier, kMinimumMultiplier,
            SafeMaximumMultiplier());
    }
}

ui_input_coherence::Generation CurrentUiInputGeneration() noexcept
{
    const auto* route = ActiveControlRoute();
    const bool current = route && route->structureCompatible.load(std::memory_order_acquire);
    return {current ? reinterpret_cast<uintptr_t>(route->wrapper) : 0,
        current ? route->generation : 0,
        gActiveNgxProviderBase.load(std::memory_order_acquire),
        gActiveNgxProviderGeneration.load(std::memory_order_acquire),
        gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire),
        gPresentationLifecycleEpoch.load(std::memory_order_acquire),
        ui_input_coherence::resourceEpoch.load(std::memory_order_acquire)};
}

void InvalidateUiInputEvidence(uint32_t viewport = UINT32_MAX) noexcept
{
    ui_input_coherence::InvalidateResources();
    std::lock_guard lock(gUiTagMutex);
    gUiInputTracker.Invalidate(viewport);
}

ui_input_coherence::Resource CaptureUiResourceTag(const sl::ResourceTag& tag,
    uint64_t tick) noexcept
{
    ui_input_coherence::Resource state{};
    state.observed = true;
    state.lastSeenTick = tick;
    if (!tag.resource || !tag.resource->native) return state;
    const auto& resource = *tag.resource;
    state.active = true;
    state.native = reinterpret_cast<uintptr_t>(resource.native);
    state.view = reinterpret_cast<uintptr_t>(resource.view);
    state.type = static_cast<uint32_t>(resource.type);
    state.lifecycle = static_cast<uint32_t>(tag.lifecycle);
    state.top = tag.extent.top;
    state.left = tag.extent.left;
    state.width = tag.extent.width ? tag.extent.width : resource.width;
    state.height = tag.extent.height ? tag.extent.height : resource.height;
    state.format = resource.nativeFormat;
    // Do not dereference a retained resource later or infer native bounds from
    // an address. Unknown descriptions remain observations, never ready input.
    state.shapeKnown = tag.structType == sl::ResourceTag::s_structType
        && tag.structVersion == sl::kStructVersion1
        && resource.structType == sl::Resource::s_structType
        && resource.structVersion == sl::kStructVersion1
        && resource.type == sl::ResourceType::eTex2d && state.format
        && tag.lifecycle >= sl::ResourceLifecycle::eOnlyValidNow
        && tag.lifecycle <= sl::ResourceLifecycle::eValidUntilEvaluate
        && resource.width && resource.height && state.width && state.height
        && state.left <= resource.width && state.width <= resource.width - state.left
        && state.top <= resource.height && state.height <= resource.height - state.top;
    return state;
}

UiInputSnapshot ReadUiInputSnapshot(uint32_t viewport)
{
    const auto generation = CurrentUiInputGeneration();
    const bool active = viewport == gLastOptionsViewport.load(std::memory_order_acquire);
    std::lock_guard lock(gUiTagMutex);
    auto snapshot = gUiInputTracker.Read(viewport, generation, GetTickCount64(),
        active ? gGameColorWidth.load(std::memory_order_relaxed) : 0,
        active ? gGameColorHeight.load(std::memory_order_relaxed) : 0,
        active ? gGameHudlessBufferFormat.load(std::memory_order_relaxed) : 0,
        active ? gGameUiBufferFormat.load(std::memory_order_relaxed) : 0);
    if (generation != CurrentUiInputGeneration())
    {
        snapshot.ready = false;
        snapshot.generationCurrent = false;
        snapshot.failure = ui_input_coherence::Failure::eGenerationChanged;
    }
    return snapshot;
}

struct UiTagBatch
{
    uint32_t viewport = UINT32_MAX;
    bool valid = false;
    bool framed = false;
    uint32_t frame = 0;
    uint64_t tick = 0;
    ui_input_coherence::Generation generation{};
    std::array<ui_input_coherence::Resource, 3> resources{};
};

UiTagBatch PrepareUiResourceTags(const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags, uint32_t numTags, bool framed, uint32_t frame)
{
    UiTagBatch batch{};
    if (!tags || !numTags || numTags > 1024) return batch;
    batch.viewport = static_cast<uint32_t>(viewport);
    batch.framed = framed;
    batch.frame = frame;
    batch.tick = GetTickCount64();
    batch.generation = CurrentUiInputGeneration();
    // Copy bounded metadata while the caller's descriptors are valid. No COM
    // references, caller chains, or FrameToken pointers survive this call.
    for (uint32_t index = 0; index < numTags; ++index)
    {
        const auto& tag = tags[index];
        size_t slot = 3;
        if (tag.type == sl::kBufferTypeHUDLessColor) slot = 0;
        else if (tag.type == sl::kBufferTypeUIAlpha) slot = 1;
        else if (tag.type == sl::kBufferTypeUIColorAndAlpha) slot = 2;
        if (slot != 3)
        {
            batch.resources[slot] = CaptureUiResourceTag(tag, batch.tick);
            batch.valid = true;
        }
    }
    // A new explicitly framed tagging call advances the observed frame even
    // if it contains only other inputs, so previous-frame UI cannot linger.
    batch.valid = batch.valid || framed;
    return batch;
}

void RecordUiResourceTags(const UiTagBatch& batch)
{
    if (!batch.valid) return;
    {
        std::lock_guard lock(gUiTagMutex);
        if (batch.generation != CurrentUiInputGeneration())
        {
            gUiInputTracker.Invalidate(batch.viewport);
            return;
        }
        auto* entry = gUiInputTracker.Begin(batch.viewport, batch.framed,
            batch.frame, batch.generation, batch.tick);
        if (!entry) return;
        for (size_t slot = 0; slot < batch.resources.size(); ++slot)
            if (batch.resources[slot].observed)
                gUiInputTracker.Observe(*entry,
                    static_cast<ui_input_coherence::Kind>(slot), batch.resources[slot]);
    }
}

bool DlssgStateAvailable(sl::Result result) noexcept
{
    return result == sl::Result::eOk
        || result == sl::Result::eWarnOutOfVRAM;
}

void ResetFpsTelemetry() noexcept
{
    gFpsWindowStart = {};
    gFpsWindowOutputFrames = 0;
    gFpsWindowOutputAvailable = false;
    gFpsOutputSource.store(0, std::memory_order_relaxed);
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        gFpsWindowFirstSamples[index] =
            intervalTrace.firstSampleCounters[index].samples;
    }
    gRealFpsMilli.store(0, std::memory_order_relaxed);
    gDlssFpsMilli.store(0, std::memory_order_relaxed);
    gFpsSampleWindowMs.store(0, std::memory_order_relaxed);
    gFpsSampleTick.store(0, std::memory_order_release);
}

// The interval trace's real frames across every handle: a monotonic key for
// the per-real-frame DLSS-G sample.
uint64_t RealFrameCount(const temporal_interval_trace::Snapshot& trace) noexcept
{
    uint64_t total = 0;
    for (const auto& counter : trace.firstSampleCounters)
        total += counter.samples;
    return total;
}

frame_telemetry::DlssgPresentedTotals ReadDlssgPresented() noexcept
{
    std::lock_guard lock(gDlssgPresentedMutex);
    return gDlssgPresented.Totals();
}

// outputSource kFpsOutputDlssgPresented ignores outputFrames/outputAvailable:
// the output is DLSS-G's own count of the window's real frames.
void UpdateFpsTelemetryForOutputPresent(uint32_t outputFrames,
    bool outputAvailable, uint32_t outputSource)
{
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now))
        return;
    if (gFpsCounterFrequency.QuadPart == 0
        && !QueryPerformanceFrequency(&gFpsCounterFrequency))
        return;
    if (gFpsWindowStart.QuadPart == 0 || now.QuadPart <= gFpsWindowStart.QuadPart)
    {
        gFpsWindowStart = now;
        gFpsWindowOutputFrames = 0;
        gFpsWindowOutputAvailable = outputAvailable;
        const temporal_interval_trace::Snapshot intervalTrace =
            temporal_interval_trace::ReadSnapshot();
        for (size_t index = 0;
             index < temporal_interval_trace::kFirstSampleHandleCapacity;
             ++index)
        {
            gFpsWindowFirstSamples[index] =
                intervalTrace.firstSampleCounters[index].samples;
        }
        gFpsWindowDlssgPresented = ReadDlssgPresented();
        return;
    }

    gFpsWindowOutputFrames += outputFrames;
    gFpsWindowOutputAvailable &= outputAvailable;
    const uint64_t elapsedTicks = static_cast<uint64_t>(
        now.QuadPart - gFpsWindowStart.QuadPart);
    const uint64_t minimumTicks = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart) / 2;
    if (elapsedTicks < minimumTicks)
        return;

    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    uint64_t realFrames = 0;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        const uint64_t current =
            intervalTrace.firstSampleCounters[index].samples;
        const uint64_t previous = gFpsWindowFirstSamples[index];
        if (current >= previous)
            realFrames = std::max(realFrames, current - previous);
        gFpsWindowFirstSamples[index] = current;
    }

    const uint64_t frequency = static_cast<uint64_t>(gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return frame_telemetry::RateMilli(frames, frequency, elapsedTicks);
    };
    uint32_t dlssMilli = gFpsWindowOutputAvailable ? rateMilli(gFpsWindowOutputFrames) : 0u;
    uint32_t source = gFpsWindowOutputAvailable ? outputSource : 0u;
    if (outputSource == kFpsOutputDlssgPresented)
    {
        const auto presented = ReadDlssgPresented();
        dlssMilli = frame_telemetry::DlssgPresentedRateMilli(gFpsWindowDlssgPresented,
            presented, realFrames, frequency, elapsedTicks, now.QuadPart);
        source = dlssMilli ? outputSource : 0u;
        gFpsWindowDlssgPresented = presented;
    }
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
    gDlssFpsMilli.store(dlssMilli, std::memory_order_relaxed);
    gFpsOutputSource.store(source, std::memory_order_relaxed);
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(GetTickCount64(), std::memory_order_release);
    gFpsWindowStart = now;
    gFpsWindowOutputFrames = 0;
    gFpsWindowOutputAvailable = outputAvailable;
}

void ResetIntervalFpsWindow(
    const temporal_interval_trace::Snapshot& intervalTrace,
    const LARGE_INTEGER& now) noexcept
{
    gIntervalFpsWindowStart = now;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        gIntervalFpsWindowFirstSamples[index] =
            intervalTrace.firstSampleCounters[index].samples;
    }
}

void UpdateFpsTelemetryWithoutPresentCallback()
{
    std::lock_guard telemetryLock(gFpsTelemetryMutex);
    const uint64_t nowTick = GetTickCount64();
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        if (gRealFpsMilli.load(std::memory_order_relaxed) != 0
            || gDlssFpsMilli.load(std::memory_order_relaxed) != 0)
        {
            ResetFpsTelemetry();
        }
        gFpsTelemetryActive = false;
        gFpsOutputPresentTick.store(0, std::memory_order_release);
        gIntervalFpsWindowStart = {};
        gIntervalFpsWindowFirstSamples.fill(0);
        return;
    }

    const uint64_t presentTick = gFpsOutputPresentTick.load(
        std::memory_order_acquire);
    const bool presentSamplerCurrent = presentTick != 0
        && nowTick >= presentTick && nowTick - presentTick <= 2000;
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    LARGE_INTEGER now{};
    if (!intervalTrace.initialized || !intervalTrace.enabled
        || !QueryPerformanceCounter(&now))
    {
        return;
    }

    if (presentSamplerCurrent)
    {
        ResetIntervalFpsWindow(intervalTrace, now);
        return;
    }

    // No ReShade final-present callback is available in the CET-only package.
    // Count index-1 temporal requests as real input frames, then scale by the
    // runtime-reported presentation multiplier. This preserves correct real
    // cadence and provides a useful DLSS output estimate without adding a
    // process-wide DXGI Present hook solely for UI telemetry.
    if (gFpsTelemetryActive)
    {
        gFpsTelemetryActive = false;
        gFpsWindowStart = {};
        gFpsWindowOutputFrames = 0;
    }
    if (gFpsCounterFrequency.QuadPart == 0
        && !QueryPerformanceFrequency(&gFpsCounterFrequency))
    {
        return;
    }
    if (gIntervalFpsWindowStart.QuadPart == 0
        || now.QuadPart <= gIntervalFpsWindowStart.QuadPart)
    {
        ResetIntervalFpsWindow(intervalTrace, now);
        gRealFpsMilli.store(0, std::memory_order_relaxed);
        gDlssFpsMilli.store(0, std::memory_order_relaxed);
        gFpsSampleWindowMs.store(0, std::memory_order_relaxed);
        gFpsSampleTick.store(0, std::memory_order_release);
        return;
    }

    const uint64_t elapsedTicks = static_cast<uint64_t>(
        now.QuadPart - gIntervalFpsWindowStart.QuadPart);
    const uint64_t minimumTicks = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart) / 2;
    if (elapsedTicks < minimumTicks)
        return;

    uint64_t realFrames = 0;
    for (size_t index = 0;
         index < temporal_interval_trace::kFirstSampleHandleCapacity;
         ++index)
    {
        const uint64_t current =
            intervalTrace.firstSampleCounters[index].samples;
        const uint64_t previous = gIntervalFpsWindowFirstSamples[index];
        if (current >= previous)
            realFrames = std::max(realFrames, current - previous);
    }

    const uint64_t frequency = static_cast<uint64_t>(
        gFpsCounterFrequency.QuadPart);
    const auto rateMilli = [&](uint64_t frames) {
        return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
            (frames * frequency * 1000u + elapsedTicks / 2u) / elapsedTicks));
    };
    gRealFpsMilli.store(rateMilli(realFrames), std::memory_order_relaxed);
    // The selected mode and the latest GetState result are not output counts
    // for this time window. Keep the real rate if DXGI telemetry is unavailable.
    gDlssFpsMilli.store(0, std::memory_order_relaxed);
    gFpsOutputSource.store(0, std::memory_order_relaxed);
    gFpsSampleWindowMs.store(static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX,
            (elapsedTicks * 1000u + frequency / 2u) / frequency)),
        std::memory_order_relaxed);
    gFpsSampleTick.store(nowTick, std::memory_order_release);
    ResetIntervalFpsWindow(intervalTrace, now);
}

void RecordDlssgStateResult(
    sl::Result result, const sl::DLSSGState& state)
{
    gGetStateCalls.fetch_add(1, std::memory_order_relaxed);
    gGetStateSeen.store(true, std::memory_order_release);
    gLastGetStateResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    if (!DlssgStateAvailable(result))
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        frame_gen_target::Publish(frame_gen_target::Source::eDlssg, false, 0.0f, 0, 0);
        return;
    }
    // autofps.1: DLSS 5 Auto follows DLSS-G Dynamic's target while Dynamic options are applied and generating
    // (a 0 target is the display's refresh rate, Streamline's own default; Dynamic keeps generation on, 2x at least).
    frame_gen_target::Publish(frame_gen_target::Source::eDlssg,
        gAppliedDynamicMode.load(std::memory_order_relaxed)
            && gAppliedDynamicTargetValid.load(std::memory_order_acquire)
            && gAppliedFrameGenerationOn.load(std::memory_order_acquire),
        gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed), 2,
        gAppliedDynamicMaxFrames.load(std::memory_order_relaxed));

    const uint32_t previous =
        gActualFramesPresented.exchange(state.numFramesActuallyPresented,
            std::memory_order_relaxed);
    latency_probe::DlssgState(state.numFramesActuallyPresented, static_cast<uint32_t>(state.status));
    {
        // The Vulkan DLSS FPS readout: one sample per real frame (present_counter.h).
        const temporal_interval_trace::Snapshot trace = temporal_interval_trace::ReadSnapshot();
        LARGE_INTEGER now{};
        if (trace.initialized && trace.enabled && QueryPerformanceCounter(&now))
        {
            std::lock_guard lock(gDlssgPresentedMutex);
            gDlssgPresented.Sample(RealFrameCount(trace), state.numFramesActuallyPresented, now.QuadPart);
        }
    }
    gDlssgStatus.store(static_cast<uint32_t>(state.status), std::memory_order_relaxed);
    if (state.structVersion >= sl::kStructVersion2)
        gNumFramesToGenerateMax.store(
            UseAmpere() ? std::min(state.numFramesToGenerateMax, AmpereAdvertisedMaximum())
                : state.numFramesToGenerateMax, std::memory_order_relaxed);
    bool dynamicChanged = false;
    if (state.structVersion < sl::kStructVersion4)
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
    }
    if (state.structVersion >= sl::kStructVersion4)
    {
        // Dynamic V-Sync additionally requires the verified 2.14.1 runtime
        // and D3D12 route; this field alone remains a general FG capability.
        const bool vsyncKnown = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue
            || state.bIsVsyncSupportAvailable == sl::Boolean::eFalse;
        gFgVsyncSupported.store(vsyncKnown
            && state.bIsVsyncSupportAvailable == sl::Boolean::eTrue,
            std::memory_order_relaxed);
        gFgVsyncSupportKnown.store(vsyncKnown, std::memory_order_release);
        const bool known = state.bIsDynamicMFGSupported == sl::Boolean::eTrue
            || state.bIsDynamicMFGSupported == sl::Boolean::eFalse;
        const bool supported = known
            && state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        dynamicChanged = gDynamicMfgSupported.exchange(supported,
            std::memory_order_relaxed) != supported;
        dynamicChanged = (gDynamicMfgCapabilityKnown.exchange(known,
            std::memory_order_release) != known) || dynamicChanged;
    }
    gStateSampleTick.store(GetTickCount64(), std::memory_order_release);
    bool capacityChanged = false;
    if (UseAmpere())
    {
        static std::atomic<uint32_t> previousCapacity{0};
        const uint32_t capacity = ampere_backend::CertifiedMaximumGeneratedFrames();
        capacityChanged = previousCapacity.exchange(capacity) != capacity;
    }
    if (capacityChanged || dynamicChanged)
    {
        // A bootstrap clamp or unsupported Dynamic request is not the desired
        // final mode. Reapply after the owned allocation/capability changes on
        // either supported GPU family, including late Ada initialization.
        gAppliedRevision.store(0, std::memory_order_release);
        gAttemptedRevision.store(0, std::memory_order_release);
    }

    if (!UseAmpere() && previous != state.numFramesActuallyPresented)
        Log(L"DLSS-G state sample: frames presented since prior query=%u "
            L"(maximum generated per real frame=%u, status=%u)",
            state.numFramesActuallyPresented,
            gNumFramesToGenerateMax.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.status));
}

bool DynamicMfgCapabilityKnown() noexcept
{
    return gDynamicMfgCapabilityKnown.load(std::memory_order_acquire);
}

bool DynamicMfgSupported() noexcept
{
    return DynamicMfgCapabilityKnown()
        && nvidia_mfg_policy::DynamicRangeFits(
            gNumFramesToGenerateMax.load(std::memory_order_acquire), SafeMaximumMultiplier())
        && gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            != static_cast<uint32_t>(NgxGraphicsApi::eVulkan)
        && (!UseAmpere() || SafeMaximumMultiplier() == ampere_policy::kMaximumGeneratedFrames + 1u)
        && gDynamicMfgSupported.load(std::memory_order_acquire);
}

std::wstring ParentPath(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
        return right;
    if (left.back() == L'\\' || left.back() == L'/')
        return left + right;
    return left + L"\\" + right;
}

uint32_t ClassifyLoadedRoute(const std::wstring& path)
{
    if (!gExecutableDirectory.empty()
        && _wcsicmp(ParentPath(path).c_str(), gExecutableDirectory.c_str()) == 0)
        return kRouteLocal;
    return kRouteExternal;
}

bool AdapterVerifiedForApi(NgxGraphicsApi api) noexcept
{
    if (UseAmpere())
    {
        // RTX 30 Vulkan: the VkPhysicalDevice the host set up verified the adapter.
        return gpu_backend::AdapterVerified()
            && (api == NgxGraphicsApi::eD3D12
                || (api == NgxGraphicsApi::eVulkan && gVulkanAdapterVerified.load(std::memory_order_acquire)));
    }
    return gpu_backend::AdapterVerified()
        && (api != NgxGraphicsApi::eVulkan
            || gVulkanAdapterVerified.load(std::memory_order_acquire));
}

entry_detour::Snapshot EffectiveNgxDetour(
    entry_detour::Kind providerKind,
    entry_detour::Kind runtimeKind, bool create)
{
    const entry_detour::Handle activeHandle = UnpackEntryHandle(
        create ? gActiveNgxCreateHandle.load(std::memory_order_acquire)
               : gActiveNgxEvaluateHandle.load(std::memory_order_acquire));
    if (activeHandle)
        return entry_detour::ReadSnapshot(activeHandle);
    // Aggregate discovery is useful only before a provider is selected. Once
    // the first FG Create identifies a provider, never borrow coverage from a
    // different provider merely because it exposes the same operation.
    if (gActiveNgxProviderBase.load(std::memory_order_acquire) != 0)
        return {};
    const entry_detour::Snapshot provider =
        entry_detour::ReadSnapshot(providerKind);
    const entry_detour::Snapshot runtime =
        entry_detour::ReadSnapshot(runtimeKind);
    const NgxDispatchRoute route = static_cast<NgxDispatchRoute>(
        gActiveNgxDispatchRoute.load(std::memory_order_acquire));
    if (route == NgxDispatchRoute::eProvider)
        return provider;
    if (route == NgxDispatchRoute::eRuntime)
        return runtime;
    if (runtime.current)
        return runtime;
    return provider;
}

entry_detour::Snapshot EffectiveNgxCreateDetour()
{
    const NgxGraphicsApi api = static_cast<NgxGraphicsApi>(
        gActiveNgxGraphicsApi.load(std::memory_order_acquire));
    if (api == NgxGraphicsApi::eVulkan)
    {
        entry_detour::Snapshot create = EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanCreateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanCreateFeature, true);
        return create.current ? create : EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanCreateFeature1,
            entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1, true);
    }
    entry_detour::Snapshot d3d12 = EffectiveNgxDetour(
        entry_detour::Kind::eNgxD3D12CreateFeature,
        entry_detour::Kind::eNgxRuntimeD3D12CreateFeature, true);
    if (api == NgxGraphicsApi::eD3D12 || d3d12.current)
        return d3d12;
    entry_detour::Snapshot vulkan = EffectiveNgxDetour(
        entry_detour::Kind::eNgxVulkanCreateFeature,
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature, true);
    return vulkan.current ? vulkan : EffectiveNgxDetour(
        entry_detour::Kind::eNgxVulkanCreateFeature1,
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1, true);
}

entry_detour::Snapshot EffectiveNgxEvaluateDetour()
{
    const NgxGraphicsApi api = static_cast<NgxGraphicsApi>(
        gActiveNgxGraphicsApi.load(std::memory_order_acquire));
    if (api == NgxGraphicsApi::eVulkan)
    {
        return EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature, false);
    }
    entry_detour::Snapshot d3d12 = EffectiveNgxDetour(
        entry_detour::Kind::eNgxD3D12EvaluateFeature,
        entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature, false);
    return api == NgxGraphicsApi::eD3D12 || d3d12.current ? d3d12
        : EffectiveNgxDetour(
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature, false);
}

bool BridgeReady()
{
    ControlRouteRecord* control = ActiveControlRoute();
    if (control
        && !control->structureCompatible.load(std::memory_order_acquire))
    {
        // Preserve the more specific unknown-version or malformed-chain code
        // published by the adapter which rejected this route.
        return false;
    }
    const entry_detour::Snapshot create = EffectiveNgxCreateDetour();
    const entry_detour::Snapshot evaluate = EffectiveNgxEvaluateDetour();
    const bool awaitingOptions = control
        && control->activeSetterPath.load(std::memory_order_acquire)
            == static_cast<uint32_t>(ControlEntryPath::eNone)
        && control->setterCalls.load(std::memory_order_acquire) == 0
        && gNgxFrameGenerationCreateCalls.load(std::memory_order_acquire) == 0
        && gActiveNgxProviderBase.load(std::memory_order_acquire) == 0;
    const universal_route_policy::Readiness state{
        control != nullptr,
        control && control->wrapperPatched,
        control && control->structureCompatible.load(
            std::memory_order_acquire),
        control && (awaitingOptions ? SetterEntryCovered(*control)
                                   : ActiveSetterCovered(*control)),
        control && StateEntryCovered(*control),
        gActiveNgxProviderBase.load(std::memory_order_acquire) != 0,
        gProviderChangedAfterCreate.load(std::memory_order_acquire),
        create.current,
        evaluate.current,
        AdapterVerifiedForApi(static_cast<NgxGraphicsApi>(
            gActiveNgxGraphicsApi.load(std::memory_order_acquire))),
        gpu_backend::Ready()
            && (!gpu_dispatch::IsAda() || ngx_mfg_gate::Ready(reinterpret_cast<HMODULE>(
                gActiveNgxProviderBase.load(std::memory_order_acquire))))
            && gFirstCreateMidpointReady.load(std::memory_order_acquire),
        awaitingOptions,
    };
    const UniversalRouteFailure failure =
        universal_route_policy::EvaluateReadiness(state);
    SetUniversalRouteFailure(failure);
    return failure == UniversalRouteFailure::eNone;
}

const char* PatchRouteName()
{
    const uintptr_t remix = gRemixRuntimeBase.load(std::memory_order_acquire);
    if (remix && remix != UINTPTR_MAX && !ActiveControlRoute())
        return "remix"; // Detection only; Remix owns FG options and allocation.
    if (!BridgeReady())
        return "pending";

    ControlRouteRecord* control = ActiveControlRoute();
    if (!control)
        return "pending";
    return ClassifyLoadedRoute(control->path) == kRouteLocal
        ? "local" : "external";
}

bool FindJsonValue(const std::string& content, const char* name, size_t& value)
{
    const std::string key = std::string("\"") + name + "\"";
    const auto keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const auto colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    value = content.find_first_not_of(" \t\r\n", colon + 1);
    return value != std::string::npos;
}

bool TryParseUnsigned(const std::string& content, const char* name,
    uint32_t minimum, uint32_t maximum, uint32_t& value)
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset) || content[offset] < '0' || content[offset] > '9')
        return false;

    uint64_t parsed = 0;
    size_t end = offset;
    while (end < content.size() && content[end] >= '0' && content[end] <= '9')
    {
        parsed = parsed * 10 + static_cast<uint32_t>(content[end] - '0');
        if (parsed > maximum)
            return false;
        ++end;
    }
    if (parsed < minimum || parsed > maximum || !ui_status_json::ValueEnd(content, end))
        return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool TryParseBoolean(const std::string& content, const char* name, bool& value)
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0)
    {
        value = true;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0)
    {
        value = false;
        return true;
    }
    return false;
}

bool TryParseControl(const char* data, size_t size, ControlConfig& control)
{
    if (!data || size == 0)
        return false;

    const std::string content(data, size);
    if (!ui_status_json::CompleteObject(content))
        return false;
    ControlConfig parsed{};
    size_t followGameOffset = 0;
    if (FindJsonValue(content, "followGame", followGameOffset))
    {
        if (!TryParseBoolean(content, "followGame", parsed.followGame))
            return false;
    }
    else
    {
        // Configs written before Follow game support represented an explicit
        // fixed/dynamic override. Preserve that meaning during migration.
        parsed.followGame = false;
    }
    if (!TryParseUnsigned(content, "multiplier",
        1u, kMaximumMultiplier, parsed.multiplier))
        return false;

    size_t modeOffset = 0;
    if (FindJsonValue(content, "mode", modeOffset))
    {
        if (content.compare(modeOffset, 9, "\"dynamic\"") == 0)
            parsed.dynamic = true;
        else if (content.compare(modeOffset, 8, "\"follow\"") == 0
            && parsed.followGame)
            parsed.dynamic = false;
        else if (content.compare(modeOffset, 7, "\"fixed\"") != 0)
            return false;
    }

    if (parsed.followGame)
        parsed.dynamic = false;

    size_t targetOffset = 0;
    if (FindJsonValue(content, "dynamicTargetFrameRate", targetOffset)
        && !TryParseUnsigned(content, "dynamicTargetFrameRate", 0, 1000,
            parsed.dynamicTargetFrameRate))
        return false;

    size_t presetOffset = 0;
    if (FindJsonValue(content, "dlssgPreset", presetOffset)
        && !TryParseUnsigned(content, "dlssgPreset", 0, 3, parsed.dlssgPreset))
        return false;
    // latency.1: B saved before the default changed is the old default, not a choice.
    uint32_t dlssgDefaults = 0;
    if (FindJsonValue(content, "dlssgDefaults", presetOffset)
        && !TryParseUnsigned(content, "dlssgDefaults", 0, 65535, dlssgDefaults))
        return false;
    if (dlssgDefaults == 0 && parsed.dlssgPreset == dlssg_preset::kPresetB)
        parsed.dlssgPreset = dlssg_preset::kGameOrDriver;
    // A letter this build does not offer selects the game/driver preset
    // instead of rejecting every other saved option.
    if (FindJsonValue(content, "dlssSrPreset", presetOffset)
        && !TryParseUnsigned(content, "dlssSrPreset", 0, 0xFFFFFF, parsed.dlssSrPreset))
        return false;
    if (!dlss_sr::IsValidSelection(parsed.dlssSrPreset))
        parsed.dlssSrPreset = dlss_sr::kGameOrDriver;
    if (FindJsonValue(content, "dlssRrPreset", presetOffset)
        && !TryParseUnsigned(content, "dlssRrPreset", 0, 0xFFFFFF, parsed.dlssRrPreset))
        return false;
    if (!dlss_sr::IsValidRrSelection(parsed.dlssRrPreset))
        parsed.dlssRrPreset = dlss_sr::kGameOrDriver;

    if (!dlss5::settings_json::Parse(content, parsed.dlss5)) return false;
    size_t experimentalOffset = 0;
    size_t pacingOffset = 0;
    if (FindJsonValue(content, "vsyncMode", pacingOffset)
        && !TryParseUnsigned(content, "vsyncMode", 0, 2, parsed.vsyncMode))
        return false;
    if (FindJsonValue(content, "reflexFrameLimitFps", pacingOffset)
        && !TryParseUnsigned(content, "reflexFrameLimitFps", 0, 1000,
            parsed.reflexFrameLimitFps))
        return false;
    size_t xessOffset = 0;
    if (FindJsonValue(content, "xessModel", xessOffset)
        && !TryParseUnsigned(content, "xessModel", 0, 1, parsed.xessModel))
        return false;
    if (FindJsonValue(content, "xessVulkan", xessOffset)
        && !TryParseBoolean(content, "xessVulkan", parsed.xessVulkan))
        return false;
    if (FindJsonValue(content, "xessVulkanFrames", xessOffset)
        && !TryParseUnsigned(content, "xessVulkanFrames", 2, 6, parsed.xessVulkanFrames))
        return false;
    uint32_t xemfgStandalone = 0;
    if (FindJsonValue(content, "xemfgStandalone", xessOffset)
        && !TryParseUnsigned(content, "xemfgStandalone", 0, 1, xemfgStandalone))
        return false;
    parsed.xemfgStandalone = xemfgStandalone != 0;
    if (FindJsonValue(content, "xemfgFrames", xessOffset)
        && !TryParseUnsigned(content, "xemfgFrames", 2, 6, parsed.xemfgFrames))
        return false;
    // Dynamic MFG for XeMFG standalone.
    uint32_t xemfgDynamic = 0, xemfgAllow1x = 1, xemfgTargetAsCap = 0;
    if ((FindJsonValue(content, "xemfgDynamic", xessOffset)
            && !TryParseUnsigned(content, "xemfgDynamic", 0, 1, xemfgDynamic))
        || (FindJsonValue(content, "xemfgTargetFps", xessOffset)
            && !TryParseUnsigned(content, "xemfgTargetFps", 0, 1000, parsed.xemfgTargetFps))
        || (FindJsonValue(content, "xemfgRounding", xessOffset)
            && !TryParseUnsigned(content, "xemfgRounding", 0, 2, parsed.xemfgRounding))
        || (FindJsonValue(content, "xemfgAllow1x", xessOffset)
            && !TryParseUnsigned(content, "xemfgAllow1x", 0, 1, xemfgAllow1x))
        || (FindJsonValue(content, "xemfgTargetAsCap", xessOffset)
            && !TryParseUnsigned(content, "xemfgTargetAsCap", 0, 1, xemfgTargetAsCap))
        // sapacing.1: XeMFG standalone's flip queue.
        || (FindJsonValue(content, "xemfgFlipQueue", xessOffset)
            && !TryParseUnsigned(content, "xemfgFlipQueue", 0, 2, parsed.xemfgFlipQueue))
        // latency.1: low-latency pacing.
        || (FindJsonValue(content, "latencyPacing", xessOffset)
            && !TryParseUnsigned(content, "latencyPacing", 0, 2, parsed.latencyPacing)))
        return false;
    parsed.xemfgDynamic = xemfgDynamic != 0;
    parsed.xemfgAllow1x = xemfgAllow1x != 0;
    parsed.xemfgTargetAsCap = xemfgTargetAsCap != 0;
    if (FindJsonValue(content, "dynamicExperimental56", experimentalOffset)
        && !TryParseBoolean(content, "dynamicExperimental56",
            parsed.dynamicExperimental56))
        return false;

    size_t intervalLoggingOffset = 0;
    bool legacyIntervalLogging = gpu_dispatch::UsesNativeProgram() || UseAmpere();
    if (FindJsonValue(content, "intervalLogging", intervalLoggingOffset)
        && !TryParseBoolean(content, "intervalLogging",
            legacyIntervalLogging))
        return false;
    // Both selected backends retain temporal-request tracing for FPS telemetry.
    // Accept legacy false settings without allowing them to disable collection.
    parsed.intervalLogging = gpu_dispatch::UsesNativeProgram() || UseAmpere()
        || legacyIntervalLogging;

    size_t generatedOnlyOffset = 0;
    if (FindJsonValue(content, "generatedOnlyDebug", generatedOnlyOffset)
        && !TryParseBoolean(content, "generatedOnlyDebug",
            parsed.generatedOnlyDebug))
        return false;

    // Protocol 18 retired single-wrapper redirection. Keep accepting the old
    // key so existing configs migrate without being rejected; it is ignored.
    size_t selectiveWrapperOffset = 0;
    bool selectiveWrapperIgnored = false;
    if (FindJsonValue(content, "selectiveOtaDlssgWrapper",
            selectiveWrapperOffset)
        && !TryParseBoolean(content, "selectiveOtaDlssgWrapper",
            selectiveWrapperIgnored))
    {
        return false;
    }

    size_t optiScalerUiOffset = 0;
    if (FindJsonValue(content, "optiScalerUi", optiScalerUiOffset))
    {
        if (content.compare(optiScalerUiOffset, 8, "\"rtxmfg\"") == 0)
            parsed.optiScalerUi = 1;
        else if (content.compare(optiScalerUiOffset, 12, "\"optiscaler\"") == 0)
            parsed.optiScalerUi = 2;
        else if (content.compare(optiScalerUiOffset, 8, "\"shared\"") == 0)
            parsed.optiScalerUi = 0;
        else
            return false;
    }

    if (UseAmpere())
    {
        if (!ampere_policy::ControlValid(parsed.multiplier, parsed.dynamic,
                parsed.dynamicExperimental56, parsed.generatedOnlyDebug)) return false;
    }
    parsed.dynamicExperimental56 = false;
    control = parsed;
    return true;
}

bool ReadControlFile(const std::wstring& path, ControlConfig& control)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    std::array<char, 4096> buffer{};
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return read && TryParseControl(buffer.data(), bytesRead, control);
}

bool ReadLastWriteTime(const std::wstring& path, FILETIME& writeTime)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
        return false;
    writeTime = attributes.ftLastWriteTime;
    return true;
}

ControlConfig ReadInitialControl()
{
    ControlConfig control{};
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        MFG_ENV_PREFIX_W L"ACTIVE_MULTIPLIER", value, _countof(value));
    if (length == 1 && value[0] >= L'2' && value[0] <= L'6')
    {
        control.multiplier = static_cast<uint32_t>(value[0] - L'0');
        control.followGame = false; // Preserve an explicit environment override.
    }

    ControlConfig fileControl{};
    if (ReadControlFile(gConfigPath, fileControl))
        return fileControl;

    // A dedicated CET install keeps its live control inside CET's mandatory
    // per-mod filesystem sandbox. Preserve an existing executable-side choice
    // as the first-launch fallback; the next CET selection is written to the
    // sandbox-local file watched by this core.
    const std::wstring executableConfig = JoinPath(
        gExecutableDirectory, MFG_CONFIG_W);
    if (_wcsicmp(gConfigPath.c_str(), executableConfig.c_str()) != 0
        && ReadControlFile(executableConfig, fileControl))
    {
        return fileControl;
    }
    return control;
}

// OptiScaler mode: OptiScaler's menu owns the frame-generation controls.
// RTXMFG follows the caller's frame-generation requests (OptiScaler's
// ratio/interpolation count, or the game's own setting) and applies none of
// its own frame-generation or latency overrides. In the shared
// UI mode (the default without OptiScaler's plugin API) RTXMFG's menu still offers the
// DLSS SR/RR presets, the DLSS-G preset (OptiScaler has none; dlssgtab.1) and
// DLSS 5, which apply; hidden mode ("optiscaler", and
// the default once the API is offered or for a ForzaTech executable) delegates those too, unless
// OptiScaler handed RTXMFG its plugin API (menutab.2): then RTXMFG's tab in
// OptiScaler's menu offers them and they apply. The saved file is not
// rewritten, so standalone launches keep their choices.
ControlConfig EffectiveControl(const ControlConfig& saved) noexcept
{
    if (!optiscaler_host::UiDelegated())
        return saved;
    ControlConfig effective = saved;
    effective.followGame = true;
    effective.dynamic = false;
    effective.dynamicTargetFrameRate = 0;
    effective.dlssgPreset = dlssg_preset::kGameOrDriver;
    effective.dlssSrPreset = dlss_sr::kGameOrDriver;
    effective.dlssRrPreset = dlss_sr::kGameOrDriver;
    effective.dlss5.enabled = false;
    effective.vsyncMode = 0;
    effective.reflexFrameLimitFps = 0;
    effective.dynamicExperimental56 = false;
    effective.generatedOnlyDebug = false;
    if (!optiscaler_host::UiHidden() || optiscaler_menu_tab::Offered())
    {
        effective.dlssgPreset = saved.dlssgPreset;
        effective.dlssSrPreset = saved.dlssSrPreset;
        effective.dlssRrPreset = saved.dlssRrPreset;
        effective.dlss5 = saved.dlss5;
    }
    return effective;
}

// The DLSS SR preset uses the DLSS-G preset's NvAPI DRS read hook. The SR
// provider reads its DRS key when NGX initializes it, which can come before
// anything selected a GPU family; the game's NGX device selects RTX 40 or
// RTX 50 here (ObserveNativeD3D12Device never selects RTX 30). RTX 30/20 select
// their family in PrepareNgxInitialization, which calls this again.
// Vulkan passes no device: the VkPhysicalDevice selected the family.
bool InstallDlssSrDrsHook(ID3D12Device* device) noexcept
{
    if (gpu_dispatch::Selected() == gpu_dispatch::Family::eUnknown && device)
        gpu_dispatch::ObserveNativeD3D12Device(device);
    if (!LivePresetGpu()) return false;
    // Only native providers take the DLSS-G key from NvAPI; the translated
    // route answers it at its own reader hook.
    if (!NativeDlssgPresetGpu()) dlssg_preset::SetNvapiFrameGenerationReads(false);
    return dlssg_preset::Prepare();
}

uint32_t ReadInitialDlssSrPreset() noexcept
{
    // The first DLSS SR Create can come before the worker published control.
    try { return EffectiveControl(ReadInitialControl()).dlssSrPreset; }
    catch (...) { return dlss_sr::kGameOrDriver; }
}

uint32_t ReadInitialDlssRrPreset() noexcept
{
    try { return EffectiveControl(ReadInitialControl()).dlssRrPreset; }
    catch (...) { return dlss_sr::kGameOrDriver; }
}

uint32_t ReadInitialDlssgPreset() noexcept
{
    // Called at the first provider settings query if the worker has not yet
    // published control. Registering this callback in DllMain performs no I/O.
    try { return EffectiveControl(ReadInitialControl()).dlssgPreset; }
    catch (...) { return dlssg_preset::kGameOrDriver; }
}

std::wstring ResolveConfigPath(HMODULE, const std::wstring& executableDirectory)
{
    // The integrated UI owns this transport; an old CET script is not a
    // consumer of the unified filenames and must not redirect the worker.
    return unified_control_paths::Config(executableDirectory);
}

std::wstring ResolveStatusPath(const std::wstring& configPath,
    const std::wstring& executableDirectory)
{
    return unified_control_paths::Status(configPath, executableDirectory);
}

uint64_t StoreControl(const ControlConfig& control)
{
    std::lock_guard controlLock(gControlSnapshotMutex);
    gDesiredFollowGame.store(control.followGame, std::memory_order_relaxed);
    gDesiredMultiplier.store(control.multiplier, std::memory_order_relaxed);
    gDesiredDynamicMode.store(control.dynamic, std::memory_order_relaxed);
    gDynamicTargetFrameRate.store(control.dynamicTargetFrameRate, std::memory_order_relaxed);
    gDlssgPresetRequested.store(control.dlssgPreset, std::memory_order_relaxed);
    gVsyncMode.store(control.vsyncMode, std::memory_order_relaxed);
    gReflexFrameLimitFps.store(control.reflexFrameLimitFps, std::memory_order_relaxed);
    dlssg_preset::SetRequested(control.dlssgPreset);
    dlss_sr::SetRequested(control.dlssSrPreset);
    dlss_sr::SetRequestedRr(control.dlssRrPreset);
    dlss5::Configure(control.dlss5);
    gDynamicExperimental56.store(control.dynamicExperimental56, std::memory_order_relaxed);
    gGeneratedOnlyDebug.store(control.generatedOnlyDebug,
        std::memory_order_relaxed);
    temporal_interval_trace::SetEnabled(gpu_dispatch::UsesNativeProgram() || UseAmpere()
        || control.intervalLogging);
    const uint64_t revision = gDesiredRevision.fetch_add(1, std::memory_order_release) + 1;
    gControlReady.store(true, std::memory_order_release);
    return revision;
}

ControlSnapshot ReadControlSnapshot()
{
    std::lock_guard controlLock(gControlSnapshotMutex);
    ControlSnapshot snapshot{};
    for (;;)
    {
        const uint64_t before = gDesiredRevision.load(std::memory_order_acquire);
        snapshot.control.followGame =
            gDesiredFollowGame.load(std::memory_order_relaxed);
        snapshot.control.multiplier = gDesiredMultiplier.load(std::memory_order_relaxed);
        snapshot.control.dynamic = gDesiredDynamicMode.load(std::memory_order_relaxed);
        snapshot.control.dynamicTargetFrameRate =
            gDynamicTargetFrameRate.load(std::memory_order_relaxed);
        snapshot.control.dlssgPreset =
            gDlssgPresetRequested.load(std::memory_order_relaxed);
        snapshot.control.vsyncMode = gVsyncMode.load(std::memory_order_relaxed);
        snapshot.control.reflexFrameLimitFps =
            gReflexFrameLimitFps.load(std::memory_order_relaxed);
        snapshot.control.dynamicExperimental56 =
            gDynamicExperimental56.load(std::memory_order_relaxed);
        snapshot.control.generatedOnlyDebug =
            gGeneratedOnlyDebug.load(std::memory_order_relaxed);
        snapshot.control.intervalLogging = temporal_interval_trace::Enabled();
        const uint64_t after = gDesiredRevision.load(std::memory_order_acquire);
        if (before == after)
        {
            snapshot.revision = after;
            // A late OptiScaler detection applies before the worker republishes.
            if (optiscaler_host::UiDelegated())
            {
                snapshot.control.followGame = true;
                snapshot.control.dynamic = false;
                snapshot.control.generatedOnlyDebug = false;
                snapshot.control.dynamicExperimental56 = false;
            }
            return snapshot;
        }
    }
}

void PublishLiveBridge(const ControlConfig& control)
{
    wchar_t multiplier[2]{ static_cast<wchar_t>(L'0' + std::clamp(
        control.multiplier, kMinimumMultiplier, kMaximumMultiplier)), L'\0' };
    wchar_t target[16]{};
    swprintf_s(target, L"%u", control.dynamicTargetFrameRate);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"ACTIVE_MULTIPLIER", multiplier);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"ACTIVE_MODE",
        control.followGame ? L"follow"
            : control.dynamic ? L"dynamic" : L"fixed");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"FOLLOW_GAME",
        control.followGame ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"DYNAMIC_TARGET", target);
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"DYNAMIC_EXPERIMENTAL_56",
        control.dynamicExperimental56 ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"GENERATED_ONLY_DEBUG",
        control.generatedOnlyDebug ? L"1" : L"0");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"INTERVAL_LOGGING", L"1");
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"AUTO_BRIDGE", L"1");
}

void PublishPatchRoute()
{
    const char* route = PatchRouteName();
    wchar_t wideRoute[16]{};
    MultiByteToWideChar(CP_UTF8, 0, route, -1, wideRoute, _countof(wideRoute));
    SetEnvironmentVariableW(MFG_ENV_PREFIX_W L"PATCH_ROUTE", wideRoute);
}

uint64_t UnixTimeSeconds()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    constexpr uint64_t kWindowsToUnixEpoch = 116444736000000000ULL;
    return (ticks.QuadPart - kWindowsToUnixEpoch) / 10000000ULL;
}

std::string JsonEscapeUtf8(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (const unsigned char c : source)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += static_cast<char>(c);
    }
    return out;
}

// The game's selected frame generation: XeSS-FG while its context is enabled
// (or a native context reported activity within 2 s), else DLSS-G while the
// game requests it. Only one is active per process.
const char* ActiveFrameGeneration(bool xessActive) noexcept
{
    if (xessActive) return "xess";
    if (gGameFrameGenerationOn.load(std::memory_order_acquire)) return "dlss";
    return "none";
}

// The XeMFG Vulkan host's frame count: the menu's own setting, else the fixed multiplier (Follow = 2X).
uint32_t XessVulkanFrames(const ControlConfig& control) noexcept
{
    return control.xessVulkanFrames ? control.xessVulkanFrames : control.followGame ? 2u : control.multiplier;
}

// XeMFG standalone's frame count, the same fallback.
uint32_t XemfgFrames(const ControlConfig& control) noexcept
{
    return std::clamp<uint32_t>(control.xemfgFrames ? control.xemfgFrames : control.followGame ? 2u : control.multiplier, 2, 6);
}

// Both hosts take the switch; the Vulkan host also the harness builds' xessVulkan key.
void ConfigureXemfg(const ControlConfig& control) noexcept
{
    // sapacing.1: before either host initializes XeFG, where the queue is decided once.
    xess_mod::SetStandaloneFlipQueue(control.xemfgFlipQueue);
    // latency.1: live, for every XeFG context sdk_pacing paces (OptiScaler's included).
    sdk_pacing::SetLatencyPacing(control.latencyPacing);
    // Under OptiScaler its XeFG outputs host XeMFG (a saved switch from a session without it stays inert).
    xess_vulkan::Configure(control.xessVulkan, control.xemfgStandalone && !optiscaler_host::Detected(),
        control.xemfgFrames ? XemfgFrames(control) : XessVulkanFrames(control));
    xess_d3d12::Configure(control.xemfgStandalone, XemfgFrames(control));
    // The menu's Reflex FPS limit. The game's frames sleep in XeLL while XeMFG generates, where the driver's
    // Reflex limit does not reach them; the hosts apply it to XeFG's presented frames as XeLL's interval.
    xess_d3d12::SetFpsLimit(control.reflexFrameLimitFps);
    xess_vulkan::SetStandaloneFpsLimit(control.reflexFrameLimitFps);
    // Dynamic MFG, up to XemfgFrames.
    xess_d3d12::SetDynamic(control.xemfgDynamic, control.xemfgTargetFps, int(control.xemfgRounding), control.xemfgAllow1x,
        control.xemfgTargetAsCap);
    xess_vulkan::SetStandaloneDynamic(control.xemfgDynamic, control.xemfgTargetFps, int(control.xemfgRounding),
        control.xemfgAllow1x, control.xemfgTargetAsCap);
}

// XeMFG replaces the game's DLSS Frame Generation (held at Off) while a host may run it.
bool XemfgReplacesDlssg() noexcept
{
    return xess_d3d12::ReplacesFrameGeneration() || xess_vulkan::ReplacesFrameGeneration();
}

// latency.1: low-latency pacing's readback for the menu in one key (an RTX 30 status is close to the 512-key limit):
// the saved mode, the real frames measured, then over the frames since the previous status (window) the hold in
// Present, the hold without the sleep (slack), the sleep moved before the input, the sleeps taken and missed (no sleep
// point reached), the guard trips and the time since the last real frame (ms, -1 none).
std::string LatencyPacingStatus(const ControlConfig& control)
{
    const auto latency = sdk_pacing::ReadLatencyPacing();
    char fragment[384]{};
    sprintf_s(fragment, ",\"latencyPacingDetail\":\"mode=%u frames=%llu window=%llu held=%.2f slack=%.2f slept=%.2f "
        "takes=%llu missed=%llu backoffs=%llu ineffective=%llu age=%lld\"",
        control.latencyPacing, static_cast<unsigned long long>(latency.frames),
        static_cast<unsigned long long>(latency.windowFrames), latency.heldMs, latency.slackMs, latency.sleptMs,
        static_cast<unsigned long long>(latency.takes), static_cast<unsigned long long>(latency.missed),
        static_cast<unsigned long long>(latency.backoffs), static_cast<unsigned long long>(latency.ineffective),
        static_cast<long long>(latency.ageMs));
    return fragment;
}

// XeMFG standalone's status for the menu, from the host of the game's API.
std::string XemfgStatus(const ControlConfig& control)
{
    const auto d3d12 = xess_d3d12::ReadSnapshot();
    const auto vulkan = xess_vulkan::ReadSnapshot();
    const bool useVulkan = vulkan.vulkanGame && !d3d12.active;
    std::string state = useVulkan ? vulkan.state : d3d12.state;
    if (useVulkan && control.xemfgStandalone && vulkan.restartNeeded) state = "restart";
    if (useVulkan && optiscaler_host::Detected()) state = "optiscaler";
    const char* source = useVulkan ? (vulkan.inputFrames ? "dlss" : "none")
        : d3d12.source == 1 ? "streamline" : d3d12.source == 2 ? "dlss" : d3d12.source == 3 ? "xess"
        : d3d12.source == 4 ? "fsr" : "none";
    return std::string(",\"xemfgStandalone\":") + (control.xemfgStandalone ? "true" : "false")
        + ",\"xemfgSavedFrames\":" + std::to_string(XemfgFrames(control))
        + ",\"xemfgApi\":\"" + (useVulkan ? "vulkan" : "d3d12") + "\""
        + ",\"xemfgState\":\"" + JsonEscapeUtf8(state) + "\""
        + ",\"xemfgReason\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.reason : d3d12.reason) + "\""
        + ",\"xemfgSource\":\"" + source + "\""
        + ",\"xemfgFrames\":" + std::to_string(useVulkan ? vulkan.frames : d3d12.frames)
        + ",\"xemfgGenerating\":" + ((useVulkan ? vulkan.active : d3d12.generating) ? "true" : "false")
        + ",\"xemfgGameFg\":" + ((useVulkan ? vulkan.gameFrameGeneration : d3d12.gameFrameGeneration) ? "true" : "false")
        + ",\"xemfgFsrFg\":" + (d3d12.fsrFrameGeneration ? "true" : "false")
        + ",\"xemfgRuntime\":\"" + JsonEscapeUtf8(d3d12.runtime) + "\""
        // After a failed runtime search, where the pair belongs and a near-miss name (the menu's note).
        + ",\"xemfgRuntimeSearch\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeSearch : d3d12.runtimeSearch) + "\""
        + ",\"xemfgRuntimeGame\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeGame : d3d12.runtimeGame) + "\""
        + ",\"xemfgRuntimeMisnamed\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeMisnamed : d3d12.runtimeMisnamed) + "\""
        // oldruntime.1: a pair short of exports, its short file and version, and where a newer pair takes over.
        + ",\"xemfgRuntimeOld\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeOld : d3d12.runtimeOld) + "\""
        + ",\"xemfgRuntimeOldFile\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeOldFile : d3d12.runtimeOldFile) + "\""
        + ",\"xemfgRuntimeOldVersion\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeOldVersion : d3d12.runtimeOldVersion) + "\""
        + ",\"xemfgRuntimeNewer\":\"" + JsonEscapeUtf8(useVulkan ? vulkan.runtimeNewer : d3d12.runtimeNewer) + "\""
        + ",\"xemfgBasePresents\":" + std::to_string(std::min<uint64_t>(useVulkan ? vulkan.basePresents : d3d12.basePresents, UINT32_MAX))
        + ",\"xemfgInputFrames\":" + std::to_string(std::min<uint64_t>(useVulkan ? vulkan.inputFrames : d3d12.inputFrames, UINT32_MAX))
        // The menu's FPS row and pinned readout, and the FPS limit's readback.
        + ",\"xemfgBaseFpsMilli\":" + std::to_string(useVulkan ? vulkan.baseFpsMilli : d3d12.baseFpsMilli)
        + ",\"xemfgOutputFpsMilli\":" + std::to_string(useVulkan ? vulkan.outputFpsMilli : d3d12.outputFpsMilli)
        + ",\"xemfgFpsAgeMs\":" + std::to_string(std::min<uint64_t>(useVulkan ? vulkan.fpsAgeMs : d3d12.fpsAgeMs, UINT32_MAX))
        + ",\"xemfgFpsLimit\":" + std::to_string(useVulkan ? vulkan.fpsLimit : d3d12.fpsLimit)
        + ",\"xemfgFpsLimited\":" + ((useVulkan ? vulkan.latencyLimited : d3d12.limitIntervalUs != 0) ? "true" : "false")
        // Dynamic MFG's saved settings, and while it runs its target and cap mode.
        + ",\"xemfgDynamic\":" + (control.xemfgDynamic ? "true" : "false")
        + ",\"xemfgTargetFps\":" + std::to_string(control.xemfgTargetFps)
        + ",\"xemfgRounding\":" + std::to_string(control.xemfgRounding)
        + ",\"xemfgAllow1x\":" + (control.xemfgAllow1x ? "true" : "false")
        + ",\"xemfgTargetAsCap\":" + (control.xemfgTargetAsCap ? "true" : "false")
        + ",\"xemfgDynamicActive\":" + ((useVulkan ? vulkan.dynamic : d3d12.dynamic) ? "true" : "false")
        + ",\"xemfgDynamicTargetMilli\":" + std::to_string(useVulkan ? vulkan.dynamicTargetMilli : d3d12.dynamicTargetMilli)
        + ",\"xemfgDynamicCap\":" + ((useVulkan ? vulkan.dynamicCap : d3d12.dynamicCap) ? "true" : "false")
        + LatencyPacingStatus(control);
}

// The XeMFG Vulkan host's status, with or without the D3D12 XeSS route.
std::string XessVulkanStatus(const ControlConfig& control)
{
    const auto vulkan = xess_vulkan::ReadSnapshot();
    return XemfgStatus(control) + ",\"xessVulkan\":" + (control.xessVulkan ? "true" : "false")
        + ",\"xessVulkanSavedFrames\":" + std::to_string(control.xessVulkanFrames)
        + ",\"xessVulkanState\":\"" + JsonEscapeUtf8(vulkan.state) + "\""
        + ",\"xessVulkanReason\":\"" + JsonEscapeUtf8(vulkan.reason) + "\""
        + ",\"xessVulkanGame\":" + (vulkan.vulkanGame ? "true" : "false")
        + ",\"xessVulkanRestart\":" + (vulkan.restartNeeded ? "true" : "false")
        + ",\"xessVulkanGameFg\":" + (vulkan.gameFrameGeneration ? "true" : "false")
        + ",\"xessVulkanHosted\":" + (vulkan.hosted ? "true" : "false")
        + ",\"xessVulkanFrames\":" + std::to_string(vulkan.frames)
        + ",\"xessVulkanBasePresents\":" + std::to_string(std::min<uint64_t>(vulkan.basePresents, UINT32_MAX))
        + ",\"xessVulkanPresents\":" + std::to_string(std::min<uint64_t>(vulkan.vulkanPresents, UINT32_MAX))
        + ",\"xessVulkanInputFrames\":" + std::to_string(std::min<uint64_t>(vulkan.inputFrames, UINT32_MAX))
        + ",\"xessVulkanMissingPrograms\":" + std::to_string(std::min<uint64_t>(vulkan.missingPrograms, UINT32_MAX));
}

std::string XessStatusFragment(const ControlConfig& control)
{
    // The XeSS route runs under OptiScaler only (xessHooksActive stays false without it).
    const auto snapshot = xess_mod::ReadSnapshot();
    if (!snapshot)
        return std::string(",\"xessAvailable\":false,\"activeFrameGeneration\":\"")
            + ActiveFrameGeneration(false) + "\"" + XessVulkanStatus(control);
    const uint64_t now = GetTickCount64();
    const uint64_t nativeAge = snapshot->nativeActivityTick && now >= snapshot->nativeActivityTick
        ? now - snapshot->nativeActivityTick : UINT64_MAX;
    const bool nativeActive = snapshot->nativeEnabled && nativeAge <= 2000;
    const bool xessActive = (snapshot->initialized && snapshot->enabled) || nativeActive;
    char numbers[768]{};
    sprintf_s(numbers, ",\"xessAvailable\":true,\"xessHooksActive\":%s,\"xessRuntimeSource\":%u,"
        "\"xessProviderLoaded\":%s,\"xessPortReady\":%s,\"xessInitialized\":%s,"
        "\"xessEnabled\":%s,\"xessGameEnabled\":%s,\"xessNativeEnabled\":%s,"
        "\"xessApplied\":%u,\"xessCapacity\":%u,"
        "\"xessFramesPresented\":%u,\"xessLastResult\":%d,\"xessLastFrameGenResult\":%d,"
        "\"xessProviderEnabled\":%s,\"xessAppFrames\":%llu,\"xessAdapterVendor\":%u",
        snapshot->importsInstalled ? "true" : "false", snapshot->runtimeSource,
        snapshot->providerLoaded ? "true" : "false", snapshot->portReady ? "true" : "false",
        snapshot->initialized ? "true" : "false", snapshot->enabled ? "true" : "false",
        snapshot->gameEnabled ? "true" : "false", nativeActive ? "true" : "false",
        snapshot->applied, snapshot->capacity,
        snapshot->framesPresented, snapshot->lastResult, snapshot->lastFrameGenResult,
        snapshot->providerEnabled ? "true" : "false",
        static_cast<unsigned long long>(snapshot->appFrames), snapshot->adapterVendor);
    // The ported model's int8 layers on tensor cores (0 not decided, 1 active, 2 unavailable, 3 off).
    const auto tensorCores = xefg_cuda::Snapshot();
    return numbers
        + std::string(",\"xessModel\":") + std::to_string(control.xessModel)
        + XessVulkanStatus(control)
        + ",\"xessModelActive\":" + std::to_string(snapshot->modelActive)
        + ",\"xessProviderFailing\":" + (snapshot->providerFailing ? "true" : "false")
        + ",\"xessTensorCores\":" + std::to_string(static_cast<uint32_t>(tensorCores.state))
        + ",\"xessTensorCoreLaunches\":" + std::to_string(std::min<uint64_t>(tensorCores.launches, UINT32_MAX))
        + ",\"xessTensorCoreFallbacks\":" + std::to_string(std::min<uint64_t>(tensorCores.fallbacks, UINT32_MAX))
        + ",\"xessTensorCoreReason\":\"" + JsonEscapeUtf8(tensorCores.reason) + "\""
        + ",\"xessRuntimeFailure\":\"" + JsonEscapeUtf8(snapshot->runtimeFailure)
        + "\",\"xessStatus\":\"" + JsonEscapeUtf8(snapshot->status)
        + "\",\"xessAdapter\":\"" + JsonEscapeUtf8(snapshot->adapter)
        + "\",\"activeFrameGeneration\":\"" + ActiveFrameGeneration(xessActive) + "\"";
}

std::string JsonEscapeWide(const std::wstring& source)
{
    if (source.empty())
        return {};
    const int byteCount = WideCharToMultiByte(CP_UTF8,
        WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()),
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
        return {};
    std::string utf8(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            source.data(), static_cast<int>(source.size()), utf8.data(),
            byteCount, nullptr, nullptr) != byteCount)
        return {};

    std::string escaped;
    escaped.reserve(utf8.size() + 16);
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char value : utf8)
    {
        switch (value)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (value < 0x20)
            {
                escaped += "\\u00";
                escaped.push_back(hex[value >> 4]);
                escaped.push_back(hex[value & 0x0F]);
            }
            else
            {
                escaped.push_back(static_cast<char>(value));
            }
            break;
        }
    }
    return escaped;
}

std::string OptiScalerStatusFragment()
{
    const auto host = optiscaler_host::Read();
    const char* owner = !host.detected ? "none" : host.uiHidden ? "optiscaler" : host.uiDelegated ? "shared" : "rtxmfg";
    return std::string(",\"integrationMode\":\"") + (host.detected ? "optiscaler" : "standalone")
        + "\",\"optiScalerDetected\":" + (host.detected ? "true" : "false")
        + ",\"optiScalerRoute\":\"" + optiscaler_host::RouteName(host.route)
        + "\",\"optiScalerUiOwner\":\"" + owner
        + "\",\"optiScalerMenuSuppressed\":" + (host.uiHidden ? "true" : "false")
        + ",\"optiScalerPluginCallerAccepted\":" + (host.pluginCallerAccepted ? "true" : "false")
        + ",\"optiScalerPluginCallerRejected\":" + (host.pluginCallerRejected ? "true" : "false")
        + ",\"optiScalerFileVersion\":\"" + JsonEscapeWide(host.fileVersion)
        + "\",\"optiScalerProductVersion\":\"" + JsonEscapeWide(host.productVersion)
        + "\",\"optiScalerPath\":\"" + JsonEscapeWide(host.path)
        + "\",\"fakeNvapiObserved\":" + (optiscaler_host::FakeNvapiObserved() ? "true" : "false");
}

const char* ControlDetourMethod(const ControlRouteRecord* route,
    bool setter) noexcept
{
    if (!route)
        return "none";
    const ControlEntryPath path = static_cast<ControlEntryPath>(
        (setter ? route->activeSetterPath : route->activeStatePath).load(
            std::memory_order_acquire));
    if (path == ControlEntryPath::eResolver)
        return "resolver";
    entry_detour::Handle handle{};
    if (setter)
        handle = path == ControlEntryPath::eInternal
            ? route->internalSetHandle : route->publicSetHandle;
    else
        handle = path == ControlEntryPath::eInternal
            ? route->internalGetHandle : route->publicGetHandle;
    return entry_detour::MethodName(
        entry_detour::ReadSnapshot(handle).method);
}

struct PresentationRuntimeProof
{
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uint64_t createHandle = 0;
    uint64_t evaluateHandle = 0;
    uint64_t lifecycleEpoch = 0;
    bool operator==(const PresentationRuntimeProof&) const = default;
};

std::mutex gPresentationProofMutex;
PresentationRuntimeProof gPresentationProof{};
uint64_t gPresentationRuntimeEpoch = 0;
bool gPresentationProviderEligible = false;

PresentationRuntimeProof CurrentPresentationProof() noexcept
{
    const auto* route = ActiveControlRoute();
    return {route ? reinterpret_cast<uintptr_t>(route->wrapper) : 0,
        route ? route->generation : 0,
        gActiveNgxProviderBase.load(std::memory_order_acquire),
        gActiveNgxProviderGeneration.load(std::memory_order_acquire),
        gActiveNgxCreateHandle.load(std::memory_order_acquire),
        gActiveNgxEvaluateHandle.load(std::memory_order_acquire),
        gPresentationLifecycleEpoch.load(std::memory_order_acquire)};
}

bool FrameGenerationRuntimeCurrent() noexcept
{
    // Callback-time validation performs no filesystem or NVIDIA calls. The
    // worker binds its version check to this exact selected route generation.
    const auto* route = ActiveControlRoute();
    if (!route || gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            == static_cast<uint32_t>(NgxGraphicsApi::eUnknown)
        || !gAppliedFrameGenerationOn.load(std::memory_order_acquire)
        || gRestartRequired.load(std::memory_order_acquire)
        || !BridgeReady()) return false;
    const auto current = CurrentPresentationProof();
    if (current.wrapper != reinterpret_cast<uintptr_t>(route->wrapper)
        || current.wrapperGeneration != route->generation) return false;
    std::lock_guard lock(gPresentationProofMutex);
    return current.provider && current.providerGeneration
        && current.createHandle && current.evaluateHandle
        && current == gPresentationProof;
}

bool PresentationRuntimeCurrent() noexcept
{
    const auto* route = ActiveControlRoute();
    if (!route || route->version.major != 2 || route->version.minor != 14
        || route->version.build != 1 || !FrameGenerationRuntimeCurrent()) return false;
    std::lock_guard lock(gPresentationProofMutex);
    return gPresentationProviderEligible;
}

bool ReflexRuntimeCurrent() noexcept
{
    // The manual cap is suspended for both requested and game-owned Dynamic
    // mode. Configure retains its saved value and the next proven Reflex
    // callback restores the game's own options before any later re-enable.
    if (gDesiredDynamicMode.load(std::memory_order_acquire)
        || gAppliedDynamicMode.load(std::memory_order_acquire)
        || !FrameGenerationRuntimeCurrent()) return false;
    // Fixed D3D12 limiting uses the stable public Reflex API; it does not
    // depend on the private Dynamic/V-Sync implementation or provider 310.9.1.
    // Keep the existing Vulkan compatibility boundary intact.
    return gActiveNgxGraphicsApi.load(std::memory_order_acquire)
            == static_cast<uint32_t>(NgxGraphicsApi::eD3D12)
        || PresentationRuntimeCurrent();
}

bool VsyncRuntimeCurrent() noexcept
{
    // NVIDIA's runtime V-Sync (the On request, Dynamic V-Sync) is not offered.
    // The V-Sync Off override needs no runtime proof; see vsync_control.h.
    return false;
}

void UpdatePresentationPolicy(const ControlConfig& control)
{
    const auto current = CurrentPresentationProof();
    bool changed = false;
    {
        std::lock_guard lock(gPresentationProofMutex);
        changed = current != gPresentationProof;
    }
    if (changed)
    {
        const auto version = ReadFileVersion(LoadedModulePath(
            reinterpret_cast<HMODULE>(current.provider)));
        std::lock_guard lock(gPresentationProofMutex);
        gPresentationProviderEligible = current.provider != 0
            && current.providerGeneration != 0 && version.major == 310
            && version.minor == 9 && version.build == 1;
        gPresentationProof = current;
        ++gPresentationRuntimeEpoch;
    }
    reflex_control::Configure(control.reflexFrameLimitFps, ReflexRuntimeCurrent());
    vsync_control::Configure(control.vsyncMode);
}

// OptiScaler's model choice for its own XeFG: [XeFG] XeFGModel (or the earlier RtxmfgModel) in the
// OptiScaler.ini beside its module. 0 auto (the control file's xessModel), 1 our port, 2 XeFG's 2x
// model. xess_mod reads it again when the model is chosen; this copy only seeds ConfigureModel.
uint32_t OptiScalerHostModel()
{
    std::wstring setting, path, key;
    return optiscaler_host::ReadXefgModel(setting, path, key);
}

// Returns true for the call which opened plug-in retention (see the flag).
bool AllowStreamlinePluginRetention(const wchar_t* boundary)
{
    if (gStreamlinePluginRetention.exchange(true, std::memory_order_acq_rel))
        return false;
    gModuleInventoryDirty.store(true, std::memory_order_release);
    Log(L"Streamline plug-in retention allowed at %s (slInit calls=%llu)", boundary,
        static_cast<unsigned long long>(gSlInitCalls.load(std::memory_order_relaxed)));
    return true;
}

void DiscoverReflexModule()
{
    // This function is called only by the normal worker, after loader discovery
    // has returned. Never install this transport from a LoadLibrary callback.
    // Its entries pin sl.reflex, which a Streamline restart must still unload.
    if (!gStreamlinePluginRetention.load(std::memory_order_acquire))
        return;
    std::vector<ModuleRecord> modules;
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            // This export identifies plugin candidates, including NVIDIA OTA
            // filenames. ObserveModule then requires the owned Reflex entries.
            if (record.wrapperExport) modules.push_back(record);
        }
    }
    for (const auto& record : modules)
    {
        // Each route and each refusal is logged once with the plug-in version.
        std::array<uint32_t, 4> version{};
        const auto observed = reflex_control::ObserveModule(record.module, record.path.c_str(),
            record.generation, &version);
        const wchar_t* outcome = observed == reflex_control::Observation::eRouted ? L"hooked"
            : observed == reflex_control::Observation::eRefusedVersion ? L"refused (supported: 2.7-2.14)"
            : observed == reflex_control::Observation::eHookUnavailable ? L"refused (Reflex entries incomplete)"
            : nullptr;
        if (outcome)
            Log(L"Reflex FPS limit plug-in %s: version %u.%u.%u.%u path=%s", outcome,
                version[0], version[1], version[2], version[3], record.path.c_str());
    }
}

// Why a saved DLSS-G preset cannot replace the frozen one while the game runs;
// eNone when the next quiescent point (or one forced re-create) applies it.
// Beside OptiScaler the switch runs on OptiScaler's own DLSS-G calls: its
// private Streamline's sl.dlss_g is a control route like the game's, so the
// forced Off, the release and its next On re-create its feature
// (eOptiScaler is no longer reported).
dlssg_preset_switch::Unavailable PresetSwitchUnavailableReason(uint32_t requested) noexcept
{
    using Unavailable = dlssg_preset_switch::Unavailable;
    if (!LivePresetGpu()) return Unavailable::eGpu;
    // RTX 30/20: only a provider with the sole original preset stays
    // restart-only; a modern one switches through its adopted reader below.
    if (UseAmpere() && ampere_backend::LegacySinglePreset()) return Unavailable::eLegacyPreset;
    const auto api = static_cast<NgxGraphicsApi>(gActiveNgxGraphicsApi.load(std::memory_order_acquire));
    // The lifetime tracker covers the runtime D3D12 route, and
    // the runtime Vulkan route while its Create, Evaluate and Release are hooked.
    if (api == NgxGraphicsApi::eVulkan ? !ampere_backend::VulkanLifetimeCovered()
            : api != NgxGraphicsApi::eUnknown && api != NgxGraphicsApi::eD3D12)
        return Unavailable::eGraphicsApi;
    if (!ampere_backend::ReadFrameGenerationLiveness().exact) return Unavailable::eTracker;
    if (gRestartRequired.load(std::memory_order_acquire)
        || gPipelineMayPredateDetour.load(std::memory_order_acquire)
        || gProviderChangedAfterCreate.load(std::memory_order_acquire))
        return Unavailable::eRestartRequired;
    const auto provider = reinterpret_cast<HMODULE>(gActiveNgxProviderBase.load(std::memory_order_acquire));
    if (provider ? !dlssg_preset::ProviderSupportsLiveSwitch(provider)
            : !dlssg_preset::ReadProvidersSupportLiveSwitch())
        return Unavailable::eProvider;
    if (gPresetSwitchFailedRequest.load(std::memory_order_acquire) == requested) return Unavailable::eFailed;
    return Unavailable::eNone;
}

// DLSS Super Resolution detection and its preset selection.
// Flat fields only (see ampere_status_fields.h).
std::string DlssSrStatusFragment()
{
    const auto sr = dlss_sr::Read();
    const auto field = [](std::string& text, const char* format, auto... values)
    {
        char buffer[256]{};
        sprintf_s(buffer, format, values...);
        text += buffer;
    };
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    std::string text;
    text.reserve(2048);
    // One feature's view: SR fields keep their established dlssSr* names, RR uses dlssRr*.
    const auto feature = [&](const char* prefix, uint32_t requested, bool detected, bool running,
        uint32_t live, const dlss_sr::FeatureView& view, const dlss_sr::DrsView& drs)
    {
        field(text, ",\"%sPresetRequested\":%u,\"%sDetected\":%s,\"%sRunning\":%s,\"%sLiveFeatures\":%u",
            prefix, requested, prefix, boolean(detected), prefix, boolean(running), prefix, live);
        field(text, ",\"%sRenderWidth\":%u,\"%sRenderHeight\":%u,\"%sOutputWidth\":%u,\"%sOutputHeight\":%u",
            prefix, view.keys.width, prefix, view.keys.height, prefix, view.keys.outWidth, prefix, view.keys.outHeight);
        field(text, ",\"%sQuality\":%d,\"%sGameHint\":%u,\"%sActivePreset\":%u,\"%sLiveSwitch\":%u",
            prefix, view.valid ? view.keys.perfQuality : -1, prefix, view.gameHint, prefix, view.activePreset,
            prefix, static_cast<uint32_t>(view.unavailable));
        field(text, ",\"%sDrsObserved\":%s,\"%sDriverPresetValid\":%s,\"%sDriverPreset\":%u,\"%sDrsSuppressed\":%s",
            prefix, boolean(drs.observed), prefix, boolean(drs.driverValueValid), prefix, drs.driverValue,
            prefix, boolean(drs.suppressed));
        field(text, ",\"%sFixedByDriver\":%s,\"%sDriverOverrideCached\":%s",
            prefix, boolean(drs.fixedByDriver), prefix, boolean(drs.overrideCached));
    };
    feature("dlssSr", sr.requested, sr.detected, sr.running, sr.counters.liveSr, sr.primary, sr.sr);
    feature("dlssRr", sr.requestedRr, sr.rrDetected, sr.rayReconstructionRunning, sr.counters.liveRr,
        sr.rrPrimary, sr.rr);
    return text;
}

// disk=false refreshes only the in-process copy the menu reads (status_transport::Publish).
bool WriteBridgeStatus(const ControlConfig& control, DWORD pid, bool disk = true)
{
    if (gStatusPath.empty())
        return false;

    UpdateFpsTelemetryWithoutPresentCallback();
    const uint32_t uiViewport = gLastOptionsViewport.load(std::memory_order_acquire);
    const UiInputSnapshot uiInputs = ReadUiInputSnapshot(uiViewport);
    const bool bridgeReady = BridgeReady();
    const char* route = PatchRouteName();
    const uint64_t desiredRevision = gDesiredRevision.load(std::memory_order_acquire);
    const uint64_t appliedRevision = gAppliedRevision.load(std::memory_order_acquire);
    const bool setOptionsSeen = gSetOptionsSeen.load(std::memory_order_acquire);
    const bool getStateSeen = gGetStateSeen.load(std::memory_order_acquire);
    const bool gameFrameGenerationOn =
        gGameFrameGenerationOn.load(std::memory_order_acquire);
    const bool appliedFrameGenerationOn =
        gAppliedFrameGenerationOn.load(std::memory_order_acquire);
    const int32_t setOptionsResult =
        gLastSetOptionsResult.load(std::memory_order_relaxed);
    const int32_t getStateResult =
        gLastGetStateResult.load(std::memory_order_relaxed);
    const bool setOptionsAccepted = setOptionsResult == static_cast<int32_t>(sl::Result::eOk)
        || setOptionsResult == static_cast<int32_t>(sl::Result::eWarnOutOfVRAM);
    const bool applied = appliedFrameGenerationOn && appliedRevision != 0
        && setOptionsAccepted;
    const bool pending = gameFrameGenerationOn && desiredRevision != appliedRevision;
    const uint64_t stateTick = gStateSampleTick.load(std::memory_order_acquire);
    const uint64_t nowTick = GetTickCount64();
    const uint64_t stateAgeMs = stateTick == 0 || nowTick < stateTick
        ? 0 : nowTick - stateTick;
    const uint64_t fpsTick = gFpsSampleTick.load(std::memory_order_acquire);
    const uint64_t fpsAgeMs = fpsTick == 0 || nowTick < fpsTick
        ? 0 : nowTick - fpsTick;
    const entry_detour::Snapshot setOptionsDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgSetOptions);
    const entry_detour::Snapshot createDetour = EffectiveNgxCreateDetour();
    const entry_detour::Snapshot evaluateDetour = EffectiveNgxEvaluateDetour();
    const entry_detour::Snapshot getStateDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgGetState);
    const entry_detour::Snapshot slInitDetour =
        entry_detour::ReadSnapshot(entry_detour::Kind::eSlInit);
    const bool slInitIatFallback =
        gSlInitIatFallbackInstalled.load(std::memory_order_acquire);
    const bool slInitResolverFallback =
        gSlInitResolverFallbackActive.load(std::memory_order_acquire);
    const bool slInitControlPathReady = slInitDetour.current
        || slInitIatFallback || slInitResolverFallback
        || gMainResolverDiscoveryInstalled.load(
            std::memory_order_acquire);
    const uint64_t slInitCalls =
        gSlInitCalls.load(std::memory_order_acquire);
    const entry_detour::Snapshot slUpgradeDetour =
        entry_detour::ReadSnapshot(gSlUpgradeInterfaceEntryHandle);
    const entry_detour::Snapshot slSetDeviceDetour =
        entry_detour::ReadSnapshot(gSlSetD3DDeviceEntryHandle);
    const uint64_t slInitFlagsAfter =
        gSlInitFlagsAfter.load(std::memory_order_acquire);
    const uint64_t allowOtaMask =
        static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA);
    const uint64_t downloadedPluginsMask = static_cast<uint64_t>(
        sl::PreferenceFlags::eLoadDownloadedPlugins);
    static_assert(allowOtaMask == streamline_ota_policy::kAllowOta);
    static_assert(downloadedPluginsMask
        == streamline_ota_policy::kLoadDownloadedPlugins);
    const uint32_t safeMaximumMultiplier = SafeMaximumMultiplier();
    const bool requestedMultiplierLimited =
        !control.followGame && !control.dynamic
        && control.multiplier > safeMaximumMultiplier;
    const bool setOptionsResolverFallbackActive =
        gSetOptionsResolverFallbackActive.load(std::memory_order_acquire);
    const uint64_t setOptionsResolverFallbackCalls =
        gSetOptionsResolverFallbackCalls.load(std::memory_order_acquire);
    const bool setOptionsControlPathReady = setOptionsDetour.current
        || (setOptionsResolverFallbackActive
            && setOptionsResolverFallbackCalls > 0);
    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    // On RTX 30/20 the translated route's own provider (GitHub issue 107:
    // otherwise these fields stay 0/false there while Preset C is applied).
    const auto preset = dlssg_preset::ReadSnapshot(reinterpret_cast<HMODULE>(dlssg_preset::StatusProvider(
        gActiveNgxProviderBase.load(std::memory_order_acquire), UseAmpere(),
        reinterpret_cast<uintptr_t>(ampere_backend::PresetProvider()))));
    const bool restartRequired =
        gRestartRequired.load(std::memory_order_acquire);
    const nvidia_mfg_policy::CapacityDecision compatibilityCapacity =
        CurrentCapacityDecision();
    const uint32_t nvidiaCompatibilityTier =
        gNvidiaCompatibilityTier.load(std::memory_order_acquire);

    const ControlRouteRecord* activeControl = ActiveControlRoute();
    const ControlEntryPath activeSetterPath = activeControl
        ? static_cast<ControlEntryPath>(activeControl->activeSetterPath.load(
            std::memory_order_acquire)) : ControlEntryPath::eNone;
    const ControlEntryPath activeStatePath = activeControl
        ? static_cast<ControlEntryPath>(activeControl->activeStatePath.load(
            std::memory_order_acquire)) : ControlEntryPath::eNone;
    const std::string activeWrapperPath = JsonEscapeWide(
        activeControl ? activeControl->path : std::wstring{});
    const HMODULE activeProvider = reinterpret_cast<HMODULE>(
        gActiveNgxProviderBase.load(std::memory_order_acquire));
    const std::wstring activeProviderPathWide = activeProvider
        ? LoadedModulePath(activeProvider) : std::wstring{};
    const std::string activeProviderPath =
        JsonEscapeWide(activeProviderPathWide);
    const FileVersion activeProviderVersion =
        ReadFileVersion(activeProviderPathWide);
    const uint32_t selectionSource = gActiveNgxSelectionSource.load(
        std::memory_order_acquire);
    const char* selectionSourceName = selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eProviderEntry)
        ? "provider-entry" : selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeCaller)
        ? "runtime-caller" : selectionSource
            == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeUniqueCandidate)
        ? "runtime-unique-candidate" : selectionSource
            == static_cast<uint32_t>(NgxProviderSelectionSource::eRuntimeDispatchTable)
        ? "runtime-dispatch-table" : "none";
    const UniversalRouteFailure routeFailure =
        static_cast<UniversalRouteFailure>(gUniversalRouteFailure.load(
            std::memory_order_acquire));
    const entry_detour::Snapshot releaseDetour = activeControl
        ? entry_detour::ReadSnapshot(activeControl->freeResourcesHandle)
        : entry_detour::Snapshot{};

    char json[16384]{};
    const int length = sprintf_s(json,
        "{\"version\":" MFG_STATUS_VERSION ",\"pid\":%lu,\"processBirth\":%llu,\"heartbeat\":%llu,\"route\":\"%s\","
        "\"bridgeReady\":%s,\"liveHookInstalled\":%s,"
        "\"loaderCoreImported\":true,"
        "\"nvidiaCompatibilityResolved\":%s,"
        "\"nvidiaProfileStatus\":%d,"
        "\"nvidiaProfileName\":\"%s\","
        "\"nvidiaCompatibilityTier\":%u,"
        "\"nvidiaCompatibilityManifestEntries\":%u,"
        "\"nvidiaPolicyCeilingMultiplier\":%u,"
        "\"wrapperNativeMaximumMultiplier\":%u,"
        "\"compatibilityFallback\":%s,"
        "\"compatibilityReason\":%u,"
        "\"fullStreamlineOtaRequested\":%s,"
        "\"fullStreamlineOtaEligible\":%s,"
        "\"downloadedStreamlinePluginsForced\":%s,"
        "\"streamlineHostVersionMajor\":%u,"
        "\"streamlineHostVersionMinor\":%u,"
        "\"streamlineHostVersionBuild\":%u,"
        "\"streamlineHostVersionPrivate\":%u,"
        "\"mainResolverDiscoveryInstalled\":%s,"
        "\"slInitEntryDetourInstalled\":%s,"
        "\"slInitEntryDetourCurrent\":%s,"
        "\"slInitCachedPointersCovered\":%s,"
        "\"slInitEntryDetourFailure\":%u,"
        "\"slInitEntryRva\":%u,"
        "\"slInitIatFallbackInstalled\":%s,"
        "\"slInitResolverFallbackActive\":%s,"
        "\"slInitControlPathReady\":%s,"
        "\"slInitCalls\":%llu,"
        "\"slUpgradeInterfaceEntryDetourCurrent\":%s,"
        "\"slUpgradeInterfaceEntryDetourFailure\":%u,"
        "\"slUpgradeInterfaceEntryRva\":%u,"
        "\"slUpgradeInterfaceResolverFallbackActive\":%s,"
        "\"slUpgradeInterfaceCalls\":%llu,"
        "\"slUpgradeInterfaceWrapped\":%llu,"
        "\"slSetD3DDeviceEntryDetourCurrent\":%s,"
        "\"slSetD3DDeviceEntryDetourFailure\":%u,"
        "\"slInitEntryInstalledAtLoad\":%s,"
        "\"slSetD3DDeviceCalls\":%llu,"
        "\"slInitFlagsBefore\":%llu,\"slInitFlagsAfter\":%llu,"
        "\"otaPreferencesForced\":%s,\"otaPreferencesEnabledAtInit\":%s,"
        "\"downloadedStreamlinePluginsEnabledAtInit\":%s,"
        "\"otaProviderPreflightSupported\":%s,\"otaForceSuppressed\":%s,"
        "\"slCommonResolverDiscoveryInstalled\":%s,"
        "\"streamlineLoaderDiscoveryInstalled\":%s,"
        "\"streamlineLoaderDiscoveryCalls\":%llu,"
        "\"setOptionsEntryDetourInstalled\":%s,"
        "\"setOptionsEntryDetourCurrent\":%s,"
        "\"setOptionsCachedPointersCovered\":%s,"
        "\"setOptionsEntryDetourFailure\":%u,"
        "\"setOptionsEntryRva\":%u,"
        "\"setOptionsResolverFallbackActive\":%s,"
        "\"setOptionsResolverFallbackCalls\":%llu,"
        "\"setOptionsControlPathReady\":%s,"
        "\"getStateEntryDetourInstalled\":%s,"
        "\"getStateEntryDetourCurrent\":%s,"
        "\"getStateCachedPointersCovered\":%s,"
        "\"getStateEntryDetourFailure\":%u,"
        "\"getStateEntryRva\":%u,"
        "\"ngxCreateEntryDetourInstalled\":%s,"
        "\"ngxCreateEntryDetourCurrent\":%s,"
        "\"ngxCreateCachedPointersCovered\":%s,"
        "\"ngxCreateEntryDetourFailure\":%u,"
        "\"ngxCreateEntryRva\":%u,"
        "\"ngxEvaluateEntryDetourInstalled\":%s,"
        "\"ngxEvaluateEntryDetourCurrent\":%s,"
        "\"ngxEvaluateCachedPointersCovered\":%s,"
        "\"ngxEvaluateEntryDetourFailure\":%u,"
        "\"ngxEvaluateEntryRva\":%u,"
        "\"ngxCreateCalls\":%llu,"
        "\"ngxFrameGenerationCreateCalls\":%llu,"
        "\"ngxEvaluateCalls\":%llu,"
        "\"lastNgxCreateResultAvailable\":false,"
        "\"lastNgxCreateResult\":%d,"
        "\"frameGenerationCreateObserved\":%s,"
        "\"backportReadyAtCreate\":%s,"
        "\"pipelineMayPredateDetour\":%s,"
        "\"restartRequired\":%s,"
        "\"streamlineRebuildRequired\":%s,"
        "\"uiTagHookInstalled\":%s,"
        "\"activeWrapperObserved\":%s,\"activeWrapperPatched\":%s,"
        "\"activeWrapperUsesNvidiaOta\":%s,"
        "\"activeWrapperVersionMajor\":%u,"
        "\"activeWrapperVersionMinor\":%u,"
        "\"activeWrapperVersionBuild\":%u,"
        "\"activeWrapperVersionPrivate\":%u,"
        "\"wrapperCompiledMaximumGeneratedFrames\":%u,"
        "\"safeMaximumMultiplier\":%u,"
        "\"loadedWrapperCandidates\":%u,\"patchedWrapperCandidates\":%u,"
        "\"loadedNgxCandidates\":%u,\"patchedNgxCandidates\":%u,"
        "\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,"
        "\"dynamicExperimental56\":%s,\"generatedOnlyDebug\":%s,"
        "\"forcedMaximumMultiplier\":%u,"
        "\"dlssgPresetRequested\":%u,\"dlssgPresetOverrideInstalled\":%s,"
        "\"dlssgPresetOverrideFailure\":%u,\"dlssgPresetReadCount\":%llu,"
        "\"dlssgCachedPresetInstalled\":%s,\"dlssgCachedPresetFailure\":%u,"
        "\"dlssgCachedPresetRva\":%u,\"dlssgCachedPresetReadCount\":%llu,"
        "\"requestedMultiplierLimited\":%s,"
        "\"intervalLoggingEnabled\":%s,\"intervalLogReady\":%s,"
        "\"intervalValidSamples\":%llu,\"intervalInvalidSamples\":%llu,"
        "\"intervalDroppedSamples\":%llu,\"intervalSeenCountMask\":%u,"
        "\"intervalSeenIndexMask\":%u,\"intervalLastCount\":%d,"
        "\"intervalLastIndex\":%d,\"intervalLastPositionNumerator\":%u,"
        "\"intervalLastPositionDenominator\":%u,"
        "\"intervalLogFile\":\"%ls\","
        "\"requestRevision\":%llu,\"appliedRevision\":%llu,"
        "\"applied\":%s,\"pending\":%s,\"gameFrameGenerationOn\":%s,"
        "\"appliedFrameGenerationOn\":%s,"
        "\"appliedMode\":\"%s\",\"appliedMultiplier\":%u,"
        "\"appliedDynamicTargetFrameRate\":%.3f,"
        "\"appliedDynamicTargetValid\":%s,"
        "\"appliedDynamicExperimental56\":%s,"
        "\"appliedGeneratedOnlyDebug\":%s,\"setOptionsSeen\":%s,"
        "\"setOptionsAccepted\":%s,"
        "\"setOptionsResult\":%d,\"getStateSeen\":%s,\"getStateResult\":%d,"
        "\"actualFramesPresented\":%u,\"numFramesToGenerateMax\":%u,"
        "\"realFpsMilli\":%u,\"dlssFpsMilli\":%u,"
        "\"fpsOutputSource\":%u,"
        "\"fpsSampleWindowMs\":%u,\"fpsSampleAgeMs\":%llu,"
        "\"dlssgStatus\":%u,\"dynamicMfgSupportKnown\":%s,"
        "\"dynamicMfgSupported\":%s,"
        "\"fgVsyncSupportKnown\":%s,\"fgVsyncSupported\":%s,"
        "\"gameOptionsStructVersion\":%u,\"gameUiRecompositionEnabled\":%s,"
        "\"gameHudlessBufferFormat\":%u,\"gameUiBufferFormat\":%u,"
        "\"hudlessTagActive\":%s,\"uiAlphaTagActive\":%s,"
        "\"uiColorAlphaTagActive\":%s,\"uiDimensionsKnown\":%s,"
        "\"uiDimensionsMatch\":%s,\"uiInputsReady\":%s,"
        "\"uiRecompositionEnabled\":%s,\"uiRecompositionForced\":%s,"
        "\"hudlessWidth\":%u,\"hudlessHeight\":%u,"
        "\"uiWidth\":%u,\"uiHeight\":%u,\"uiTagFormat\":%u,"
        "\"uiTagAgeMs\":%llu,\"setTagCalls\":%llu,"
        "\"setTagForFrameCalls\":%llu,"
        "\"stateSampleAgeMs\":%llu,\"setOptionsCalls\":%llu,"
        "\"getStateCalls\":%llu,\"liveReapplyCount\":%llu,"
        "\"notInitializedRetryCount\":%llu,"
        "\"activeWrapperPath\":\"%s\","
        "\"activeWrapperGeneration\":%llu,"
        "\"activeControlPath\":\"%s\","
        "\"activeStatePath\":\"%s\","
        "\"activeControlDetour\":\"%s\","
        "\"activeStateDetour\":\"%s\","
        "\"activeProviderPath\":\"%s\","
        "\"activeProviderVersionMajor\":%u,"
        "\"activeProviderVersionMinor\":%u,"
        "\"activeProviderVersionBuild\":%u,"
        "\"activeProviderVersionPrivate\":%u,"
        "\"activeProviderGeneration\":%llu,"
        "\"providerSelectionSource\":\"%s\","
        "\"providerCreateDetour\":\"%s\","
        "\"providerEvaluateDetour\":\"%s\","
        "\"midpointReadyAtFirstCreate\":%s,"
        "\"activeLastCallRevision\":%llu,"
        "\"activeLastAcceptedRevision\":%llu,"
        "\"universalRouteFailure\":%u,"
        "\"universalRouteFailureReason\":\"%s\","
        "\"releaseEntryCurrent\":%s,"
        "\"frameGenerationOffAccepted\":%s,"
        "\"releaseObserved\":%s}\n",
        static_cast<unsigned long>(pid),
        static_cast<unsigned long long>(diagnostic_paths::ProcessBirth()),
        static_cast<unsigned long long>(UnixTimeSeconds()), route,
        bridgeReady ? "true" : "false",
        (gLiveHookInstalled.load(std::memory_order_relaxed)
            || gOriginalGetFeatureFunction.load(std::memory_order_acquire)) ? "true" : "false",
        gNvidiaCompatibilityResolved.load(
            std::memory_order_acquire) ? "true" : "false",
        gNvidiaProfileStatus.load(std::memory_order_relaxed),
        gNvidiaProfileName.c_str(),
        nvidiaCompatibilityTier,
        nvidia_mfg_policy::ManifestEntryCount(),
        compatibilityCapacity.nvidiaCeilingMultiplier,
        compatibilityCapacity.wrapperNativeMaximumMultiplier,
        compatibilityCapacity.fallback ? "true" : "false",
        static_cast<uint32_t>(compatibilityCapacity.reason),
        gFullStreamlineOtaRequested.load(
            std::memory_order_relaxed) ? "true" : "false",
        gFullStreamlineOtaEligible.load(
            std::memory_order_relaxed) ? "true" : "false",
        gDownloadedStreamlinePluginsForced.load(
            std::memory_order_relaxed) ? "true" : "false",
        gStreamlineHostVersionMajor.load(std::memory_order_relaxed),
        gStreamlineHostVersionMinor.load(std::memory_order_relaxed),
        gStreamlineHostVersionBuild.load(std::memory_order_relaxed),
        gStreamlineHostVersionPrivate.load(std::memory_order_relaxed),
        gMainResolverDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        slInitDetour.installed ? "true" : "false",
        slInitDetour.current ? "true" : "false",
        slInitDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(slInitDetour.failure),
        slInitDetour.targetRva,
        slInitIatFallback ? "true" : "false",
        slInitResolverFallback ? "true" : "false",
        slInitControlPathReady ? "true" : "false",
        static_cast<unsigned long long>(slInitCalls),
        slUpgradeDetour.current ? "true" : "false",
        static_cast<uint32_t>(slUpgradeDetour.failure),
        slUpgradeDetour.targetRva,
        gSlUpgradeInterfaceResolverFallbackActive.load(
            std::memory_order_acquire) ? "true" : "false",
        static_cast<unsigned long long>(
            gSlUpgradeInterfaceCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSlUpgradeInterfaceWrapped.load(std::memory_order_relaxed)),
        slSetDeviceDetour.current ? "true" : "false",
        static_cast<uint32_t>(slSetDeviceDetour.failure),
        gSlInitInstalledAtLoad.load(std::memory_order_acquire) ? "true" : "false",
        static_cast<unsigned long long>(
            gSlSetD3DDeviceCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSlInitFlagsBefore.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(slInitFlagsAfter),
        gOtaPreferencesForced.load(
            std::memory_order_relaxed) ? "true" : "false",
        slInitCalls > 0 && (slInitFlagsAfter & allowOtaMask) != 0
            ? "true" : "false",
        slInitCalls > 0
                && (slInitFlagsAfter & downloadedPluginsMask) != 0
            ? "true" : "false",
        gOtaProviderPreflightSupported.load(
            std::memory_order_relaxed) ? "true" : "false",
        gOtaForceSuppressed.load(
            std::memory_order_relaxed) ? "true" : "false",
        gSlCommonResolverDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        gStreamlineLoaderDiscoveryInstalled.load(
            std::memory_order_relaxed) ? "true" : "false",
        static_cast<unsigned long long>(
            gStreamlineLoaderDiscoveryCalls.load(std::memory_order_relaxed)),
        setOptionsDetour.installed ? "true" : "false",
        setOptionsDetour.current ? "true" : "false",
        setOptionsDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(setOptionsDetour.failure),
        setOptionsDetour.targetRva,
        setOptionsResolverFallbackActive ? "true" : "false",
        static_cast<unsigned long long>(setOptionsResolverFallbackCalls),
        setOptionsControlPathReady ? "true" : "false",
        getStateDetour.installed ? "true" : "false",
        getStateDetour.current ? "true" : "false",
        getStateDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(getStateDetour.failure),
        getStateDetour.targetRva,
        createDetour.installed ? "true" : "false",
        createDetour.current ? "true" : "false",
        createDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(createDetour.failure),
        createDetour.targetRva,
        evaluateDetour.installed ? "true" : "false",
        evaluateDetour.current ? "true" : "false",
        evaluateDetour.cachedPointersCovered ? "true" : "false",
        static_cast<uint32_t>(evaluateDetour.failure),
        evaluateDetour.targetRva,
        static_cast<unsigned long long>(
            gNgxCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxFrameGenerationCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNgxEvaluateCalls.load(std::memory_order_relaxed)),
        gLastNgxCreateResult.load(std::memory_order_relaxed),
        gFrameGenerationCreateObserved.load(
            std::memory_order_relaxed) ? "true" : "false",
        gBackportReadyAtCreate.load(
            std::memory_order_relaxed) ? "true" : "false",
        gPipelineMayPredateDetour.load(
            std::memory_order_relaxed) ? "true" : "false",
        restartRequired ? "true" : "false",
        restartRequired ? "true" : "false",
        gUiTagHookInstalled.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperObserved.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperPatched.load(std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperUsesNvidiaOta.load(
            std::memory_order_relaxed) ? "true" : "false",
        gActiveWrapperVersionMajor.load(std::memory_order_relaxed),
        gActiveWrapperVersionMinor.load(std::memory_order_relaxed),
        gActiveWrapperVersionBuild.load(std::memory_order_relaxed),
        gActiveWrapperVersionPrivate.load(std::memory_order_relaxed),
        gWrapperCompiledMaximumGeneratedFrames.load(
            std::memory_order_relaxed),
        safeMaximumMultiplier,
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        control.followGame ? "true" : "false",
        control.followGame ? "follow"
            : control.dynamic ? "dynamic" : "fixed",
        control.multiplier,
        control.dynamicTargetFrameRate,
        control.dynamicExperimental56 ? "true" : "false",
        control.generatedOnlyDebug ? "true" : "false",
        safeMaximumMultiplier,
        control.dlssgPreset, preset.installed ? "true" : "false", preset.failure,
        static_cast<unsigned long long>(preset.providerReads),
        preset.cachedReaderInstalled ? "true" : "false", preset.cachedReaderFailure,
        preset.cachedReaderRva, static_cast<unsigned long long>(preset.cachedReaderReads),
        requestedMultiplierLimited ? "true" : "false",
        intervalTrace.enabled ? "true" : "false",
        intervalTrace.logReady ? "true" : "false",
        static_cast<unsigned long long>(intervalTrace.validSamples),
        static_cast<unsigned long long>(intervalTrace.invalidSamples),
        static_cast<unsigned long long>(intervalTrace.droppedSamples),
        intervalTrace.seenCountMask, intervalTrace.seenIndexMask,
        intervalTrace.lastCount, intervalTrace.lastIndex,
        intervalTrace.lastPositionNumerator,
        intervalTrace.lastPositionDenominator,
        temporal_interval_trace::FileName(),
        static_cast<unsigned long long>(desiredRevision),
        static_cast<unsigned long long>(appliedRevision),
        applied ? "true" : "false", pending ? "true" : "false",
        gameFrameGenerationOn ? "true" : "false",
        appliedFrameGenerationOn ? "true" : "false",
        gAppliedDynamicMode.load(std::memory_order_relaxed) ? "dynamic" : "fixed",
        gAppliedMultiplier.load(std::memory_order_relaxed),
        static_cast<double>(gAppliedDynamicTargetFrameRate.load(std::memory_order_relaxed)),
        gAppliedDynamicTargetValid.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedDynamicExperimental56.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedGeneratedOnlyDebug.load(std::memory_order_relaxed) ? "true" : "false",
        setOptionsSeen ? "true" : "false",
        setOptionsAccepted ? "true" : "false", setOptionsResult,
        getStateSeen ? "true" : "false", getStateResult,
        gActualFramesPresented.load(std::memory_order_relaxed),
        gNumFramesToGenerateMax.load(std::memory_order_relaxed),
        gRealFpsMilli.load(std::memory_order_relaxed),
        gDlssFpsMilli.load(std::memory_order_relaxed),
        gFpsOutputSource.load(std::memory_order_relaxed),
        gFpsSampleWindowMs.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(fpsAgeMs),
        gDlssgStatus.load(std::memory_order_relaxed),
        DynamicMfgCapabilityKnown() ? "true" : "false",
        // NVIDIA's Dynamic MFG is D3D12-only and the options path never applies
        // it on Vulkan; the menu must not offer it there on any GPU.
        (UseAmpere() ? DynamicMfgSupported() : gDynamicMfgSupported.load(std::memory_order_relaxed)
            && gActiveNgxGraphicsApi.load(std::memory_order_acquire)
                != static_cast<uint32_t>(NgxGraphicsApi::eVulkan)) ? "true" : "false",
        gFgVsyncSupportKnown.load(std::memory_order_relaxed) ? "true" : "false",
        gFgVsyncSupported.load(std::memory_order_relaxed) ? "true" : "false",
        gGameOptionsStructVersion.load(std::memory_order_relaxed),
        gGameUiRecompositionEnabled.load(std::memory_order_relaxed) ? "true" : "false",
        gGameHudlessBufferFormat.load(std::memory_order_relaxed),
        gGameUiBufferFormat.load(std::memory_order_relaxed),
        uiInputs.hudless ? "true" : "false",
        uiInputs.uiAlpha ? "true" : "false",
        uiInputs.uiColorAlpha ? "true" : "false",
        uiInputs.dimensionsKnown ? "true" : "false",
        uiInputs.dimensionsMatch ? "true" : "false",
        uiInputs.ready ? "true" : "false",
        gAppliedUiRecompositionEnabled.load(std::memory_order_relaxed) ? "true" : "false",
        gAppliedUiRecompositionForced.load(std::memory_order_relaxed) ? "true" : "false",
        uiInputs.hudlessWidth, uiInputs.hudlessHeight,
        uiInputs.uiWidth, uiInputs.uiHeight, uiInputs.uiFormat,
        static_cast<unsigned long long>(uiInputs.oldestAgeMs),
        static_cast<unsigned long long>(gSetTagCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gSetTagForFrameCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(stateAgeMs),
        static_cast<unsigned long long>(gSetOptionsCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gGetStateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLiveReapplyCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            gNotInitializedRetryCount.load(std::memory_order_relaxed)),
        activeWrapperPath.c_str(),
        static_cast<unsigned long long>(
            activeControl ? activeControl->generation : 0),
        ControlPathName(activeSetterPath),
        ControlPathName(activeStatePath),
        ControlDetourMethod(activeControl, true),
        ControlDetourMethod(activeControl, false),
        activeProviderPath.c_str(),
        activeProviderVersion.major, activeProviderVersion.minor,
        activeProviderVersion.build, activeProviderVersion.privatePart,
        static_cast<unsigned long long>(
            gActiveNgxProviderGeneration.load(std::memory_order_acquire)),
        selectionSourceName,
        activeProvider ? entry_detour::MethodName(createDetour.method)
                       : "none",
        activeProvider ? entry_detour::MethodName(evaluateDetour.method)
                       : "none",
        gFirstCreateMidpointReady.load(
            std::memory_order_acquire) ? "true" : "false",
        static_cast<unsigned long long>(activeControl
            ? activeControl->lastCallRevision.load(
                std::memory_order_acquire) : 0),
        static_cast<unsigned long long>(activeControl
            ? activeControl->lastAcceptedRevision.load(
                std::memory_order_acquire) : 0),
        static_cast<uint32_t>(routeFailure),
        universal_route_policy::FailureName(routeFailure),
        releaseDetour.current ? "true" : "false",
        activeControl && activeControl->frameGenerationOffAccepted.load(
            std::memory_order_acquire) ? "true" : "false",
        activeControl && activeControl->releaseObserved.load(
            std::memory_order_acquire) ? "true" : "false");
    if (length <= 0)
        return false;

    // ReShade reads concurrently. Publish a complete replacement so a reader
    // sees the previous snapshot until the new one is ready, never a truncated
    // or partially written status file.
    std::string serialized(json, static_cast<size_t>(length));
    const size_t gpuClosing = serialized.rfind('}');
    if (gpuClosing == std::string::npos) return false;
    // graphicsApi: the FG pipeline's NGX API once committed, otherwise the API
    // whose device selected RTX 30 (1 D3D12, 2 Vulkan, 0 unknown).
    uint32_t graphicsApi = gActiveNgxGraphicsApi.load(std::memory_order_acquire);
    if (!graphicsApi && UseAmpere())
        graphicsApi = static_cast<uint32_t>(ampere_backend::DeviceApi());
    else if (!graphicsApi && gVulkanAdapterVerified.load(std::memory_order_acquire))
        graphicsApi = static_cast<uint32_t>(NgxGraphicsApi::eVulkan);
    char gpu[320]{};
    sprintf_s(gpu, ",\"product\":\"RTXMFG\",\"gpuFamily\":%u,\"gpuAdapterLuid\":%llu,\"gpuSelectionFailure\":%u,"
        "\"gpuVendorId\":%u,\"graphicsApi\":%u",
        static_cast<uint32_t>(gpu_dispatch::Selected()),
        static_cast<unsigned long long>(gpu_dispatch::AdapterLuid()), gpu_dispatch::FailureCode(),
        gpu_dispatch::AdapterVendor(), graphicsApi);
    serialized.insert(gpuClosing, gpu);
    char presetStatus[1152]{};
    const auto liveness = ampere_backend::ReadFrameGenerationLiveness();
    sprintf_s(presetStatus, ",\"dlssgPresetLatched\":%u,"
        "\"dlssgPresetSelectionFrozen\":%s,\"dlssgPresetRestartRequired\":%s,"
        "\"dlssgPresetObserved\":%u,\"dlssgPresetObservedValid\":%s,"
        "\"dlssgPresetOverrideReadCount\":%llu,\"dlssgPresetCAvailable\":%s,"
        "\"dlssgPresetLiveSwitch\":%u,\"dlssgPresetSwitching\":%s,\"fgLifetimeExact\":%s,\"fgLifetimeVulkan\":%s,"
        "\"fgLifetimeLive\":%u,\"fgLifetimeCreates\":%llu,\"fgLifetimeReleases\":%llu",
        preset.latched, preset.selectionFrozen ? "true" : "false",
        preset.restartRequired ? "true" : "false", preset.observedPreset,
        preset.observedPresetValid ? "true" : "false",
        static_cast<unsigned long long>(preset.overrideReads),
        preset.presetCAvailable ? "true" : "false",
        static_cast<uint32_t>(PresetSwitchUnavailableReason(preset.requested)),
        gPresetSwitchPhase.load(std::memory_order_acquire)
            == static_cast<uint32_t>(dlssg_preset_switch::Phase::eDraining) ? "true" : "false",
        liveness.exact ? "true" : "false",
        // The runtime Vulkan route's Create, Evaluate and Release
        // are hooked; the tracked FG features (D3D12 and Vulkan).
        ampere_backend::VulkanLifetimeCovered() ? "true" : "false", liveness.liveFeatures,
        static_cast<unsigned long long>(liveness.trackedCreates),
        static_cast<unsigned long long>(liveness.trackedReleases));
    serialized.insert(serialized.rfind('}'), presetStatus);
    serialized.insert(serialized.rfind('}'), DlssSrStatusFragment());
    {
        const auto nr=dlss5::Read();char fragment[1280]{};
        // dx11nr.1: dlss5D3D11 (the DirectX 11 bridge carries DLSS 5) and dlss5D3D11Refusal (why it passes through).
        sprintf_s(fragment,",\"dlss5State\":%u,\"dlss5Result\":%u,\"dlss5Route\":%u,\"dlss5Stage\":%u,\"dlss5Detected\":%s,\"dlss5Pending\":%s,\"dlss5Width\":%u,\"dlss5Height\":%u,\"dlss5ExtentWidth\":%u,\"dlss5ExtentHeight\":%u,\"dlss5Passes\":%u,\"dlss5Live\":%u,\"dlss5Creates\":%llu,\"dlss5Evaluations\":%llu,\"dlss5Failures\":%llu,\"dlss5EffectiveScale\":%.1f,\"dlss5GpuMs\":%.3f,\"dlss5BudgetMs\":%.3f,\"dlss5ExposureGain\":%.3f,\"dlss5FrameMs\":%.3f,\"dlss5BoostNetworkMs\":%.3f,\"dlss5ScaleFloorHeld\":%s,\"dlss5D3D11\":%s,\"dlss5D3D11Refusal\":%u",
            static_cast<uint32_t>(nr.state),nr.result,nr.route,nr.stage,nr.providerDetected?"true":"false",nr.pending?"true":"false",
            nr.width,nr.height,nr.extentWidth,nr.extentHeight,nr.passes,nr.live,
            static_cast<unsigned long long>(nr.creates),static_cast<unsigned long long>(nr.evaluations),static_cast<unsigned long long>(nr.failures),
            static_cast<double>(nr.scale),static_cast<double>(std::isfinite(nr.gpuMs)?nr.gpuMs:-1.0f),
            static_cast<double>(nr.budgetMs),static_cast<double>(std::isfinite(nr.exposureGain)?nr.exposureGain:1.0f),
            static_cast<double>(std::isfinite(nr.frameMs)?nr.frameMs:0.0f),
            static_cast<double>(std::isfinite(nr.boostNetworkMs)?nr.boostNetworkMs:-1.0f),nr.scaleFloorHeld?"true":"false",
            nr.d3d11?"true":"false",nr.d3d11Refusal);
        serialized.insert(serialized.rfind('}'),dlss5::settings_json::StatusFields(nr.requested)+fragment);
    }
    const auto vsync = vsync_control::ReadSnapshot();
    const auto reflex = reflex_control::ReadSnapshot();
    // The last refused Reflex plug-in ("" none, "unreadable" without a version resource).
    char refusedReflex[48]{};
    if (reflex.refusedModule && reflex.refusedVersionRead)
        sprintf_s(refusedReflex, "%u.%u.%u.%u", reflex.refusedVersion[0], reflex.refusedVersion[1],
            reflex.refusedVersion[2], reflex.refusedVersion[3]);
    else if (reflex.refusedModule)
        strcpy_s(refusedReflex, "unreadable");
    char pacing[2048]{};
    sprintf_s(pacing, ",\"vsyncMode\":%u,\"reflexFrameLimitFps\":%u,"
        "\"dynamicVsyncAvailable\":%s,\"vsyncControlAvailable\":%s,"
        "\"vsyncOverrideApplied\":%s,\"vsyncPresentationObserved\":%s,"
        "\"vsyncOriginalInterval\":%u,\"vsyncSubmittedInterval\":%u,\"vsyncFailure\":%u,"
        "\"vsyncVulkanSwapchain\":%s,\"vsyncVulkanHook\":%s,"
        "\"vsyncOriginalPresentMode\":%u,\"vsyncSubmittedPresentMode\":%u,"
        "\"reflexControlAvailable\":%s,\"reflexAppliedKnown\":%s,"
        "\"reflexAppliedFrameLimitUs\":%u,\"reflexLimitPending\":%s,"
        "\"reflexRestorePending\":%s,\"reflexStatus\":%u,\"reflexLastResult\":%u,"
        "\"reflexHookMask\":%u,\"reflexModuleVersionMajor\":%u,"
        "\"reflexModuleVersionMinor\":%u,\"reflexModuleVersionPatch\":%u,"
        "\"reflexModuleGeneration\":%llu,\"reflexRefusedVersion\":\"%s\"",
        control.vsyncMode, control.reflexFrameLimitFps,
        VsyncRuntimeCurrent() && DynamicMfgSupported() ? "true" : "false",
        vsync.available ? "true" : "false", vsync.overrideApplied ? "true" : "false",
        vsync.matched ? "true" : "false", vsync.originalInterval, vsync.submittedInterval,
        static_cast<uint32_t>(vsync.failure),
        vsync.vulkanSwapchain ? "true" : "false", vsync.vulkanHook ? "true" : "false",
        vsync.originalPresentMode, vsync.submittedPresentMode,
        reflex.eligible && reflex.cachedPointersCovered && reflex.availabilityKnown
            && reflex.lowLatencyAvailable ? "true" : "false",
        reflex.appliedKnown ? "true" : "false", reflex.appliedFrameLimitUs,
        reflex.pending ? "true" : "false", reflex.restorePending ? "true" : "false",
        static_cast<uint32_t>(reflex.status), reflex.lastSetResult,
        reflex.currentHookMask, reflex.moduleVersionMajor, reflex.moduleVersionMinor,
        reflex.moduleVersionPatch, static_cast<unsigned long long>(reflex.moduleGeneration),
        refusedReflex);
    serialized.insert(serialized.rfind('}'), pacing);
    serialized.insert(serialized.rfind('}'), latency_probe::StatusFragment()); // latmon.2: the menu's Latency row
    serialized.insert(serialized.rfind('}'), XessStatusFragment(control));
    serialized.insert(serialized.rfind('}'), OptiScalerStatusFragment());
    // Flat fields only (ampere_status_fields.h): nested objects made the menu
    // reject every RTX 30 status snapshot.
    if (const auto declaration = ampere_provider_arch::Read(); declaration.armed)
        serialized.insert(serialized.rfind('}'), ampere_status_fields::Declaration(declaration));
    if (UseAmpere())
    {
        // The filter runs inside the wrapper's startup; report it from here.
        static std::atomic<bool> flipMeteringReported{false};
        if (gFlipMeteringHidden.load(std::memory_order_relaxed)
            && !flipMeteringReported.exchange(true, std::memory_order_acq_rel))
            Log(L"Ampere flip metering withheld from the DLSS-G wrapper: dispatchers=%llu hidden=%llu; "
                L"CPU-paced presentation keeps one output per generated frame",
                static_cast<unsigned long long>(gWrapperNvapiFilters.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gFlipMeteringHidden.load(std::memory_order_relaxed)));
        static std::atomic<bool> flipMeteringForwardedReported{false};
        if (gFlipMeteringForwarded.load(std::memory_order_relaxed)
            && !flipMeteringForwardedReported.exchange(true, std::memory_order_acq_rel))
            Log(L"Ampere flip metering forwarded to the DLSS-G wrapper: dispatchers=%llu forwarded=%llu "
                L"observing=%d; repeated outputs are admitted only while SetFlipConfig reports metering",
                static_cast<unsigned long long>(gWrapperNvapiFilters.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gFlipMeteringForwarded.load(std::memory_order_relaxed)),
                static_cast<int>(ampere_flip_metering::Observing()));
        const size_t closing = serialized.rfind('}');
        if (closing == std::string::npos) return false;
        const auto diagnostic = ampere_backend::Diagnostics();
        char ampere[2048]{};
        sprintf_s(ampere, ",\"ampereProgramReadyMfg\":%s,"
            "\"ampereFailure\":%u,\"presetQueries\":%llu,"
            "\"ampereEvaluations\":%llu,\"ampereRejections\":%llu,"
            "\"ampereNativeMaximum\":%u,\"ampereLastCreateCount\":%d,"
            "\"ampereNativeMaximumReadResult\":%u,\"ampereStartupMaximum\":%u,\"amperePresentationBuffers\":%u,"
            "\"ampereCreatedFeatures\":%llu,\"ampereSubmittedBatches\":%llu,"
            "\"ampereFailedBatches\":%llu,\"ampereRecoveredBatches\":%llu,"
            "\"ampereOutputReuseLatches\":%llu,\"ampereFlipMeteringFilterInstalled\":%s,"
            "\"ampereFlipMeteringResolverRouted\":%s,"
            "\"ampereWrapperNvapiFilters\":%llu,\"ampereFlipMeteringHidden\":%llu,"
            "\"ampereKernelImage\":%u,\"ampereNativeCacheStatus\":%u,\"ampereCertifiedMaximum\":%u,"
            "\"ampereFirstFailure\":%u,"
            "\"ampereFirstNgxResult\":%u,"
            "\"amperePrimaryFailure\":%u,"
            "\"amperePrimaryNgxResult\":%u,"
            "\"ampereLastFailure\":%u,"
            "\"ampereLastNgxResult\":%u,"
            "\"ampereCreateAttempts\":%llu,"
            "\"ampereCreateBlockedBeforeProvider\":%llu,"
            "\"ampereEvaluateAttempts\":%llu,"
            "\"amperePreparationStage\":%u,\"ampereStartupFailureMask\":%u,"
            "\"ampereAdmissionUnavailable\":%s,\"ampereAdmissionHeldRequests\":%llu,"
            "\"ampereCandidateVersionMajor\":%u,"
            "\"ampereCandidateVersionMinor\":%u,"
            "\"ampereCandidateVersionBuild\":%u,\"ampereLegacySinglePreset\":%s",
            ampere_backend::Ready() ? "true" : "false", ampere_backend::FailureCode(),
            static_cast<unsigned long long>(ampere_backend::PresetQueries()),
            static_cast<unsigned long long>(ampere_backend::Evaluations()),
            static_cast<unsigned long long>(ampere_backend::Rejections()),
            ampere_backend::NativeMaximumGeneratedFrames(), ampere_backend::LastCreateCount(),
            ampere_backend::NativeMaximumReadResult(), ampere_backend::StartupMaximumGeneratedFrames(), ampere_backend::PresentationBuffers(),
            static_cast<unsigned long long>(ampere_backend::CreatedFeatures()),
            static_cast<unsigned long long>(ampere_backend::SubmittedBatches()),
            static_cast<unsigned long long>(ampere_backend::FailedBatches()),
            static_cast<unsigned long long>(ampere_backend::RecoveredBatches()),
            static_cast<unsigned long long>(ampere_backend::OutputReuseLatches()),
            gWrapperNvapiFilterInstalled.load(std::memory_order_acquire) ? "true" : "false",
            gAmpereWrapperResolverRouted.load(std::memory_order_acquire) ? "true" : "false",
            static_cast<unsigned long long>(gWrapperNvapiFilters.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(gFlipMeteringHidden.load(std::memory_order_relaxed)),
            ampere_gpu::KernelImage(), ampere_gpu::NativeCacheStatus(),
            ampere_backend::CertifiedMaximumGeneratedFrames(),
            diagnostic.first.code,
            diagnostic.first.ngxResult,
            diagnostic.primary.code,
            diagnostic.primary.ngxResult,
            diagnostic.last.code,
            diagnostic.last.ngxResult,
            static_cast<unsigned long long>(diagnostic.createAttempts),
            static_cast<unsigned long long>(diagnostic.createBlockedBeforeProvider),
            static_cast<unsigned long long>(diagnostic.evaluateAttempts),
            diagnostic.preparationStage, diagnostic.startupFailureMask,
            ampere_backend::AdmissionUnavailable() ? "true" : "false",
            static_cast<unsigned long long>(gAmpereAdmissionHeldRequests.load(std::memory_order_relaxed)),
            static_cast<uint32_t>(diagnostic.candidateVersion >> 32),
            static_cast<uint32_t>((diagnostic.candidateVersion >> 16) & 0xffffu),
            static_cast<uint32_t>(diagnostic.candidateVersion & 0xffffu),
            ampere_backend::LegacySinglePreset() ? "true" : "false");
        serialized.insert(closing, ampere);
    }
    if (const auto selection = dlssg_provider_selection::Read(); selection.prepared)
        serialized.insert(serialized.rfind('}'),
            ampere_status_fields::ProviderSelection(selection, JsonEscapeWide(selection.selected.path)));
    const auto publication=status_transport::Publish(gStatusPath,serialized,disk);
    static DWORD lastPrimaryError=ERROR_SUCCESS,lastFallbackError=ERROR_SUCCESS;
    static bool lastFallback=false;
    // The file's state changes only when it is written; memory-only publications would toggle it every heartbeat.
    if(publication.disk&&(publication.primaryError!=lastPrimaryError||publication.fallbackError!=lastFallbackError
        ||publication.fallback!=lastFallback)){
        Log(L"MFG_STATUS_TRANSPORT published=%d fallback=%d primaryError=%lu fallbackError=%lu path=%s",
            publication.published,publication.fallback,publication.primaryError,publication.fallbackError,
            (publication.fallback?status_transport::MemoryName(gStatusPath):gStatusPath).c_str());
        lastPrimaryError=publication.primaryError;lastFallbackError=publication.fallbackError;
        lastFallback=publication.fallback;
    }
    return publication.published;
}

using ChainFindStatus = universal_route_policy::StructureStatus;

struct BaseStructureFields
{
    sl::BaseStructure* next = nullptr;
    sl::StructType type{};
    size_t version = 0;
};

bool ReadBaseStructureFields(const sl::BaseStructure* source,
    BaseStructureFields& fields) noexcept
{
    if (!source)
        return false;
    __try
    {
        fields.next = source->next;
        fields.type = source->structType;
        fields.version = source->structVersion;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
ChainFindStatus FindStructBounded(const sl::BaseStructure* chain,
    const T*& result, size_t& version) noexcept
{
    constexpr size_t kMaximumChainNodes = 32;
    const sl::BaseStructure* visited[kMaximumChainNodes]{};
    result = nullptr;
    version = 0;
    for (size_t index = 0; chain && index < kMaximumChainNodes; ++index)
    {
        for (size_t prior = 0; prior < index; ++prior)
        {
            if (visited[prior] == chain)
                return ChainFindStatus::eMalformed;
        }
        visited[index] = chain;
        BaseStructureFields fields{};
        if (!ReadBaseStructureFields(chain, fields))
            return ChainFindStatus::eMalformed;
        if (fields.type == T::s_structType)
        {
            if (UseAmpere())
            {
                // Validate the tail too: the complete chain is forwarded after
                // our fixed options prefix, so a later cycle must not escape.
                if (!result)
                {
                    result = static_cast<const T*>(chain);
                    version = fields.version;
                }
            }
            else
            {
                result = static_cast<const T*>(chain);
                version = fields.version;
                return ChainFindStatus::eFound;
            }
        }
        chain = fields.next;
    }
    return chain ? ChainFindStatus::eMalformed
                 : result ? ChainFindStatus::eFound : ChainFindStatus::eNotFound;
}

bool IsSupportedOptionsVersion(size_t version) noexcept
{
    static_assert(sl::kStructVersion1 == 1);
    static_assert(sl::kStructVersion5 == 5);
    return universal_route_policy::IsSupportedOptionsVersion(version);
}

bool IsSupportedStateVersion(size_t version) noexcept
{
    static_assert(sl::kStructVersion1 == 1);
    static_assert(sl::kStructVersion4 == 4);
    return universal_route_policy::IsSupportedStateVersion(version);
}

ControlRouteRecord* ControlRouteAt(uint32_t slot) noexcept
{
    if (slot >= gControlRoutes.size()
        || !gControlRoutes[slot].claimed.load(std::memory_order_acquire))
        return nullptr;
    return &gControlRoutes[slot];
}

ControlRouteRecord* ActiveControlRoute() noexcept
{
    return ControlRouteAt(
        gActiveControlRouteSlot.load(std::memory_order_acquire));
}

const char* ControlPathName(ControlEntryPath path) noexcept
{
    switch (path)
    {
    case ControlEntryPath::ePublic: return "public";
    case ControlEntryPath::eInternal: return "internal";
    case ControlEntryPath::eResolver: return "resolver";
    default: return "none";
    }
}

bool IsAcceptedControlResult(sl::Result result) noexcept
{
    return universal_route_policy::IsAcceptedResult(
        static_cast<int32_t>(result),
        static_cast<int32_t>(sl::Result::eOk),
        static_cast<int32_t>(sl::Result::eWarnOutOfVRAM));
}

sl::Result HostControlResult(sl::Result result) noexcept
{
    // Match the adjusted-options path at every validated DLSS-G boundary,
    // including the initial native enable before BridgeReady. SL_FAILED
    // treats all nonzero results as errors, including this accepted warning.
    // Record the raw result before returning; real errors remain unchanged.
    return result == sl::Result::eWarnOutOfVRAM ? sl::Result::eOk : result;
}

void ClearAppliedDynamicTelemetry() noexcept
{
    gAppliedDynamicTargetValid.store(false, std::memory_order_release);
    gAppliedDynamicMode.store(false, std::memory_order_relaxed);
    gAppliedDynamicMaxFrames.store(0, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(0.0f, std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(false, std::memory_order_relaxed);
}

bool SameRetainedFeature(const RetainedFeatureIdentity& left,
    const RetainedFeatureIdentity& right) noexcept
{
    return left.handle == right.handle && left.runtime == right.runtime
        && left.runtimeGeneration == right.runtimeGeneration
        && left.provider == right.provider
        && left.providerGeneration == right.providerGeneration
        && left.wrapper == right.wrapper
        && left.wrapperGeneration == right.wrapperGeneration
        && left.publication == right.publication
        && left.adapterLuid == right.adapterLuid
        && left.generatedFrameCapacity == right.generatedFrameCapacity
        && left.lifetime == right.lifetime
        && left.createAttemptEpoch == right.createAttemptEpoch;
}

bool SameObservedFeature(const RetainedFeatureIdentity& feature,
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    return feature.handle == event.handle
        && feature.lifetime == event.lifetime
        && feature.runtime == event.runtime
        && feature.runtimeGeneration == event.runtimeGeneration
        && feature.provider == event.provider
        && feature.providerGeneration == event.providerGeneration
        && feature.wrapper == event.wrapper
        && feature.wrapperGeneration == event.wrapperGeneration
        && feature.publication == event.publication
        && feature.adapterLuid == event.adapterLuid
        && feature.generatedFrameCapacity == event.generatedFrameCapacity
        && feature.createAttemptEpoch == event.createAttemptToken;
}

bool AmpereFeatureEntriesCurrent(uintptr_t runtimeOwner, uint64_t runtimeGeneration,
    uintptr_t providerOwner, uint64_t providerGeneration) noexcept
{
    if (!runtimeOwner || !runtimeGeneration || !providerOwner || !providerGeneration)
        return false;
    if (gActiveNgxGraphicsApi.load(std::memory_order_acquire)
        == static_cast<uint32_t>(NgxGraphicsApi::eVulkan))
    {
        // RTX 30 Vulkan: Streamline enters Create1, or Create for older
        // providers. Each module keeps one current entry per Create kind.
        using entry_detour::Kind;
        const auto entry = [](Kind kind, uintptr_t owner, uint64_t generation) noexcept {
            const auto state = entry_detour::ReadSnapshot(kind, reinterpret_cast<HMODULE>(owner));
            return state.current && state.currentEntries == 1 && state.generation == generation
                ? state : entry_detour::Snapshot{};
        };
        const auto runtime1 = entry(Kind::eNgxRuntimeVulkanCreateFeature1, runtimeOwner, runtimeGeneration);
        const auto runtime0 = entry(Kind::eNgxRuntimeVulkanCreateFeature, runtimeOwner, runtimeGeneration);
        const auto provider1 = entry(Kind::eNgxVulkanCreateFeature1, providerOwner, providerGeneration);
        const auto provider0 = entry(Kind::eNgxVulkanCreateFeature, providerOwner, providerGeneration);
        if ((!runtime1.current && !runtime0.current) || (!provider1.current && !provider0.current))
            return false;
        const auto selected = entry_detour::ReadSnapshot(UnpackEntryHandle(
            gActiveNgxCreateHandle.load(std::memory_order_acquire)));
        if (!selected.current) return false;
        switch (selected.kind)
        {
        case Kind::eNgxRuntimeVulkanCreateFeature1: return runtime1.current && selected.handle == runtime1.handle;
        case Kind::eNgxRuntimeVulkanCreateFeature: return runtime0.current && selected.handle == runtime0.handle;
        case Kind::eNgxVulkanCreateFeature1: return provider1.current && selected.handle == provider1.handle;
        case Kind::eNgxVulkanCreateFeature: return provider0.current && selected.handle == provider0.handle;
        default: return false;
        }
    }
    const auto runtime = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxRuntimeD3D12CreateFeature,
        reinterpret_cast<HMODULE>(runtimeOwner));
    const auto provider = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxD3D12CreateFeature,
        reinterpret_cast<HMODULE>(providerOwner));
    if (!runtime.current || runtime.currentEntries != 1 || runtime.generation != runtimeGeneration
        || !provider.current || provider.currentEntries != 1 || provider.generation != providerGeneration)
        return false;
    const auto selected = entry_detour::ReadSnapshot(UnpackEntryHandle(
        gActiveNgxCreateHandle.load(std::memory_order_acquire)));
    if (!selected.current) return false;
    // A runtime may select its provider before forwarding, or the nested
    // concrete provider may select the route. Both entries remain required;
    // the selected kind determines which exact immutable handle must match.
    if (selected.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature)
        return selected.handle == runtime.handle;
    if (selected.kind == entry_detour::Kind::eNgxD3D12CreateFeature)
        return selected.handle == provider.handle;
    return false;
}

bool RetainedFeatureMatchesRuntime(const RetainedFeatureIdentity& feature,
    const ControlRouteRecord& route) noexcept
{
    if (!feature || feature.wrapper != reinterpret_cast<uintptr_t>(route.wrapper)
        || feature.wrapperGeneration != route.generation
        || feature.provider
            != gActiveNgxProviderBase.load(std::memory_order_acquire)
        || feature.providerGeneration
            != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
        || feature.createAttemptEpoch
            != gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire)
        || !gFrameGenerationCreateObserved.load(std::memory_order_acquire))
        return false;
    if (!AmpereFeatureEntriesCurrent(feature.runtime, feature.runtimeGeneration,
            feature.provider, feature.providerGeneration))
        return false;
    if (!UseAmpere())
        return false;
    return feature.publication == ampere_gpu::Publication()
        && feature.adapterLuid == ampere_gpu::AdapterLuid();
}

uint32_t ControlRouteSlot(const ControlRouteRecord& route) noexcept
{
    const ptrdiff_t slot = &route - gControlRoutes.data();
    return slot >= 0 && static_cast<size_t>(slot) < gControlRoutes.size()
        ? static_cast<uint32_t>(slot) : UINT32_MAX;
}

void RecordGameFrameGenerationIntent(const sl::ViewportHandle& viewport,
    bool enabled) noexcept
{
    gGameFrameGenerationViewport.store(
        static_cast<uint32_t>(viewport), std::memory_order_release);
    gGameFrameGenerationOn.store(enabled, std::memory_order_release);
    // The XeMFG Vulkan host stays off while the game's own DLSS Frame Generation is on (harness);
    // under XeMFG standalone that switch turns XeMFG on and off instead, on both hosts.
    xess_vulkan::SetGameFrameGeneration(enabled);
    xess_d3d12::GameFrameGeneration(enabled);
}

bool ControlRequestsEnabled(const ControlSnapshot& snapshot,
    const sl::DLSSGOptions& source) noexcept
{
    if (!gGameFrameGenerationOn.load(std::memory_order_acquire)
        || source.mode == sl::DLSSGMode::eOff)
        return false;
    if (!UseAmpere())
        return true;
    return snapshot.control.followGame
        || snapshot.control.multiplier > 1;
}

// RTX 30 options from the control and the game's own request; see
// ampere_policy::DecideOptions.
ampere_policy::OptionsDecision AmpereOptions(
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot) noexcept
{
    const auto gameMode = source.mode == sl::DLSSGMode::eOff ? ampere_policy::OptionsMode::eOff
        : source.mode == sl::DLSSGMode::eDynamic ? ampere_policy::OptionsMode::eDynamic
        : ampere_policy::OptionsMode::eOn;
    return ampere_policy::DecideOptions(snapshot.control.followGame,
        snapshot.control.multiplier, snapshot.control.dynamic, gameMode,
        source.numFramesToGenerate, SafeMaximumMultiplier(), DynamicMfgSupported());
}

bool CanReenableRetainedFeatureLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, const ControlSnapshot& snapshot,
    const sl::DLSSGOptions& source,
    RetainedFeatureIdentity* expectedLiveFeature = nullptr) noexcept
{
    if (expectedLiveFeature)
        *expectedLiveFeature = {};
    if (!ControlRequestsEnabled(snapshot, source))
        return true;
    // Ada retains its existing Streamline control contract.  The opaque
    // handle/capacity observer below belongs only to the experimental Ampere
    // backend; applying it to Ada would reject every live On revision.
    // rtx30ngx.1: nor does it apply while a replacement provider owns frame
    // generation on RTX 30/20 (no translated feature is retained).
    if (!AmpereOptionsApply())
        return true;
    const uint32_t routeSlot = ControlRouteSlot(route);
    if (routeSlot == UINT32_MAX
        || !RetainedFeatureMatchesRuntime(
            gRetainedFrameGenerationFeature, route))
        return false;
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        const bool acceptedOff = gAcceptedOffEvidence.routeSlot == routeSlot
            && gAcceptedOffEvidence.viewport
                == static_cast<uint32_t>(viewport)
            && SameRetainedFeature(gAcceptedOffEvidence.feature,
                gRetainedFrameGenerationFeature);
        const bool freshRecreation = SameRetainedFeature(
            gFreshRecreatedFrameGenerationFeature,
            gRetainedFrameGenerationFeature);
        if (!acceptedOff && !freshRecreation)
            return false;
    }
    const uint32_t requiredMultiplier =
        AmpereOptions(source, snapshot).generatedFrames + 1u;
    if (gRetainedFrameGenerationFeature.generatedFrameCapacity + 1u
        < requiredMultiplier)
        return false;
    if (expectedLiveFeature)
        *expectedLiveFeature = gRetainedFrameGenerationFeature;
    return true;
}

bool RetainedFeatureStillExactForLiveOnLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const RetainedFeatureIdentity& expected) noexcept
{
    const uint32_t routeSlot = ControlRouteSlot(route);
    if (!expected || routeSlot == UINT32_MAX
        || !SameRetainedFeature(expected, gRetainedFrameGenerationFeature)
        || !RetainedFeatureMatchesRuntime(
            gRetainedFrameGenerationFeature, route))
        return false;
    if (gAppliedFrameGenerationOn.load(std::memory_order_acquire))
        return true;
    const bool acceptedOff = gAcceptedOffEvidence.routeSlot == routeSlot
        && gAcceptedOffEvidence.viewport == static_cast<uint32_t>(viewport)
        && SameRetainedFeature(
            expected, gAcceptedOffEvidence.feature);
    return acceptedOff || SameRetainedFeature(
        expected, gFreshRecreatedFrameGenerationFeature);
}

void RecordAcceptedSetOptionsLifecycleLocked(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, bool enabled) noexcept
{
    route.lastAcceptedRevision.store(
        gDesiredRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    if (gAppliedFrameGenerationOn.exchange(
            enabled, std::memory_order_acq_rel) != enabled)
        gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    // xemfglive.1: a chain XeMFG took live waits for DLSS-G applied Off before it generates.
    xess_d3d12::DlssgApplied(enabled);
    route.frameGenerationOffAccepted.store(!enabled,
        std::memory_order_release);
    if (enabled)
    {
        route.releaseObserved.store(false, std::memory_order_release);
        gAcceptedOffEvidence = {};
        gFreshRecreatedFrameGenerationFeature = {};
    }
    else
    {
        InvalidateUiInputEvidence(static_cast<uint32_t>(viewport));
        gFreshRecreatedFrameGenerationFeature = {};
        if (!UseAmpere())
        {
            // Ada has no opaque NGX-feature observer. Preserve only the exact
            // accepted route/viewport Off boundary: live Off -> On remains
            // supported until a matching slFreeResources succeeds, at which
            // point lifetime ownership is unprovable and restart is required.
            const uint32_t routeSlot = ControlRouteSlot(route);
            gAcceptedOffEvidence = routeSlot == UINT32_MAX
                ? AcceptedOffEvidence{}
                : AcceptedOffEvidence{{}, routeSlot,
                    static_cast<uint32_t>(viewport),
                    gDesiredRevision.load(std::memory_order_acquire)};
        }
        else
        {
            const uint32_t routeSlot = ControlRouteSlot(route);
            if (routeSlot != UINT32_MAX
                && RetainedFeatureMatchesRuntime(
                    gRetainedFrameGenerationFeature, route))
            {
                gAcceptedOffEvidence = {gRetainedFrameGenerationFeature,
                    routeSlot, static_cast<uint32_t>(viewport),
                    gDesiredRevision.load(std::memory_order_acquire)};
            }
            else
            {
                // Off was accepted, but no exact retained feature is available
                // for a later live Ampere On. Preserve the stopped state and
                // require a known recreation boundary instead of guessing from
                // route-wide flags.
                gAcceptedOffEvidence = {};
                gRestartRequired.store(true, std::memory_order_release);
            }
        }
        ClearAppliedDynamicTelemetry();
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
    }
}

void RecordSetOptionsLifecycle(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, bool enabled, sl::Result result) noexcept
{
    if (!IsAcceptedControlResult(result))
        return;
    std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
    RecordAcceptedSetOptionsLifecycleLocked(route, viewport, enabled);
}

void ResetReleasedPipelineStateLocked(ControlRouteRecord& route) noexcept
{
    InvalidateUiInputEvidence();
    gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
    gAppliedUiRecompositionForced.store(false, std::memory_order_release);
    gRetainedFrameGenerationFeature = {};
    gFreshRecreatedFrameGenerationFeature = {};
    gAcceptedOffEvidence = {};
    gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    gFgVsyncSupportKnown.store(false, std::memory_order_release);
    gFgVsyncSupported.store(false, std::memory_order_release);
    gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
    gDynamicMfgSupported.store(false, std::memory_order_release);
    ClearAppliedDynamicTelemetry();
    route.frameGenerationOffAccepted.store(false,
        std::memory_order_release);
    route.releaseObserved.store(true, std::memory_order_release);
    gAppliedFrameGenerationOn.store(false, std::memory_order_release);
    xess_d3d12::DlssgApplied(false);   // xemfglive.1
    // Applied/attempted revisions are scoped to the released native feature.
    // A later exact Create must receive the current request even when the
    // config revision itself did not change across recreation.
    gAppliedRevision.store(0, std::memory_order_release);
    gAttemptedRevision.store(0, std::memory_order_release);
    gFrameGenerationCreateObserved.store(false, std::memory_order_release);
    gFirstCreateMidpointReady.store(false, std::memory_order_release);
    gBackportReadyAtCreate.store(false, std::memory_order_release);
    gPipelineMayPredateDetour.store(false, std::memory_order_release);
    gRestartRequired.store(false, std::memory_order_release);
    gProviderChangedAfterCreate.store(false, std::memory_order_release);
    gActiveNgxDispatchRoute.store(
        static_cast<uint32_t>(NgxDispatchRoute::ePending),
        std::memory_order_release);
    gActiveNgxGraphicsApi.store(
        static_cast<uint32_t>(NgxGraphicsApi::eUnknown),
        std::memory_order_release);
    gActiveNgxCreateHandle.store(0, std::memory_order_release);
    gActiveNgxEvaluateHandle.store(0, std::memory_order_release);
    gActiveNgxProviderBase.store(0, std::memory_order_release);
    gActiveNgxProviderGeneration.store(0, std::memory_order_release);
    gActiveNgxSelectionSource.store(0, std::memory_order_release);
    SetUniversalRouteFailure(UniversalRouteFailure::eProviderNotSelected);
    Log(L"Active FG pipeline released after accepted Off; the next On/Create "
        L"will select one coherent wrapper/provider route");
}

ControlRouteRecord* RouteForObservedFeature(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    for (auto& route : gControlRoutes)
    {
        if (route.claimed.load(std::memory_order_acquire)
            && reinterpret_cast<uintptr_t>(route.wrapper) == event.wrapper
            && route.generation == event.wrapperGeneration)
            return &route;
    }
    return nullptr;
}

void ObserveAmpereFeatureLifetime(
    const ampere_backend::FeatureLifetimeEvent& event) noexcept
{
    if (!UseAmpere() || !event.handle || !event.lifetime || !event.runtime
        || !event.runtimeGeneration || !event.provider
        || !event.providerGeneration || !event.wrapper
        || !event.wrapperGeneration || !event.publication
        || !event.generatedFrameCapacity || !event.createAttemptToken)
        return;

    ControlRouteRecord* route = RouteForObservedFeature(event);
    if (!route)
    {
        gRestartRequired.store(true, std::memory_order_release);
        return;
    }

    std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
    if (event.phase == ampere_backend::FeatureLifetimePhase::eCreated)
    {
        // Older queued completions cannot adopt a newer in-flight operation's
        // epoch even when NGX reuses the same numeric handle and modules.
        if (event.createAttemptToken != gFrameGenerationCreateAttemptEpoch.load(
                std::memory_order_acquire))
            return;
        if (!AmpereFeatureEntriesCurrent(event.runtime, event.runtimeGeneration,
                event.provider, event.providerGeneration)
            || event.provider
                != gActiveNgxProviderBase.load(std::memory_order_acquire)
            || event.providerGeneration
                != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
            || event.publication != ampere_gpu::Publication()
            || event.adapterLuid != ampere_gpu::AdapterLuid())
        {
            gRestartRequired.store(true, std::memory_order_release);
            return;
        }

        // The backend snapshots mutations under its lifecycle lock and drains
        // them in strict FIFO order after releasing it. Keep the monotonic
        // lifetime comparison as a defensive identity gate for delayed or
        // reentrant observer delivery and numeric handle reuse.
        if (gRetainedFrameGenerationFeature
            && gRetainedFrameGenerationFeature.runtime == event.runtime
            && gRetainedFrameGenerationFeature.runtimeGeneration
                == event.runtimeGeneration
            && gRetainedFrameGenerationFeature.lifetime > event.lifetime)
            return;

        const bool replacesAcceptedOff = gRetainedFrameGenerationFeature
            && SameRetainedFeature(gAcceptedOffEvidence.feature,
                gRetainedFrameGenerationFeature);
        const bool followsExactRelease = route->releaseObserved.load(
            std::memory_order_acquire);
        gRetainedFrameGenerationFeature = {
            event.handle, event.runtime, event.runtimeGeneration,
            event.provider, event.providerGeneration,
            event.wrapper, event.wrapperGeneration,
            event.publication, event.adapterLuid,
            event.generatedFrameCapacity, event.lifetime,
            event.createAttemptToken};
        gFreshRecreatedFrameGenerationFeature = followsExactRelease
            ? gRetainedFrameGenerationFeature : RetainedFeatureIdentity{};
        gAcceptedOffEvidence = {};
        route->releaseObserved.store(false, std::memory_order_release);
        gFrameGenerationCreateObserved.store(true, std::memory_order_release);
        if ((!gRestartRequired.load(std::memory_order_acquire)
                || replacesAcceptedOff || followsExactRelease)
            && !gProviderChangedAfterCreate.load(std::memory_order_acquire))
            gRestartRequired.store(false, std::memory_order_release);
        return;
    }

    if (event.phase != ampere_backend::FeatureLifetimePhase::eReleased
        || !SameObservedFeature(gRetainedFrameGenerationFeature, event))
        return;

    if (gRetainedFrameGenerationFeature.createAttemptEpoch
        != gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire))
    {
        // A newer Create has begun after this exact release mutation but before
        // observer delivery. Retire only the old feature-scoped state: clearing
        // the active dispatch/provider selection here would clobber the newer
        // in-flight pre-Create publication. FIFO delivery lets its successful
        // Created event consume releaseObserved and certify a fresh lifetime;
        // if it fails, restart remains required.
        gRetainedFrameGenerationFeature = {};
        gFreshRecreatedFrameGenerationFeature = {};
        gAcceptedOffEvidence = {};
        gPresentationLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
        gDynamicMfgSupported.store(false, std::memory_order_release);
        ClearAppliedDynamicTelemetry();
        route->frameGenerationOffAccepted.store(false,
            std::memory_order_release);
        route->releaseObserved.store(true, std::memory_order_release);
        gAppliedFrameGenerationOn.store(false, std::memory_order_release);
        xess_d3d12::DlssgApplied(false);   // xemfglive.1
        gAppliedRevision.store(0, std::memory_order_release);
        gAttemptedRevision.store(0, std::memory_order_release);
        gRestartRequired.store(true, std::memory_order_release);
        return;
    }
    ResetReleasedPipelineStateLocked(*route);
}

void EnsureAmpereFeatureLifetimeObserver() noexcept
{
    // Storing the same callback is idempotent and avoids a publication window
    // between a separate once flag and the observer pointer itself.
    ampere_backend::SetFeatureLifetimeCallback(
        &ObserveAmpereFeatureLifetime);
}

void SetUniversalRouteFailure(UniversalRouteFailure failure) noexcept
{
    gUniversalRouteFailure.store(static_cast<uint32_t>(failure),
        std::memory_order_release);
}

// The caller holds gActiveControlRouteMutex through slot and metadata publication.
bool PublishActiveControlRoute(ControlRouteRecord& route)
{
    if (UseAmpere())
    {
        ampere_backend::ObserveActiveWrapper(route.wrapper, route.generation);
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(route.wrapper);
    const uintptr_t previous = gActiveWrapperBase.exchange(
        base, std::memory_order_acq_rel);
    gActiveWrapperPatched.store(route.wrapperPatched,
        std::memory_order_release);
    gActiveWrapperObserved.store(true, std::memory_order_release);
    gActiveWrapperUsesNvidiaOta.store(
        UsesNvidiaOtaCache(route.path), std::memory_order_release);
    gActiveWrapperVersionMajor.store(route.version.major,
        std::memory_order_release);
    gActiveWrapperVersionMinor.store(route.version.minor,
        std::memory_order_release);
    gActiveWrapperVersionBuild.store(route.version.build,
        std::memory_order_release);
    gActiveWrapperVersionPrivate.store(route.version.privatePart,
        std::memory_order_release);
    gWrapperCompiledMaximumGeneratedFrames.store(
        route.compiledMaximumGeneratedFrames, std::memory_order_release);
    if (previous != base)
    {
        gFgVsyncSupportKnown.store(false, std::memory_order_release);
        gFgVsyncSupported.store(false, std::memory_order_release);
        gDynamicMfgCapabilityKnown.store(false, std::memory_order_release);
        gDynamicMfgSupported.store(false, std::memory_order_release);
    }
    return previous != base;
}

bool ActivateControlRoute(uint32_t slot, ControlEntryPath path,
    bool setter) noexcept
{
    ControlRouteRecord* route = ControlRouteAt(slot);
    if (!route)
        return false;
    std::unique_lock publicationLock(gActiveControlRouteMutex);
    uint32_t active = gActiveControlRouteSlot.load(
        std::memory_order_acquire);
    if (active == UINT32_MAX)
    {
        gActiveControlRouteSlot.compare_exchange_strong(
            active, slot, std::memory_order_acq_rel,
            std::memory_order_acquire);
        active = gActiveControlRouteSlot.load(std::memory_order_acquire);
    }
    if (active != slot)
    {
        ControlRouteRecord* activeRoute = ControlRouteAt(active);
        const universal_route_policy::Identity activeIdentity{
            reinterpret_cast<uintptr_t>(
                activeRoute ? activeRoute->wrapper : nullptr),
            activeRoute ? activeRoute->generation : 0};
        const universal_route_policy::Identity candidateIdentity{
            reinterpret_cast<uintptr_t>(route->wrapper), route->generation};
        universal_route_policy::Lifecycle lifecycle{};
        lifecycle.frameGenerationOn = gAppliedFrameGenerationOn.load(
            std::memory_order_acquire);
        lifecycle.pipelineCreated = gFrameGenerationCreateObserved.load(
            std::memory_order_acquire);
        if (!universal_route_policy::CanActivateRoute(
                activeIdentity, candidateIdentity, lifecycle))
        {
            SetUniversalRouteFailure(
                UniversalRouteFailure::eWrapperChangedAfterCreate);
            gRestartRequired.store(true, std::memory_order_release);
            return false;
        }
        gActiveControlRouteSlot.store(slot, std::memory_order_release);
    }

    if (setter)
    {
        route->activeSetterPath.store(static_cast<uint32_t>(path),
            std::memory_order_release);
        route->setterCalls.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        route->activeStatePath.store(static_cast<uint32_t>(path),
            std::memory_order_release);
        route->stateCalls.fetch_add(1, std::memory_order_relaxed);
    }
    route->lastCallTick.store(GetTickCount64(), std::memory_order_release);
    route->lastCallRevision.store(
        gDesiredRevision.load(std::memory_order_acquire),
        std::memory_order_release);
    const bool changed = PublishActiveControlRoute(*route);
    publicationLock.unlock();
    if (changed)
    {
        Log(L"Active DLSS-G wrapper selected by real call: generation=%llu "
            L"patched=%d compiledMaximum=%u version=%u.%u.%u.%u path=%s",
            static_cast<unsigned long long>(route->generation),
            route->wrapperPatched.load(), route->compiledMaximumGeneratedFrames.load(),
            route->version.major, route->version.minor, route->version.build,
            route->version.privatePart, route->path.c_str());
    }
    if (!route->wrapperPatched)
    {
        SetUniversalRouteFailure(
            UniversalRouteFailure::eActiveWrapperUnpatched);
        // A real public call still establishes game intent and accepted FG
        // state. BridgeReady keeps every override disabled for this route.
        // Do not patch generic lifecycle entries merely to observe it.
        return route->structureCompatible.load(std::memory_order_acquire);
    }
    InstallControlRouteLifecycleEntry(slot);
    return route->structureCompatible.load(std::memory_order_acquire);
}

void InvalidateControlRoute(uint32_t slot,
    UniversalRouteFailure failure) noexcept
{
    if (ControlRouteRecord* route = ControlRouteAt(slot))
        route->structureCompatible.store(false, std::memory_order_release);
    if (slot == gActiveControlRouteSlot.load(std::memory_order_acquire))
        InvalidateUiInputEvidence();
    SetUniversalRouteFailure(failure);
}

bool ControlEntryCurrent(entry_detour::Handle handle) noexcept
{
    return handle && entry_detour::ReadSnapshot(handle).current;
}

template <typename Function>
Function* EntryOriginal(const std::atomic<Function*>& published,
    entry_detour::Handle handle) noexcept
{
    if (Function* original = published.load(std::memory_order_acquire))
        return original;
    return handle ? reinterpret_cast<Function*>(
        entry_detour::ReadSnapshot(handle).original) : nullptr;
}

bool ActiveSetterCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::IsCovered(
        static_cast<ControlEntryPath>(route.activeSetterPath.load(
            std::memory_order_acquire)),
        ControlEntryCurrent(route.publicSetHandle),
        ControlEntryCurrent(route.internalSetHandle),
        route.publicSetResolverFallback.load(std::memory_order_acquire)
            && route.publicSetOriginal.load(std::memory_order_acquire));
}

bool StateEntryCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::HasCoveredEntry(
        ControlEntryCurrent(route.publicGetHandle),
        ControlEntryCurrent(route.internalGetHandle),
        route.publicGetResolverFallback.load(std::memory_order_acquire)
            && route.publicGetOriginal.load(std::memory_order_acquire));
}

bool SetterEntryCovered(const ControlRouteRecord& route) noexcept
{
    return universal_route_policy::HasCoveredEntry(
        ControlEntryCurrent(route.publicSetHandle),
        ControlEntryCurrent(route.internalSetHandle),
        route.publicSetResolverFallback.load(std::memory_order_acquire)
            && route.publicSetOriginal.load(std::memory_order_acquire));
}

sl::DLSSGOptions CopyKnownOptions(const sl::DLSSGOptions& source, bool preserveNext)
{
    sl::DLSSGOptions copy{};
    copy.next = preserveNext ? source.next : nullptr;
    copy.structType = source.structType;
    copy.structVersion = source.structVersion;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= sl::kStructVersion2)
        copy.bReserved15 = source.bReserved15;
    if (source.structVersion >= sl::kStructVersion3)
        copy.queueParallelismMode = source.queueParallelismMode;
    if (source.structVersion >= sl::kStructVersion4)
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    if (source.structVersion >= sl::kStructVersion5)
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    return copy;
}

bool GameEnablesUiRecomposition(const sl::DLSSGOptions& source) noexcept
{
    return source.structVersion >= sl::kStructVersion4
        && source.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
}

// RTX 30/20 startup admission failed (or the boundary is fatal), so the Create
// gate refuses every DLSS-G feature (120) before the provider, and Streamline
// would retry that Create on every frame (a failed Create per frame, VRAM
// growing to 41 GB, then a crash). The game's On reaches Streamline as Off
// instead; the menu reports the state as unavailable.
bool AmpereAdmissionHeld() noexcept
{
    return UseAmpere() && ampere_backend::AdmissionUnavailable();
}

// The held Off releases what Streamline keeps for DLSS-G, as the preset
// switch's Off does.
void HoldAmpereOptionsOff(sl::DLSSGOptions& adjusted)
{
    adjusted.mode = sl::DLSSGMode::eOff;
    adjusted.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(adjusted.flags)
        & ~static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff));
    gAmpereAdmissionHeldRequests.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        Log(L"RTX 30/20 DLSS-G held Off: startup admission is unavailable (startup mask 0x%02X, failure %u); "
            L"the game's frame generation request reaches Streamline as Off for the rest of this session",
            ampere_backend::Diagnostics().startupFailureMask, ampere_backend::FailureCode());
}

sl::DLSSGOptions BuildAdjustedOptions(
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool preserveNext)
{
    sl::DLSSGOptions adjusted = CopyKnownOptions(source, preserveNext);
    if (!snapshot.control.followGame && snapshot.control.dynamic
        && DynamicMfgSupported())
    {
        // The injected object is a complete v5 structure even when the game
        // supplied an older prefix, so the active wrapper can consume the
        // dynamic target without reading beyond the game's allocation.
        adjusted.structVersion = sl::kStructVersion5;
        adjusted.mode = sl::DLSSGMode::eDynamic;
        adjusted.dynamicTargetFrameRate =
            static_cast<float>(snapshot.control.dynamicTargetFrameRate);
    }
    else if (!snapshot.control.followGame && !snapshot.control.dynamic)
    {
        // RTX 30/20 store 1x as Off; EffectiveMultiplier never returns it elsewhere.
        const uint32_t multiplier = EffectiveMultiplier(snapshot.control);
        adjusted.mode = multiplier < 2 ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
        adjusted.numFramesToGenerate = multiplier < 2 ? 1u : multiplier - 1;
    }
    // rtx30ngx.1: not while a replacement provider owns frame generation; its
    // options follow the game (or the menu, above) as on RTX 40.
    if (AmpereOptionsApply())
    {
        const bool held = ampere_backend::AdmissionUnavailable();
        const auto decision = ampere_policy::HoldOffWithoutAdmission(AmpereOptions(source, snapshot), held);
        if (decision.mode == ampere_policy::OptionsMode::eOff)
            adjusted.mode = sl::DLSSGMode::eOff;
        else if (decision.mode == ampere_policy::OptionsMode::eDynamic)
        {
            // Follow game keeps the game's own structure and target.
            if (!decision.gameDynamicTarget)
                adjusted.structVersion = sl::kStructVersion5;
            adjusted.mode = sl::DLSSGMode::eDynamic;
        }
        else
            adjusted.mode = sl::DLSSGMode::eOn;
        adjusted.numFramesToGenerate = decision.generatedFrames;
        if (!decision.gameDynamicTarget)
            adjusted.dynamicTargetFrameRate = adjusted.mode == sl::DLSSGMode::eDynamic
                ? static_cast<float>(snapshot.control.dynamicTargetFrameRate) : 0.0f;
        adjusted.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(adjusted.flags)
            & ~static_cast<uint32_t>(sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
        if (held && source.mode != sl::DLSSGMode::eOff)
            HoldAmpereOptionsOff(adjusted);
    }
    if (snapshot.control.generatedOnlyDebug)
    {
        adjusted.flags = static_cast<sl::DLSSGFlags>(
            static_cast<uint32_t>(adjusted.flags)
            | static_cast<uint32_t>(
                sl::DLSSGFlags::eShowOnlyInterpolatedFrame));
    }
    return adjusted;
}

void CaptureGameOptions(
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    const uint32_t capturedViewport = static_cast<uint32_t>(viewport);
    if (capturedViewport != gLastOptionsViewport.load(std::memory_order_acquire)
        || options.colorWidth != gGameColorWidth.load(std::memory_order_relaxed)
        || options.colorHeight != gGameColorHeight.load(std::memory_order_relaxed)
        || options.hudLessBufferFormat != gGameHudlessBufferFormat.load(std::memory_order_relaxed)
        || options.uiBufferFormat != gGameUiBufferFormat.load(std::memory_order_relaxed))
        InvalidateUiInputEvidence(capturedViewport);
    {
        std::lock_guard lock(gLastOptionsMutex);
        gLastGameOptions.viewport = viewport;
        gLastGameOptions.options = CopyKnownOptions(options, false);
        gLastGameOptions.valid = true;
    }
    const uint32_t viewportValue = static_cast<uint32_t>(viewport);
    gLastOptionsViewport.store(viewportValue, std::memory_order_release);
    gGameOptionsStructVersion.store(
        static_cast<uint32_t>(options.structVersion), std::memory_order_relaxed);
    gGameColorWidth.store(options.colorWidth, std::memory_order_relaxed);
    gGameColorHeight.store(options.colorHeight, std::memory_order_relaxed);
    gGameHudlessBufferFormat.store(options.hudLessBufferFormat, std::memory_order_relaxed);
    gGameUiBufferFormat.store(options.uiBufferFormat, std::memory_order_relaxed);
    gGameUiRecompositionEnabled.store(options.structVersion >= sl::kStructVersion4
        && options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
        std::memory_order_relaxed);
}

bool ReadLastGameOptions(
    const sl::ViewportHandle& viewport, sl::DLSSGOptions& options)
{
    std::lock_guard lock(gLastOptionsMutex);
    if (!gLastGameOptions.valid
        || static_cast<uint32_t>(gLastGameOptions.viewport)
            != static_cast<uint32_t>(viewport))
        return false;
    options = gLastGameOptions.options;
    return true;
}

void RecordAppliedControl(const ControlSnapshot& snapshot, sl::Result result,
    bool liveReapply, bool uiRecompositionEnabled, bool uiRecompositionForced,
    uint32_t effectiveMultiplier, bool effectiveDynamicMode,
    bool effectiveDynamicExperimental56, bool dynamicOverrideApplied,
    float effectiveDynamicTargetFrameRate, bool effectiveDynamicTargetValid)
{
    gSetOptionsSeen.store(true, std::memory_order_release);
    gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
    gLastAttemptTick.store(GetTickCount64(), std::memory_order_relaxed);
    gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    // eWarnOutOfVRAM is emitted after Streamline accepts work when DXGI reports
    // no remaining budget. Keep the raw warning for telemetry, but do not leave
    // a successfully submitted multiplier permanently marked as pending.
    if (result != sl::Result::eOk && result != sl::Result::eWarnOutOfVRAM)
        return;

    const uint64_t previous = gAppliedRevision.load(std::memory_order_acquire);
    gAppliedDynamicMode.store(effectiveDynamicMode, std::memory_order_relaxed);
    gAppliedDynamicMaxFrames.store(effectiveDynamicMode ? SafeMaximumMultiplier() : 0u, std::memory_order_relaxed);
    gAppliedMultiplier.store(effectiveMultiplier, std::memory_order_relaxed);
    gAppliedDynamicTargetFrameRate.store(
        effectiveDynamicTargetValid ? effectiveDynamicTargetFrameRate : 0.0f,
        std::memory_order_relaxed);
    gAppliedDynamicTargetValid.store(effectiveDynamicTargetValid,
        std::memory_order_relaxed);
    gAppliedDynamicExperimental56.store(
        effectiveDynamicExperimental56, std::memory_order_relaxed);
    gAppliedGeneratedOnlyDebug.store(
        snapshot.control.generatedOnlyDebug, std::memory_order_relaxed);
    gAppliedUiRecompositionEnabled.store(
        uiRecompositionEnabled, std::memory_order_relaxed);
    gAppliedUiRecompositionForced.store(
        uiRecompositionForced, std::memory_order_relaxed);
    const bool desiredApplied = snapshot.control.followGame
        || (snapshot.control.dynamic
            ? dynamicOverrideApplied && effectiveDynamicMode
            : effectiveMultiplier == snapshot.control.multiplier);
    gAppliedRevision.store(desiredApplied ? snapshot.revision : 0, std::memory_order_release);
    if (!desiredApplied) gAttemptedRevision.store(0, std::memory_order_release);
    if (liveReapply)
        gLiveReapplyCount.fetch_add(1, std::memory_order_relaxed);

    if (previous == snapshot.revision)
        return;
    if (snapshot.control.followGame)
        Log(L"%s follow-game options: dynamic=%d multiplier=%ux result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            effectiveDynamicMode, effectiveMultiplier,
            static_cast<int>(result));
    else if (snapshot.control.dynamic && !dynamicOverrideApplied)
        Log(L"%s dynamic MFG request not applied: capability=%s; "
            L"preserved game options (%s %ux), result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            DynamicMfgCapabilityKnown() ? L"unsupported" : L"checking",
            effectiveDynamicMode ? L"dynamic" : L"fixed",
            effectiveMultiplier, static_cast<int>(result));
    else if (effectiveDynamicMode)
        Log(L"%s dynamic MFG: target=%u FPS experimental56=%d max=%ux result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            snapshot.control.dynamicTargetFrameRate,
            effectiveDynamicExperimental56,
            SafeMaximumMultiplier(),
            static_cast<int>(result));
    else
        Log(L"%s fixed multiplier: %ux, result=%d",
            liveReapply ? L"Live-reapplied" : L"Applied",
            effectiveMultiplier, static_cast<int>(result));
    Log(L"UI recomposition: enabled=%d forced=%d inputsReady=%d "
        L"gameEnabled=%d optionsVersion=%u hudlessFormat=%u uiFormat=%u",
        uiRecompositionEnabled, uiRecompositionForced,
        ReadUiInputSnapshot(gLastOptionsViewport.load(std::memory_order_acquire)).ready,
        gGameUiRecompositionEnabled.load(std::memory_order_relaxed),
        gGameOptionsStructVersion.load(std::memory_order_relaxed),
        gGameHudlessBufferFormat.load(std::memory_order_relaxed),
        gGameUiBufferFormat.load(std::memory_order_relaxed));
}

uint32_t InternalControlBypassDepth() noexcept
{
    const DWORD index = gInternalControlBypassTlsIndex.load(
        std::memory_order_acquire);
    if (index == TLS_OUT_OF_INDEXES)
        return 0;
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(TlsGetValue(index)));
}

class ScopedInternalControlBypass
{
public:
    ScopedInternalControlBypass() noexcept
    {
        index_ = gInternalControlBypassTlsIndex.load(
            std::memory_order_acquire);
        if (index_ == TLS_OUT_OF_INDEXES)
            return;
        previousDepth_ = reinterpret_cast<uintptr_t>(TlsGetValue(index_));
        active_ = TlsSetValue(index_, reinterpret_cast<void*>(
            previousDepth_ + 1)) != FALSE;
    }
    ~ScopedInternalControlBypass()
    {
        if (active_)
        {
            TlsSetValue(index_,
                reinterpret_cast<void*>(previousDepth_));
        }
    }

    ScopedInternalControlBypass(const ScopedInternalControlBypass&) = delete;
    ScopedInternalControlBypass& operator=(
        const ScopedInternalControlBypass&) = delete;

private:
    DWORD index_ = TLS_OUT_OF_INDEXES;
    uintptr_t previousDepth_ = 0;
    bool active_ = false;
};

sl::Result CallRouteSetOptions(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept
{
    if (auto* original = EntryOriginal(
            route.publicSetOriginal, route.publicSetHandle))
    {
        ScopedInternalControlBypass bypass;
        return original(viewport, options);
    }
    auto* internal = EntryOriginal(
        route.internalSetOriginal, route.internalSetHandle);
    if (!internal)
        return sl::Result::eErrorNotInitialized;
    sl::ViewportHandle viewportCopy{
        static_cast<uint32_t>(viewport)};
    sl::DLSSGOptions optionsCopy = CopyKnownOptions(options, false);
    viewportCopy.next = &optionsCopy;
    ScopedInternalControlBypass bypass;
    return internal(&viewportCopy, nullptr);
}

sl::Result CallRouteGetState(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, sl::DLSSGState& state,
    const sl::DLSSGOptions* options) noexcept
{
    if (auto* original = EntryOriginal(
            route.publicGetOriginal, route.publicGetHandle))
    {
        ScopedInternalControlBypass bypass;
        return original(viewport, state, options);
    }
    auto* internal = EntryOriginal(
        route.internalGetOriginal, route.internalGetHandle);
    if (!internal)
        return sl::Result::eErrorNotInitialized;
    sl::ViewportHandle viewportCopy{
        static_cast<uint32_t>(viewport)};
    sl::DLSSGOptions optionsCopy{};
    if (options && IsSupportedOptionsVersion(options->structVersion))
    {
        optionsCopy = CopyKnownOptions(*options, false);
        viewportCopy.next = &optionsCopy;
    }
    ScopedInternalControlBypass bypass;
    return internal(&viewportCopy, &state, nullptr);
}

bool RouteHasStateFunction(const ControlRouteRecord& route) noexcept
{
    return EntryOriginal(route.publicGetOriginal, route.publicGetHandle)
        || EntryOriginal(route.internalGetOriginal, route.internalGetHandle);
}

// A live preset switch keeps DLSS-G Off, without resource retention, until
// Streamline has released every frame-generation feature.
void ApplyPresetSwitchOff(sl::DLSSGOptions& adjusted) noexcept
{
    if (gPresetSwitchPhase.load(std::memory_order_acquire)
        != static_cast<uint32_t>(dlssg_preset_switch::Phase::eDraining))
        return;
    adjusted.mode = sl::DLSSGMode::eOff;
    adjusted.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(adjusted.flags)
        & ~static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff));
}

template <typename SubmitOptions, typename QueryState>
sl::Result SubmitAdjustedOptionsImpl(
    ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool liveReapply, SubmitOptions&& submitOptions,
    QueryState&& queryState, bool hasState,
    const RetainedFeatureIdentity* expectedLiveFeature = nullptr)
{
    const bool dynamicRequested = !snapshot.control.followGame
        && snapshot.control.dynamic;
    if (dynamicRequested && !DynamicMfgCapabilityKnown() && hasState)
    {
        sl::DLSSGState state{};
        const sl::Result stateResult = queryState(state, &source);
        RecordDlssgStateResult(stateResult, state);
    }

    sl::DLSSGOptions adjusted = BuildAdjustedOptions(
        source, snapshot, !liveReapply);
    ApplyPresetSwitchOff(adjusted);
    bool dynamicOverrideApplied = dynamicRequested && DynamicMfgSupported();
    sl::Result result = submitOptions(adjusted);
    sl::Result acceptedResult = result;
    if ((result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM)
        && hasState
        && (!liveReapply
            || (dynamicRequested && !DynamicMfgCapabilityKnown())))
    {
        sl::DLSSGState state{};
        const sl::Result stateResult = queryState(state, &adjusted);
        RecordDlssgStateResult(stateResult, state);
    }

    // A capability query can become valid only after the game's original
    // options initialize DLSS-G. Apply Dynamic immediately once that query
    // explicitly confirms support; otherwise leave the game options intact.
    if ((result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM)
        && dynamicRequested && !dynamicOverrideApplied
        && DynamicMfgSupported())
    {
        const sl::DLSSGOptions acceptedFallback = adjusted;
        adjusted = BuildAdjustedOptions(
            source, snapshot, !liveReapply);
        ApplyPresetSwitchOff(adjusted);
        dynamicOverrideApplied = true;
        result = submitOptions(adjusted);
        if (IsAcceptedControlResult(result))
            acceptedResult = result;
        else
        {
            // The startup fallback was accepted even if the first Dynamic
            // request failed. Keep that actual state separate from the latest
            // submission error; neither the requested target nor an older
            // accepted target describes the active options in this case.
            adjusted = acceptedFallback;
            dynamicOverrideApplied = false;
        }
    }

    const bool preserveGameControl = snapshot.control.followGame
        || (dynamicRequested && !dynamicOverrideApplied);
    const uint32_t effectiveMultiplier =
        [&]() {
            if (UseAmpere()) return adjusted.mode == sl::DLSSGMode::eOff ? 1u : adjusted.numFramesToGenerate + 1u;
            return preserveGameControl
        ? std::clamp(std::min(source.numFramesToGenerate,
                kMaximumMultiplier - 1u) + 1u,
            kMinimumMultiplier, kMaximumMultiplier)
        : EffectiveMultiplier(snapshot.control);
        }();
    const bool effectiveDynamicMode =
        [&]() {
            if (UseAmpere()) return adjusted.mode == sl::DLSSGMode::eDynamic;
            return preserveGameControl
        ? source.mode == sl::DLSSGMode::eDynamic
            || source.mode == sl::DLSSGMode::eAuto
        : snapshot.control.dynamic;
        }();
    const bool effectiveDynamicExperimental56 = false;
    const bool uiRecompositionEnabled = adjusted.structVersion >= sl::kStructVersion4
        && adjusted.enableUserInterfaceRecomposition == sl::Boolean::eTrue;
    const bool effectiveDynamicTargetValid = adjusted.mode == sl::DLSSGMode::eDynamic
        && std::isfinite(adjusted.dynamicTargetFrameRate)
        && adjusted.dynamicTargetFrameRate >= 0.0f;
    std::unique_lock<std::mutex> liveFeatureLock;
    if (liveReapply && AmpereOptionsApply()
        && adjusted.mode != sl::DLSSGMode::eOff
        && IsAcceptedControlResult(acceptedResult))
    {
        liveFeatureLock = std::unique_lock<std::mutex>(
            gFrameGenerationLifetimeMutex);
        if (!expectedLiveFeature
            || !RetainedFeatureStillExactForLiveOnLocked(
                route, viewport, *expectedLiveFeature))
        {
            gSetOptionsSeen.store(true, std::memory_order_release);
            gLastSetOptionsResult.store(static_cast<int32_t>(result),
                std::memory_order_relaxed);
            gLastAttemptTick.store(GetTickCount64(),
                std::memory_order_relaxed);
            gAttemptedRevision.store(snapshot.revision,
                std::memory_order_release);
            gAppliedRevision.store(0, std::memory_order_release);
            if (gAppliedFrameGenerationOn.exchange(
                    false, std::memory_order_acq_rel))
            {
                gPresentationLifecycleEpoch.fetch_add(
                    1, std::memory_order_acq_rel);
            }
            ClearAppliedDynamicTelemetry();
            gFgVsyncSupportKnown.store(false, std::memory_order_release);
            gFgVsyncSupported.store(false, std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Accepted live On revision %llu left pending: retained "
                L"feature changed during the setter call",
                static_cast<unsigned long long>(snapshot.revision));
            return HostControlResult(result);
        }
    }
    RecordAppliedControl(snapshot, acceptedResult, liveReapply,
        uiRecompositionEnabled, uiRecompositionEnabled && !GameEnablesUiRecomposition(source),
        effectiveMultiplier, effectiveDynamicMode,
        effectiveDynamicExperimental56, dynamicOverrideApplied,
        adjusted.dynamicTargetFrameRate, effectiveDynamicTargetValid);
    if (result != acceptedResult)
    {
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        gAttemptedRevision.store(snapshot.revision, std::memory_order_release);
    }
    if (IsAcceptedControlResult(acceptedResult))
    {
        if (liveFeatureLock.owns_lock())
        {
            RecordAcceptedSetOptionsLifecycleLocked(route, viewport,
                adjusted.mode != sl::DLSSGMode::eOff);
        }
        else
        {
            RecordSetOptionsLifecycle(route, viewport,
                adjusted.mode != sl::DLSSGMode::eOff, acceptedResult);
        }
        // The lifecycle helper also serves unsnapshotted game calls. Restore
        // the exact revision accepted by this adjusted submission in case a
        // newer config arrived while the native setter was running.
        route.lastAcceptedRevision.store(snapshot.revision,
            std::memory_order_release);
    }
    return HostControlResult(result);
}

sl::Result SubmitAdjustedOptions(
    ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& source, const ControlSnapshot& snapshot,
    bool liveReapply,
    const RetainedFeatureIdentity* expectedLiveFeature = nullptr)
{
    const bool hasState = RouteHasStateFunction(route);
    return SubmitAdjustedOptionsImpl(route, viewport, source, snapshot,
        liveReapply,
        [&](const sl::DLSSGOptions& adjusted) {
            return CallRouteSetOptions(route, viewport, adjusted);
        },
        [&](sl::DLSSGState& state, const sl::DLSSGOptions* options) {
            return CallRouteGetState(route, viewport, state, options);
        }, hasState, expectedLiveFeature);
}

sl::Result PassPublicSet(ControlRouteRecord& route, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept;

// Live DLSS-G preset switching, on the game's own Streamline calls and
// under gStreamlineCallMutex. With submit, this call sends the forced Off and
// the game's options afterwards itself (GetState). Without it, the game's
// SetOptions call that follows carries them (ApplyPresetSwitchOff).
void AdvancePresetSwitch(const sl::ViewportHandle& viewport, bool submit)
{
    using namespace dlssg_preset_switch;
    const auto phase = static_cast<Phase>(gPresetSwitchPhase.load(std::memory_order_acquire));
    const auto selection = dlssg_preset::ReadSelection();
    if (phase == Phase::eIdle && !selection.restartRequired)
        return;
    if (!gControlReady.load(std::memory_order_acquire)
        || gGameFrameGenerationViewport.load(std::memory_order_acquire)
            != static_cast<uint32_t>(viewport))
        return;
    ControlRouteRecord* route = ActiveControlRoute();
    if (!route)
        return;
    sl::DLSSGOptions source{};
    Inputs inputs{};
    inputs.phase = phase;
    inputs.pending = selection.restartRequired;
    inputs.supported = PresetSwitchUnavailableReason(selection.requested) == Unavailable::eNone;
    inputs.quiescent = ampere_backend::FrameGenerationQuiescent();
    inputs.gameWantsOn = gGameFrameGenerationOn.load(std::memory_order_acquire);
    inputs.optionsKnown = ReadLastGameOptions(viewport, source);
    inputs.bridgeReady = BridgeReady();
    const uint64_t now = GetTickCount64();
    inputs.elapsedMs = phase == Phase::eDraining && now >= gPresetSwitchStartTick
        ? now - gPresetSwitchStartTick : 0;
    const auto resume = [&]() {
        if (!submit || !inputs.gameWantsOn || !inputs.optionsKnown)
            return;
        // As the game's own call: Streamline creates a new feature, whose
        // EndpointCore::Create reads the latched preset. Without a ready control
        // route, pass the game's options as PassPublicSet does (RTX 30/20: its
        // fixed translated-route options).
        sl::Result result = sl::Result::eErrorNotInitialized;
        if (BridgeReady())
            result = SubmitAdjustedOptions(*route, viewport, source, ReadControlSnapshot(), false);
        else
        {
            result = UseAmpere() ? PassPublicSet(*route, viewport, source)
                : CallRouteSetOptions(*route, viewport, source);
            gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
            RecordSetOptionsLifecycle(*route, viewport,
                source.mode != sl::DLSSGMode::eOff && !AmpereAdmissionHeld(), result);
        }
        if (!IsAcceptedControlResult(result))
            Log(L"DLSS-G preset switch: frame generation On after the re-create failed: result=%d",
                static_cast<int>(result));
    };
    const auto relatch = [&]() {
        const auto before = dlssg_preset::ReadSelection();
        if (!dlssg_preset::RelatchPending())
            return false;
        gPresetSwitchFailedRequest.store(UINT32_MAX, std::memory_order_release);
        Log(L"DLSS-G preset switch: preset %u -> %u latched; configuration reset; awaiting configuration read",
            before.latched, before.requested);
        return true;
    };
    switch (Decide(inputs))
    {
    case Action::eNone:
    case Action::eKeepDraining:
        return;
    case Action::eRelatch:
        relatch();
        return;
    case Action::eBeginDrain:
    {
        gPresetSwitchStartTick = now;
        gPresetSwitchPhase.store(static_cast<uint32_t>(Phase::eDraining), std::memory_order_release);
        const auto liveness = ampere_backend::ReadFrameGenerationLiveness();
        Log(L"DLSS-G preset switch: preset %u -> %u; frame generation Off without resource retention "
            L"until %u live feature(s) are released", selection.latched, selection.requested,
            liveness.liveFeatures);
        if (!submit)
            return;
        const sl::Result result = SubmitAdjustedOptions(*route, viewport, source, ReadControlSnapshot(), true);
        if (IsAcceptedControlResult(result))
            return;
        // Nothing changed: the game's feature keeps running with the old preset.
        gPresetSwitchPhase.store(static_cast<uint32_t>(Phase::eIdle), std::memory_order_release);
        gPresetSwitchFailedRequest.store(selection.requested, std::memory_order_release);
        Log(L"DLSS-G preset switch: Off was refused (result=%d); preset %u applies after a restart",
            static_cast<int>(result), selection.requested);
        return;
    }
    case Action::eRelatchAndResume:
        if (!relatch())
        {
            // A temporary endpoint/VRAM estimate may hold the gate. Keep Off
            // while retrying, bounded by the same drain timeout. A refusal must
            // never turn into repeated Off/On cycles or a reported success.
            if (inputs.elapsedMs < kDrainTimeoutMs && inputs.supported) return;
            gPresetSwitchFailedRequest.store(selection.requested, std::memory_order_release);
            Log(L"DLSS-G preset switch: features released but the latch was refused; preset %u stays saved",
                selection.requested);
        }
        gPresetSwitchPhase.store(static_cast<uint32_t>(Phase::eIdle), std::memory_order_release);
        resume();
        return;
    case Action::eResume:
        gPresetSwitchPhase.store(static_cast<uint32_t>(Phase::eIdle), std::memory_order_release);
        Log(L"DLSS-G preset switch: selection returned to preset %u; frame generation resumes", selection.latched);
        resume();
        return;
    case Action::eAbort:
    {
        gPresetSwitchPhase.store(static_cast<uint32_t>(Phase::eIdle), std::memory_order_release);
        const auto liveness = ampere_backend::ReadFrameGenerationLiveness();
        // A game that turned frame generation off itself retries at its next On.
        if (inputs.gameWantsOn)
            gPresetSwitchFailedRequest.store(selection.requested, std::memory_order_release);
        Log(L"DLSS-G preset switch: %u feature(s) still live (%u Create(s) in flight) after %llu ms; "
            L"preset %u applies after a restart", liveness.liveFeatures, liveness.createsInFlight,
            static_cast<unsigned long long>(inputs.elapsedMs), selection.requested);
        resume();
        return;
    }
    }
}

void ReapplyPendingControl(const sl::ViewportHandle& viewport)
{
    if (!gControlReady.load(std::memory_order_acquire)
        || !gGameFrameGenerationOn.load(std::memory_order_acquire)
        || gGameFrameGenerationViewport.load(std::memory_order_acquire)
            != static_cast<uint32_t>(viewport)
        || gRestartRequired.load(std::memory_order_acquire)
        // A live preset switch owns the submissions until its re-create.
        || gPresetSwitchPhase.load(std::memory_order_acquire)
            != static_cast<uint32_t>(dlssg_preset_switch::Phase::eIdle)
        || !BridgeReady())
        return;

    const ControlSnapshot snapshot = ReadControlSnapshot();
    if (snapshot.revision == 0
        || snapshot.revision == gAppliedRevision.load(std::memory_order_acquire))
        return;
    if (!snapshot.control.followGame && snapshot.control.dynamic
        && !DynamicMfgSupported())
        return;
    // Keep an unavailable fixed request pending without repeatedly submitting
    // a lower fallback or labelling that fallback as the requested revision.
    if (!snapshot.control.followGame && !snapshot.control.dynamic
        && snapshot.control.multiplier > SafeMaximumMultiplier())
        return;

    const uint64_t attemptedRevision =
        gAttemptedRevision.load(std::memory_order_acquire);
    bool retryNotInitialized = false;
    if (snapshot.revision == attemptedRevision)
    {
        const int32_t result = gLastSetOptionsResult.load(std::memory_order_relaxed);
        if (result != static_cast<int32_t>(sl::Result::eErrorNotInitialized))
            return;
        const uint64_t now = GetTickCount64();
        const uint64_t previousAttempt =
            gLastAttemptTick.load(std::memory_order_relaxed);
        if (now < previousAttempt
            || now - previousAttempt < kNotInitializedRetryDelayMs)
            return;
        retryNotInitialized = true;
    }

    ControlRouteRecord* route = ActiveControlRoute();
    sl::DLSSGOptions source{};
    if (!route || !ReadLastGameOptions(viewport, source))
        return;
    RetainedFeatureIdentity expectedLiveFeature{};
    std::unique_lock<std::mutex> lifetimeLock(gFrameGenerationLifetimeMutex, std::defer_lock);
    if (UseAmpere())
    {
        lifetimeLock.lock();
        const uint64_t token = gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire);
        const auto pending = ampere_backend::FeatureCreatePendingStatus(token);
        if (pending == ampere_backend::FeatureCreatePendingState::eCurrent
            || pending == ampere_backend::FeatureCreatePendingState::eReadBusy)
            return;
        if (pending == ampere_backend::FeatureCreatePendingState::eInvalid)
        {
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Live On revision %llu blocked: unfinished feature lifecycle proof is no longer exact",
                static_cast<unsigned long long>(snapshot.revision));
            return;
        }
    }
    if (!CanReenableRetainedFeatureLocked(*route, viewport, snapshot, source,
            &expectedLiveFeature))
    {
        // The exact epoch/entry publication uses this same lifetime lock.
        // A newer Create cannot slip between this decision and its latch.
        gRestartRequired.store(true, std::memory_order_release);
        Log(L"Live On revision %llu blocked: retained feature identity, "
            L"capacity, provider, publication, or generation is no longer exact",
            static_cast<unsigned long long>(snapshot.revision));
        return;
    }
    if (lifetimeLock.owns_lock()) lifetimeLock.unlock();
    if (retryNotInitialized)
    {
        const uint64_t retry =
            gNotInitializedRetryCount.fetch_add(1, std::memory_order_relaxed) + 1;
        Log(L"Retrying request revision %llu after Streamline result 21 (retry %llu)",
            static_cast<unsigned long long>(snapshot.revision),
            static_cast<unsigned long long>(retry));
    }

    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::Result result =
        SubmitAdjustedOptions(*route, viewport, source, snapshot, true,
            &expectedLiveFeature);
    if (result != sl::Result::eOk)
        Log(L"Live reapply failed for request revision %llu: result=%d",
            static_cast<unsigned long long>(snapshot.revision), static_cast<int>(result));
}

bool SameMfgControl(const ControlConfig& left,
    const ControlConfig& right) noexcept
{
    return left.followGame == right.followGame
        && left.multiplier == right.multiplier
        && left.dynamic == right.dynamic
        && left.dynamicTargetFrameRate == right.dynamicTargetFrameRate
        && left.dlssgPreset == right.dlssgPreset
        && left.dlssSrPreset == right.dlssSrPreset
        && left.dlssRrPreset == right.dlssRrPreset
        && left.dlss5 == right.dlss5
        && left.vsyncMode == right.vsyncMode
        && left.reflexFrameLimitFps == right.reflexFrameLimitFps
        && left.dynamicExperimental56 == right.dynamicExperimental56
        && left.generatedOnlyDebug == right.generatedOnlyDebug;
}

HMODULE ModuleFromAddress(const void* address)
{
    MEMORY_BASIC_INFORMATION memory{};
    return address
        && VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        ? static_cast<HMODULE>(memory.AllocationBase) : nullptr;
}

void PublishProviderCreateState(HMODULE provider, bool backportReady)
{
    std::lock_guard lock(gModuleMutex);
    for (auto& record : gModuleRecords)
    {
        if (record.module != provider)
            continue;
        record.ngxTemporalPatched = backportReady;
        if (UseAmpere())
        {
            record.ngxPatched = backportReady;
        }
        break;
    }
    RecomputeModuleStateLocked();
}

bool ExportsNgxD3D12Route(HMODULE module) noexcept
{
    return module
        && GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature")
        && GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature")
        && GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements");
}

bool ExportsNgxVulkanRoute(HMODULE module) noexcept
{
    return module
        && (GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature")
            || GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1"))
        && GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature")
        && GetProcAddress(module, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
}

bool IsNgxRuntimeModule(HMODULE module) noexcept
{
    // OptiScaler exports an NGX-compatible facade (D3D12/Vulkan CreateFeature,
    // Init_ProjectID...). It is the caller of NVIDIA's runtime, not the runtime.
    if (optiscaler_host::IsHostModule(module) || optiscaler_host::IsOptiScalerImage(module))
        return false;
    return ngx_runtime_policy::IsRuntime(module);
}

const wchar_t* NgxDispatchRouteName(NgxDispatchRoute route) noexcept
{
    return route == NgxDispatchRoute::eRuntime ? L"runtime" : L"provider";
}

const wchar_t* NgxGraphicsApiName(NgxGraphicsApi api) noexcept
{
    switch (api)
    {
    case NgxGraphicsApi::eD3D12: return L"D3D12";
    case NgxGraphicsApi::eVulkan: return L"Vulkan";
    default: return L"unknown";
    }
}

bool CanResolveNgxDispatchRoute(NgxDispatchRoute route) noexcept
{
    const auto active = static_cast<NgxDispatchRoute>(
        gActiveNgxDispatchRoute.load(std::memory_order_acquire));
    return universal_route_policy::ShouldInspectNgxCreate(
        true, active, route);
}

bool CommitNgxDispatchRoute(NgxDispatchRoute route) noexcept
{
    uint32_t expected = static_cast<uint32_t>(NgxDispatchRoute::ePending);
    const uint32_t requested = static_cast<uint32_t>(route);
    if (gActiveNgxDispatchRoute.compare_exchange_strong(
            expected, requested, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return true;
    return universal_route_policy::CanCommitNgxDispatchRoute(true,
        static_cast<NgxDispatchRoute>(expected), route);
}

bool CommitNgxGraphicsApi(NgxGraphicsApi api) noexcept
{
    uint32_t expected = static_cast<uint32_t>(NgxGraphicsApi::eUnknown);
    const uint32_t requested = static_cast<uint32_t>(api);
    if (gActiveNgxGraphicsApi.compare_exchange_strong(
            expected, requested, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return true;
    if (expected == requested)
        return true;
    gProviderChangedAfterCreate.store(true, std::memory_order_release);
    gRestartRequired.store(true, std::memory_order_release);
    Log(L"Rejected graphics API change after FG pipeline selection: "
        L"active=%s observed=%s; restart required",
        NgxGraphicsApiName(static_cast<NgxGraphicsApi>(expected)),
        NgxGraphicsApiName(api));
    return false;
}

bool ResolveUniqueHookedProvider(NgxGraphicsApi api, HMODULE& provider,
    std::wstring* path) noexcept
{
    HMODULE candidate = nullptr;
    std::wstring candidatePath;
    uint32_t candidateCount = 0;
    // RTX 30 Vulkan: a provider counts once the Ampere route prepared its SM86
    // program and covers its Vulkan entries (ngxPatched follows the first
    // Create there). That check takes the Ampere lock, so it runs after the
    // module lock is released.
    const bool ampereVulkan = UseAmpere() && api == NgxGraphicsApi::eVulkan;
    std::array<HMODULE, 8> ampereModules{};
    std::array<std::wstring, 8> amperePaths{};
    size_t ampereCount = 0;
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            if (!record.ngxExport || !record.ngxCandidate
                || (!ampereVulkan && !record.ngxPatched))
                continue;
            const entry_detour::Kind createKind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanCreateFeature
                : entry_detour::Kind::eNgxD3D12CreateFeature;
            const entry_detour::Kind create1Kind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanCreateFeature1
                : entry_detour::Kind::eCount;
            const entry_detour::Kind evaluateKind =
                api == NgxGraphicsApi::eVulkan
                ? entry_detour::Kind::eNgxVulkanEvaluateFeature
                : entry_detour::Kind::eNgxD3D12EvaluateFeature;
            const entry_detour::Snapshot create =
                entry_detour::ReadSnapshot(createKind, record.module);
            const entry_detour::Snapshot create1 =
                create1Kind != entry_detour::Kind::eCount
                ? entry_detour::ReadSnapshot(create1Kind, record.module)
                : entry_detour::Snapshot{};
            const entry_detour::Snapshot evaluate =
                entry_detour::ReadSnapshot(evaluateKind, record.module);
            const entry_detour::Snapshot selectedCreate = create.current
                ? create : create1;
            if (!selectedCreate.current || !evaluate.current
                || selectedCreate.generation != record.generation
                || evaluate.generation != record.generation)
                continue;
            if (ampereVulkan)
            {
                // More candidates than routes fail closed below.
                if (ampereCount == ampereModules.size()) { ++candidateCount; continue; }
                ampereModules[ampereCount] = record.module;
                amperePaths[ampereCount++] = record.path;
                continue;
            }
            ++candidateCount;
            candidate = record.module;
            if (path)
                candidatePath = record.path;
        }
    }
    for (size_t i = 0; i < ampereCount; ++i)
    {
        if (!ampere_backend::VulkanProviderPrepared(ampereModules[i])
            || !dlssg_provider_policy::IsSupportedRetainedAmpereProvider(ampereModules[i]))
            continue;
        ++candidateCount;
        candidate = ampereModules[i];
        if (path)
            candidatePath = amperePaths[i];
    }
    if (universal_route_policy::ResolveProvider(false, candidateCount)
        != universal_route_policy::ProviderResolution::eUniqueCandidate)
        return false;
    provider = candidate;
    if (path)
        *path = std::move(candidatePath);
    return true;
}

bool ResolveMidpointProvider(NgxDispatchRoute route, NgxGraphicsApi api,
    const entry_detour::Snapshot& detour, const void* originalCaller,
    HMODULE& provider, std::wstring* path,
    NgxProviderSelectionSource& source)
{
    provider = nullptr;
    if (path)
        path->clear();
    source = NgxProviderSelectionSource::eNone;
    if (route == NgxDispatchRoute::eProvider)
    {
        provider = detour.owner;
        if (dlssg_provider_policy::IsSupportedRetainedProvider(provider))
        {
            if (path)
                *path = LoadedModulePath(provider);
            source = NgxProviderSelectionSource::eProviderEntry;
            return true;
        }
        return false;
    }

    // The runtime's own dispatch table names the provider NGX bound to Frame Generation. RTX 30 always
    // used it; RTX 40 and RTX 50 need it when a game's own DLSS-G and an NVIDIA app override copy are both
    // loaded (both fully covered: the unique-candidate fallback below must not choose between them).
    if ((UseAmpere() || gpu_dispatch::UsesNativeProgram()) && api == NgxGraphicsApi::eD3D12)
    {
        const auto create = detour.kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
            ? detour : entry_detour::ReadSnapshot(entry_detour::Kind::eNgxRuntimeD3D12CreateFeature, detour.owner);
        ngx_runtime_dispatch::Selection selected{};
        const auto result = ngx_runtime_dispatch::Read(create, NVSDK_NGX_Feature_FrameGeneration, selected);
        if (result == ngx_runtime_dispatch::ReadResult::eInvalid) return false;
        if (result == ngx_runtime_dispatch::ReadResult::eSelected)
        {
            const auto entry = entry_detour::ReadSnapshot(entry_detour::Kind::eNgxD3D12CreateFeature, selected.provider);
            if (!entry.current || entry.generation != ModuleGeneration(selected.provider)
                || entry.target != reinterpret_cast<void*>(selected.target)
                || !ngx_runtime_dispatch::StillCurrent(selected)
                || !(UseAmpere() ? dlssg_provider_policy::IsSupportedRetainedAmpereProvider(selected.provider)
                    : dlssg_provider_policy::IsSupportedRetainedProvider(selected.provider))) return false;
            provider = selected.provider;
            if (path) *path = LoadedModulePath(provider);
            source = NgxProviderSelectionSource::eRuntimeDispatchTable;
            return true;
        }
    }

    // Prefer the preserved caller when the runtime really was entered by a
    // DLSS-G provider. Some NGX runtimes instead receive the call from a
    // runtime-owned dispatcher. In that case, fall back only when discovery
    // has exactly one supported, patched provider with current Create and
    // Evaluate entry detours. Zero or multiple candidates remain fail-closed.
    provider = ModuleFromAddress(originalCaller);
    if (dlssg_provider_policy::IsSupportedRetainedProvider(provider))
    {
        if (path)
            *path = LoadedModulePath(provider);
        source = NgxProviderSelectionSource::eRuntimeCaller;
        return true;
    }
    provider = nullptr;
    if (path)
        path->clear();
    if (!ResolveUniqueHookedProvider(api, provider, path))
        return false;
    source = NgxProviderSelectionSource::eRuntimeUniqueCandidate;
    return true;
}

void BeforeNgxCreateFeatureForRoute(NgxDispatchRoute route,
    NgxGraphicsApi api, void* commandContext, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle,
    entry_detour::Handle entryHandle, const void* originalCaller) noexcept
{
    (void)parameters;
    (void)handle;

    const bool frameGeneration = feature == NVSDK_NGX_Feature_FrameGeneration;
    // The shared NGX runtime also carries DLSS Super Resolution and other
    // feature traffic. Those calls must reach NVIDIA without MFG logging,
    // route publication, provider selection, or descriptor work.
    if (!frameGeneration)
        return;
    if (UseAmpere())
        EnsureAmpereFeatureLifetimeObserver();
    if (!CanResolveNgxDispatchRoute(route))
        return;
    const uint64_t observedCall = gNgxCreateCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (observedCall <= 16)
    {
        Log(L"NGX_CREATE observed=%llu dispatch=%s feature=%u "
            L"api=%s frameGeneration=%d",
            static_cast<unsigned long long>(observedCall),
            NgxDispatchRouteName(route), static_cast<uint32_t>(feature),
            NgxGraphicsApiName(api), frameGeneration);
    }

    const uint64_t call = gNgxFrameGenerationCreateCalls.fetch_add(
        1, std::memory_order_relaxed) + 1;
    const entry_detour::Snapshot detour =
        entry_detour::ReadSnapshot(entryHandle);
    HMODULE provider = nullptr;
    std::wstring path;
    NgxProviderSelectionSource selectionSource =
        NgxProviderSelectionSource::eNone;
    const bool providerAccepted = detour.current
        && ResolveMidpointProvider(route, api, detour, originalCaller,
            provider, &path, selectionSource);

    if (!providerAccepted)
    {
        // In particular, do not let an ambiguous shared-runtime observation
        // poison this Create. The nested concrete provider entry can still
        // claim the pending route before NVIDIA executes the implementation.
        if (call == 1 || (call & (call - 1)) == 0)
        {
            Log(L"MFG_CREATE pre-call=%llu dispatch=%s api=%s "
                L"providerAccepted=0 "
                L"routeCommitted=0 adapterVerified=%d "
                L"backportReadyAtCreate=0 targetRva=0x%X "
                L"callerPreserved=%d",
                static_cast<unsigned long long>(call),
                NgxDispatchRouteName(route), NgxGraphicsApiName(api),
                AdapterVerifiedForApi(api),
                detour.targetRva, detour.forwarding);
        }
        return;
    }

    if (!CommitNgxGraphicsApi(api))
        return;

    const uintptr_t providerBase = reinterpret_cast<uintptr_t>(provider);
    uintptr_t activeProvider = gActiveNgxProviderBase.load(
        std::memory_order_acquire);
    if (activeProvider == 0)
    {
        gActiveNgxProviderBase.compare_exchange_strong(
            activeProvider, providerBase, std::memory_order_acq_rel,
            std::memory_order_acquire);
        activeProvider = gActiveNgxProviderBase.load(
            std::memory_order_acquire);
    }
    if (universal_route_policy::SelectProvider(activeProvider, providerBase)
        == universal_route_policy::ProviderSelection::eRejectedChange)
    {
        gProviderChangedAfterCreate.store(true, std::memory_order_release);
        gRestartRequired.store(true, std::memory_order_release);
        gBackportReadyAtCreate.store(false, std::memory_order_release);
        Log(L"Rejected provider change after FG pipeline selection: "
            L"active=%p observed=%p; recreate required",
            reinterpret_cast<void*>(activeProvider), provider);
        return;
    }
    if (!CommitNgxDispatchRoute(route))
        return;

    // A post-call lifecycle observer publishes the opaque handle only after
    // native Create succeeds.  This pre-call epoch prevents an older Release
    // completion from clearing route state after a newer Create has begun.
    const uint64_t providerGeneration = ModuleGeneration(provider);
    const entry_detour::Kind evaluateKind = api == NgxGraphicsApi::eVulkan
        ? route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature
            : entry_detour::Kind::eNgxVulkanEvaluateFeature
        : route == NgxDispatchRoute::eRuntime
            ? entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature
            : entry_detour::Kind::eNgxD3D12EvaluateFeature;
    const entry_detour::Snapshot evaluate =
        entry_detour::ReadSnapshot(evaluateKind, detour.owner);
    bool firstPipelineCreate = false;
    {
        std::unique_lock<std::mutex> lifetimeLock(gFrameGenerationLifetimeMutex, std::defer_lock);
        if (UseAmpere()) lifetimeLock.lock();
        uint64_t createAttemptToken =
            gFrameGenerationCreateAttemptEpoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        if (!createAttemptToken)
        {
            createAttemptToken =
                gFrameGenerationCreateAttemptEpoch.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
        }
        if (UseAmpere())
        {
            ampere_backend::SetCurrentFeatureCreateAttemptToken(
                createAttemptToken);
        }
        firstPipelineCreate =
            !gFrameGenerationCreateObserved.exchange(
                true, std::memory_order_acq_rel);
        gActiveNgxProviderGeneration.store(providerGeneration, std::memory_order_release);
        gActiveNgxSelectionSource.store(static_cast<uint32_t>(selectionSource),
            std::memory_order_release);
        gActiveNgxCreateHandle.store(PackEntryHandle(entryHandle),
            std::memory_order_release);
        if (evaluate.current)
        {
            gActiveNgxEvaluateHandle.store(
                PackEntryHandle(evaluate.handle), std::memory_order_release);
        }
    }
    if (selectionSource
        == NgxProviderSelectionSource::eRuntimeUniqueCandidate
        && firstPipelineCreate)
    {
        Log(L"Runtime Create caller was not a provider; selected the one "
            L"fully covered DLSS-G provider: path=%s", path.c_str());
    }

    // The loader-friendly route cannot assume that Streamline exposed
    // slSetD3DDevice before this call. The command list is authoritative for
    // the device that will create the FG feature, so validate that adapter at
    // the last safe pre-create point.
    if (api == NgxGraphicsApi::eD3D12
        && !gpu_backend::AdapterVerified() && commandContext)
    {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(
            commandContext);
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(commandList->GetDevice(
                __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))
            && device)
        {
            gpu_backend::ObserveD3D12Device(device);
            device->Release();
        }
    }
    if (NativeDlssgPresetGpu())
    {
        dlssg_preset::Prepare();
        InspectLoadedModule(provider, path);
    }
    const bool adapterVerified = AdapterVerifiedForApi(api);
    // Temporal publication alone is insufficient: the provider must also
    // accept the multi-frame count/index before the first feature compiles.
    const bool deviceGateReady = !gpu_dispatch::IsAda() || ngx_mfg_gate::Ready(provider);
    // GitHub issue 149: the provider this Create uses decides whether RTX 40 holds the game at 2X (later inspections
    // of other loaded providers no longer change it).
    if (gpu_dispatch::IsAda())
    {
        const bool missing = !deviceGateReady && ngx_mfg_gate::Missing(provider);
        if (missing || deviceGateReady)
            NoteAdaProviderGate(missing, path, ada_gate_hold::Source::eCreate);
    }
    bool ready = adapterVerified && deviceGateReady
        && gpu_backend::PatchProvider(provider, path.c_str());
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    if (ready && gpu_dispatch::IsAda())
    {
        const midpoint_fix::OutputPullMaskCreateBoundary boundary{
            api == NgxGraphicsApi::eD3D12
                ? midpoint_fix::OutputPullMaskGraphicsApi::eD3D12
                : api == NgxGraphicsApi::eVulkan
                    ? midpoint_fix::OutputPullMaskGraphicsApi::eVulkan
                    : midpoint_fix::OutputPullMaskGraphicsApi::eUnknown,
            firstPipelineCreate,
            gPipelineMayPredateDetour.load(std::memory_order_acquire),
            OutputPullMaskEarlyInitProven(provider, api)};
        midpoint_fix::PrepareOutputPullMaskForCreate(
            provider, path.c_str(), boundary);
        const auto mask = midpoint_fix::ReadOutputPullMaskSnapshot();
        // An unsupported optional optimization retains the temporal route.
        // Uncertain publication/protection ownership must never be hidden by
        // the readiness value captured before the optional publisher ran.
        if (mask.requiresRestart)
        {
            ready = false;
            gRestartRequired.store(true, std::memory_order_release);
        }
        else
        {
            ready = gpu_backend::Ready();
        }
    }
#endif
    gBackportReadyAtCreate.store(ready, std::memory_order_release);
    if (firstPipelineCreate)
        gFirstCreateMidpointReady.store(ready, std::memory_order_release);
    if (provider)
        PublishProviderCreateState(provider, ready);
    if (ready)
    {
        gPipelineMayPredateDetour.store(false, std::memory_order_release);
        gRestartRequired.store(false, std::memory_order_release);
    }
    if (call == 1 || (call & (call - 1)) == 0)
    {
        Log(L"MFG_CREATE pre-call=%llu dispatch=%s api=%s "
            L"providerAccepted=%d "
            L"routeCommitted=1 adapterVerified=%d backportReadyAtCreate=%d "
            L"targetRva=0x%X callerPreserved=%d",
            static_cast<unsigned long long>(call),
            NgxDispatchRouteName(route), NgxGraphicsApiName(api),
            providerAccepted,
            adapterVerified, ready, detour.targetRva, detour.forwarding);
    }
}

void WINAPI BeforeNgxD3D12CreateFeature(void* commandList,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eD3D12, commandList,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeD3D12CreateFeature(void* commandList,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eD3D12, commandList,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

bool TryInstallNgxCreateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return ampere_backend::InstallRoute(provider, path.c_str(), generation, false,
        &BeforeNgxD3D12CreateFeature, &BeforeNgxD3D12EvaluateFeature);
}

void BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute route,
    NgxGraphicsApi api, void* commandList, uintptr_t handleValue,
    const void* parameters,
    void* callback, entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    const uint32_t active = gActiveNgxDispatchRoute.load(
        std::memory_order_acquire);
    if (gActiveNgxGraphicsApi.load(std::memory_order_acquire)
        != static_cast<uint32_t>(api))
        return;

    if (UseAmpere())
    {
        // The Ampere gate invokes this only inside an owned runtime operation.
        // Check the pinned entry identity without reopening the provider file on
        // every frame. Optional interval collection remains disabled by default.
        // RTX 30 Vulkan uses the same nested provider observation.
        const bool vulkan = api == NgxGraphicsApi::eVulkan;
        const auto providerEvaluateKind = vulkan
            ? entry_detour::Kind::eNgxVulkanEvaluateFeature : entry_detour::Kind::eNgxD3D12EvaluateFeature;
        const auto runtimeEvaluateKind = vulkan
            ? entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature : entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature;
        const auto runtimeCreateKind = [vulkan](entry_detour::Kind kind) noexcept {
            return vulkan ? kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
                    || kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1
                : kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature;
        };
        const auto verified = entry_detour::ReadSnapshot(entryHandle);
        if ((api != NgxGraphicsApi::eD3D12 && !vulkan) || route != NgxDispatchRoute::eProvider
            || !verified.current || verified.kind != providerEvaluateKind
            || verified.owner != ampere_gpu::Provider()
            || reinterpret_cast<uintptr_t>(verified.owner) != gActiveNgxProviderBase.load(std::memory_order_acquire)
            || verified.generation != gActiveNgxProviderGeneration.load(std::memory_order_acquire))
            return;
        const auto providerEvaluate = entry_detour::ReadSnapshot(providerEvaluateKind, verified.owner);
        const auto selected = entry_detour::ReadSnapshot(UnpackEntryHandle(
            gActiveNgxEvaluateHandle.load(std::memory_order_acquire)));
        if (!providerEvaluate.current || providerEvaluate.currentEntries != 1
            || providerEvaluate.handle != entryHandle || !selected.current)
            return;
        if (active == static_cast<uint32_t>(NgxDispatchRoute::eProvider))
        {
            if (selected.kind != providerEvaluateKind
                || selected.handle != entryHandle)
                return;
        }
        else if (active == static_cast<uint32_t>(NgxDispatchRoute::eRuntime))
        {
            // Selection belongs to the outer runtime, while this observation
            // still comes only from the admitted nested provider entry. Never
            // record the outer callback or replace the selected route handle.
            const auto runtimeEvaluate = entry_detour::ReadSnapshot(runtimeEvaluateKind, selected.owner);
            const auto create = entry_detour::ReadSnapshot(UnpackEntryHandle(
                gActiveNgxCreateHandle.load(std::memory_order_acquire)));
            if (selected.kind != runtimeEvaluateKind
                || selected.owner != ModuleFromAddress(originalCaller)
                || !runtimeEvaluate.current || runtimeEvaluate.currentEntries != 1
                || runtimeEvaluate.handle != selected.handle
                || !create.current || !runtimeCreateKind(create.kind)
                || create.owner != selected.owner || create.generation != selected.generation
                || !AmpereFeatureEntriesCurrent(reinterpret_cast<uintptr_t>(selected.owner), selected.generation,
                    reinterpret_cast<uintptr_t>(verified.owner), verified.generation))
                return;
        }
        else return;
        temporal_interval_trace::Record(reinterpret_cast<const NVSDK_NGX_Handle*>(handleValue),
            static_cast<const NVSDK_NGX_Parameter*>(parameters), gpu_backend::Ready());
        gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    else
    {
        if (active != static_cast<uint32_t>(route))
            return;
        // Evaluate has no feature-id argument. Prefer the selected provider as the
        // caller. Some games (including AFOP) enter the shared runtime through a
        // runtime-owned dispatcher even though Create safely resolved one unique
        // covered DLSS-G provider. For that shape, defer admission until the
        // namespaced temporal parameters independently identify this as FG; an SR
        // or other DLSS 5 Evaluate remains invisible to this telemetry path.
        const bool callerIsSelectedProvider =
            reinterpret_cast<uintptr_t>(ModuleFromAddress(originalCaller))
            == gActiveNgxProviderBase.load(std::memory_order_acquire);
        // Create resolved its provider through the runtime: the one covered provider, or (two
        // DLSS-G copies loaded) the runtime's own dispatch table. Either way the runtime's FG
        // Evaluates are admitted once they resolve to that provider and carry temporal parameters.
        const uint32_t activeSource =
            gActiveNgxSelectionSource.load(std::memory_order_acquire);
        const bool selectedThroughRuntime =
            activeSource == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeUniqueCandidate)
            || activeSource == static_cast<uint32_t>(
                NgxProviderSelectionSource::eRuntimeDispatchTable);
        if (route == NgxDispatchRoute::eRuntime
            && !callerIsSelectedProvider && !selectedThroughRuntime)
            return;

        const entry_detour::Snapshot detour =
            entry_detour::ReadSnapshot(entryHandle);
        HMODULE observedProvider = nullptr;
        NgxProviderSelectionSource selectionSource =
            NgxProviderSelectionSource::eNone;
        const bool providerResolved = detour.current
            && ResolveMidpointProvider(route, api, detour, originalCaller,
                observedProvider, nullptr, selectionSource);
        if (!providerResolved || reinterpret_cast<uintptr_t>(observedProvider)
                != gActiveNgxProviderBase.load(std::memory_order_acquire))
            return;

        uint64_t activeEvaluate = gActiveNgxEvaluateHandle.load(
            std::memory_order_acquire);
        const uint64_t requested = PackEntryHandle(entryHandle);
        if (activeEvaluate == 0)
        {
            gActiveNgxEvaluateHandle.compare_exchange_strong(
                activeEvaluate, requested, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
        if (gActiveNgxEvaluateHandle.load(std::memory_order_acquire)
            != requested)
            return;
        (void)commandList;
        (void)callback;
        const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(
            handleValue);
        const auto* ngxParameters = static_cast<const NVSDK_NGX_Parameter*>(
            parameters);
        if (route == NgxDispatchRoute::eRuntime
            && !callerIsSelectedProvider)
        {
            const bool validTemporalSample =
                temporal_interval_trace::RecordIfValidTemporalSample(
                    handle, ngxParameters, gpu_backend::Ready());
            if (!universal_route_policy::CanInspectRuntimeEvaluate(false,
                    selectedThroughRuntime, validTemporalSample))
                return;
        }
        else
        {
            temporal_interval_trace::Record(
                handle, ngxParameters, gpu_backend::Ready());
        }
        gNgxEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
    }
}

void WINAPI BeforeNgxD3D12EvaluateFeature(void* commandList,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eD3D12, commandList, handle, parameters, callback,
        entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeD3D12EvaluateFeature(void* commandList,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eD3D12, commandList, handle, parameters, callback,
        entryHandle,
        originalCaller);
}

bool TryInstallNgxEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return ampere_backend::InstallRoute(provider, path.c_str(), generation, false,
        &BeforeNgxD3D12CreateFeature, &BeforeNgxD3D12EvaluateFeature);
}

bool TryInstallNgxRuntimeCreateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime)) return false;
    return ampere_backend::InstallRoute(runtime, path.c_str(), generation, true,
        &BeforeNgxRuntimeD3D12CreateFeature, &BeforeNgxRuntimeD3D12EvaluateFeature);
}

bool TryInstallNgxRuntimeEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime)) return false;
    return ampere_backend::InstallRoute(runtime, path.c_str(), generation, true,
        &BeforeNgxRuntimeD3D12CreateFeature, &BeforeNgxRuntimeD3D12EvaluateFeature);
}

void WINAPI BeforeNgxVulkanCreateFeature(void* commandBuffer,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan, commandBuffer,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanCreateFeature(void* commandBuffer,
    uintptr_t feature, const void* parameters, void* handle,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan, commandBuffer,
        static_cast<NVSDK_NGX_Feature>(feature),
        static_cast<const NVSDK_NGX_Parameter*>(parameters),
        static_cast<NVSDK_NGX_Handle**>(handle), entryHandle,
        originalCaller);
}

// CreateFeature1 places VkDevice, VkCommandBuffer, feature and parameters in
// the four register arguments; the output handle is the untouched fifth stack
// argument. The forwarding relay preserves it while this pre-call reads only
// the feature and parameters.
void WINAPI BeforeNgxVulkanCreateFeature1(void*, uintptr_t commandBuffer,
    const void* featureValue, void* parameters, uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan,
        reinterpret_cast<void*>(commandBuffer),
        static_cast<NVSDK_NGX_Feature>(
            reinterpret_cast<uintptr_t>(featureValue)),
        static_cast<const NVSDK_NGX_Parameter*>(parameters), nullptr,
        entryHandle, originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanCreateFeature1(void*,
    uintptr_t commandBuffer, const void* featureValue, void* parameters,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxCreateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan,
        reinterpret_cast<void*>(commandBuffer),
        static_cast<NVSDK_NGX_Feature>(
            reinterpret_cast<uintptr_t>(featureValue)),
        static_cast<const NVSDK_NGX_Parameter*>(parameters), nullptr,
        entryHandle, originalCaller);
}

NVSDK_NGX_Result NVSDK_CONV RejectUnsupportedVulkanCall(void*, uintptr_t, const void*, void*) noexcept
{
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

bool WINAPI VulkanCreateGate(void* argument1, uintptr_t argument2, const void* argument3, void* argument4,
    uintptr_t argument5, uintptr_t argument6, entry_detour::Handle handle, const void* caller) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    // On RTX 40/50 an entry another hook covers still forwards natively.
    if (!entry.current && !(entry.overlaid && !UseAmpere() && !gpu_dispatch::UsesTranslatedProgram())) return false;
    if (!entry.current) ampere_backend::ReportOverlaidEntry(entry);
    const bool version1 = entry.kind == entry_detour::Kind::eNgxVulkanCreateFeature1
        || entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1;
    const uintptr_t feature = version1 ? reinterpret_cast<uintptr_t>(argument3) : argument2;
    if (UseAmpere())
    {
        // RTX 30: a runtime Create diverts to the Ampere route's thunk (this
        // entry's rejected target); the nested provider Create is admitted by it.
        const bool runtime = entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
            || entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1;
        if (runtime || !ampere_backend::VulkanProviderGate(argument1, argument2, argument3, argument4,
                argument5, argument6, handle, caller))
            return false;
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
        // That build clears the entry's pre-call; run it after admission.
        (version1 ? &BeforeNgxVulkanCreateFeature1 : &BeforeNgxVulkanCreateFeature)(argument1, argument2,
            argument3, argument4, argument5, argument6, handle, caller);
#endif
        return true;
    }
    if (!gpu_dispatch::AllowVulkanCreate(gpu_dispatch::Selected(),
            feature == NVSDK_NGX_Feature_FrameGeneration,
            gVulkanAdapterVerified.load(std::memory_order_acquire)))
        return false;
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    // A forwarding gate runs before its pre-call callback. Run preparation
    // inside this gate so an unsafe publication rejects this same native call.
    entry_detour::ForwardPreCall before = nullptr;
    switch (entry.kind)
    {
    case entry_detour::Kind::eNgxVulkanCreateFeature:
        before = &BeforeNgxVulkanCreateFeature; break;
    case entry_detour::Kind::eNgxRuntimeVulkanCreateFeature:
        before = &BeforeNgxRuntimeVulkanCreateFeature; break;
    case entry_detour::Kind::eNgxVulkanCreateFeature1:
        before = &BeforeNgxVulkanCreateFeature1; break;
    case entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1:
        before = &BeforeNgxRuntimeVulkanCreateFeature1; break;
    default:
        return false;
    }
    before(argument1, argument2, argument3, argument4,
        argument5, argument6, handle, caller);
    if (feature == NVSDK_NGX_Feature_FrameGeneration
        && midpoint_fix::OutputPullMaskRequiresRestart())
        return false;
#endif
    // RTX 40/50: a runtime FG, SR or RR Create runs through
    // its route's thunk (FG lifetime, SR/RR presets) instead of the relay.
    const bool runtimeEntry = entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
        || entry.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1;
    if (runtimeEntry && ampere_backend::DivertNativeVulkanCall(handle, true, feature))
        return false;
    // An FG Create no tracked runtime Create spans leaves the tracker inexact.
    if (feature == NVSDK_NGX_Feature_FrameGeneration)
        ampere_backend::ObserveProviderFrameGenerationCreate();
    return true;
}

bool WINAPI VulkanEvaluateGate(void* argument1, uintptr_t argument2, const void* argument3, void* argument4,
    uintptr_t argument5, uintptr_t argument6, entry_detour::Handle handle, const void* caller) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    // RTX 30: as for Create, the runtime diverts and the provider is admitted
    // only inside the Ampere route's tracked runtime Evaluate.
    if (UseAmpere())
        return entry.current && entry.kind == entry_detour::Kind::eNgxVulkanEvaluateFeature
            && ampere_backend::VulkanProviderGate(argument1, argument2, argument3, argument4,
                argument5, argument6, handle, caller);
    // As in VulkanCreateGate, an entry another hook covers forwards natively.
    const bool overlaid = !entry.current && entry.overlaid && !gpu_dispatch::UsesTranslatedProgram();
    if (overlaid) ampere_backend::ReportOverlaidEntry(entry);
    const bool allowed = (entry.current || overlaid)
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
        && (entry.kind != entry_detour::Kind::eNgxVulkanEvaluateFeature
            || !midpoint_fix::OutputPullMaskRequiresRestart())
#endif
        && gpu_dispatch::AllowVulkanEvaluate(gpu_dispatch::Selected(),
        entry.kind == entry_detour::Kind::eNgxVulkanEvaluateFeature,
        gVulkanAdapterVerified.load(std::memory_order_acquire));
    // RTX 40/50: while an SR/RR feature is tracked, runtime
    // Evaluates run through the route's thunk, which can substitute our
    // re-created feature for the game's.
    if (allowed && entry.kind == entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature
        && ampere_backend::DivertNativeVulkanCall(handle, false, 0))
        return false;
    return allowed;
}

bool InstallNgxVulkanCreateEntry(HMODULE owner,
    const std::wstring& path, uint64_t generation, const char* exportName,
    entry_detour::Kind kind, entry_detour::ForwardPreCall callback,
    bool featureIsSecondArgument)
{
    void* target = reinterpret_cast<void*>(
        GetProcAddress(owner, exportName));
    if (!target)
        return false;
    entry_detour::InstallOptions options{};
    options.generation = generation;
    options.allowRelocated = true;
    (void)featureIsSecondArgument; // The gate decodes both public Create ABIs.
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    callback = nullptr; // The gate prepares exactly once, then checks publication.
#endif
    // A runtime entry's refusal target is its RTX 30 route thunk (it refuses as
    // RejectUnsupportedVulkanCall does whenever RTX 30 is not selected).
    entry_detour::Handle handle{};
    const bool runtime = kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
        || kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1;
    const bool installed = ampere_backend::InstallVulkanEntry(owner, generation, runtime, kind,
        target, callback, &VulkanCreateGate, reinterpret_cast<void*>(&RejectUnsupportedVulkanCall),
        options, &handle);
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(handle);
    LogOnChange(L"NGX Vulkan %hs entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s", exportName, installed, state.current,
        state.cachedPointersCovered, entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxVulkanCreateEntryDetours(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    if (!provider
        || !dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str()))
        return false;
    const bool create = InstallNgxVulkanCreateEntry(provider, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature",
        entry_detour::Kind::eNgxVulkanCreateFeature,
        &BeforeNgxVulkanCreateFeature, true);
    const bool create1 = InstallNgxVulkanCreateEntry(provider, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature1",
        entry_detour::Kind::eNgxVulkanCreateFeature1,
        &BeforeNgxVulkanCreateFeature1, false);
    return create || create1;
}

bool TryInstallNgxRuntimeVulkanCreateEntryDetours(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    if (!IsNgxRuntimeModule(runtime))
        return false;
    const bool create = InstallNgxVulkanCreateEntry(runtime, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature",
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature,
        &BeforeNgxRuntimeVulkanCreateFeature, true);
    const bool create1 = InstallNgxVulkanCreateEntry(runtime, path,
        generation, "NVSDK_NGX_VULKAN_CreateFeature1",
        entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1,
        &BeforeNgxRuntimeVulkanCreateFeature1, false);
    return create || create1;
}

void WINAPI BeforeNgxVulkanEvaluateFeature(void* commandBuffer,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eProvider,
        NgxGraphicsApi::eVulkan, commandBuffer, handle, parameters,
        callback, entryHandle, originalCaller);
}

void WINAPI BeforeNgxRuntimeVulkanEvaluateFeature(void* commandBuffer,
    uintptr_t handle, const void* parameters, void* callback,
    uintptr_t, uintptr_t,
    entry_detour::Handle entryHandle,
    const void* originalCaller) noexcept
{
    BeforeNgxEvaluateFeatureForRoute(NgxDispatchRoute::eRuntime,
        NgxGraphicsApi::eVulkan, commandBuffer, handle, parameters,
        callback, entryHandle, originalCaller);
}

bool InstallNgxVulkanEvaluateEntry(HMODULE owner,
    const std::wstring& path, uint64_t generation,
    entry_detour::Kind kind, entry_detour::ForwardPreCall callback)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(
        owner, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    if (!target)
        return false;
    entry_detour::InstallOptions options{};
    options.generation = generation;
    options.allowRelocated = true;
    entry_detour::Handle handle{};
    const bool installed = ampere_backend::InstallVulkanEntry(owner, generation,
        kind == entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature, kind,
        target, callback, &VulkanEvaluateGate, reinterpret_cast<void*>(&RejectUnsupportedVulkanCall),
        options, &handle);
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(handle);
    LogOnChange(L"NGX Vulkan EvaluateFeature entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d method=%hs failure=%u "
        L"targetRva=0x%X path=%s", installed, state.current,
        state.cachedPointersCovered, entry_detour::MethodName(state.method),
        static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
    return installed;
}

bool TryInstallNgxVulkanEvaluateEntryDetour(
    HMODULE provider, const std::wstring& path, uint64_t generation)
{
    return provider
        && dlssg_provider_policy::IsSupportedProvider(
            provider, path.c_str())
        && InstallNgxVulkanEvaluateEntry(provider, path, generation,
            entry_detour::Kind::eNgxVulkanEvaluateFeature,
            &BeforeNgxVulkanEvaluateFeature);
}

bool TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
    HMODULE runtime, const std::wstring& path, uint64_t generation)
{
    return IsNgxRuntimeModule(runtime)
        && InstallNgxVulkanEvaluateEntry(runtime, path, generation,
            entry_detour::Kind::eNgxRuntimeVulkanEvaluateFeature,
            &BeforeNgxRuntimeVulkanEvaluateFeature);
}

void PrepareVulkanProviderCapabilities(entry_detour::Handle handle)
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    if (!entry.current || !gpu_dispatch::IsAda()
        || !gVulkanAdapterVerified.load(std::memory_order_acquire)
        || !dlssg_provider_policy::IsSupportedRetainedProvider(entry.owner))
        return;
    // Only the provider actually entered by NGX may select the temporal
    // program. Passive discovery can also see an unused bundled provider
    // while NGX selects a different NVIDIA override image.
    const auto path = LoadedModulePath(entry.owner);
    const auto record = InspectLoadedModule(entry.owner, path);
    const auto create = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxVulkanCreateFeature, entry.owner);
    const auto evaluate = entry_detour::ReadSnapshot(
        entry_detour::Kind::eNgxVulkanEvaluateFeature, entry.owner);
    if (!record.ngxPatched || !create.current || !evaluate.current
        || entry.generation != record.generation
        || create.generation != record.generation || evaluate.generation != record.generation
        || !gpu_backend::PatchProvider(entry.owner, path.c_str()))
        return;
    const auto capability = vulkan_capability::Find(entry.owner);
    if (capability.threshold && *capability.threshold != 0x90)
    {
        const bool published = vulkan_capability::Publish(capability);
        Log(L"Vulkan DLSS-G capability maximum: published=%d generatedFrames=5 "
            L"targetRva=0x%X path=%s", published, capability.rva, path.c_str());
    }
    else if (!capability.threshold)
    {
        // Without the recognized 1/5 decision the game is offered 2x on Vulkan.
        static std::atomic<HMODULE> reported{nullptr};
        if (reported.exchange(entry.owner, std::memory_order_relaxed) != entry.owner)
            Log(L"Vulkan DLSS-G capability maximum not published: decision matches=%u (expected 1); "
                L"generated frames stay at the provider's value path=%s", capability.count, path.c_str());
    }
}

void WINAPI BeforeNgxVulkanInit(void*, uintptr_t, const void* instance,
    void* physicalDevice, uintptr_t device, uintptr_t, entry_detour::Handle handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        // The DLSS 5 Vulkan stage's device and its direct provider route.
        dlss5::vulkan::ObserveInit(const_cast<void*>(instance), physicalDevice, reinterpret_cast<void*>(device));
        if (NativeDlssgPresetGpu()) dlssg_preset::Prepare();
        // The SR/RR DRS hook, before NVIDIA's Vulkan Init reads those keys.
        dlss_sr::PrepareForVulkan();
        // As slSetD3DDevice does: the real-frame readout needs the interval trace.
        temporal_interval_trace::SetEnabled(true);
        // The base Init names its VkDevice (fifth argument).
        if (UseAmpere()) ampere_backend::ObserveVulkanDevice(reinterpret_cast<void*>(device), false);
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
        ObserveOutputPullMaskVulkanInit(handle);
        PrepareVulkanProviderCapabilities(handle);
    }
}

void WINAPI BeforeNgxVulkanProjectInit(void*, uintptr_t, const void*,
    void*, uintptr_t instance, uintptr_t physicalDevice, entry_detour::Handle handle,
    const void*) noexcept
{
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(
            reinterpret_cast<void*>(physicalDevice));
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        // The DLSS 5 Vulkan stage's instance and adapter (the project
        // Init's VkDevice is its seventh argument; the game's CreateFeature1 names it).
        dlss5::vulkan::ObserveInit(reinterpret_cast<void*>(instance), reinterpret_cast<void*>(physicalDevice), nullptr);
        if (NativeDlssgPresetGpu()) dlssg_preset::Prepare();
        // The SR/RR DRS hook, before NVIDIA's Vulkan Init reads those keys.
        dlss_sr::PrepareForVulkan();
        temporal_interval_trace::SetEnabled(true);
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
        ObserveOutputPullMaskVulkanInit(handle);
        PrepareVulkanProviderCapabilities(handle);
    }
}

bool TryInstallNgxVulkanAdapterEntryDetours(HMODULE module,
    const std::wstring& path, uint64_t generation)
{
    // RTX 30 Vulkan hosts are observed here as well: NGX Init names the
    // VkPhysicalDevice (and, for the base Init, the VkDevice).
    if (!module || !ExportsNgxVulkanRoute(module))
        return false;
    struct AdapterEntry
    {
        const char* name;
        entry_detour::Kind kind;
        entry_detour::ForwardPreCall callback;
    };
    const AdapterEntry entries[] = {
        {"NVSDK_NGX_VULKAN_Init",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_Ext",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_Ext2",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanInit},
        {"NVSDK_NGX_VULKAN_Init_with_ProjectID",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
        {"NVSDK_NGX_VULKAN_Init_ProjectID",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
        {"NVSDK_NGX_VULKAN_Init_ProjectID_Ext",
            entry_detour::Kind::eNgxVulkanAdapterInit,
            &BeforeNgxVulkanProjectInit},
    };
    bool installedAny = false;
    for (const AdapterEntry& entry : entries)
    {
        void* target = reinterpret_cast<void*>(
            GetProcAddress(module, entry.name));
        if (!target)
            continue;
        // Some providers alias the base D3D12 and Vulkan Init exports. Their
        // fourth arguments have different meanings, so observe the base
        // Vulkan entry only when it is a distinct implementation. Ext/Ext2
        // and project-ID entries carry an unambiguous VkPhysicalDevice.
        if (std::strcmp(entry.name, "NVSDK_NGX_VULKAN_Init") == 0
            && target == reinterpret_cast<void*>(GetProcAddress(
                module, "NVSDK_NGX_D3D12_Init")))
            continue;
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* trampoline = nullptr;
        entry_detour::Handle handle{};
        const bool installed = entry_detour::InstallForwarding(
            entry.kind, module, target, entry.callback, trampoline,
            options, &handle);
        const entry_detour::Snapshot state =
            entry_detour::ReadSnapshot(handle);
        LogOnChange(L"NGX Vulkan adapter entry %hs: installed=%d current=%d "
            L"method=%hs failure=%u targetRva=0x%X path=%s",
            entry.name, installed, state.current,
            entry_detour::MethodName(state.method),
            static_cast<uint32_t>(state.failure), state.targetRva,
            path.c_str());
        installedAny = installedAny || installed;
    }
    return installedAny;
}

bool CopySupportedOptions(const sl::DLSSGOptions* source,
    sl::DLSSGOptions& destination, bool preserveNext) noexcept
{
    if (!source)
        return false;
    __try
    {
        if (source->structType != sl::DLSSGOptions::s_structType
            || !IsSupportedOptionsVersion(source->structVersion))
            return false;
        destination = CopyKnownOptions(*source, preserveNext);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadViewportValue(const sl::ViewportHandle* viewport,
    uint32_t& value) noexcept
{
    if (!viewport)
        return false;
    __try
    {
        value = static_cast<uint32_t>(*viewport);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

sl::DLSSGOptions AmpereFixedOptions(const sl::DLSSGOptions& source, bool preserveNext)
{
    return BuildAdjustedOptions(source, ReadControlSnapshot(), preserveNext);
}


sl::Result PassPublicSet(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options) noexcept
{
    auto* original = EntryOriginal(
        route.publicSetOriginal, route.publicSetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    ScopedInternalControlBypass bypass;
    if (UseAmpere())
    {
        sl::DLSSGOptions known{};
        const sl::DLSSGOptions* found = nullptr;
        size_t version = 0;
        if (FindStructBounded(&options, found, version) != ChainFindStatus::eFound || found != &options)
            return sl::Result::eErrorInvalidParameter;
        if (!CopySupportedOptions(&options, known, true)) return sl::Result::eErrorInvalidParameter;
        const sl::DLSSGOptions fixed = AmpereFixedOptions(known, true);
        return original(viewport, fixed);
    }
    else if (AdaProviderWithoutGate())
    {
        // GitHub issue 149: a provider without the count/index check takes one generated frame only.
        sl::DLSSGOptions known{};
        const sl::DLSSGOptions* found = nullptr;
        size_t version = 0;
        if (FindStructBounded(&options, found, version) != ChainFindStatus::eFound || found != &options)
            return sl::Result::eErrorInvalidParameter;
        if (!CopySupportedOptions(&options, known, true)) return sl::Result::eErrorInvalidParameter;
        if (known.numFramesToGenerate <= 1)
            return original(viewport, options);
        known.numFramesToGenerate = 1;
        return original(viewport, known);
    }
    else
    {
        return original(viewport, options);
    }
}

// Before the control path is ready, game options pass through unchanged.
sl::Result PassPublicSetBeforeControl(ControlRouteRecord& route,
    const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) noexcept
{
    return PassPublicSet(route, viewport, options);
}

sl::Result HandlePublicSetOptions(uint32_t routeSlot,
    ControlEntryPath path, const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    auto* original = route ? EntryOriginal(
        route->publicSetOriginal, route->publicSetHandle) : nullptr;
    if (!original)
        return sl::Result::eErrorNotInitialized;
    // A second route's entry reached from our own forwarded call. The outer
    // call owns gStreamlineCallMutex and the route choice; forward unchanged.
    if (InternalControlBypassDepth() != 0)
        return original(viewport, options);

    BaseStructureFields fields{};
    if (!ReadBaseStructureFields(&options, fields)
        || fields.type != sl::DLSSGOptions::s_structType)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return PassPublicSet(*route, viewport, options);
    }
    if (!IsSupportedOptionsVersion(fields.version))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return PassPublicSet(*route, viewport, options);
    }
    if (!ActivateControlRoute(routeSlot, path, true))
        return PassPublicSet(*route, viewport, options);

    std::lock_guard callLock(gStreamlineCallMutex);
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    if (path == ControlEntryPath::eResolver)
    {
        gSetOptionsResolverFallbackCalls.fetch_add(
            1, std::memory_order_release);
    }

    const bool enabled = options.mode == sl::DLSSGMode::eOn
        || options.mode == sl::DLSSGMode::eAuto
        || options.mode == sl::DLSSGMode::eDynamic;
    RecordGameFrameGenerationIntent(viewport, enabled);
    // XeMFG standalone replaces the game's DLSS Frame Generation. The game's switch (recorded above)
    // turns XeMFG on and off, and DLSS-G itself gets Off, so two frame generators never stack.
    if (enabled && XemfgReplacesDlssg())
    {
        sl::DLSSGOptions off{};
        if (!CopySupportedOptions(&options, off, true))
            return PassPublicSet(*route, viewport, options);
        off.mode = sl::DLSSGMode::eOff;
        // xemfglive.6: a live move to XeMFG waits for the exact release of DLSS-G's feature, as a live preset
        // switch's drain does (ApplyPresetSwitchOff): Off without resource retention.
        if (xess_d3d12::DlssgReleaseWanted())
            off.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(off.flags)
                & ~static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff));
        gSetOptionsSeen.store(true, std::memory_order_release);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            Log(L"XeMFG standalone: the game's DLSS Frame Generation is held at Off; its switch turns XeMFG on and off");
        // Straight to the provider: PassPublicSet's RTX 30 options would follow the fixed multiplier instead.
        ScopedInternalControlBypass bypass;
        const sl::Result result = original(viewport, off);
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, viewport, false, result);
        return HostControlResult(result);
    }
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        // xemfglive.6: the game's own Off releases DLSS-G's feature too while a live move waits for that release.
        sl::DLSSGOptions released{};
        const bool release = xess_d3d12::DlssgReleaseWanted()
            && CopySupportedOptions(&options, released, true);
        if (release)
            released.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(released.flags)
                & ~static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff));
        const sl::Result result = PassPublicSet(*route, viewport, release ? released : options);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, viewport, false, result);
        return HostControlResult(result);
    }

    CaptureGameOptions(viewport, options);
    AdvancePresetSwitch(viewport, false);
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = PassPublicSetBeforeControl(*route, viewport, options);
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        // A held request reached Streamline as Off.
        RecordSetOptionsLifecycle(*route, viewport, !AmpereAdmissionHeld(), result);
        if (route->wrapperPatched && IsAcceptedControlResult(result)
            && !gFrameGenerationCreateObserved.load(
                std::memory_order_acquire)
            && (!UseAmpere() || !ampere_backend::EarlyInitObserved())
            )
        {
            gPipelineMayPredateDetour.store(true,
                std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
        }
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return HostControlResult(result);
    }

    return SubmitAdjustedOptions(*route, viewport, options,
        ReadControlSnapshot(), false);
}

sl::Result HandlePublicGetState(uint32_t routeSlot, ControlEntryPath path,
    const sl::ViewportHandle& viewport, sl::DLSSGState& state,
    const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->publicGetOriginal, route->publicGetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    // See HandlePublicSetOptions: a nested entry from our own forwarded call.
    if (InternalControlBypassDepth() != 0)
        return original(viewport, state, options);

    const auto passThrough = [&](bool record = true) {
        if (UseAmpere())
        {
            const sl::DLSSGState* known = nullptr;
            size_t version = 0;
            if (FindStructBounded(&state, known, version) != ChainFindStatus::eFound
                || !IsSupportedStateVersion(version)) return sl::Result::eErrorInvalidParameter;
            sl::DLSSGOptions query{};
            if (options)
            {
                const sl::DLSSGOptions* found = nullptr;
                if (FindStructBounded(options, found, version) != ChainFindStatus::eFound
                    || !CopySupportedOptions(options, query, true)) return sl::Result::eErrorInvalidParameter;
                query = AmpereFixedOptions(query, true);
            }
            const auto result = original(viewport, state, options ? &query : nullptr);
            if (DlssgStateAvailable(result) && state.structVersion >= sl::kStructVersion2)
            {
                state.numFramesToGenerateMax = std::min(state.numFramesToGenerateMax, AmpereAdvertisedMaximum());
            }
            if (record) RecordDlssgStateResult(result, state);
            return result;
        }
        else
        {
            const auto result = original(viewport, state, options);
            // GitHub issue 149: the game offers no mode above 2X with a provider that lacks the count/index check.
            if (AdaProviderWithoutGate() && DlssgStateAvailable(result) && state.structVersion >= sl::kStructVersion2)
                state.numFramesToGenerateMax = std::min<uint32_t>(state.numFramesToGenerateMax, 1u);
            return result;
        }
    };

    BaseStructureFields stateFields{};
    if (!ReadBaseStructureFields(&state, stateFields)
        || stateFields.type != sl::DLSSGState::s_structType)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    if (!IsSupportedStateVersion(stateFields.version))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownStateVersion);
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    if (options)
    {
        BaseStructureFields optionFields{};
        if (!ReadBaseStructureFields(options, optionFields)
            || optionFields.type != sl::DLSSGOptions::s_structType)
        {
            InvalidateControlRoute(routeSlot,
                UniversalRouteFailure::eMalformedStructureChain);
            ScopedInternalControlBypass bypass;
            return passThrough();
        }
        if (!IsSupportedOptionsVersion(optionFields.version))
        {
            InvalidateControlRoute(routeSlot,
                UniversalRouteFailure::eUnknownOptionsVersion);
            ScopedInternalControlBypass bypass;
            return passThrough();
        }
    }
    if (!ActivateControlRoute(routeSlot, path, false))
    {
        ScopedInternalControlBypass bypass;
        return passThrough();
    }
    std::lock_guard callLock(gStreamlineCallMutex);
    if (path == ControlEntryPath::eResolver)
    {
        gGetStateResolverFallbackCalls.fetch_add(
            1, std::memory_order_release);
    }
    AdvancePresetSwitch(viewport, true);
    ReapplyPendingControl(viewport);
    ScopedInternalControlBypass bypass;
    const sl::Result result = passThrough(false);
    RecordDlssgStateResult(result, state);
    return HostControlResult(result);
}

sl::Result HandleInternalSetData(uint32_t routeSlot,
    const sl::BaseStructure* inputs, sl::CommandBuffer* commandBuffer)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->internalSetOriginal, route->internalSetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (InternalControlBypassDepth() != 0)
        return original(inputs, commandBuffer);
    const auto passThrough = [&]() {
        ScopedInternalControlBypass bypass;
        if (UseAmpere())
        {
            const sl::DLSSGOptions* requested = nullptr;
            const sl::ViewportHandle* selectedViewport = nullptr;
            size_t optionsVersion = 0, viewportVersion = 0;
            const auto shape = FindStructBounded(inputs, requested, optionsVersion);
            if (shape == ChainFindStatus::eMalformed) return sl::Result::eErrorInvalidParameter;
            if (shape == ChainFindStatus::eFound)
            {
                sl::DLSSGOptions known{};
                uint32_t viewportValue = 0;
                if (!CopySupportedOptions(requested, known, false)
                    || FindStructBounded(inputs, selectedViewport, viewportVersion) != ChainFindStatus::eFound
                    || !ReadViewportValue(selectedViewport, viewportValue)) return sl::Result::eErrorInvalidParameter;
                sl::DLSSGOptions fixed = AmpereFixedOptions(known, false);
                sl::ViewportHandle viewportCopy{viewportValue};
                viewportCopy.next = &fixed;
                fixed.next = const_cast<sl::BaseStructure*>(inputs);
                return original(&viewportCopy, commandBuffer);
            }
        }
        return original(inputs, commandBuffer);
    };

    const sl::DLSSGOptions* options = nullptr;
    const sl::ViewportHandle* viewport = nullptr;
    size_t optionsVersion = 0;
    size_t viewportVersion = 0;
    const ChainFindStatus optionsStatus = FindStructBounded(
        inputs, options, optionsVersion);
    const ChainFindStatus viewportStatus = FindStructBounded(
        inputs, viewport, viewportVersion);
    const universal_route_policy::AdapterAction action =
        universal_route_policy::ClassifyInternalSet(
            optionsStatus, viewportStatus, optionsVersion);
    if (action == universal_route_policy::AdapterAction::ePassThrough)
        return passThrough();
    if (action == universal_route_policy::AdapterAction::eRejectMalformed)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownOptions)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return passThrough();
    }
    sl::DLSSGOptions source{};
    uint32_t viewportValue = 0;
    if (!CopySupportedOptions(options, source, false)
        || !ReadViewportValue(viewport, viewportValue))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (!ActivateControlRoute(
            routeSlot, ControlEntryPath::eInternal, true))
        return passThrough();
    std::lock_guard callLock(gStreamlineCallMutex);
    gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed);
    const sl::ViewportHandle publicViewport{viewportValue};
    const bool enabled = source.mode == sl::DLSSGMode::eOn
        || source.mode == sl::DLSSGMode::eAuto
        || source.mode == sl::DLSSGMode::eDynamic;
    RecordGameFrameGenerationIntent(publicViewport, enabled);
    // As HandlePublicSetOptions, XeMFG standalone holds DLSS-G at Off (our copy leads the chain).
    if (enabled && XemfgReplacesDlssg())
    {
        sl::DLSSGOptions off = CopyKnownOptions(source, false);
        off.mode = sl::DLSSGMode::eOff;
        sl::ViewportHandle viewportCopy{viewportValue};
        viewportCopy.next = &off;
        off.next = const_cast<sl::BaseStructure*>(inputs);
        gSetOptionsSeen.store(true, std::memory_order_release);
        sl::Result result;
        {
            ScopedInternalControlBypass bypass;
            result = original(&viewportCopy, commandBuffer);
        }
        gLastSetOptionsResult.store(static_cast<int32_t>(result), std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, publicViewport, false, result);
        return HostControlResult(result);
    }
    if (!enabled)
    {
        gSetOptionsSeen.store(true, std::memory_order_release);
        const sl::Result result = passThrough();
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        RecordSetOptionsLifecycle(*route, publicViewport, false, result);
        return HostControlResult(result);
    }

    CaptureGameOptions(publicViewport, source);
    AdvancePresetSwitch(publicViewport, false);
    // As PassPublicSetBeforeControl: the game's options pass through unchanged.
    const auto passThroughBeforeControl = [&]() {
        return passThrough();
    };
    if (!gControlReady.load(std::memory_order_acquire)
        || !BridgeReady())
    {
        const sl::Result result = passThroughBeforeControl();
        gSetOptionsSeen.store(true, std::memory_order_release);
        gLastSetOptionsResult.store(static_cast<int32_t>(result),
            std::memory_order_relaxed);
        // A held request reached Streamline as Off.
        RecordSetOptionsLifecycle(*route, publicViewport, !AmpereAdmissionHeld(), result);
        if (!IsAcceptedControlResult(result) || !BridgeReady())
            return HostControlResult(result);
    }

    auto submit = [&](const sl::DLSSGOptions& adjustedSource) {
        sl::ViewportHandle viewportCopy{viewportValue};
        sl::DLSSGOptions adjusted = CopyKnownOptions(
            adjustedSource, false);
        viewportCopy.next = &adjusted;
        adjusted.next = const_cast<sl::BaseStructure*>(inputs);
        ScopedInternalControlBypass bypass;
        return original(&viewportCopy, commandBuffer);
    };
    auto query = [&](sl::DLSSGState& state,
                         const sl::DLSSGOptions* queryOptions) {
        return CallRouteGetState(
            *route, publicViewport, state, queryOptions);
    };
    return SubmitAdjustedOptionsImpl(*route, publicViewport, source,
        ReadControlSnapshot(), false, submit, query,
        RouteHasStateFunction(*route));
}

sl::Result HandleInternalGetData(uint32_t routeSlot,
    const sl::BaseStructure* inputs, sl::BaseStructure* outputs,
    sl::CommandBuffer* commandBuffer)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->internalGetOriginal, route->internalGetHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (InternalControlBypassDepth() != 0)
        return original(inputs, outputs, commandBuffer);
    const auto passThrough = [&](bool record = true) {
        ScopedInternalControlBypass bypass;
        if (UseAmpere())
        {
            const sl::DLSSGState* known = nullptr;
            size_t version = 0;
            const auto shape = FindStructBounded(outputs, known, version);
            if (shape == ChainFindStatus::eMalformed || (shape == ChainFindStatus::eFound
                && !IsSupportedStateVersion(version))) return sl::Result::eErrorInvalidParameter;
            const auto result = original(inputs, outputs, commandBuffer);
            if (known)
            {
                auto& state = *const_cast<sl::DLSSGState*>(known);
                if (DlssgStateAvailable(result) && state.structVersion >= sl::kStructVersion2)
                    state.numFramesToGenerateMax = std::min(state.numFramesToGenerateMax, AmpereAdvertisedMaximum());
                if (record) RecordDlssgStateResult(result, state);
            }
            return result;
        }
        else
        {
            return original(inputs, outputs, commandBuffer);
        }
    };

    const sl::DLSSGState* stateView = nullptr;
    const sl::DLSSGOptions* optionsView = nullptr;
    const sl::ViewportHandle* viewport = nullptr;
    size_t stateVersion = 0;
    size_t viewportVersion = 0;
    const ChainFindStatus stateStatus = FindStructBounded(
        outputs, stateView, stateVersion);
    const ChainFindStatus viewportStatus = FindStructBounded(
        inputs, viewport, viewportVersion);
    size_t optionsVersion = 0;
    const ChainFindStatus optionsStatus = FindStructBounded(
        inputs, optionsView, optionsVersion);
    const universal_route_policy::AdapterAction action =
        universal_route_policy::ClassifyInternalGet(
            stateStatus, viewportStatus, stateVersion,
            optionsStatus, optionsVersion);
    if (action == universal_route_policy::AdapterAction::ePassThrough)
        return passThrough();
    if (action == universal_route_policy::AdapterAction::eRejectMalformed)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownState)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownStateVersion);
        return passThrough();
    }
    if (action
        == universal_route_policy::AdapterAction::eRejectUnknownOptions)
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eUnknownOptionsVersion);
        return passThrough();
    }
    uint32_t viewportValue = 0;
    if (!ReadViewportValue(viewport, viewportValue))
    {
        InvalidateControlRoute(routeSlot,
            UniversalRouteFailure::eMalformedStructureChain);
        return passThrough();
    }
    if (!ActivateControlRoute(
            routeSlot, ControlEntryPath::eInternal, false))
        return passThrough();
    std::lock_guard callLock(gStreamlineCallMutex);
    const sl::ViewportHandle publicViewport{viewportValue};
    AdvancePresetSwitch(publicViewport, true);
    ReapplyPendingControl(publicViewport);
    const sl::Result result = passThrough(false);
    auto& mutableState = *const_cast<sl::DLSSGState*>(stateView);
    RecordDlssgStateResult(result, mutableState);
    return HostControlResult(result);
}

sl::Result HandleFreeResources(uint32_t routeSlot, sl::Feature feature,
    const sl::ViewportHandle& viewport)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route)
        return sl::Result::eErrorNotInitialized;
    auto* original = EntryOriginal(
        route->freeResourcesOriginal, route->freeResourcesHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    AcceptedOffEvidence expected{};
    if (feature == sl::kFeatureDLSS_G)
    {
        std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
        if (gAcceptedOffEvidence.routeSlot == routeSlot
            && gAcceptedOffEvidence.viewport
                == static_cast<uint32_t>(viewport))
            expected = gAcceptedOffEvidence;
    }
    const sl::Result result = original(feature, viewport);
    route->releaseCalls.fetch_add(1, std::memory_order_relaxed);
    if (feature == sl::kFeatureDLSS_G && IsAcceptedControlResult(result))
    {
        InvalidateUiInputEvidence(static_cast<uint32_t>(viewport));
        if (static_cast<uint32_t>(viewport)
            == gLastOptionsViewport.load(std::memory_order_acquire))
        {
            gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
            gAppliedUiRecompositionForced.store(false, std::memory_order_release);
        }
    }
    if (expected.routeSlot != UINT32_MAX
        && IsAcceptedControlResult(result))
    {
        if (!expected.feature)
        {
            // Ada exposes no exact post-ReleaseFeature identity. The matching
            // accepted free is therefore a one-way safety boundary even if an
            // On call raced with the native free: do not reapply to a possibly
            // released lifetime from polling.
            gAppliedRevision.store(0, std::memory_order_release);
            gAttemptedRevision.store(0, std::memory_order_release);
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Matching Ada DLSS-G resources were freed after accepted "
                L"Off; exact feature recreation is unavailable, restart required");
            return result;
        }
        std::lock_guard lifetimeLock(gFrameGenerationLifetimeMutex);
        if (SameRetainedFeature(expected.feature,
                gRetainedFrameGenerationFeature)
            && !route->releaseObserved.load(std::memory_order_acquire))
        {
            // slFreeResources success is not itself proof that this exact NGX
            // feature reached ReleaseFeature.  A matching post-call observer
            // may already have retired it from inside the native call; absent
            // that evidence, keep ownership and require a known recreation.
            gRestartRequired.store(true, std::memory_order_release);
            Log(L"Matching DLSS-G resources were freed without an exact "
                L"ReleaseFeature boundary; retained feature %p remains "
                L"restricted until restart/recreation",
                reinterpret_cast<void*>(expected.feature.handle));
        }
    }
    return result;
}

template <size_t Slot>
sl::Result PublicSetThunk(const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options)
{
    ControlRouteRecord* route = ControlRouteAt(static_cast<uint32_t>(Slot));
    const ControlEntryPath path = route
            && route->publicSetResolverFallback.load(
                std::memory_order_acquire)
        ? ControlEntryPath::eResolver : ControlEntryPath::ePublic;
    return HandlePublicSetOptions(static_cast<uint32_t>(Slot),
        path, viewport, options);
}

template <size_t Slot>
sl::Result PublicGetThunk(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ControlRouteAt(static_cast<uint32_t>(Slot));
    const ControlEntryPath path = route
            && route->publicGetResolverFallback.load(
                std::memory_order_acquire)
        ? ControlEntryPath::eResolver : ControlEntryPath::ePublic;
    return HandlePublicGetState(static_cast<uint32_t>(Slot),
        path, viewport, state, options);
}

template <size_t Slot>
sl::Result InternalSetThunk(const sl::BaseStructure* inputs,
    sl::CommandBuffer* commandBuffer)
{
    return HandleInternalSetData(
        static_cast<uint32_t>(Slot), inputs, commandBuffer);
}

template <size_t Slot>
sl::Result InternalGetThunk(const sl::BaseStructure* inputs,
    sl::BaseStructure* outputs, sl::CommandBuffer* commandBuffer)
{
    return HandleInternalGetData(static_cast<uint32_t>(Slot),
        inputs, outputs, commandBuffer);
}

template <size_t Slot>
sl::Result FreeResourcesThunk(sl::Feature feature,
    const sl::ViewportHandle& viewport)
{
    return HandleFreeResources(static_cast<uint32_t>(Slot),
        feature, viewport);
}

template <size_t... Slots>
constexpr auto MakePublicSetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slDLSSGSetOptions*, sizeof...(Slots)>{
        &PublicSetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakePublicGetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slDLSSGGetState*, sizeof...(Slots)>{
        &PublicGetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeInternalSetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slSetDataInternal*, sizeof...(Slots)>{
        &InternalSetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeInternalGetThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slGetDataInternal*, sizeof...(Slots)>{
        &InternalGetThunk<Slots>...};
}

template <size_t... Slots>
constexpr auto MakeFreeResourcesThunks(std::index_sequence<Slots...>)
{
    return std::array<PFun_slFreeResources*, sizeof...(Slots)>{
        &FreeResourcesThunk<Slots>...};
}

constexpr auto gPublicSetThunks = MakePublicSetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gPublicGetThunks = MakePublicGetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gInternalSetThunks = MakeInternalSetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gInternalGetThunks = MakeInternalGetThunks(
    std::make_index_sequence<kControlRouteCapacity>{});
constexpr auto gFreeResourcesThunks = MakeFreeResourcesThunks(
    std::make_index_sequence<kControlRouteCapacity>{});

uint32_t EnsureControlRoute(HMODULE wrapper, const std::wstring& path,
    uint64_t generation, bool wrapperPatched,
    uint32_t compiledMaximumGeneratedFrames)
{
    if (!wrapper)
        return UINT32_MAX;
    std::lock_guard lock(gControlRouteMutex);
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord& route = gControlRoutes[slot];
        if (route.claimed.load(std::memory_order_acquire)
            && route.wrapper == wrapper && route.generation == generation)
        {
            route.compiledMaximumGeneratedFrames.store(compiledMaximumGeneratedFrames, std::memory_order_release);
            route.wrapperPatched.store(wrapperPatched, std::memory_order_release);
            return slot;
        }
    }
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord& route = gControlRoutes[slot];
        if (route.claimed.load(std::memory_order_acquire))
            continue;
        route.wrapper = wrapper;
        route.generation = generation;
        route.path = path;
        route.version = ReadFileVersion(path);
        route.wrapperPatched = wrapperPatched;
        route.compiledMaximumGeneratedFrames =
            compiledMaximumGeneratedFrames;
        route.claimed.store(true, std::memory_order_release);
        return slot;
    }
    return UINT32_MAX;
}

bool InstallControlRouteEntries(uint32_t routeSlot)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route || !route->wrapperPatched)
        return false;
    using GetPluginFunctionFn = void* (*)(const char*);
    auto* resolver = reinterpret_cast<GetPluginFunctionFn>(
        GetProcAddress(route->wrapper, "slGetPluginFunction"));
    if (!resolver)
        return false;

    void* publicSet = resolver("slDLSSGSetOptions");
    void* publicGet = resolver("slDLSSGGetState");
    entry_detour::InstallOptions relocated{};
    relocated.generation = route->generation;
    relocated.allowRelocated = true;
    entry_detour::InstallOptions hotpatch{};
    hotpatch.generation = route->generation;

    // Public DLSS-G entries are the narrowest ABI boundary and cover pointers
    // which the host cached before this module was discovered. Do not relocate
    // the generic internal or lifecycle entries on every merely discovered
    // wrapper: those functions also participate in plugin startup. They are
    // installed only as a missing-public-entry fallback, while lifecycle is
    // delayed until a real call selects this wrapper.
    void* trampoline = nullptr;
    bool publicSetCurrent = ControlEntryCurrent(route->publicSetHandle);
    if (!publicSetCurrent && publicSet)
    {
        HMODULE owner = ModuleFromAddress(publicSet);
        if (owner)
        {
            publicSetCurrent = entry_detour::Install(
                entry_detour::Kind::eDlssgSetOptions, owner, publicSet,
                reinterpret_cast<void*>(gPublicSetThunks[routeSlot]),
                trampoline, hotpatch, &route->publicSetHandle);
            if (!publicSetCurrent)
            {
                publicSetCurrent = entry_detour::Install(
                    entry_detour::Kind::eDlssgSetOptions, owner, publicSet,
                    reinterpret_cast<void*>(gPublicSetThunks[routeSlot]),
                    trampoline, relocated, &route->publicSetHandle);
            }
            if (publicSetCurrent)
            {
                route->publicSetResolverFallback.store(false,
                    std::memory_order_release);
                route->publicSetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGSetOptions*>(trampoline),
                    std::memory_order_release);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    trampoline = nullptr;
    bool publicGetCurrent = ControlEntryCurrent(route->publicGetHandle);
    if (!publicGetCurrent && publicGet)
    {
        HMODULE owner = ModuleFromAddress(publicGet);
        if (owner)
        {
            publicGetCurrent = entry_detour::Install(
                entry_detour::Kind::eDlssgGetState, owner, publicGet,
                reinterpret_cast<void*>(gPublicGetThunks[routeSlot]),
                trampoline, hotpatch, &route->publicGetHandle);
            if (!publicGetCurrent)
            {
                publicGetCurrent = entry_detour::Install(
                    entry_detour::Kind::eDlssgGetState, owner, publicGet,
                    reinterpret_cast<void*>(gPublicGetThunks[routeSlot]),
                    trampoline, relocated, &route->publicGetHandle);
            }
            if (publicGetCurrent)
            {
                route->publicGetResolverFallback.store(false,
                    std::memory_order_release);
                route->publicGetOriginal.store(
                    reinterpret_cast<PFun_slDLSSGGetState*>(trampoline),
                    std::memory_order_release);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    const bool setResolverCovered =
        route->publicSetResolverFallback.load(std::memory_order_acquire)
        && route->publicSetOriginal.load(std::memory_order_acquire);
    const bool getResolverCovered =
        route->publicGetResolverFallback.load(std::memory_order_acquire)
        && route->publicGetOriginal.load(std::memory_order_acquire);

    bool internalSetCurrent = ControlEntryCurrent(route->internalSetHandle);
    if (!internalSetCurrent
        && universal_route_policy::NeedsInternalFallback(
            publicSetCurrent || setResolverCovered))
    {
        void* internalSet = resolver("slSetData");
        if (internalSet)
        {
            trampoline = nullptr;
            HMODULE owner = ModuleFromAddress(internalSet);
            internalSetCurrent = owner && entry_detour::Install(
                entry_detour::Kind::eSlSetData, owner, internalSet,
                reinterpret_cast<void*>(gInternalSetThunks[routeSlot]),
                trampoline, relocated, &route->internalSetHandle);
            if (internalSetCurrent)
            {
                route->internalSetOriginal.store(
                    reinterpret_cast<PFun_slSetDataInternal*>(trampoline),
                    std::memory_order_release);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    bool internalGetCurrent = ControlEntryCurrent(route->internalGetHandle);
    if (!internalGetCurrent
        && universal_route_policy::NeedsInternalFallback(
            publicGetCurrent || getResolverCovered))
    {
        void* internalGet = resolver("slGetData");
        if (internalGet)
        {
            trampoline = nullptr;
            HMODULE owner = ModuleFromAddress(internalGet);
            internalGetCurrent = owner && entry_detour::Install(
                entry_detour::Kind::eSlGetData, owner, internalGet,
                reinterpret_cast<void*>(gInternalGetThunks[routeSlot]),
                trampoline, relocated, &route->internalGetHandle);
            if (internalGetCurrent)
            {
                route->internalGetOriginal.store(
                    reinterpret_cast<PFun_slGetDataInternal*>(trampoline),
                    std::memory_order_release);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
            }
        }
    }

    const bool activeRoute =
        gActiveControlRouteSlot.load(std::memory_order_acquire) == routeSlot;
    if (universal_route_policy::CanInstallLifecycleEntry(activeRoute))
        InstallControlRouteLifecycleEntry(routeSlot);

    const entry_detour::Snapshot publicSetState =
        entry_detour::ReadSnapshot(route->publicSetHandle);
    const entry_detour::Snapshot publicGetState =
        entry_detour::ReadSnapshot(route->publicGetHandle);
    const entry_detour::Snapshot internalSetState =
        entry_detour::ReadSnapshot(route->internalSetHandle);
    const entry_detour::Snapshot internalGetState =
        entry_detour::ReadSnapshot(route->internalGetHandle);
    const entry_detour::Snapshot releaseState =
        entry_detour::ReadSnapshot(route->freeResourcesHandle);
    Log(L"DLSS-G wrapper route generation=%llu public=%d/%d (%hs/%hs) "
        L"internalFallback=%d/%d (%hs/%hs) release=%d (%hs) active=%d "
        L"path=%s",
        static_cast<unsigned long long>(route->generation),
        publicSetCurrent, publicGetCurrent,
        entry_detour::MethodName(publicSetState.method),
        entry_detour::MethodName(publicGetState.method),
        internalSetCurrent, internalGetCurrent,
        entry_detour::MethodName(internalSetState.method),
        entry_detour::MethodName(internalGetState.method),
        releaseState.current, entry_detour::MethodName(releaseState.method),
        activeRoute, route->path.c_str());
    return (publicSetCurrent || internalSetCurrent || setResolverCovered)
        && (publicGetCurrent || internalGetCurrent || getResolverCovered);
}

bool InstallControlRouteLifecycleEntry(uint32_t routeSlot)
{
    ControlRouteRecord* route = ControlRouteAt(routeSlot);
    if (!route
        || !route->wrapperPatched
        || !route->structureCompatible.load(std::memory_order_acquire)
        || !universal_route_policy::CanInstallLifecycleEntry(
            gActiveControlRouteSlot.load(std::memory_order_acquire)
                == routeSlot))
        return false;
    if (ControlEntryCurrent(route->freeResourcesHandle))
        return true;

    bool expected = false;
    if (!route->lifecycleInstallAttempted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return ControlEntryCurrent(route->freeResourcesHandle);

    using GetPluginFunctionFn = void* (*)(const char*);
    auto* resolver = reinterpret_cast<GetPluginFunctionFn>(
        GetProcAddress(route->wrapper, "slGetPluginFunction"));
    void* freeResources = resolver ? resolver("slFreeResources") : nullptr;
    entry_detour::InstallOptions relocated{};
    relocated.generation = route->generation;
    relocated.allowRelocated = true;
    void* trampoline = nullptr;
    bool freeResourcesCurrent = false;
    if (freeResources)
    {
        HMODULE owner = ModuleFromAddress(freeResources);
        freeResourcesCurrent = owner && entry_detour::Install(
            entry_detour::Kind::eSlFreeResources, owner, freeResources,
            reinterpret_cast<void*>(gFreeResourcesThunks[routeSlot]),
            trampoline, relocated, &route->freeResourcesHandle);
        if (freeResourcesCurrent)
        {
            route->freeResourcesOriginal.store(
                reinterpret_cast<PFun_slFreeResources*>(trampoline),
                std::memory_order_release);
        }
    }
    const entry_detour::Snapshot releaseState =
        entry_detour::ReadSnapshot(route->freeResourcesHandle);
    Log(L"DLSS-G lifecycle route generation=%llu installed=%d current=%d "
        L"method=%hs failure=%u path=%s",
        static_cast<unsigned long long>(route->generation),
        freeResourcesCurrent, releaseState.current,
        entry_detour::MethodName(releaseState.method),
        static_cast<uint32_t>(releaseState.failure), route->path.c_str());
    return freeResourcesCurrent;
}

// Generation of the module instance now mapped at this base. A retired
// module's record can share a recycled base until inventory removes it, so
// the live path selects the current instance.
uint64_t LiveModuleGeneration(HMODULE module)
{
    const std::wstring path = LoadedModulePath(module);
    if (path.empty())
        return 0;
    std::lock_guard lock(gModuleMutex);
    const auto record = std::find_if(gModuleRecords.begin(),
        gModuleRecords.end(), [&](const ModuleRecord& candidate) {
            return candidate.module == module
                && _wcsicmp(candidate.path.c_str(), path.c_str()) == 0;
        });
    return record == gModuleRecords.end() ? 0 : record->generation;
}

uint32_t FindControlRouteForTarget(void* target) noexcept
{
    HMODULE owner = ModuleFromAddress(target);
    if (!owner)
        return UINT32_MAX;
    // A route kept for an unloaded wrapper must not claim the module now at
    // its recycled base: its resolver fallback would wrap an entry the live
    // route already detours, and that call would re-enter the control path.
    const uint64_t generation = LiveModuleGeneration(owner);
    if (!generation)
        return UINT32_MAX;
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (!route || route->generation != generation)
            continue;
        for (entry_detour::Handle handle : {route->publicSetHandle,
                 route->publicGetHandle, route->internalSetHandle,
                 route->internalGetHandle, route->freeResourcesHandle})
        {
            const entry_detour::Snapshot state =
                entry_detour::ReadSnapshot(handle);
            if (state.owner == owner || state.target == target)
                return slot;
        }
        if (route->wrapper == owner)
            return slot;
    }
    return UINT32_MAX;
}

bool TryInstallSetOptionsEntryDetour(HMODULE wrapper, void*)
{
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (route && route->wrapper == wrapper)
            return InstallControlRouteEntries(slot)
                && (ControlEntryCurrent(route->publicSetHandle)
                    || ControlEntryCurrent(route->internalSetHandle));
    }
    return false;
}

bool TryInstallGetStateEntryDetour(HMODULE wrapper, void*)
{
    for (uint32_t slot = 0; slot < gControlRoutes.size(); ++slot)
    {
        ControlRouteRecord* route = ControlRouteAt(slot);
        if (route && route->wrapper == wrapper)
            return InstallControlRouteEntries(slot)
                && (ControlEntryCurrent(route->publicGetHandle)
                    || ControlEntryCurrent(route->internalGetHandle));
    }
    return false;
}

// Compatibility entry points are retained for binaries which resolve the
// symbols directly. Resolver fallbacks below always publish a typed route thunk.
sl::Result HookSlDLSSGGetState(const sl::ViewportHandle& viewport,
    sl::DLSSGState& state, const sl::DLSSGOptions* options)
{
    ControlRouteRecord* route = ActiveControlRoute();
    return route ? HandlePublicGetState(
        gActiveControlRouteSlot.load(std::memory_order_acquire),
        ControlEntryPath::eResolver, viewport, state, options)
        : sl::Result::eErrorNotInitialized;
}

uint32_t ResolvedDlssgControlRoute(void* resolved)
{
    HMODULE owner = ModuleFromAddress(resolved);
    MEMORY_BASIC_INFORMATION memory{};
    if (!owner || !VirtualQuery(resolved, &memory, sizeof(memory))
        || memory.AllocationBase != owner || memory.State != MEM_COMMIT
        || memory.Type != MEM_IMAGE || (memory.Protect & PAGE_GUARD)
        || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        || !GetProcAddress(owner, "slGetPluginFunction"))
        return UINT32_MAX;
    const uint32_t slot = FindControlRouteForTarget(resolved);
    if (slot != UINT32_MAX)
        return slot;

    // The successful, feature-specific lookup proves callable identity. Its
    // owner must remain loaded for every published typed forwarding pointer.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(resolved), &pinned) || pinned != owner)
        return UINT32_MAX;
    const ModuleRecord discovered = InspectLoadedModule(owner, LoadedModulePath(owner));
    if (discovered.controlRouteSlot != UINT32_MAX)
        return discovered.controlRouteSlot;
    if (!discovered.wrapperExport || !discovered.generation)
        return UINT32_MAX;
    // Unknown wrapper layouts get resolver forwarding only: no executable
    // entry patch, maximum claim, lifecycle patch, or MFG authorization.
    return EnsureControlRoute(owner, discovered.path, discovered.generation, false, 0);
}

// The game's latency markers and Reflex sleep, for XeMFG standalone's XeLL (frame ids and pacing). One
// Streamline per process: the first resolved function of each kind is the one forwarded to.
std::atomic<PFun_slPCLSetMarker*> gOriginalPclSetMarker{nullptr};
using PFun_slReflexSetMarkerLegacy = sl::Result(uint32_t marker, const sl::FrameToken& frame);   // SL 2.0-2.4
std::atomic<PFun_slReflexSetMarkerLegacy*> gOriginalReflexSetMarker{nullptr};
std::atomic<PFun_slReflexSleep*> gOriginalReflexSleep{nullptr};

// nulltoken.1 (Indiana Jones, beta.1 cut 5 as winmm.dll): Streamline's ABI passes the frame token by pointer and a
// game may pass null. TheGreatCircle's renderer called slPCLSetMarker with one: sl.pcl answered
// eErrorMissingInputParameter, then this hook read the token's vtable and the game crashed. Our Streamline hooks read
// a token only through here. A null or unreadable token goes on to Streamline unchanged and skips our bookkeeping.
// The address passes through a volatile so the null test is not optimised away (a C++ reference is assumed non-null).
bool ReadFrameToken(const sl::FrameToken& frame, uint32_t& id) noexcept
{
    const sl::FrameToken* volatile address = &frame;
    const sl::FrameToken* token = address;
    if (!token) return false;
    __try
    {
        id = static_cast<uint32_t>(*token);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void NoteUnreadableToken(const wchar_t* function, sl::Result result) noexcept
{
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1, std::memory_order_relaxed) < 4)
        Log(L"Streamline: the game passed %s a null or unreadable frame token (Streamline answered %d); passed on "
            L"unchanged, without our latency or tag bookkeeping for that call", function, static_cast<int>(result));
}

// latmon.1: the latency monitor stamps each marker before it is forwarded, as the driver's own timestamp would be.
void ProbeMarker(uint32_t marker, uint32_t frame, uint32_t source) noexcept
{
    if (!latency_probe::Recording()) return;
    __try { latency_probe::Marker(marker, frame, source); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

sl::Result HookSlPclSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    auto* original = gOriginalPclSetMarker.load(std::memory_order_acquire);
    if (!original) return sl::Result::eErrorNotInitialized;
    uint32_t id = 0;
    if (!ReadFrameToken(frame, id))
    {
        const sl::Result result = original(marker, frame);
        NoteUnreadableToken(L"slPCLSetMarker", result);
        return result;
    }
    ProbeMarker(static_cast<uint32_t>(marker), id, latency_probe::kSourcePcl);
    const sl::Result result = original(marker, frame);
    xess_d3d12::LatencyMarker(static_cast<uint32_t>(marker), id);
    return result;
}

sl::Result HookSlReflexSetMarker(uint32_t marker, const sl::FrameToken& frame)
{
    auto* original = gOriginalReflexSetMarker.load(std::memory_order_acquire);
    if (!original) return sl::Result::eErrorNotInitialized;
    uint32_t id = 0;
    if (!ReadFrameToken(frame, id))
    {
        const sl::Result result = original(marker, frame);
        NoteUnreadableToken(L"slReflexSetMarker", result);
        return result;
    }
    ProbeMarker(marker, id, latency_probe::kSourceReflexLegacy);
    const sl::Result result = original(marker, frame);
    xess_d3d12::LatencyMarker(marker, id);
    return result;
}

sl::Result HookSlReflexSleep(const sl::FrameToken& frame)
{
    auto* original = gOriginalReflexSleep.load(std::memory_order_acquire);
    if (!original) return sl::Result::eErrorNotInitialized;
    // nulltoken.1: no frame id for XeLL, so Streamline decides (it answers a null token with an error).
    uint32_t id = 0;
    if (!ReadFrameToken(frame, id))
    {
        const sl::Result result = original(frame);
        NoteUnreadableToken(L"slReflexSleep", result);
        return result;
    }
    // sleepsight.1: the game's sleep is the flip queue's evidence (less our own low-latency sleep inside it).
    sdk_pacing::HostSleepScope sleepScope(sdk_pacing::HostSleepSource::kReflex);
    // While XeMFG generates, the frame sleeps in XeLL only (the driver's Reflex sleep would be a second pacer,
    // judging the game by XeFG's extra presents: a 30 fps base fell to 17).
    if (xess_d3d12::ReplacesReflexSleep())
    {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            Log(L"XeMFG standalone: the game's Reflex sleep goes to XeLL while XeMFG generates");
        xess_d3d12::LatencySleep(id, false);
        return sl::Result::eOk;
    }
    const sl::Result result = original(frame);
    xess_d3d12::LatencySleep(id, result == sl::Result::eOk);
    return result;
}

template<class Fn> void WrapFeatureFunction(std::atomic<Fn*>& slot, Fn* hook, void*& function, const wchar_t* name)
{
    auto* candidate = reinterpret_cast<Fn*>(function);
    if (candidate == hook) return;
    Fn* expected = nullptr;
    const bool first = slot.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel);
    if (first || expected == candidate)
    {
        function = reinterpret_cast<void*>(hook);
        if (first && xess_d3d12::Ready())
            Log(L"XeMFG standalone: %s wrapped (the game's latency markers and sleep reach XeLL)", name);
        else if (first)
            Log(L"Latency monitor: %s wrapped (the game's latency markers are timed, then forwarded)", name);
    }
}

// sleepsight.1 (AMD: a tester, E33, RX 7800 XT, OptiScaler: pacerfix.3 lost 12-22% output fps): on XeMFG's other routes
// the game's Reflex sleep is timed only, never changed. OptiScaler (or the driver) turns it into XeLL, Anti-Lag 2 or
// Reflex, which the flip queue's clock never saw: the game slept at our cadence and the clock learned that cadence. One
// Streamline per process, as above. optisleep.1: under OptiScaler the low-latency pacing's sleep follows it
// (sdk_pacing HostSleepPoint); the game's Reflex call itself is still passed on unchanged.
std::atomic<PFun_slReflexSleep*> gObservedReflexSleep{nullptr};

sl::Result ObserveSlReflexSleep(const sl::FrameToken& frame)
{
    auto* original = gObservedReflexSleep.load(std::memory_order_acquire);
    if (!original) return sl::Result::eErrorNotInitialized;
    sl::Result result;
    {
        sdk_pacing::HostSleepScope sleepScope(sdk_pacing::HostSleepSource::kReflex);
        result = original(frame);
    }
    // optisleep.1: under OptiScaler the game's Reflex sleep is its sleep point before its input, whatever OptiScaler turns
    // it into (latency.1's sleep follows it, unless a libxell sleep inside it already took this frame's).
    if (optiscaler_host::Detected()) sdk_pacing::HostSleepPoint();
    return result;
}

void ObserveReflexSleep(void*& function)
{
    auto* candidate = reinterpret_cast<PFun_slReflexSleep*>(function);
    if (candidate == &ObserveSlReflexSleep) return;
    PFun_slReflexSleep* expected = nullptr;
    const bool first = gObservedReflexSleep.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel);
    if (!first && expected != candidate) return;
    function = reinterpret_cast<void*>(&ObserveSlReflexSleep);
    if (first) Log(L"XeMFG: slReflexSleep timed for the flip queue's clock (the game's low-latency sleep)");
}

void WrapLatencyFunction(sl::Feature feature, const char* functionName, void*& function)
{
    // xemfglive.1: also while XeMFG is off but could be switched on live (the wrappers then only forward).
    const bool xemfg = xess_d3d12::Ready();
    // sleepsight.1: XeMFG on its other routes (OptiScaler's, the game's own XeFG): the game's Reflex sleep is timed
    // only (ObserveReflexSleep; optisleep.1's sleep point under OptiScaler). While XeMFG standalone could start
    // (Ready), HookSlReflexSleep below times it the same way (HostSleepScope).
    if (!xemfg && xess_mod::Active() && feature == sl::kFeatureReflex && strcmp(functionName, "slReflexSleep") == 0)
    {
        ObserveReflexSleep(function);
        return;
    }
    // latmon.2: the latency monitor wraps the marker setters on every route too ([Overlay] LatencyMonitor=0 keeps them
    // as they were); the hooks only forward while nothing shows the readout (xess_d3d12::LatencyMarker does nothing
    // while XeMFG standalone is off). The sleep stays XeMFG's.
    if (!xemfg && !latency_probe::Available()) return;
    if (feature == sl::kFeaturePCL && strcmp(functionName, "slPCLSetMarker") == 0)
        WrapFeatureFunction(gOriginalPclSetMarker, &HookSlPclSetMarker, function, L"slPCLSetMarker");
    else if (feature == sl::kFeatureReflex && strcmp(functionName, "slReflexSetMarker") == 0)
        WrapFeatureFunction(gOriginalReflexSetMarker, &HookSlReflexSetMarker, function, L"slReflexSetMarker");
    else if (xemfg && feature == sl::kFeatureReflex && strcmp(functionName, "slReflexSleep") == 0)
        WrapFeatureFunction(gOriginalReflexSleep, &HookSlReflexSleep, function, L"slReflexSleep");
}

sl::Result HookSlGetFeatureFunction(
    sl::Feature feature, const char* functionName, void*& function)
{
    auto* original = gOriginalGetFeatureFunction.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    // Hosts which let the interposer create the device never call
    // slSetD3DDevice; feature functions are only available once it exists.
    if (AllowStreamlinePluginRetention(L"slGetFeatureFunction"))
        InspectAlreadyLoadedModules();

    const sl::Result result = original(feature, functionName, function);
    if (result == sl::Result::eOk && function && functionName)
        WrapLatencyFunction(feature, functionName, function);
    if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G)
        return result;
    if (function && functionName && strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        void* const resolved = function;
        const uint32_t routeSlot = ResolvedDlssgControlRoute(resolved);
        ControlRouteRecord* route = ControlRouteAt(routeSlot);
        if (route)
        {
            InstallControlRouteEntries(routeSlot);
            const bool publicEntry =
                ControlEntryCurrent(route->publicSetHandle);
            const bool internalEntry =
                ControlEntryCurrent(route->internalSetHandle);
            if (!publicEntry && !internalEntry
                && resolved != reinterpret_cast<void*>(
                    gPublicSetThunks[routeSlot]))
            {
                auto* candidate = reinterpret_cast<PFun_slDLSSGSetOptions*>(resolved);
                auto* expected = static_cast<PFun_slDLSSGSetOptions*>(nullptr);
                if (!route->publicSetOriginal.compare_exchange_strong(expected, candidate,
                        std::memory_order_acq_rel) && expected != candidate)
                    return result;
                route->publicSetResolverFallback.store(
                    true, std::memory_order_release);
                function = reinterpret_cast<void*>(
                    gPublicSetThunks[routeSlot]);
                gSetOptionsHookExposed.store(true,
                    std::memory_order_release);
                gSetOptionsResolverFallbackActive.store(
                    true, std::memory_order_release);
            }
        }
    }
    else if (function && functionName && strcmp(functionName, "slDLSSGGetState") == 0)
    {
        void* const resolved = function;
        const uint32_t routeSlot = ResolvedDlssgControlRoute(resolved);
        ControlRouteRecord* route = ControlRouteAt(routeSlot);
        if (route)
        {
            InstallControlRouteEntries(routeSlot);
            const bool publicEntry =
                ControlEntryCurrent(route->publicGetHandle);
            const bool internalEntry =
                ControlEntryCurrent(route->internalGetHandle);
            if (!publicEntry && !internalEntry
                && resolved != reinterpret_cast<void*>(
                    gPublicGetThunks[routeSlot]))
            {
                auto* candidate = reinterpret_cast<PFun_slDLSSGGetState*>(resolved);
                auto* expected = static_cast<PFun_slDLSSGGetState*>(nullptr);
                if (!route->publicGetOriginal.compare_exchange_strong(expected, candidate,
                        std::memory_order_acq_rel) && expected != candidate)
                    return result;
                route->publicGetResolverFallback.store(
                    true, std::memory_order_release);
                function = reinterpret_cast<void*>(
                    gPublicGetThunks[routeSlot]);
                gGetStateHookExposed.store(true,
                    std::memory_order_release);
                gGetStateResolverFallbackActive.store(
                    true, std::memory_order_release);
            }
        }
    }
    return result;
}

// Logs the first tag of each buffer type once, so a new integration's tagging
// (empty resources, struct versions, lifecycles) is visible without a debugger.
void LogFirstTagOfType(const sl::ResourceTag& tag, bool framed, bool hasList) noexcept
{
    static std::array<std::atomic<bool>, 96> logged{};
    if (tag.type >= logged.size() || logged[tag.type].exchange(true, std::memory_order_relaxed))
        return;
    const bool tagIdentity = tag.structType == sl::ResourceTag::s_structType;
    const auto* resource = tag.resource;
    const bool resourceIdentity = resource && resource->structType == sl::Resource::s_structType;
    Log(L"Streamline tag first seen: type=%u framed=%d list=%d tagIdentity=%d tagVersion=%u "
        L"lifecycle=%u extent=%u,%u %ux%u resource=%d resourceIdentity=%d resourceVersion=%u "
        L"resourceType=0x%08X resourceKind=%u native=%d state=0x%X format=%u size=%ux%u",
        tag.type, framed, hasList, tagIdentity, tag.structVersion,
        static_cast<uint32_t>(tag.lifecycle), tag.extent.left, tag.extent.top,
        tag.extent.width, tag.extent.height, resource != nullptr, resourceIdentity,
        resource ? resource->structVersion : 0u,
        resource ? static_cast<uint32_t>(resource->structType.data1) : 0u,
        resource ? static_cast<uint32_t>(resource->type) : 0u, resource && resource->native,
        resource ? resource->state : 0u, resource ? resource->nativeFormat : 0u,
        resource ? resource->width : 0u, resource ? resource->height : 0u);
}

void LogFirstTags(const sl::ResourceTag* tags, uint32_t numTags, bool framed,
    bool hasList) noexcept
{
    for (uint32_t index = 0; tags && index < numTags && index < 1024; ++index)
        LogFirstTagOfType(tags[index], framed, hasList);
}

// DLSS 5 Present stage with Block Hudless: frame generation must not
// interpolate the game's unprocessed HUD-less image, so a copy of the tags
// goes on with that tag cleared (a null resource untags it). Every consumer
// below, ours included, sees the forwarded tags.
const sl::ResourceTag* WithholdHudless(const sl::ResourceTag* tags, uint32_t numTags,
    std::vector<sl::ResourceTag>& copy) noexcept
{
    if (!tags || numTags > 1024 || !dlss5::BlockHudless()) return tags;
    bool tagged = false;
    for (uint32_t index = 0; index < numTags; ++index)
        tagged |= tags[index].type == sl::kBufferTypeHUDLessColor && tags[index].resource;
    if (!tagged) return tags;
    try { copy.assign(tags, tags + numTags); }
    catch (...) { return tags; }
    if (!dlss5::WithholdTags(copy.data(), numTags, sl::kBufferTypeHUDLessColor)) return tags;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        Log(L"DLSS 5 Present stage: the game's HUD-less tag is withheld from frame generation (Block Hudless)");
    return copy.data();
}

sl::Result HookSlSetTag(const sl::ViewportHandle& viewport,
    const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    auto* original = gOriginalSetTag.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (!numTags)
        return sl::Result::eOk;
    std::vector<sl::ResourceTag> forwarded;
    tags = WithholdHudless(tags, numTags, forwarded);
    const auto batch = PrepareUiResourceTags(viewport, tags, numTags, false, 0);
    const sl::Result result = original(viewport, tags, numTags, cmdBuffer);
    gSetTagCalls.fetch_add(1, std::memory_order_relaxed);
    if (result == sl::Result::eOk)
    {
        RecordUiResourceTags(batch);
        LogFirstTags(tags, numTags, false, cmdBuffer != nullptr);
        // XeMFG standalone's inputs (the latest frame token's).
        xess_d3d12::StreamlineTags(static_cast<uint32_t>(viewport), false, 0, tags, numTags, cmdBuffer);
    }
    return result;
}

sl::Result HookSlSetTagForFrame(const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
    uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    auto* original = gOriginalSetTagForFrame.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    if (!numTags)
        return sl::Result::eOk;
    std::vector<sl::ResourceTag> forwarded;
    tags = WithholdHudless(tags, numTags, forwarded);
    // nulltoken.1: a null or unreadable token is forwarded without our UI or XeMFG bookkeeping.
    uint32_t id = 0;
    const bool readable = ReadFrameToken(frame, id);
    const auto batch = readable ? PrepareUiResourceTags(viewport, tags, numTags, true, id) : UiTagBatch{};
    const sl::Result result = original(frame, viewport, tags, numTags, cmdBuffer);
    gSetTagForFrameCalls.fetch_add(1, std::memory_order_relaxed);
    if (!readable)
        NoteUnreadableToken(L"slSetTagForFrame", result);
    else if (result == sl::Result::eOk)
    {
        RecordUiResourceTags(batch);
        LogFirstTags(tags, numTags, true, cmdBuffer != nullptr);
        // XeMFG standalone's inputs.
        xess_d3d12::StreamlineTags(static_cast<uint32_t>(viewport), true, id, tags, numTags, cmdBuffer);
    }
    return result;
}

// The game's per-frame camera (jitter, motion-vector scale, reset, depth convention, near/far/FOV) for
// XeMFG standalone, after Streamline accepted it.
sl::Result HookSlSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
    const sl::ViewportHandle& viewport)
{
    auto* original = gOriginalSetConstants.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const sl::Result result = original(values, frame, viewport);
    uint32_t id = 0;
    if (!ReadFrameToken(frame, id))
        NoteUnreadableToken(L"slSetConstants", result);
    else if (result == sl::Result::eOk)
        xess_d3d12::StreamlineConstants(values, id, static_cast<uint32_t>(viewport));
    return result;
}

bool InspectAlreadyLoadedModules();

// One call of a hook on this thread: a nested call sees a nonzero depth.
struct ReentryScope
{
    uint32_t& depth;
    explicit ReentryScope(uint32_t& value) noexcept : depth(value) { ++depth; }
    ~ReentryScope() { --depth; }
};

// An import or resolver route and the interposer entry detour can both lead to
// one Streamline hook for one call. The outer call (depth 0) does the work and
// forwards along its own route; the nested entry call goes straight to the entry body.
template <typename Fn>
Fn* RoutedOriginal(uint32_t depth, Fn* entry, Fn* routed) noexcept
{
    return depth != 0 ? (entry ? entry : routed) : (routed ? routed : entry);
}

sl::Result HookSlSetD3DDevice(void* device)
{
    static thread_local uint32_t depth = 0;
    auto* original = RoutedOriginal(depth, EntryOriginal(gOriginalSlSetD3DDeviceEntry,
        gSlSetD3DDeviceEntryHandle), gOriginalSetD3DDevice.load(std::memory_order_acquire));
    if (!original || depth != 0)
        return original ? original(device) : sl::Result::eErrorNotInitialized;
    ReentryScope nested(depth);
    gSlSetD3DDeviceCalls.fetch_add(1, std::memory_order_relaxed);
    AllowStreamlinePluginRetention(L"slSetD3DDevice");
    InvalidateUiInputEvidence();
    const bool verifiedDevice = gpu_backend::ObserveD3D12Device(device);
    if (verifiedDevice)
    {
        gModuleInventoryDirty.store(true, std::memory_order_release);
        if (NativeDlssgPresetGpu())
        {
            // RTX 40 and RTX 50: re-inspect the plug-ins recorded before the family was known.
            dlssg_preset::Prepare();
            temporal_interval_trace::SetEnabled(true);
            InspectAlreadyLoadedModules();
        }
        else if (UseAmpere())
        {
            temporal_interval_trace::SetEnabled(true);
        }
    }
    const sl::Result result = UseAmpere()
        ? InvokeAmpereDeviceSetup(original, device) : original(device);
    return result;
}

void PatchRecordedCandidates();

bool OptiScalerPrivateStreamline(const wchar_t* path, size_t length) noexcept;

// After the private instance's RTX 30 startup its sl.dlss_g is a control route
// like the game's. OptiScaler resolves slDLSSGSetOptions/GetState through its
// private interposer (whose entries stay untouched), so hook those public
// wrapper entries: OptiScaler's first real call then selects the active wrapper
// that the RTX 30 Create checks. Returns the routes with both entries current.
uint32_t InstallOptiScalerPrivateWrapperRoutes()
{
    // Wrappers recorded before RTX 30 was selected are admitted on re-inspection.
    PatchRecordedCandidates();
    std::vector<uint32_t> slots;
    {
        std::lock_guard lock(gModuleMutex);
        for (const ModuleRecord& record : gModuleRecords)
        {
            if (record.wrapperCandidate && record.wrapperPatched
                && record.controlRouteSlot != UINT32_MAX
                && OptiScalerPrivateStreamline(record.path.c_str(), record.path.size()))
                slots.push_back(record.controlRouteSlot);
        }
    }
    uint32_t installed = 0;
    for (const uint32_t slot : slots)
    {
        InstallControlRouteEntries(slot);
        const ControlRouteRecord* route = ControlRouteAt(slot);
        if (route && ControlEntryCurrent(route->publicSetHandle)
            && ControlEntryCurrent(route->publicGetHandle))
            ++installed;
    }
    return installed;
}

// OptiScaler's DLSS-G output runs its private Streamline, whose slSetD3DDevice
// (called from OptiScaler's D3D12CreateDevice hook) starts the plug-ins before
// the game's interposer sees a device. sl.dlss_g's slOnPluginStartup caches
// min(DLSSG.MultiFrameCountMax, compiled maximum) there, so on Ada the wrapper
// clamp and the provider gate must already be patched. On RTX 30 this call is a
// startup bracket of its own (capability bootstrap, admission; early init too
// when the game has no Streamline), also beside the game's Streamline before the
// first FG Create: the game's later bracket bootstraps its own sl.dlss_g, but
// this one is the instance OptiScaler's output runs. Then the private wrapper
// becomes a control route. It binds no host, opens no plug-in retention and
// forwards only into the private instance.
sl::Result HookOptiScalerSlSetD3DDevice(void* device)
{
    auto* original = EntryOriginal(gOriginalOptiScalerSlSetD3DDevice,
        gOptiScalerSlSetD3DDeviceEntryHandle);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const uint64_t call = gOptiScalerSlSetD3DDeviceCalls.fetch_add(1,
        std::memory_order_relaxed) + 1;
    // RTX 30 declares its architecture at startup and keeps its own route.
    // RTX 40 patches its plug-ins here; RTX 50 only records them.
    const bool ampere = ampere_provider_arch::Armed() || UseAmpere();
    const bool native = !ampere && gpu_dispatch::ObserveNativeD3D12Device(device);
    bool inventoried = false;
    if (native)
    {
        // Its slInit loaded the plug-ins on this thread just before; discovery
        // may not have recorded them yet. Toolhelp can fail while another
        // thread loads a module, so retry briefly, then use the records held.
        for (int attempt = 0; attempt < 8 && !inventoried; ++attempt)
        {
            if (attempt) Sleep(2);
            inventoried = InspectAlreadyLoadedModules();
        }
        PatchRecordedCandidates();
    }
    Log(L"OptiScaler private Streamline slSetD3DDevice: call=%llu ada=%d ampere=%d inventory=%d rtx50=%d "
        L"(before its plug-in startup)", static_cast<unsigned long long>(call), native && gpu_dispatch::IsAda(),
        ampere, inventoried, native && gpu_dispatch::IsBlackwell());
    if (!ampere)
        return original(device);

    optiscaler_private_streamline::Inputs inputs{};
    inputs.admittedBefore = gOptiScalerPrivateAmpereAdmitted.load(std::memory_order_acquire);
    inputs.gameStreamline = ampere_backend::EarlyInitObserved()
        || gHostInterposer.load(std::memory_order_acquire) != nullptr;
    inputs.beforeFirstCreate = ampere_backend::BeforeFirstFeatureCreate();
    inputs.hookedAtLoad = gOptiScalerSlSetD3DDeviceAtLoad.load(std::memory_order_acquire);
    inputs.armedAtLoad = gOptiScalerSlSetD3DDeviceArmedAtLoad.load(std::memory_order_acquire);
    if (optiscaler_private_streamline::NeedsDevice(inputs))
    {
        // As at the game's slSetD3DDevice: this device selects and verifies the adapter.
        inputs.ampereDevice = gpu_backend::ObserveD3D12Device(device) && UseAmpere();
        if (inputs.ampereDevice)
        {
            gModuleInventoryDirty.store(true, std::memory_order_release);
            temporal_interval_trace::SetEnabled(true);
        }
    }
    const auto decision = optiscaler_private_streamline::Decide(inputs);
    Log(L"OptiScaler private Streamline RTX 30 startup: call=%llu decision=%hs gameStreamline=%d "
        L"beforeCreate=%d hookedAtLoad=%d armedAtLoad=%d device=%d",
        static_cast<unsigned long long>(call), optiscaler_private_streamline::DecisionName(decision),
        inputs.gameStreamline, inputs.beforeFirstCreate, inputs.hookedAtLoad, inputs.armedAtLoad,
        inputs.ampereDevice);
    if (!optiscaler_private_streamline::RunsStartup(decision, inputs.ampereDevice))
        return original(device);
    if (decision == optiscaler_private_streamline::Decision::eAdmit)
    {
        // Hooked as its interposer mapped, with the declaration armed: the
        // early boundary the game's slInit hook establishes (or already did).
        gOptiScalerPrivateAmpereAdmitted.store(true, std::memory_order_release);
        ampere_backend::ObserveEarlyInit();
    }
    const sl::Result result = InvokeAmpereDeviceSetup(original, device);
    const bool ready = ampere_backend::Ready();
    const uint32_t routes = ready ? InstallOptiScalerPrivateWrapperRoutes() : 0;
    Log(L"OptiScaler private Streamline RTX 30 startup result: call=%llu result=%u ready=%d "
        L"failure=%u startupMaximum=%u wrapperRoutes=%u",
        static_cast<unsigned long long>(call), static_cast<unsigned>(result), ready,
        ampere_backend::FailureCode(), ampere_backend::StartupMaximumGeneratedFrames(), routes);
    return result;
}

// sl::VulkanInfo v1-v3 carry the same device and physical-device prefix.
void ReadVulkanInfo(const VulkanInfoPrefix& info, void*& physicalDevice, void*& device, void*& instance) noexcept
{
    physicalDevice = nullptr;
    device = nullptr;
    instance = nullptr;
    __try
    {
        if (info.structVersion >= sl::kStructVersion1
            && info.structVersion <= sl::kStructVersion3)
        {
            physicalDevice = info.physicalDevice;
            device = info.device;
            instance = info.instance;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        physicalDevice = nullptr;
        device = nullptr;
        instance = nullptr;
    }
}

sl::Result HookSlSetVulkanInfo(const VulkanInfoPrefix& info)
{
    static thread_local uint32_t depth = 0;
    auto* original = RoutedOriginal(depth, EntryOriginal(gOriginalSlSetVulkanInfoEntry,
        gSlSetVulkanInfoEntryHandle), gOriginalSetVulkanInfo.load(std::memory_order_acquire));
    if (!original || depth != 0)
        return original ? original(info) : sl::Result::eErrorNotInitialized;
    ReentryScope nested(depth);
    AllowStreamlinePluginRetention(L"slSetVulkanInfo");
    InvalidateUiInputEvidence();
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* instance = nullptr;
    ReadVulkanInfo(info, physicalDevice, device, instance);
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (verified)
    {
        // Streamline names the game's VkDevice here (its NGX Init is the project-ID form).
        dlss5::vulkan::ObserveInit(instance, physicalDevice, device);
        if (NativeDlssgPresetGpu()) dlssg_preset::Prepare();
        // The SR/RR DRS hook, before NVIDIA's Vulkan Init reads those keys.
        dlss_sr::PrepareForVulkan();
        temporal_interval_trace::SetEnabled(true);
        if (UseAmpere()) ampere_backend::ObserveVulkanDevice(device, true);
        gModuleInventoryDirty.store(true, std::memory_order_release);
        InspectAlreadyLoadedModules();
    }
    return UseAmpere() && verified ? InvokeAmpereVulkanSetup(original, info) : original(info);
}

// The interposer's vkCreateDevice, on RTX 30 systems only: it creates the
// host's device and then starts the Streamline plug-ins on it.
int32_t WINAPI HookSlVulkanCreateDevice(void* physicalDevice, const void* createInfo,
    const void* allocator, void** device)
{
    auto* original = EntryOriginal(gOriginalSlVulkanCreateDevice, gSlVulkanCreateDeviceEntryHandle);
    if (!original)
        return kVkErrorInitializationFailed;
    if (!ampere_provider_arch::Armed())
        return original(physicalDevice, createInfo, allocator, device);
    const bool verified = physicalDevice
        && gpu_backend::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanAdapterVerified.store(verified, std::memory_order_release);
    if (!verified || !UseAmpere())
        return original(physicalDevice, createInfo, allocator, device);
    AllowStreamlinePluginRetention(L"vkCreateDevice");
    InvalidateUiInputEvidence();
    temporal_interval_trace::SetEnabled(true);
    gModuleInventoryDirty.store(true, std::memory_order_release);
    return InvokeAmpereVulkanDeviceCreation(original, physicalDevice, createInfo, allocator, device);
}

bool HookModuleImport(HMODULE module, const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
    // VirtualProtect applies to an entire page, including neighbouring IAT
    // slots. Serialize discovery writers through protection restoration.
    // No loader calls or external callbacks run while this mutex is held.
    std::lock_guard publicationLock(gImportPublicationMutex);
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const auto& importDirectory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress || !importDirectory.Size)
        return false;

    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + importDirectory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0)
            continue;

        // Without a name table (Themida-packed executables such as Gears of
        // War E-Day) the bound FirstThunk holds resolved addresses, not name
        // RVAs, so no slot can be identified by name. Leave it untouched.
        if (!descriptor->OriginalFirstThunk)
            continue;
        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + descriptor->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++thunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(originalThunk->u1.Ordinal))
                continue;
            if (originalThunk->u1.AddressOfData >= imageSize)
                return false;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) != 0)
                continue;

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            auto* current = protected_pointer::ReadPointer(
                reinterpret_cast<uintptr_t>(slot));
            if (current == replacement)
                return true;

            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory)
                || memory.AllocationBase != module || memory.Type != MEM_IMAGE
                || memory.State != MEM_COMMIT)
                return false;
            const auto published = protected_pointer::ReplaceProtectedPointer(
                reinterpret_cast<uintptr_t>(slot),
                reinterpret_cast<uintptr_t>(current),
                reinterpret_cast<uintptr_t>(replacement),
                &VirtualProtect, PAGE_READWRITE);
            if (published.replacementWasPublished)
                original = current;
            return published.disposition
                == protected_pointer::PublishDisposition::ePublishedRestored;
        }
    }
    return false;
}

// The current value of one named import slot (read only), or null.
void* ReadModuleImport(HMODULE module, const char* importedModule,
    const char* importedFunction) noexcept
{
    __try
    {
        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return nullptr;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !directory.Size) return nullptr;
        for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
             descriptor->Name; ++descriptor)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), importedModule) != 0) continue;
            // As in HookModuleImport: a bound FirstThunk carries no names.
            if (!descriptor->OriginalFirstThunk) continue;
            const auto* name = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
            const auto* slot = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
            for (; name->u1.AddressOfData; ++name, ++slot)
                if (!IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal)
                    && name->u1.AddressOfData < nt->OptionalHeader.SizeOfImage && strcmp(reinterpret_cast<const char*>(
                        reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name),
                        importedFunction) == 0)
                    return protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(&slot->u1.Function));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// apisetgpa.1: the library-loader API-set contract `module` imports this function from, as an executable linked
// against mincore or OneCore does, or null. The name sits in the image's import table, so it stays valid for the
// process. A descriptor with no name table cannot be identified and is skipped (ReadModuleImport).
const char* LoaderContractImporting(HMODULE module, const char* importedFunction) noexcept
{
    static constexpr char kContract[] = "api-ms-win-core-libraryloader-";
    __try
    {
        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return nullptr;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress || !directory.Size) return nullptr;
        for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
             descriptor->Name; ++descriptor)
        {
            const char* name = reinterpret_cast<const char*>(base + descriptor->Name);
            if (_strnicmp(name, kContract, sizeof(kContract) - 1) == 0
                && ReadModuleImport(module, name, importedFunction))
                return name;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

bool HookMainExecutableImport(const char* importedModule,
    const char* importedFunction, void* replacement, void*& original)
{
    return HookModuleImport(GetModuleHandleW(nullptr), importedModule,
        importedFunction, replacement, original);
}

bool ModuleFileNameEquals(const std::wstring& path,
    const wchar_t* expected)
{
    const size_t separator = path.find_last_of(L"\\/");
    const wchar_t* name = separator == std::wstring::npos
        ? path.c_str() : path.c_str() + separator + 1;
    return expected && _wcsicmp(name, expected) == 0;
}

std::wstring WidePathFromAnsi(const char* path)
{
    if (!path)
        return {};
    const int count = MultiByteToWideChar(
        CP_ACP, 0, path, -1, nullptr, 0);
    if (count <= 1)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1,
            result.data(), count))
    {
        return {};
    }
    result.pop_back();
    return result;
}

std::string AnsiPathFromWide(const std::wstring& path)
{
    if (path.empty())
        return {};
    const int count = WideCharToMultiByte(
        CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
        return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1,
            result.data(), count, nullptr, nullptr))
    {
        return {};
    }
    result.pop_back();
    return result;
}

void InspectStreamlineLoadedModule(HMODULE module)
{
    if (!module)
        return;
    gStreamlineLoaderDiscoveryCalls.fetch_add(1, std::memory_order_relaxed);
    InspectLoadedModule(module, LoadedModulePath(module));
}

dlssg_provider_selection::LoadPlan AmpereProviderLoadPlan(
    const std::wstring& requested, HANDLE file = nullptr, DWORD flags = 0)
{
    return dlssg_provider_selection::Plan(requested, file, flags,
        ampere_provider_arch::Armed() && ampere_backend::BeforeFirstFeatureCreate());
}

void VerifySelectedProviderLoad(const dlssg_provider_selection::LoadPlan& plan,
    const std::wstring& requested, HMODULE& module)
{
    if (!plan) return;
    const DWORD error = module ? ERROR_INVALID_DATA : GetLastError();
    const bool verified = dlssg_provider_selection::Loaded(plan, module);
    if (!verified && module) { FreeLibrary(module); module = nullptr; }
    static std::atomic<bool> successReported{false}, failureReported{false};
    if (!(verified ? successReported : failureReported).exchange(true))
        Log(L"Ampere DLSS-G provider selection: verified=%u error=%lu requested=%s selected=%s",
            static_cast<unsigned>(verified), verified ? ERROR_SUCCESS : error,
            requested.c_str(), plan.path.c_str());
    if (!verified) SetLastError(error);
}

void PrepareAmpereProviderSelection()
{
    if (!ampere_provider_arch::Armed() || !ampere_backend::BeforeFirstFeatureCreate()) return;
    dlssg_provider_selection::Prepare(gExecutableDirectory);
    static std::atomic<bool> reported{false};
    const auto state = dlssg_provider_selection::Read();
    if (state.prepared && !reported.exchange(true))
        Log(L"Ampere DLSS-G OTA selection: version=%u.%u.%u nvidiaSigned=%u nativeProviderPinned=%u "
            L"examined=%u signatureRejections=%u scanLimited=%u selected=%s; fallback=original native load",
            state.selected.version.major, state.selected.version.minor, state.selected.version.build,
            static_cast<unsigned>(state.selected.nvidiaSigned), static_cast<unsigned>(state.nativeProviderPinned),
            state.examined, state.signatureRejections, static_cast<unsigned>(state.scanLimited), state.selected.path.c_str());
}

HMODULE WINAPI HookStreamlineLoadLibraryA(LPCSTR fileName)
{
    auto* original = gOriginalStreamlineLoadLibraryA.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryA)
        return nullptr;
    const std::wstring requested = WidePathFromAnsi(fileName);
    const auto providerPlan = AmpereProviderLoadPlan(requested);
    const std::wstring redirect = providerPlan ? providerPlan.path : std::wstring{};
    HMODULE module = nullptr;
    if (!redirect.empty())
    {
        const std::string redirected = AnsiPathFromWide(redirect);
        if (!redirected.empty())
            module = original(redirected.c_str());
        else SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        VerifySelectedProviderLoad(providerPlan, requested, module);
        if (!module)
            module = original(fileName);
    }
    else
    {
        module = original(fileName);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryW(LPCWSTR fileName)
{
    auto* original = gOriginalStreamlineLoadLibraryW.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryW)
        return nullptr;
    const std::wstring requested = fileName ? fileName : L"";
    const auto providerPlan = AmpereProviderLoadPlan(requested);
    const std::wstring redirect = providerPlan ? providerPlan.path : std::wstring{};
    HMODULE module = redirect.empty() ? original(fileName)
        : original(redirect.c_str());
    if (!redirect.empty())
    {
        VerifySelectedProviderLoad(providerPlan, requested, module);
        if (!module)
            module = original(fileName);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryExA(
    LPCSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalStreamlineLoadLibraryExA.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryExA)
        return nullptr;
    const std::wstring requested = WidePathFromAnsi(fileName);
    const auto providerPlan = AmpereProviderLoadPlan(requested, file, flags);
    const std::wstring redirect = providerPlan ? providerPlan.path : std::wstring{};
    HMODULE module = nullptr;
    if (!redirect.empty())
    {
        const std::string redirected = AnsiPathFromWide(redirect);
        if (!redirected.empty())
            module = original(redirected.c_str(), file, flags);
        else SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        VerifySelectedProviderLoad(providerPlan, requested, module);
        if (!module)
            module = original(fileName, file, flags);
    }
    else
    {
        module = original(fileName, file, flags);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

HMODULE WINAPI HookStreamlineLoadLibraryExW(
    LPCWSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalStreamlineLoadLibraryExW.load(
        std::memory_order_acquire);
    if (!original || original == &HookStreamlineLoadLibraryExW)
        return nullptr;
    const std::wstring requested = fileName ? fileName : L"";
    const auto providerPlan = AmpereProviderLoadPlan(requested, file, flags);
    const std::wstring redirect = providerPlan ? providerPlan.path : std::wstring{};
    HMODULE module = redirect.empty() ? original(fileName, file, flags)
        : original(redirect.c_str(), file, flags);
    if (!redirect.empty())
    {
        VerifySelectedProviderLoad(providerPlan, requested, module);
        if (!module)
            module = original(fileName, file, flags);
    }
    InspectStreamlineLoadedModule(module);
    return module;
}

template <typename Function>
bool PreserveLoaderImportOriginal(std::atomic<Function>& destination,
    void* original, Function hook, const wchar_t* functionName,
    const std::wstring& path)
{
    if (!original)
        return destination.load(std::memory_order_acquire) != nullptr;
    auto* candidate = reinterpret_cast<Function>(original);
    if (candidate == hook)
        return destination.load(std::memory_order_acquire) != nullptr;
    Function expected = nullptr;
    if (!destination.compare_exchange_strong(expected, candidate,
            std::memory_order_acq_rel, std::memory_order_acquire)
        && expected != candidate)
    {
        Log(L"Streamline loader discovery skipped conflicting %s chain: %s",
            functionName, path.c_str());
        return false;
    }
    return true;
}

bool InstallStreamlineLoaderDiscovery(
    HMODULE module, const std::wstring& path)
{
    if (!module)
        return false;

    bool installed = false;
    bool newlyHooked = false;
    void* original = nullptr;
    // A failed protection restoration can leave the hook visible. Retain its
    // callable original independently of whether publication verified success.
    bool published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryA",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryA), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryA, original,
            &HookStreamlineLoadLibraryA, L"LoadLibraryA", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryW",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryW), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryW, original,
            &HookStreamlineLoadLibraryW, L"LoadLibraryW", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExA",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryExA), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExA, original,
            &HookStreamlineLoadLibraryExA, L"LoadLibraryExA", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    original = nullptr;
    published = HookModuleImport(module, "KERNEL32.dll", "LoadLibraryExW",
        reinterpret_cast<void*>(&HookStreamlineLoadLibraryExW), original);
    if (original || published)
    {
        const bool chainReady = PreserveLoaderImportOriginal(
            gOriginalStreamlineLoadLibraryExW, original,
            &HookStreamlineLoadLibraryExW, L"LoadLibraryExW", path);
        installed = (published && chainReady) || installed;
        newlyHooked = (published && chainReady && original != nullptr) || newlyHooked;
    }
    if (installed)
        gStreamlineLoaderDiscoveryInstalled.store(true,
            std::memory_order_release);
    if (newlyHooked)
    {
        Log(L"Streamline loader-return discovery installed: %s",
            path.c_str());
    }
    return installed;
}

// The slInit entry detour's own trampoline (the real slInit) while that detour is installed, also when it no longer
// verifies. When MFG Unlocker loads more than 2 GB from sl.interposer, the entry jumps through a near relay, and
// OptiScaler's Detours hook can follow our jumps and re-hook that relay. target: the export the trampoline must
// belong to (null: any).
PFun_slInit* SlInitEntryTrampoline(const void* target) noexcept
{
    const auto state = entry_detour::ReadSnapshot(gSlInitEntryHandle);
    if (!state.installed || !state.original || (target && state.target != target))
        return nullptr;
    return reinterpret_cast<PFun_slInit*>(state.original);
}

sl::Result HookSlInit(const sl::Preferences& preferences,
    uint64_t sdkVersion)
{
    // Never run twice on one thread. A chain that leads back into this hook (another hook library on
    // our entry while ours calls the export) forwards once to the real slInit instead of recursing until the stack
    // overflows.
    thread_local uint32_t inside = 0;
    if (inside)
    {
        auto* real = SlInitEntryTrampoline(nullptr);
        return real ? real(preferences, sdkVersion) : sl::Result::eErrorNotInitialized;
    }
    ReentryScope insideScope(inside);
    fault_capture::BeforeSlInit();
    auto* original = EntryOriginal(gOriginalSlInit, gSlInitEntryHandle);
    if (!original || original == &HookSlInit)
        return sl::Result::eErrorNotInitialized;
    // Recheck application factory imports at the existing early boundary.
    // The UI creates no graphics objects and modifies no native/Streamline tables.
    // The entry detour jumps here, so this is the game's slInit call site.
    single_overlay::BeforeStreamlineInit(_ReturnAddress());
    const auto maskInitEntry = entry_detour::ReadSnapshot(gSlInitEntryHandle);
    {
        std::lock_guard lock(gOutputPullMaskInitMutex);
        gOutputPullMaskSlInit = {};
        gOutputPullMaskVulkanInit = {};
    }

    InvalidateUiInputEvidence();
    gAppliedUiRecompositionEnabled.store(false, std::memory_order_release);
    gAppliedUiRecompositionForced.store(false, std::memory_order_release);
    ampere_backend::ObserveEarlyInit();
    // Before slInit asks NGX for FG requirements, which maps the DLSS-G
    // provider. Its architecture declaration is applied as it is mapped.
    LogAmpereArchitectureDetection(ampere_provider_arch::DetectAndArm());
    PrepareAmpereProviderSelection();

    sl::Preferences adjusted = preferences;
    const uint64_t before = static_cast<uint64_t>(adjusted.flags);
    static_assert(static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA)
        == streamline_ota_policy::kAllowOta);
    static_assert(static_cast<uint64_t>(
        sl::PreferenceFlags::eLoadDownloadedPlugins)
        == streamline_ota_policy::kLoadDownloadedPlugins);
    const std::wstring providerPath = JoinPath(
        gExecutableDirectory, L"nvngx_dlssg.dll");
    const bool providerPreflight =
        dlssg_provider_policy::SupportedProviderVersionMatches(
            providerPath.c_str());
    // The game's interposer, even when another instance was loaded first.
    HMODULE interposer = gHostInterposer.load(std::memory_order_acquire);
    if (!interposer)
        interposer = GetModuleHandleW(L"sl.interposer.dll");
    const std::wstring interposerPath = interposer
        ? LoadedModulePath(interposer) : std::wstring{};
    // Publish the existing resolver gateway before slInit can cache Vulkan
    // entry points inside the interposer. The worker may not have seen it yet.
    if (interposer)
    {
        InstallSlCommonResolverDiscovery(interposer, interposerPath);
        // Manual-hooking hosts upgrade their own DXGI objects after slInit.
        TryInstallSlUpgradeInterfaceEntryDetour(interposer, nullptr);
        // Hosts that resolve the interposer's exports themselves reach
        // slSetD3DDevice only through its entry. It identifies the GPU and
        // patches the DLSS-G wrapper before the plugins read their limits.
        TryInstallSlSetD3DDeviceEntryDetour(interposer);
        // RTX 30 Vulkan hosts: slSetVulkanInfo and the interposer's
        // vkCreateDevice start the plug-ins (detected RTX 30 systems only).
        TryInstallSlVulkanEntryDetours(interposer);
    }
    if (interposer
        && !gStreamlineLoaderDiscoveryInstalled.load(
            std::memory_order_acquire))
    {
        // A resolver-return slInit detour can beat the background module scan.
        // Install discovery synchronously at the last point before the real
        // slInit starts selecting and loading its plug-in set.
        InstallStreamlineLoaderDiscovery(interposer, interposerPath);
    }
    const bool loaderDiscovery =
        gStreamlineLoaderDiscoveryInstalled.load(std::memory_order_acquire);
    ResolveNvidiaCompatibilityPolicy();
    const auto tier = static_cast<nvidia_mfg_policy::Tier>(
        gNvidiaCompatibilityTier.load(std::memory_order_acquire));
    const bool officialSixX = tier == nvidia_mfg_policy::Tier::eSixX;
    FileVersion hostVersion{};
    if (interposer)
        hostVersion = ReadFileVersion(interposerPath);
    const bool coherentHost = hostVersion.major == 2u
        && hostVersion.minor >= 10u;
    gStreamlineHostVersionMajor.store(
        hostVersion.major, std::memory_order_relaxed);
    gStreamlineHostVersionMinor.store(
        hostVersion.minor, std::memory_order_relaxed);
    gStreamlineHostVersionBuild.store(
        hostVersion.build, std::memory_order_relaxed);
    gStreamlineHostVersionPrivate.store(
        hostVersion.privatePart, std::memory_order_relaxed);
    // Selectively redirecting only sl.dlss_g crossed Streamline ABI families
    // and caused startup failures, so that route is retired: official 6X titles
    // opt into Streamline's coherent full-set OTA selection instead.
    // slSetD3DDevice normally follows slInit. Trusted NVAPI inventory may
    // request the coherent downloaded plugin set before that boundary, but
    // does not select gpu_dispatch or relax its later adapter/LUID checks.
    using OtaGpu = streamline_ota_policy::Gpu;
    static_assert(static_cast<uint32_t>(OtaGpu::eUnknown) == static_cast<uint32_t>(gpu_dispatch::Family::eUnknown)
        && static_cast<uint32_t>(OtaGpu::eAda) == static_cast<uint32_t>(gpu_dispatch::Family::eAda)
        && static_cast<uint32_t>(OtaGpu::eAmpere) == static_cast<uint32_t>(gpu_dispatch::Family::eAmpere)
        && static_cast<uint32_t>(OtaGpu::eTuring) == static_cast<uint32_t>(gpu_dispatch::Family::eTuring)
        && static_cast<uint32_t>(OtaGpu::eBlackwell) == static_cast<uint32_t>(gpu_dispatch::Family::eBlackwell)
        && static_cast<uint32_t>(OtaGpu::eConflict) == static_cast<uint32_t>(gpu_dispatch::Family::eConflict));
    const auto earlyHardware = ampere_provider_arch::Read();
    const OtaGpu inventoryGpu = streamline_ota_policy::InventoryGpu(earlyHardware.detected,
        earlyHardware.physicalGpuCount, earlyHardware.singleGpuArchitecture,
        ampere_provider_arch::IsTranslatedRtx(earlyHardware.singleGpuArchitecture, earlyHardware.singleGpuImplementation));
    // Only RTX 30 uses the early inventory. Ada keeps the full OTA once
    // gpu_dispatch has selected Ada, so an RTX 40 title's plugin set does not
    // change with this port.
    const OtaGpu earlyGpu = (inventoryGpu == OtaGpu::eAmpere || inventoryGpu == OtaGpu::eTuring) ? inventoryGpu : OtaGpu::eUnknown;
    const auto selectedGpu = static_cast<OtaGpu>(gpu_dispatch::Selected());
    static const bool nativeStreamlinePinned = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_NATIVE_STREAMLINE", value, 2) == 1 && value[0] == L'1';
    }();
    const streamline_ota_policy::Result policy = streamline_ota_policy::Apply(
        before, officialSixX, coherentHost, loaderDiscovery, selectedGpu, earlyGpu, nativeStreamlinePinned);
    // DLSS 5 records on the game's command lists inside its DLSS evaluations; Streamline re-binds the game's
    // state after them only while its proxies track it (GitHub issue 137).
    const streamline_state_policy::Result clState = streamline_state_policy::Apply(
        policy.flags, dlss5::Offered(), hostVersion.major);
    const uint64_t after = clState.flags;
    adjusted.flags = static_cast<sl::PreferenceFlags>(after);
    gSlInitFlagsBefore.store(before, std::memory_order_release);
    gSlInitFlagsAfter.store(after, std::memory_order_release);
    gOtaPreferencesForced.store(
        policy.allowOtaForced || policy.loadDownloadedPluginsForced,
        std::memory_order_release);
    gDownloadedStreamlinePluginsForced.store(
        policy.loadDownloadedPluginsForced, std::memory_order_release);
    gOtaProviderPreflightSupported.store(
        providerPreflight, std::memory_order_release);
    gOtaForceSuppressed.store(
        policy.fullOtaSuppressed,
        std::memory_order_release);
    gFullStreamlineOtaRequested.store(
        policy.fullOtaRequested, std::memory_order_release);
    gFullStreamlineOtaEligible.store(
        policy.fullOtaRequested && !policy.fullOtaSuppressed,
        std::memory_order_release);
    const uint64_t call =
        gSlInitCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    Log(L"Streamline slInit call=%llu sdkVersion=0x%llX "
        L"flagsBefore=0x%llX flagsAfter=0x%llX ngxOtaAllowed=%d "
        L"downloadedStreamlinePlugins=%d "
        L"loaderDiscovery=%d fullOtaRequested=%d fullOtaEligible=%d "
        L"profile=%hs nvidiaTier=%hs hostVersion=%u.%u.%u.%u "
        L"providerPreflight=%d fullOtaReason=%hs selectedGpu=%u earlyGpu=%u "
        L"nvapiGpuCount=%u nvapiArchitecture=0x%X nativeStreamlinePinned=%d clStateTracking=%hs",
        static_cast<unsigned long long>(call),
        static_cast<unsigned long long>(sdkVersion),
        static_cast<unsigned long long>(before),
        static_cast<unsigned long long>(after),
        (after & streamline_ota_policy::kAllowOta) != 0,
        (after & streamline_ota_policy::kLoadDownloadedPlugins) != 0,
        loaderDiscovery,
        policy.fullOtaRequested,
        policy.fullOtaRequested && !policy.fullOtaSuppressed,
        gNvidiaProfileName.c_str(), nvidia_mfg_policy::TierName(tier),
        hostVersion.major, hostVersion.minor, hostVersion.build,
        hostVersion.privatePart, providerPreflight, streamline_ota_policy::ReasonName(policy.reason),
        static_cast<unsigned>(selectedGpu), static_cast<unsigned>(earlyGpu), earlyHardware.physicalGpuCount,
        earlyHardware.singleGpuArchitecture, nativeStreamlinePinned,
        streamline_state_policy::ReasonName(clState.reason));
    const sl::Result result = InvokeAmpereInit(original, adjusted, sdkVersion);
    Log(L"Streamline slInit original result=%u call=%llu", static_cast<unsigned>(result),
        static_cast<unsigned long long>(call));
    ObserveOutputPullMaskSlInitResult(maskInitEntry, result == sl::Result::eOk);
    if (result == sl::Result::eOk)
    {
        // Streamline may load feature or NGX provider modules before returning.
        // Inspect them synchronously so already-resolved NGX pointers enter the
        // code-entry detours before the game can create its first FG pipeline.
        HMODULE common = GetModuleHandleW(L"sl.common.dll");
        if (common)
        {
            InstallSlCommonResolverDiscovery(
                common, LoadedModulePath(common));
        }
        InspectAlreadyLoadedModules();
    }
    return result;
}

// The Streamline entry hooks keep one original per entry, so they serve one
// interposer: the game's. Any other instance keeps its entries untouched, or a
// call could be forwarded into the wrong instance or re-enter the hooks. That
// includes the private Streamline OptiScaler loads from its OptiScaler\streamline
// folder for its own DLSS-G output, even when it loads before the game's.
bool OptiScalerPrivateStreamline(const wchar_t* path, size_t length) noexcept
{
    if (!path || !optiscaler_host::Detected())
        return false;
    const optiscaler_host::Snapshot host = optiscaler_host::Read();
    const wchar_t* name = wcsrchr(host.path, L'\\');
    if (!name)
        return false;
    const wchar_t* folder = L"OptiScaler\\streamline\\";
    const size_t base = static_cast<size_t>(name - host.path) + 1;
    const size_t folderLength = wcslen(folder);
    return length > base + folderLength && _wcsnicmp(path, host.path, base) == 0
        && _wcsnicmp(path + base, folder, folderLength) == 0;
}

// Binds the first eligible interposer. Loader-lock safe given the mapped path.
bool HostInterposer(HMODULE interposer, const wchar_t* path, size_t length) noexcept
{
    if (!interposer || OptiScalerPrivateStreamline(path, length))
        return false;
    HMODULE expected = nullptr;
    return gHostInterposer.compare_exchange_strong(expected, interposer,
        std::memory_order_acq_rel) || expected == interposer;
}

bool HostInterposer(HMODULE interposer, const std::wstring& path) noexcept
{
    return HostInterposer(interposer, path.c_str(), path.size());
}

// Manual-hooking hosts (Streamline eUseManualHooking, e.g. Wuthering Waves)
// create native DXGI objects themselves, so no factory or swapchain creation
// reaches the menu. Streamline returns its factory proxy here; the menu wraps
// it exactly as it wraps a creation return, and so sees the swapchains.
sl::Result HookSlUpgradeInterface(void** baseInterface)
{
    auto* original = EntryOriginal(gOriginalSlUpgradeInterface,
        gSlUpgradeInterfaceEntryHandle);
    if (!original || original == &HookSlUpgradeInterface)
        return sl::Result::eErrorNotInitialized;
    const bool owned = baseInterface
        && single_overlay::OwnsInterface(*baseInterface);
    // Every upgrade (first 32): who asked, what went in, what came
    // back. Shows whether the swap chain's factory went through Streamline.
    void* const caller = _ReturnAddress();
    void* const input = baseInterface ? *baseInterface : nullptr;
    const bool describe = gSlUpgradeInterfaceCalls.load(std::memory_order_relaxed) < 32;
    wchar_t inputText[256]{};
    if (describe)
        single_overlay::DescribeInterface(input, inputText, 256);
    const sl::Result result = original(baseInterface);
    const uint64_t call =
        gSlUpgradeInterfaceCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (describe && call <= 32)
    {
        void* const output = baseInterface ? *baseInterface : nullptr;
        wchar_t outputText[256]{};
        if (output != input)
            single_overlay::DescribeInterface(output, outputText, 256);
        else
            wcscpy_s(outputText, L"unchanged");
        HMODULE callerModule = nullptr;
        wchar_t callerPath[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(caller), &callerModule)
            && callerModule)
            GetModuleFileNameW(callerModule, callerPath, MAX_PATH);
        const wchar_t* callerLeaf = wcsrchr(callerPath, L'\\');
        Log(L"Streamline slUpgradeInterface call=%llu result=%d "
            L"ownedInput=%d caller=%ls+0x%llX input={%ls} output={%ls}",
            static_cast<unsigned long long>(call), static_cast<int>(result),
            owned ? 1 : 0,
            callerLeaf ? callerLeaf + 1 : (callerPath[0] ? callerPath : L"(none)"),
            static_cast<unsigned long long>(callerModule
                ? reinterpret_cast<uintptr_t>(caller)
                    - reinterpret_cast<uintptr_t>(callerModule) : 0),
            inputText, outputText);
    }
    if (result == sl::Result::eOk && !owned && baseInterface && *baseInterface
        && single_overlay::WrapUpgradedInterface(baseInterface))
    {
        const uint64_t wrapped = gSlUpgradeInterfaceWrapped.fetch_add(1,
            std::memory_order_relaxed) + 1;
        Log(L"Streamline slUpgradeInterface: upgraded DXGI factory wrapped "
            L"for the menu call=%llu wrapped=%llu",
            static_cast<unsigned long long>(call),
            static_cast<unsigned long long>(wrapped));
    }
    return result;
}

bool TryInstallSlUpgradeInterfaceEntryDetour(HMODULE interposer,
    void* resolvedTarget)
{
    if (!interposer)
        return false;
    const std::wstring path = LoadedModulePath(interposer);
    if (!ModuleFileNameEquals(path, L"sl.interposer.dll")
        || !HostInterposer(interposer, path))
        return false;
    void* target = resolvedTarget ? resolvedTarget
        : reinterpret_cast<void*>(
            GetProcAddress(interposer, "slUpgradeInterface"));
    if (!target || ModuleFromAddress(target) != interposer)
        return false;

    // No version-dependent entry pattern is known; use the relocation path.
    entry_detour::InstallOptions options{};
    options.allowRelocated = true;
    void* trampoline = nullptr;
    const bool installed = entry_detour::Install(
        entry_detour::Kind::eSlUpgradeInterface, interposer, target,
        reinterpret_cast<void*>(&HookSlUpgradeInterface), trampoline,
        options, &gSlUpgradeInterfaceEntryHandle);
    if (trampoline)
    {
        gOriginalSlUpgradeInterface.store(
            reinterpret_cast<PFun_slUpgradeInterface*>(trampoline),
            std::memory_order_release);
    }
    static std::atomic<uint32_t> logged{0};
    const uint32_t outcome = installed ? 1u : 2u;
    if (logged.exchange(outcome, std::memory_order_acq_rel) != outcome)
    {
        const entry_detour::Snapshot state =
            entry_detour::ReadSnapshot(gSlUpgradeInterfaceEntryHandle);
        Log(L"Streamline slUpgradeInterface entry detour: installed=%d "
            L"current=%d method=%hs failure=%u targetRva=0x%X path=%s",
            installed, state.current, entry_detour::MethodName(state.method),
            static_cast<uint32_t>(state.failure), state.targetRva,
            path.c_str());
    }
    return installed;
}

bool TryInstallSlSetD3DDeviceEntryDetour(HMODULE interposer)
{
    if (!interposer)
        return false;
    const std::wstring path = LoadedModulePath(interposer);
    if (!ModuleFileNameEquals(path, L"sl.interposer.dll")
        || !HostInterposer(interposer, path))
        return false;
    void* target = reinterpret_cast<void*>(
        GetProcAddress(interposer, "slSetD3DDevice"));
    if (!target || ModuleFromAddress(target) != interposer)
        return false;

    // No version-dependent entry pattern is known; use the relocation path.
    entry_detour::InstallOptions options{};
    options.allowRelocated = true;
    void* trampoline = nullptr;
    const bool installed = entry_detour::Install(
        entry_detour::Kind::eSlSetD3DDevice, interposer, target,
        reinterpret_cast<void*>(&HookSlSetD3DDevice), trampoline,
        options, &gSlSetD3DDeviceEntryHandle);
    if (trampoline)
    {
        gOriginalSlSetD3DDeviceEntry.store(
            reinterpret_cast<PFun_slSetD3DDevice*>(trampoline),
            std::memory_order_release);
    }
    static std::atomic<uint32_t> logged{0};
    const uint32_t outcome = installed ? 1u : 2u;
    if (logged.exchange(outcome, std::memory_order_acq_rel) != outcome)
    {
        const entry_detour::Snapshot state =
            entry_detour::ReadSnapshot(gSlSetD3DDeviceEntryHandle);
        Log(L"Streamline slSetD3DDevice entry detour: installed=%d "
            L"current=%d method=%hs failure=%u targetRva=0x%X path=%s",
            installed, state.current, entry_detour::MethodName(state.method),
            static_cast<uint32_t>(state.failure), state.targetRva,
            path.c_str());
    }
    return installed;
}

// RTX 30 systems: the interposer's slSetVulkanInfo (hosts that resolve its
// exports themselves) and its vkCreateDevice (interposer-loaded Vulkan hosts,
// which never call slSetVulkanInfo) are the Vulkan startup brackets.
bool TryInstallSlVulkanEntryDetours(HMODULE interposer)
{
    if (!interposer || !ampere_provider_arch::Armed())
        return false;
    const std::wstring path = LoadedModulePath(interposer);
    if (!ModuleFileNameEquals(path, L"sl.interposer.dll")
        || !HostInterposer(interposer, path))
        return false;
    struct Entry
    {
        const char* name;
        entry_detour::Kind kind;
        void* hook;
        entry_detour::Handle* handle;
    };
    const Entry entries[] = {
        {"slSetVulkanInfo", entry_detour::Kind::eSlSetVulkanInfo,
            reinterpret_cast<void*>(&HookSlSetVulkanInfo), &gSlSetVulkanInfoEntryHandle},
        {"vkCreateDevice", entry_detour::Kind::eSlVulkanCreateDevice,
            reinterpret_cast<void*>(&HookSlVulkanCreateDevice), &gSlVulkanCreateDeviceEntryHandle},
    };
    bool installedAll = true;
    for (const Entry& entry : entries)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(interposer, entry.name));
        if (!target || ModuleFromAddress(target) != interposer)
        {
            installedAll = false;
            continue;
        }
        // No version-dependent entry pattern is known; use the relocation path.
        entry_detour::InstallOptions options{};
        options.allowRelocated = true;
        void* trampoline = nullptr;
        const bool installed = entry_detour::Install(entry.kind, interposer, target,
            entry.hook, trampoline, options, entry.handle);
        if (trampoline && entry.kind == entry_detour::Kind::eSlSetVulkanInfo)
            gOriginalSlSetVulkanInfoEntry.store(
                reinterpret_cast<PFun_slSetVulkanInfoAbi*>(trampoline), std::memory_order_release);
        if (trampoline && entry.kind == entry_detour::Kind::eSlVulkanCreateDevice)
            gOriginalSlVulkanCreateDevice.store(
                reinterpret_cast<PFun_vkCreateDeviceAbi*>(trampoline), std::memory_order_release);
        installedAll = installedAll && installed;
        static std::atomic<uint32_t> logged[2]{};
        const uint32_t outcome = installed ? 1u : 2u;
        auto& slot = logged[entry.kind == entry_detour::Kind::eSlSetVulkanInfo ? 0 : 1];
        if (slot.exchange(outcome, std::memory_order_acq_rel) != outcome)
        {
            const entry_detour::Snapshot state = entry_detour::ReadSnapshot(*entry.handle);
            Log(L"Streamline %hs entry detour (RTX 30 Vulkan startup): installed=%d "
                L"current=%d method=%hs failure=%u targetRva=0x%X path=%s",
                entry.name, installed, state.current, entry_detour::MethodName(state.method),
                static_cast<uint32_t>(state.failure), state.targetRva, path.c_str());
        }
    }
    return installedAll;
}

// Loader notification for OptiScaler's private sl.interposer.dll, with the
// loader lock held. OptiScaler loads it and calls slInit and slSetD3DDevice
// straight away, before discovery would see it. Hotpatch only (the 2.14 entry
// profile), never waits for the detour registry and never logs.
void InstallOptiScalerSlSetD3DDeviceAtLoad(HMODULE interposer,
    const UNICODE_STRING* path) noexcept
{
    if (!interposer || !path || !path->Buffer
        || !OptiScalerPrivateStreamline(path->Buffer, path->Length / sizeof(wchar_t)))
        return;
    void* target = MappedExportAddress(interposer, "slSetD3DDevice");
    if (!target)
        return;
    entry_detour::InstallOptions options{};
    options.nonBlocking = true;
    void* trampoline = nullptr;
    entry_detour::Handle handle{};
    bool installed = false;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        installed = entry_detour::Install(entry_detour::Kind::eOptiScalerSlSetD3DDevice,
            interposer, target, reinterpret_cast<void*>(&HookOptiScalerSlSetD3DDevice),
            trampoline, options, &handle);
        if (installed || handle)
            break;
        Sleep(1);
    }
    if (trampoline)
        gOriginalOptiScalerSlSetD3DDevice.store(
            reinterpret_cast<PFun_slSetD3DDevice*>(trampoline), std::memory_order_release);
    if (installed)
    {
        gOptiScalerSlSetD3DDeviceEntryHandle = handle;
        gOptiScalerSlSetD3DDeviceArmedAtLoad.store(ampere_provider_arch::Armed(),
            std::memory_order_release);
        gOptiScalerSlSetD3DDeviceAtLoad.store(true, std::memory_order_release);
    }
}

// Discovery path for OptiScaler's private interposer: keeps the load-time
// hotpatch, or installs the entry (relocation allowed) when that profile did
// not match. Its slInit and other entries stay untouched.
bool TryInstallOptiScalerSlSetD3DDeviceEntryDetour(HMODULE interposer,
    const std::wstring& path)
{
    if (!interposer || !OptiScalerPrivateStreamline(path.c_str(), path.size()))
        return false;
    void* target = reinterpret_cast<void*>(
        GetProcAddress(interposer, "slSetD3DDevice"));
    if (!target || ModuleFromAddress(target) != interposer)
        return false;
    entry_detour::InstallOptions options{};
    options.allowRelocated = true;
    void* trampoline = nullptr;
    const bool installed = entry_detour::Install(
        entry_detour::Kind::eOptiScalerSlSetD3DDevice, interposer, target,
        reinterpret_cast<void*>(&HookOptiScalerSlSetD3DDevice), trampoline,
        options, &gOptiScalerSlSetD3DDeviceEntryHandle);
    if (trampoline)
        gOriginalOptiScalerSlSetD3DDevice.store(
            reinterpret_cast<PFun_slSetD3DDevice*>(trampoline), std::memory_order_release);
    static std::atomic<uint32_t> logged{0};
    const uint32_t outcome = installed ? 1u : 2u;
    if (logged.exchange(outcome, std::memory_order_acq_rel) != outcome)
    {
        const entry_detour::Snapshot state =
            entry_detour::ReadSnapshot(gOptiScalerSlSetD3DDeviceEntryHandle);
        Log(L"OptiScaler private Streamline slSetD3DDevice entry detour: installed=%d "
            L"current=%d atLoad=%d method=%hs failure=%u targetRva=0x%X path=%s",
            installed, state.current,
            gOptiScalerSlSetD3DDeviceAtLoad.load(std::memory_order_acquire),
            entry_detour::MethodName(state.method), static_cast<uint32_t>(state.failure),
            state.targetRva, path.c_str());
    }
    return installed;
}

bool TryInstallSlInitEntryDetour(HMODULE interposer, void* resolvedTarget)
{
    if (!interposer)
        return false;
    const std::wstring path = LoadedModulePath(interposer);
    if (!ModuleFileNameEquals(path, L"sl.interposer.dll"))
        return false;
    if (!HostInterposer(interposer, path))
    {
        // OptiScaler's private instance: only its own slSetD3DDevice entry, so
        // Ada's wrapper maximum is patched before its plug-ins start.
        const bool optiScaler = TryInstallOptiScalerSlSetD3DDeviceEntryDetour(interposer, path);
        static std::atomic<HMODULE> logged{nullptr};
        if (logged.exchange(interposer, std::memory_order_acq_rel) != interposer)
            Log(optiScaler
                    ? L"Streamline interposer is OptiScaler's private instance; only its slSetD3DDevice "
                      L"is hooked (not the game's instance): path=%s"
                    : L"Streamline interposer left untouched (not the game's instance): path=%s",
                path.c_str());
        return false;
    }
    TryInstallSlUpgradeInterfaceEntryDetour(interposer, nullptr);
    TryInstallSlSetD3DDeviceEntryDetour(interposer);
    void* target = resolvedTarget ? resolvedTarget
        : reinterpret_cast<void*>(GetProcAddress(interposer, "slInit"));
    if (!target || ModuleFromAddress(target) != interposer)
        return false;

    void* trampoline = nullptr;
    const bool installed = entry_detour::Install(
        entry_detour::Kind::eSlInit, interposer, target,
        reinterpret_cast<void*>(&HookSlInit), trampoline,
        entry_detour::InstallOptions{}, &gSlInitEntryHandle);
    if (trampoline)
    {
        gOriginalSlInit.store(reinterpret_cast<PFun_slInit*>(trampoline),
            std::memory_order_release);
    }
    const entry_detour::Snapshot state = entry_detour::ReadSnapshot(
        entry_detour::Kind::eSlInit);
    LogOnChange(L"Streamline slInit entry detour: installed=%d current=%d "
        L"cachedPointersCovered=%d failure=%u targetRva=0x%X path=%s",
        installed, state.current, state.cachedPointersCovered,
        static_cast<uint32_t>(state.failure), state.targetRva,
        path.c_str());
    return installed;
}

// The NGX Vulkan Init exports (the runtime's and the providers' adapter entries).
bool IsNgxVulkanInitEntry(const char* name) noexcept
{
    constexpr const char* kNames[] = {"NVSDK_NGX_VULKAN_Init", "NVSDK_NGX_VULKAN_Init_Ext",
        "NVSDK_NGX_VULKAN_Init_Ext2", "NVSDK_NGX_VULKAN_Init_with_ProjectID",
        "NVSDK_NGX_VULKAN_Init_ProjectID", "NVSDK_NGX_VULKAN_Init_ProjectID_Ext"};
    for (const char* entry : kNames)
        if (std::strcmp(name, entry) == 0) return true;
    return false;
}

// dx11nr.1: the NGX D3D11 Init exports. A DirectX 11 game may create its DLSS feature straight after Init, so the
// runtime route, whose D3D11 entries carry DLSS 5, is installed when Init is resolved rather than on the discovery
// worker's poll. Apart from ngx_initialization::IsEntry: the D3D12 Init observer never sees these.
bool IsNgxD3D11InitEntry(const char* name) noexcept
{
    constexpr const char* kNames[] = {"NVSDK_NGX_D3D11_Init", "NVSDK_NGX_D3D11_Init_Ext",
        "NVSDK_NGX_D3D11_Init_ProjectID", "NVSDK_NGX_D3D11_Init_with_ProjectID", "NVSDK_NGX_D3D11_Init_ProjectID_Ext"};
    for (const char* entry : kNames)
        if (std::strcmp(name, entry) == 0) return true;
    return false;
}

FARPROC WINAPI HookMainGetProcAddress(HMODULE module, LPCSTR functionName)
{
    GetProcAddressFn original = gOriginalMainGetProcAddress.load(
        std::memory_order_acquire);
    if (!original)
        return nullptr;
    FARPROC resolved = original(module, functionName);
    resolved = fault_capture::ResolveProc(module, functionName, resolved);
    // The NGX SDK library in the executable loads the runtime and calls its
    // Init straight after resolving it: install the module's NGX entries
    // (Init pre-calls included) now, not on the discovery worker, and the
    // Vulkan Init entries too; their pre-call verifies the VkPhysicalDevice. The D3D11 Init too (dx11nr.1).
    if (resolved && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && (ngx_initialization::IsEntry(functionName) || IsNgxVulkanInitEntry(functionName)
            || IsNgxD3D11InitEntry(functionName)))
        InspectLoadedModule(module, LoadedModulePath(module));
    if (resolved && NativeDlssgPresetGpu()) dlssg_preset::PrepareForExport(module, functionName);
    if (resolved) single_overlay::application_imports::ArmModule(module);
    resolved = single_overlay::ResolveProc(module, functionName, resolved);
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu)
    {
        return resolved;
    }
    // Only the game's Streamline instance is routed (see HostInterposer).
    const std::wstring interposerPath = LoadedModulePath(module);
    if (!ModuleFileNameEquals(interposerPath, L"sl.interposer.dll")
        || !HostInterposer(module, interposerPath))
    {
        return resolved;
    }

    if (std::strcmp(functionName, "slGetFeatureFunction") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slGetFeatureFunction*>(resolved);
        if (candidate == &HookSlGetFeatureFunction)
            return resolved;
        if (ModuleFromAddress(reinterpret_cast<void*>(resolved)) != module)
            return resolved;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(resolved), &pinned) || pinned != module)
            return resolved;
        auto* expected = static_cast<PFun_slGetFeatureFunction*>(nullptr);
        if (!gOriginalGetFeatureFunction.compare_exchange_strong(expected, candidate,
                std::memory_order_acq_rel) && expected != candidate)
            return resolved;
        if (!expected)
            Log(L"Streamline feature-function dynamic gateway installed: path=%s",
                LoadedModulePath(module).c_str());
        return reinterpret_cast<FARPROC>(&HookSlGetFeatureFunction);
    }

    if (std::strcmp(functionName, "slSetD3DDevice") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetD3DDevice*>(resolved);
        if (candidate != &HookSlSetD3DDevice) gOriginalSetD3DDevice.store(candidate);
        return reinterpret_cast<FARPROC>(&HookSlSetD3DDevice);
    }

    if (std::strcmp(functionName, "slSetVulkanInfo") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetVulkanInfoAbi*>(
            resolved);
        if (candidate != &HookSlSetVulkanInfo)
            gOriginalSetVulkanInfo.store(candidate,
                std::memory_order_release);
        return reinterpret_cast<FARPROC>(&HookSlSetVulkanInfo);
    }

    // Engines that load the interposer themselves (Unreal's Streamline
    // plugin) resolve the tagging entries here rather than through imports.
    if (std::strcmp(functionName, "slSetTag") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetTag*>(resolved);
        if (candidate != &HookSlSetTag)
            gOriginalSetTag.store(candidate, std::memory_order_release);
        if (!gUiTagHookInstalled.exchange(true, std::memory_order_acq_rel))
            Log(L"Streamline UI tag dynamic gateway installed: slSetTag path=%s",
                LoadedModulePath(module).c_str());
        return reinterpret_cast<FARPROC>(&HookSlSetTag);
    }
    if (std::strcmp(functionName, "slSetTagForFrame") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetTagForFrame*>(resolved);
        if (candidate != &HookSlSetTagForFrame)
            gOriginalSetTagForFrame.store(candidate, std::memory_order_release);
        if (!gUiTagHookInstalled.exchange(true, std::memory_order_acq_rel))
            Log(L"Streamline UI tag dynamic gateway installed: slSetTagForFrame path=%s",
                LoadedModulePath(module).c_str());
        return reinterpret_cast<FARPROC>(&HookSlSetTagForFrame);
    }
    if (std::strcmp(functionName, "slSetConstants") == 0)
    {
        auto* candidate = reinterpret_cast<PFun_slSetConstants*>(resolved);
        if (candidate != &HookSlSetConstants)
            gOriginalSetConstants.store(candidate, std::memory_order_release);
        return reinterpret_cast<FARPROC>(&HookSlSetConstants);
    }
    if (std::strcmp(functionName, "slUpgradeInterface") == 0)
    {
        if (TryInstallSlUpgradeInterfaceEntryDetour(module,
                reinterpret_cast<void*>(resolved)))
            return resolved;
        auto* candidate = reinterpret_cast<PFun_slUpgradeInterface*>(resolved);
        if (candidate != &HookSlUpgradeInterface)
            gOriginalSlUpgradeInterface.store(candidate,
                std::memory_order_release);
        gSlUpgradeInterfaceResolverFallbackActive.store(true,
            std::memory_order_release);
        return reinterpret_cast<FARPROC>(&HookSlUpgradeInterface);
    }
    if (std::strcmp(functionName, "slInit") != 0)
        return resolved;

    if (TryInstallSlInitEntryDetour(module,
            reinterpret_cast<void*>(resolved)))
    {
        return resolved;
    }
    // An entry detour that is installed but no longer verifies still routes the export into HookSlInit.
    // Keep its trampoline as the original and hand out the export. Storing the export as the original and returning
    // HookSlInit would make our hook and OptiScaler's call each other until the stack overflows.
    if (auto* trampoline = SlInitEntryTrampoline(reinterpret_cast<void*>(resolved)))
    {
        gOriginalSlInit.store(trampoline, std::memory_order_release);
        return resolved;
    }

    auto* candidate = reinterpret_cast<PFun_slInit*>(resolved);
    if (candidate != &HookSlInit)
        gOriginalSlInit.store(candidate, std::memory_order_release);
    gSlInitResolverFallbackActive.store(true, std::memory_order_release);
    return reinterpret_cast<FARPROC>(&HookSlInit);
}

bool InstallMainResolverDiscovery()
{
    void* original = nullptr;
    const char* route = "KERNEL32.dll";
    bool installed = HookMainExecutableImport(route,
        "GetProcAddress", reinterpret_cast<void*>(&HookMainGetProcAddress),
        original);
    if (!installed)
    {
        // apisetgpa.1: an executable linked against mincore or OneCore (shadPS4) has no KERNEL32.dll import of
        // GetProcAddress. Its API-set import gets the same pointer publication; without it a Vulkan game's
        // instance and device never reach the wraps the XeMFG DLSS-G provider records them from.
        route = LoaderContractImporting(GetModuleHandleW(nullptr), "GetProcAddress");
        if (route)
            installed = HookMainExecutableImport(route, "GetProcAddress",
                reinterpret_cast<void*>(&HookMainGetProcAddress), original);
    }
    if (!installed)
    {
        // GTA Enhanced has four-byte-aligned x64 imports. Preserve the strict
        // pointer publisher and the IAT; intercept only main-executable callers
        // at the identified public resolver entry instead.
        HMODULE executable = GetModuleHandleW(nullptr);
        single_overlay::slots::VisitImports(executable, [&](const char* name, void** slot) {
            if (strcmp(name, "GetProcAddress")) return true;
            void* target = nullptr;
            memcpy(&target, slot, sizeof(target));
            if (!caller_scoped_import::Eligible(executable, slot, target, name)) return true;
            void* trampoline = nullptr;
            const auto replacement = reinterpret_cast<void*>(&HookMainGetProcAddress);
            if (caller_scoped_import::Prepare(executable, slot, target,replacement,name,trampoline)) {
                gOriginalMainGetProcAddress.store(reinterpret_cast<GetProcAddressFn>(trampoline),std::memory_order_release);
                installed = caller_scoped_import::Activate(target,replacement);
                if (installed) { original = trampoline; route = "public entry"; }
            }
            return false;
        });
    }
    if (original)
    {
        const auto candidate = reinterpret_cast<GetProcAddressFn>(original);
        GetProcAddressFn expected = nullptr;
        if (!gOriginalMainGetProcAddress.compare_exchange_strong(
                expected, candidate, std::memory_order_acq_rel,
                std::memory_order_acquire)
            && expected != candidate)
        {
            return false;
        }
    }
    gMainResolverImportRoute.store(installed ? route : nullptr, std::memory_order_release);
    gMainResolverDiscoveryInstalled.store(installed,
        std::memory_order_release);
    return installed;
}

bool InstallSlInitIatFallback()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slInit", reinterpret_cast<void*>(&HookSlInit), original);
    if (original)
    {
        auto* candidate = reinterpret_cast<PFun_slInit*>(original);
        if (candidate != &HookSlInit)
            gOriginalSlInit.store(candidate, std::memory_order_release);
    }
    gSlInitIatFallbackInstalled.store(installed,
        std::memory_order_release);
    return installed;
}

bool InstallSlInitControlPath()
{
    const bool resolverDiscovery = InstallMainResolverDiscovery();
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    const bool entryInstalled = interposer
        && TryInstallSlInitEntryDetour(interposer, nullptr);
    // No IAT fallback while an installed entry detour still routes the export here.
    const bool iatFallback = entryInstalled || SlInitEntryTrampoline(nullptr)
        ? false : InstallSlInitIatFallback();
    return entryInstalled || iatFallback || resolverDiscovery;
}

FARPROC WINAPI HookSlCommonGetProcAddress(
    HMODULE module, LPCSTR functionName)
{
    GetProcAddressFn original = gOriginalSlCommonGetProcAddress.load(
        std::memory_order_acquire);
    if (!original)
        return nullptr;
    FARPROC resolved = original(module, functionName);
    // RTX 30: the DLSS-G wrapper's flip-metering probe (no-op otherwise).
    resolved = AmpereWrapperNvapiView(module, functionName, resolved, _ReturnAddress());
    // Vulkan is often resolved inside Streamline rather than the executable.
    // Reuse its established resolver boundary without wrapping internal DXGI
    // factories or changing shared Streamline/native method implementations.
    if (functionName && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && functionName[0] == 'v' && functionName[1] == 'k')
        resolved = single_overlay::ResolveProc(module, functionName, resolved);
    if (resolved && reinterpret_cast<uintptr_t>(functionName) > 0xFFFFu
        && (ngx_initialization::IsEntry(functionName) || IsNgxD3D11InitEntry(functionName)))
        InspectLoadedModule(module, LoadedModulePath(module));
    if (resolved && NativeDlssgPresetGpu()) dlssg_preset::PrepareForExport(module, functionName);
    if (!resolved || !functionName
        || reinterpret_cast<uintptr_t>(functionName) <= 0xFFFFu)
        return resolved;
    const bool create = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_CreateFeature") == 0;
    const bool evaluate = std::strcmp(functionName,
        "NVSDK_NGX_D3D12_EvaluateFeature") == 0;
    const bool ampereStartup = true
        && (std::strcmp(functionName, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0
            || std::strcmp(functionName, "NVSDK_NGX_D3D12_GetCapabilityParameters") == 0);
    const bool vulkan = std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_CreateFeature") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_CreateFeature1") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_EvaluateFeature") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_Ext") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_Ext2") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_with_ProjectID") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_ProjectID") == 0
        || std::strcmp(functionName,
            "NVSDK_NGX_VULKAN_Init_ProjectID_Ext") == 0;
    if (!create && !evaluate && !vulkan && !ampereStartup)
        return resolved;

    // This resolver interception is discovery only. The raw function pointer
    // is returned unchanged after the implementation entry itself has been
    // atomically detoured, so already-cached and newly-resolved pointers take
    // the same route.
    const std::wstring path = LoadedModulePath(module);
    const ModuleRecord record = InspectLoadedModule(module, path);
    if (record.ngxExport)
    {
        TryInstallNgxCreateEntryDetour(module, path, record.generation);
        TryInstallNgxEvaluateEntryDetour(module, path, record.generation);
        ampere_backend::InstallVulkanRoute(module, path.c_str(), record.generation, false);
        TryInstallNgxVulkanCreateEntryDetours(
            module, path, record.generation);
        TryInstallNgxVulkanEvaluateEntryDetour(
            module, path, record.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            module, path, record.generation);
    }
    if (record.ngxRuntimeExport)
    {
        TryInstallNgxRuntimeCreateEntryDetour(
            module, path, record.generation);
        TryInstallNgxRuntimeEvaluateEntryDetour(
            module, path, record.generation);
        if (IsNgxRuntimeModule(module))
            ampere_backend::InstallVulkanRoute(module, path.c_str(), record.generation, true);
        TryInstallNgxRuntimeVulkanCreateEntryDetours(
            module, path, record.generation);
        TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
            module, path, record.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            module, path, record.generation);
    }
    return resolved;
}

bool InstallSlCommonResolverDiscovery(
    HMODULE module, const std::wstring& path)
{
    if (!module)
        return false;
    void* original = nullptr;
    const bool installed = HookModuleImport(module, "KERNEL32.dll",
        "GetProcAddress", reinterpret_cast<void*>(&HookSlCommonGetProcAddress),
        original);
    if (original)
    {
        GetProcAddressFn expected = nullptr;
        const auto candidate = reinterpret_cast<GetProcAddressFn>(original);
        if (!gOriginalSlCommonGetProcAddress.compare_exchange_strong(
                expected, candidate, std::memory_order_acq_rel,
                std::memory_order_acquire)
            && expected != candidate)
            return false;
    }
    if (installed)
    {
        gSlCommonResolverDiscoveryInstalled.store(
            true, std::memory_order_release);
        if (original)
        {
            Log(L"Streamline NGX resolver discovery installed: %s",
                path.c_str());
        }
        std::lock_guard lock(gModuleMutex);
        for (auto& record : gModuleRecords)
        {
            if (record.module == module)
            {
                record.createResolverDiscoveryHooked = true;
                break;
            }
        }
    }
    return installed;
}

bool InstallFeatureFunctionHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slGetFeatureFunction", reinterpret_cast<void*>(&HookSlGetFeatureFunction), original);
    if (original)
    {
        gOriginalGetFeatureFunction.store(
            reinterpret_cast<PFun_slGetFeatureFunction*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallD3DDeviceHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slSetD3DDevice", reinterpret_cast<void*>(&HookSlSetD3DDevice), original);
    if (original)
    {
        gOriginalSetD3DDevice.store(
            reinterpret_cast<PFun_slSetD3DDevice*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallVulkanInfoHook()
{
    void* original = nullptr;
    const bool installed = HookMainExecutableImport("sl.interposer.dll",
        "slSetVulkanInfo", reinterpret_cast<void*>(&HookSlSetVulkanInfo),
        original);
    if (original)
    {
        gOriginalSetVulkanInfo.store(
            reinterpret_cast<PFun_slSetVulkanInfoAbi*>(original),
            std::memory_order_release);
    }
    return installed;
}

bool InstallUiTagHooks()
{
    void* legacyOriginal = nullptr;
    const bool legacyInstalled = HookMainExecutableImport("sl.interposer.dll",
        "slSetTag", reinterpret_cast<void*>(&HookSlSetTag), legacyOriginal);
    if (legacyOriginal)
    {
        gOriginalSetTag.store(reinterpret_cast<PFun_slSetTag*>(legacyOriginal),
            std::memory_order_release);
    }

    void* frameOriginal = nullptr;
    const bool frameInstalled = HookMainExecutableImport("sl.interposer.dll",
        "slSetTagForFrame", reinterpret_cast<void*>(&HookSlSetTagForFrame), frameOriginal);
    if (frameOriginal)
    {
        gOriginalSetTagForFrame.store(
            reinterpret_cast<PFun_slSetTagForFrame*>(frameOriginal),
            std::memory_order_release);
    }

    // The camera constants beside the tags (XeMFG standalone; forwards unchanged otherwise).
    void* constantsOriginal = nullptr;
    HookMainExecutableImport("sl.interposer.dll", "slSetConstants",
        reinterpret_cast<void*>(&HookSlSetConstants), constantsOriginal);
    if (constantsOriginal)
    {
        gOriginalSetConstants.store(reinterpret_cast<PFun_slSetConstants*>(constantsOriginal),
            std::memory_order_release);
    }

    const bool installed = legacyInstalled || frameInstalled;
    gUiTagHookInstalled.store(installed, std::memory_order_release);
    return installed;
}

static constexpr std::array<uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
static constexpr std::array<uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };

struct PatternPatchResult
{
    bool candidate = false;
    bool patched = false;
    uint8_t* match = nullptr;
    uint32_t profileMaximum = 0;
};

const IMAGE_NT_HEADERS64* ImageHeaders(HMODULE module)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    if (!base)
        return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || static_cast<size_t>(dos->e_lfanew) > 1024 * 1024)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    return nt;
}

bool RvaRangeIsValid(const IMAGE_NT_HEADERS64* nt, DWORD rva, size_t size)
{
    return nt && rva < nt->OptionalHeader.SizeOfImage
        && size <= static_cast<size_t>(nt->OptionalHeader.SizeOfImage - rva);
}

bool ModuleExportsFunction(HMODULE module, const char* expected)
{
    const auto* nt = ImageHeaders(module);
    if (!nt || !expected)
        return false;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress
        || !RvaRangeIsValid(nt, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return false;

    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    const size_t namesSize = static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD);
    if (!exports->AddressOfNames
        || !RvaRangeIsValid(nt, exports->AddressOfNames, namesSize))
        return false;

    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index)
    {
        const DWORD nameRva = names[index];
        if (!RvaRangeIsValid(nt, nameRva, 1))
            continue;
        const char* name = reinterpret_cast<const char*>(base + nameRva);
        const size_t remaining = nt->OptionalHeader.SizeOfImage - nameRva;
        const size_t length = strnlen_s(name, remaining);
        if (length < remaining && strcmp(name, expected) == 0)
            return true;
    }
    return false;
}

PatternPatchResult PatchNgxMfgGate(HMODULE module, const std::wstring& path)
{
    const bool wasReady = ngx_mfg_gate::Ready(module);
    const auto result = ngx_mfg_gate::Patch(module, gpu_dispatch::IsAda()
        && (gpu_backend::AdapterVerified() || gVulkanAdapterVerified.load(std::memory_order_acquire)));
    if (result.patched && !wasReady)
        Log(L"NGX MFG count/index gate restored with verified atomic publication at RVA 0x%zX: %s",
            static_cast<size_t>(result.match + 2 - reinterpret_cast<uint8_t*>(module)), path.c_str());
    // Only a supported provider counts: with the check site found (whether or not patched yet) or without one. An
    // inspection decides only until a frame-generation Create names the provider in use (ada_gate_hold.h).
    if (gpu_dispatch::IsAda())
    {
        const bool missing = !result.candidate && ngx_mfg_gate::Missing(module);
        if (missing || result.candidate)
            NoteAdaProviderGate(missing, path, ada_gate_hold::Source::eInspection);
    }
    return {result.candidate, result.patched, result.match};
}

// A provider's count/index gate on the selected GPU. RTX 40 restores it; RTX
// 50 accepts multi-frame counts natively, so its supported provider is only
// identified and counts as ready with nothing written.
PatternPatchResult InspectNgxMfgGate(HMODULE module, const std::wstring& path)
{
    if (gpu_dispatch::IsBlackwell())
    {
        const bool supported = dlssg_provider_policy::IsSupportedRetainedProvider(module);
        return {supported, supported, nullptr};
    }
    return gpu_dispatch::IsAda() ? PatchNgxMfgGate(module, path) : PatternPatchResult{true, false, nullptr};
}

// The RTX 30 legacy-wrapper (2x) audit runs on the selected Ampere adapter, or
// before adapter selection when NVAPI found an RTX 30 GPU. Ada and systems
// without an RTX 30 GPU skip it.
bool AmpereWrapperAuditApplies() noexcept
{
    return UseAmpere() || (gpu_dispatch::Selected() == gpu_dispatch::Family::eUnknown
        && ampere_provider_arch::Armed());
}

// RTX 30: the DLSS-G wrapper's view of NVAPI hides flip metering
// (ampere_flip_metering.h). The wrapper's GetProcAddress import already runs
// through HookSlCommonGetProcAddress (resolver discovery covers every
// Streamline plug-in), so the view is applied there, not by a second hook on
// the same import slot, which resolver discovery on the same loader return
// would replace before it ever ran. Only calls from a registered DLSS-G
// wrapper, only nvapi_QueryInterface of NVIDIA's dispatcher, and only the two
// flip-metering interfaces are affected.
bool AmpereFilteredWrapper(HMODULE module) noexcept
{
    if (!module) return false;
    for (const auto& wrapper : gAmpereFilteredWrappers)
        if (wrapper.load(std::memory_order_acquire) == module) return true;
    return false;
}

// [Compatibility] Rtx30FlipMetering in RTX40MFG-UI.ini beside the game
// executable (on by default), read once on the first wrapper's
// loader return (before its startup probe); the choice holds for the process.
void ConfigureAmpereFlipMetering()
{
    if (ampere_flip_metering::Configured()) return;
    const std::wstring path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    const auto setting = ampere_flip_metering::ReadSetting(path.c_str());
    const bool enabled = ampere_flip_metering::Enabled(setting);
    ampere_flip_metering::Configure(enabled);
    const wchar_t* source = setting == ampere_flip_metering::Setting::eOn ? L"set to 1"
        : setting == ampere_flip_metering::Setting::eOff ? L"set to 0"
        : setting == ampere_flip_metering::Setting::eInvalid ? L"not 1 or 0, default used" : L"not set, default used";
    Log(L"Ampere flip metering path: %s ([%s] %s %s in %s); %s",
        enabled ? L"on" : L"off", ampere_flip_metering::kSection, ampere_flip_metering::kKey, source, path.c_str(),
        enabled ? L"the DLSS-G wrapper may use flip-metering presentation"
                : L"the DLSS-G wrapper stays on CPU-paced presentation");
}

bool AmpereFlipMeteringForwarded() noexcept
{
    return ampere_flip_metering::OptIn() && !gFlipForwardBlocked.load(std::memory_order_acquire);
}

void RegisterAmpereFilteredWrapper(HMODULE module, const std::wstring& path)
{
    if (AmpereFilteredWrapper(module)) return;
    ConfigureAmpereFlipMetering();
    const bool v2Generation = ampere_flip_metering::ImageQueriesCaps(module);
    if (ampere_flip_metering::OptIn() && !v2Generation)
    {
        gFlipForwardBlocked.store(true, std::memory_order_release);
        Log(L"Ampere flip metering path not applied: the wrapper has no V2 capability query "
            L"(pre-2.11 generation; withheld as in beta.4): %s", path.c_str());
    }
    for (auto& wrapper : gAmpereFilteredWrappers)
    {
        HMODULE expected = nullptr;
        if (wrapper.compare_exchange_strong(expected, module, std::memory_order_acq_rel))
        {
            gWrapperNvapiFilterInstalled.store(true, std::memory_order_release);
            Log(AmpereFlipMeteringForwarded()
                    ? L"Ampere DLSS-G wrapper NVAPI view: flip metering forwarded (SetFlipConfig observed) "
                      L"through resolver discovery: %s"
                    : L"Ampere DLSS-G wrapper NVAPI view: flip metering withheld (SetFlipConfig, caps) "
                      L"through resolver discovery: %s", path.c_str());
            return;
        }
    }
    Log(L"Ampere DLSS-G wrapper NVAPI view unavailable: wrapper slots full: %s", path.c_str());
}

// Flip-metering path: forwards the wrapper's SetFlipConfig and records whether it asked for
// metered batches (ampere_flip_metering.h). Runs on the wrapper's present path.
int __cdecl ObservedSetFlipConfig(void* device, void* reserved, void* params, void* extra)
{
    const auto real = gWrapperSetFlipConfig.load(std::memory_order_acquire);
    if (!real) return -1; // NVAPI_ERROR
    uint32_t version = 0, framesPerBatch = 0;
    const bool read = ampere_flip_metering::ReadParams(params, version, framesPerBatch);
    const int status = real(device, reserved, params, extra);
    if (ampere_flip_metering::Observe(read ? version : 0, framesPerBatch, status))
    {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
            Log(L"Ampere flip metering %s: SetFlipConfig version=0x%X nFramesPerBatch=%u status=%d",
                ampere_flip_metering::Active() ? L"active (repeated outputs admitted)" : L"inactive",
                version, framesPerBatch, status);
    }
    return status;
}

void* __cdecl FilteredWrapperNvapiQuery(uint32_t id)
{
    const auto query = gWrapperNvapiQuery.load(std::memory_order_acquire);
    if (!query)
        return nullptr;
    if (ampere_flip_metering::Hidden(id) && AmpereWrapperAuditApplies())
    {
        if (!AmpereFlipMeteringForwarded())
        {
            gFlipMeteringHidden.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        void* resolved = query(id);
        gFlipMeteringForwarded.fetch_add(1, std::memory_order_relaxed);
        if (id != ampere_flip_metering::kSetFlipConfig || !resolved)
            return resolved; // The V2 capability query is forwarded unchanged.
        auto* real = reinterpret_cast<NvapiSetFlipConfigFn>(resolved);
        NvapiSetFlipConfigFn expected = nullptr;
        if (!gWrapperSetFlipConfig.compare_exchange_strong(expected, real,
                std::memory_order_acq_rel, std::memory_order_acquire) && expected != real)
            return resolved; // Unobserved: repeated outputs stay refused (and latch at 2x).
        ampere_flip_metering::MarkObserving();
        return reinterpret_cast<void*>(&ObservedSetFlipConfig);
    }
    return query(id);
}

// Called by HookSlCommonGetProcAddress with the address its caller returns to.
FARPROC AmpereWrapperNvapiView(HMODULE module, LPCSTR name, FARPROC resolved, const void* caller) noexcept
{
    if (!resolved || !name || IS_INTRESOURCE(name) || strcmp(name, "nvapi_QueryInterface") != 0
        || !gWrapperNvapiFilterInstalled.load(std::memory_order_acquire) || !AmpereWrapperAuditApplies())
        return resolved;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(caller), &owner) || !AmpereFilteredWrapper(owner))
        return resolved;
    // The wrapper loads NVAPI from System32 (LOAD_LIBRARY_SEARCH_SYSTEM32).
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* leaf = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!leaf || _wcsicmp(leaf + 1, L"nvapi64.dll") != 0)
        return resolved;
    auto* query = reinterpret_cast<NvapiQueryInterfaceFn>(resolved);
    NvapiQueryInterfaceFn expected = nullptr;
    if (!gWrapperNvapiQuery.compare_exchange_strong(expected, query,
            std::memory_order_acq_rel, std::memory_order_acquire) && expected != query)
        return resolved; // A different dispatcher than the one already wrapped stays native.
    gWrapperNvapiFilters.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<FARPROC>(&FilteredWrapperNvapiQuery);
}

PatternPatchResult PatchUniqueWrapperMaximumPattern(
    HMODULE module, const std::wstring& path)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = ImageHeaders(module);
    if (!nt)
        return {};

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    uint8_t* match = nullptr;
    size_t matchCount = 0;
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections;
         ++index, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0
            || section->VirtualAddress >= nt->OptionalHeader.SizeOfImage)
        {
            continue;
        }
        auto* begin = const_cast<uint8_t*>(
            base + section->VirtualAddress);
        const size_t available =
            nt->OptionalHeader.SizeOfImage - section->VirtualAddress;
        const size_t size = std::min<size_t>(available,
            std::max<size_t>(section->Misc.VirtualSize,
                section->SizeOfRawData));
        for (size_t offset = 0;
             offset + universal_wrapper_profile::kPatternSize <= size;
             ++offset)
        {
            if (!universal_wrapper_profile::Matches(
                    begin + offset, size - offset))
            {
                continue;
            }
            match = begin + offset;
            ++matchCount;
        }
    }

    if (matchCount == 0)
    {
        // Older pacer wrappers use a different clamp instruction. Admit their
        // control route only after all independent allocation contracts match.
        // No native clamp is rewritten for the 2x Ampere fallback.
        ampere_wrapper::Layout legacy{};
        if (AmpereWrapperAuditApplies() && ampere_wrapper::Discover(module, legacy)
            && legacy.maximumGeneratedFrames == 1)
            return {true, UseAmpere(), nullptr, 1};
        return {};
    }
    if (matchCount != 1 || !match)
    {
        Log(L"Streamline maximum: expected one 1/3/5 profile, found %zu: %s",
            matchCount, path.c_str());
        return {true, false, nullptr};
    }

    uint32_t compiledMaximum = 0;
    memcpy(&compiledMaximum,
        match + universal_wrapper_profile::kMaximumOffset,
        sizeof(compiledMaximum));
    if (!universal_wrapper_profile::IsSupportedMaximum(compiledMaximum))
    {
        Log(L"Streamline maximum: unsupported compiled value %u: %s",
            compiledMaximum, path.c_str());
        return {true, false, match};
    }

    if (!gpu_dispatch::IsAda())
    {
        ampere_wrapper::Layout audited{};
        if (AmpereWrapperAuditApplies() && ampere_wrapper::Discover(module, audited)
            && audited.maximumGeneratedFrames == 1)
            return {true, UseAmpere(), nullptr, 1};
        // RTX 30 and RTX 50 keep the wrapper's own clamp: RTX 30 declares its
        // architecture, and RTX 50's provider reports its real maximum.
        return {true, (UseAmpere() || gpu_dispatch::IsBlackwell())
            && memcmp(match + universal_wrapper_profile::kPatchOffset,
                kWrapperOriginal.data(), kWrapperOriginal.size()) == 0, match, compiledMaximum};
    }
    uint8_t* address = match + universal_wrapper_profile::kPatchOffset;
    if (memcmp(address, kWrapperReplacement.data(),
            kWrapperReplacement.size()) == 0)
    {
        Log(L"Streamline maximum: already patched at RVA 0x%zX "
            L"(compiled=%u): %s",
            static_cast<size_t>(address
                - const_cast<uint8_t*>(base)),
            compiledMaximum, path.c_str());
        return {true, true, match, compiledMaximum};
    }
    if (memcmp(address, kWrapperOriginal.data(),
            kWrapperOriginal.size()) != 0)
    {
        Log(L"Streamline maximum: matched context but original bytes differ: %s",
            path.c_str());
        return {true, false, match, compiledMaximum};
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(address, kWrapperReplacement.size(),
            PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Log(L"Streamline maximum: VirtualProtect failed (%lu): %s",
            GetLastError(), path.c_str());
        return {true, false, match, compiledMaximum};
    }
    memcpy(address, kWrapperReplacement.data(),
        kWrapperReplacement.size());
    FlushInstructionCache(GetCurrentProcess(), address,
        kWrapperReplacement.size());
    DWORD ignoredProtection = 0;
    const BOOL restored = VirtualProtect(address,
        kWrapperReplacement.size(), oldProtection, &ignoredProtection);
    if (!restored)
    {
        Log(L"Streamline maximum: protection restore failed (%lu): %s",
            GetLastError(), path.c_str());
        return {true, false, match, compiledMaximum};
    }

    Log(L"Streamline maximum: patched RVA 0x%zX (compiled=%u): %s",
        static_cast<size_t>(address - const_cast<uint8_t*>(base)),
        compiledMaximum, path.c_str());
    return {true, true, match, compiledMaximum};
}

std::wstring LoadedModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

uint64_t ModuleGeneration(HMODULE module) noexcept
{
    if (!module)
        return 0;
    std::lock_guard lock(gModuleMutex);
    const auto record = std::find_if(gModuleRecords.begin(),
        gModuleRecords.end(), [&](const ModuleRecord& candidate) {
            return candidate.module == module;
        });
    return record == gModuleRecords.end() ? 0 : record->generation;
}

ampere_gpu::PreparationBoundary ResolveAmperePreparationBoundary(HMODULE provider, uint64_t generation) noexcept
{
    if (!provider || !generation || !UseAmpere()
        || !gDllNotificationRegistered.load(std::memory_order_acquire)
        || !ampere_backend::BeforeFirstFeatureCreate()
        || gFrameGenerationCreateAttemptEpoch.load(std::memory_order_acquire)) return {};
    uint64_t loadToken = 0;
    bool nativeOwnerCurrent = false;
    const auto native = gAmpereNativeInitBoundary;
    if (native.current && (!gAmpereNativeInitTicket
        || ngx_initialization::PreparationTicket() != gAmpereNativeInitTicket)) return {};
    {
        std::lock_guard lock(gModuleMutex);
        for (const auto& record : gModuleRecords)
        {
            if (record.module == provider && record.generation == generation && record.ngxExport)
                loadToken = record.freshLoadToken;
            if (native.current && record.module == native.owner && record.generation == native.generation
                && (record.ngxExport || record.ngxRuntimeD3D12Export)
                && gOutputPullMaskLoads.Matches(reinterpret_cast<uintptr_t>(record.module), record.freshLoadToken))
                nativeOwnerCurrent = true;
        }
    }
    const auto module = reinterpret_cast<uintptr_t>(provider);
    if (!gOutputPullMaskLoads.Matches(module, loadToken)) return {};
    bool proven = false;
    if (nativeOwnerCurrent)
    {
        const auto current = entry_detour::ReadSnapshot(native.handle);
        proven = current.current && current.owner == native.owner && current.generation == native.generation
            && current.target == native.target && current.original == native.original
            && (current.kind == entry_detour::Kind::eNgxD3D12Init
                || current.kind == entry_detour::Kind::eNgxD3D12InitProject);
    }
    if (!proven)
    {
        OutputPullMaskInitEvidence init;
        { std::lock_guard lock(gOutputPullMaskInitMutex); init = gOutputPullMaskSlInit; }
        const auto current = entry_detour::ReadSnapshot(init.handle);
        proven = init.owner && current.current && current.kind == entry_detour::Kind::eSlInit
            && current.owner == init.owner && current.generation == init.generation;
    }
    // Positive initialization evidence is paired with an actual map event and
    // the current inventory generation; counters alone never authorize a write.
    if (!proven || !gOutputPullMaskLoads.Matches(module, loadToken)) return {};
    return {generation, loadToken, true, true, &AmperePreparationStillCurrent};
}

bool AmperePreparationStillCurrent(HMODULE provider, const ampere_gpu::PreparationBoundary& before) noexcept
{
    if (!before.Proven()) return false;
    const auto current = ResolveAmperePreparationBoundary(provider, before.providerGeneration);
    return current.Proven() && current.providerGeneration == before.providerGeneration
        && current.freshLoadToken == before.freshLoadToken;
}

void PrepareNgxInitialization(void* device, const entry_detour::Snapshot& entry,
    const void*, bool firstInit) noexcept
{
    // Before NVIDIA's Init: the DLSS SR provider reads its DRS preset key once,
    // when NGX initializes it.
    if (entry.current && device && gpu_dispatch::Selected() != gpu_dispatch::Family::eConflict)
        dlss_sr::PrepareForDevice(static_cast<ID3D12Device*>(device));
    if (gpu_dispatch::UsesNativeProgram() || gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return;
    const auto ticket = ngx_initialization::PreparationTicket();
    if (!firstInit || !ticket || !entry.current || !device
        || ModuleGeneration(entry.owner) != entry.generation
        || !gOutputPullMaskLoads.Current(reinterpret_cast<uintptr_t>(entry.owner))) return;
    if (!ampere_backend::BeginNativePreparation()) return;
    __try
    {
        // Reserve the nonblocking lifecycle scope before taking GPU selection
        // or adapter locks. These observations never publish a provider program.
        if (adapter_discovery::IsAmpereD3D12Device(device)
            && gpu_backend::ObserveD3D12Device(device) && UseAmpere()
            && ngx_initialization::PreparationTicket() == ticket)
        {
            gAmpereNativeInitBoundary = entry;
            gAmpereNativeInitTicket = ticket;
            LogAmpereArchitectureDetection(ampere_provider_arch::DetectAndArm());
            PrepareAmpereProviderSelection();
            // RTX 30/20 now has its family: the SR/RR DRS hook, still before
            // NVIDIA's Init reads those keys.
            dlss_sr::PrepareForDevice(static_cast<ID3D12Device*>(device));
            gModuleInventoryDirty.store(true, std::memory_order_release);
            InspectAlreadyLoadedModules();
        }
    }
    __finally
    {
        gAmpereNativeInitBoundary = {};
        gAmpereNativeInitTicket = 0;
        ampere_backend::EndNativePreparation();
    }
}

void ObserveOutputPullMaskSlInitResult(
    const entry_detour::Snapshot& before, bool succeeded) noexcept
{
    const auto current = entry_detour::ReadSnapshot(before.handle);
    const bool valid = succeeded && before.current && before.owner
        && before.kind == entry_detour::Kind::eSlInit && current.current
        && current.handle == before.handle && current.owner == before.owner
        && current.generation == before.generation && current.original == before.original;
    std::lock_guard lock(gOutputPullMaskInitMutex);
    gOutputPullMaskSlInit = valid
        ? OutputPullMaskInitEvidence{before.handle, before.owner, before.generation}
        : OutputPullMaskInitEvidence{};
}

void ObserveOutputPullMaskVulkanInit(entry_detour::Handle handle) noexcept
{
    const auto entry = entry_detour::ReadSnapshot(handle);
    const bool valid = entry.current && entry.owner && entry.generation
        && entry.kind == entry_detour::Kind::eNgxVulkanAdapterInit
        && ModuleGeneration(entry.owner) == entry.generation;
    std::lock_guard lock(gOutputPullMaskInitMutex);
    gOutputPullMaskVulkanInit = valid
        ? OutputPullMaskInitEvidence{handle, entry.owner, entry.generation}
        : OutputPullMaskInitEvidence{};
}

bool OutputPullMaskEarlyInitProven(HMODULE provider, NgxGraphicsApi api) noexcept
{
    if (!provider || !gpu_dispatch::IsAda()
        || (api != NgxGraphicsApi::eD3D12 && api != NgxGraphicsApi::eVulkan)
        || !gDllNotificationRegistered.load(std::memory_order_acquire))
        return false;
    uint64_t generation = 0;
    uint64_t loadToken = 0;
    {
        std::lock_guard lock(gModuleMutex);
        const auto record = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& candidate) { return candidate.module == provider; });
        if (record == gModuleRecords.end() || !record->ngxExport)
            return false;
        generation = record->generation;
        loadToken = record->freshLoadToken;
    }
    const uintptr_t module = reinterpret_cast<uintptr_t>(provider);
    if (!generation || generation != gActiveNgxProviderGeneration.load(std::memory_order_acquire)
        || !gOutputPullMaskLoads.Matches(module, loadToken))
        return false;
    OutputPullMaskInitEvidence slInit;
    OutputPullMaskInitEvidence vulkanInit;
    {
        std::lock_guard lock(gOutputPullMaskInitMutex);
        slInit = gOutputPullMaskSlInit;
        vulkanInit = gOutputPullMaskVulkanInit;
    }
    const auto proofCurrent = [](const OutputPullMaskInitEvidence& proof,
        entry_detour::Kind kind) noexcept {
        const auto current = entry_detour::ReadSnapshot(proof.handle);
        return proof.owner && current.current && current.kind == kind
            && current.handle == proof.handle && current.owner == proof.owner
            && current.generation == proof.generation;
    };
    // An observed call counter is insufficient: only successful slInit with
    // the same retained entry grants evidence. Nested in-progress calls skip.
    if (proofCurrent(slInit, entry_detour::Kind::eSlInit))
        return gOutputPullMaskLoads.Matches(module, loadToken);
    if (api != NgxGraphicsApi::eVulkan
        || !proofCurrent(vulkanInit, entry_detour::Kind::eNgxVulkanAdapterInit))
        return false;
    if (vulkanInit.owner == provider)
        return vulkanInit.generation == generation
            && gOutputPullMaskLoads.Matches(module, loadToken);
    // Shared runtime Init is useful only for the exact current runtime Create
    // route. An unrelated runtime or provider cannot donate init evidence.
    const auto create = entry_detour::ReadSnapshot(UnpackEntryHandle(
        gActiveNgxCreateHandle.load(std::memory_order_acquire)));
    return create.current && create.owner == vulkanInit.owner
        && create.generation == vulkanInit.generation
        && (create.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature
            || create.kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1)
        && ModuleGeneration(vulkanInit.owner) == vulkanInit.generation
        && gOutputPullMaskLoads.Matches(module, loadToken);
}

bool UsesNvidiaOtaCache(const std::wstring& path)
{
    std::wstring normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return normalized.find(L"\\nvidia\\ngx\\models\\")
        != std::wstring::npos;
}

FileVersion ReadFileVersion(const std::wstring& path)
{
    FileVersion version{};
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes)
        return version;
    std::vector<uint8_t> data(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()))
        return version;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\",
            reinterpret_cast<void**>(&fixed), &fixedBytes)
        || !fixed || fixedBytes < sizeof(*fixed)
        || fixed->dwSignature != 0xFEEF04BDu)
        return version;
    version.major = HIWORD(fixed->dwFileVersionMS);
    version.minor = LOWORD(fixed->dwFileVersionMS);
    version.build = HIWORD(fixed->dwFileVersionLS);
    version.privatePart = LOWORD(fixed->dwFileVersionLS);
    return version;
}

class ScopedModuleReference
{
public:
    explicit ScopedModuleReference(HMODULE module) noexcept
    {
        HMODULE retained = nullptr;
        if (module
            && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(module), &retained)
            && retained == module)
        {
            module_ = retained;
        }
        else if (retained)
        {
            FreeLibrary(retained);
        }
    }

    ~ScopedModuleReference()
    {
        if (module_)
            FreeLibrary(module_);
    }

    ScopedModuleReference(const ScopedModuleReference&) = delete;
    ScopedModuleReference& operator=(const ScopedModuleReference&) = delete;

    explicit operator bool() const noexcept { return module_ != nullptr; }
    HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_ = nullptr;
};

void RecomputeModuleStateLocked()
{
    uint32_t wrapperCandidates = 0;
    uint32_t patchedWrappers = 0;
    uint32_t ngxCandidates = 0;
    uint32_t patchedNgx = 0;
    uint32_t wrapperRouteBits = 0;
    uint32_t ngxRouteBits = 0;
    uint32_t activeCompiledMaximum = 0;
    const uintptr_t activeWrapper =
        gActiveWrapperBase.load(std::memory_order_acquire);
    for (const auto& record : gModuleRecords)
    {
        if (record.wrapperCandidate)
            ++wrapperCandidates;
        if (record.wrapperPatched)
        {
            ++patchedWrappers;
            wrapperRouteBits |= ClassifyLoadedRoute(record.path);
        }
        if (record.wrapperCompiledMaximumGeneratedFrames != 0)
        {
            if (reinterpret_cast<uintptr_t>(record.module)
                == activeWrapper)
            {
                activeCompiledMaximum =
                    record.wrapperCompiledMaximumGeneratedFrames;
            }
        }
        if (record.ngxCandidate)
            ++ngxCandidates;
        if (record.ngxPatched && record.ngxTemporalPatched)
        {
            ++patchedNgx;
            ngxRouteBits |= ClassifyLoadedRoute(record.path);
        }
    }
    gLoadedWrapperCandidates.store(wrapperCandidates, std::memory_order_release);
    gPatchedWrapperCandidates.store(patchedWrappers, std::memory_order_release);
    gLoadedNgxCandidates.store(ngxCandidates, std::memory_order_release);
    gPatchedNgxCandidates.store(patchedNgx, std::memory_order_release);
    gWrapperRouteBits.store(wrapperRouteBits, std::memory_order_release);
    gNgxRouteBits.store(ngxRouteBits, std::memory_order_release);
    // Candidate discovery is not activity. Do not publish a capacity from an
    // inactive local/OTA wrapper; the UI remains in "detecting" until a real
    // setter/state call selects one generation.
    const uint32_t selectedCompiledMaximum = activeWrapper != 0
        ? activeCompiledMaximum : 0u;
    gWrapperCompiledMaximumGeneratedFrames.store(
        selectedCompiledMaximum, std::memory_order_release);
    gSafeMaximumMultiplier.store(
        std::clamp(CurrentCapacityDecision().effectiveMaximumMultiplier,
            kMinimumMultiplier, kMaximumMultiplier),
        std::memory_order_release);
}

void LogModuleInventory(const ModuleRecord& record)
{
    if (!record.wrapperExport && !record.ngxExport
        && !record.ngxRuntimeExport)
        return;
    Log(L"Loaded module: wrapperExport=%d wrapperCandidate=%d wrapperPatched=%d "
        L"wrapperCompiledMaximum=%u "
        L"ngxExport=%d d3d12=%d vulkan=%d ngxCandidate=%d ngxPatched=%d "
        L"midpointPatched=%d ngxRuntimeExport=%d runtimeD3D12=%d "
        L"runtimeVulkan=%d path=%s",
        record.wrapperExport, record.wrapperCandidate, record.wrapperPatched,
        record.wrapperCompiledMaximumGeneratedFrames,
        record.ngxExport, record.ngxD3D12Export, record.ngxVulkanExport,
        record.ngxCandidate, record.ngxPatched,
        record.ngxTemporalPatched, record.ngxRuntimeExport,
        record.ngxRuntimeD3D12Export, record.ngxRuntimeVulkanExport,
        record.path.c_str());
}

ModuleRecord InspectLoadedModule(HMODULE module, const std::wstring& suppliedPath)
{
    if (!module)
        return {};
    // Toolhelp snapshots do not retain module references. NVIDIA's transient
    // NGX model DLLs can unload between snapshot enumeration and the pattern
    // scan, leaving a stale executable-section pointer. Acquire a loader
    // reference before reading any PE data and keep it for the full inspection.
    ScopedModuleReference retained(module);
    if (!retained)
        return {};
    module = retained.get();
    const std::wstring livePath = LoadedModulePath(module);
    const std::wstring path = livePath.empty() ? suppliedPath : livePath;
    if (ModuleFileNameEquals(path, L"sl.interposer.dll"))
    {
        TryInstallSlInitEntryDetour(module, nullptr);
        InstallStreamlineLoaderDiscovery(module, path);
        InstallSlCommonResolverDiscovery(module, path);
    }
    if (ModuleFileNameEquals(path, L"sl.common.dll"))
    {
        InstallStreamlineLoaderDiscovery(module, path);
        InstallSlCommonResolverDiscovery(module, path);
    }
    ModuleRecord snapshot{};
    bool logInventory = false;
    {
        std::lock_guard lock(gModuleMutex);
        const auto existing = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& record) {
                return record.module == module
                    && _wcsicmp(record.path.c_str(), path.c_str()) == 0;
            });
        if (existing != gModuleRecords.end())
        {
            if (existing->wrapperCandidate && !existing->wrapperPatched)
            {
                const PatternPatchResult result =
                    PatchUniqueWrapperMaximumPattern(
                        module, path);
                existing->wrapperCandidate = result.candidate;
                existing->wrapperPatched = result.patched;
                existing->wrapperCompiledMaximumGeneratedFrames =
                    result.profileMaximum;
                if (existing->wrapperCandidate)
                {
                    existing->controlRouteSlot = EnsureControlRoute(
                        existing->module, existing->path,
                        existing->generation, existing->wrapperPatched,
                        existing->wrapperCompiledMaximumGeneratedFrames);
                }
                RecomputeModuleStateLocked();
            }
            if (gpu_dispatch::UsesNativeProgram() && existing->ngxExport && !existing->ngxPatched)
            {
                const auto result = InspectNgxMfgGate(module, path);
                existing->ngxCandidate = result.candidate;
                existing->ngxPatched = result.patched;
                RecomputeModuleStateLocked();
            }
            if (gLogReady.load(std::memory_order_acquire) && !existing->inventoryLogged)
            {
                existing->inventoryLogged = true;
                logInventory = true;
            }
            snapshot = *existing;
        }
        else
        {
            ModuleRecord record{};
            record.module = module;
            record.path = path;
            record.generation = gNextModuleGeneration.fetch_add(
                1, std::memory_order_relaxed);
            record.freshLoadToken = gOutputPullMaskLoads.Current(
                reinterpret_cast<uintptr_t>(module));
            record.wrapperExport = ModuleExportsFunction(module, "slGetPluginFunction");
            record.ngxD3D12Export = ExportsNgxD3D12Route(module);
            record.ngxVulkanExport = ExportsNgxVulkanRoute(module);
            record.ngxExport =
                dlssg_provider_policy::IsDlssgImplementationModule(module)
                && (record.ngxD3D12Export || record.ngxVulkanExport)
                && ModuleExportsFunction(
                    module, "NVSDK_NGX_GetGPUArchitecture");
            record.ngxRuntimeExport = IsNgxRuntimeModule(module);
            record.ngxRuntimeD3D12Export = record.ngxRuntimeExport
                && record.ngxD3D12Export;
            record.ngxRuntimeVulkanExport = record.ngxRuntimeExport
                && record.ngxVulkanExport;
            if (!record.wrapperExport && !record.ngxExport
                && !record.ngxRuntimeExport)
                return record;
            // Wrapper and provider eligibility are independent. A game-local
            // or NVIDIA-cache wrapper which has exactly one recognized native
            // 1/3/5-frame clamp is patched immediately. Unknown or ambiguous
            // wrapper layouts remain untouched; the separately loaded NGX
            // provider still has to pass its own identity/version checks.
            if (record.wrapperExport)
            {
                const PatternPatchResult result =
                    PatchUniqueWrapperMaximumPattern(module, path);
                record.wrapperCandidate = result.candidate;
                record.wrapperPatched = result.patched;
                record.wrapperCompiledMaximumGeneratedFrames =
                    result.profileMaximum;
                if (record.wrapperCandidate)
                {
                    record.controlRouteSlot = EnsureControlRoute(
                        record.module, record.path, record.generation,
                        record.wrapperPatched,
                        record.wrapperCompiledMaximumGeneratedFrames);
                }
                // Before the plugin's startup probes flip metering; resolver
                // discovery below covers this plug-in's GetProcAddress import.
                if (record.wrapperCandidate && ampere_provider_arch::Armed())
                    RegisterAmpereFilteredWrapper(module, path);
            }
            if (record.ngxExport && ampere_provider_arch::Armed())
            {
                const auto declared = ampere_provider_arch::Declare(module);
                // The Vulkan record follows a successful D3D12 declaration only.
                const bool vulkanChecked = declared == ampere_provider_arch::Outcome::eDeclared
                    || declared == ampere_provider_arch::Outcome::eAlreadyDeclared;
                Log(L"Ampere DLSS-G architecture declaration: outcome=%hs vulkanRequirements=%hs path=%s",
                    ampere_provider_arch::OutcomeName(declared), ampere_provider_arch::VulkanOutcomeName(
                        vulkanChecked ? ampere_provider_arch::Read().lastVulkan
                            : ampere_provider_arch::VulkanOutcome::eNotChecked), path.c_str());
            }
            if (record.ngxExport)
            {
                const PatternPatchResult result = InspectNgxMfgGate(module, path);
                record.ngxCandidate = result.candidate;
                record.ngxPatched = result.patched;
            }
            record.inventoryLogged = gLogReady.load(std::memory_order_acquire);
            logInventory = record.inventoryLogged;
            gModuleRecords.push_back(record);
            RecomputeModuleStateLocked();
            snapshot = record;
        }
    }
    // A Streamline process can contain an on-disk wrapper plus a later NVIDIA
    // override wrapper. Only the uniquely signature-verified DLSS-G candidate
    // is safe to pin and detour during passive discovery. The generic
    // slGetPluginFunction export is shared by inactive feature modules and is
    // not proof that their returned DLSS-G entry belongs to the active route.
    // A Streamline plug-in is retained only once the host's session has a
    // device; until then the host may still unload and reload it.
    const bool retainable = !snapshot.wrapperExport
        || gStreamlinePluginRetention.load(std::memory_order_acquire);
    if (retainable)
        ngx_initialization::ObserveModule(snapshot.module, snapshot.generation, &PrepareNgxInitialization);
    ampere_backend::SetPreparationBoundaryResolver(&ResolveAmperePreparationBoundary);
    ampere_backend::ObserveModule(snapshot.module, snapshot.path.c_str(), snapshot.generation,
        snapshot.wrapperCandidate, snapshot.ngxExport, snapshot.ngxRuntimeExport);

    if (snapshot.wrapperExport)
    {
        // NVIDIA OTA plugins keep their Streamline identity but use opaque
        // cache filenames. Continue discovery structurally through every
        // Streamline plugin so the next provider is patched on loader return,
        // before that plugin can resolve and cache its NGX entry points.
        InstallStreamlineLoaderDiscovery(snapshot.module, snapshot.path);
        InstallSlCommonResolverDiscovery(snapshot.module, snapshot.path);
        // RTX 30 flip-metering view: prove the wrapper's GetProcAddress import
        // now runs through HookSlCommonGetProcAddress.
        if (AmpereFilteredWrapper(snapshot.module))
        {
            const bool routed = ReadModuleImport(snapshot.module, "KERNEL32.dll", "GetProcAddress")
                == reinterpret_cast<void*>(&HookSlCommonGetProcAddress);
            gAmpereWrapperResolverRouted.store(routed, std::memory_order_release);
            Log(L"Ampere DLSS-G wrapper NVAPI view route: resolverHook=%d path=%s",
                static_cast<int>(routed), snapshot.path.c_str());
        }
    }
    if (snapshot.wrapperCandidate && retainable)
    {
        InstallControlRouteEntries(snapshot.controlRouteSlot);
    }
    if (snapshot.ngxExport && retainable)
    {
        TryInstallNgxCreateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        // RTX 30 systems: the provider's Vulkan Release before its Create entries.
        ampere_backend::InstallVulkanRoute(
            snapshot.module, snapshot.path.c_str(), snapshot.generation, false);
        TryInstallNgxVulkanCreateEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        if (NativeDlssgPresetGpu()) dlssg_preset::ObserveProvider(snapshot.module, snapshot.generation);
        if (logInventory)
        {
            const auto preset = dlssg_preset::ReadSnapshot(snapshot.module);
            Log(L"DLSS-G cached preset reader: installed=%d failure=%u "
                L"targetRva=0x%X path=%s", preset.cachedReaderInstalled,
                preset.cachedReaderFailure, preset.cachedReaderRva, snapshot.path.c_str());
        }
    }
    if (snapshot.ngxRuntimeExport)
    {
        // Embedded NGX hosts load providers themselves. Cover their loader
        // return and resolver boundary before they cache provider functions.
        InstallStreamlineLoaderDiscovery(snapshot.module, snapshot.path);
        InstallSlCommonResolverDiscovery(snapshot.module, snapshot.path);
    }
    if (snapshot.ngxRuntimeExport && retainable)
    {
        if (ngx_runtime_policy::IsRemixRuntime(snapshot.module))
        {
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(snapshot.module), &pinned)
                && pinned == snapshot.module)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(pinned);
                uintptr_t previous = 0;
                if (gRemixRuntimeBase.compare_exchange_strong(previous, base,
                        std::memory_order_acq_rel))
                    Log(L"RTX Remix embedded Vulkan NGX runtime detected; "
                        L"FG controls and resource capacity remain game-managed: %s",
                        snapshot.path.c_str());
                else if (previous != base)
                    gRemixRuntimeBase.store(UINTPTR_MAX, std::memory_order_release);
            }
        }
        TryInstallNgxRuntimeCreateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxRuntimeEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        // RTX 30 systems: the runtime's Vulkan Release, requirements and
        // capability parameters, before its Vulkan Create entries. RTX 40/50:
        // its Vulkan Release, for live preset switching.
        if (IsNgxRuntimeModule(snapshot.module))
        {
            const bool vulkanRoute = ampere_backend::InstallVulkanRoute(
                snapshot.module, snapshot.path.c_str(), snapshot.generation, true);
            if (NativeDlssgPresetGpu())
                LogOnChange(L"NGX Vulkan runtime ReleaseFeature hook (live preset switching): "
                    L"installed=%d path=%s", vulkanRoute, snapshot.path.c_str());
        }
        TryInstallNgxRuntimeVulkanCreateEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxRuntimeVulkanEvaluateEntryDetour(
            snapshot.module, snapshot.path, snapshot.generation);
        TryInstallNgxVulkanAdapterEntryDetours(
            snapshot.module, snapshot.path, snapshot.generation);
    }
    if (logInventory)
        LogModuleInventory(snapshot);
    return snapshot;
}

void FlushModuleInventoryToLog()
{
    std::vector<ModuleRecord> records;
    {
        std::lock_guard lock(gModuleMutex);
        for (auto& record : gModuleRecords)
        {
            if (!record.inventoryLogged)
            {
                record.inventoryLogged = true;
                records.push_back(record);
            }
        }
    }
    for (const auto& record : records)
        LogModuleInventory(record);
}

void RemoveLoadedModule(const ModuleRecord& expected)
{
    const HMODULE module = expected.module;
    if (!module || !expected.generation)
        return;
    // Toolhelp is a point-in-time inventory. Inspection can load and register
    // more modules, so absence from that inventory is not unload evidence.
    // Take the loader reference outside gModuleMutex to avoid lock inversion.
    ScopedModuleReference retained(module);
    if (retained && _wcsicmp(LoadedModulePath(module).c_str(),
            expected.path.c_str()) == 0)
        return;
    {
        std::lock_guard lock(gModuleMutex);
        const auto found = std::find_if(gModuleRecords.begin(), gModuleRecords.end(),
            [&](const ModuleRecord& record) {
                return record.module == module
                    && record.generation == expected.generation
                    && _wcsicmp(record.path.c_str(), expected.path.c_str()) == 0;
            });
        if (found == gModuleRecords.end())
            return;
        gModuleRecords.erase(found);
        RecomputeModuleStateLocked();
        // A newer observation at the same address owns its own route state.
        if (std::any_of(gModuleRecords.begin(), gModuleRecords.end(),
                [&](const ModuleRecord& record) { return record.module == module; }))
            return;
    }
    // A route may be selected after inventory retirement releases gModuleMutex.
    // Retire its slot and metadata as one transaction only for this generation.
    std::lock_guard publicationLock(gActiveControlRouteMutex);
    if (ControlRouteRecord* route = ActiveControlRoute();
        route && route->wrapper == module && route->generation == expected.generation)
    {
        gActiveControlRouteSlot.store(UINT32_MAX,
            std::memory_order_release);
        SetUniversalRouteFailure(UniversalRouteFailure::eNoActiveRoute);
        gActiveWrapperPatched.store(false, std::memory_order_release);
        gActiveWrapperObserved.store(false, std::memory_order_release);
        gActiveWrapperBase.store(0, std::memory_order_release);
        gActiveWrapperUsesNvidiaOta.store(false, std::memory_order_release);
        gActiveWrapperVersionMajor.store(0, std::memory_order_release);
        gActiveWrapperVersionMinor.store(0, std::memory_order_release);
        gActiveWrapperVersionBuild.store(0, std::memory_order_release);
        gActiveWrapperVersionPrivate.store(0, std::memory_order_release);
    }
}

bool InspectAlreadyLoadedModules()
{
    // Only consider observations which predate the snapshot for retirement.
    std::vector<ModuleRecord> previousRecords;
    {
        std::lock_guard lock(gModuleMutex);
        previousRecords = gModuleRecords;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        Log(L"Could not enumerate loaded modules (%lu)", GetLastError());
        return false;
    }

    std::vector<HMODULE> loadedModules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            HMODULE module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
            loadedModules.push_back(module);
            single_overlay::application_imports::ArmModule(module);
            InspectLoadedModule(module, entry.szExePath);
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    for (const ModuleRecord& record : previousRecords)
    {
        if (std::find(loadedModules.begin(), loadedModules.end(),
                record.module) == loadedModules.end())
            RemoveLoadedModule(record);
    }
    return true;
}

// Patches the wrapper clamps and provider gates recorded before Ada was
// selected, from the records already held (no Toolhelp inventory, which can
// fail while another thread loads a module). A module observed from now on is
// patched as soon as it is recorded.
void PatchRecordedCandidates()
{
    std::vector<ModuleRecord> pending;
    {
        std::lock_guard lock(gModuleMutex);
        for (const ModuleRecord& record : gModuleRecords)
        {
            if ((record.wrapperCandidate && !record.wrapperPatched)
                || (record.ngxExport && !record.ngxPatched))
                pending.push_back(record);
        }
    }
    for (const ModuleRecord& record : pending)
    {
        ScopedModuleReference retained(record.module);
        if (retained && _wcsicmp(LoadedModulePath(record.module).c_str(),
                record.path.c_str()) == 0)
            InspectLoadedModule(record.module, record.path);
    }
}

struct MfgLdrDllLoadedNotificationData
{
    ULONG flags;
    const UNICODE_STRING* fullDllName;
    const UNICODE_STRING* baseDllName;
    PVOID dllBase;
    ULONG sizeOfImage;
};

union MfgLdrDllNotificationData
{
    MfgLdrDllLoadedNotificationData loaded;
    MfgLdrDllLoadedNotificationData unloaded;
};

using MfgLdrDllNotificationFunction = void (CALLBACK*)(
    ULONG reason, const MfgLdrDllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = NTSTATUS (NTAPI*)(
    ULONG flags, MfgLdrDllNotificationFunction callback, void* context, void** cookie);

// Reads a named, non-forwarded export from a mapped image's export table.
void* MappedExportAddress(HMODULE module, const char* expected) noexcept
{
    const auto* nt = ImageHeaders(module);
    if (!nt || !expected)
        return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress
        || !RvaRangeIsValid(nt, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    if (!exports->AddressOfNames || !exports->AddressOfNameOrdinals
        || !exports->AddressOfFunctions
        || !RvaRangeIsValid(nt, exports->AddressOfNames,
            static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD))
        || !RvaRangeIsValid(nt, exports->AddressOfNameOrdinals,
            static_cast<size_t>(exports->NumberOfNames) * sizeof(WORD))
        || !RvaRangeIsValid(nt, exports->AddressOfFunctions,
            static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD)))
        return nullptr;
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index)
    {
        const DWORD nameRva = names[index];
        if (!RvaRangeIsValid(nt, nameRva, 1))
            continue;
        const char* name = reinterpret_cast<const char*>(base + nameRva);
        const size_t remaining = nt->OptionalHeader.SizeOfImage - nameRva;
        if (strnlen_s(name, remaining) >= remaining || strcmp(name, expected) != 0)
            continue;
        if (ordinals[index] >= exports->NumberOfFunctions)
            return nullptr;
        const DWORD rva = functions[ordinals[index]];
        const bool forwarded = rva >= directory.VirtualAddress
            && rva - directory.VirtualAddress < directory.Size;
        return !forwarded && RvaRangeIsValid(nt, rva, 1) ? base + rva : nullptr;
    }
    return nullptr;
}

// Loader notification for sl.interposer.dll, with the loader lock held. Hosts
// that resolve exports themselves (Wuthering Waves) call slInit as soon as
// LoadLibrary returns, before module discovery sees the interposer; HookSlInit
// then installs the remaining Streamline entries in time. Hotpatch only, never
// waits for the detour registry and never logs; discovery logs it later.
void InstallSlInitEntryAtLoad(HMODULE interposer, const UNICODE_STRING* path) noexcept
{
    if (!path || !path->Buffer
        || !HostInterposer(interposer, path->Buffer, path->Length / sizeof(wchar_t)))
        return;
    void* target = MappedExportAddress(interposer, "slInit");
    if (!target)
        return;
    entry_detour::InstallOptions options{};
    options.nonBlocking = true;
    void* trampoline = nullptr;
    entry_detour::Handle handle{};
    bool installed = false;
    // An empty handle means another install held the registry; retry for a
    // bounded time. Waiting could deadlock against the loader lock.
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        installed = entry_detour::Install(entry_detour::Kind::eSlInit,
            interposer, target, reinterpret_cast<void*>(&HookSlInit),
            trampoline, options, &handle);
        if (installed || handle)
            break;
        Sleep(1);
    }
    if (trampoline)
        gOriginalSlInit.store(reinterpret_cast<PFun_slInit*>(trampoline),
            std::memory_order_release);
    if (installed)
    {
        gSlInitEntryHandle = handle;
        gSlInitInstalledAtLoad.store(true, std::memory_order_release);
    }
}

bool UnicodeNameEquals(const UNICODE_STRING* name, const wchar_t* expected) noexcept
{
    const size_t length = wcslen(expected);
    return name && name->Buffer && name->Length == length * sizeof(wchar_t)
        && _wcsnicmp(name->Buffer, expected, length) == 0;
}

// Who maps nvngx_dlssnr.dll (NVIDIA's NGX runtime, another DLSS 5 add-on, or our direct route).
// Under the loader lock only the raw return addresses and the path are copied; the worker describes them.
struct ProviderLoad
{
    std::atomic<bool> ready{false};
    bool logged = false;
    void* base = nullptr;
    USHORT frames = 0;
    void* stack[16]{};
    wchar_t path[MAX_PATH]{};
};
std::array<ProviderLoad, 4> gProviderLoads;
std::atomic<uint32_t> gProviderLoadCount{0};

void CaptureProviderLoad(const MfgLdrDllNotificationData& data) noexcept
{
    const uint32_t index = gProviderLoadCount.fetch_add(1, std::memory_order_acq_rel);
    if (index >= gProviderLoads.size()) return;
    auto& load = gProviderLoads[index];
    load.base = data.loaded.dllBase;
    load.frames = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(load.stack)), load.stack, nullptr);
    const UNICODE_STRING* name = data.loaded.fullDllName;
    if (name && name->Buffer)
        wcsncpy_s(load.path, name->Buffer, std::min<size_t>(name->Length / sizeof(wchar_t), MAX_PATH - 1));
    load.ready.store(true, std::memory_order_release);
}

void LogProviderLoads()
{
    const uint32_t count = std::min<uint32_t>(gProviderLoadCount.load(std::memory_order_acquire),
        static_cast<uint32_t>(gProviderLoads.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        auto& load = gProviderLoads[i];
        if (load.logged || !load.ready.load(std::memory_order_acquire)) continue;
        load.logged = true;
        std::wstring stack, loader;
        for (USHORT frame = 0; frame < load.frames; ++frame)
        {
            wchar_t path[MAX_PATH]{}, text[MAX_PATH + 32]{};
            HMODULE module = nullptr;
            const bool named = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(load.stack[frame]), &module) && module && GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = named ? (wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path) : L"?";
            swprintf_s(text, L"%s%ls+0x%llX", frame ? L" < " : L"", name, static_cast<unsigned long long>(
                static_cast<const char*>(load.stack[frame]) - reinterpret_cast<const char*>(module)));
            stack += text;
            // The loading module: the first frame outside the loader itself.
            if (named && loader.empty() && _wcsicmp(name, L"ntdll.dll") && _wcsicmp(name, L"KERNELBASE.dll")
                && _wcsicmp(name, L"KERNEL32.DLL"))
                loader = path;
        }
        Log(L"DLSS 5 provider mapped: nvngx_dlssnr.dll base=0x%p path=%s loader=%s stack=%s", load.base, load.path,
            loader.empty() ? L"-" : loader.c_str(), stack.c_str());
    }
}

void CALLBACK OnDllNotification(
    ULONG reason, const MfgLdrDllNotificationData* data, void*)
{
    static constexpr ULONG kDllLoaded = 1;
    static constexpr ULONG kDllUnloaded = 2;
    if (data && reason == kDllLoaded && data->loaded.dllBase
        && UnicodeNameEquals(data->loaded.baseDllName, L"nvngx_dlssnr.dll"))
        CaptureProviderLoad(*data);
    if (data && reason == kDllLoaded && data->loaded.dllBase
        && UnicodeNameEquals(data->loaded.baseDllName, L"sl.interposer.dll"))
    {
        InstallSlInitEntryAtLoad(static_cast<HMODULE>(data->loaded.dllBase),
            data->loaded.fullDllName);
        InstallOptiScalerSlSetD3DDeviceAtLoad(static_cast<HMODULE>(data->loaded.dllBase),
            data->loaded.fullDllName);
    }
    // NGX validates a DLSS-G snippet against the GPU right after mapping it.
    // Declare RTX 30 support before LoadLibrary returns (no waits or logging).
    if (data && reason == kDllLoaded && data->loaded.dllBase && ampere_provider_arch::Armed())
        ampere_provider_arch::Declare(static_cast<HMODULE>(data->loaded.dllBase));
    // A reloaded game interposer binds again.
    if (data && reason == kDllUnloaded)
    {
        HMODULE unloaded = static_cast<HMODULE>(data->unloaded.dllBase);
        gHostInterposer.compare_exchange_strong(unloaded, nullptr, std::memory_order_acq_rel);
    }
    // Loader lock held: resource-directory read only, no logging. The worker
    // reports the transition and applies the OptiScaler-mode policy.
    if (data && reason == kDllLoaded && data->loaded.dllBase)
        optiscaler_host::ObserveMappedModule(static_cast<HMODULE>(data->loaded.dllBase));
    if (data && (reason == kDllLoaded || reason == kDllUnloaded))
    {
        const uintptr_t module = reinterpret_cast<uintptr_t>(
            reason == kDllLoaded ? data->loaded.dllBase : data->unloaded.dllBase);
        if (reason == kDllLoaded)
            gOutputPullMaskLoads.Loaded(module);
        else
            gOutputPullMaskLoads.Unloaded(module);
        gModuleInventoryDirty.store(true, std::memory_order_release);
    }
}

bool RegisterDllNotification()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* registerNotification = ntdll ? reinterpret_cast<LdrRegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
    if (!registerNotification)
        return false;

    void* cookie = nullptr;
    const NTSTATUS status = registerNotification(0, &OnDllNotification, nullptr, &cookie);
    const bool registered = status >= 0 && cookie != nullptr;
    gDllNotificationRegistered.store(registered, std::memory_order_release);
    return registered;
}

#if MFG_UNLOCK_SESSION_DIAGNOSTICS
// What the backend was doing, logged when it changes and once a minute with the counters; the crash and
// exit records repeat the last sample.
void LogSessionState(const ControlConfig& control, bool heartbeat)
{
    wchar_t state[512]{};
    int used = swprintf_s(state, L"route=%hs ready=%d routeFailure=%u control=%s %ux gameFG=%d appliedFG=%d restart=%d",
        PatchRouteName(), BridgeReady(), gUniversalRouteFailure.load(std::memory_order_acquire),
        control.followGame ? L"follow" : control.dynamic ? L"dynamic" : L"fixed", control.multiplier,
        gGameFrameGenerationOn.load(std::memory_order_acquire), gAppliedFrameGenerationOn.load(std::memory_order_acquire),
        gRestartRequired.load(std::memory_order_acquire));
    wchar_t counters[512]{};
    int counted = swprintf_s(counters, L"setOptions=%llu getState=%llu ngxCreate=%llu fgCreate=%llu ngxEvaluate=%llu "
        L"presented=%u",
        static_cast<unsigned long long>(gSetOptionsCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gGetStateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gNgxCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gNgxFrameGenerationCreateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gNgxEvaluateCalls.load(std::memory_order_relaxed)),
        gActualFramesPresented.load(std::memory_order_relaxed));
    // admitoff.1: RTX 30/20 FG Creates, those refused before the provider, and
    // option requests held at Off; a refusal loop shows as a climbing blocked count.
    if (UseAmpere() && counted > 0)
    {
        const auto ampere = ampere_backend::Diagnostics();
        const int added = swprintf_s(counters + counted, _countof(counters) - counted,
            L" ampereCreate=%llu blockedBeforeProvider=%llu admissionHeld=%llu",
            static_cast<unsigned long long>(ampere.createAttempts),
            static_cast<unsigned long long>(ampere.createBlockedBeforeProvider),
            static_cast<unsigned long long>(gAmpereAdmissionHeldRequests.load(std::memory_order_relaxed)));
        if (added > 0) counted += added;
    }
    const auto xess = xess_mod::ReadSnapshot();
    if (xess && xess->providerLoaded && used > 0 && counted > 0)
    {
        swprintf_s(state + used, _countof(state) - used, L" xefg=%s%s%s game=%d applied=%u capacity=%u model=%u "
            L"result=%d fgResult=%d", xess->initialized ? L"init" : L"-", xess->enabled ? L"+on" : L"",
            xess->composition ? L"+ui" : L"", xess->gameEnabled, xess->applied, xess->capacity, xess->modelActive,
            xess->lastResult, xess->lastFrameGenResult);
        swprintf_s(counters + counted, _countof(counters) - counted, L" xefgAppFrames=%llu xefgPresented=%u",
            static_cast<unsigned long long>(xess->appFrames), xess->framesPresented);
    }
    if (session_diagnostics::PublishState(state, counters))
        Log(L"State: %s", state);
    else if (heartbeat)
        Log(L"State heartbeat: %s | %s suppressedRepeats=%llu", state, counters,
            session_diagnostics::SuppressedRepeats());
}
#endif

#if MFG_UNLOCK_SESSION_DIAGNOSTICS
#define MFG_SESSION_DIAGNOSTICS_LABEL L"+diag.1"
#else
#define MFG_SESSION_DIAGNOSTICS_LABEL L""
#endif

// The worker's start-up steps, logged directly (not queued) and named by the start-up hang watchdog, so a log that
// ends during start-up still says which step stalled.
void StartupStep(const wchar_t* step)
{
    Log(L"Worker start-up: %s", step);
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    session_diagnostics::StartupStep(step);
#endif
}

DWORD WINAPI PatchWorker(void* context)
{
    EnsureAmpereFeatureLifetimeObserver();
    const DWORD pid = GetCurrentProcessId();
    // The game folder, or %TEMP% when it cannot be written.
    const std::wstring& logDirectory = diagnostic_paths::LogDirectory();
    // GitHub issue 122: Diagnostics > Write log file in the menu, [Diagnostics] Logging in RTX40MFG-UI.ini. Off, this
    // session neither rotates nor opens the log (OutputDebugString still carries every line).
    const auto logSetting = runtime_log_setting::ReadSetting(runtime_log_setting::Path().c_str());
    const bool logging = logSetting != runtime_log_setting::Setting::eOff;
    runtime_log_setting::SetSession(logging);
    std::wstring logPath = logging ? diagnostic_paths::RuntimeLog() : std::wstring{};
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    std::wstring busyLog;
    DWORD busyError = 0;
#endif
    if (!logPath.empty())
    {
        // Keep the previous session's log (a crashed session is usually
        // followed by a relaunch) beside the new one as *.previous.log, and
        // the one before as *.previous2.log: a launcher stub of the same exe
        // (0.3 s, no device) would otherwise rotate the session that had
        // worked out of a single previous slot.
        const std::wstring stem = logPath.substr(0, logPath.size() - 4);
        const std::wstring previousLog = stem + L".previous.log";
        MoveFileExW(previousLog.c_str(), (stem + L".previous2.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(logPath.c_str(), previousLog.c_str(), MOVEFILE_REPLACE_EXISTING);
        gLog = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
        // A log still held open (a crashed session's handles closing, a scan on close) would leave this
        // session without any log; it writes beside it under its process id instead.
        if (!gLog)
        {
            busyError = GetLastError();
            busyLog = logPath;
            logPath = logPath.substr(0, logPath.size() - 4) + L"." + std::to_wstring(pid) + L".log";
            gLog = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
        }
#endif
    }
    gLogReady.store(gLog != nullptr, std::memory_order_release);
    single_module::DrainLog(&QueuedLog);
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    // The environment header, then crash and exit records for the rest of the session.
    session_diagnostics::Start(gLog, logPath.c_str(), &MidpointLog, &single_module::TryDrainLog);
    if (gLog && !busyLog.empty())
        Log(L"Runtime log %s could not be opened (error %lu); this session logs to %s instead", busyLog.c_str(),
            busyError, logPath.c_str());
#endif
    fault_capture::Initialize(&MidpointLog);
    // Opt-in; before the game reads its adapter name for its settings.
    gpu_name_override::ArmFromSettings();
    single_module::DrainLog(&QueuedLog);

    const std::wstring mappingName = MfgUnlockObjectName(L"Status", pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    auto* shared = mapping ? static_cast<MfgUnlockStatus*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MfgUnlockStatus))) : nullptr;

    const std::wstring eventName = MfgUnlockObjectName(L"Ready", pid);
    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());

    std::wstring executablePath(32768, L'\0');
    const DWORD executableLength = GetModuleFileNameW(nullptr,
        executablePath.data(), static_cast<DWORD>(executablePath.size()));
    executablePath.resize(executableLength < executablePath.size()
        ? executableLength : 0);
    const std::wstring executableDirectory = ParentPath(executablePath);
    if (gExecutablePath.empty())
        gExecutablePath = executablePath;
    if (gConfigPath.empty())
    {
        gConfigPath = ResolveConfigPath(
            static_cast<HMODULE>(context), executableDirectory);
    }
    gStatusPath = ResolveStatusPath(gConfigPath, executableDirectory);
    // The first atomic publication replaces the previous launch's snapshot.
    // Never delete a file another live process may still own.
    temporal_interval_trace::Initialize(logDirectory.c_str(), executablePath.c_str());
    ControlConfig savedControl = ReadInitialControl();
    optiscaler_host::SetUiMode(static_cast<optiscaler_host::UiMode>(savedControl.optiScalerUi));
    bool uiDelegated = optiscaler_host::UiDelegated();
    bool menuTabOffered = optiscaler_menu_tab::Offered();
    const ControlConfig initialControl = EffectiveControl(savedControl);
    StoreControl(initialControl);
    // Before the game's first Vulkan instance (its API version is raised only while the harness-only
    // standalone host is on; OptiScaler raises it for its XeFG output).
    ConfigureXemfg(initialControl);
    {
        const auto host = optiscaler_host::Read();
        if (host.detected)
            Log(L"OptiScaler: route=%hs uiOwner=%s version=%s (%s) path=%s pluginCaller=%d menuTabApi=%d",
                optiscaler_host::RouteName(host.route), host.uiHidden ? L"optiscaler" : uiDelegated ? L"shared" : L"rtxmfg",
                host.productVersion, host.fileVersion, host.path, host.pluginCallerAccepted,
                menuTabOffered);
    }
    bool xessStarted = false;
    const auto startXess = [&] {
        xessStarted = true;
        single_module::DrainLog(&QueuedLog);
        StartupStep(L"XeSS route");
        const bool xessImportsReady = xess_mod::InstallImports(static_cast<HMODULE>(context));
        if (!xessImportsReady)
            Log(L"XESS SDK discovery disabled: backend module ownership/pin verification failed.");
        xess_mod::InitializeOutsideLoaderLock();
        xess_mod::ConfigureModel(initialControl.xessModel, OptiScalerHostModel());
        if (xessImportsReady)
            xess_mod::DiscoverSdkModules();
        // ngxprov.1: the DLL OptiScaler's custom DLSSG folder gives it (vk_provider_shim), in D3D12 games too.
        xess_vulkan_provider::ShimInstalled();
        // asi_start, sdk_route_requires_optiscaler and vk_host_config reach the file before the next step.
        single_module::DrainLog(&QueuedLog);
        StartupStep(L"XeSS route started");
    };
    // Under OptiScaler the XeFG route comes first. OptiScaler resolves its XeFG
    // exports before plug-ins load and can create its context a few hundred ms after
    // start: a context created before the detours stays stock 2x. The NVIDIA profile,
    // Streamline hooks and module inventory below do not gate it. Without OptiScaler
    // the route stays idle and the order below is unchanged.
    if (optiscaler_host::Detected())
        startXess();
    ResolveNvidiaCompatibilityPolicy();
    FILETIME configWriteTime{};
    ReadLastWriteTime(gConfigPath, configWriteTime);
    Log(L"Initial control: mode=%s multiplier=%ux dynamicTarget=%u FPS "
        L"dynamicExperimental56=%d generatedOnlyDebug=%d "
        L"intervalLogging=%d; config: %s",
        initialControl.followGame ? L"follow"
            : initialControl.dynamic ? L"dynamic" : L"fixed",
        initialControl.multiplier,
        initialControl.dynamicTargetFrameRate, initialControl.dynamicExperimental56,
        initialControl.generatedOnlyDebug, initialControl.intervalLogging,
        gConfigPath.c_str());
    if (initialControl.intervalLogging && temporal_interval_trace::FileEnabled())
    {
        Log(L"NGX temporal-request interval trace enabled: %s",
            temporal_interval_trace::FilePath());
    }

    Log(L"Patch worker started for PID %lu", static_cast<unsigned long>(pid));
    LogAmpereArchitectureDetection(ampere_provider_arch::DetectAndArm());
    Log(L"Diagnostics reused per executable: log=%s status=%s processBirth=%llu executable=%s",
        logPath.c_str(),gStatusPath.c_str(),static_cast<unsigned long long>(diagnostic_paths::ProcessBirth()),executablePath.c_str());
    // GitHub issue 122: the menu reads the status from process memory every second; the status file is rewritten
    // on changes and every 30 s. Harnesses that poll the file opt into every second.
    const bool statusFileEverySecond = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", value, 2) == 1 && value[0] == L'1';
    }();
    Log(L"Diagnostic files: runtime log on%s ([%s] %s=0 in RTX40MFG-UI.ini: off), status file every %s "
        L"(RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT=1: every second), interval trace %s (RTXMFG_DIAGNOSTIC_INTERVAL_TRACE=1)",
        logSetting == runtime_log_setting::Setting::eInvalid ? L" (saved value is not 1 or 0)" : L"",
        runtime_log_setting::kSection, runtime_log_setting::kKey,
        statusFileEverySecond ? L"second" : L"30 s and on changes",
        temporal_interval_trace::FileEnabled() ? temporal_interval_trace::FilePath() : L"off");
    Log(L"RTXMFG build=1.7.1 source=v1.5-flipmeter-20261006 base=intel6.6+optiscaler.1+ring2.3+dltb.2+slhost.1+flipmeter.18+hairdiscovery.1.4.2+reflexfg.2+capture.2+optixell.1+optiout.1+optipace.1+model2x.1+mvfill.1+uioff.1+uistable.1+mfgport.1+hudmatch.1+optiport.1+optcapture.1+optiportdefault.1+xefgdiscovery.1+slrestart.1+nvapipin.1+modeltoggle.1+uirefresh.1+noport.1+optiredirect.1+reflexport.1+portback.1+optionly.1+delayload.1+rtx30.3+xellungated.1+specialk.1a+themidaimports.1+modelchoice.1+xefgmodel.1+splat.2+scenechange.1+xellinput.1+rtx30sl2732.1+mfgunlocker.1+presetc.3+rtx30presetc.1+rtx30stable.1+rtx30ota.1+cudawrapped.2+l10n.8+pin.4+turing.1+sm120.1+unet.1+nvlist.1+rtx30nvapi.1+rtx30buffers.1+gpuname.1+arcnative.3+rtx30flip.2+cudaowner.1+dredtable.1+optidlssg.1+xefgflipmeter.2+releaseleak.1+flipkey.1+cap6.1+optirtx30.1+livepreset.2+routegen.1+hitch.1+xefg134.1+rtx30vulkan.1+vkholes.1+vkvsync.1+vkfps.1+srpreset.1+srpreset.2+srpreset.3+srpreset.4+seam.1+scene.1+logpath.1+rtx20dlssg.1+slinitloop.1+lean160.1+xefgrace.1+latency.1+livepresetall.1+rtx50.1+slinitfg.1+slinitfg.2+psorefused.1+ring2.4+flipqueue.2+vsync.1+coupled.1+wine.1+follow.1+prodtarget.1+menucap.1+vkpreset.1+vsync.2+gatepass.1+vkxefg.1+vkxefg.2+vkxefg.3+vkxefg.4+vkxefg.5+hostgate.1+menu16.1+amdkeep.1+rtx30presetstatus.1+vkswapext.1+presentqueue.1+lockin.1+logkeep.1+amdflip.1+amdqueue.1+presenterlead.1+cudafault.1+mailbox.1+stallguard.1+reflexgate.1+slinitfg.3+dlaalabel.1+crashhost.1+ngxforward.1+vkreflex.1+amdcount.1+hdrpq.1+amdlog.1+amdqueue.2+amdqueue.3+heldrate.1+dlss5.1+dlss5queue.1+dlss5tab.1+dlss5inputs.1+dlss5present.1+dlss5layout.1+dlss5auto.1+dlss5autolabel.1+dlss5rgbamv.1+dlss5unwrap.2+dlss5optiui.1+dlss5optichain.2+dlss5optikey.1+dlss5compare.5+dlss5unwrap.3+dlss5inputs.2+vkdlss5.1+xefgmenu.1+dlss5boost.1+dlss5boost.2+dlss5boost.3+dlss5boost.4+dlss5dots.1+diag171.1+ringcap.1+xemfgsa.1+xemfgsa.2+xemfgsa.3+xemfgsa.4+xemfgsa.5+xemfgsa.6+dlss5adv.1+hairshade.1+hairtrans.3+hairbounce.1+hairalbedo.1+hairtrans.4+hairrefl.1+hairdebug.1+hairstencil.1+hairfold.1+dlss5halfrate.1+dlss5gate.1+vkstages.1+xemfghint.1+underlinetabs.1+halfrateoff.1+firstready.1+interopmenu.3+halfgrid.1+menupoll.1+autoscale.1+autoscale.2+outputwait.1+menufallback.1+dlss5owner.1+runtimesearch.1+startwatch.1+seamstats.1+dlss5cost.1+dx11note.1+faultalive.1+exposureoff.1+amd6x.1+bimodal.1+hdrbridge.1+optiuirevert.1+deepheld.1+smallchain.1+factoryhook.1+smallchain.2+flipkeyoff.1+optimenu.1+admitoffmin.1+dlss5noratio.1+menutab.2+menumouse.1+sapacing.2+admitoff.1+oldruntime.1+nvpresent.1+keybinds.2+debloat.1+debloat.2+slim.1+keybinds.3+winemenu.1+menunotes.1+menuout.1+dlssgtab.1+dlssgtab.2+nrouter.1+optinr.1+menuout.2+nrnames.1+dx11nr.1+nrscale.1+latency.2+pacelock.1+idanchor.1+tabdefault.1+clstate.1+latmon.1+xemfglive.1+latmon.2+latmon.3+xemfglive.2+latmon.4+xemfglive.3+xemfglive.4+xemfglive.6+autorect.1+autofps.1+xemfglive.7+xemfglive.8+xemfglive.9+keyrelease.1+pluginabi.1+diskwrites.1+diskwrites.2+diskwrites.3+pacerfix.3+vmemlog.1+privboot.1+heldprobe.1+wrapbuffers.1+metergate.1+hosthandle.1+rejectlog.1+winequeue.1+rejectlog.2+sleepsight.1+optisleep.1+sdkscan.1+optispace.1+dispoll.1+rtxhair.1+rtxhair.2+rtxhair.3+rtxhair.4+rtxhair.5+rtxhair.6+vkprov.1+vkprov.2+vkprov.3+vkprov.4+hairauto.1+nulltoken.1+discordfix.1+hudlatch.1+portstatus.1+pacediag.1+dlss5presets.1+dlss5presets.2+dlss5tab.1+ngxprov.1+fg95prov.1+vkhair.1+apisetgpa.1+beta2fix.1+ngxrect.1+rtx30ngx.1+ghfix.1+vkhairchain.1+ngxcuda.1+ngxcuda.2+hairmem.2+sl212direct.1+beta3fix.1+pluginprov.2+gatehold.1+xmxdefault.1+hostport.1+slim.2" MFG_SESSION_DIAGNOSTICS_LABEL L" outputPullMask=%d occupancyHint=%d "
        L"uiInputs=framed-observations uiRecomposition=game-managed",
        MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY,
        MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY);
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    // Publication can precede worker/log startup. Preserve the one-time result
    // here as well as at Create so early failures remain locally diagnosable.
    const auto maskStartup = midpoint_fix::ReadOutputPullMaskSnapshot();
    Log(L"OUTPUT_PULL_MASK startup attempted=%d ready=%d published=%d "
        L"failure=%u publicationAttempted=%d replacementWasVisible=%d "
        L"unsafePublication=%d requiresRestart=%d firstCreate=%d "
        L"mayPredate=%d earlyInit=%d sourceSha256=%hs "
        L"selectedSha256=%hs",
        maskStartup.attempted, maskStartup.ready, maskStartup.published,
        static_cast<unsigned>(maskStartup.failure),
        maskStartup.publicationAttempted, maskStartup.replacementWasVisible,
        maskStartup.unsafePublication, maskStartup.requiresRestart,
        maskStartup.firstPipelineCreate, maskStartup.pipelineMayPredateDetour,
        maskStartup.earlyInitObserved,
        maskStartup.sourceSha256, maskStartup.selectedSha256);
#endif
    Log(L"NVIDIA compatibility profile: resolved=%d status=%d profile=%hs "
        L"tier=%hs manifestEntries=%u fetched=%hs sha256=%hs",
        gNvidiaCompatibilityResolved.load(std::memory_order_acquire),
        gNvidiaProfileStatus.load(std::memory_order_relaxed),
        gNvidiaProfileName.c_str(),
        nvidia_mfg_policy::TierName(
            static_cast<nvidia_mfg_policy::Tier>(
                gNvidiaCompatibilityTier.load(std::memory_order_relaxed))),
        nvidia_mfg_policy::ManifestEntryCount(),
        nvidia_mfg_policy::ManifestFetchedDate(),
        nvidia_mfg_policy::ManifestSha256());
    Log(L"Early DLL notification registered: %d",
        gDllNotificationRegistered.load(std::memory_order_acquire));
    Log(L"Streamline slInit control path: entryCurrent=%d iatFallback=%d "
        L"resolverDiscovery=%d calls=%llu",
        entry_detour::ReadSnapshot(
            entry_detour::Kind::eSlInit).current,
        gSlInitIatFallbackInstalled.load(std::memory_order_acquire),
        gMainResolverDiscoveryInstalled.load(std::memory_order_acquire),
        static_cast<unsigned long long>(
            gSlInitCalls.load(std::memory_order_acquire)));
    {
        const char* resolverRoute = gMainResolverImportRoute.load(std::memory_order_acquire);
        Log(L"Main-executable GetProcAddress hook: route=%hs", resolverRoute ? resolverRoute : "none");
    }
    const bool liveHookInstalled = InstallFeatureFunctionHook();
    gLiveHookInstalled.store(liveHookInstalled, std::memory_order_release);
    Log(L"Streamline feature-function interception installed: %d", liveHookInstalled);
    Log(L"Streamline D3D device interception installed: %d",
        InstallD3DDeviceHook());
    Log(L"Streamline Vulkan info interception installed: %d",
        InstallVulkanInfoHook());
    const bool uiTagHookInstalled = InstallUiTagHooks();
    Log(L"Streamline UI tag interception installed: %d", uiTagHookInstalled);
    InspectAlreadyLoadedModules();
    FlushModuleInventoryToLog();
    Log(L"Loaded-module discovery initialized: ready=%d route=%hs wrappers=%u/%u ngx=%u/%u",
        BridgeReady(), PatchRouteName(),
        gPatchedWrapperCandidates.load(std::memory_order_relaxed),
        gLoadedWrapperCandidates.load(std::memory_order_relaxed),
        gPatchedNgxCandidates.load(std::memory_order_relaxed),
        gLoadedNgxCandidates.load(std::memory_order_relaxed));

    if (shared)
    {
        shared->magic = kMfgUnlockStatusMagic;
        shared->win32Error = liveHookInstalled ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND;
        shared->wrapperPatchCount = static_cast<LONG>(
            gPatchedWrapperCandidates.load(std::memory_order_relaxed));
        shared->ngxPatchCount = static_cast<LONG>(
            gPatchedNgxCandidates.load(std::memory_order_relaxed));
        if (!logPath.empty())
            wcsncpy_s(shared->logPath, logPath.c_str(), _TRUNCATE);
        InterlockedExchange(&shared->state,
            BridgeReady() ? 1 : liveHookInstalled ? 0 : -1);
    }
    if (readyEvent)
        SetEvent(readyEvent);

    if (shared)
        UnmapViewOfFile(shared);
    if (mapping)
        CloseHandle(mapping);
    if (readyEvent)
        CloseHandle(readyEvent);
    PublishPatchRoute();
    PublishLiveBridge(initialControl);
    ControlConfig activeControl = initialControl;
    if (!xessStarted)
        startXess();
    uint32_t xessDiscoveryTicks = 0;
    reflex_control::SetRuntimeValidator(&ReflexRuntimeCurrent);
    StartupStep(L"Reflex discovery");
    DiscoverReflexModule();
    StartupStep(L"presentation policy");
    UpdatePresentationPolicy(activeControl);
    StartupStep(L"bridge status");
    if (!WriteBridgeStatus(activeControl, pid))
        Log(L"Could not publish CET bridge status file: %s", gStatusPath.c_str());
    StartupStep(L"watching the control file");
    latency_probe::Start(); // latmon.1/.2: the latency monitor's switches

    // The active frontend writes its selected config path when the user changes
    // the mode. Watch it off the presenting thread and atomically publish
    // changes for the SetOptions hook.
    uint32_t heartbeatTicks = 0;
    constexpr uint32_t kStatusFileSeconds = 30;
    uint32_t statusFileSeconds = 0;
    uint32_t inventoryTicks = 0;
    bool previousReady = BridgeReady();
    std::string previousRoute = PatchRouteName();
    auto optiscalerNgxRoute = ampere_backend::OptiScalerNgxRoute::eWaiting;
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    uint32_t stateSeconds = 0;
    LogSessionState(activeControl, false);
    // Whether a GPU driver's first-chance fault ended the session.
    bool faultAliveLogged = false, faultSeen = false;
    uint64_t faultPresents = 0;
#endif
    for (;;)
    {
        Sleep(100);
        single_module::DrainLog(&QueuedLog);
        latency_probe::Flush();
        LogProviderLoads();
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
        ULONGLONG faultTick = 0;
        ULONG64 faultAddress = 0;
        if (!faultAliveLogged && session_diagnostics::DriverFault(faultTick, faultAddress))
        {
            if (!faultSeen)
            {
                faultSeen = true;
                faultPresents = gTelemetryPresents.load(std::memory_order_relaxed);
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= faultTick + 5000)
            {
                faultAliveLogged = true;
                wchar_t where[160]{};
                session_diagnostics::DescribeAddress(faultAddress, where, _countof(where));
                Log(L"Still running %.1f s after the first-chance fault at %s; presents since: %llu",
                    double(now - faultTick) / 1000.0, where, static_cast<unsigned long long>(
                        gTelemetryPresents.load(std::memory_order_relaxed) - faultPresents));
            }
        }
#endif
        ++inventoryTicks;
        if (gModuleInventoryDirty.exchange(false, std::memory_order_acq_rel))
        {
            inventoryTicks = 0;
            InspectAlreadyLoadedModules();
            DiscoverReflexModule();
            xess_mod::DiscoverSdkModules();
            xess_d3d12::DiscoverUpscalers();   // XeSS SR and FidelityFX API modules
            xessDiscoveryTicks = 0;
        }
        // A missed load notification still finds the SDK pair within 5 s.
        if (++xessDiscoveryTicks >= 50)
        {
            xessDiscoveryTicks = 0;
            xess_mod::DiscoverSdkModules();
            xess_d3d12::DiscoverUpscalers();
        }
        FILETIME latestWriteTime{};
        if (ReadLastWriteTime(gConfigPath, latestWriteTime)
            && CompareFileTime(&latestWriteTime, &configWriteTime) != 0)
        {
            configWriteTime = latestWriteTime;
            ControlConfig control{};
            if (!ReadControlFile(gConfigPath, control))
            {
                Log(L"Ignored an invalid live control config update");
            }
            else
            {
                savedControl = control;
                optiscaler_host::SetUiMode(static_cast<optiscaler_host::UiMode>(control.optiScalerUi));
                uiDelegated = optiscaler_host::UiDelegated();
                control = EffectiveControl(control);
                const bool mfgChanged = !SameMfgControl(activeControl, control);
                activeControl = control;
                xess_mod::ConfigureModel(control.xessModel, OptiScalerHostModel());
                ConfigureXemfg(control);
                if (mfgChanged)
                    StoreControl(activeControl);
                else
                    temporal_interval_trace::SetEnabled(gpu_dispatch::UsesNativeProgram() || UseAmpere()
                        || activeControl.intervalLogging);
                PublishLiveBridge(activeControl);
                WriteBridgeStatus(activeControl, pid);
                Log(L"Live control requested: mode=%s multiplier=%ux dynamicTarget=%u FPS "
                    L"dynamicExperimental56=%d generatedOnlyDebug=%d "
                    L"intervalLogging=%d mfgChanged=%d",
                    activeControl.followGame ? L"follow"
                        : activeControl.dynamic ? L"dynamic" : L"fixed",
                    activeControl.multiplier,
                    activeControl.dynamicTargetFrameRate,
                    activeControl.dynamicExperimental56,
                    activeControl.generatedOnlyDebug,
                    activeControl.intervalLogging, mfgChanged);
            }
        }

        // OptiScaler mapped after startup (late route), the UI opt-in changed
        // through another writer, or a late-loaded plugin got OptiScaler's
        // plugin API (menutab.2): republish the effective control once.
        if (optiscaler_host::UiDelegated() != uiDelegated || optiscaler_menu_tab::Offered() != menuTabOffered)
        {
            uiDelegated = optiscaler_host::UiDelegated();
            menuTabOffered = optiscaler_menu_tab::Offered();
            const ControlConfig control = EffectiveControl(savedControl);
            const bool mfgChanged = !SameMfgControl(activeControl, control);
            activeControl = control;
            xess_mod::ConfigureModel(control.xessModel, OptiScalerHostModel());
            ConfigureXemfg(control);
            if (mfgChanged)
                StoreControl(activeControl);
            PublishLiveBridge(activeControl);
            WriteBridgeStatus(activeControl, pid);
            const auto host = optiscaler_host::Read();
            Log(L"OptiScaler mode %s: route=%hs version=%s path=%s",
                uiDelegated ? L"on (OptiScaler's menu owns frame generation)" : L"off",
                optiscaler_host::RouteName(host.route), host.productVersion, host.path);
        }

        // menutab.2: RTXMFG's tab in OptiScaler's menu, once the UI mode is hidden and OptiScaler handed its API.
        switch (optiscaler_menu_tab::Update())
        {
        case optiscaler_menu_tab::Change::eRegistered:
            Log(L"OptiScaler menu tab registered: \"MFG Unlocker\" (DLSS presets and DLSS 5 in OptiScaler's menu)");
            break;
        case optiscaler_menu_tab::Change::eRefused:
            Log(L"OptiScaler refused the menu tab; the saved DLSS presets and DLSS 5 still apply");
            break;
        case optiscaler_menu_tab::Change::eNone:
            break;
        }

        // nrouter.1: DLSS 5 on the game's DLSS calls into OptiScaler, outside OptiScaler's upscaler timer, once
        // NVIDIA's D3D12 NGX runtime route is ours. Until then, and if this fails, DLSS 5 stays on that route.
        if (optiscalerNgxRoute == ampere_backend::OptiScalerNgxRoute::eWaiting && optiscaler_host::Detected())
        {
            optiscalerNgxRoute = ampere_backend::InstallOptiScalerNgxRoute(optiscaler_host::Module());
            if (optiscalerNgxRoute == ampere_backend::OptiScalerNgxRoute::eInstalled)
                Log(L"OptiScaler D3D12 NGX exports hooked: DLSS 5 runs on the game's DLSS calls into OptiScaler, "
                    L"after its upscaling");
            else if (optiscalerNgxRoute == ampere_backend::OptiScalerNgxRoute::eFailed)
                Log(L"OptiScaler D3D12 NGX exports could not be hooked; DLSS 5 stays on NVIDIA's runtime route, "
                    L"inside OptiScaler's upscaler call");
        }

        UpdatePresentationPolicy(activeControl);
        temporal_interval_trace::Flush();

        const bool ready = BridgeReady();
        const std::string route = PatchRouteName();
        if (ready != previousReady || route != previousRoute)
        {
            previousReady = ready;
            previousRoute = route;
            PublishPatchRoute();
            WriteBridgeStatus(activeControl, pid);
            Log(L"Bridge readiness changed: ready=%d route=%hs wrappers=%u/%u ngx=%u/%u",
                ready, route.c_str(),
                gPatchedWrapperCandidates.load(std::memory_order_relaxed),
                gLoadedWrapperCandidates.load(std::memory_order_relaxed),
                gPatchedNgxCandidates.load(std::memory_order_relaxed),
                gLoadedNgxCandidates.load(std::memory_order_relaxed));
        }

        if (++heartbeatTicks >= 10)
        {
            const bool statusFile = statusFileEverySecond || ++statusFileSeconds >= kStatusFileSeconds;
            if (statusFile) statusFileSeconds = 0;
            WriteBridgeStatus(activeControl, pid, statusFile);
            heartbeatTicks = 0;
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
            const bool minute = ++stateSeconds >= 60;
            if (minute) stateSeconds = 0;
            LogSessionState(activeControl, minute);
#endif
        }
    }
}
}

BOOL SampleFrameTelemetry(uint64_t owner, uint32_t presentCount,
    bool outputAvailable, bool cumulative, bool dlssgPresented = false)
{
    gFpsOutputPresentTick.store(GetTickCount64(), std::memory_order_release);
    gTelemetryPresents.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard telemetryLock(gFpsTelemetryMutex);
    if (!gAppliedFrameGenerationOn.load(std::memory_order_acquire))
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        gFpsPresentCounter = {};
        return FALSE;
    }

    const temporal_interval_trace::Snapshot intervalTrace =
        temporal_interval_trace::ReadSnapshot();
    if (!intervalTrace.initialized || !intervalTrace.enabled)
    {
        if (gFpsTelemetryActive)
            ResetFpsTelemetry();
        gFpsTelemetryActive = false;
        gFpsPresentCounter = {};
        return FALSE;
    }

    if (!gFpsTelemetryActive)
    {
        ResetFpsTelemetry();
        gFpsPresentCounter = {};
        gFpsTelemetryActive = true;
    }

    if (dlssgPresented)
        UpdateFpsTelemetryForOutputPresent(0u, false, kFpsOutputDlssgPresented);
    else if (cumulative)
    {
        const auto delta = gFpsPresentCounter.Sample(owner, presentCount,
            GetTickCount64(), outputAvailable);
        if (delta.resetWindow) ResetFpsTelemetry();
        UpdateFpsTelemetryForOutputPresent(delta.frames, delta.available, 2u);
    }
    else
        UpdateFpsTelemetryForOutputPresent(1u, outputAvailable, 1u);
    return TRUE;
}

void frame_telemetry::SamplePresentCounter(
    uint64_t owner, uint32_t count, bool available)
{
    SampleFrameTelemetry(owner, count, available, true);
}

void frame_telemetry::SampleVulkanPresent()
{
    SampleFrameTelemetry(0, 0, false, false, true);
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockSampleFrameTelemetry()
{
    // A Vulkan/application callback alone does not count generated presents.
    return SampleFrameTelemetry(0, 0, false, true);
}

extern "C" __declspec(dllexport) BOOL WINAPI MfgUnlockCoreLoaded()
{
    return TRUE;
}

extern "C" BOOL WINAPI MfgUnlockCoreEntry(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        const DWORD bypassTlsIndex = TlsAlloc();
        if (bypassTlsIndex == TLS_OUT_OF_INDEXES)
            return FALSE;
        gInternalControlBypassTlsIndex.store(
            bypassTlsIndex, std::memory_order_release);
        // Dynamic TLS does not require thread attach notifications. Some host
        // loader states can still reject this optimization; DllMain's frame is
        // deliberately kept small so an unexpected notification remains safe.
        DisableThreadLibraryCalls(instance);
        gpu_backend::SetLogCallback(&MidpointLog);
        gExecutablePathBuffer.fill(L'\0');
        GetModuleFileNameW(nullptr, gExecutablePathBuffer.data(),
            static_cast<DWORD>(gExecutablePathBuffer.size()));
        gExecutablePathBuffer.back() = L'\0';
        gExecutablePath = gExecutablePathBuffer.data();
        gExecutableDirectory = ParentPath(gExecutablePathBuffer.data());
        // The slInit detour runs before the worker thread in early loaders.
        // Resolve the startup-only wrapper policy now so HookSlInit can read
        // the same control file before Streamline selects its plugins.
        gConfigPath = ResolveConfigPath(instance, gExecutableDirectory);
        // The OptiScaler UI opt-in decides which install steps run below and
        // in DllMain; read it before them (one bounded file read).
        if (optiscaler_host::Detected())
        {
            ControlConfig saved{};
            if (ReadControlFile(gConfigPath, saved))
                optiscaler_host::SetUiMode(static_cast<optiscaler_host::UiMode>(saved.optiScalerUi));
        }
        dlssg_preset::SetInitialSelectionLoader(&ReadInitialDlssgPreset);
        dlssg_preset::SetCreateQuiescenceProbe(&ampere_backend::FrameGenerationQuiescent);
        dlss_sr::SetLogCallback(&MidpointLog);
        dlss5::SetLogCallback(&MidpointLog);
        dlss_sr::SetInitialSelectionLoader(&ReadInitialDlssSrPreset);
        dlss_sr::SetInitialRrSelectionLoader(&ReadInitialDlssRrPreset);
        dlss_sr::SetDrsHookInstaller(&InstallDlssSrDrsHook);
        dlss_sr::SetRuntimeEligibilityProbe(&LivePresetGpu);
        dlssg_preset::SetSettingFilter(dlss_sr::kDrsPresetSetting, &dlss_sr::FilterDrsRead);
        dlssg_preset::SetSettingFilter(dlss_sr::kDrsRrPresetSetting, &dlss_sr::FilterDrsRead);
        if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
        {
            const std::wstring path = LoadedModulePath(interposer);
            InstallStreamlineLoaderDiscovery(interposer, path);
            InstallSlCommonResolverDiscovery(interposer, path);
        }
        InstallSlInitControlPath();
        gLiveHookInstalled.store(InstallFeatureFunctionHook(), std::memory_order_release);
        InstallD3DDeviceHook();
        InstallVulkanInfoHook();
        InstallUiTagHooks();
        // The imported core is initialized before the loader-facing ASI's
        // DllMain. Install the bounded entry hooks synchronously when their
        // modules already exist, closing the worker-start race for pointers
        // which Streamline cached before this plugin was discovered.
        if (HMODULE common = GetModuleHandleW(L"sl.common.dll"))
        {
            InstallSlCommonResolverDiscovery(
                common, LoadedModulePath(common));
        }
        if (HMODULE provider = GetModuleHandleW(L"nvngx_dlssg.dll"))
        {
            InspectLoadedModule(provider, LoadedModulePath(provider));
        }
        if (HMODULE runtime = GetModuleHandleW(L"_nvngx.dll"))
        {
            InspectLoadedModule(runtime, LoadedModulePath(runtime));
        }
        RegisterDllNotification();
        HANDLE thread = CreateThread(nullptr, 0, PatchWorker, instance, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
