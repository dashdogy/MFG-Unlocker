#pragma once
#include <Windows.h>
namespace single_overlay::vulkan
{
// Public import/resolver gateways only. Never detour the loader or driver.
FARPROC Resolve(HMODULE module, LPCSTR name, FARPROC original) noexcept;
// vkprov.2: Resolve beside OptiScaler's own menu (RTXMFG's overlay off). Only while the XeMFG DLSS-G provider's shim is
// installed, and only the instance and device lifecycle the provider needs: no swapchain, present or surface entry,
// now or through a wrapped vkGetInstanceProcAddr / vkGetDeviceProcAddr. Anything else returns `original`.
FARPROC ResolveLifecycle(HMODULE module, LPCSTR name, FARPROC original) noexcept;
// dx11menu.1: a Vulkan menu swapchain is live (read only; the DirectX 11 menu yields to it).
bool AnySession() noexcept;
}
