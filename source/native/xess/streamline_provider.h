#pragma once
// slprov.1: XeMFG behind the game's own Streamline, without OptiScaler. The NGX runtime's FrameGeneration feature on
// D3D12 is XeFG's internal generator (xess/d3d12_provider.h): the runtime route's replacing entries (ampere_backend.cpp
// Create, Evaluate, Release) offer each call here before NVIDIA's provider is reached, and Streamline's DLSS-G keeps the
// swap chain, the presenting, the pacing and Reflex.
//
// genswap.1: this is how XeMFG standalone runs in a game that drives Streamline's DLSS-G on RTX 40/50, and how its
// switch works live there. The switch only chooses which generator the game's next DLSS-G feature belongs to; the
// control path (patcher.cpp) has Streamline release the current feature exactly and create it again, so no swap chain
// is replaced (xess/streamline_provider_policy.h DecideSwap). Where the route cannot run, the D3D12 host takes the
// game's chain over as before (xess/d3d12_host.h BehindStreamline).
//
// RTX 40/50 through the route's native branch; genswap30.1: RTX 30/20 through the translated branch, once the translated
// program admits it (streamline_provider_policy.h TranslatedRoute, ampere_backend::XefgStandInAvailable). A Create that
// can't be served here (no XeFG runtime, XeFG not a profiled image, inputs refused at creation, on RTX 30/20 a wrapper
// ring short of 6x) goes on to NVIDIA's provider, so the game keeps real DLSS-G; the refusal is final for the process
// and hands XeMFG back to the takeover.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace xess_streamline_provider
{
// At every control change: the menu's XeMFG standalone switch (false under OptiScaler, which hosts XeFG itself) and the
// control key xemfgStreamline (0 auto, 1 on, 2 off). The next Create reads them; a feature created here stays here
// until Streamline releases it.
void Configure(bool switchOn, uint32_t key) noexcept;
uint32_t KeyValue() noexcept;
// XeFG should own the game's next DLSS-G feature: the switch is on, the key is not off and XeFG has not refused.
bool Wanted() noexcept;
// XeFG refused a feature in this process.
bool Refused() noexcept;
// The features XeFG holds, and the time since its last one was released (UINT64_MAX: none yet).
uint32_t Live() noexcept;
uint64_t SinceReleaseMs() noexcept;
// patcher.cpp: a Streamline DLSS-G plug-in was found in the process (a control route exists).
void NoteRouteLoaded() noexcept;
bool RouteLoaded() noexcept;
// The provider's one-time preparation on the game's device (the player's XeSS pair, the route's unlock and port,
// tensor cores): at the swap chain's creation with the switch on, while the game still loads, else at the first
// Present after the switch (xess_d3d12::PrepareRoute). False: refused, final for the process.
bool Prewarm(ID3D12Device* device) noexcept;
// The preparation succeeded (here or in a Create). Until then the control path sends XeMFG's options nowhere and
// starts no swap toward XeFG, so a refusal can never leave NVIDIA's provider with XeMFG's frame count.
bool Prepared() noexcept;
// patcher.cpp's live switch for the menu: Off is being submitted for a swap (`holding`), or a feature stays with the
// generator the switch no longer names until the game re-creates it (`stuck`).
void NoteSwap(bool holding, bool stuck) noexcept;

// For the menu (xess_d3d12::ReadSnapshot while the route is in charge).
struct Status
{
    uint32_t live = 0;
    bool generating = false;      // XeFG generated within the last second
    uint32_t frames = 0;          // presented frames per game frame of the last batch (generated + 1)
    bool switching = false, stuck = false;
    uint64_t batches = 0;
    uint32_t baseFpsMilli = 0, outputFpsMilli = 0;
    uint64_t fpsAgeMs = UINT64_MAX;
};
Status ReadStatus() noexcept;

// True when the call was served here, `result` being its NVSDK_NGX_Result. False: it isn't this unit's (forward it).
bool TryCreate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output,
               int32_t& result) noexcept;
bool TryEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                 int32_t& result) noexcept;
bool TryRelease(NVSDK_NGX_Handle* handle, int32_t& result) noexcept;
// genswap30.1: the handle is a feature XeFG holds (compares addresses only). RTX 30/20's translated branch asks before its
// own feature table, which refuses every handle it did not create.
bool Holds(const NVSDK_NGX_Handle* handle) noexcept;
// genswap30.1: a Create the route was offered cannot be XeFG's for a reason found outside it (`reason`, a JSON-safe
// word): as a refusal, final for the process; NVIDIA's provider takes the feature and XeMFG goes back to the takeover.
void Decline(const char* reason) noexcept;
}
