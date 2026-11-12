#pragma once
#include "dlss5.h"
#include "dlss5_auto.h"
#include "frame_gen_target.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

// What the D3D12 stage (dlss5.cpp) and the Vulkan stage (dlss5_vulkan.cpp)
// keep and decide alike. Textures, creation, release, recording and completion
// stay with each stage. API-neutral: no D3D12 or Vulkan types.
namespace dlss5::common {
// A provider feature's passes and what it was created with; each stage adds
// its textures at the processing extent.
struct FeatureCore {
    bool createdReady = false;
    // The settings the feature was created with (SameFeature fields).
    Settings applied{};
    NVSDK_NGX_Parameter* params = nullptr;
    std::array<NVSDK_NGX_Handle*, kMaxPasses> handles{};
    uint32_t passes = 0;
    uint32_t width = 0, height = 0, extentWidth = 0, extentHeight = 0;
    // autorect.1: the network's rect (from 0,0) and Resolution Scale on the latest
    // evaluation (Activate); the textures keep the processing extent. 0 before one.
    uint32_t activeWidth = 0, activeHeight = 0;
    float activeScale = 0.0f;
    bool HasFeature() const noexcept { return passes != 0; }
};
// A slot's plain state; each stage adds its device, NGX entries, features and
// measurements.
struct SlotState {
    size_t route = 0;
    uintptr_t game = 0;
    bool occupied = false, retiring = false, nativeAttempted = false, direct = false;
    bool failed = false, resetHistory = true, directOwnershipDenied = false, nativeSucceeded = false, cleanupFailed = false;
    bool ownershipLogged = false; uint32_t nativeResult = 0;
    uint64_t revision = 0, failureRevision = 0;
    State failureState = State::eOff; uint32_t failureResult = 0;
    dlss_sr::CreateKeys keys{};
    // Encoding Auto resolves to this (the Present stage: the swapchain's).
    Encoding automatic = Encoding::eLinear;
    // DLSS 5 Auto: the Resolution Scale level, adaptive exposure and the real
    // frame clock, fed by this slot's GPU timestamps and luminance meter.
    auto_mode::ScaleController scaling{};
    bool scalingSeeded = false, floorLogged = false;
    auto_mode::ExposureController exposure{};
    auto_mode::FrameClock clock{};
    double frameSeconds = 0.0;
    float gpuMs = -1.0f;
    shared::CostLog cost{};
    // autofps.1: the Dynamic frame generation target the log last named (whole FPS, most frames; 0 none).
    long loggedTargetFps = 0;
    uint32_t loggedMaxFrames = 0;
};

// The settings a stage runs with: Hook Method Auto and Resolution Scale Auto
// resolved. autorect.1: with Resolution Scale Auto the feature is created at
// Auto's capacity, so `scale` stays put while Auto steps (Activate).
inline Settings Effective(SlotState& slot, const Settings& wanted, Stage stage, uint32_t width, uint32_t height) noexcept {
    Settings run = wanted; run.stage = static_cast<uint32_t>(stage);
    if (wanted.scaleAuto) {
        const size_t ceiling = auto_mode::CeilingLevel(wanted.scale);
        if (!slot.scalingSeeded) {
            slot.scaling.Reset(std::clamp(auto_mode::StartingLevel(width, height), ceiling, auto_mode::FloorLevel(ceiling)));
            slot.scalingSeeded = true;
        } else slot.scaling.Clamp(ceiling);
        run.scale = auto_mode::CapacityScale(wanted.scale);
    }
    return run;
}
// autorect.1: the network's rect for this evaluation: Auto's level within the
// processing extent, otherwise the whole extent. A rect that moved restarts the
// temporal history.
inline void Activate(FeatureCore& feature, SlotState& slot, const Settings& wanted, const Settings& run) noexcept {
    uint32_t width = feature.extentWidth, height = feature.extentHeight;
    float scale = run.scale;
    uint32_t levelWidth = 0, levelHeight = 0;
    if (wanted.scaleAuto && slot.scalingSeeded && slot.scaling.Scale() < run.scale
        && ProcessingExtent(feature.width, feature.height, slot.scaling.Scale(), levelWidth, levelHeight)) {
        width = std::min(levelWidth, width); height = std::min(levelHeight, height);
        scale = slot.scaling.Scale();
    }
    if (feature.activeWidth && (feature.activeWidth != width || feature.activeHeight != height)) slot.resetHistory = true;
    feature.activeWidth = width; feature.activeHeight = height; feature.activeScale = scale;
}
// The rect and scale a live feature reports (status, cost line): the latest
// evaluation's, or the processing extent before one.
inline uint32_t ShownWidth(const FeatureCore& feature) noexcept { return feature.activeWidth ? feature.activeWidth : feature.extentWidth; }
inline uint32_t ShownHeight(const FeatureCore& feature) noexcept { return feature.activeHeight ? feature.activeHeight : feature.extentHeight; }
inline float ShownScale(const FeatureCore& feature) noexcept { return feature.activeWidth ? feature.activeScale : feature.applied.scale; }
// Counts this evaluation into the real frame clock. A template on the stage's
// Slot type, so each stage keeps its own counter frequency under its own lock.
template <class SlotT> void Tick(SlotT& slot) noexcept {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    slot.frameSeconds += slot.clock.Tick(static_cast<uint64_t>(now.QuadPart), static_cast<uint64_t>(frequency.QuadPart));
}
// Linear reconstruction color is unbounded when the DLSS feature is HDR
// (Present: an FP16 or HDR10 swapchain); HDR10 and scRGB always are.
inline bool Unbounded(const SlotState& slot, const Settings& wanted) noexcept {
    return UnboundedColor(wanted, slot.automatic, slot.keys.createFlags);
}
// Adaptive exposure applies to unbounded color only: SDR keeps a unit gain,
// and the status reports the gain the shader receives.
inline float AdaptiveGain(const SlotState& slot, const Settings& wanted) noexcept {
    return wanted.adaptiveExposure && Unbounded(slot, wanted) && slot.exposure.valid ? slot.exposure.Gain() : 1.0f;
}
// autofps.1: Auto's budget for the stage, taking in Dynamic frame generation's
// target while one runs (auto_mode::DynamicBudgetMs).
inline float AutoBudgetMs(const SlotState& slot, const frame_gen_target::Target& target) noexcept {
    return auto_mode::DynamicBudgetMs(slot.clock.averageMs, slot.gpuMs, target.outputFps, target.minFrames, target.maxFrames);
}
inline float AutoBudgetMs(const SlotState& slot) noexcept { return AutoBudgetMs(slot, frame_gen_target::Current()); }
// Auto's Resolution Scale on the stage's new GPU time (Harvest), over the
// `seconds` since the previous feed. `stepped` and `held` are the log formats
// for a level change and for the quality floor held below the slider.
inline void StepScale(SlotState& slot, const Settings& wanted, double seconds, bool otherLevel, LogCallback logLine,
    const wchar_t* stepped, const wchar_t* held) noexcept {
    const float before = slot.scaling.Scale();
    const frame_gen_target::Target target = frame_gen_target::Current();
    const float budget = AutoBudgetMs(slot, target);
    const long targetFps = target.Active() ? std::lround(target.outputFps) : 0;
    const uint32_t maxFrames = target.Active() ? target.maxFrames : 0;
    if (wanted.scaleAuto && (targetFps != slot.loggedTargetFps || maxFrames != slot.loggedMaxFrames)) {
        wchar_t message[240]{};
        if (targetFps)
            swprintf_s(message, L"DLSS 5 Auto follows Dynamic frame generation's target: %ld FPS with %u to %u frames per rendered frame (budget %.2f ms)",
                targetFps, target.minFrames, maxFrames, budget);
        else swprintf_s(message, L"DLSS 5 Auto no longer follows a Dynamic frame generation target (budget %.2f ms)", budget);
        if (targetFps || slot.loggedTargetFps) logLine(message);
        slot.loggedTargetFps = targetFps; slot.loggedMaxFrames = maxFrames;
    }
    if (wanted.scaleAuto && slot.scalingSeeded && !otherLevel
        && slot.scaling.Update(slot.gpuMs, budget, seconds, auto_mode::CeilingLevel(wanted.scale))) {
        wchar_t message[200]{};
        swprintf_s(message, stepped, before, slot.scaling.Scale(), slot.gpuMs, budget);
        logLine(message); slot.floorLogged = false;
    }
    // Logged once per level.
    if (slot.scaling.floorHeld && !slot.floorLogged) {
        slot.floorLogged = true;
        wchar_t message[200]{};
        swprintf_s(message, held, slot.scaling.Scale(), slot.gpuMs, budget);
        logLine(message);
    }
}
// An nvngx_dlssnr.dll mapped before our first create (NVIDIA's NGX runtime, which can map a signed
// provider, or another DLSS 5 add-on): the file it is, the one beside the game, and our native creates so far.
inline void LogProviderLoaded(LogCallback logLine, const wchar_t* api, HMODULE loaded, const std::wstring& provider,
    uint64_t attempts) noexcept {
    wchar_t module[1024]{};
    const DWORD length = GetModuleFileNameW(loaded, module, 1024);
    if (!length || length >= 1024) wcscpy_s(module, L"-");
    wchar_t message[2400]{};
    swprintf_s(message, L"DLSS 5%ls provider already loaded before MFG Unlocker's first create: module=%ls provider=%.1000ls samePath=%d ownNativeCreates=%llu",
        api, module, provider.empty() ? L"-" : provider.c_str(), !provider.empty() && !_wcsicmp(module, provider.c_str()),
        static_cast<unsigned long long>(attempts));
    logLine(message);
}
// The direct provider's FileVersion and size, once per process (both stages). Identity diagnostics only:
// admission never reads them (dlss5_provider.cpp).
inline void LogDirectIdentity(LogCallback logLine, const wchar_t* api, HMODULE module, const std::wstring& path) noexcept {
    static std::atomic<bool> logged{false};
    if (!module || logged.exchange(true)) return;
    unsigned version[4]{};
    if (const HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16))) // VS_VERSION_INFO, RT_VERSION
        if (const HGLOBAL resource = LoadResource(module, info)) {
            const auto* data = static_cast<const BYTE*>(LockResource(resource));
            const DWORD size = SizeofResource(module, info);
            for (DWORD at = 0; data && at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
                const auto* fixed = reinterpret_cast<const VS_FIXEDFILEINFO*>(data + at);
                if (fixed->dwSignature != 0xFEEF04BD) continue;
                version[0] = HIWORD(fixed->dwFileVersionMS); version[1] = LOWORD(fixed->dwFileVersionMS);
                version[2] = HIWORD(fixed->dwFileVersionLS); version[3] = LOWORD(fixed->dwFileVersionLS);
                break;
            }
        }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const unsigned long long bytes = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)
        ? (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow : 0;
    wchar_t message[2048]{};
    swprintf_s(message, L"DLSS 5%ls direct provider identity (diagnostics only, not admission): FileVersion=%u.%u.%u.%u size=%llu path=%.1700ls",
        api, version[0], version[1], version[2], version[3], bytes, path.c_str());
    logLine(message);
}
}
