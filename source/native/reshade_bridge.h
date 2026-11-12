#pragma once
#include "dlss5_policy.h"
#include "build_variant.h"

#include <Windows.h>

#include <cstdint>

// One DLSS feature in the menu snapshot: Super Resolution from the
// dlssSr* status keys, Ray Reconstruction from dlssRr*. Presets: 0 game/driver,
// SR 10..13 J..M, RR 4..6 D..F. Active/driver/hint values describe the game's
// most recently evaluated feature; the live feature counts decide whether the
// menu shows the SR or the RR controls.
struct MfgUnlockDlssFeature
{
    uint32_t presetRequested = 0;
    BOOL detected = FALSE;
    BOOL running = FALSE;
    uint32_t liveFeatures = 0;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    int32_t quality = -1;
    uint32_t gameHint = 0;
    uint32_t activePreset = 0;
    // dlss_sr::Unavailable for the selection on that feature (0 = live).
    uint32_t liveSwitch = 0;
    // The provider's one DRS preset read per session: seen, the driver's
    // (NVIDIA app) value, answered "not set" for a selected preset, whether
    // that value fixes the preset until a restart, and a cached driver value
    // (which can coexist with a live selector hook).
    BOOL drsObserved = FALSE;
    BOOL driverPresetValid = FALSE;
    uint32_t driverPreset = 0;
    BOOL drsSuppressed = FALSE;
    BOOL fixedByDriver = FALSE;
    BOOL driverOverrideCached = FALSE;
};

