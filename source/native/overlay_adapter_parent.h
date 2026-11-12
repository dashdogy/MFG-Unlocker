#pragma once
#include <unknwn.h>
namespace single_overlay::adapter_parent {
// Observe the exact adapter supplied to a public D3D12CreateDevice call.
// The adapter pointer and all native interface tables remain unchanged.
void Observe(IUnknown*) noexcept;
// dx11menu.1: the same for the adapter of a device a public D3D11CreateDevice or D3D11CreateDeviceAndSwapChain
// call returned (IDXGIDevice::GetAdapter), so the factory the game takes from it is the menu's proxy.
void ObserveD3D11(IUnknown* device) noexcept;
}
