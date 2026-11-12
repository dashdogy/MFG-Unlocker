// MfgXeFGVkGetApi (mfg_xefg_vk.h): the XeFG-on-Vulkan engine for OptiScaler's XeFG_Vk output. A thin translation of
// the C ABI onto vk_host's hosted functions.
#include "vk_host.h"
#include "mfg_xefg_vk.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
using namespace xess_vulkan;

void* VKAPI_CALL PrepareDeviceApi(const MfgXeFGVkDeviceInfo* info, const VkDeviceCreateInfo* createInfo, const VkDeviceCreateInfo** edited)
{
    if (edited) *edited = createInfo;
    if (!info || info->size < sizeof(MfgXeFGVkDeviceInfo) || !createInfo || !edited) return nullptr;
    auto edit = hosted::PrepareDevice(info->instance, info->getInstanceProcAddr, info->instanceApiVersion, info->physicalDevice, createInfo);
    if (!edit) return nullptr;
    *edited = EditedInfo(edit.get(), createInfo);
    return edit.release();
}

void VKAPI_CALL DiscardDeviceApi(void* edit)
{
    DeviceEditPtr owned(static_cast<DeviceEdit*>(edit));
}

VkBool32 VKAPI_CALL DeviceCreatedApi(void* edit, VkDevice device, PFN_MfgXeFGVkOriginalProc originals, void* user)
{
    DeviceEditPtr owned(static_cast<DeviceEdit*>(edit));
    return owned && hosted::DeviceCreated(std::move(owned), device, originals, user) ? VK_TRUE : VK_FALSE;
}

void VKAPI_CALL DeviceDestroyingApi(VkDevice device) { DeviceDestroying(device); }

VkResult VKAPI_CALL CreateSwapchainApi(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo, HWND window, uint32_t initFlags,
                                       VkSwapchainKHR* swapchain)
{
    return hosted::CreateSwapchain(device, createInfo, window, (initFlags & MFG_XEFG_VK_INIT_INVERTED_DEPTH) != 0, swapchain);
}

VkBool32 VKAPI_CALL DestroySwapchainApi(VkDevice device, VkSwapchainKHR swapchain)
{
    return hosted::DestroySwapchain(device, swapchain) ? VK_TRUE : VK_FALSE;
}

VkResult VKAPI_CALL GetSwapchainImagesApi(VkDevice, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images)
{
    return hosted::GetSwapchainImages(swapchain, count, images);
}

VkResult VKAPI_CALL AcquireNextImageApi(VkDevice, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                        uint32_t* index)
{
    return hosted::AcquireNextImage(swapchain, timeout, semaphore, fence, index);
}

VkResult VKAPI_CALL RecordFrameApi(VkSwapchainKHR swapchain, VkCommandBuffer command, const MfgXeFGVkFrame* frame)
{
    if (!frame || frame->size < sizeof(MfgXeFGVkFrame)) return VK_ERROR_INITIALIZATION_FAILED;
    DlssInputs in;
    in.depth = frame->depth.image;
    in.depthView = frame->depth.view;
    in.depthFormat = frame->depth.format;
    in.depthLayout = frame->depth.layout;
    in.motion = frame->motion.image;
    in.motionView = frame->motion.view;
    in.motionFormat = frame->motion.format;
    in.motionLayout = frame->motion.layout;
    in.renderWidth = frame->depth.width;
    in.renderHeight = frame->depth.height;
    in.motionWidth = frame->motion.width;
    in.motionHeight = frame->motion.height;
    in.motionLowRes = frame->motion.width <= frame->depth.width && frame->motion.height <= frame->depth.height;
    in.jitterX = frame->jitterX;
    in.jitterY = frame->jitterY;
    in.motionScaleX = frame->motionScaleX ? frame->motionScaleX : 1.0f;
    in.motionScaleY = frame->motionScaleY ? frame->motionScaleY : 1.0f;
    in.cameraNear = frame->cameraNear;
    in.cameraFar = frame->cameraFar;
    in.cameraVFov = frame->cameraVFov;
    in.frameTimeMs = frame->frameTimeMs;
    in.reset = frame->reset != 0;
    return hosted::RecordFrame(swapchain, command, in);
}

