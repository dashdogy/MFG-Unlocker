// vkdlss5.1 real-GPU check (local tool, not shipped): DLSS 5 on Vulkan through MFG Unlocker. MFG Unlocker loads as
// this process's winmm.dll; the driver's NGX runtime runs DLSS Super Resolution on an NVIDIA Vulkan device (NGX
// project-ID Init, CreateFeature1); DLSS 5 is switched through RTXMFG-Universal.json while the feature is evaluated,
// and the DLSS output is read back after each phase.
//
// Phases: A DLSS 5 off; B on at the requested Hook Method (After upscaling by default); C Hook Method Present (no
// Vulkan stage: the output must stay SR's and the status report "Hook method unavailable"); D off again; then the SR
// feature is released and the stage's feature must go with it. Exit 0 = every check passed, 1 = a check failed,
// 2 = setup failed, 3 = DLSS 5 never ran (the status names the reason).
//
// Usage: Dlss5VkHarness [frames=N] [stage=0|2] [passes=N] [scale=P] [model=N] [validation] [dump] [create0]
#include <Windows.h>
#include <mmsystem.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include "harness_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace harness;

namespace
{
std::mutex gLogMutex;
FILE* gNgxFile = nullptr;
std::set<std::string> gValidation;
unsigned gValidationCount = 0;

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::lock_guard lock(gLogMutex);
    if (gNgxFile) { fputs(message ? message : "", gNgxFile); fflush(gNgxFile); }
}

VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT || !data || !data->pMessage) return VK_FALSE;
    std::lock_guard lock(gLogMutex);
    ++gValidationCount;
    const std::string id = data->pMessageIdName ? data->pMessageIdName : "?";
    if (gValidation.insert(id).second) printf("VALIDATION %s %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
        data->pMessage);
    return VK_FALSE;
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    NVSDK_NGX_Resource_VK resource{};
};

struct Vulkan
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
};

uint32_t MemoryType(const Vulkan& vk, uint32_t bits, VkMemoryPropertyFlags wanted)
{
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}

