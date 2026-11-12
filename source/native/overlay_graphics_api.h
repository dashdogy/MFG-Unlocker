#pragma once
#include <atomic>
#include <cstdint>

// dx11menu.1: the graphics APIs the application uses, as the menu's own routes observe them. Bookkeeping only: the
// DirectX 11 menu exists only in a DirectX 11 application; a D3D12 or Vulkan route never changes.
//   kD3D11: a D3D11 device returned by the application's D3D11CreateDevice(AndSwapChain) gateway.
//   kD3D12: a D3D12 device returned by the D3D12CreateDevice gateway, or a D3D12 queue's swapchain creation.
//   kVulkan: a Vulkan instance or device created through the menu's Vulkan entries.
//   kD3D12Linked / kVulkanLinked: the main executable imports D3D12CreateDevice / vkCreateInstance and neither
//   D3D11 creation entry (a D3D12 or Vulkan game from its first instruction).
namespace single_overlay::graphics_api
{
inline constexpr uint32_t kD3D11 = 1u, kD3D12 = 2u, kVulkan = 4u, kD3D12Linked = 8u, kVulkanLinked = 16u;
inline std::atomic<uint32_t> gSeen{0};
inline std::atomic<uint32_t> gFirst{0};

inline void Note(uint32_t api) noexcept
{
    uint32_t none = 0;
    gFirst.compare_exchange_strong(none, api, std::memory_order_acq_rel);
    gSeen.fetch_or(api, std::memory_order_acq_rel);
}
inline uint32_t Seen() noexcept { return gSeen.load(std::memory_order_acquire); }
inline uint32_t First() noexcept { return gFirst.load(std::memory_order_acquire); }
// No D3D12 or Vulkan use has been seen, linked or created, before or after a D3D11 one.
inline bool D3D11Application() noexcept { return !(Seen() & ~kD3D11); }
}
