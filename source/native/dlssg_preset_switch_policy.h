#pragma once

#include <cstdint>

// Live DLSS-G preset switching. A live frame-generation feature keeps
// a copy of the process-wide configuration. The singleton must be reset between
// endpoint calls before a re-create can apply a new preset,
// so a new selection is latched only while no FG Create is in flight and no FG
// feature lives. While the game runs frame generation that takes one forced
// re-create: DLSS-G Off without eRetainResourcesWhenOff, the exact release of
// every tracked feature, the latch, then the game's own options again.
namespace dlssg_preset_switch
{
enum class Phase : uint8_t { eIdle, eDraining };

enum class Action : uint8_t
{
    eNone,
    // No FG feature and no Create in flight: latch now; the next Create reads it.
    eRelatch,
    // Submit Off without resource retention.
    eBeginDrain,
    // Off stays submitted until every tracked feature is released.
    eKeepDraining,
    // Released: latch, then submit the game's options again.
    eRelatchAndResume,
    // Released, but the selection went back to the latched value: submit the game's options again.
    eResume,
    // Not released in time: submit the game's options again; a restart applies the selection.
    eAbort,
};

struct Inputs
{
    Phase phase = Phase::eIdle;
    // The frozen selection differs from the saved request.
    bool pending = false;
    // Provider, graphics API, route and lifetime tracker allow a live switch.
    bool supported = false;
    // No FG Create in flight and no tracked live FG feature.
    bool quiescent = false;
    // The game's own request on the frame-generation viewport.
    bool gameWantsOn = false;
    // The game's last enabled options on that viewport are captured.
    bool optionsKnown = false;
    // The control route can submit adjusted options (patcher BridgeReady). An
    // RTX 30 exact release resets it until the next Create.
    bool bridgeReady = false;
    // Time since the drain began.
    uint64_t elapsedMs = 0;
};

// Streamline frees the feature on its next frame after Off; this bounds a game
// that stops presenting or keeps the feature alive anyway.
inline constexpr uint64_t kDrainTimeoutMs = 4000;

constexpr Action Decide(const Inputs& in) noexcept
{
    if (in.phase == Phase::eDraining)
    {
        if (in.quiescent)
            return !in.pending ? Action::eResume : !in.supported ? Action::eAbort : Action::eRelatchAndResume;
        // A live feature without a ready control route cannot be driven to its release.
        if (!in.bridgeReady || in.elapsedMs >= kDrainTimeoutMs) return Action::eAbort;
        return Action::eKeepDraining;
    }
    if (!in.pending || !in.supported) return Action::eNone;
    if (in.quiescent) return Action::eRelatch;
    if (in.gameWantsOn && in.optionsKnown && in.bridgeReady) return Action::eBeginDrain;
    // The game has turned frame generation off but kept its feature: its next On drains it.
    return Action::eNone;
}

// Status code: why the selection cannot switch live (0 = it can).
enum class Unavailable : uint32_t
{
    eNone = 0,
    eOptiScaler = 1,
    eProvider = 2,
    eLegacyPreset = 3,
    eGraphicsApi = 4,
    eTracker = 5,
    eRestartRequired = 6,
    eFailed = 7,
    eGpu = 8,
};
}
