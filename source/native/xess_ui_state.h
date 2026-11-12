#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

// Display policy only. This code never calls a provider. XeSS MFG runs under
// OptiScaler only and OptiScaler's menu owns its controls, so the menu shows status here.
namespace xess_ui
{
struct State
{
    BOOL available = FALSE, hooksActive = FALSE, optiScaler = FALSE;
    BOOL initialized = FALSE, enabled = FALSE;
    uint32_t applied = 0, capacity = 0, framesPresented = 0, adapterVendor = 0, gpuVendorId = 0;
    uint32_t modelActive = 0; // 0 none yet, 1 our port of Intel's MFG model, 2 XeFG's 2x model
    BOOL providerFailing = FALSE; // the Nvngx replacement provider's recent batches all failed
    // The port's int8 layers on tensor cores: 0 not decided, 1 active, 2 unavailable, 3 off.
    uint32_t tensorCores = 0, tensorCoreLaunches = 0, tensorCoreFallbacks = 0;
    std::string tensorCoreReason;
    std::string runtimeFailure, status, adapter;
    std::string activeFrameGeneration = "none";
    // XeMFG standalone, RTXMFG as the XeFG host without OptiScaler (the saved switch and frame count, and
    // the host of the game's API: state off/optiscaler/waiting/active/restart/unavailable, why, input source).
    BOOL xemfgStandalone = FALSE, xemfgGenerating = FALSE, xemfgGameFg = FALSE, xemfgFsrFg = FALSE;
    uint32_t xemfgSavedFrames = 0, xemfgFrames = 0;
    std::string xemfgState, xemfgReason, xemfgSource, xemfgApi;
    // After a failed runtime search, the RTXMFG-XeSS folder beside MFG Unlocker, the game's EXE folder
    // and a near-miss name (UTF-8; empty from older backends).
    std::string xemfgRuntimeSearch, xemfgRuntimeGame, xemfgRuntimeMisnamed;
    // oldruntime.1: after a pair loaded without every export, its folder, the short file, that file's version and the
    // folder a newer pair takes over from (UTF-8; empty from older backends).
    std::string xemfgRuntimeOld, xemfgRuntimeOldFile, xemfgRuntimeOldVersion, xemfgRuntimeNewer;
    // The game's and XeMFG's frame rates (FPS x 1000) and the sample's age in ms (absent = no sample), the
    // FPS limit the host applies and whether XeLL holds it.
    uint32_t xemfgBaseFpsMilli = 0, xemfgOutputFpsMilli = 0, xemfgFpsAgeMs = UINT32_MAX, xemfgFpsLimit = 0;
    BOOL xemfgFpsLimited = FALSE;
    // Dynamic MFG's saved settings (xemfgSavedFrames is its most frames: target 0 = the FPS limit, else just
    // under the refresh rate; rounding 0 at most, 1 nearest, 2 at least), and while it runs its target and cap mode.
    BOOL xemfgDynamic = FALSE, xemfgAllow1x = TRUE, xemfgTargetAsCap = FALSE;
    uint32_t xemfgTargetFps = 0, xemfgRounding = 1;
    BOOL xemfgDynamicActive = FALSE, xemfgDynamicCap = FALSE;
    uint32_t xemfgDynamicTargetMilli = 0;
    // latency.1: low-latency pacing's saved mode (0 auto, 1 on, 2 off) and readback (latencyPacingDetail): the real
    // frames measured, and over the frames since the backend's previous status the hold in Present, the hold without
    // the sleep, the sleep (ms), the sleeps taken and missed, the guard trips and the last real frame's age (ms, -1
    // none). Absent from older backends: detail false.
    BOOL latencyPacingDetail = FALSE;
    uint32_t latencyPacing = 0;
    unsigned long long latencyPacingFrames = 0, latencyPacingWindow = 0, latencyPacingTakes = 0, latencyPacingMissed = 0;
    unsigned long long latencyPacingBackoffs = 0, latencyPacingIneffective = 0;
    double latencyPacingHeldMs = 0, latencyPacingSlackMs = 0, latencyPacingSleptMs = 0;
    long long latencyPacingAgeMs = -1;
};

// latency.1: the backend's latencyPacingDetail string (patcher.cpp LatencyPacingStatus).
inline bool ParseLatencyPacing(const std::string& detail, State& state) noexcept
{
    State next = state;
    if (sscanf_s(detail.c_str(), "mode=%u frames=%llu window=%llu held=%lf slack=%lf slept=%lf takes=%llu missed=%llu "
            "backoffs=%llu ineffective=%llu age=%lld", &next.latencyPacing, &next.latencyPacingFrames,
            &next.latencyPacingWindow, &next.latencyPacingHeldMs, &next.latencyPacingSlackMs, &next.latencyPacingSleptMs,
            &next.latencyPacingTakes, &next.latencyPacingMissed, &next.latencyPacingBackoffs,
            &next.latencyPacingIneffective, &next.latencyPacingAgeMs) != 11 || next.latencyPacing > 2) return false;
    next.latencyPacingDetail = TRUE;
    state = next;
    return true;
}
// latency.1: low-latency pacing's sample is fresh (the backend rewrites its status about once a second).
inline bool LatencyPacingCurrent(const State& state) noexcept
{
    return state.latencyPacingDetail && state.latencyPacingWindow > 0 && state.latencyPacingAgeMs >= 0
        && state.latencyPacingAgeMs <= 2000;
}

// A frame-rate sample fresh enough to show (the backend rewrites its status about once a second).
inline bool XemfgFpsCurrent(const State& state) noexcept
{
    return state.xemfgBaseFpsMilli > 0 && state.xemfgFpsAgeMs <= 2000;
}
// XeMFG standalone runs here: the switch is on without OptiScaler and a host has the game's chain (or waits for it).
inline bool XemfgRuns(const State& state) noexcept
{
    return state.xemfgStandalone && !state.optiScaler && (state.xemfgState == "active" || state.xemfgState == "waiting");
}

// Use the frontend's existing scalar readers. Absent fields stay false or zero,
// so an older native snapshot cannot imply an applied count.
template<class BoolReader, class IntegerReader, class StringReader>
State Read(const std::string& text, BoolReader boolean, IntegerReader integer, StringReader string)
{
    State next;
    integer(text, "gpuVendorId", next.gpuVendorId);
    string(text, "activeFrameGeneration", next.activeFrameGeneration);
    boolean(text, "optiScalerDetected", next.optiScaler);
    boolean(text, "xemfgStandalone", next.xemfgStandalone);
    boolean(text, "xemfgGenerating", next.xemfgGenerating);
    boolean(text, "xemfgGameFg", next.xemfgGameFg);
    boolean(text, "xemfgFsrFg", next.xemfgFsrFg);
    integer(text, "xemfgSavedFrames", next.xemfgSavedFrames);
    integer(text, "xemfgFrames", next.xemfgFrames);
    string(text, "xemfgState", next.xemfgState);
    string(text, "xemfgReason", next.xemfgReason);
    string(text, "xemfgSource", next.xemfgSource);
    string(text, "xemfgApi", next.xemfgApi);
    string(text, "xemfgRuntimeSearch", next.xemfgRuntimeSearch);
    string(text, "xemfgRuntimeGame", next.xemfgRuntimeGame);
    string(text, "xemfgRuntimeMisnamed", next.xemfgRuntimeMisnamed);
    string(text, "xemfgRuntimeOld", next.xemfgRuntimeOld);
    string(text, "xemfgRuntimeOldFile", next.xemfgRuntimeOldFile);
    string(text, "xemfgRuntimeOldVersion", next.xemfgRuntimeOldVersion);
    string(text, "xemfgRuntimeNewer", next.xemfgRuntimeNewer);
    integer(text, "xemfgBaseFpsMilli", next.xemfgBaseFpsMilli);
    integer(text, "xemfgOutputFpsMilli", next.xemfgOutputFpsMilli);
    integer(text, "xemfgFpsAgeMs", next.xemfgFpsAgeMs);
    integer(text, "xemfgFpsLimit", next.xemfgFpsLimit);
    boolean(text, "xemfgFpsLimited", next.xemfgFpsLimited);
    boolean(text, "xemfgDynamic", next.xemfgDynamic);
    integer(text, "xemfgTargetFps", next.xemfgTargetFps);
    integer(text, "xemfgRounding", next.xemfgRounding);
    boolean(text, "xemfgAllow1x", next.xemfgAllow1x);
    boolean(text, "xemfgTargetAsCap", next.xemfgTargetAsCap);
    boolean(text, "xemfgDynamicActive", next.xemfgDynamicActive);
    boolean(text, "xemfgDynamicCap", next.xemfgDynamicCap);
    integer(text, "xemfgDynamicTargetMilli", next.xemfgDynamicTargetMilli);
    std::string latency;
    string(text, "latencyPacingDetail", latency);
    if (!latency.empty()) ParseLatencyPacing(latency, next);
    if (!boolean(text, "xessAvailable", next.available) || !next.available) return next;
    boolean(text, "xessHooksActive", next.hooksActive);
    boolean(text, "xessInitialized", next.initialized);
    boolean(text, "xessEnabled", next.enabled);
    integer(text, "xessApplied", next.applied);
    integer(text, "xessCapacity", next.capacity);
    integer(text, "xessFramesPresented", next.framesPresented);
    integer(text, "xessAdapterVendor", next.adapterVendor);
    integer(text, "xessModelActive", next.modelActive);
    boolean(text, "xessProviderFailing", next.providerFailing);
    integer(text, "xessTensorCores", next.tensorCores);
    integer(text, "xessTensorCoreLaunches", next.tensorCoreLaunches);
    integer(text, "xessTensorCoreFallbacks", next.tensorCoreFallbacks);
    string(text, "xessTensorCoreReason", next.tensorCoreReason);
    string(text, "xessRuntimeFailure", next.runtimeFailure);
    string(text, "xessStatus", next.status);
    string(text, "xessAdapter", next.adapter);
    return next;
}

inline uint32_t Vendor(const State& state) noexcept
{
    return state.gpuVendorId ? state.gpuVendorId : state.adapterVendor;
}
// Which settings the menu shows: the frame generation selected in the game.
// XeSS frame generation on shows XeSS and DLSS frame generation on shows DLSS,
// on any GPU. With frame generation off the menu keeps the last one the game
// turned on this session (lastActive). Before the game has turned either on,
// XeSS shows on non-NVIDIA GPUs, in games offering only XeSS, and on NVIDIA
// GPUs the DLSS backend checked and found without DLSS MFG (gpuFamily 1 Ada,
// 2 Ampere, 4 Turing RTX, 5 Blackwell RTX); DLSS shows otherwise.
enum class Section { Dlss, Xess };
constexpr uint32_t kNvidiaVendor = 0x10de;
inline bool DlssMfgFamily(uint32_t gpuFamily) noexcept
{
    return gpuFamily == 1 || gpuFamily == 2 || gpuFamily == 4 || gpuFamily == 5;
}
inline Section SettingsSection(const State& state, const std::string& lastActive, bool dlssOffered,
    uint32_t gpuFamily) noexcept
{
    // With XeMFG standalone switched on, XeMFG replaces the game's frame generation on every GPU.
    if (state.xemfgStandalone && !state.optiScaler) return Section::Xess;
    const std::string& selected = state.activeFrameGeneration != "none" ? state.activeFrameGeneration : lastActive;
    if (selected == "xess") return Section::Xess;
    if (selected == "dlss") return Section::Dlss;
    const uint32_t vendor = Vendor(state);
    if (vendor && vendor != kNvidiaVendor) return Section::Xess;
    if (!state.available || !state.hooksActive) return Section::Dlss;
    if (!dlssOffered) return Section::Xess;
    // gpuVendorId is published only after the DLSS backend's device check, so
    // an unchecked GPU keeps the DLSS settings it may still need before FG is on.
    return state.gpuVendorId == kNvidiaVendor && !DlssMfgFamily(gpuFamily) ? Section::Xess : Section::Dlss;
}
inline bool AppliedKnown(const State& state) noexcept
{
    // An accepted count needs a matching allocation.
    return state.initialized && state.applied >= 2 && state.applied <= 6 && state.capacity >= state.applied - 1;
}

// portstatus.1: the model OptiScaler's XeFG runs. The XMX port runs Intel's multi-frame model as DXIL on the shader
// cores; it is the CUDA port once one of its int8 layers has run on the tensor cores.
enum class FgPort { None, XmxPort, CudaPort, Intel2x };
inline FgPort ActivePort(const State& state) noexcept
{
    if (!state.available || !state.optiScaler) return FgPort::None;
    if (state.modelActive == 2) return FgPort::Intel2x;
    if (state.modelActive != 1) return FgPort::None;
    return state.tensorCores == 1 && state.tensorCoreLaunches ? FgPort::CudaPort : FgPort::XmxPort;
}
// Why the XMX port is not the CUDA port (the shared menu's Tensor cores row): the route is ready but no layer has
// run, OptiScaler.ini turned it off, or this NVIDIA GPU or driver has none. Other vendors get no note.
enum class PortTensorCores { None, Ready, OffSetting, Unavailable };
inline PortTensorCores PortTensorCoreNote(const State& state) noexcept
{
    if (ActivePort(state) != FgPort::XmxPort) return PortTensorCores::None;
    if (state.tensorCores == 1) return PortTensorCores::Ready;
    if (state.tensorCores == 3) return PortTensorCores::OffSetting;
    if (state.tensorCores == 2 && Vendor(state) == kNvidiaVendor) return PortTensorCores::Unavailable;
    return PortTensorCores::None;
}

enum class VsyncReadback { Pending, Overridden, AlreadyOff };
constexpr VsyncReadback VsyncResult(bool available, bool observed, bool overridden,
    uint32_t originalInterval, uint32_t submittedInterval) noexcept
{
    if (!available || !observed || submittedInterval != 0) return VsyncReadback::Pending;
    if (overridden && originalInterval != 0) return VsyncReadback::Overridden;
    if (!overridden && originalInterval == 0) return VsyncReadback::AlreadyOff;
    return VsyncReadback::Pending;
}
}
