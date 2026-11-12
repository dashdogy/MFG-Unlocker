#include "single_module_status.h"
#if MFG_VK_STREAMLINE
#include <sl_core_api.h>
#endif
#include <Windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;


#if MFG_VK_DYNAMIC
PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
PFN_vkAllocateMemory vkAllocateMemory = nullptr;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
PFN_vkCreateBuffer vkCreateBuffer = nullptr;
PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
PFN_vkCreateDevice vkCreateDevice = nullptr;
PFN_vkCreateFence vkCreateFence = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
PFN_vkDestroyDevice vkDestroyDevice = nullptr;
PFN_vkDestroyFence vkDestroyFence = nullptr;
PFN_vkDestroyInstance vkDestroyInstance = nullptr;
PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
PFN_vkFreeMemory vkFreeMemory = nullptr;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
PFN_vkMapMemory vkMapMemory = nullptr;
PFN_vkQueueSubmit vkQueueSubmit = nullptr;
PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
PFN_vkResetCommandPool vkResetCommandPool = nullptr;
PFN_vkResetFences vkResetFences = nullptr;
PFN_vkUnmapMemory vkUnmapMemory = nullptr;
PFN_vkWaitForFences vkWaitForFences = nullptr;
#endif

namespace
{
std::atomic<unsigned> gErrors{0};
struct CodeSnapshot
{
    const unsigned char* address;
    std::vector<unsigned char> bytes;
};
std::vector<CodeSnapshot> SnapshotCode(HMODULE module)
{
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    std::vector<CodeSnapshot> result;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i=0; i<nt->FileHeader.NumberOfSections; ++i)
        if (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
        {
            const auto* start = base + section[i].VirtualAddress;
            const size_t length = section[i].Misc.VirtualSize;
            result.push_back({start, {start, start+length}});
        }
    return result;
}
bool CodeUnchanged(const std::vector<CodeSnapshot>& code)
{
    return !code.empty() && std::all_of(code.begin(), code.end(), [](const auto& row) {
        return !memcmp(row.address, row.bytes.data(), row.bytes.size()); });
}
void Require(bool passed, const char* message)
{
    printf("%s %s\n", passed ? "PASS" : "FAIL", message); fflush(stdout);
    if (!passed) throw std::runtime_error(message);
}
void Vk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) { printf("VkResult=%d ", result); Require(false, operation); }
}
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    { ++gErrors; printf("VULKAN_ERROR %s\n", data->pMessage); fflush(stdout); }
    return VK_FALSE;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{ return DefWindowProcW(window, message, wparam, lparam); }
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
uint32_t MemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
        if ((bits & (1u << index)) && (properties.memoryTypes[index].propertyFlags & flags) == flags) return index;
    throw std::runtime_error("Required memory type unavailable");
}
float Half(uint16_t bits)
{
    const int exponent = (bits >> 10) & 31, fraction = bits & 1023;
    return (bits & 0x8000 ? -1.0f : 1.0f) * (exponent ? std::ldexp(1.0f+fraction/1024.0f, exponent-15) : std::ldexp(float(fraction), -24));
}
float PqNits(float value)
{
    const float p = std::pow(value, 32.0f/2523.0f);
    return 10000.0f*std::pow(std::max(p-3424.0f/4096.0f, 0.0f)/(2413.0f/128.0f-2392.0f/128.0f*p), 16384.0f/2610.0f);
}
unsigned char Srgb(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<unsigned char>(255.0f*(value <= 0.0031308f ? value*12.92f : 1.055f*std::pow(value, 1.0f/2.4f)-0.055f)+0.5f);
}
// V-Sync through the saved config, as the menu does. The present
// mode is fixed at swapchain creation, so a change waits for the next one.
std::filesystem::path ExecutableDirectory()
{
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    return std::filesystem::path(path).parent_path();
}
bool WriteVsyncConfig(unsigned vsyncMode)
{
    std::ofstream config(ExecutableDirectory() / L"RTXMFG-Universal.json", std::ios::binary | std::ios::trunc);
    config << "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,"
              "\"dynamicTargetFrameRate\":0,\"dlssgPreset\":0,\"vsyncMode\":" << vsyncMode
           << ",\"reflexFrameLimitFps\":0,\"generatedOnlyDebug\":false,"
              "\"intervalLogging\":false,\"version\":13}\n";
    return static_cast<bool>(config);
}
// Read with full sharing and close at once: an open reader makes the backend's
// replace of the status file fail and moves it to the shared-memory fallback.
std::string ReadStatusFile(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 22))
    {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(file);
    return text;
}
bool WaitForStatus(std::initializer_list<std::string> expected)
{
    const auto status = ExecutableDirectory() / L"RTXMFG-Universal.status.json";
    std::string text;
    for (unsigned attempt = 0; attempt < 400; ++attempt)
    {
        text = ReadStatusFile(status);
        if (std::all_of(expected.begin(), expected.end(), [&](const std::string& value) {
                return text.find(value) != std::string::npos; }))
            return true;
        Pump(); Sleep(25);
    }
    for (const auto& value : expected)
        if (text.find(value) == std::string::npos) printf("status lacks %s\n", value.c_str());
    return false;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 3) return 2;
    try
    {
        const int colorMode = argc > 3 && wcscmp(argv[3], L"scrgb") == 0 ? 1
            : argc > 3 && wcscmp(argv[3], L"hdr10") == 0 ? 2
            : argc > 3 && wcscmp(argv[3], L"srgb") == 0 ? 3 : 0;
        const bool resolutionTest = argc > 5;
        const UINT initialWidth = resolutionTest ? static_cast<UINT>(_wtoi(argv[4])) : 800u;
        const UINT initialHeight = resolutionTest ? static_cast<UINT>(_wtoi(argv[5])) : 700u;
        Require(initialWidth >= 640 && initialHeight >= 480, "valid Vulkan output resolution");
        HMODULE loaderBefore = LoadLibraryW(L"vulkan-1.dll");
        Require(loaderBefore != nullptr, "loader available for code preservation check");
        const auto originalCode = SnapshotCode(loaderBefore);
        Require(WriteVsyncConfig(0), "saved V-Sync mode Game written before startup");
        HMODULE module = LoadLibraryExW(std::filesystem::absolute(argv[1]).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module != nullptr, "integrated module loaded before Vulkan instance");
        Require(CodeUnchanged(originalCode), "Vulkan loader executable sections unchanged by installation");
        auto query = reinterpret_cast<MfgSingleModuleQueryFn>(GetProcAddress(module, "MfgUnlockSingleModuleQuery"));
        Require(query != nullptr, "integrated query available");
#if MFG_VK_STREAMLINE
        // Model the real interposer: Vulkan resolvers and CreateInstance are
        // cached by its own module during slInit, before the app asks for them.
        wchar_t executablePath[32768]{}; GetModuleFileNameW(nullptr,executablePath,32768);
        const auto interposerPath=std::filesystem::path(executablePath).parent_path()/L"sl.interposer.dll";
        HMODULE interposer=LoadLibraryExW(interposerPath.c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(interposer!=nullptr,"interposer loaded after mod initialization");
        auto enable=reinterpret_cast<void(*)()>(GetProcAddress(interposer,"FixtureEnableVulkan"));
        auto initialize=reinterpret_cast<sl::Result(*)(const sl::Preferences&,uint64_t)>(GetProcAddress(interposer,"slInit"));
        auto getVulkanProc=reinterpret_cast<FARPROC(WINAPI*)(const char*)>(GetProcAddress(interposer,"FixtureVulkanProc"));
        Require(enable && initialize && getVulkanProc,"interposer ABI fixture available");
        enable(); sl::Preferences preferences{};
        Require(initialize(preferences,sl::kSDKVersion)==sl::Result::eOk,"interposer initialized and cached Vulkan functions internally");
#if MFG_VK_MIXED
        auto getInterposerProc=getVulkanProc;
        getVulkanProc=+[](const char* name)->FARPROC{return GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);};
#endif
#else
        auto getVulkanProc=[](const char* name){return GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),name);};
