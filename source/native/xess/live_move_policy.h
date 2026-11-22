#pragma once
// takeover.1: a live move of a running game's D3D12 swap chain to XeFG or back (d3d12_host.h Move): which refusals
// pass by themselves, how often the move is tried again, and how long its waits are. Pure, so tests/xemfg_standalone
// covers it without a device.
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace xess_live_move
{
// A move at a Present is tried again at the game's next Presents for this long after its first refusal.
constexpr uint64_t kRetryWindowMs = 2000;
// A chain someone else holds costs the menu's session each try: this many tries in all, this far apart.
constexpr uint32_t kSharedTries = 3;
constexpr uint64_t kSharedGapMs = 250;
// A move in the game's own resize: the window is nudged this many times in all, this far apart.
constexpr uint32_t kResizeAsks = 3;
constexpr uint64_t kResizeGapMs = 300;
// XeFG's context is made on a worker before the old chain goes. The game's Presents (or the window's nudge) wait this
// long for it, and the move itself this long more; past that the game keeps its chain.
constexpr uint64_t kPrepareHoldMs = 3000;
constexpr uint64_t kPrepareJoinMs = 3000;
// When neither kind of chain could be created on the freed window, a native one is tried for this long more before
// the game is told its device is gone.
constexpr uint64_t kLastChainMs = 1000;

// Passes by itself: another call was inside the proxy, the GPU still read the proxy's back buffers, the menu was
// drawing or closing, or someone held a reference to the chain for a moment.
inline bool Transient(const char* reason) noexcept
{
    if (!reason) return false;
    for (const char* name : {"concurrent_call", "gpu_drain", "menu_busy", "menu_drawing", "chain_shared"})
        if (!std::strcmp(reason, name)) return true;
    return false;
}

struct Retry
{
    bool again = false;    // try once more; else the refusal is the move's result
    uint64_t waitMs = 0;   // not before this long from now
};

// A move at a Present was refused. `sinceFirstMs`: since this move's first refusal (0 at it). `sharedTries`: its
// refusals for a held chain so far, this one included.
inline Retry AtPresent(const char* reason, uint64_t sinceFirstMs, uint32_t sharedTries) noexcept
{
    if (!Transient(reason) || sinceFirstMs >= kRetryWindowMs) return {};
    if (!std::strcmp(reason, "chain_shared")) return sharedTries < kSharedTries ? Retry{true, kSharedGapMs} : Retry{};
    return {true, 0};
}

// A move in the game's resize was refused. `asks`: nudges of the window for this position of the switch so far.
inline Retry InResize(const char* reason, uint32_t asks) noexcept
{
    return Transient(reason) && asks < kResizeAsks ? Retry{true, kResizeGapMs} : Retry{};
}
}
