#pragma once
// XeMFG standalone on D3D12. RTXMFG is the XeFG host without OptiScaler: the DXGI factory proxy
// (overlay_dxgi_proxy.cpp) asks here before it creates a game's swap chain, and when the host takes it, XeFG creates
// the real chain (on a system DXGI factory, so frame generators above us see one present per frame) and the game gets
// XeFG's proxy chain, wrapped by the menu's proxy like any other. Every frame's depth and motion vectors are tagged on
// the game's own command lists as they arrive (XeFG copies what is valid only then), from Streamline's DLSS-G tags or
// from the upscaler the game evaluates (xess/standalone_policy.h). The game's own DLSS Frame Generation is held at Off
// while the host replaces it (patcher.cpp HandlePublicSetOptions).
//
// libxess_fg.dll and libxell.dll are never bundled: the player's copy in an RTXMFG-XeSS folder beside MFG Unlocker or
// the game, or beside either, as for the Vulkan host. Everything fails closed: a chain, frame or input that does not
// meet every requirement stays the game's own.
#include <Windows.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;
namespace sl { struct ResourceTag; struct Constants; }

namespace xess_d3d12
{
// The menu's XeMFG standalone switch and frame count (2..6), read at start and on every control change. A chain
// created while the switch is on is taken over at its creation; a running game's chain moves live (below).
void Configure(bool enabled, uint32_t frames) noexcept;
bool Enabled() noexcept;
// xemfglive.1: XeMFG could start in this process without a restart: it is on, or the player's XeSS pair is installed
// where the host loads it (one file search; nothing is loaded) and OptiScaler does not host XeFG. Launch-time hooks
// only XeMFG uses (patcher.cpp's Streamline latency wrappers, DiscoverUpscalers' hooks; both pass straight through
// while it is off) are placed then, so switching on later finds them.
bool Ready() noexcept;
// The menu's FPS limit in presented frames per second (0 off), applied as XeLL's minimum interval between
// the game's frames while the game's frames sleep in XeLL.
void SetFpsLimit(uint32_t fps) noexcept;
// Dynamic MFG (xess/xemfg_dynamic.h): the count follows the real frame rate toward the target (0: the FPS
// limit, else just under the refresh rate), up to Configure's frames; rounding 0 at most, 1 nearest, 2 at least; 1X
// (generation off) when allowed; "Use target as cap" holds the target with the XeLL limit. Applies live.
void SetDynamic(bool enabled, uint32_t targetFps, int rounding, bool allow1x, bool targetAsCap) noexcept;

// DXGI factory proxy, CreateSwapChainForHwnd / CreateSwapChain: true when the host created the chain (hr and *chain
// are the result), false to create it natively. `caller` is the creation's return address.
bool CreateSwapChain(IUnknown* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, void* caller, IDXGISwapChain1** chain,
    HRESULT& hr) noexcept;
// This thread is inside CreateSwapChain's takeover: XeFG's output chain created now takes the menu
// (overlay_dx12.cpp OutputSwapchainWanted, AttachOutputSwapchain), the game's chain gets a menu-less proxy.
bool InsideTakeover() noexcept;
// The menu proxy could not wrap the host's chain (it would never see its presents or its release): the chain
// returned by CreateSwapChain is released and its XeFG context destroyed; the caller then creates it natively.
void Abandon(IDXGISwapChain* chain) noexcept;
bool Owns(IDXGISwapChain* chain) noexcept;
// Around the game's own Present/Present1 on a host chain (the menu proxy's outer call, not DXGI_PRESENT_TEST).
void BeforePresent(IDXGISwapChain* chain) noexcept;
void AfterPresent(IDXGISwapChain* chain, HRESULT result) noexcept;
// pacerfix.1: the game's ResizeBuffers / ResizeBuffers1 on a host chain succeeded (the menu proxy): the flip queue reads
// XeFG's native chain's back buffers again (sdk_pacing.h RecheckCapacity).
void ChainResized(IDXGISwapChain* chain) noexcept;
// The menu proxy released its reference to the chain (the game's last): XeFG's context goes.
void ChainReleased(IDXGISwapChain* chain, const char* why = "game_release") noexcept;

// xemfglive.1: live switching. The game's chain moves to XeFG (switched on after the game created it natively) or
// back to a native chain (switched off) inside the game's own ResizeBuffers on the menu proxy, the one call after
// which the game holds none of the chain's buffers (overlay_dxgi_proxy.cpp MoveOnResize). When the switch changes,
// the proxy over the game's main chain asks for that resize once: the host shrinks the game's window by a pixel and
// restores it. A game that ignores it moves at its next resolution or window-mode change, or a restart.
// xemfglive.3: a chain whose game renders into the proxy's own back buffers (overlay_back_buffers.h) moves at a Present
// instead, with no resize: the proxy holds the game's DLSS-G (HoldForMove) until it is Off at the provider, then
// replaces the chain between two of the game's frames.
enum class Move { eNone, eToXefg, eToNative };
// What the chain behind a menu proxy should become (`hostChain`: it is XeFG's). Lock-free: every Present asks.
Move WantedMove(bool hostChain) noexcept;
// Changes with every on/off of the switch: a proxy asks for the resize once per generation.
uint32_t SwitchGeneration() noexcept;
void RequestResize(HWND window, uint32_t generation) noexcept;
// xemfglive.3: a move at a Present is under way (the menu's "switching", the hold on the game's DLSS-G) for the next
// 1.5 s; the proxy renews it every Present while it waits. `first` logs the move's start.
void HoldForMove(uint32_t generation, bool first) noexcept;
// The game's DLSS-G is applied On at the provider (the last DlssgApplied).
bool DlssgOn() noexcept;
// Before a running game's chain is released for XeFG: the takeover's refusals for its description and window, and
// the player's XeSS pair (loaded here when the switch was off at launch). False with the reason.
bool LivePreflight(HWND window, const DXGI_SWAP_CHAIN_DESC1& desc, std::string& reason) noexcept;
// A ResizeBuffers on a proxy whose chain should move (before anything is released; the resize thread waits for it),
// and how the move ended: `refused` null when the chain moved, else why it stayed (logged and shown in the menu).
void MoveAttempt() noexcept;
void MoveResult(Move move, const char* refused, bool retried = false) noexcept;
// XeFG's chain for a running game, from the proxy's ResizeBuffers once the game's chain is gone. `force`: also
// with the switch off (the native chain that should replace it could not be created). The menu attaches to XeFG's
// output chain as at creation; the result is XeFG's proxy chain (the caller's reference).
bool CreateSwapChainLive(IUnknown* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, bool force, IDXGISwapChain1** chain, std::string& reason) noexcept;
// Whether Streamline's DLSS Frame Generation is applied On (patcher.cpp, at every accepted SetOptions): a chain
// moved while it was waits with generation until the hold reached the provider, so two generators never stack.
void DlssgApplied(bool on) noexcept;
// xemfglive.5: a live move released the game's native chain. Once DLSS-G was applied On in this process, Streamline
// would create its feature a second time on the next chain, so the game's DLSS-G is then held at Off until a restart
// (ReplacesFrameGeneration), and XeFG keeps the chain when switched off (WantedMove; the menu's state "held").
// xemfglive.6: that hold is the fallback only. A move to XeFG of a native chain first asks for the exact release of
// DLSS-G's feature (WantDlssgRelease: the game's DLSS-G Off without eRetainResourcesWhenOff, as a live preset switch
// does) and moves once DlssgReleased (no live feature, no Create in flight); switching back re-creates DLSS-G then.
void NativeChainReleased() noexcept;
void WantDlssgRelease(bool want) noexcept;
bool DlssgReleaseWanted() noexcept;
bool DlssgEverApplied() noexcept;
bool DlssgReleased() noexcept;
// xemfglive.9: the process's video memory on the adapter of `queue` (the game's creation queue): local and non-local
// usage and the local budget, logged as d3d12_live_move_vram at a live move's steps (`point`).
void LogVideoMemory(IUnknown* queue, const char* point) noexcept;

// While the host runs (or may still take a chain over), the game's DLSS Frame Generation is held at Off and its
// on/off request switches XeMFG instead.
bool ReplacesFrameGeneration() noexcept;
void GameFrameGeneration(bool on) noexcept;

// Inputs. Streamline: after the game's slSetTag/slSetTagForFrame and slSetConstants returned eOk (patcher.cpp);
// `framed` false for the legacy slSetTag, whose frame is the latest token seen.
void StreamlineTags(uint32_t viewport, bool framed, uint32_t frame, const sl::ResourceTag* tags, uint32_t count,
    void* commandList) noexcept;
void StreamlineConstants(const sl::Constants& constants, uint32_t frame, uint32_t viewport) noexcept;
// Streamline's PCL/Reflex markers (sl::PCLMarker 0..5 = XeLL's markers) and the game's Reflex sleep; `slept` when
// the game's own sleep ran (XeLL then takes the markers only).
void LatencyMarker(uint32_t marker, uint32_t frame) noexcept;
void LatencySleep(uint32_t frame, bool slept) noexcept;
// While XeMFG generates, the game's Reflex sleep is XeLL's (as OptiScaler's XeLL output does with XeFG):
// the driver's Reflex would pace the game against XeFG's extra presents, a second pacer on top of XeLL's.
bool ReplacesReflexSleep() noexcept;
// A DLSS Super Resolution / Ray Reconstruction D3D12 evaluation is about to run on this list (ampere_backend.cpp's
// NGX thunks, before the real Evaluate).
void CaptureDlssEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters) noexcept;
// Hooks the XeSS SR (libxess) and FidelityFX API (amd_fidelityfx_*_dx12) modules the process loaded; the worker
// calls it on module changes and every few seconds. While XeMFG is Ready (xemfglive.1), so a live switch finds them.
void DiscoverUpscalers() noexcept;

