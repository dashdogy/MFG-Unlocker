#pragma once
#include <dxgi1_6.h>
#include <cstdint>
// dx11menu.1: the menu on a DirectX 11 game's swapchain, with the D3D12 session's contract (overlay_dx11.cpp lists
// it). The menu offers DLSS 5, the Keybinds that act here and the general settings; the rest needs DirectX 12 or Vulkan.
namespace single_overlay::dx11 {
struct Session;
// device: the creation's device argument as the application holds it. Null when the swapchain's owner, its device,
// the Direct3D 11.1 context state or the immediate context is not established: the chain is then left untouched.
Session* CreateSession(IDXGISwapChain*, IUnknown* device) noexcept;
void DestroySession(Session*) noexcept;
bool BeginPresent(Session*, UINT flags, bool partial) noexcept;
void EndPresent(Session*, HRESULT, UINT flags) noexcept;
// No buffer reference or view of ours outlives a Present, so a resize waits only for a Present in flight.
bool BeginResize(Session*) noexcept;
void EndResize(Session*, HRESULT) noexcept;
void ObserveColorSpace(Session*, DXGI_COLOR_SPACE_TYPE) noexcept;
}
