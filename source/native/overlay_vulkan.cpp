#include "single_module.h"
#include "overlay_vulkan.h"
#include "overlay_platform.h"
#include "overlay_dx12.h"
#include "overlay_graphics_api.h"
#include "overlay_hook.h"
#include "overlay_install.h"
#include "overlay_color_spv.h"
#include "present_counter.h"
#include "rtx_hair/universal_hair.h"
#include "ui_input_coherence.h"
#include "vsync_control.h"
#include "xess/vulkan/vk_host.h"
#include "xess/vulkan/vk_provider.h"
#include "rtx_hair/vk_hair.h"
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace single_overlay::vulkan
{
namespace
{
std::atomic<PFN_vkGetInstanceProcAddr> gLoaderGipa{nullptr};
PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original);

// vkprov.2: the XeMFG DLSS-G provider's route beside OptiScaler's own menu (ResolveLifecycle) has resolved an entry.
std::atomic<bool> gLifecycleOnly{false};
std::atomic<bool> gLifecycleLogged{false};
// What the provider needs: the instance and device it records into, their queues, and their destruction.
bool LifecycleEntry(const char* name)
{
    for (const char* entry : {"vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateInstance", "vkDestroyInstance",
            "vkEnumeratePhysicalDevices", "vkCreateDevice", "vkDestroyDevice", "vkGetDeviceQueue", "vkGetDeviceQueue2"})
        if (strcmp(name, entry) == 0) return true;
    return false;
}

struct Instance
{
    VkInstance handle = VK_NULL_HANDLE;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
};
struct Device
{
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::shared_ptr<Instance> instance;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    std::vector<VkQueueFamilyProperties> families;

    PFN_vkVoidFunction Get(const char* name) const
    {
        InternalScope internal;
        if (auto function = gdpa(handle, name)) return function;
        return instance->gipa(instance->handle, name);
    }
};
struct Queue
{
    VkQueue handle = VK_NULL_HANDLE;
    std::shared_ptr<Device> device;
    uint32_t family = UINT32_MAX;
    bool protectedQueue = false;
};
struct Surface
{
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
};

std::mutex gRegistryMutex;
std::unordered_map<VkInstance, std::shared_ptr<Instance>> gInstances;
std::unordered_map<VkPhysicalDevice, std::shared_ptr<Instance>> gPhysicalDevices;
std::unordered_map<VkDevice, std::shared_ptr<Device>> gDevices;
std::unordered_map<VkQueue, Queue> gQueues;
std::unordered_map<VkSurfaceKHR, Surface> gSurfaces;
// Only ordinary, locally created binary semaphores can be consumed and
// re-signaled around the UI draw. Imports can replace a semaphore's payload.
std::unordered_map<VkSemaphore, VkDevice> gNativeSemaphores;
std::atomic<bool> gSemaphoreCoverage{true};

#define MFG_VK_RENDER_FUNCTIONS(X) \
    X(GetSwapchainImagesKHR) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) X(ResetFences) \
    X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle)

struct RenderFunctions
{
#define MFG_VK_FIELD(name) PFN_vk##name name = nullptr;
    MFG_VK_RENDER_FUNCTIONS(MFG_VK_FIELD)
#undef MFG_VK_FIELD
    bool Load(const Device& device)
    {
#define MFG_VK_LOAD(name) name = reinterpret_cast<PFN_vk##name>(device.Get("vk" #name)); if (!name) return false;
        MFG_VK_RENDER_FUNCTIONS(MFG_VK_LOAD)
#undef MFG_VK_LOAD
        return true;
    }
};

struct Frame
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
};

struct State
{
    std::mutex mutex;
    std::shared_ptr<Device> device;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    VkSwapchainCreateFlagsKHR flags = 0;
    uint32_t layers = 1;
    uint32_t minImageCount = 2;
    VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<uint32_t> sharingFamilies;
    HWND window = nullptr;
    Queue queue;
    RenderFunctions vk;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<Frame> frames;
    PlatformState platform;
    bool initialized = false;
    bool rendererInitialized = false;
    bool disabled = false;

    static PFN_vkVoidFunction LoadImGuiFunction(const char* name, void* user)
    {
        return static_cast<Device*>(user)->Get(name);
    }
    static void CheckResult(VkResult result)
    {
        if (result < 0) RecordFailure(L"Vulkan UI backend reported a graphics error");
    }

