#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>

namespace reflex_control
{
enum class Status : uint32_t
{
    eFollowGame = 0,
    eWaitingForModule,
    eUnsupportedModule,
    eHookUnavailable,
    eIneligible,
    eWaitingForGameOptions,
    eWaitingForThread,
    eWaitingForAvailability,
    eUnavailable,
    eApplied,
    eRestorePending,
    eCallRejected,
    eUnknownShape,
    eStaleRoute,
    eBusy,
};

enum Hook : uint32_t
{
    eSetOptionsHook = 1u << 0,
    eSetDataHook = 1u << 1,
    eSleepHook = 1u << 2,
    eGetStateHook = 1u << 3,
    eAllHooks = eSetOptionsHook | eSetDataHook | eSleepHook | eGetStateHook,
};

struct Snapshot
{
    uint32_t requestedFrameLimitFps = 0;
    uint32_t requestedFrameLimitUs = 0;
    uint32_t appliedFrameLimitUs = 0;
    uint32_t gameFrameLimitUs = 0;
    bool appliedKnown = false;
    bool gameOptionsKnown = false;
    bool replaySafe = false;
    bool pending = false;
    bool restorePending = false;
    bool eligible = false;
    bool availabilityKnown = false;
    bool lowLatencyAvailable = false;
    bool cachedPointersCovered = false;
    uint32_t installedHookMask = 0;
    uint32_t currentHookMask = 0;
    uint32_t hookFailure = 0;
    uint32_t moduleVersionMajor = 0;
    uint32_t moduleVersionMinor = 0;
    uint32_t moduleVersionPatch = 0;
    uint32_t moduleVersionBuild = 0;
    uint64_t moduleGeneration = 0;
    // The last refused Reflex plug-in; refusedVersionRead is
    // false when it had no readable version resource.
    bool refusedModule = false;
    bool refusedVersionRead = false;
    std::array<uint32_t, 4> refusedVersion{};
    uint64_t acceptedOptionCalls = 0;
    uint64_t rejectedOptionCalls = 0;
    uint64_t replayCalls = 0;
    uint32_t lastSetResult = UINT32_MAX;
    uint32_t lastGetStateResult = UINT32_MAX;
    Status status = Status::eFollowGame;
};

// Zero follows the game's last accepted options; it never means force the
// limiter off. Invalid FPS values (>1000) leave the prior request unchanged.
// Changing eligibility also schedules restoration of an existing override.
// This function never invokes NVIDIA code.
bool Configure(uint32_t frameLimitFps, bool eligible) noexcept;

using RuntimeValidator = bool (*)() noexcept;
// Optional immediate ownership/FG-state check. Invoked outside our locks;
// the callback must be bounded and must not call NVIDIA APIs.
void SetRuntimeValidator(RuntimeValidator validator) noexcept;

enum class Observation : uint32_t
{
    eNone = 0,        // Not Reflex, already routed, or already refused/reported.
    eRouted,          // A new route was published and its entries installed.
    eRefusedVersion,  // Reflex entries resolved; the version is unsupported or unreadable.
    eHookUnavailable, // slReflexSetOptions resolved, another Reflex entry did not.
};

// Call outside DllMain/loader lock at normal discovery boundaries. Only an
// owned Reflex plugin with loaded version 2.7-2.14 (any patch) is eligible.
// Filenames are not feature identity; OTA and renamed plugins use the same
// module-owned resolver/entry checks as a game-local sl.reflex.dll.
// The resolver and installed code entries are pinned for process lifetime.
// A refusal is reported once per module generation; version receives the
// plug-in's file version when one was read (zeros otherwise).
Observation ObserveModule(HMODULE module, const wchar_t* path,
    uint64_t generation, std::array<uint32_t, 4>* version = nullptr) noexcept;

// Applied reports an accepted options call, not measured FPS or presentation.
Snapshot ReadSnapshot() noexcept;
const char* StatusName(Status status) noexcept;
}
