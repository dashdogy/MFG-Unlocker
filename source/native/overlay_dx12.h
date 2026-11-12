#pragma once
#include <dxgi1_6.h>
#include <cstdint>
struct ID3D12CommandQueue;
namespace single_overlay::dx12 {
struct Session;
// provider: the XeSS-FG runtime image that owns a provider swapchain, or
// null for a system DXGI swapchain. output: the chain a frame
// generator presents its output frames on; it carries the menu only.
// streamlineBase: such a chain beneath Streamline (DLSS-G's), whose layer
// above keeps DLSS 5 and the counters.
Session* CreateSession(IDXGISwapChain*, IUnknown*, HMODULE provider = nullptr, bool output = false,
    bool streamlineBase = false) noexcept;
// The chain beneath this one (Streamline's base) is our proxy of a streamlineBase session.
bool AboveStreamlineBase(IUnknown* chain) noexcept;
// nvpresent.1: NVIDIA's driver present layer (Smooth Motion) under the chain; the DirectX 11 session accepts it too.
bool NvPresentChain(IDXGISwapChain3*) noexcept;
bool RetireSession(Session*) noexcept;
// A live session draws on this window.
bool WindowSession(HWND) noexcept;
// The XeSS-FG swapchain the game presents through. The menu attaches to it
// with the exact provider generation and presentation queue.
bool AttachProviderSwapchain(IDXGISwapChain*, ID3D12CommandQueue*, HMODULE, uint64_t, IDXGISwapChain**) noexcept;
bool DetachProviderSwapchain(IDXGISwapChain*, uint64_t) noexcept;
// XeFG's native output chain of an OptiScaler context, as XeFG
// created it on the creation thread of OptiScaler's InitFromSwapChainDesc.
// Wanted: OptiScaler is detected and its UI mode lets this menu show. Attach
// replaces *chain with a proxy that keeps XeFG's reference (keep receives
// another); null when attached, otherwise why not. Retire, before XeFG's
// destruction: the menu session ends and the proxy keeps forwarding to the
// chain until XeFG releases it (keep must be released first).
bool OutputSwapchainWanted() noexcept;
const char* AttachOutputSwapchain(IDXGISwapChain1** chain, IUnknown* queue, HWND window, uint64_t generation,
    IDXGISwapChain** keep) noexcept;
bool RetireOutputSwapchain(IDXGISwapChain*, uint64_t generation) noexcept;
void DestroySession(Session*) noexcept;
bool BeginPresent(Session*, UINT flags, bool partial) noexcept;
// The session presents the game's main output (largest area, foreground first).
bool IsPrincipal(Session*) noexcept;
// dx11menu.1: a D3D12 menu session is live (read only; the DirectX 11 menu yields to it).
bool AnySession() noexcept;
void EndPresent(Session*, HRESULT, UINT flags) noexcept;
bool BeginResize(Session*) noexcept;
void EndResize(Session*, HRESULT) noexcept;
void ObserveColorSpace(Session*, DXGI_COLOR_SPACE_TYPE) noexcept;
void VerifyResizeQueues(Session*, UINT, IUnknown* const*) noexcept;
}
