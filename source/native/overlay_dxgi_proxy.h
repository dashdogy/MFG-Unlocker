#pragma once
#include "single_module_status.h"
#include <dxgi1_6.h>
#include <d3d11.h>
namespace single_overlay::proxy {
using FactoryFn=HRESULT (WINAPI*)(REFIID,void**);
using Factory2Fn=HRESULT (WINAPI*)(UINT,REFIID,void**);
using ParentFn=HRESULT (STDMETHODCALLTYPE*)(IDXGIObject*,REFIID,void**);
HRESULT FactoryCall(FactoryFn,REFIID,void**) noexcept;
HRESULT FactoryCall(Factory2Fn,UINT,REFIID,void**) noexcept;
HRESULT ParentFactoryCall(ParentFn,IDXGIObject*,REFIID,void**) noexcept;
// dx11menu.1: an application's D3D11CreateDeviceAndSwapChain (caller: its return address, for the log). The call is
// one outer creation, so nothing created inside it is wrapped; the swapchain it returns gets the DirectX 11 menu as
// a factory proxy's would (the same checks and refusals), unless OptiScaler hosts the game.
HRESULT D3D11ChainCall(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN,void* caller,IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,
    const D3D_FEATURE_LEVEL*,UINT,UINT,const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,ID3D11Device**,D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**) noexcept;
// Streamline manual hooking: the application creates native DXGI objects and
// upgrades them with slUpgradeInterface. Owned reports this layer's proxies;
// WrapUpgraded wraps the upgraded factory in *output as if creation had
// returned it, so its swapchains get the menu. True when *output was replaced.
bool Owned(IUnknown*) noexcept;
bool WrapUpgraded(void** output) noexcept;
// Logging: "<pointer> <kind> ours=<0|1> queryInterface=<module+0xRVA>".
// Kind is the first public interface the object answers: factory, swapchain,
// d3d12-device, d3d12-queue, adapter or other.
void Describe(IUnknown*,wchar_t* text,size_t count) noexcept;
void ReadStatus(MfgSingleModuleStatus&) noexcept;
}