VkResult VKAPI_CALL QueuePresentApi(VkQueue queue, const VkPresentInfoKHR* presentInfo) { return hosted::QueuePresent(queue, presentInfo); }

void VKAPI_CALL SetHdrMetadataApi(VkDevice, VkSwapchainKHR swapchain, const VkHdrMetadataEXT* metadata)
{
    hosted::SetHdrMetadata(swapchain, metadata);
}

VkResult VKAPI_CALL WaitForPresentApi(VkDevice, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout)
{
    return hosted::WaitForPresent(swapchain, presentId, timeout);
}

void VKAPI_CALL SetEnabledApi(VkSwapchainKHR swapchain, VkBool32 enabled) { hosted::SetEnabled(swapchain, enabled != VK_FALSE); }

VkBool32 VKAPI_CALL SetInterpolatedFramesApi(VkSwapchainKHR swapchain, uint32_t count)
{
    return hosted::SetInterpolatedFrames(swapchain, count) ? VK_TRUE : VK_FALSE;
}

uint32_t VKAPI_CALL MaxInterpolatedFramesApi() { return hosted::kMaxInterpolatedFrames; }

VkBool32 VKAPI_CALL OwnsSwapchainApi(VkSwapchainKHR swapchain) { return hosted::OwnsSwapchain(swapchain) ? VK_TRUE : VK_FALSE; }

void VKAPI_CALL GetStatusApi(MfgXeFGVkStatus* status)
{
    // A host built before the latency fields passes the shorter structure; those fields need the longer one.
    constexpr size_t kBaseSize = offsetof(MfgXeFGVkStatus, latencyMode);
    if (!status || status->size < kBaseSize) return;
    const auto snapshot = ReadSnapshot();
    if (status->size >= sizeof(MfgXeFGVkStatus)) {
        status->latencyMode = snapshot.latencyMode;
        status->latencyLimited = snapshot.latencyLimited ? 1 : 0;
    }
    status->active = snapshot.active ? 1 : 0;
    status->framesPerBaseFrame = snapshot.frames;
    status->basePresents = snapshot.basePresents;
    status->vulkanPresents = snapshot.vulkanPresents;
    status->missingPrograms = snapshot.missingPrograms;
    const size_t length = std::min(snapshot.reason.size(), sizeof(status->reason) - 1);
    memcpy(status->reason, snapshot.reason.data(), length);
    status->reason[length] = 0;
}

VkResult VKAPI_CALL ReleaseSwapchainImagesApi(VkDevice, VkSwapchainKHR swapchain, uint32_t count, const uint32_t* indices)
{
    return hosted::ReleaseSwapchainImages(swapchain, count, indices);
}

void VKAPI_CALL SetDebugViewApi(VkSwapchainKHR swapchain, VkBool32 enabled) { hosted::SetDebugView(swapchain, enabled != VK_FALSE); }

void VKAPI_CALL SetHostRoutesApi(uint32_t routes) { hosted::SetHostRoutes(routes); }

void VKAPI_CALL SetLatencyApi(const MfgXeFGVkLatency* latency)
{
    if (!latency || latency->size < sizeof(MfgXeFGVkLatency)) return;
    hosted::SetLatency(latency->mode, latency->lowLatency != 0, latency->boost != 0, latency->outputFpsLimit);
}

const MfgXeFGVkApi kApi = {
    sizeof(MfgXeFGVkApi), MFG_XEFG_VK_API_VERSION,
    PrepareDeviceApi, DiscardDeviceApi, DeviceCreatedApi, DeviceDestroyingApi,
    CreateSwapchainApi, DestroySwapchainApi, GetSwapchainImagesApi, AcquireNextImageApi, RecordFrameApi, QueuePresentApi,
    SetHdrMetadataApi, WaitForPresentApi,
    SetEnabledApi, SetInterpolatedFramesApi, MaxInterpolatedFramesApi, OwnsSwapchainApi, GetStatusApi,
    ReleaseSwapchainImagesApi, SetDebugViewApi, SetHostRoutesApi, SetLatencyApi,
};
} // namespace

extern "C" __declspec(dllexport) const MfgXeFGVkApi* VKAPI_CALL MfgXeFGVkGetApi(uint32_t version)
{
    return version == MFG_XEFG_VK_API_VERSION ? &kApi : nullptr;
}
