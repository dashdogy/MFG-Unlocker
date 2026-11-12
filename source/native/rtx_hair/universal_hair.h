#pragma once
// rtxhair.1: universal path-traced hair. A D3D12 game's RTX 50 hair (NVAPI linear swept
// spheres, LSS) on RTX 40 and RTX 30 series GPUs, with no per-game code:
//
// - NVAPI (nvapi_QueryInterface): the LSS capability reads as supported, the LSS shader
//   ops this module rewrites read as supported, ENABLE_LSS_SUPPORT is left out of the
//   pipeline options, and LSS acceleration-structure builds become DOTS triangle builds
//   (dots_builder.h) recorded on the game's own command list.
// - D3D12 (witcher_dots/gpu_runtime, InitializeTracking): DXIL with NVAPI LSS ops is
//   rewritten as the game creates its state objects and compute pipelines (lss_dxil.h),
//   and the converter's compute bindings are handed back to the game after each build.
//
// Admitted only on an NVIDIA RTX 40 or RTX 30 series GPU (CUDA compute capability 8.9 or
// 8.6, matched to the game's adapter by LUID) without native LSS, whose driver supports
// the triangle position fetch ops (86, 98), with DXR 1.1, and with dxcompiler.dll and
// dxil.dll beside the game executable or this module. Anything else leaves the game as
// it is on that GPU. Witcher 3's own DOTS path stands down while it is on.
#include <Windows.h>
// IUnknown: Windows.h leaves the COM headers out under WIN32_LEAN_AND_MEAN.
#include <Unknwn.h>

#include <cstdint>

namespace rtx_hair::universal {
// On unless [RtxHair] Universal=0 in RTXMFG.RtxHair.ini beside the game executable; the
// environment variable RTXMFG_RTX_HAIR=0 or 1 decides whatever the file says. Read once
// per process.
bool Requested() noexcept;
// After the game's D3D12CreateDevice returned `device` (or a forwarding wrapper of it):
// prepares on the first device of an admitted GPU, once per process.
void ObserveDevice(IUnknown* device) noexcept;
// nulltoken.1: after the game created a Vulkan device (converted by vk_hair.h; a D3D12 device
// seen before or after still decides as above).
void ObserveVulkanDevice() noexcept;
// vkhair.4: whether the GPU with this LUID is an RTX 40 or RTX 30 series one (one CUDA device
// with that LUID, compute capability 8.9 or 8.6); `major` and `minor` get its capability.
bool CudaAdmits(const LUID& luid, int& major, int& minor) noexcept;

// For the menu's Path traced hair panel.
enum class State : uint32_t
{
    Off,         // turned off (Universal=0 or RTXMFG_RTX_HAIR=0)
    Waiting,     // requested, no D3D12 device (or converted Vulkan device) yet
    NotAdmitted, // this GPU, driver or game folder (see reason); the game runs as on a stock GPU
    Failed,      // preparation failed after the device hooks: off for this process
    Prepared,    // LSS reported to the game, its hair builds and shaders converted
};
struct Snapshot
{
    State state{};
    uint64_t capsAnswered{}, optionsStripped{}, shadersRewritten{}, shadersRefused{};
    uint64_t prebuilds{}, builds{}, updates{}, inactiveBuilds{}, skippedUpdates{}, failures{};
    uint64_t lastBuildAgeMs{UINT64_MAX}; // since the latest converted hair build
    uint64_t poolBuffers{}, poolBytes{}, evictions{}, rebuilds{};
    uint64_t hairBlas{}, topLevelPatched{}, topLevelUnpatched{}, instanceCopyBytes{}; // hair instance culling (instance_patch.h)
    // hairmem.1, Vulkan only (held now): spare and retired vertex buffers, reuses, and the game's own hair BLASes.
    bool vulkan{};
    uint32_t ribbons{};
    uint64_t spareBytes{}, retiredBytes{}, reuses{}, gameHairBlasBytes{};
    bool witcherLook{}, lookRefused{};        // Witcher 3: its look fixes (on, or refused by the driver)
    uint64_t lookShaders{}, lookFailures{};   // shaders created with them; anchor or validation failures
    char reason[256]{};    // why not admitted or failed
    char lastError[256]{}; // the builder's latest error
};
Snapshot ReadSnapshot() noexcept;
}
