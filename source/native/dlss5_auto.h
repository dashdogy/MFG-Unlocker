#pragma once

#include "dlss5_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// DLSS 5 Auto: placement, resolution scale and exposure chosen for
// the game at hand from what the stage observes, then adapted while it runs.
// Pure policy; dlss5.cpp supplies the measurements (GPU time of the stage,
// log-average luminance of its color, the real frame interval).
namespace dlss5::auto_mode
{
// Hook Method Auto: after upscaling, where the provider sees the reconstructed,
// temporally stable image, with Resolution Scale managing the cost. Before
// upscaling processes jittered render-resolution input and is never chosen.
// Present runs on the NGX instance of the game's DLSS evaluations too, so it
// offers nothing to a game without DLSS; it stays a choice of its own.
constexpr Stage AutoStage() noexcept { return Stage::eUpscaled; }

// The Resolution Scale levels Auto moves between, highest first.
inline constexpr std::array<float, 6> kLevels{100.0f, 85.0f, 75.0f, 66.0f, 50.0f, 33.0f};

// Auto's quality floor: never below 75 % (in a 1080p A/B, 50 % kept far less of DLSS 5's change and fine detail).
// Over budget at the floor, Auto stays there: the frame rate pays, not the detail.
inline constexpr size_t kFloorLevel = 2;
static_assert(kLevels[kFloorLevel] == 75.0f);

// The starting level before anything is measured: native up to 1440p-class
// extents, the floor (75 %) beyond.
inline size_t StartingLevel(uint32_t width, uint32_t height) noexcept
{
    const uint64_t pixels = uint64_t(width) * height;
    return pixels <= 2560ull * 1600 ? 0 : kFloorLevel;
}

// Auto never goes above the user's Resolution Scale: the highest level at or
// below it (the lowest level when the slider is below every level).
inline size_t CeilingLevel(float userScale) noexcept
{
    for (size_t level = 0; level < kLevels.size(); ++level)
        if (kLevels[level] <= userScale + 0.01f) return level;
    return kLevels.size() - 1;
}

// The lowest level Auto goes to: the quality floor, or the slider's own level
// when the slider is below the floor (the user's choice wins).
inline size_t FloorLevel(size_t ceiling) noexcept
{
    return std::max(kFloorLevel, std::min(ceiling, kLevels.size() - 1));
}

// autorect.1: the Resolution Scale of the feature Auto runs in. The feature is created once at the highest level
// Auto may use (the slider's, CeilingLevel), and a step changes only the network's rect inside it: DLSSNR keeps its
// create size as the capacity, and an Output subrect within it only resets its history (310.9.16). A step so costs
// one frame of fresh history instead of a create on the game's render thread and a second live feature.
inline float CapacityScale(float userScale) noexcept
{
    return kLevels[CeilingLevel(userScale)];
}

// autorect.1: Boost's look model textures in a feature Auto runs in, sized for every level from `ceiling` down to
// FloorLevel(ceiling) at this stage extent: the most cells across those levels (cellsX, cellsY and bins; `cell` is
// the ceiling level's). A lower level can need more cells where the cell size steps down (at 3200x1800, 100 % has
// 100x57 32-texel cells, 85 % 170x96 16-texel cells).
inline bool BoostGridCapacity(uint32_t width, uint32_t height, size_t ceiling, BoostGrid& grid) noexcept
{
    ceiling = std::min(ceiling, kLevels.size() - 1);
    for (size_t level = ceiling; level <= FloorLevel(ceiling); ++level)
    {
        uint32_t levelWidth = 0, levelHeight = 0;
        BoostGrid at{};
        if (!ProcessingExtent(width, height, kLevels[level], levelWidth, levelHeight) || !BoostGridFor(levelWidth, levelHeight, at))
            return false;
        if (level == ceiling) grid = at;
        else
        {
            grid.cellsX = std::max(grid.cellsX, at.cellsX);
            grid.cellsY = std::max(grid.cellsY, at.cellsY);
        }
    }
    return true;
}

// The stage's GPU budget: DLSS 5 may take kBudgetShare of each real (rendered)
// frame; NR runs once per rendered frame, generated frames do not run it. The
// frame interval includes the stage itself, so the budget comes from the rest
// of the frame (the interval minus the stage's own GPU time, stageMs). That
// rest does not move with the Resolution Scale, so a lower scale (a faster
// stage, a higher frame rate) does not shrink the budget it is held to.
// Without a stage time the interval is taken as it is (kBudgetShare of it).
inline constexpr float kBudgetShare = 0.30f, kMinBudgetMs = 2.0f, kMaxBudgetMs = 12.0f;

inline float BudgetMs(float realFrameMs, float stageMs = -1.0f) noexcept
{
    if (!std::isfinite(realFrameMs) || realFrameMs <= 0.0f) return kMaxBudgetMs;
    const float rest = std::isfinite(stageMs) && stageMs >= 0.0f ? std::max(realFrameMs - stageMs, 0.0f)
        : realFrameMs * (1.0f - kBudgetShare);
    return std::clamp(rest * kBudgetShare / (1.0f - kBudgetShare), kMinBudgetMs, kMaxBudgetMs);
}

// autofps.1: the stage's budget while Dynamic frame generation aims at `targetFps` presented frames a second with
// `minFrames` to `maxFrames` presented per real (rendered) frame (frame_gen_target.h). The real frame without the
// stage (`rest`) needs `needed` frames to reach the target: rest over the presented interval, rounded up, from
// minFrames to maxFrames. DLSS 5 may:
//  - never take more than keeps the target within maxFrames: maxFrames intervals less the rest. A frame that
//    misses the target already gets 0, so Auto holds its floor and the target is missed by as little as it can be;
//  - always take what is left before Dynamic would need one more frame: needed intervals less the rest;
//  - otherwise take what BudgetMs gives, within the first.
// Without a target, or before the frame and the stage were measured: BudgetMs.
inline float DynamicBudgetMs(float realFrameMs, float stageMs, float targetFps, uint32_t minFrames,
    uint32_t maxFrames) noexcept
{
    const float base = BudgetMs(realFrameMs, stageMs);
    if (!std::isfinite(targetFps) || targetFps <= 0.0f || !maxFrames || !std::isfinite(realFrameMs) || realFrameMs <= 0.0f
        || !std::isfinite(stageMs) || stageMs < 0.0f) return base;
    const double interval = 1000.0 / double(targetFps), rest = std::max(double(realFrameMs) - double(stageMs), 0.0);
    const double reach = double(maxFrames) * interval - rest;
    if (!(reach > 0.0)) return 0.0f;
    const double fewest = double(std::clamp<uint32_t>(minFrames, 1, maxFrames));
    const double needed = std::clamp(std::ceil(rest / interval - 1e-6), fewest, double(maxFrames));
    const double slack = needed * interval - rest;
    return static_cast<float>(std::min(std::max(double(base), slack), reach));
}

// Adaptive Resolution Scale. A level change moves the network's rect within
// the feature (CapacityScale) and restarts its temporal history, so changes
// are rare: down after a sustained overload, never below FloorLevel, up only
// when the next level's predicted cost fits comfortably, and never within
// kMinStepSeconds of the previous change.
// nrscale.1: a step up undone within kFailedUpSeconds failed, and the next one waits upWait: kFirstUpWaitSeconds,
// doubling to kMaxUpWaitSeconds; one that held kFailedUpSeconds clears the wait. A level over the game's video memory
// budget can cost several times its prediction, and Auto would otherwise oscillate every 10-25 s.
struct ScaleController
{
    static constexpr double kMinStepSeconds = 8.0, kDownAfterSeconds = 1.5, kUpAfterSeconds = 6.0;
    static constexpr double kFailedUpSeconds = 30.0, kFirstUpWaitSeconds = 30.0, kMaxUpWaitSeconds = 600.0;
    static constexpr float kDownAbove = 1.15f, kUpBelow = 0.85f, kSmoothing = 0.1f;
    static constexpr uint32_t kMinSamples = 10;

