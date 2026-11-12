#pragma once
// vkhair.4: universal RTX hair in Vulkan games. A game's RTX 50 hair (VK_NV_ray_tracing_linear_swept_spheres) on RTX 40 and RTX 30 series GPUs:
//
// - vkEnumerateDeviceExtensionProperties lists the LSS extension and vkGetPhysicalDeviceFeatures2(KHR) reports
//   linearSweptSpheres on an admitted GPU (NVIDIA, CUDA compute capability 8.9 or 8.6 matched by LUID, no LSS of its
//   own, VK_KHR_ray_tracing_position_fetch with its feature).
// - vkCreateDevice: when the game enables the LSS extension, it is taken out, its feature structure reads false, and
//   VK_KHR_ray_tracing_position_fetch is enabled in its place. Only such a device is converted.
// - Its LSS shaders are rewritten to triangle position fetch (lss_spirv.h) at vkCreateShaderModule, in pipelines'
//   inline modules and in vkCreateShadersEXT; pipelines lose the LSS pipeline flag.
// - Its acceleration-structure size queries and builds go through vk_dots_builder.h (LSS -> tagged DOTS triangles,
//   instance culling off on hair, ALLOW_DATA_ACCESS on every bottom level); the compute pipeline a command buffer has
//   bound is tracked so the converter can bind it back.
//
// overlay_vulkan.cpp calls in here. Only Vulkan 1.0 types cross this header (that file builds with the module's
// pinned Vulkan 1.2 headers; this module with the 1.4 ones). Everything fails closed: an unadmitted GPU, or a device
// whose game did not enable LSS, sees only pass-through wrappers.
#include <Windows.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <memory>

namespace rtx_hair::vk {
// vkCreateInstance succeeded: the loader's vkGetInstanceProcAddr for it (admission queries its physical devices).
void InstanceCreated(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) noexcept;
void InstanceDestroyed(VkInstance instance) noexcept;

// vkCreateDevice. PrepareDevice returns null unless the game enables the LSS extension on an admitted GPU; then
// EditedInfo is the create info to pass on (valid while the edit lives), and DeviceCreated converts the new device.
struct DeviceEdit;
struct DeviceEditDeleter
{
    void operator()(DeviceEdit* edit) const noexcept;
};
using DeviceEditPtr = std::unique_ptr<DeviceEdit, DeviceEditDeleter>;
DeviceEditPtr PrepareDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept;
const VkDeviceCreateInfo* EditedInfo(const DeviceEdit* edit, const VkDeviceCreateInfo* original) noexcept;
void DeviceCreated(DeviceEditPtr edit, VkDevice device) noexcept;
// vkDestroyDevice, before the driver's: the device is idle (the game's obligation), so everything of ours goes.
void DeviceDestroyed(VkDevice device) noexcept;

// The wrapper for a Vulkan entry point universal hair needs, else `original`. From vkGetInstanceProcAddr,
// vkGetDeviceProcAddr and the loader's exports alike; each wrapper passes through on devices that are not converted.
PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original) noexcept;
}