    bool Initialize(const Queue& presentQueue)
    {
        if (initialized)
            return queue.handle == presentQueue.handle && queue.device == presentQueue.device;
        if (presentQueue.device != device || presentQueue.protectedQueue
            || presentQueue.family >= device->families.size()
            || !(device->families[presentQueue.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            || !(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || layers != 1
            || (flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
            || !extent.width || !extent.height || !window
            || !IsWindow(window)) return false;
        if (sharingMode == VK_SHARING_MODE_CONCURRENT
            && std::find(sharingFamilies.begin(), sharingFamilies.end(), presentQueue.family) == sharingFamilies.end())
            return false;
        int colorMode = 0;
        if (colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) colorMode = 1;
        else if (colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) colorMode = 2;
        else if (colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB) colorMode = 3;
        }
        else return false;
        queue = presentQueue;
        if (!vk.Load(*device)) return false;
        // A chain the XeMFG Vulkan host took over has XeFG's back buffers as its images.
        auto images_ = [&](uint32_t* count, VkImage* images) {
            VkResult hosted = VK_SUCCESS;
            if (xess_vulkan::GetSwapchainImages(device->handle, swapchain, count, images, hosted)) return hosted;
            return vk.GetSwapchainImagesKHR(device->handle, swapchain, count, images);
        };
        uint32_t count = 0;
        if (images_(&count, nullptr) != VK_SUCCESS || count < 2 || count > 16) return false;
        std::vector<VkImage> images(count);
        if (images_(&count, images.data()) != VK_SUCCESS) return false;
        if (!platform.Initialize(window)) return false;
        ContextScope current(platform.context);
        InternalScope internal;
        frames.resize(count);
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderInfo.attachmentCount = 1;
        renderInfo.pAttachments = &attachment;
        renderInfo.subpassCount = 1;
        renderInfo.pSubpasses = &subpass;
        renderInfo.dependencyCount = 1;
        renderInfo.pDependencies = &dependency;
        if (vk.CreateRenderPass(device->handle, &renderInfo, nullptr, &renderPass) != VK_SUCCESS) return false;
        for (uint32_t index = 0; index < count; ++index)
        {
            auto& frame = frames[index];
            frame.image = images[index];
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            // XeFG's back buffers also carry storage usage, which an sRGB view must not inherit.
            VkImageViewUsageCreateInfo viewUsage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
            if (xess_vulkan::OwnsSwapchain(swapchain)) viewInfo.pNext = &viewUsage;
            viewInfo.image = images[index];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vk.CreateImageView(device->handle, &viewInfo, nullptr, &frame.view) != VK_SUCCESS) return false;
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &frame.view;
            framebuffer.width = extent.width;
            framebuffer.height = extent.height;
            framebuffer.layers = 1;
            if (vk.CreateFramebuffer(device->handle, &framebuffer, nullptr, &frame.framebuffer) != VK_SUCCESS) return false;
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = queue.family;
            if (vk.CreateCommandPool(device->handle, &pool, nullptr, &frame.pool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = frame.pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            if (vk.AllocateCommandBuffers(device->handle, &allocation, &frame.commands) != VK_SUCCESS) return false;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vk.CreateFence(device->handle, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) return false;
        }
        if (!ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get())) return false;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = device->instance->apiVersion;
        info.Instance = device->instance->handle;
        info.PhysicalDevice = device->physical;
        info.Device = device->handle;
        info.QueueFamily = queue.family;
        info.Queue = queue.handle;
        info.DescriptorPoolSize = 128;
        info.MinImageCount = std::clamp(minImageCount, 2u, count);
        info.ImageCount = count;
        info.PipelineInfoMain.RenderPass = renderPass;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = &CheckResult;
        if (colorMode)
        {
            auto& shader = info.CustomShaderFragCreateInfo;
            shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            if (colorMode == 1) { shader.codeSize = sizeof(color::shader1); shader.pCode = color::shader1; }
            else if (colorMode == 2) { shader.codeSize = sizeof(color::shader2); shader.pCode = color::shader2; }
            else { shader.codeSize = sizeof(color::shader3); shader.pCode = color::shader3; }
        }
        rendererInitialized = ImGui_ImplVulkan_Init(&info);
        initialized = rendererInitialized;
        return initialized;
    }

    void Disable()
    {
        disabled = true;
        platform.RetireInput();
    }

    bool Drain()
    {
        if (!vk.WaitForFences || !queue.handle) return true;
        for (const auto& frame : frames)
        {
            if (!frame.submitted || !frame.fence) continue;
            const VkResult result = vk.WaitForFences(device->handle, 1, &frame.fence, VK_TRUE, 3000000000ull);
            if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return false;
        }
        // The UI owns only draw resources. It never gives an owned semaphore
        // to WSI, whose asynchronous lifetime is not covered by QueueWaitIdle.
        return true;
    }

    bool Shutdown()
    {
        if (!Drain())
        {
            Disable();
            RecordFailure(L"Vulkan UI drain timed out; GPU resources retained");
            return false;
        }
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (platform.context)
        {
            ContextScope current(platform.context);
            if (rendererInitialized)
            {
                ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get());
                ImGui_ImplVulkan_Shutdown();
            }
        }
        rendererInitialized = false;
        for (auto& frame : frames)
        {
            if (frame.framebuffer) vk.DestroyFramebuffer(device->handle, frame.framebuffer, nullptr);
            if (frame.view) vk.DestroyImageView(device->handle, frame.view, nullptr);
            if (frame.pool) vk.DestroyCommandPool(device->handle, frame.pool, nullptr);
            if (frame.fence) vk.DestroyFence(device->handle, frame.fence, nullptr);
        }
        frames.clear();
        if (renderPass) vk.DestroyRenderPass(device->handle, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
        platform.Shutdown();
        initialized = false;
        return true;
    }

    // Wait at ALL_COMMANDS, draw, then re-signal the same proven native binary
    // semaphores. Present keeps its original wait set and owns their lifetime.
    bool Render(const Queue& presentQueue, uint32_t index,
        const VkPresentInfoKHR& present, VkResult& failure)
    {
        std::lock_guard stateLock(mutex);
        if (disabled) return false;
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (!Initialize(presentQueue))
        {
            Disable();
            RecordFailure(L"Vulkan UI initialization rejected for this swapchain");
            Shutdown();
            return false;
        }
        if (!platform.WantsFrame()) return false;
        if (index >= frames.size()) { Disable(); return false; }
        auto& frame = frames[index];
        if (frame.submitted && vk.WaitForFences(device->handle, 1, &frame.fence,
                VK_TRUE, 3000000000ull) != VK_SUCCESS)
        {
            Disable();
            RecordFailure(L"Vulkan UI frame fence timed out");
            return false;
        }
        ContextScope current(platform.context);
        // The pinned backend has one dispatch table. Serialize and select the
        // exact device before touching a context, including multiple devices.
        if (!ImGui_ImplVulkan_LoadFunctions(device->instance->apiVersion, &LoadImGuiFunction, device.get())) return false;
        ImGui_ImplVulkan_NewFrame();
        if (!platform.NewFrame(extent.width, extent.height)) return false;
        platform.Draw();
        if (vk.ResetCommandPool(device->handle, frame.pool, 0) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.BeginCommandBuffer(frame.commands, &begin) != VK_SUCCESS) return false;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass;
        pass.framebuffer = frame.framebuffer;
        pass.renderArea.extent = extent;
        vk.CmdBeginRenderPass(frame.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
        vk.CmdEndRenderPass(frame.commands);
        if (vk.EndCommandBuffer(frame.commands) != VK_SUCCESS) return false;
        if (vk.ResetFences(device->handle, 1, &frame.fence) != VK_SUCCESS) return false;
        frame.submitted = false;
        std::vector<VkPipelineStageFlags> stages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commands;
        submit.signalSemaphoreCount = present.waitSemaphoreCount;
        submit.pSignalSemaphores = present.pWaitSemaphores;
        const VkResult result = vk.QueueSubmit(queue.handle, 1, &submit, frame.fence);
        if (result != VK_SUCCESS)
        {
            Disable();
            failure = result;
            RecordFailure(L"Vulkan UI submission failed");
            return false;
        }
        frame.submitted = true;
        gRenderedFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

std::unordered_map<VkSwapchainKHR, std::shared_ptr<State>> gSwapchains;
std::vector<std::shared_ptr<State>> gRetained;

std::shared_ptr<Device> FindDevice(VkDevice handle)
{
    std::lock_guard lock(gRegistryMutex);
    const auto found = gDevices.find(handle);
    return found == gDevices.end() ? nullptr : found->second;
}

void RememberQueue(VkDevice handle, uint32_t family, bool isProtected, VkQueue queue)
{
    if (!queue) return;
    auto device = FindDevice(handle);
    if (!device || family >= device->families.size()) return;
    std::lock_guard lock(gRegistryMutex);
    if (gQueues.size() < 128)
        gQueues[queue] = Queue{queue, device, family, isProtected};
}

void RememberDevice(VkPhysicalDevice physical, VkDevice handle)
{
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(gRegistryMutex);
        const auto found = gPhysicalDevices.find(physical);
        if (found != gPhysicalDevices.end()) instance = found->second;
    }
    if (!instance || !handle) return;
    InternalScope internal;
    auto device = std::make_shared<Device>();
    device->handle = handle;
    device->physical = physical;
    device->instance = instance;
    device->gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance->gipa(instance->handle, "vkGetDeviceProcAddr"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance->gipa(instance->handle, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!device->gdpa || !properties) return;
    uint32_t count = 0;
    properties(physical, &count, nullptr);
    if (!count || count > 64) return;
    device->families.resize(count);
    properties(physical, &count, device->families.data());
    device->families.resize(count);
    std::lock_guard lock(gRegistryMutex);
    if (gDevices.size() < 32) gDevices[handle] = std::move(device);
}

struct GipaTag
{
    static PFN_vkVoidFunction Call(PFN_vkGetInstanceProcAddr original, VkInstance instance, const char* name)
    {
        const auto result = original(instance, name);
        return gInsideOverlay ? result : Wrap(name, result);
    }
};
struct GdpaTag
{
    static PFN_vkVoidFunction Call(PFN_vkGetDeviceProcAddr original, VkDevice device, const char* name)
    {
        const auto result = original(device, name);
        return gInsideOverlay ? result : Wrap(name, result);
    }
};
struct CreateInstanceTag
{
    static VkResult Call(PFN_vkCreateInstance original, const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* output)
    {
        const VkInstanceCreateInfo* used = info;
        // The XeMFG Vulkan host's programs need Vulkan 1.3 (the game's own version, if lower, is
        // raised only when the host is enabled and the loader offers it; a refusal retries the game's own).
        xess_vulkan::InstanceEdit edit;
        if (!gInsideOverlay) used = xess_vulkan::PrepareInstance(info, gLoaderGipa.load(std::memory_order_acquire), edit);
        VkResult result = original(used, allocator, output);
        if (result != VK_SUCCESS && used != info)
        {
            used = info;
            result = original(info, allocator, output);
        }
        if (!gInsideOverlay && result == VK_SUCCESS && output && used)
        {
            graphics_api::Note(graphics_api::kVulkan); // dx11menu.1: bookkeeping only
            auto instance = std::make_shared<Instance>();
            instance->handle = *output;
            instance->gipa = gLoaderGipa.load(std::memory_order_acquire);
            if (used->pApplicationInfo && used->pApplicationInfo->apiVersion)
                instance->apiVersion = used->pApplicationInfo->apiVersion;
            xess_vulkan::InstanceCreated(*output, used, instance->gipa);
            rtx_hair::vk::InstanceCreated(*output, instance->gipa); // vkhair.4: universal hair's GPU admission
            std::lock_guard lock(gRegistryMutex);
            if (instance->gipa && gInstances.size() < 16) gInstances[*output] = std::move(instance);
        }
        return result;
    }
};
struct EnumeratePhysicalTag
{
    static VkResult Call(PFN_vkEnumeratePhysicalDevices original, VkInstance instance,
        uint32_t* count, VkPhysicalDevice* physical)
    {
        const VkResult result = original(instance, count, physical);
        if (!gInsideOverlay && (result == VK_SUCCESS || result == VK_INCOMPLETE) && physical && count)
        {
            std::lock_guard lock(gRegistryMutex);
            const auto found = gInstances.find(instance);
            if (found != gInstances.end() && *count < 128)
                for (uint32_t index = 0; index < *count; ++index)
                    gPhysicalDevices[physical[index]] = found->second;
        }
        return result;
    }
};
struct CreateDeviceTag
{
    static VkResult Call(PFN_vkCreateDevice original, VkPhysicalDevice physical,
        const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* output)
    {
        // The XeMFG Vulkan host's queues, extensions and features join the game's device. A device
        // that refuses them is created again exactly as the game asked, and stays native.
        auto edit = gInsideOverlay ? xess_vulkan::DeviceEditPtr() : xess_vulkan::PrepareDevice(physical, info);
        const VkDeviceCreateInfo* used = xess_vulkan::EditedInfo(edit.get(), info);
        // vkhair.4: a game that enables LSS on an admitted GPU gets position fetch in its place (on top of
        // the host's edit), and its device is converted for universal hair.
        auto hair = gInsideOverlay ? rtx_hair::vk::DeviceEditPtr() : rtx_hair::vk::PrepareDevice(physical, used);
        VkResult result = original(physical, rtx_hair::vk::EditedInfo(hair.get(), used), allocator, output);
        if (result != VK_SUCCESS && (edit || hair))
        {
            // apisetgpa.1: logged, since a device that refuses the edit is otherwise silent.
            single_module::Log((L"MFG_PROXY_UI edited vkCreateDevice failed (" + std::to_wstring(static_cast<int>(result))
                + L"); creating the game's own device, provider edit=" + (edit ? L"1" : L"0")
                + L" hair edit=" + (hair ? L"1" : L"0")).c_str());
            edit.reset();
            hair.reset();
            result = original(physical, info, allocator, output);
        }
        // The first result and every failure, written directly so they are on disk if the game ends at once.
        static std::atomic<bool> firstResultLogged{false};
        if (!gInsideOverlay && (result != VK_SUCCESS || !firstResultLogged.exchange(true)))
            single_module::Log((L"MFG_PROXY_UI vkCreateDevice returned " + std::to_wstring(static_cast<int>(result))).c_str());
        if (!gInsideOverlay && result == VK_SUCCESS && output)
        {
            graphics_api::Note(graphics_api::kVulkan); // dx11menu.1: bookkeeping only
            rtx_hair::universal::ObserveVulkanDevice(); // nulltoken.1: the hair panel's Vulkan reason
            RememberDevice(physical, *output);
            if (edit) xess_vulkan::DeviceCreated(std::move(edit), *output);
            if (hair) rtx_hair::vk::DeviceCreated(std::move(hair), *output);
        }
        return result;
    }
};
struct GetQueueTag
{
    static void Call(PFN_vkGetDeviceQueue original, VkDevice device, uint32_t family, uint32_t index, VkQueue* output)
    {
        original(device, family, index, output);
        if (!gInsideOverlay && output) RememberQueue(device, family, false, *output);
        if (!gInsideOverlay && output && *output) xess_vulkan::QueueSeen(device, family, index, *output);
    }
};
struct GetQueue2Tag
{
    static void Call(PFN_vkGetDeviceQueue2 original, VkDevice device,
        const VkDeviceQueueInfo2* info, VkQueue* output)
    {
        original(device, info, output);
        if (!gInsideOverlay && output && info)
            RememberQueue(device, info->queueFamilyIndex, (info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) != 0, *output);
        if (!gInsideOverlay && output && *output && info && !info->flags)
            xess_vulkan::QueueSeen(device, info->queueFamilyIndex, info->queueIndex, *output);
    }
};
struct CreateSemaphoreTag
{
    static VkResult Call(PFN_vkCreateSemaphore original, VkDevice device,
        const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks* allocator, VkSemaphore* output)
    {
        const VkResult result = original(device, info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && info && output)
        {
            std::lock_guard lock(gRegistryMutex);
            if (!info->pNext && !info->flags && gDevices.contains(device) && gNativeSemaphores.size() < 8192)
                gNativeSemaphores[*output] = device;
        }
        return result;
    }
};
struct DestroySemaphoreTag
{
    static void Call(PFN_vkDestroySemaphore original, VkDevice device,
        VkSemaphore semaphore, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            gNativeSemaphores.erase(semaphore);
        }
        original(device, semaphore, allocator);
    }
};
struct ImportSemaphoreTag
{
    static VkResult Call(PFN_vkImportSemaphoreWin32HandleKHR original, VkDevice device,
        const VkImportSemaphoreWin32HandleInfoKHR* info)
    {
        if (!gInsideOverlay && info)
        {
            std::lock_guard lock(gRegistryMutex);
            gNativeSemaphores.erase(info->semaphore);
        }
        return original(device, info);
    }
};
struct CreateSurfaceTag
{
    static VkResult Call(PFN_vkCreateWin32SurfaceKHR original, VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* output)
    {
        const VkResult result = original(instance, info, allocator, output);
        if (!gInsideOverlay && result == VK_SUCCESS && output && info)
        {
            std::lock_guard lock(gRegistryMutex);
            if (gSurfaces.size() < 64) gSurfaces[*output] = Surface{info->hwnd, instance};
        }
        if (!gInsideOverlay && result == VK_SUCCESS && output && info) xess_vulkan::SurfaceCreated(instance, *output, info->hwnd);
        return result;
    }
};
// VK_EXT_swapchain_maintenance1 (newer than the pinned headers): the chain's
// own present-mode list constrains presentMode and lets each present switch.
constexpr VkStructureType kSwapchainPresentModesCreateInfo = static_cast<VkStructureType>(1000275002);

// The modes the creation's surface lists, for the V-Sync Off choice. A
// full-screen exclusive creation is asked about in that exact state.
vsync_control::VulkanSurfaceModes SurfacePresentModes(const Device& device, const VkSwapchainCreateInfoKHR& info)
{
    vsync_control::VulkanSurfaceModes modes{};
    modes.queried = true;
    modes.unsupported = true;
    const VkSurfaceFullScreenExclusiveInfoEXT* exclusive = nullptr;
    const VkSurfaceFullScreenExclusiveWin32InfoEXT* monitor = nullptr;
    unsigned links = 0;
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
    {
        if (++links > 64 || next->sType == kSwapchainPresentModesCreateInfo) return modes;
        if (next->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT)
            exclusive = reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(next);
        else if (next->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT)
            monitor = reinterpret_cast<const VkSurfaceFullScreenExclusiveWin32InfoEXT*>(next);
    }
    InternalScope internal;
    const auto& instance = *device.instance;
    std::array<VkPresentModeKHR, 32> list{};
    uint32_t count = 0;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (exclusive || monitor)
    {
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT>(
            instance.gipa(instance.handle, "vkGetPhysicalDeviceSurfacePresentModes2EXT"));
        if (!query) return modes;
        VkSurfaceFullScreenExclusiveWin32InfoEXT monitorCopy{};
        VkSurfaceFullScreenExclusiveInfoEXT exclusiveCopy{};
        VkPhysicalDeviceSurfaceInfo2KHR surface{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        surface.surface = info.surface;
        if (monitor)
        {
            monitorCopy = *monitor;
            monitorCopy.pNext = nullptr;
            surface.pNext = &monitorCopy;
        }
        if (exclusive)
        {
            exclusiveCopy = *exclusive;
            exclusiveCopy.pNext = const_cast<void*>(surface.pNext);
            surface.pNext = &exclusiveCopy;
        }
        result = query(device.physical, &surface, &count, nullptr);
        if (result == VK_SUCCESS && count && count <= list.size())
            result = query(device.physical, &surface, &count, list.data());
    }
    else
    {
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            instance.gipa(instance.handle, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
        if (!query) return modes;
        result = query(device.physical, info.surface, &count, nullptr);
        if (result == VK_SUCCESS && count && count <= list.size())
            result = query(device.physical, info.surface, &count, list.data());
    }
    if (result != VK_SUCCESS || !count || count > list.size()) return modes;
    modes.unsupported = false;
    for (uint32_t index = 0; index < count; ++index)
    {
        modes.immediate |= list[index] == VK_PRESENT_MODE_IMMEDIATE_KHR;
        modes.mailbox |= list[index] == VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return modes;
}

struct CreateSwapchainTag
{
    static VkResult Call(PFN_vkCreateSwapchainKHR original, VkDevice handle,
        const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* output)
    {
        if (gInsideOverlay || !info) return original(handle, info, allocator, output);
        auto device = FindDevice(handle);
        VkSwapchainCreateInfoKHR adjusted = *info;
        if (device && !(adjusted.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        {
            InternalScope internal;
            auto capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                device->instance->gipa(device->instance->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
            VkSurfaceCapabilitiesKHR support{};
            if (capabilities && capabilities(device->physical, info->surface, &support) == VK_SUCCESS
                && (support.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                adjusted.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        // V-Sync Off: the present mode is fixed here, so a change applies to
        // the game's next swapchain. Only presentMode changes, and only to a
        // mode the surface lists; Game leaves it untouched (vsync_control.h).
        vsync_control::VulkanSwapchainSubmission vsync{};
        if (device)
        {
            vsync = vsync_control::BeforeVulkanSwapchain(static_cast<uint32_t>(info->presentMode),
                vsync_control::OffRequested() ? SurfacePresentModes(*device, *info)
                    : vsync_control::VulkanSurfaceModes{});
            adjusted.presentMode = static_cast<VkPresentModeKHR>(vsync.presentMode);
        }
        VkResult result;
        {
            // Windows Vulkan drivers may build an internal DXGI swapchain.
            // It belongs to Vulkan WSI, not the application's D3D12 renderer.
            InternalScope internal;
            // The XeMFG Vulkan host may take the chain over (the handle is then its own).
            if (!device || !xess_vulkan::CreateSwapchain(handle, &adjusted, allocator, output, original, result))
                result = original(handle, &adjusted, allocator, output);
        }
        if (device)
            vsync_control::AfterVulkanSwapchain(vsync, result,
                result == VK_SUCCESS && output ? reinterpret_cast<uint64_t>(*output) : 0);
        if (result == VK_SUCCESS && output && device)
        {
            // Match the rebuilt DXGI path's bounded input setup. No graphics
            // work or waits occur here, and no Vulkan executable is patched.
            if (!install::PrepareInput()) return result;
            install::VulkanReady();
            ui_input_coherence::InvalidateResources();
            auto state = std::make_shared<State>();
            state->device = device;
            state->swapchain = *output;
            state->surface = info->surface;
            state->format = info->imageFormat;
            state->colorSpace = info->imageColorSpace;
            state->extent = info->imageExtent;
            state->usage = adjusted.imageUsage;
            state->flags = info->flags;
            state->layers = info->imageArrayLayers;
            state->minImageCount = info->minImageCount;
            state->sharingMode = info->imageSharingMode;
            if (info->queueFamilyIndexCount <= 64 && info->queueFamilyIndexCount && info->pQueueFamilyIndices)
                state->sharingFamilies.assign(info->pQueueFamilyIndices, info->pQueueFamilyIndices+info->queueFamilyIndexCount);
            std::lock_guard lock(gRegistryMutex);
            const auto surface = gSurfaces.find(info->surface);
            if (surface != gSurfaces.end() && surface->second.instance == device->instance->handle)
                state->window = surface->second.window;
            if (gSwapchains.size() < 64) gSwapchains[*output] = std::move(state);
        }
        return result;
    }
};

struct PresentTag
{
    static VkResult Call(PFN_vkQueuePresentKHR original, VkQueue handle, const VkPresentInfoKHR* info)
    {
        if (gInsideOverlay || !info || !info->swapchainCount || !info->pSwapchains || !info->pImageIndices)
            return original(handle, info);
        Queue queue;
        std::shared_ptr<State> selected;
        uint32_t selectedIndex = 0;
        bool nativeWaits = gSemaphoreCoverage.load(std::memory_order_acquire)
            && info->waitSemaphoreCount > 0 && info->waitSemaphoreCount <= 64 && info->pWaitSemaphores;
        {
            std::lock_guard lock(gRegistryMutex);
            const auto foundQueue = gQueues.find(handle);
            if (foundQueue != gQueues.end()) queue = foundQueue->second;
            if (queue.device && nativeWaits)
                for (uint32_t index = 0; index < info->waitSemaphoreCount; ++index)
                {
                    const auto semaphore = gNativeSemaphores.find(info->pWaitSemaphores[index]);
                    if (semaphore == gNativeSemaphores.end() || semaphore->second != queue.device->handle)
                    { nativeWaits = false; break; }
                }
            if (queue.device)
                for (uint32_t index = 0; index < info->swapchainCount; ++index)
                {
                    const auto found = gSwapchains.find(info->pSwapchains[index]);
                    if (found != gSwapchains.end() && found->second->device == queue.device)
                    {
                        selected = found->second;
                        selectedIndex = index;
                        break;
                    }
                }
        }
        VkResult renderFailure = VK_SUCCESS;
        bool supportedPresent = true;
        // Preserve arbitrary extensions verbatim. Rendering with incremental
        // regions or device-group masks needs additional image ownership proof.
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR
                || next->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR)
                supportedPresent = false;
        // One menu per window. OptiScaler's XeFG-on-Vulkan interop presents this chain's image through
        // XeFG's D3D12 output chain on the same window, whose output layer draws the menu after frame generation; a
        // menu drawn here would be in the game's frame that XeFG interpolates.
        if (selected && supportedPresent && nativeWaits && single_module::OverlayAllowed()
            && !dx12::WindowSession(selected->window))
        {
            gVulkanFrames.fetch_add(1, std::memory_order_relaxed);
            selected->Render(queue, info->pImageIndices[selectedIndex], *info, renderFailure);
        }
        if (renderFailure != VK_SUCCESS)
        {
            if (info->pResults)
                std::fill_n(info->pResults, info->swapchainCount, renderFailure);
            return renderFailure;
        }
        // The device dispatch may re-enter the loader entry. Only one layer
        // may consume or substitute this call's original semaphore set.
        InternalScope internal;
        VulkanPresentForwardingScope forwarding;
        // A taken-over chain presents through XeFG (after the menu was drawn into the game's image).
        VkResult hosted = VK_SUCCESS;
        if (xess_vulkan::QueuePresent(handle, info, hosted))
        {
            if (selected && (hosted == VK_SUCCESS || hosted == VK_SUBOPTIMAL_KHR))
                frame_telemetry::SampleVulkanPresent();
            return hosted;
        }
        const VkResult result = original(handle, info);
        if (selected && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
            frame_telemetry::SampleVulkanPresent();
        return xess_vulkan::AfterNativePresent(handle, info, result);
    }
};

void ForgetSwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    std::shared_ptr<State> state;
    {
        std::lock_guard lock(gRegistryMutex);
        const auto found = gSwapchains.find(swapchain);
        if (found != gSwapchains.end() && found->second->device->handle == device)
        {
            state = found->second;
            gSwapchains.erase(found);
        }
    }
    if (state)
    {
        ui_input_coherence::InvalidateResources();
        std::lock_guard lock(state->mutex);
        if (!state->Shutdown())
        {
            std::lock_guard registryLock(gRegistryMutex);
            gRetained.push_back(std::move(state));
        }
    }
}
struct DestroySwapchainTag
{
    static void Call(PFN_vkDestroySwapchainKHR original, VkDevice device,
        VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            ForgetSwapchain(device, swapchain);
            vsync_control::VulkanSwapchainDestroyed(reinterpret_cast<uint64_t>(swapchain));
            InternalScope internal;
            if (xess_vulkan::DestroySwapchain(device, swapchain)) return;
        }
        original(device, swapchain, allocator);
    }
};
struct DestroyDeviceTag
{
    static void Call(PFN_vkDestroyDevice original, VkDevice device, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::vector<VkSwapchainKHR> chains;
            {
                std::lock_guard lock(gRegistryMutex);
                for (const auto& [chain, state] : gSwapchains)
                    if (state->device->handle == device) chains.push_back(chain);
            }
            for (auto chain : chains)
            {
                ForgetSwapchain(device, chain);
                vsync_control::VulkanSwapchainDestroyed(reinterpret_cast<uint64_t>(chain));
            }
            {
                std::lock_guard lock(gRegistryMutex);
                std::erase_if(gQueues, [&](const auto& row) { return row.second.device->handle == device; });
                std::erase_if(gNativeSemaphores, [&](const auto& row) { return row.second == device; });
                gDevices.erase(device);
            }
            InternalScope internal;
            xess_vulkan::DeviceDestroying(device);
            rtx_hair::vk::DeviceDestroyed(device); // vkhair.4: its converter buffers and pipelines
        }
        original(device, allocator);
    }
};
struct DestroySurfaceTag
{
    static void Call(PFN_vkDestroySurfaceKHR original, VkInstance instance,
        VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            gSurfaces.erase(surface);
        }
        if (!gInsideOverlay) xess_vulkan::SurfaceDestroyed(surface);
        original(instance, surface, allocator);
    }
};
struct DestroyInstanceTag
{
    static void Call(PFN_vkDestroyInstance original, VkInstance instance, const VkAllocationCallbacks* allocator)
    {
        if (!gInsideOverlay)
        {
            std::lock_guard lock(gRegistryMutex);
            std::erase_if(gPhysicalDevices, [&](const auto& row) { return row.second->handle == instance; });
            gInstances.erase(instance);
        }
        if (!gInsideOverlay) xess_vulkan::InstanceDestroyed(instance);
        if (!gInsideOverlay) rtx_hair::vk::InstanceDestroyed(instance);
        original(instance, allocator);
    }
};

// vkDeviceWaitIdle synchronizes every queue of the device, the XeMFG host's layer queues included.
struct DeviceWaitIdleTag
{
    static VkResult Call(PFN_vkDeviceWaitIdle original, VkDevice device)
    {
        VkResult result = VK_SUCCESS;
        if (!gInsideOverlay && xess_vulkan::DeviceWaitIdle(device, result)) return result;
        return original(device);
    }
};
// The swap-chain entry points a taken-over chain's handle reaches. Native handles pass straight on.
struct GetSwapchainImagesTag
{
    static VkResult Call(PFN_vkGetSwapchainImagesKHR original, VkDevice device, VkSwapchainKHR swapchain,
        uint32_t* count, VkImage* images)
    {
        VkResult result = VK_SUCCESS;
        if (!gInsideOverlay && xess_vulkan::GetSwapchainImages(device, swapchain, count, images, result)) return result;
        return original(device, swapchain, count, images);
    }
};
struct AcquireNextImageTag
{
    static VkResult Call(PFN_vkAcquireNextImageKHR original, VkDevice device, VkSwapchainKHR swapchain,
        uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index)
    {
        VkResult result = VK_SUCCESS;
        if (!gInsideOverlay)
        {
            InternalScope internal;
            if (xess_vulkan::AcquireNextImage(device, swapchain, timeout, semaphore, fence, index, result)) return result;
        }
        return original(device, swapchain, timeout, semaphore, fence, index);
    }
};
struct AcquireNextImage2Tag
{
    static VkResult Call(PFN_vkAcquireNextImage2KHR original, VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index)
    {
        VkResult result = VK_SUCCESS;
        if (!gInsideOverlay && info)
        {
            InternalScope internal;
            if (xess_vulkan::AcquireNextImage(device, info->swapchain, info->timeout, info->semaphore, info->fence, index, result))
                return result;
        }
        return original(device, info, index);
    }
};
struct SetHdrMetadataTag
{
    static void Call(PFN_vkSetHdrMetadataEXT original, VkDevice device, uint32_t count,
        const VkSwapchainKHR* swapchains, const VkHdrMetadataEXT* metadata)
    {
        if (!gInsideOverlay && count && swapchains && metadata)
        {
            std::vector<VkSwapchainKHR> native;
            std::vector<VkHdrMetadataEXT> nativeMetadata;
            for (uint32_t i = 0; i < count; ++i)
                if (!xess_vulkan::SetHdrMetadata(device, swapchains[i], &metadata[i]))
                {
                    native.push_back(swapchains[i]);
                    nativeMetadata.push_back(metadata[i]);
                }
            if (!native.empty()) original(device, uint32_t(native.size()), native.data(), nativeMetadata.data());
            return;
        }
        original(device, count, swapchains, metadata);
    }
};

PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original)
{
    if (!name || !original) return original;
    // vkhair.4: universal hair's entries, whatever else the overlay wraps; each passes through on devices it
    // does not convert.
    if (const auto hair = rtx_hair::vk::Wrap(name, original); hair != original) return hair;
    // vkprov.2: beside OptiScaler's own menu the provider's route wraps the instance and device lifecycle only;
    // swapchain, present, surface and semaphore entries stay the game's, as without the provider.
    if (gLifecycleOnly.load(std::memory_order_acquire) && !single_module::OverlayAllowed() && !LifecycleEntry(name))
        return original;
    // A missed destroy/import could leave a stale or replaced payload marked
    // native. Disable this route if either callable cannot be intercepted.
#define MFG_VK_SEMAPHORE_WRAP(nameLiteral, Tag, Result, ...) \
    if (strcmp(name, nameLiteral) == 0) { \
        const auto wrapped = reinterpret_cast<PFN_vkVoidFunction>( \
            HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false)); \
        if (wrapped == original) gSemaphoreCoverage.store(false, std::memory_order_release); \
        return wrapped; }
    MFG_VK_SEMAPHORE_WRAP("vkDestroySemaphore", DestroySemaphoreTag, void, VkDevice, VkSemaphore, const VkAllocationCallbacks*)
    MFG_VK_SEMAPHORE_WRAP("vkImportSemaphoreWin32HandleKHR", ImportSemaphoreTag, VkResult, VkDevice, const VkImportSemaphoreWin32HandleInfoKHR*)
#undef MFG_VK_SEMAPHORE_WRAP
    // The V-Sync status distinguishes a missing swapchain from a missing route.
    if (strcmp(name, "vkCreateSwapchainKHR") == 0)
    {
        const auto wrapped = reinterpret_cast<PFN_vkVoidFunction>(
            HookFamily<CreateSwapchainTag, VkResult, VkDevice, const VkSwapchainCreateInfoKHR*,
                const VkAllocationCallbacks*, VkSwapchainKHR*>::Bind(reinterpret_cast<void*>(original), false));
        if (wrapped != original) vsync_control::VulkanSwapchainHookInstalled();
        if (wrapped != original) xess_vulkan::EntryWrapped("vkCreateSwapchainKHR");
        return wrapped;
    }
    // A taken-over chain needs every entry point its handle reaches to be ours.
    for (const char* hosted : {"vkQueuePresentKHR", "vkDestroySwapchainKHR"})
        if (strcmp(name, hosted) == 0)
        {
            const auto wrapped = strcmp(name, "vkQueuePresentKHR") == 0
                ? reinterpret_cast<PFN_vkVoidFunction>(HookFamily<PresentTag, VkResult, VkQueue, const VkPresentInfoKHR*>::Bind(
                    reinterpret_cast<void*>(original), false))
                : reinterpret_cast<PFN_vkVoidFunction>(HookFamily<DestroySwapchainTag, void, VkDevice, VkSwapchainKHR,
                    const VkAllocationCallbacks*>::Bind(reinterpret_cast<void*>(original), false));
            if (wrapped != original) xess_vulkan::EntryWrapped(hosted);
            return wrapped;
        }
#define MFG_VK_WRAP(nameLiteral, Tag, Result, ...) \
    if (strcmp(name, nameLiteral) == 0) return reinterpret_cast<PFN_vkVoidFunction>( \
        HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false));
    MFG_VK_WRAP("vkGetInstanceProcAddr", GipaTag, PFN_vkVoidFunction, VkInstance, const char*)
    MFG_VK_WRAP("vkGetDeviceProcAddr", GdpaTag, PFN_vkVoidFunction, VkDevice, const char*)
    MFG_VK_WRAP("vkCreateInstance", CreateInstanceTag, VkResult, const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*)
    MFG_VK_WRAP("vkEnumeratePhysicalDevices", EnumeratePhysicalTag, VkResult, VkInstance, uint32_t*, VkPhysicalDevice*)
    MFG_VK_WRAP("vkCreateDevice", CreateDeviceTag, VkResult, VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*)
    MFG_VK_WRAP("vkGetDeviceQueue", GetQueueTag, void, VkDevice, uint32_t, uint32_t, VkQueue*)
    MFG_VK_WRAP("vkGetDeviceQueue2", GetQueue2Tag, void, VkDevice, const VkDeviceQueueInfo2*, VkQueue*)
    MFG_VK_WRAP("vkCreateSemaphore", CreateSemaphoreTag, VkResult, VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*)
    MFG_VK_WRAP("vkCreateWin32SurfaceKHR", CreateSurfaceTag, VkResult, VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*)
    MFG_VK_WRAP("vkQueuePresentKHR", PresentTag, VkResult, VkQueue, const VkPresentInfoKHR*)
    MFG_VK_WRAP("vkDestroySwapchainKHR", DestroySwapchainTag, void, VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroyDevice", DestroyDeviceTag, void, VkDevice, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroySurfaceKHR", DestroySurfaceTag, void, VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*)
    MFG_VK_WRAP("vkDestroyInstance", DestroyInstanceTag, void, VkInstance, const VkAllocationCallbacks*)
#define MFG_VK_HOST_WRAP(nameLiteral, Tag, Result, ...) \
    if (strcmp(name, nameLiteral) == 0) { \
        const auto wrapped = reinterpret_cast<PFN_vkVoidFunction>( \
            HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false)); \
        if (wrapped != original) xess_vulkan::EntryWrapped(nameLiteral); \
        return wrapped; }
    MFG_VK_HOST_WRAP("vkGetSwapchainImagesKHR", GetSwapchainImagesTag, VkResult, VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)
    MFG_VK_HOST_WRAP("vkAcquireNextImageKHR", AcquireNextImageTag, VkResult, VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)
    MFG_VK_HOST_WRAP("vkAcquireNextImage2KHR", AcquireNextImage2Tag, VkResult, VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*)
    MFG_VK_HOST_WRAP("vkSetHdrMetadataEXT", SetHdrMetadataTag, void, VkDevice, uint32_t, const VkSwapchainKHR*, const VkHdrMetadataEXT*)
    MFG_VK_HOST_WRAP("vkDeviceWaitIdle", DeviceWaitIdleTag, VkResult, VkDevice)
#undef MFG_VK_HOST_WRAP
#undef MFG_VK_WRAP
    return original;
}
}

bool AnySession() noexcept
{
    std::lock_guard lock(gRegistryMutex);
    return !gSwapchains.empty();
}

FARPROC Resolve(HMODULE module, LPCSTR name, FARPROC original) noexcept
{
    if (!name || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !original
        || name[0] != 'v' || name[1] != 'k') return original;
    // Establish the public loader ABI in its exact image. All downstream
    // layer/driver functions still come from that loader's GIPA/GDPA chain.
    // Bind(false) pins each returned callable and never changes its code.
    wchar_t path[32768]{};
    const DWORD length = module ? GetModuleFileNameW(module, path, 32768) : 0;
    if (!length || length >= 32768) return original;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    if (_wcsicmp(leaf ? leaf+1 : path, L"vulkan-1.dll")
        || !slots::ImageEntry(module, reinterpret_cast<void*>(original))) return original;
    for (const char* symbol : {"vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
            "vkCreateInstance", "vkEnumeratePhysicalDevices", "vkCreateDevice"})
        if (!slots::ImageEntry(module, reinterpret_cast<void*>(GetProcAddress(module, symbol))))
            return original;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(original), &pinned) || pinned != module) return original;
    auto loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    auto expected = static_cast<PFN_vkGetInstanceProcAddr>(nullptr);
    if (!gLoaderGipa.compare_exchange_strong(expected, loader, std::memory_order_acq_rel)
        && expected != loader) return original; // Never mix loader ownership.
    const auto wrapped = reinterpret_cast<FARPROC>(Wrap(name, reinterpret_cast<PFN_vkVoidFunction>(original)));
    if (wrapped != original) gVulkanHooked.store(true, std::memory_order_release);
    return wrapped;
}

FARPROC ResolveLifecycle(HMODULE module, LPCSTR name, FARPROC original) noexcept
{
    if (!name || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !original || !LifecycleEntry(name)
        || !xess_vulkan_provider::ShimInstalled()) return original;
    gLifecycleOnly.store(true, std::memory_order_release);
    const FARPROC wrapped = Resolve(module, name, original);
    if (wrapped != original && !gLifecycleLogged.exchange(true, std::memory_order_acq_rel))
        single_module::Log(L"MFG_PROXY_UI Vulkan instance and device entries wrapped for the XeMFG DLSS-G provider "
                           L"beside OptiScaler's menu (no RTXMFG Vulkan overlay)");
    return wrapped;
}
}