    size_t level = 0;
    float averageMs = 0.0f;
    uint32_t samples = 0;
    double sinceStep = 0.0, overload = 0.0, headroom = 0.0, upWait = 0.0;
    // At the quality floor (below the slider's level) and over budget for
    // kDownAfterSeconds: the menu says the frame rate pays (FloorHeld).
    bool floorHeld = false;
    // The last change was a step up (not the slider's ceiling moving).
    bool steppedUp = false;

    void Reset(size_t start) noexcept
    {
        *this = {};
        level = std::min(start, kLevels.size() - 1);
    }

    float Scale() const noexcept { return kLevels[level]; }

    // The level within [ceiling, FloorLevel(ceiling)]. True when it moved.
    bool Clamp(size_t ceiling) noexcept
    {
        ceiling = std::min(ceiling, kLevels.size() - 1);
        const size_t lowest = FloorLevel(ceiling);
        if (level < ceiling) { Step(ceiling); return true; }
        if (level > lowest) { Step(lowest); return true; }
        return false;
    }

    // One completed measurement of the stage at the current level. ceiling:
    // CeilingLevel of the user's slider. True when `level` changed.
    bool Update(float gpuMs, float budgetMs, double dtSeconds, size_t ceiling) noexcept
    {
        if (Clamp(ceiling)) return true;
        ceiling = std::min(ceiling, kLevels.size() - 1);
        if (!std::isfinite(gpuMs) || gpuMs < 0.0f || !std::isfinite(dtSeconds) || dtSeconds < 0.0) return false;
        dtSeconds = std::min(dtSeconds, 1.0);
        averageMs = samples ? averageMs + kSmoothing * (gpuMs - averageMs) : gpuMs;
        ++samples;
        sinceStep += dtSeconds;
        if (steppedUp && sinceStep >= kFailedUpSeconds) { steppedUp = false; upWait = 0.0; }
        bool fitsHigher = false;
        if (level > ceiling)
        {
            const float ratio = kLevels[level - 1] / kLevels[level];
            fitsHigher = averageMs * ratio * ratio < budgetMs * kUpBelow;
        }
        if (averageMs > budgetMs * kDownAbove) { overload += dtSeconds; headroom = 0.0; }
        else if (fitsHigher) { headroom += dtSeconds; overload = 0.0; }
        else { overload = 0.0; headroom = 0.0; }
        const bool atFloor = level >= FloorLevel(ceiling);
        floorHeld = atFloor && level > ceiling && overload >= kDownAfterSeconds && samples >= kMinSamples;
        if (sinceStep < kMinStepSeconds || samples < kMinSamples) return false;
        if (overload >= kDownAfterSeconds && !atFloor)
        {
            if (steppedUp) upWait = upWait > 0.0 ? std::min(upWait * 2.0, kMaxUpWaitSeconds) : kFirstUpWaitSeconds;
            Step(level + 1);
            return true;
        }
        if (headroom >= kUpAfterSeconds && level > ceiling && sinceStep >= upWait)
        {
            Step(level - 1);
            steppedUp = true;
            return true;
        }
        return false;
    }

private:
    void Step(size_t next) noexcept
    {
        level = next;
        averageMs = 0.0f;
        samples = 0;
        sinceStep = overload = headroom = 0.0;
        floorHeld = steppedUp = false;
    }
};

// Adaptive exposure: scales the stage color the network sees so its
// log-average luminance sits at middle grey, smoothed over time. The decode
// pass divides by the same factor, so the game's brightness is unchanged.
// Only for unbounded (HDR) color; display-referred SDR color is already exposed.
struct ExposureController
{
    static constexpr float kTarget = 0.18f, kMinGain = 0.125f, kMaxGain = 8.0f;
    static constexpr double kSeconds = 0.8;

