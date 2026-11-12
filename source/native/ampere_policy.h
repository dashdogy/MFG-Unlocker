#pragma once

#include <cstdint>
#include "ampere_wrapper_capacity.h"

namespace ampere_policy
{
constexpr uint32_t kPresetSetting = 0x10E41DF1u;
constexpr uint32_t kPresetB = 2;
constexpr uint32_t kMaximumGeneratedFrames = ampere_mfg::kMaximumGeneratedFrames;

// A wrapper layout's (and so the startup bootstrap's) capacity: legacy 2x or
// the six-buffer MFG program.
constexpr bool CapacityValid(uint32_t generated) noexcept
{
    return generated == 1 || generated == kMaximumGeneratedFrames;
}

// What a Create may certify. The wrapper's ring holds N+1 buffers
// for the N its startup cached, so any N up to the program's five is proven.
constexpr bool CertifiedCapacityValid(uint32_t generated) noexcept
{
    return generated >= 1 && generated <= kMaximumGeneratedFrames;
}

// An evaluation's generated-frame count fits the feature's certified ring.
// Count validity itself is Batch::Accepts' (and ReadEvaluationInput's) check.
constexpr bool EvaluationFitsCapacity(int count, uint32_t created) noexcept
{
    return CertifiedCapacityValid(created) && count <= static_cast<int>(created);
}

// The multiplier the certified ring carries (before NVIDIA's title ceiling):
// N+1 on an MFG startup, 2x on a legacy startup or without a certified ring.
constexpr uint32_t RingMaximumMultiplier(uint32_t startupMaximum, uint32_t certified) noexcept
{
    return startupMaximum == kMaximumGeneratedFrames && CertifiedCapacityValid(certified) ? certified + 1 : 2;
}

// rtx30ngx.1: the RTX 30/20 options and ring policy governs NVIDIA's DLSS-G provider running the translated
// program, whose ring a Create certifies. It does not govern an Nvngx replacement provider that owns
// FrameGeneration: one that offered its maximum while no FrameGeneration Create reached NVIDIA's provider
// (through the NGX hooks or the Ampere backend). The replacement allocates its own outputs, so no Ampere Create
// will ever certify a ring and the uncertified 2x would cap it for good.
// A Create on NVIDIA's provider hands the decision back to the certified ring.
constexpr bool ReplacementOwnsFrameGeneration(bool replacementOffered,
    uint64_t nvidiaFrameGenerationCreates, uint64_t ampereCreatedFeatures) noexcept
{
    return replacementOffered && nvidiaFrameGenerationCreates == 0 && ampereCreatedFeatures == 0;
}

// The safe maximum multiplier on RTX 30/20: a replacement's own maximum (its generated frames + 1, at most
// six), else the certified ring's (RingMaximumMultiplier), else 2x before the backend is ready.
constexpr uint32_t SafeMaximumMultiplier(bool replacementOwns, uint32_t replacementGeneratedFrames,
    bool backendReady, uint32_t startupMaximum, uint32_t certified) noexcept
{
    if (replacementOwns)
        return replacementGeneratedFrames < 1 ? 2
            : replacementGeneratedFrames > kMaximumGeneratedFrames ? kMaximumGeneratedFrames + 1
            : replacementGeneratedFrames + 1;
    return backendReady ? RingMaximumMultiplier(startupMaximum, certified) : 2;
}

constexpr bool EvaluationCountValid(int count) noexcept
{
    return count >= 1 && count <= static_cast<int>(kMaximumGeneratedFrames);
}

// One native render interval owns a fixed count of distinct submissions.
// Counts may change only between complete intervals, within the same proven
// allocation. A failed interval can restart only at index 1 of a newer frame.
// This records successful submissions, not completed GPU work or presentation.
// flipMetered: the wrapper is in flip-metering presentation (observed through
// SetFlipConfig, ampere_flip_metering.h), where each generated frame is
// presented before the next index writes the shared output, so an output may
// repeat within the interval. Order, count and frame rules are unchanged.
struct Batch
{
    uint64_t frame = 0;
    std::array<uintptr_t, kMaximumGeneratedFrames> outputs{};
    uint32_t generated = 0;
    uint32_t submitted = 0;
    bool started = false;
    bool failed = false;