struct Snapshot
{
    bool enabled = false;
    bool active = false;           // a game chain presents through XeFG
    bool generating = false;       // XeFG generated for the last frames
    bool restartNeeded = false;    // switched on after the game's chain was created natively, and it did not move
    bool moving = false;           // xemfglive.1: a live move was asked for and is awaited
    bool dlssgRetired = false;     // xemfglive.5: the game's DLSS-G is held at Off until a restart
    bool gameFrameGeneration = false, dlssgSeen = false, fsrFrameGeneration = false;
    uint32_t frames = 0;           // XeFG's presented frames per base frame (generated + 1) once applied
    uint32_t source = 0;           // xemfg_standalone::Source of the last frame
    uint64_t basePresents = 0, inputFrames = 0;
    // The game's and XeFG's frame rates (FPS x 1000) and the sample's age (UINT64_MAX before the first);
    // the FPS limit and the XeLL interval it set (0 = none).
    uint32_t baseFpsMilli = 0, outputFpsMilli = 0;
    uint64_t fpsAgeMs = UINT64_MAX;
    uint32_t fpsLimit = 0, limitIntervalUs = 0;
    // Dynamic MFG runs (frames is its count, 1 = generation off), its target (FPS x 1000) and its cap mode.
    bool dynamic = false, dynamicCap = false;
    uint32_t dynamicTargetMilli = 0;
    std::string state = "off";
    std::string reason;
    std::string runtime;           // the folder XeFG was loaded from
    // After a failed search: the RTXMFG-XeSS folder beside MFG Unlocker, the game's EXE folder and
    // the first near-miss name (UTF-8).
    std::string runtimeSearch, runtimeGame, runtimeMisnamed;
    // oldruntime.1, after a pair loaded without every export: its folder, the short file (libxess_fg.dll or libxell.dll),
    // that file's version (empty when unreadable) and the folder a newer pair takes over from (UTF-8).
    std::string runtimeOld, runtimeOldFile, runtimeOldVersion, runtimeNewer;
};
Snapshot ReadSnapshot() noexcept;
}