    float logGain = 0.0f;
    bool valid = false;

    void Reset() noexcept { *this = {}; }

    float Gain() const noexcept { return std::exp2(logGain); }

    // logAverage: mean log2 luminance of the exposed stage color measured
    // without this controller's gain; aim: the luminance it is brought to
    // (Adaptive exposure target).
    void Update(float logAverage, double dtSeconds, float aim = kTarget) noexcept
    {
        if (!std::isfinite(logAverage) || !std::isfinite(dtSeconds) || dtSeconds < 0.0) return;
        if (!(aim > 0.0f) || !std::isfinite(aim)) aim = kTarget;
        const float target = std::clamp(std::log2(aim) - logAverage,
            std::log2(kMinGain), std::log2(kMaxGain));
        if (!valid) { logGain = target; valid = true; return; }
        const double blend = 1.0 - std::exp(-std::min(dtSeconds, 1.0) / kSeconds);
        logGain += static_cast<float>(blend) * (target - logGain);
    }
};

// The real frame interval from successive stage evaluations, smoothed.
struct FrameClock
{
    float averageMs = 0.0f;
    uint64_t last = 0;

    // now and frequency: QueryPerformanceCounter units. Returns the interval
    // since the previous call in seconds (0 for the first call).
    double Tick(uint64_t now, uint64_t frequency) noexcept
    {
        if (!frequency || !last || now <= last) { last = now; return 0.0; }
        const double seconds = double(now - last) / double(frequency);
        last = now;
        // A pause (menu, loading, alt-tab) is not a frame interval.
        if (seconds > 0.25) return 0.0;
        const float ms = static_cast<float>(seconds * 1000.0);
        averageMs = averageMs > 0.0f ? averageMs + 0.05f * (ms - averageMs) : ms;
        return seconds;
    }
};
}
