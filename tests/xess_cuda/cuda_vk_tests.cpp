// Gate V0, import prerequisite. No dispatch is claimed until the exact kernel image imports.
#include "xess/vulkan/xvk_tensor.h"
#include "xess/vulkan/xvk_internal.h"
#include "sha256_util.h"
#include "cuda_cases.h"
#include "xess/cuda/cuda_route.h"
#include <wrl/client.h>
#include <thread>
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstring>

namespace
{
void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(result));
}
struct Vulkan
{
    HMODULE loader{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkDestroyInstance destroyInstance{};
    PFN_vkDestroyDevice destroyDevice{};
    xvk::DeviceDesc desc{};
    Vulkan()
    {
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("system Vulkan loader missing");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("loader entry missing");
        const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "XefgCudaVkTests V0";
        application.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &application;
        Check(create(&info, nullptr, &instance), "vkCreateInstance");
#define INSTANCE(type, name) const auto name = reinterpret_cast<PFN_##type>(gipa(instance, #type))
        INSTANCE(vkEnumeratePhysicalDevices, enumerate);
        INSTANCE(vkGetPhysicalDeviceProperties, properties);
        INSTANCE(vkEnumerateDeviceExtensionProperties, extensions);
        INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties, families);
        INSTANCE(vkCreateDevice, createDevice);
        gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
#undef INSTANCE
        destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        uint32_t count{}; Check(enumerate(instance, &count, nullptr), "physical count");
        std::vector<VkPhysicalDevice> devices(count); Check(enumerate(instance, &count, devices.data()), "physical devices");
        VkPhysicalDeviceProperties chosen{};
        for (auto candidate : devices)
        {
            VkPhysicalDeviceProperties p{}; properties(candidate, &p);
            if (p.vendorID == 0x10DE) { physical = candidate; chosen = p; break; }
        }
        if (!physical) throw std::runtime_error("no NVIDIA Vulkan adapter");
        Check(extensions(physical, nullptr, &count, nullptr), "extension count");
        std::vector<VkExtensionProperties> offered(count);
        Check(extensions(physical, nullptr, &count, offered.data()), "extensions");
        for (const auto& e : offered)
        {
            if (!std::strcmp(e.extensionName, VK_NVX_BINARY_IMPORT_EXTENSION_NAME)) desc.binaryImportSpec = e.specVersion;
            if (!std::strcmp(e.extensionName, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME)) desc.imageViewHandleSpec = e.specVersion;
        }
        std::printf("DEVICE name=\"%s\" vendor=%04x device=%04x driverRaw=%u driver=%u.%u.%u.%u api=%u.%u.%u binaryImportSpec=%u imageViewHandleSpec=%u\n",
            chosen.deviceName, chosen.vendorID, chosen.deviceID, chosen.driverVersion,
            chosen.driverVersion >> 22, (chosen.driverVersion >> 14) & 255, (chosen.driverVersion >> 6) & 255, chosen.driverVersion & 63,
            VK_VERSION_MAJOR(chosen.apiVersion), VK_VERSION_MINOR(chosen.apiVersion), VK_VERSION_PATCH(chosen.apiVersion),
            desc.binaryImportSpec, desc.imageViewHandleSpec);
        if (!desc.binaryImportSpec || !desc.imageViewHandleSpec) throw std::runtime_error("NVX extensions not offered");
        families(physical, &count, nullptr); std::vector<VkQueueFamilyProperties> queues(count); families(physical, &count, queues.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
        if (family == UINT32_MAX) throw std::runtime_error("no compute queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        const char* names[] = {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        v13.synchronization2 = v13.maintenance4 = v13.shaderIntegerDotProduct = VK_TRUE;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        v12.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceFeatures features{}; features.shaderStorageImageExtendedFormats = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queue;
        deviceInfo.pNext = &v12; deviceInfo.pEnabledFeatures = &features;
        deviceInfo.enabledExtensionCount = 3; deviceInfo.ppEnabledExtensionNames = names;
        Check(createDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice with NVX");
        destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"));
        desc.instance = instance; desc.physicalDevice = physical; desc.device = device; desc.getInstanceProcAddr = gipa;
        desc.tensorExtensions = true; desc.recorded = true; desc.recordedFamily = family;
        std::puts("EXTENSIONS explicitly_enabled=VK_NVX_binary_import,VK_NVX_image_view_handle");
    }
    ~Vulkan()
    {
        if (device && destroyDevice) destroyDevice(device, nullptr);
        if (instance && destroyInstance) destroyInstance(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }
};
}

#include "cuda_vk_recording.h"

namespace single_module { void Log(const wchar_t* text) noexcept { std::wprintf(L"%ls\n", text); } }

int wmain(int argc, wchar_t** argv)
{
    try
    {
        std::filesystem::path fatbin, candidate;
        std::vector<std::filesystem::path> programDirs;
        xvk::TensorOptions options;
        bool importOnly = false;
        std::wstring test = L"equivalence";
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];
            auto value = [&]() -> std::wstring { if (++i == argc) throw std::runtime_error("missing option value"); return argv[i]; };
            if (arg == L"--fatbin") fatbin = value();
            else if (arg == L"--candidate-dll") candidate = value();
            else if (arg == L"--programs") programDirs.emplace_back(value());
            else if (arg == L"--texturing") { const auto v = value(); if (v != L"64" && v != L"32") throw std::runtime_error("texturing"); options.texturing64 = v == L"64"; }
            else if (arg == L"--module") { const auto v = value(); if (v != L"elf" && v != L"fatbin") throw std::runtime_error("module"); options.elfModule = v == L"elf"; }
            else if (arg == L"--params") { const auto v = value(); if (v != L"args" && v != L"extras") throw std::runtime_error("params"); options.argumentParams = v == L"args"; }
            else if (arg == L"--barriers") { const auto v = value(); if (v != L"xefg" && v != L"full") throw std::runtime_error("barriers"); options.fullBarriers = v == L"full"; }
            else if (arg == L"--import-only") importOnly = true;
            else if (arg == L"--case") test = value();
            else throw std::runtime_error("unknown option");
        }

        if (test == L"gate") {
            size_t checks = 0;
            for (const auto [major, minor] : {std::pair{7,5}, std::pair{8,6}, std::pair{8,9}, std::pair{12,0}, std::pair{12,1}, std::pair{9,0}})
                for (const std::string name : {"NVIDIA GeForce RTX 4080", "NVIDIA GeForce GTX 1650", "Tesla T4"}) {
                    if (xvk::TensorRoute::ArchitectureAllowed(major, minor, name) != xefg_cuda::SupportedArchitecture(major, minor, name))
                        throw std::runtime_error("Vulkan architecture gate differs");
                    ++checks;
                }
            std::printf("VK_ARCHITECTURE_GATE_PASS checks=%zu\n", checks); return 0;
        }
        std::ifstream file(fatbin, std::ios::binary);
        if (!file) throw std::runtime_error("fatbin input missing");
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        sha256_util::Digest digest{};
        if (!sha256_util::Compute(bytes.data(), bytes.size(), digest)) throw std::runtime_error("fatbin hash failed");
        const auto hash = sha256_util::LowerHex(digest);
        if (hash != "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7") throw std::runtime_error("fatbin pin differs");
        std::printf("FATBIN sha256=%s bytes=%zu\n", hash.c_str(), bytes.size());
        Vulkan vk;
        if (!importOnly)
        {
            HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (!module) throw std::runtime_error("candidate program module missing");
            std::map<std::string, std::vector<uint8_t>> bySha;
            for (const auto& dir : programDirs)
                for (const auto& entry : std::filesystem::directory_iterator(dir))
                    if (entry.is_regular_file()) {
                        std::ifstream input(entry.path(), std::ios::binary);
                        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                        sha256_util::Digest d{};
                        if (!data.empty() && sha256_util::Compute(data.data(), data.size(), d)) bySha.emplace(sha256_util::LowerHex(d), std::move(data));
                    }
            size_t cases = 0, failures = 0;
            {
                Gpu g(vk, module);
                std::string reason;
                if (!xvk::InstallTensorCores(g.device.Get(), bytes.data(), bytes.size(), options, reason))
                    throw std::runtime_error("route install: " + reason);
                xefg_cuda::SetLog([](const char* stage, const std::string& fields) noexcept { std::printf("STAGE %s %s\n", stage, fields.c_str()); });
                for (const auto& spec : xefg_cuda_cases::kSpecs)
                {
                    const xefg_cuda::LayerInfo* layer = nullptr;
                    for (const auto& row : xefg_cuda::Layers()) if (!std::strcmp(row.program, spec.name)) layer = &row;
                    if (!layer || !bySha.count(layer->sha256)) throw std::runtime_error(std::string("missing layer fixture: ") + spec.name);
                    Program program; program.dxil = bySha.at(layer->sha256);
                    Check(g.device->CreateRootSignature(0, program.dxil.data(), program.dxil.size(), IID_PPV_ARGS(&program.root)), "root signature");
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = program.root.Get(); desc.CS = {program.dxil.data(), program.dxil.size()};
                    Check(g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&program.reference)), "pipeline");
                    const auto beforeLayer = failures;
                    for (const auto size : {std::pair{37u, 29u}, std::pair{101u, 67u}, std::pair{16u, 8u}})
                        for (int mode = 0; mode < 5; ++mode)
                            for (int realistic = 0; realistic < 2; ++realistic)
                            {
                                if (test == L"refusals" && (std::strcmp(spec.name, "aa016078") || size != std::pair{37u,29u} || mode >= 4 || realistic != 1)) continue;
                                if (test == L"shadow_refresh" && (size != std::pair{37u,29u} || mode != 0 || realistic != 1)) continue;
                                if (test == L"borders" && mode != 1 && mode != 2) continue;
                                if (mode == 4 && (spec.family != xefg_cuda_cases::Family::Flow || spec.nout != 8)) continue;
                                auto c = Make(spec, size.first, size.second, 7u + uint32_t(cases), realistic != 0, mode);
                                // These DXIL programs write uint2 and their SPIR-V storage image is Rg32ui.
                                // Use matching Vulkan views in every mode; RGBA32 leaves z/w undefined.
                                if (!std::strcmp(spec.name, "fc1b8a04") || !std::strcmp(spec.name, "3459485")
                                    || !std::strcmp(spec.name, "3452733")) c.components = 2;
                                const int refusal = test == L"refusals" ? mode + 1 : 0;
                                if (refusal == 2) {
                                    const auto* device = static_cast<xvk::Device*>(static_cast<ID3D12Device1*>(g.device.Get()));
                                    c.buffers[1].resize(size_t(device->properties().limits.maxImageDimension1D + 1) * 4, 0);
                                }
                                const bool refresh = test == L"shadow_refresh";
                                xvk::SetTensorCoresEnabled(g.device.Get(), false);
                                std::vector<uint32_t> unmodified;
                                if (refresh) unmodified = Run(g, spec, c, program, program.reference.Get(), false);
                                const auto reference = Run(g, spec, c, program, program.reference.Get(), false, refresh, refusal);
                                const auto before = xefg_cuda::Snapshot();
                                xvk::SetTensorCoresEnabled(g.device.Get(), true);
                                const auto routed = Run(g, spec, c, program, program.reference.Get(), false, refresh, refusal);
                                const auto after = xefg_cuda::Snapshot();
                                size_t poison{}; const auto bad = Mismatches(reference, routed, poison);
                                const bool ok = !bad && after.launches == before.launches + (refusal ? 0 : refresh ? 2 : 1)
                                    && after.fallbacks == before.fallbacks + (refusal ? 1 : 0)
                                    && (!refresh || unmodified != reference);
                                ++cases; failures += !ok;
                                if (!ok) std::printf("FAIL %s mismatches=%zu launches=%llu fallbacks=%llu\n", c.label.c_str(), bad,
                                    static_cast<unsigned long long>(after.launches - before.launches), static_cast<unsigned long long>(after.fallbacks - before.fallbacks));
                                if (bad)
                                {
                                    size_t components[4]{}, shown = 0;
                                    for (size_t word = 0; word < reference.size(); ++word)
                                        if (reference[word] != routed[word])
                                        {
                                            ++components[word % 4];
                                            if (shown++ < 8) std::printf("WORD layer=%s texel=%zu component=%c reference=%08x cuda=%08x\n",
                                                spec.name, word / 4, "xyzw"[word % 4], reference[word], routed[word]);
                                        }
                                    std::printf("WORDS layer=%s x=%zu y=%zu z=%zu w=%zu\n", spec.name,
                                        components[0], components[1], components[2], components[3]);
                                }
                            }
                    std::printf("LAYER %s failures=%zu\n", spec.name, failures - beforeLayer);
                }
                const auto status = xefg_cuda::Snapshot();
                std::printf("V0_EQUIVALENCE cases=%zu failures=%zu launches=%llu fallbacks=%llu\n", cases, failures,
                    static_cast<unsigned long long>(status.launches), static_cast<unsigned long long>(status.fallbacks));
            }
            FreeLibrary(module);
            return failures ? 1 : 0;
        }
        xvk::TensorRoute route;
        std::string reason;
        const bool installed = route.Install(vk.desc, bytes, options, reason);
        std::printf("V0_IMPORT texturing=%s module=%s params=%s barriers=%s installed=%s reason=%s moduleResult=%d fault=0x%08lx moduleBytes=%zu launches=0 equivalence=not_reached\n",
            options.texturing64 ? "64" : "32", options.elfModule ? "elf" : "fatbin", options.argumentParams ? "args" : "extras",
            options.fullBarriers ? "full" : "xefg", installed ? "true" : "false", reason.c_str(), int(route.moduleResult()), route.faultCode(), route.moduleBytes());
        return installed ? 0 : 3;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "V0_SETUP_FAILURE %s\n", e.what()); return 2; }
}