struct MfgUnlockReShadeSnapshot
{
    uint32_t structSize = sizeof(MfgUnlockReShadeSnapshot);
    uint32_t startupProfile = 0;
    uint32_t startupFailure = 0;
    uint32_t startupValidationMask = 0;
    BOOL centralHookInstalled = FALSE;
    BOOL startupSnapshotClean = FALSE;
    BOOL workerIdentityCertified = FALSE;
    BOOL bridgeReady = FALSE;
    BOOL gameFrameGenerationOn = FALSE;
    BOOL appliedFrameGenerationOn = FALSE;
    BOOL streamlineRebuildRequired = FALSE;
    BOOL dynamicMode = FALSE;
    BOOL appliedDynamicMode = FALSE;
    BOOL dynamicExperimental56 = FALSE;
    BOOL generatedOnlyDebug = FALSE;
    BOOL perSampleSynthesisReady = FALSE;
    BOOL highCapabilityPublicationAllowed = FALSE;
    uint32_t desiredMultiplier = 2;
    uint32_t appliedMultiplier = 0;
    uint32_t dynamicTargetFrameRate = 0;
    uint32_t actualFramesPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
    int32_t lastSetOptionsResult = 0;
    int32_t lastGetStateResult = 0;
    uint64_t desiredRevision = 0;
    uint64_t appliedRevision = 0;
    char patchRoute[16]{};
    uint32_t backendAbiVersion = 0;
    BOOL transportEarlyHostCertified = FALSE;
    BOOL dllNotificationRegistered = FALSE;
    BOOL pluginLoaderHooksInstalled = FALSE;
    BOOL preEntryRouteInstalled = FALSE;
    BOOL selectedOwnerPublished = FALSE;
    uint64_t preEntryRouteAttempts = 0;
    uint64_t preEntryRouteSuccesses = 0;
    uint64_t preEntryRouteFailures = 0;
    uint32_t frontendClients = 0;
    BOOL followGameMode = TRUE;
    uint32_t statusProtocolVersion = 0;
    BOOL loaderCoreImported = FALSE;
    BOOL setOptionsEntryDetourCurrent = FALSE;
    BOOL setOptionsResolverFallbackActive = FALSE;
    uint64_t setOptionsResolverFallbackCalls = 0;
    BOOL setOptionsControlPathReady = FALSE;
    BOOL getStateEntryDetourCurrent = FALSE;
    BOOL ngxCreateEntryDetourCurrent = FALSE;
    BOOL ngxEvaluateEntryDetourCurrent = FALSE;
    BOOL backportReadyAtCreate = FALSE;
    BOOL pipelineMayPredateDetour = FALSE;
    BOOL setOptionsSeen = FALSE;
    BOOL setOptionsAccepted = FALSE;
    BOOL getStateSeen = FALSE;
    BOOL intervalLoggingEnabled = FALSE;
    BOOL intervalLogReady = FALSE;
    uint64_t intervalValidSamples = 0;
    uint64_t intervalInvalidSamples = 0;
    uint64_t intervalDroppedSamples = 0;
    int32_t intervalLastCount = 0;
    int32_t intervalLastIndex = 0;
    uint32_t intervalLastPositionNumerator = 0;
    uint32_t intervalLastPositionDenominator = 0;
    uint32_t realFpsMilli = 0;
    uint32_t dlssFpsMilli = 0;
    uint64_t fpsSampleAgeMs = 0;
    BOOL uiInputsReady = FALSE;
    BOOL uiRecompositionEnabled = FALSE;
    BOOL uiRecompositionForced = FALSE;
    BOOL appliedGeneratedOnlyDebug = FALSE;
    char intervalLogFile[64]{};
    BOOL mainResolverDiscoveryInstalled = FALSE;
    BOOL slInitEntryDetourCurrent = FALSE;
    BOOL slInitIatFallbackInstalled = FALSE;
    BOOL slInitResolverFallbackActive = FALSE;
    BOOL slInitControlPathReady = FALSE;
    uint64_t slInitCalls = 0;
    uint64_t slInitFlagsBefore = 0;
    uint64_t slInitFlagsAfter = 0;
    BOOL otaPreferencesForced = FALSE;
    BOOL otaPreferencesEnabledAtInit = FALSE;
    BOOL downloadedStreamlinePluginsEnabledAtInit = FALSE;
    BOOL otaProviderPreflightSupported = FALSE;
    BOOL otaForceSuppressed = FALSE;
    BOOL streamlineLoaderDiscoveryInstalled = FALSE;
    uint64_t streamlineLoaderDiscoveryCalls = 0;
    BOOL activeWrapperObserved = FALSE;
    BOOL activeWrapperUsesNvidiaOta = FALSE;
    uint32_t activeWrapperVersionMajor = 0;
    uint32_t activeWrapperVersionMinor = 0;
    uint32_t activeWrapperVersionBuild = 0;
    uint32_t activeWrapperVersionPrivate = 0;
    uint32_t wrapperCompiledMaximumGeneratedFrames = 0;
    uint32_t safeMaximumMultiplier = 2;
    BOOL requestedMultiplierLimited = FALSE;
    BOOL nvidiaCompatibilityResolved = FALSE;
    int32_t nvidiaProfileStatus = -1;
    char nvidiaProfileName[128]{};
    // Protocol 40: 0 unknown, 2 native FG listed without MFG override, 4/6 listed MFG maximum.
    uint32_t nvidiaCompatibilityTier = 0;
    uint32_t nvidiaCompatibilityManifestEntries = 0;
    // Tier 2 uses the mod's conservative 2x bound; NVIDIA's MFG cell is blank.
    uint32_t nvidiaPolicyCeilingMultiplier = 0;
    uint32_t wrapperNativeMaximumMultiplier = 2;
    BOOL compatibilityFallback = FALSE;
    uint32_t compatibilityReason = 0;
    BOOL fullStreamlineOtaRequested = FALSE;
    BOOL fullStreamlineOtaEligible = FALSE;
    BOOL downloadedStreamlinePluginsForced = FALSE;
    uint32_t streamlineHostVersionMajor = 0;
    uint32_t streamlineHostVersionMinor = 0;
    uint32_t streamlineHostVersionBuild = 0;
    uint32_t streamlineHostVersionPrivate = 0;
    BOOL dynamicMfgSupportKnown = FALSE;
    BOOL dynamicMfgSupported = FALSE;
    char activeWrapperPath[512]{};
    uint64_t activeWrapperGeneration = 0;
    char activeControlPath[16]{};
    char activeStatePath[16]{};
    char activeControlDetour[16]{};
    char activeStateDetour[16]{};
    char activeProviderPath[512]{};
    uint32_t activeProviderVersionMajor = 0;
    uint32_t activeProviderVersionMinor = 0;
    uint32_t activeProviderVersionBuild = 0;
    uint32_t activeProviderVersionPrivate = 0;
    uint64_t activeProviderGeneration = 0;
    char providerSelectionSource[24]{};
    char providerCreateDetour[16]{};
    char providerEvaluateDetour[16]{};
    BOOL midpointReadyAtFirstCreate = FALSE;
    uint64_t activeLastCallRevision = 0;
    uint64_t activeLastAcceptedRevision = 0;
    uint32_t universalRouteFailure = 0;
    char universalRouteFailureReason[64]{};
    BOOL releaseEntryCurrent = FALSE;
    BOOL frameGenerationOffAccepted = FALSE;
    BOOL releaseObserved = FALSE;
    uint32_t dlssgPresetRequested = 0; // latency.1: game / driver by default (was B)
    BOOL dlssgPresetOverrideInstalled = FALSE;
    uint32_t dlssgPresetOverrideFailure = 0;
    uint64_t dlssgPresetReadCount = 0;
    BOOL ampereProgramReadyMfg = FALSE;
    uint32_t ampereKernelImage = 0;
    uint32_t ampereNativeCacheStatus = 0;
    uint32_t ampereCertifiedMaximum = 0;
    uint32_t ampereFailure = 0;
    uint64_t presetQueries = 0;