    bool Accepts(uint64_t value, int count, int index, uintptr_t output, bool flipMetered = false) const noexcept
    {
        if (!output || !EvaluationCountValid(count) || index < 1 || index > count) return false;
        if (!started) return index == 1;
        if (failed || submitted == generated)
            return index == 1 && value > frame;
        if (value != frame || count != generated || index != submitted + 1) return false;
        return flipMetered || !RepeatsOutput(output);
    }
    // The output is one this in-progress interval already submitted.
    bool RepeatsOutput(uintptr_t output) const noexcept
    {
        if (!started || failed || submitted >= generated) return false;
        for (uint32_t i = 0; i < submitted; ++i)
            if (output == outputs[i]) return true;
        return false;
    }
    // No interval is in progress: the next accepted call starts one at index 1.
    bool BetweenIntervals() const noexcept { return !started || failed || submitted == generated; }
    // The next call of the current interval, refused only because its output is
    // one this interval already submitted (a wrapper sharing one surface across
    // the batch, e.g. flip-metering presentation's two-surface pool).
    bool DuplicateOutput(uint64_t value, int count, int index, uintptr_t output) const noexcept
    {
        return output && value == frame && count == static_cast<int>(generated)
            && index == static_cast<int>(submitted) + 1 && RepeatsOutput(output);
    }
    void Fail(uint64_t value, int count, int index) noexcept
    {
        // Retain the failed interval even when its first native call failed.
        // A retry of that frame, a stale frame, or its remaining indices must
        // never be forwarded. No feature/module/publication proof is reset.
        if ((!started || (index == 1 && value > frame)) && EvaluationCountValid(count))
        {
            frame = value; generated = static_cast<uint32_t>(count);
            outputs = {}; submitted = 0; started = true;
        }
        failed = true;
    }
    void Complete(uint64_t value, int count, int index, uintptr_t output, bool succeeded,
        bool flipMetered = false) noexcept
    {
        if (!succeeded || !Accepts(value, count, index, output, flipMetered)) { Fail(value, count, index); return; }
        if (index == 1) { frame = value; generated = static_cast<uint32_t>(count); outputs = {}; started = true; failed = false; }
        outputs[static_cast<size_t>(index - 1)] = output;
        submitted = static_cast<uint32_t>(index);
    }
};

constexpr bool AdapterMatches(uint64_t d3dLuid, uint64_t cudaLuid,
    unsigned matches, int major, int minor, unsigned nodeMask) noexcept
{
    return d3dLuid != 0 && d3dLuid == cudaLuid && matches == 1
        && ((major == 8 && minor == 6) || (major == 7 && minor == 5)) && nodeMask != 0
        && (nodeMask & (nodeMask - 1)) == 0;
}

constexpr bool ControlValid(unsigned multiplier, bool dynamic,
    bool experimental, bool generatedOnly) noexcept
{
    return multiplier >= 1 && multiplier <= kMaximumGeneratedFrames + 1
        && (!dynamic || multiplier != 1) && !experimental && !generatedOnly;
}

// The DLSS-G options RTXMFG submits on RTX 30. Follow game keeps the game's
// own mode, frame count and Dynamic target; the count is limited to the safe
// maximum, and Dynamic needs the full-capacity Dynamic proof (otherwise On).
// A fixed or Dynamic menu choice replaces the game's request.
enum class OptionsMode : uint8_t { eOff, eOn, eDynamic };

struct OptionsDecision
{
    OptionsMode mode = OptionsMode::eOff;
    uint32_t generatedFrames = 1;
    bool gameDynamicTarget = false;
};

constexpr OptionsDecision DecideOptions(bool followGame, uint32_t controlMultiplier,
    bool controlDynamic, OptionsMode gameMode, uint32_t gameGeneratedFrames,
    uint32_t safeMaximumMultiplier, bool dynamicSupported) noexcept
{
    const uint32_t limit = safeMaximumMultiplier > 2 ? safeMaximumMultiplier - 1 : 1;
    OptionsDecision decision{};
    if (followGame)
    {
        decision.generatedFrames = gameGeneratedFrames < 1 ? 1
            : gameGeneratedFrames > limit ? limit : gameGeneratedFrames;
        decision.gameDynamicTarget = gameMode == OptionsMode::eDynamic && dynamicSupported;
        decision.mode = gameMode == OptionsMode::eOff ? OptionsMode::eOff
            : decision.gameDynamicTarget ? OptionsMode::eDynamic : OptionsMode::eOn;
        return decision;
    }
    const uint32_t multiplier = controlMultiplier < 1 ? 1
        : controlMultiplier > safeMaximumMultiplier ? safeMaximumMultiplier : controlMultiplier;
    decision.generatedFrames = multiplier > 2 ? multiplier - 1 : 1;
    decision.mode = controlMultiplier == 1 || gameMode == OptionsMode::eOff ? OptionsMode::eOff
        : controlDynamic && dynamicSupported ? OptionsMode::eDynamic : OptionsMode::eOn;
    return decision;
}

// After startup admission failed (or the boundary turned fatal),
// every FrameGeneration Create is refused before the provider, and Streamline
// retries a refused Create on every frame. Off stops the retries; the frame
// count is kept so the capacity checks read the same request.
constexpr OptionsDecision HoldOffWithoutAdmission(OptionsDecision decision, bool admissionUnavailable) noexcept
{
    if (admissionUnavailable)
    {
        decision.mode = OptionsMode::eOff;
        decision.gameDynamicTarget = false;
    }
    return decision;
}

// The DLSS-G maximum the game sees (numFramesToGenerateMax). A full-capacity
// wrapper offers its five generated frames only up to NVIDIA's per-title
// multiplier ceiling, so a 4X title is not offered 5x/6x that the options
// path would then clamp. A legacy 2x wrapper always reports one frame.
// Once a Create has certified a smaller ring (ringMaximum, 0 = none yet), the
// game is offered no more than that ring holds.
constexpr uint32_t AdvertisedGeneratedFrames(uint32_t startupMaximum,
    uint32_t policyMaximumMultiplier, uint32_t ringMaximum = 0) noexcept
{
    if (startupMaximum != kMaximumGeneratedFrames) return 1;
    const uint32_t limit = policyMaximumMultiplier > 2 ? policyMaximumMultiplier - 1 : 1;
    const uint32_t offered = limit < kMaximumGeneratedFrames ? limit : kMaximumGeneratedFrames;
    return CertifiedCapacityValid(ringMaximum) && ringMaximum < offered ? ringMaximum : offered;
}

// Public NGX handles are opaque. No EndpointCore offsets, inferred allocation
// size, or unobserved release are accepted as a substitute for this identity.
struct Feature
{
    uintptr_t handle = 0;
    uintptr_t owner = 0;
    uint64_t generation = 0;
    uint64_t publication = 0;
    uint64_t luid = 0;
    bool evaluated = false;
    uintptr_t provider = 0;
    uint64_t providerGeneration = 0;
    uintptr_t providerHandle = 0;
    uint32_t createdGeneratedFrames = 0;
    uintptr_t device = 0; // Identity only; NGX owns the device lifetime.
    uint64_t outputWidth = 0;
    uint32_t outputHeight = 0, outputFormat = 0;
    Batch batch{};
    uintptr_t wrapper = 0;
    uint64_t wrapperGeneration = 0;
    ampere_wrapper::Capacity capacity{};
    // Distinguishes successive native feature lifetimes when NGX reuses the
    // same opaque handle and all module/publication identities are unchanged.
    // Kept after the original aggregate fields so policy fixtures remain exact.
    uint64_t lifetime = 0;
    uint64_t createAttemptToken = 0;
    uint64_t evaluationSerial = 0;
    bool rejectionReported = false;
    bool submissionFailureReported = false;
    bool recoveryPending = false;
    // The wrapper shared one output across a batch: this feature stays at 2x
    // (certified one generated frame) until it is released and recreated.
    bool outputReuseLatched = false;
    // Flip-metering path: repeats refused while SetFlipConfig did not report
    // metering (the mode switch costs at most one batch). Past the allowance the
    // feature latches at 2x as without the flip-metering path.
    uint32_t unmeteredRepeats = 0;
    // NGX entry family that created the feature (ampere_backend::GraphicsApi):
    // device/outputFormat then hold Vulkan identities (VkDevice, VkFormat).
    uint32_t api = 0;
    static constexpr uint32_t kUnmeteredRepeatAllowance = 4;
    bool LatchRepeatedOutput(bool observingFlipMetering) noexcept
    {
        return !observingFlipMetering || ++unmeteredRepeats > kUnmeteredRepeatAllowance;
    }

    bool Matches(uintptr_t value, uintptr_t module, uint64_t load,
        uint64_t program, uint64_t adapter) const noexcept
    {
        return handle && value == handle && owner == module && generation == load
            && publication == program && luid == adapter;
    }
    bool Release(bool succeeded) noexcept
    {
        if (!handle || !succeeded) return false;
        *this = {};
        return true;
    }
};
}
