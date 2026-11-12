#pragma once

#include <cstdint>

// Streamline's command-list state tracking at slInit (GitHub issue 137). DLSS 5's Render and Upscaled stages record
// on the game's command list inside its DLSS evaluation and bind our descriptor heap, compute root signature and
// pipeline there; D3D12 cannot read the game's back. Streamline's list proxies record that state and slEvaluateFeature
// re-binds it after every evaluation (restorePipeline, Streamline 2.0 to 2.14), but only for a host without manual
// hooking whose proxies track it. Hosts that set eDisableCLStateTracking (the SDK default) promise to re-bind it
// themselves; RE Engine (Pragmata) does not, and its next SetComputeRootDescriptorTable ran against our root
// signature. While DLSS 5 can run, tracking is turned back on for such hosts.
namespace streamline_state_policy
{
constexpr uint64_t kDisableClStateTracking = 1ull << 0;
constexpr uint64_t kUseManualHooking = 1ull << 2;
enum class Reason : uint32_t { eNoDlss5, eHostVersion, eManualHooking, eHostTracks, eRestored };
constexpr const char* ReasonName(Reason reason) noexcept
{
    switch (reason)
    {
    case Reason::eNoDlss5: return "no-dlss5";
    case Reason::eHostVersion: return "host-version";
    case Reason::eManualHooking: return "manual-hooking";
    case Reason::eHostTracks: return "host-tracks";
    case Reason::eRestored: return "restored";
    default: return "unknown";
    }
}

struct Result
{
    uint64_t flags = 0;
    Reason reason = Reason::eNoDlss5;
};

// dlss5Possible: DLSS 5 is on in the saved settings or its provider sits beside the game or MFG Unlocker (the menu
// offers it then). hostMajor: the interposer's major version (0 when unknown).
constexpr Result Apply(uint64_t flags, bool dlss5Possible, uint32_t hostMajor) noexcept
{
    const Reason reason = !dlss5Possible ? Reason::eNoDlss5 : hostMajor != 2 ? Reason::eHostVersion
        : (flags & kUseManualHooking) ? Reason::eManualHooking
        : !(flags & kDisableClStateTracking) ? Reason::eHostTracks : Reason::eRestored;
    return { reason == Reason::eRestored ? flags & ~kDisableClStateTracking : flags, reason };
}
}