    uint32_t ampereFirstFailure = 0;
    uint32_t ampereFirstNgxResult = 0;
    uint32_t amperePrimaryFailure = 0;
    uint32_t amperePrimaryNgxResult = 0;
    uint32_t ampereLastFailure = 0;
    uint32_t ampereLastNgxResult = 0;
    uint64_t ampereCreateAttempts = 0;
    uint64_t ampereCreateBlockedBeforeProvider = 0;
    uint64_t ampereEvaluateAttempts = 0;
    uint32_t amperePreparationStage = 0;
    uint32_t ampereStartupFailureMask = 0;
    // admitoff.1: admission failed, so the game's DLSS-G request is held at Off.
    BOOL ampereAdmissionUnavailable = FALSE;
    uint64_t ampereAdmissionHeldRequests = 0;
    uint32_t ampereCandidateVersionMajor = 0;
    uint32_t ampereCandidateVersionMinor = 0;
    uint32_t ampereCandidateVersionBuild = 0;
    uint64_t ampereCreatedFeatures = 0;
    uint64_t ampereEvaluations = 0;
    uint32_t gpuFamily = 0; // 0 unknown, 1 Ada, 2 Ampere, 3 conflict, 4 Turing RTX, 5 Blackwell RTX
    uint64_t gpuAdapterLuid = 0;
    uint32_t gpuSelectionFailure = 0;

    uint32_t dlssgPresetLatched = 2;
    BOOL dlssgPresetSelectionFrozen = FALSE;
    BOOL dlssgPresetRestartRequired = FALSE;
    uint32_t dlssgPresetObserved = 0;
    BOOL dlssgPresetObservedValid = FALSE;
    uint64_t dlssgPresetOverrideReadCount = 0;
    float appliedDynamicTargetFrameRate = 0.0f;
    BOOL appliedDynamicTargetValid = FALSE;
    BOOL fgVsyncSupportKnown = FALSE;
    BOOL fgVsyncSupported = FALSE;

    uint32_t vsyncMode = 0; // 0 game, 1 off, 2 on; application request only.
    uint32_t reflexFrameLimitFps = 0; // 0 preserves the game's limiter.
    BOOL dynamicVsyncAvailable = FALSE;
    BOOL vsyncControlAvailable = FALSE;
    BOOL vsyncOverrideApplied = FALSE;
    BOOL vsyncPresentationObserved = FALSE;
    uint32_t vsyncOriginalInterval = 0;
    uint32_t vsyncSubmittedInterval = 0;
    uint32_t vsyncFailure = 0;
    BOOL reflexControlAvailable = FALSE;
    BOOL reflexAppliedKnown = FALSE;
    uint32_t reflexAppliedFrameLimitUs = 0;
    BOOL reflexLimitPending = FALSE;
    BOOL reflexRestorePending = FALSE;
    uint32_t reflexStatus = 0;
    int32_t reflexLastResult = 0;
    uint32_t reflexHookMask = 0;
    uint32_t reflexModuleVersionMajor = 0;
    uint32_t reflexModuleVersionMinor = 0;
    uint32_t reflexModuleVersionPatch = 0;
    uint64_t reflexModuleGeneration = 0;

    BOOL ampereLegacySinglePreset = FALSE;

    // The game's own DLSS-G UI recomposition option (RTXMFG never changes it).
    BOOL gameUiRecompositionEnabled = FALSE;

    // The provider implements Frame Generation Preset C (DLSS-G 310.9.16 and newer).
    BOOL dlssgPresetCAvailable = FALSE;