#endif
        HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
        Require(loader != nullptr, "real Vulkan loader available");
#if MFG_VK_DYNAMIC
        vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(getVulkanProc("vkAcquireNextImageKHR"));
        Require(vkAcquireNextImageKHR != nullptr, "dynamic vkAcquireNextImageKHR resolved");
        vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(getVulkanProc("vkAllocateCommandBuffers"));
        Require(vkAllocateCommandBuffers != nullptr, "dynamic vkAllocateCommandBuffers resolved");
        vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(getVulkanProc("vkAllocateMemory"));
        Require(vkAllocateMemory != nullptr, "dynamic vkAllocateMemory resolved");
        vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(getVulkanProc("vkBeginCommandBuffer"));
        Require(vkBeginCommandBuffer != nullptr, "dynamic vkBeginCommandBuffer resolved");
        vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(getVulkanProc("vkBindBufferMemory"));
        Require(vkBindBufferMemory != nullptr, "dynamic vkBindBufferMemory resolved");
        vkCmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(getVulkanProc("vkCmdClearColorImage"));
        Require(vkCmdClearColorImage != nullptr, "dynamic vkCmdClearColorImage resolved");
        vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(getVulkanProc("vkCmdCopyImageToBuffer"));
        Require(vkCmdCopyImageToBuffer != nullptr, "dynamic vkCmdCopyImageToBuffer resolved");
        vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(getVulkanProc("vkCmdPipelineBarrier"));
        Require(vkCmdPipelineBarrier != nullptr, "dynamic vkCmdPipelineBarrier resolved");
        vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(getVulkanProc("vkCreateBuffer"));
        Require(vkCreateBuffer != nullptr, "dynamic vkCreateBuffer resolved");
        vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(getVulkanProc("vkCreateCommandPool"));
        Require(vkCreateCommandPool != nullptr, "dynamic vkCreateCommandPool resolved");
        vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(getVulkanProc("vkCreateDevice"));
        Require(vkCreateDevice != nullptr, "dynamic vkCreateDevice resolved");
        vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(getVulkanProc("vkCreateFence"));
        Require(vkCreateFence != nullptr, "dynamic vkCreateFence resolved");
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(getVulkanProc("vkCreateInstance"));
        Require(vkCreateInstance != nullptr, "dynamic vkCreateInstance resolved");
        vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(getVulkanProc("vkCreateSemaphore"));
        Require(vkCreateSemaphore != nullptr, "dynamic vkCreateSemaphore resolved");
        vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(getVulkanProc("vkCreateSwapchainKHR"));
        Require(vkCreateSwapchainKHR != nullptr, "dynamic vkCreateSwapchainKHR resolved");
        vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(getVulkanProc("vkCreateWin32SurfaceKHR"));
        Require(vkCreateWin32SurfaceKHR != nullptr, "dynamic vkCreateWin32SurfaceKHR resolved");
        vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(getVulkanProc("vkDestroyBuffer"));
        Require(vkDestroyBuffer != nullptr, "dynamic vkDestroyBuffer resolved");
        vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(getVulkanProc("vkDestroyCommandPool"));
        Require(vkDestroyCommandPool != nullptr, "dynamic vkDestroyCommandPool resolved");
        vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getVulkanProc("vkDestroyDevice"));
        Require(vkDestroyDevice != nullptr, "dynamic vkDestroyDevice resolved");
        vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(getVulkanProc("vkDestroyFence"));
        Require(vkDestroyFence != nullptr, "dynamic vkDestroyFence resolved");
        vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(getVulkanProc("vkDestroyInstance"));
        Require(vkDestroyInstance != nullptr, "dynamic vkDestroyInstance resolved");
        vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(getVulkanProc("vkDestroySemaphore"));
        Require(vkDestroySemaphore != nullptr, "dynamic vkDestroySemaphore resolved");
        vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(getVulkanProc("vkDestroySurfaceKHR"));
        Require(vkDestroySurfaceKHR != nullptr, "dynamic vkDestroySurfaceKHR resolved");
        vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(getVulkanProc("vkDestroySwapchainKHR"));
        Require(vkDestroySwapchainKHR != nullptr, "dynamic vkDestroySwapchainKHR resolved");
        vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(getVulkanProc("vkDeviceWaitIdle"));
        Require(vkDeviceWaitIdle != nullptr, "dynamic vkDeviceWaitIdle resolved");
        vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(getVulkanProc("vkEndCommandBuffer"));
        Require(vkEndCommandBuffer != nullptr, "dynamic vkEndCommandBuffer resolved");
        vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(getVulkanProc("vkEnumerateInstanceExtensionProperties"));
        Require(vkEnumerateInstanceExtensionProperties != nullptr, "dynamic vkEnumerateInstanceExtensionProperties resolved");
        vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(getVulkanProc("vkEnumerateInstanceLayerProperties"));
        Require(vkEnumerateInstanceLayerProperties != nullptr, "dynamic vkEnumerateInstanceLayerProperties resolved");
        vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getVulkanProc("vkEnumeratePhysicalDevices"));
        Require(vkEnumeratePhysicalDevices != nullptr, "dynamic vkEnumeratePhysicalDevices resolved");
        vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(getVulkanProc("vkFreeMemory"));
        Require(vkFreeMemory != nullptr, "dynamic vkFreeMemory resolved");
        vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(getVulkanProc("vkGetBufferMemoryRequirements"));
        Require(vkGetBufferMemoryRequirements != nullptr, "dynamic vkGetBufferMemoryRequirements resolved");
        vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getVulkanProc("vkGetDeviceProcAddr"));
        Require(vkGetDeviceProcAddr != nullptr, "dynamic vkGetDeviceProcAddr resolved");
        vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(getVulkanProc("vkGetDeviceQueue"));
        Require(vkGetDeviceQueue != nullptr, "dynamic vkGetDeviceQueue resolved");
        vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(getVulkanProc("vkGetInstanceProcAddr"));
        Require(vkGetInstanceProcAddr != nullptr, "dynamic vkGetInstanceProcAddr resolved");
        vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getVulkanProc("vkGetPhysicalDeviceMemoryProperties"));
        Require(vkGetPhysicalDeviceMemoryProperties != nullptr, "dynamic vkGetPhysicalDeviceMemoryProperties resolved");
        vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getVulkanProc("vkGetPhysicalDeviceProperties"));
        Require(vkGetPhysicalDeviceProperties != nullptr, "dynamic vkGetPhysicalDeviceProperties resolved");
        vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getVulkanProc("vkGetPhysicalDeviceQueueFamilyProperties"));
        Require(vkGetPhysicalDeviceQueueFamilyProperties != nullptr, "dynamic vkGetPhysicalDeviceQueueFamilyProperties resolved");
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(getVulkanProc("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        Require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR != nullptr, "dynamic vkGetPhysicalDeviceSurfaceCapabilitiesKHR resolved");
        vkGetPhysicalDeviceSurfaceFormatsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(getVulkanProc("vkGetPhysicalDeviceSurfaceFormatsKHR"));
        Require(vkGetPhysicalDeviceSurfaceFormatsKHR != nullptr, "dynamic vkGetPhysicalDeviceSurfaceFormatsKHR resolved");
        vkGetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(getVulkanProc("vkGetPhysicalDeviceSurfacePresentModesKHR"));
        Require(vkGetPhysicalDeviceSurfacePresentModesKHR != nullptr, "dynamic vkGetPhysicalDeviceSurfacePresentModesKHR resolved");
        vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(getVulkanProc("vkGetPhysicalDeviceSurfaceSupportKHR"));
        Require(vkGetPhysicalDeviceSurfaceSupportKHR != nullptr, "dynamic vkGetPhysicalDeviceSurfaceSupportKHR resolved");
        vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(getVulkanProc("vkGetSwapchainImagesKHR"));
        Require(vkGetSwapchainImagesKHR != nullptr, "dynamic vkGetSwapchainImagesKHR resolved");
        vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(getVulkanProc("vkMapMemory"));
        Require(vkMapMemory != nullptr, "dynamic vkMapMemory resolved");
        vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(getVulkanProc("vkQueueSubmit"));
        Require(vkQueueSubmit != nullptr, "dynamic vkQueueSubmit resolved");
        vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(getVulkanProc("vkQueueWaitIdle"));
        Require(vkQueueWaitIdle != nullptr, "dynamic vkQueueWaitIdle resolved");
        vkResetCommandPool = reinterpret_cast<PFN_vkResetCommandPool>(getVulkanProc("vkResetCommandPool"));
        Require(vkResetCommandPool != nullptr, "dynamic vkResetCommandPool resolved");
        vkResetFences = reinterpret_cast<PFN_vkResetFences>(getVulkanProc("vkResetFences"));
        Require(vkResetFences != nullptr, "dynamic vkResetFences resolved");
        vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(getVulkanProc("vkUnmapMemory"));
        Require(vkUnmapMemory != nullptr, "dynamic vkUnmapMemory resolved");
        vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(getVulkanProc("vkWaitForFences"));
        Require(vkWaitForFences != nullptr, "dynamic vkWaitForFences resolved");
#endif
        HMODULE resolverOwner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vkGetInstanceProcAddr), &resolverOwner);
        MfgSingleModuleStatus initialStatus{}; initialStatus.size = sizeof(initialStatus);
        Require(query(&initialStatus) && initialStatus.vulkanHooked
#if MFG_VK_DYNAMIC
            && resolverOwner == module
#endif
            , "Vulkan resolver routed through exact candidate DLL");
        Require(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkRTXMFGUnknownFunction") == nullptr,
            "unknown Vulkan function remains null");
        uint32_t count = 0; Vk(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
        std::vector<VkLayerProperties> layers(count); Vk(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read layers");
        bool validation = std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
        Vk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "enumerate instance extensions");
        std::vector<VkExtensionProperties> extensions(count);
        Vk(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()), "read instance extensions");
        bool debugUtils = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0; });
        std::vector<const char*> enabledExtensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        if (debugUtils) enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return strcmp(extension.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0; }))
            enabledExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        const char* layer = "VK_LAYER_KHRONOS_validation";
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "MFG single module validation"; application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
        instanceInfo.enabledLayerCount = validation ? 1 : 0; instanceInfo.ppEnabledLayerNames = &layer;
        VkInstance instance = VK_NULL_HANDLE; Vk(vkCreateInstance(&instanceInfo, nullptr, &instance), "create instance");
