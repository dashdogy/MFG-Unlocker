#pragma once
// slprov.1: the API-free rules of XeMFG behind the game's own Streamline (xess/streamline_provider.cpp). Header only, so
// tests/xemfg_standalone checks them on the CPU.
#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_streamline_provider
{
// NVSDK_NGX_Handle ids of the features created here, far from the small ids NGX hands out.
inline constexpr uint32_t kFirstHandleId = 0x7E000000u;
// Live FrameGeneration features: one per viewport Streamline drives. A Create past this goes to NVIDIA's provider.
inline constexpr size_t kMaxFeatures = 8;

// Whether a FrameGeneration Create can be offered to XeFG: the key is on (and OptiScaler absent, which hosts its own
// provider route), the call is on the NGX runtime's entry, and the GPU takes a branch XeFG can stand in on
// (GeneratorRoute: RTX 40/50's native branch, or RTX 30/20's translated program once TranslatedRoute holds).
constexpr bool Offer(bool wanted, bool runtimeRoute, bool generatorRoute) noexcept
{
    return wanted && runtimeRoute && generatorRoute;
}

// genswap30.1: the generated frames the game's Streamline must hold on RTX 30/20 for XeFG to stand in there (6x). A
// wrapper whose ring is smaller keeps XeMFG standalone on the takeover for the session, so a higher XeMFG frame count
// in the menu never outgrows the route while the game runs.
inline constexpr uint32_t kTranslatedRing = 5;

// genswap30.1: RTX 30/20, whose DLSS-G runs on the translated program (ampere_backend.cpp). XeFG stands in for that
// program's feature only once its startup admission is complete (ampere_backend::Ready: device, adapter, runtime and
// provider entries, program), so switching back always finds NVIDIA's provider prepared; and only while the wrapper's
// ring holds kTranslatedRing: its startup maximum, and the ring a translated Create certified (0: none yet).
struct TranslatedProgram
{
    bool ready = false;
    uint32_t startupMaximum = 0;
    uint32_t certifiedRing = 0;
};

constexpr bool RingHolds(uint32_t generatedFrames) noexcept { return generatedFrames >= kTranslatedRing; }

constexpr bool TranslatedRoute(const TranslatedProgram& in) noexcept
{
    return in.ready && RingHolds(in.startupMaximum) && (!in.certifiedRing || RingHolds(in.certifiedRing));
}

constexpr bool GeneratorRoute(bool nativeProgram, bool translatedRoute) noexcept
{
    return nativeProgram || translatedRoute;
}

// genswap.1: the control key xemfgStreamline. Auto and on are the same: the route runs wherever it can; off keeps XeMFG
// standalone on its own swap chain everywhere (the way back for a game the route does not suit).
enum class Key : uint32_t { eAuto = 0, eOn = 1, eOff = 2 };

// genswap.1: where XeMFG standalone runs on D3D12 while its switch is on. Behind the game's Streamline, XeFG is the
// game's DLSS-G feature and Streamline keeps the swap chain, so the switch never replaces the chain; otherwise the host
// takes the chain over (xess/d3d12_host.h), as before this route existed.
struct RouteInputs
{
    Key key = Key::eAuto;
    bool optiScaler = false;      // OptiScaler hosts XeFG itself
    bool refused = false;         // XeFG refused a feature here once (final for the process)
    bool nativeProgram = false;   // RTX 40/50: the NGX runtime route's native branch
    bool translatedRoute = false; // genswap30.1: RTX 30/20 whose translated program TranslatedRoute admits
    bool routeLoaded = false;     // a Streamline DLSS-G plug-in (sl.dlss_g) is loaded: the game may drive DLSS-G
    bool dlssgDriven = false;     // the game has set DLSS-G options at least once
    // Upscaler evaluations seen while the game never set DLSS-G options: a game that renders that many frames without
    // touching DLSS-G does not drive it, so only the takeover can run XeMFG there (from the upscaler's inputs).
    uint64_t undrivenEvaluations = 0;
    bool hostOwnsChain = false;   // the takeover already has the game's chain: it keeps it until switched off
    bool dlssgRetired = false;    // xemfglive.5: the game's DLSS-G is held Off until a restart
};

inline constexpr uint64_t kUndrivenEvaluations = 300;

constexpr bool BehindStreamline(const RouteInputs& in) noexcept
{
    if (in.key == Key::eOff || in.optiScaler || in.refused || !in.routeLoaded) return false;
    if (!GeneratorRoute(in.nativeProgram, in.translatedRoute)) return false;
    if (in.hostOwnsChain || in.dlssgRetired) return false;
    return in.dlssgDriven || in.undrivenEvaluations < kUndrivenEvaluations;
}

// genswap.1: the live switch behind Streamline. A frame-generation feature belongs to the generator that created it
// (NVIDIA's provider or XeFG) until Streamline releases it, so switching means one re-create: the game's DLSS-G gets Off
// without eRetainResourcesWhenOff until every feature of the wrong generator is released (the exact release the 6x
// unlock needs before any re-creation), then the game's own options again, whose Create goes to the wanted generator.
// The game keeps presenting throughout: only the generated frames stop for the time of the release.
struct SwapInputs
{
    bool wantedXefg = false;       // the generator the next feature should belong to
    bool gameWantsOn = false;      // the game's own DLSS-G request
    uint32_t liveXefg = 0;         // features XeFG holds
    uint32_t liveTotal = 0;        // every tracked frame-generation feature
    bool createsInFlight = false;
    bool exact = true;             // the lifetime tracker can prove a release
    bool holding = false;          // Off is being submitted for this swap
    bool attempted = false;        // a swap toward this generator already ran
    uint64_t heldMs = 0;           // since the hold began
    // Since XeFG's last feature was released (UINT64_MAX: never). Its context lives on for the provider's retire time.
    uint64_t sinceXefgReleaseMs = UINT64_MAX;
};

enum class SwapAction : uint8_t
{
    eNone,
    eBegin,         // start submitting Off without resource retention
    eHold,          // keep submitting it
    eDone,          // released: the game's options go through again
    eGiveUp,        // not released in time: the game's options go through, the old generator stays
    ePause,         // the game turned frame generation off itself: its next On starts again
    eWait,          // XeFG's last context is still being retired: the old generator keeps running a moment longer
    eUnavailable,   // a release cannot be proven: the switch applies when the game re-creates its feature
};

// Streamline frees the feature on its next frame after Off; this bounds a game that stops presenting or keeps it.
inline constexpr uint64_t kSwapTimeoutMs = 4000;
// The D3D12 provider keeps a released context for 2 s (d3d12_provider.h): a new XeFG feature waits that out, so quick
// toggles never hold two XeFG contexts in video memory.
inline constexpr uint64_t kSwapSpacingMs = 2000;

constexpr bool WrongGenerator(const SwapInputs& in) noexcept
{
    return in.wantedXefg ? in.liveTotal > in.liveXefg : in.liveXefg > 0;
}

constexpr SwapAction DecideSwap(const SwapInputs& in) noexcept
{
    const bool wrong = WrongGenerator(in);
    if (in.holding) {
        if (!wrong && !in.createsInFlight) return SwapAction::eDone;
        if (!in.gameWantsOn) return SwapAction::ePause;
        return in.heldMs >= kSwapTimeoutMs ? SwapAction::eGiveUp : SwapAction::eHold;
    }
    // One swap per position of the switch: a feature that comes back to the wrong generator (XeFG refused it, or the
    // Create bypassed the runtime route) must not start a release loop.
    if (!wrong || !in.gameWantsOn || in.attempted) return SwapAction::eNone;
    if (!in.exact) return SwapAction::eUnavailable;
    if (in.wantedXefg && in.sinceXefgReleaseMs < kSwapSpacingMs) return SwapAction::eWait;
    return SwapAction::eBegin;
}

// The menu's XeMFG frame count (2..6 presented per game frame) as DLSS-G's numFramesToGenerate, within what the
// Streamline wrapper takes (its compiled maximum of generated frames; 0 when unknown keeps the game's own count).
constexpr uint32_t GeneratedFrames(uint32_t xemfgFrames, uint32_t wrapperMaximum, uint32_t gameCount) noexcept
{
    if (!wrapperMaximum) return gameCount ? gameCount : 1;
    const uint32_t wanted = xemfgFrames > 1 ? xemfgFrames - 1 : 1;
    return wanted < wrapperMaximum ? wanted : wrapperMaximum;
}

// The features created here, found by the address Streamline holds. Compares addresses only, so any NGX handle can be
// asked about without reading it.
template <class T, size_t N>
class FeatureTable
{
  public:
    bool Add(T* item) noexcept
    {
        if (!item || Find(item)) return false;
        for (T*& slot : items_)
            if (!slot) { slot = item; return true; }
        return false;
    }
    T* Find(const void* key) const noexcept
    {
        if (!key) return nullptr;
        for (T* item : items_)
            if (static_cast<const void*>(item) == key) return item;
        return nullptr;
    }
    bool Remove(const void* key) noexcept
    {
        if (!key) return false;
        for (T*& slot : items_)
            if (static_cast<const void*>(slot) == key) { slot = nullptr; return true; }
        return false;
    }
    size_t Count() const noexcept
    {
        size_t count = 0;
        for (T* item : items_) count += item != nullptr;
        return count;
    }

  private:
    std::array<T*, N> items_{};
};
}