    // Live preset switching: dlssg_preset_switch::Unavailable, 0 when a
    // saved preset replaces the running one. A backend without the field keeps
    // restart-to-apply (2, provider).
    uint32_t dlssgPresetLiveSwitch = 2;
    BOOL dlssgPresetSwitching = FALSE;

    // DLSS Super Resolution and Ray Reconstruction.
    MfgUnlockDlssFeature dlssSr{};
    MfgUnlockDlssFeature dlssRr{};

    // NGX graphics API of the FG pipeline, or of the device that selected the
    // GPU before the first Create: 0 unknown, 1 D3D12, 2 Vulkan.
    uint32_t graphicsApi = 0;

    // V-Sync on Vulkan (vsync_control.h): the vsync* readback describes the
    // game's live Vulkan swapchain, whose present mode (VkPresentModeKHR) is
    // fixed at creation. vsyncVulkanHook: the creation wrapper is installed.
    BOOL vsyncVulkanSwapchain = FALSE;
    BOOL vsyncVulkanHook = FALSE;
    uint32_t vsyncOriginalPresentMode = 0;
    uint32_t vsyncSubmittedPresentMode = 0;

    // Source of dlssFpsMilli: 0 unavailable, 1 present callback, 2 DXGI
    // present count, 4 DLSS-G's own presented-frame count.
    uint32_t fpsOutputSource = 0;
    // DLSSNR: saved settings and independently observed execution.
    dlss5::Settings dlss5{};
    BOOL dlss5Detected=FALSE, dlss5Pending=FALSE;
    uint32_t dlss5State=0, dlss5Result=0, dlss5Route=0, dlss5Stage=0, dlss5Passes=0;
    uint32_t dlss5Width=0, dlss5Height=0, dlss5ExtentWidth=0, dlss5ExtentHeight=0;
    uint64_t dlss5Evaluations=0;
    // DLSS 5 Auto: live Resolution Scale, stage GPU time (-1 before the first
    // measurement) and its budget, adaptive exposure gain.
    float dlss5EffectiveScale=0.0f, dlss5GpuMs=-1.0f, dlss5BudgetMs=0.0f, dlss5ExposureGain=1.0f;
    // OptiScaler UI owner (status "optiScalerUiOwner"): 0 none, 1 shared (its menu
    // owns frame generation, this menu the DLSS presets and DLSS 5), 2 optiscaler
    // (no RTXMFG menu), 3 rtxmfg.
    uint32_t optiScalerUiOwner=0;
    // DLSS 5: the real frame interval its GPU time is a share of (status "dlss5FrameMs").
    float dlss5FrameMs=0.0f;
    // Boost: a network evaluation's GPU time; dlss5GpuMs is then the mean per
    // evaluation (status "dlss5BoostNetworkMs", -1 without Boost).
    float dlss5BoostNetworkMs=-1.0f;
    // Resolution Scale Auto holds its quality floor while DLSS 5 stays over
    // budget (status "dlss5ScaleFloorHeld").
    BOOL dlss5ScaleFloorHeld=FALSE;
    // dx11nr.1: the game's DLSS runs on DirectX 11 and DLSS 5 through the DirectX 11 bridge (status "dlss5D3D11");
    // why the bridge leaves its frames as they are (status "dlss5D3D11Refusal", dlss5_policy.h d3d11::Refusal).
    BOOL dlss5D3D11=FALSE;
    uint32_t dlss5D3D11Refusal=0;
    // latmon.2: live PC latency (latency_probe.h; status "latency*"). latencyAvailable: [Overlay] LatencyMonitor is
    // not 0. latencyState (latency_monitor.h State): 0 off, 1 measuring, 2 live, 3 no Reflex markers, 4 no output
    // timing on this route, 5 no flip times. Means over the last second of frames, in microseconds: input to the first
    // output's flip (mean, 99th percentile), to the real frame's flip, and the game / frame generation / display parts.
    BOOL latencyAvailable=FALSE;
    uint32_t latencyState=0, latencyFrames=0, latencyExactPermille=0;
    uint32_t latencyMeanUs=0, latencyP99Us=0, latencyRealUs=0, latencyGameUs=0, latencyFgUs=0, latencyDisplayUs=0;
    uint64_t latencyAgeMs=UINT32_MAX;
};