#if MFG_VK_DYNAMIC
        vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        Require(vkEnumeratePhysicalDevices != nullptr, "instance-dispatch vkEnumeratePhysicalDevices");
        vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(vkGetInstanceProcAddr(instance, "vkCreateDevice"));
        Require(vkCreateDevice != nullptr, "instance-dispatch vkCreateDevice");
        vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
        Require(vkCreateWin32SurfaceKHR != nullptr, "instance-dispatch vkCreateWin32SurfaceKHR");
        vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
        Require(vkDestroySurfaceKHR != nullptr, "instance-dispatch vkDestroySurfaceKHR");
        vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(vkGetInstanceProcAddr(instance, "vkDestroyInstance"));
        Require(vkDestroyInstance != nullptr, "instance-dispatch vkDestroyInstance");
#endif
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (debugUtils && createDebug)
        {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = &Debug; Vk(createDebug(instance, &info, nullptr, &messenger), "create debug messenger");
        }
        printf("validation_layer=%s debug_utils=%s\n", validation ? "enabled" : "unavailable", debugUtils ? "enabled" : "unavailable");
        Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical devices");
        std::vector<VkPhysicalDevice> physicals(count); Vk(vkEnumeratePhysicalDevices(instance, &count, physicals.data()), "read physical devices");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        for (auto candidate : physicals)
        {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x10de)
            { physical = candidate; printf("adapter=%s driver=%u api=%u\n", properties.deviceName, properties.driverVersion, properties.apiVersion); break; }
        }
        Require(physical != VK_NULL_HANDLE, "NVIDIA hardware adapter selected");
        WNDCLASSW windowClass{}; windowClass.lpfnWndProc = &WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr); windowClass.lpszClassName = L"MfgSingleVulkanHarness";
        RegisterClassW(&windowClass);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"MFG local Vulkan validation",
            WS_POPUP, 0, 0, initialWidth, initialHeight, nullptr, nullptr, windowClass.hInstance, nullptr);
        Require(window != nullptr, "hidden Vulkan harness window created");
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = windowClass.hInstance; surfaceInfo.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE; Vk(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "create Win32 surface");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t index = 0; index < count; ++index)
        {
            VkBool32 supported = VK_FALSE; Vk(vkGetPhysicalDeviceSurfaceSupportKHR(physical, index, surface, &supported), "surface queue support");
            if (supported && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = index; break; }
        }
        Require(family != UINT32_MAX, "graphics presentation queue proven");
        float priority = 1; VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
        VkDevice device = VK_NULL_HANDLE; Vk(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "create device");