bool MakeImage(Vulkan& vk, Image& out, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(vk.device, out.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(vk.device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(vk.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(vk.device, &view, nullptr, &out.view) != VK_SUCCESS) return false;
    out.width = width; out.height = height;
    out.resource = NVSDK_NGX_Create_ImageView_Resource_VK(out.view, out.image, view.subresourceRange, format, width, height, readWrite);
    return true;
}

struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; };
bool MakeBuffer(Vulkan& vk, Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage;
    if (vkCreateBuffer(vk.device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(vk.device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(vk, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    return allocate.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(vk.device, &allocate, nullptr, &out.memory) == VK_SUCCESS
        && vkBindBufferMemory(vk.device, out.buffer, out.memory, 0) == VK_SUCCESS
        && vkMapMemory(vk.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

bool Begin(Vulkan& vk)
{
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkResetCommandBuffer(vk.command, 0) == VK_SUCCESS && vkBeginCommandBuffer(vk.command, &begin) == VK_SUCCESS;
}

bool Submit(Vulkan& vk)
{
    if (vkEndCommandBuffer(vk.command) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &vk.command;
    if (vkResetFences(vk.device, 1, &vk.fence) != VK_SUCCESS) return false;
    if (vkQueueSubmit(vk.queue, 1, &submit, vk.fence) != VK_SUCCESS) return false;
    return vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
}

void Transition(Vulkan& vk, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from; barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(vk.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}

int main(int argc, char** argv)
{
    timeBeginPeriod(1);
    unsigned frames = 90;
    Nr nr{};
    bool validation = false, dump = false, create0 = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strncmp(argv[i], "frames=", 7)) frames = static_cast<unsigned>(atoi(argv[i] + 7));
        else if (!strncmp(argv[i], "stage=", 6)) nr.stage = static_cast<unsigned>(atoi(argv[i] + 6));
        else if (!strncmp(argv[i], "passes=", 7)) nr.passes = static_cast<unsigned>(atoi(argv[i] + 7));
        else if (!strncmp(argv[i], "scale=", 6)) nr.scale = static_cast<float>(atof(argv[i] + 6));
        else if (!strncmp(argv[i], "model=", 6)) nr.model = static_cast<unsigned>(atoi(argv[i] + 6));
        else if (!strncmp(argv[i], "extra=", 6)) nr.extra = ExtraFields(argv[i] + 6);
        else if (!strcmp(argv[i], "validation")) validation = true;
        else if (!strcmp(argv[i], "dump")) dump = true;
        else if (!strcmp(argv[i], "create0")) create0 = true;
    }
    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(L"winmm.dll"), modPath, MAX_PATH);
    printf("winmm.dll = %ls\nframes %u stage %u passes %u scale %.0f model %u validation %d create%s extra '%s'\n", modPath, frames, nr.stage,
        nr.passes, nr.scale, nr.model, validation, create0 ? "" : "1", nr.extra.c_str());
    if (!WriteControl(Nr{})) { printf("SETUP control write failed\n"); return 2; }

    Vulkan vk{};
    unsigned instanceCount = 0, deviceCount = 0;
    const char** instanceExtensions = nullptr;
    const char** deviceExtensions = nullptr;
    NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instanceExtensions, &deviceCount, &deviceExtensions);
    std::vector<const char*> instanceNames(instanceExtensions, instanceExtensions + instanceCount);
    std::vector<const char*> layers;
    if (validation) { layers.push_back("VK_LAYER_KHRONOS_validation"); instanceNames.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "dlss5_vk_harness"; application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &application;
    instance.enabledExtensionCount = static_cast<uint32_t>(instanceNames.size()); instance.ppEnabledExtensionNames = instanceNames.data();
    instance.enabledLayerCount = static_cast<uint32_t>(layers.size()); instance.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&instance, nullptr, &vk.instance) != VK_SUCCESS) { printf("SETUP vkCreateInstance\n"); return 2; }
    if (validation)
    {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        m.pfnUserCallback = Validation;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(vk.instance, &m, nullptr, &vk.messenger);
    }
    uint32_t physicalCount = 0;
    vkEnumeratePhysicalDevices(vk.instance, &physicalCount, nullptr);
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    vkEnumeratePhysicalDevices(vk.instance, &physicalCount, physicals.data());
    for (VkPhysicalDevice physical : physicals)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.vendorID == 0x10DE && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            vk.physical = physical;
            printf("adapter: %s (driver 0x%X)\n", properties.deviceName, properties.driverVersion);
            break;
        }
    }
    if (!vk.physical) { printf("SETUP no NVIDIA Vulkan device\n"); return 2; }
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { vk.family = i; break; }
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features11;
    // Only the 1.2 features NGX needs, as a game would enable them (no storage-without-format features).
    VkPhysicalDeviceVulkan12Features wanted12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vkGetPhysicalDeviceFeatures2(vk.physical, &features);
    // NGX lists VK_EXT_buffer_device_address but allocates with the core flag: on Vulkan 1.2 the core feature serves both
    // (with the EXT also enabled, VUID-VkDeviceCreateInfo-pNext-04748; with the EXT feature alone, the allocations fail it).
    std::vector<const char*> deviceNames;
    for (unsigned i = 0; i < deviceCount; ++i)
        if (strcmp(deviceExtensions[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) deviceNames.push_back(deviceExtensions[i]);
    wanted12.bufferDeviceAddress = features12.bufferDeviceAddress; wanted12.timelineSemaphore = features12.timelineSemaphore;
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.pNext = &wanted12;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = vk.family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    for (const char* name : deviceNames) printf("  device extension %s\n", name);
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pNext = &enabled; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = static_cast<uint32_t>(deviceNames.size()); device.ppEnabledExtensionNames = deviceNames.data();
    if (vkCreateDevice(vk.physical, &device, nullptr, &vk.device) != VK_SUCCESS) { printf("SETUP vkCreateDevice\n"); return 2; }
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = vk.family;
    vkCreateCommandPool(vk.device, &pool, nullptr, &vk.pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = vk.pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk.device, &allocate, &vk.command);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vk.device, &fence, nullptr, &vk.fence);

    const std::wstring dataPath = Directory();
    gNgxFile = _wfopen((dataPath + L"\\ngx-callback.log").c_str(), L"wb");
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID("7f4b5c2e-91d3-4a60-b8e7-3c5d0a1f9e24", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
        dataPath.c_str(), vk.instance, vk.physical, vk.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
    printf("NGX init 0x%08X\n", static_cast<unsigned>(result));
    if (NVSDK_NGX_FAILED(result)) return 2;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
    int available = 0;
    if (parameters) parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    printf("SuperSampling.Available=%d\n", available);
    if (!available) return 2;

    const unsigned outWidth = 2560, outHeight = 1440, renderWidth = 1707, renderHeight = 960;
    Image color, depth, motion, output;
    if (!MakeImage(vk, color, renderWidth, renderHeight, VK_FORMAT_R16G16B16A16_SFLOAT, false)
        || !MakeImage(vk, depth, renderWidth, renderHeight, VK_FORMAT_R32_SFLOAT, false)
        || !MakeImage(vk, motion, renderWidth, renderHeight, VK_FORMAT_R16G16_SFLOAT, false)
        || !MakeImage(vk, output, outWidth, outHeight, VK_FORMAT_R16G16B16A16_SFLOAT, true))
    { printf("SETUP images\n"); return 2; }
    Buffer upload, readback;
    const auto scene = Scene(renderWidth, renderHeight);
    if (!MakeBuffer(vk, upload, scene.size() * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
        || !MakeBuffer(vk, readback, VkDeviceSize(outWidth) * outHeight * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
    { printf("SETUP buffers\n"); return 2; }
    std::memcpy(upload.mapped, scene.data(), scene.size() * 2);
    // Inputs as a game hands them to DLSS (Streamline): SHADER_READ_ONLY_OPTIMAL; the output in GENERAL.
    if (!Begin(vk)) return 2;
    for (Image* image : {&color, &depth, &motion}) Transition(vk, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Transition(vk, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {renderWidth, renderHeight, 1};
    vkCmdCopyBufferToImage(vk.command, upload.buffer, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkClearColorValue half{{0.5f, 0.5f, 0.5f, 0.5f}}, zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.command, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &half, 1, &range);
    vkCmdClearColorImage(vk.command, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    vkCmdClearColorImage(vk.command, output.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    for (Image* image : {&color, &depth, &motion}) Transition(vk, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!Submit(vk)) { printf("SETUP upload\n"); return 2; }

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = renderWidth; create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = outWidth; create.Feature.InTargetHeight = outHeight;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    NVSDK_NGX_Handle* handle = nullptr;
    if (!Begin(vk)) return 2;
    result = NGX_VULKAN_CREATE_DLSS_EXT1(create0 ? VK_NULL_HANDLE : vk.device, vk.command, 1, 1, &handle, parameters, &create);
    const bool createSubmitted = Submit(vk);
    printf("SR CREATE 0x%08X handle=%p submitted=%d\n", static_cast<unsigned>(result), static_cast<void*>(handle), createSubmitted);
    if (NVSDK_NGX_FAILED(result) || !handle || !createSubmitted) return 2;

    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &color.resource; eval.Feature.pInOutput = &output.resource;
    eval.pInDepth = &depth.resource; eval.pInMotionVectors = &motion.resource;
    eval.InRenderSubrectDimensions = {renderWidth, renderHeight};
    unsigned evaluateFailures = 0, submitFailures = 0, frameIndex = 0;
    const auto run = [&](unsigned count)
    {
        for (unsigned i = 0; i != count; ++i, ++frameIndex)
        {
            // A converging static scene: a small fixed jitter cycle.
            eval.InJitterOffsetX = (frameIndex % 2) ? 0.25f : -0.25f;
            eval.InJitterOffsetY = (frameIndex % 3) ? 0.25f : -0.25f;
            if (!Begin(vk)) { ++submitFailures; continue; }
            evaluateFailures += NVSDK_NGX_FAILED(NGX_VULKAN_EVALUATE_DLSS_EXT(vk.command, handle, parameters, &eval));
            submitFailures += !Submit(vk);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    };
    const auto read = [&](const char* name)
    {
        std::vector<float> texels(size_t(outWidth) * outHeight * 4);
        if (!Begin(vk)) return texels;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(vk.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {outWidth, outHeight, 1};
        vkCmdCopyImageToBuffer(vk.command, output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
        if (!Submit(vk)) { printf("readback submit failed\n"); return texels; }
        Decode(static_cast<const uint16_t*>(readback.mapped), texels, dump, name);
        return texels;
    };
    bool ok = true;
    const auto check = [&ok](bool condition, const char* what) { printf("%s %s\n", condition ? "PASS" : "FAIL", what); ok &= condition; };

    run(30);
    const auto a = read("a-off");
    const Status off = PrintStatus("A off");
    check(off.state == 0 && off.live <= 0, "A: DLSS 5 off, no feature");

    Nr on = nr; on.on = true;
    if (!WriteControl(on)) { printf("SETUP control write failed\n"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(frames);
    // Read at a fixed frame count (the D3D12 twin reads at the same one).
    const auto b = read("b-on");
    // The status worker writes the file on its own clock: sample it while frames still flow.
    Status running{};
    for (int sample = 0; sample < 20; ++sample)
    {
        run(4);
        running = PrintStatus("B on");
        if (running.state == 2) break;
    }
    const Stats changed = Compare(a, b);
    printf("B vs A: meanAbs=%.5f maxAbs=%.4f changed=%.2f%% nonFinite=%zu meanLuma A=%.4f B=%.4f\n", changed.meanAbs, changed.maxAbs,
        changed.changed * 100.0, changed.nonFinite, Compare(a, a).meanLuma, changed.meanLuma);
    if (running.state != 2 && running.evaluations <= 0) { printf("DLSS 5 did not run (state %lld)\n", running.state); }
    check(running.state == 2, "B: dlss5State 2 (running)");
    check(running.live == 1 && running.evaluations > 0 && running.failures == 0, "B: one live feature, evaluations, no failures");
    check(changed.nonFinite == 0 && changed.changed > 0.01 && changed.meanAbs > 1e-4, "B: the DLSS output changed, finite");
    const double lumaA = Compare(a, a).meanLuma;
    check(lumaA > 0 && changed.meanLuma > 0.4 * lumaA && changed.meanLuma < 1.6 * lumaA, "B: brightness plausible (mean luminance 40-160% of SR's)");

    // Settled: the status worker has written the last of B's evaluations.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const Status settled = PrintStatus("B settled");
    Nr present = on; present.stage = 3;
    WriteControl(present);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(30);
    const auto c = read("c-present");
    const Status unavailable = PrintStatus("C present");
    const Stats restored = Compare(a, c);
    printf("C vs A: meanAbs=%.5f changed=%.2f%%\n", restored.meanAbs, restored.changed * 100.0);
    check(unavailable.state == 10, "C: Present has no Vulkan stage (dlss5State 10)");
    check(unavailable.evaluations == settled.evaluations, "C: no DLSS 5 evaluations");

    WriteControl(Nr{});
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(30);
    const auto d = read("d-off");
    const Stats back = Compare(a, d);
    const Status offAgain = PrintStatus("D off");
    printf("D vs A: meanAbs=%.5f changed=%.2f%%\n", back.meanAbs, back.changed * 100.0);
    check(offAgain.state == 0, "D: off");
    check(back.meanAbs < changed.meanAbs * 0.25, "D: the output is SR's again");

    vkDeviceWaitIdle(vk.device);
    result = NVSDK_NGX_VULKAN_ReleaseFeature(handle);
    printf("SR RELEASE 0x%08X evaluateFailures=%u submitFailures=%u\n", static_cast<unsigned>(result), evaluateFailures, submitFailures);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const Status released = PrintStatus("released");
    check(result == NVSDK_NGX_Result_Success && !evaluateFailures && !submitFailures, "SR evaluations and release");
    check(released.live == 0, "released: the DLSS 5 feature went with the SR feature");
    if (validation) printf("validation messages %u (unique %zu)\n", gValidationCount, gValidation.size());
    printf("DONE %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (gNgxFile) fflush(gNgxFile);
    const int code = ok ? 0 : running.evaluations > 0 ? 1 : 3;
    TerminateProcess(GetCurrentProcess(), code);
    return code;
}