#if MFG_VK_DYNAMIC
        vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(vkGetDeviceProcAddr(device, "vkGetDeviceQueue"));
        Require(vkGetDeviceQueue != nullptr, "device-dispatch vkGetDeviceQueue");
        vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(vkGetDeviceProcAddr(device, "vkCreateSemaphore"));
        Require(vkCreateSemaphore != nullptr, "device-dispatch vkCreateSemaphore");
        vkDestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(vkGetDeviceProcAddr(device, "vkDestroySemaphore"));
        Require(vkDestroySemaphore != nullptr, "device-dispatch vkDestroySemaphore");
        vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
        Require(vkCreateSwapchainKHR != nullptr, "device-dispatch vkCreateSwapchainKHR");
        vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR"));
        Require(vkDestroySwapchainKHR != nullptr, "device-dispatch vkDestroySwapchainKHR");
        vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
        Require(vkDestroyDevice != nullptr, "device-dispatch vkDestroyDevice");
#endif
#if MFG_VK_MIXED
        vkCreateSemaphore=reinterpret_cast<PFN_vkCreateSemaphore>(getInterposerProc("vkCreateSemaphore"));
        vkDestroySemaphore=reinterpret_cast<PFN_vkDestroySemaphore>(getInterposerProc("vkDestroySemaphore"));
        Require(vkCreateSemaphore && vkDestroySemaphore,"presentation semaphores supplied by interposer resolver");
#endif
        VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(device, family, 0, &queue);
        // Exercise the device resolver too, rather than only loader imports.
        auto present = reinterpret_cast<PFN_vkQueuePresentKHR>(vkGetDeviceProcAddr(device, "vkQueuePresentKHR"));
        Require(present != nullptr, "device-resolved Present available");
        Vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "enumerate surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count); Vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "read formats");
        auto format = std::find_if(formats.begin(), formats.end(), [colorMode](const auto& value) {
            if (colorMode == 1) return value.format == VK_FORMAT_R16G16B16A16_SFLOAT && value.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            if (colorMode == 2) return (value.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || value.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
                && value.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
            if (colorMode == 3) return (value.format == VK_FORMAT_B8G8R8A8_SRGB || value.format == VK_FORMAT_R8G8B8A8_SRGB)
                && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            return (value.format == VK_FORMAT_B8G8R8A8_UNORM || value.format == VK_FORMAT_R8G8B8A8_UNORM)
                && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; });
        if (format == formats.end())
        {
            printf("SKIP requested color mode %d unavailable; supported formats:\n", colorMode);
            for (const auto& value : formats) printf("format=%d color_space=%d\n", value.format, value.colorSpace);
            vkDestroyDevice(device, nullptr); vkDestroySurfaceKHR(instance, surface, nullptr);
            if (messenger && destroyDebug) destroyDebug(instance, messenger, nullptr);
            vkDestroyInstance(instance, nullptr); DestroyWindow(window); return 77;
        }
        printf("color_mode=%d format=%d color_space=%d\n", colorMode, format->format, format->colorSpace);
        Vk(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr), "enumerate present modes");
        std::vector<VkPresentModeKHR> presentModes(count);
        Vk(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, presentModes.data()), "read present modes");
        const auto listed = [&](VkPresentModeKHR mode) {
            return std::find(presentModes.begin(), presentModes.end(), mode) != presentModes.end(); };
        // Off replaces the game's FIFO with IMMEDIATE, else MAILBOX, when listed.
        const VkPresentModeKHR offMode = listed(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
            : listed(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
        printf("present_modes=%u off_mode=%d\n", count, offMode);
        for (unsigned phase = 0; phase < 2; ++phase)
        {
            const VkExtent2D requested = phase
                ? (resolutionTest ? VkExtent2D{1920,1080} : VkExtent2D{900,750})
                : VkExtent2D{initialWidth,initialHeight};
            if (phase) SetWindowPos(window, nullptr, 0, 0, requested.width, requested.height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Pump();
            VkSurfaceCapabilitiesKHR caps{}; Vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX) extent = requested;
            Require(extent.width == requested.width && extent.height == requested.height, "Vulkan surface matches requested pixel dimensions");
            Require((caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT), "readback and clear usages supported");
            VkSwapchainCreateInfoKHR chainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            chainInfo.surface = surface; chainInfo.minImageCount = std::max(3u, caps.minImageCount);
            if (caps.maxImageCount) chainInfo.minImageCount = std::min(chainInfo.minImageCount, caps.maxImageCount);
            chainInfo.imageFormat = format->format; chainInfo.imageColorSpace = format->colorSpace; chainInfo.imageExtent = extent;
            chainInfo.imageArrayLayers = 1; chainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            chainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; chainInfo.preTransform = caps.currentTransform;
            chainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; chainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; chainInfo.clipped = VK_TRUE;
            VkSwapchainKHR chain = VK_NULL_HANDLE; Vk(vkCreateSwapchainKHR(device, &chainInfo, nullptr, &chain), "create/recreate swapchain");
            if (!phase)
                Require(WaitForStatus({"\"vsyncMode\":0,", "\"vsyncVulkanSwapchain\":true,", "\"vsyncVulkanHook\":true,",
                    "\"vsyncOriginalPresentMode\":2,", "\"vsyncSubmittedPresentMode\":2,", "\"vsyncFailure\":0,"}),
                    "Game: FIFO swapchain created unchanged and reported");
            else if (offMode != VK_PRESENT_MODE_FIFO_KHR)
                Require(WaitForStatus({"\"vsyncMode\":1,", "\"vsyncOverrideApplied\":true,", "\"vsyncPresentationObserved\":true,",
                    "\"vsyncOriginalPresentMode\":2,", "\"vsyncSubmittedPresentMode\":" + std::to_string(static_cast<int>(offMode)) + ",",
                    "\"vsyncFailure\":0,"}), "Off: recreated swapchain submitted without V-Sync");
            else
                Require(WaitForStatus({"\"vsyncMode\":1,", "\"vsyncSubmittedPresentMode\":2,", "\"vsyncFailure\":14,"}),
                    "Off: FIFO kept and reported when the surface lists no other mode");
#if MFG_VK_MIXED
            MfgSingleModuleStatus attached{};attached.size=sizeof(attached);
            Require(query(&attached) && attached.overlayInstallState==2,"mixed route attaches swapchain before drawing");
#endif
            Vk(vkGetSwapchainImagesKHR(device, chain, &count, nullptr), "count images");
            std::vector<VkImage> images(count); Vk(vkGetSwapchainImagesKHR(device, chain, &count, images.data()), "read images");
            std::vector<bool> used(count, false); std::vector<VkSemaphore> ready(count, VK_NULL_HANDLE);
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (auto& semaphore : ready) Vk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "create per-image game semaphore");
            VkSemaphore acquire = VK_NULL_HANDLE; Vk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquire), "create acquire semaphore");
            // A valid binary semaphore with an unrecognized creation chain
            // must be forwarded without overlay submission or re-signaling.
            VkSemaphoreTypeCreateInfo explicitType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            explicitType.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
            VkSemaphoreCreateInfo unknownInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; unknownInfo.pNext = &explicitType;
            VkSemaphore unknown = VK_NULL_HANDLE; Vk(vkCreateSemaphore(device, &unknownInfo, nullptr, &unknown), "create unknown-shape binary semaphore");
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE; Vk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "create game fence");
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = family;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCommandPool pool = VK_NULL_HANDLE; Vk(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "create game pool");
            VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commandInfo.commandPool = pool;
            commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
            VkCommandBuffer commands = VK_NULL_HANDLE; Vk(vkAllocateCommandBuffers(device, &commandInfo, &commands), "allocate game commands");
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bufferInfo.size = VkDeviceSize(extent.width)*extent.height*(colorMode == 1 ? 8 : 4);
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer readback = VK_NULL_HANDLE; Vk(vkCreateBuffer(device, &bufferInfo, nullptr, &readback), "create readback buffer");
            VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, readback, &requirements);
            VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memoryInfo.allocationSize = requirements.size;
            memoryInfo.memoryTypeIndex = MemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory memory = VK_NULL_HANDLE; Vk(vkAllocateMemory(device, &memoryInfo, nullptr, &memory), "allocate readback memory");
            Vk(vkBindBufferMemory(device, readback, memory, 0), "bind readback");
            bool captured = false;
            for (unsigned frame = 0; frame < 18; ++frame)
            {
                if (frame == 2 || frame == 15)
                {
                    PostMessageW(window, WM_KEYDOWN, VK_BACK, 0);
                    PostMessageW(window, WM_KEYUP, VK_BACK, 1ll << 31);
                }
                Pump();
                uint32_t index = 0; Vk(vkAcquireNextImageKHR(device, chain, 5000000000ull, acquire, VK_NULL_HANDLE, &index), "acquire image");
                const VkSemaphore presentReady = frame == 17 ? unknown : ready[index];
                Vk(vkResetCommandPool(device, pool, 0), "reset game commands");
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                Vk(vkBeginCommandBuffer(commands, &begin), "begin game commands");
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = images[index];
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier.oldLayout = used[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
                const bool capture = frame >= 9 && frame <= 13 && used[index] && !captured;
                if (capture)
                {
                    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {extent.width, extent.height, 1};
                    vkCmdCopyImageToBuffer(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                }
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkClearColorValue clear{{0.025f, 0.08f, 0.14f, 1.0f}};
                vkCmdClearColorImage(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                Vk(vkEndCommandBuffer(commands), "end game commands");
                VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquire;
                submit.pWaitDstStageMask = &stage; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands;
                submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &presentReady;
                Vk(vkQueueSubmit(queue, 1, &submit, fence), "submit game commands");
                Vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull), "wait game fence");
                Vk(vkResetFences(device, 1, &fence), "reset game fence");
                if (capture)
                {
                    void* mapped = nullptr; Vk(vkMapMemory(device, memory, 0, bufferInfo.size, 0, &mapped), "map readback");
                    std::ofstream image(std::filesystem::path(argv[2]) / (phase ? "vulkan-after-recreate.ppm" : "vulkan-before-recreate.ppm"), std::ios::binary);
                    image << "P6\n" << extent.width << " " << extent.height << "\n255\n";
                    size_t bright = 0;
                    float maximumNits = 0;
                    for (size_t pixel = 0; pixel < size_t(extent.width)*extent.height; ++pixel)
                    {
                        const auto* rgba = static_cast<const unsigned char*>(mapped)+pixel*(colorMode == 1 ? 8 : 4);
                        unsigned char rgb[3]{};
                        if (colorMode == 1 || colorMode == 2)
                        {
                            float nits[3]{};
                            if (colorMode == 1)
                            {
                                const auto* half = reinterpret_cast<const uint16_t*>(rgba);
                                for (int channel = 0; channel < 3; ++channel) nits[channel] = Half(half[channel])*80.0f;
                            }
                            else
                            {
                                uint32_t packed; memcpy(&packed, rgba, 4);
                                const bool bgr = format->format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
                                for (int channel = 0; channel < 3; ++channel)
                                    nits[channel] = PqNits(float((packed >> (10*(bgr ? 2-channel : channel))) & 1023)/1023.0f);
                            }
                            for (int channel = 0; channel < 3; ++channel)
                            { maximumNits = std::max(maximumNits, nits[channel]); rgb[channel] = Srgb(nits[channel]/203.0f); }
                        }
                        else
                        {
                            const bool bgr = format->format == VK_FORMAT_B8G8R8A8_UNORM || format->format == VK_FORMAT_B8G8R8A8_SRGB;
                            rgb[0] = rgba[bgr ? 2 : 0]; rgb[1] = rgba[1]; rgb[2] = rgba[bgr ? 0 : 2];
                        }
                        image.write(reinterpret_cast<const char*>(rgb), 3);
                        if (rgb[0] > 100 && rgb[1] > 100 && rgb[2] > 100) ++bright;
                    }
                    if (colorMode == 1 || colorMode == 2)
                    {
                        printf("ui_maximum_nits=%.3f\n", maximumNits);
                        Require(maximumNits >= 197 && maximumNits <= 210, "HDR Vulkan UI white is encoded near 203 nits");
                    }
                    vkUnmapMemory(device, memory); Require(bright > 300, "Vulkan readback contains rendered UI text"); captured = true;
                }
                VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; presentInfo.waitSemaphoreCount = 1;
                presentInfo.pWaitSemaphores = &presentReady; presentInfo.swapchainCount = 1; presentInfo.pSwapchains = &chain; presentInfo.pImageIndices = &index;
                MfgSingleModuleStatus before{}; before.size = sizeof(before); Require(query(&before), "read pre-present counters");
                Vk(present(queue, &presentInfo), "present with preserved game semaphore set"); used[index] = true;
                if (frame == 17)
                {
                    MfgSingleModuleStatus after{}; after.size = sizeof(after);
                    Require(query(&after) && after.renderedFrames == before.renderedFrames,
                        "unknown semaphore shape forwards presentation without overlay mutation");
                }
                Vk(vkQueueWaitIdle(queue), "wait game presentation queue"); Sleep(15);
            }
            Require(captured, "Vulkan UI capture completed for this swapchain generation");
            // Switch live while this swapchain exists: its mode stays until recreation.
            Require(WriteVsyncConfig(phase ? 0 : 1) && WaitForStatus({phase ? "\"vsyncMode\":0," : "\"vsyncMode\":1,",
                "\"vsyncVulkanSwapchain\":true,",
                phase && offMode == VK_PRESENT_MODE_FIFO_KHR ? "\"vsyncFailure\":0," : "\"vsyncFailure\":13,"}),
                phase ? "Game selected live: overridden swapchain reported until recreation"
                    : "Off selected live: applies when the game recreates its swapchain");
            Vk(vkDeviceWaitIdle(device), "drain device for destruction");
            vkDestroyBuffer(device, readback, nullptr); vkFreeMemory(device, memory, nullptr);
            vkDestroyCommandPool(device, pool, nullptr); vkDestroyFence(device, fence, nullptr);
            vkDestroySwapchainKHR(device, chain, nullptr);
            vkDestroySemaphore(device, acquire, nullptr); for (auto semaphore : ready) vkDestroySemaphore(device, semaphore, nullptr);
            vkDestroySemaphore(device, unknown, nullptr);
        }
        Require(WaitForStatus({"\"vsyncMode\":0,", "\"vsyncVulkanSwapchain\":false,", "\"vsyncFailure\":2,"}),
            "destroyed swapchain clears the Vulkan V-Sync record");
        MfgSingleModuleStatus status{}; status.size = sizeof(status);
        Require(query(&status) && status.backendOwner && status.vulkanFrames == 34 && status.renderedFrames == 34
            && !status.renderFailures && !status.overlayVisible, "Vulkan toggle, rendering and swapchain recreation remain healthy");
        printf("vulkan_frames=%llu rendered_frames=%llu failures=%u\n", status.vulkanFrames, status.renderedFrames, status.renderFailures);
        vkDestroyDevice(device, nullptr); vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger && destroyDebug) destroyDebug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr); DestroyWindow(window); Pump();
        Require(gErrors.load() == 0, "no Vulkan validation errors");
        Require(CodeUnchanged(originalCode), "Vulkan loader executable sections unchanged after rendering and teardown");
        FreeLibrary(module); return 0;
    }
    catch (const std::exception& error) { printf("ERROR %s\n", error.what()); return 1; }
}
