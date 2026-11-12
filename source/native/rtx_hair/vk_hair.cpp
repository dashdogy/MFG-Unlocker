#include "rtx_hair/vk_hair.h"

#include "rtx_hair/lss_spirv.h"
#include "rtx_hair/universal_hair.h" // Requested(), CudaAdmits()
#include "rtx_hair/vk_dots_builder.h"
#include "rtx_hair/vk_hair_status.h"

#include "overlay_hook.h"
#include "single_module.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtx_hair::vk {
namespace {
using single_overlay::HookFamily;

constexpr const char* kLssExtension = VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME;
constexpr const char* kFetchExtension = VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME;
constexpr VkPipelineCreateFlags2 kLssPipelineFlag = VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES_BIT_NV;
constexpr uint32_t kDumpLimit = 256;

void Log(const std::string& text) noexcept
{
    try
    {
        const std::wstring wide(text.begin(), text.end());
        single_module::Log(wide.c_str());
    }
    catch (...)
    {
    }
}

bool Below(std::atomic<uint32_t>& counter, uint32_t limit)
{
    return counter.fetch_add(1, std::memory_order_relaxed) < limit;
}

template <class T>
T* FindChain(const void* chain, VkStructureType type)
{
    for (auto* s = static_cast<const VkBaseInStructure*>(chain); s; s = s->pNext)
        if (s->sType == type) return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(s));
    return nullptr;
}

// The game's own structures are edited in place (as the XeMFG Vulkan host does) only where that memory is writable.
bool Writable(const void* p, size_t bytes)
{
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (!(m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    return static_cast<const uint8_t*>(p) + bytes <= static_cast<const uint8_t*>(m.BaseAddress) + m.RegionSize;
}

std::filesystem::path GameFolder()
{
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n);
    return std::filesystem::path(path).parent_path();
}

// vkhair.4: [RtxHair] DumpSpirv=1 in RTXMFG.RtxHair.ini beside the game, or RTXMFG_RTX_HAIR_DUMP=1, writes each LSS
// module before and after the rewrite to RTXMFG-hair-spirv\ beside the game (at most 256). Off by default.
bool DumpRequested()
{
    static const bool requested = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"RTXMFG_RTX_HAIR_DUMP", value, static_cast<DWORD>(std::size(value)));
        if (length == 1 && (value[0] == L'0' || value[0] == L'1')) return value[0] == L'1';
        const auto folder = GameFolder();
        if (folder.empty()) return false;
        return GetPrivateProfileIntW(L"RtxHair", L"DumpSpirv", 0, (folder / L"RTXMFG.RtxHair.ini").c_str()) != 0;
    }();
    return requested;
}

// hairmem.1: [RtxHair] Ribbons and FastBuild in RTXMFG.RtxHair.ini beside the game (vk_dots_builder.h DotsOptions),
// read once. Ribbons=2 costs a third less hair memory and build work; a strand's apparent width varies more.
const DotsOptions& HairOptions()
{
    static const DotsOptions options = [] {
        DotsOptions o;
        const auto folder = GameFolder();
        if (folder.empty()) return o;
        const auto ini = (folder / L"RTXMFG.RtxHair.ini").wstring();
        o.ribbons = GetPrivateProfileIntW(L"RtxHair", L"Ribbons", 3, ini.c_str()) == 2 ? 2 : 3;
        o.fastBuild = GetPrivateProfileIntW(L"RtxHair", L"FastBuild", 0, ini.c_str()) != 0;
        return o;
    }();
    return options;
}

uint64_t Fnv(std::span<const uint32_t> words)
{
    uint64_t h = 1469598103934665603ull;
    for (const uint32_t w : words)
        for (int b = 0; b < 4; ++b)
        {
            h ^= (w >> (8 * b)) & 0xFFu;
            h *= 1099511628211ull;
        }
    return h;
}

struct Admission
{
    bool decided{}, admitted{};
    std::string reason;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa{};
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 budget{}; // hairmem.1: with VK_EXT_memory_budget on this GPU, for the summary line
    std::string name;
};

struct HairDevice
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    void* key{}; // the loader's dispatch table: the first word of the device and of each of its command buffers
    DeviceFunctions functions{};
    DotsBuilderVk builder;
    bool ready{};
};

struct State
{
    std::mutex lock;
    std::vector<std::pair<VkInstance, PFN_vkGetInstanceProcAddr>> instances;
    std::map<VkPhysicalDevice, Admission> admissions;
    std::vector<std::unique_ptr<HairDevice>> devices; // kept until their vkDestroyDevice
    std::string reason;
    bool seen{}, admitted{}, requested{}, prepared{}, failed{};
};
// Never destroyed: wrappers may still run while the process exits.
State& S()
{
    static State* state = new State;
    return *state;
}

std::atomic<HairDevice*> gHair{nullptr}; // the converted device (the first LSS device of the process)
std::atomic<uint64_t> gExtensionReports{}, gFeatureReports{}, gFlagsStripped{}, gRewritten{}, gNeutral{}, gRewriteFailed{}, gHostRefused{}, gLastBuild{};
std::atomic<uint32_t> gExtensionLogs{}, gFeatureLogs{}, gDeviceLogs{}, gShaderLogs{}, gFlagLogs{}, gBuildLogs{}, gDumps{}, gCensusLogs{}, gPipelineResultLogs{},
    gPipelineFailureLogs{};

HairDevice* Hair(VkDevice device)
{
    HairDevice* d = gHair.load(std::memory_order_acquire);
    return d && device && d->device == device ? d : nullptr;
}

HairDevice* HairOf(VkCommandBuffer list)
{
    HairDevice* d = gHair.load(std::memory_order_acquire);
    return d && list && *reinterpret_cast<void* const*>(list) == d->key ? d : nullptr;
}

// The compute pipeline each command buffer of the converted device has bound (vkCmdBindPipeline), cleared at
// vkBeginCommandBuffer. Sharded: games record from many threads.
struct Shard
{
    std::mutex lock;
    std::unordered_map<VkCommandBuffer, VkPipeline> bound;
};
std::array<Shard, 32>& Shards()
{
    static auto* shards = new std::array<Shard, 32>;
    return *shards;
}
Shard& ShardOf(VkCommandBuffer list)
{
    return Shards()[(reinterpret_cast<uintptr_t>(list) >> 4) & 31];
}
VkPipeline BoundCompute(VkCommandBuffer list)
{
    Shard& shard = ShardOf(list);
    std::lock_guard lock(shard.lock);
    const auto it = shard.bound.find(list);
    return it == shard.bound.end() ? VK_NULL_HANDLE : it->second;
}

// Whether `physical` is a GPU universal hair converts for (S().lock held).
Admission& Admit(VkPhysicalDevice physical)
{
    State& s = S();
    Admission& a = s.admissions[physical];
    if (a.decided) return a;
    a.decided = true;
    auto refuse = [&](std::string why) -> Admission& {
        a.reason = std::move(why);
        if (s.reason.empty() || !s.admitted) s.reason = a.reason;
        Log("RTX_HAIR universal hair not admitted (Vulkan): " + a.reason);
        return a;
    };
    if (!universal::Requested()) return refuse("turned off (Universal=0 or RTXMFG_RTX_HAIR=0)");
    for (const auto& [instance, gipa] : s.instances)
    {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        uint32_t count = 0;
        if (!enumerate || enumerate(instance, &count, nullptr) != VK_SUCCESS || !count || count > 64) continue;
        std::vector<VkPhysicalDevice> list(count);
        if (enumerate(instance, &count, list.data()) < 0) continue;
        if (std::find(list.begin(), list.begin() + count, physical) == list.begin() + count) continue;
        a.instance = instance;
        a.gipa = gipa;
        break;
    }
    if (!a.instance) return refuse("the Vulkan instance of this GPU is unknown");
    auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(a.gipa(a.instance, "vkGetPhysicalDeviceProperties2"));
    if (!properties2) properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(a.gipa(a.instance, "vkGetPhysicalDeviceProperties2KHR"));
    auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(a.gipa(a.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!features2) features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(a.gipa(a.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(a.gipa(a.instance, "vkEnumerateDeviceExtensionProperties"));
    auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(a.gipa(a.instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!properties2 || !features2 || !extensions || !memory) return refuse("Vulkan 1.1 physical-device queries are missing");
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    properties2(physical, &properties);
    a.name = properties.properties.deviceName;
    if (properties.properties.vendorID != 0x10de) return refuse(a.name + " is not an NVIDIA GPU");
    uint32_t n = 0;
    std::vector<VkExtensionProperties> list;
    if (extensions(physical, nullptr, &n, nullptr) == VK_SUCCESS && n && n < 4096)
    {
        list.resize(n);
        if (extensions(physical, nullptr, &n, list.data()) < 0) n = 0;
        list.resize(n);
    }
    auto has = [&](const char* name) {
        return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    if (has(kLssExtension)) return refuse(a.name + " has linear swept spheres itself (RTX 50): nothing to convert");
    if (!has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) return refuse(a.name + " has no Vulkan ray tracing");
    if (!has(kFetchExtension)) return refuse(a.name + ": the driver lists no VK_KHR_ray_tracing_position_fetch");
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR fetch{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &fetch};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &address};
    features2(physical, &features);
    if (!fetch.rayTracingPositionFetch || !address.bufferDeviceAddress) return refuse(a.name + ": position fetch or buffer device addresses unsupported");
    if (!id.deviceLUIDValid) return refuse(a.name + ": no LUID to match against CUDA");
    LUID luid{};
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    int major = 0, minor = 0;
    if (!universal::CudaAdmits(luid, major, minor))
        return refuse(a.name + " is not an RTX 40 or RTX 30 series GPU (CUDA " + std::to_string(major) + "." + std::to_string(minor) + ")");
    memory(physical, &a.memory);
    if (has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
    {
        a.budget = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(a.gipa(a.instance, "vkGetPhysicalDeviceMemoryProperties2"));
        if (!a.budget) a.budget = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(a.gipa(a.instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
    }
    a.admitted = true;
    s.admitted = true;
    s.reason.clear();
    Log("RTX_HAIR universal hair admitted (Vulkan): " + a.name + ", CUDA " + std::to_string(major) + "." + std::to_string(minor)
        + "; the LSS extension is reported to the game on it");
    return a;
}

bool Admitted(VkPhysicalDevice physical)
{
    if (!physical) return false;
    std::lock_guard lock(S().lock);
    return Admit(physical).admitted;
}

void Dump(std::span<const uint32_t> original, const std::vector<uint32_t>& rewritten, const char* kind)
{
    if (!DumpRequested() || !Below(gDumps, kDumpLimit)) return;
    try
    {
        const auto folder = GameFolder() / L"RTXMFG-hair-spirv";
        std::filesystem::create_directories(folder);
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(Fnv(original)));
        std::ofstream(folder / (std::string(name) + ".original.spv"), std::ios::binary)
            .write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size() * 4));
        if (!rewritten.empty())
            std::ofstream(folder / (std::string(name) + "." + kind + ".spv"), std::ios::binary)
                .write(reinterpret_cast<const char*>(rewritten.data()), static_cast<std::streamsize>(rewritten.size() * 4));
        if (gDumps.load() == 1) Log("RTX_HAIR SPIR-V dump on: LSS modules are written to " + folder.string());
    }
    catch (...)
    {
    }
}

// An LSS module rewritten (or put in its neutral form); false: leave the game's words as they are.
bool Rewrite(std::span<const uint32_t> words, std::vector<uint32_t>& out, const char* where)
{
    if (!SpirvUsesLss(words)) return false;
    SpirvRewriteReport report;
    std::string error;
    // hairmem.1: the decode reads the layout the converter writes ([RtxHair] Ribbons).
    LssDecodeLayout layout;
    layout.trianglesPerSegment = HairOptions().ribbons * 2;
    layout.ribbon = RibbonScale(HairOptions().ribbons);
    const SpirvRewrite result = RewriteLssSpirv(words, out, report, error, layout);
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(Fnv(words)));
    switch (result)
    {
    case SpirvRewrite::Rewritten:
        ++gRewritten;
        Dump(words, out, "rewritten");
        if (Below(gShaderLogs, 32))
            Log(std::string("RTX_HAIR SPIR-V rewritten (") + where + ", " + hash + "): " + std::to_string(words.size()) + " -> " + std::to_string(out.size())
                + " words, hitEntryPoints=" + std::to_string(report.hitEntryPoints) + " lssBuiltIns=" + std::to_string(report.builtIns) + " queryOps="
                + std::to_string(report.ops) + " reads=" + std::to_string(report.reads) + (report.note.empty() ? "" : " note: " + report.note));
        return true;
    case SpirvRewrite::Neutral:
        ++gNeutral;
        Dump(words, out, "neutral");
        if (Below(gShaderLogs, 32))
            Log(std::string("RTX_HAIR SPIR-V LSS module NOT converted (") + where + ", " + hash + "): neutral form, its LSS reads are zero: " + error);
        return true;
    case SpirvRewrite::Failed:
        ++gRewriteFailed;
        Dump(words, out, "failed");
        if (Below(gShaderLogs, 32)) Log(std::string("RTX_HAIR SPIR-V LSS module could not be read (") + where + ", " + hash + "), passed on as is: " + error);
        return false;
    default: return false;
    }
}

// Pipeline creation: the LSS pipeline flag cleared and inline LSS modules rewritten in the game's own structures, put
// back when the call returns (kept for a deferred operation, which may read them later).
class PipelineEdits
{
public:
    ~PipelineEdits()
    {
        if (keep_) return;
        for (const auto& [flags, value] : flags_) *flags = value;
        for (const auto& m : modules_)
        {
            m.info->pCode = m.code;
            m.info->codeSize = m.size;
        }
    }
    void Flags(const void* chain)
    {
        auto* f = FindChain<VkPipelineCreateFlags2CreateInfo>(chain, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
        if (!f || !(f->flags & kLssPipelineFlag) || !Writable(f, sizeof(*f))) return;
        flags_.push_back({&f->flags, f->flags});
        f->flags &= ~kLssPipelineFlag;
        ++gFlagsStripped;
        if (Below(gFlagLogs, 1)) Log("RTX_HAIR pipeline flag ALLOW_SPHERES_AND_LINEAR_SWEPT_SPHERES left out (hair shaders are rewritten to position fetch)");
    }
    void Stages(const VkPipelineShaderStageCreateInfo* stages, uint32_t count, const char* where)
    {
        for (uint32_t i = 0; stages && i < count; ++i)
        {
            if (stages[i].module) continue;
            if (FindChain<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(stages[i].pNext,
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)
                && Below(gCensusLogs, 8))
                Log("RTX_HAIR census: a pipeline stage given by module identifier only (cannot be rewritten if it is LSS)");
            auto* info = FindChain<VkShaderModuleCreateInfo>(stages[i].pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
            if (!info || !info->pCode || !Writable(info, sizeof(*info))) continue;
            auto& out = storage_.emplace_back();
            if (!Rewrite(std::span<const uint32_t>(info->pCode, info->codeSize / 4), out, where))
            {
                storage_.pop_back();
                continue;
            }
            modules_.push_back({info, info->pCode, info->codeSize});
            info->pCode = out.data();
            info->codeSize = out.size() * 4;
        }
    }
    // A deferred operation reads the structures later: leave them edited and the rewritten code alive.
    void Keep()
    {
        static std::mutex lock;
        static auto* kept = new std::deque<std::vector<uint32_t>>;
        std::lock_guard guard(lock);
        for (auto& words : storage_) kept->push_back(std::move(words));
        keep_ = true;
    }

private:
    std::vector<std::pair<VkPipelineCreateFlags2*, VkPipelineCreateFlags2>> flags_;
    struct Module
    {
        VkShaderModuleCreateInfo* info;
        const uint32_t* code;
        size_t size;
    };
    std::vector<Module> modules_;
    std::deque<std::vector<uint32_t>> storage_; // stable element addresses
    bool keep_{};
};

// ---------------------------------------------------------------------------------------------------------------
// Wrappers. Each passes straight through unless its device (or command buffer) is the converted one.

struct EnumerateExtensionsTag
{
    static VkResult Call(PFN_vkEnumerateDeviceExtensionProperties original, VkPhysicalDevice physical, const char* layer, uint32_t* count,
        VkExtensionProperties* properties)
    {
        if (layer || !count || !Admitted(physical)) return original(physical, layer, count, properties);
        uint32_t n = 0;
        VkResult r = original(physical, nullptr, &n, nullptr);
        if (r != VK_SUCCESS) return r;
        std::vector<VkExtensionProperties> list(n);
        r = original(physical, nullptr, &n, list.data());
        if (r < 0) return r;
        list.resize(n);
        if (std::none_of(list.begin(), list.end(), [](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, kLssExtension) == 0; }))
        {
            VkExtensionProperties lss{};
            strncpy_s(lss.extensionName, kLssExtension, _TRUNCATE);
            lss.specVersion = VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_SPEC_VERSION;
            list.push_back(lss);
        }
        if (!properties)
        {
            *count = static_cast<uint32_t>(list.size());
            return VK_SUCCESS;
        }
        const uint32_t given = std::min<uint32_t>(*count, static_cast<uint32_t>(list.size()));
        std::copy_n(list.begin(), given, properties);
        *count = given;
        ++gExtensionReports;
        if (Below(gExtensionLogs, 1)) Log("RTX_HAIR LSS extension reported (Vulkan): VK_NV_ray_tracing_linear_swept_spheres is listed to the game");
        return given < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }
};

struct Features2Tag
{
    static void Call(PFN_vkGetPhysicalDeviceFeatures2 original, VkPhysicalDevice physical, VkPhysicalDeviceFeatures2* features)
    {
        original(physical, features);
        if (!features) return;
        auto* lss = FindChain<VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV>(features->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV);
        if (!lss || !Admitted(physical)) return;
        lss->linearSweptSpheres = VK_TRUE;
        lss->spheres = VK_FALSE; // sphere primitives are not converted
        ++gFeatureReports;
        if (Below(gFeatureLogs, 1)) Log("RTX_HAIR census: the game asked for the LSS feature (linearSweptSpheres reported true, spheres false)");
    }
};

struct CreateShaderModuleTag
{
    static VkResult Call(PFN_vkCreateShaderModule original, VkDevice device, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator,
        VkShaderModule* module)
    {
        if (!Hair(device) || !info || !info->pCode || info->codeSize < 20) return original(device, info, allocator, module);
        std::vector<uint32_t> out;
        if (!Rewrite(std::span<const uint32_t>(info->pCode, info->codeSize / 4), out, "vkCreateShaderModule")) return original(device, info, allocator, module);
        VkShaderModuleCreateInfo copy = *info;
        copy.pCode = out.data();
        copy.codeSize = out.size() * 4;
        return original(device, &copy, allocator, module);
    }
};

struct CreateShadersTag
{
    static VkResult Call(PFN_vkCreateShadersEXT original, VkDevice device, uint32_t count, const VkShaderCreateInfoEXT* infos,
        const VkAllocationCallbacks* allocator, VkShaderEXT* shaders)
    {
        if (!Hair(device) || !count || !infos) return original(device, count, infos, allocator, shaders);
        std::vector<VkShaderCreateInfoEXT> copies(infos, infos + count);
        std::deque<std::vector<uint32_t>> storage;
        for (auto& c : copies)
        {
            if (c.codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT || !c.pCode || c.codeSize < 20 || (reinterpret_cast<uintptr_t>(c.pCode) & 3)) continue;
            auto& out = storage.emplace_back();
            if (!Rewrite(std::span<const uint32_t>(static_cast<const uint32_t*>(c.pCode), c.codeSize / 4), out, "vkCreateShadersEXT")) continue;
            c.pCode = out.data();
            c.codeSize = out.size() * 4;
        }
        return original(device, count, copies.data(), allocator, shaders);
    }
};

// vkhairchain.1: a refused pipeline leaves the game tracing with no program (a driver crash with nothing before it in
// the log). The first result on any device, then each failure.
void NoteRtPipelines(VkResult result, uint32_t count, const VkPipeline* pipelines, bool converted)
{
    if (result == VK_PIPELINE_COMPILE_REQUIRED) return; // the game's own early-return request, not a failure
    if (result < 0 ? !Below(gPipelineFailureLogs, 8) : !Below(gPipelineResultLogs, 1)) return;
    // A deferred operation writes the handles later, from another thread.
    uint32_t created = 0;
    for (uint32_t i = 0; result != VK_OPERATION_DEFERRED_KHR && pipelines && i < count; ++i)
        if (pipelines[i] != VK_NULL_HANDLE) ++created;
    Log("RTX_HAIR ray-tracing pipelines (Vulkan): result " + std::to_string(static_cast<int>(result)) + ", " + std::to_string(created) + " of "
        + std::to_string(count) + " created on " + (converted ? "the converted device" : "an unconverted device") + "; LSS modules so far: rewritten "
        + std::to_string(gRewritten.load()) + ", neutral " + std::to_string(gNeutral.load()) + ", unreadable " + std::to_string(gRewriteFailed.load()));
}

struct CreateRtPipelinesTag
{
    static VkResult Call(PFN_vkCreateRayTracingPipelinesKHR original, VkDevice device, VkDeferredOperationKHR deferred, VkPipelineCache cache, uint32_t count,
        const VkRayTracingPipelineCreateInfoKHR* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
    {
        if (!Hair(device) || !infos)
        {
            const VkResult passed = original(device, deferred, cache, count, infos, allocator, pipelines);
            NoteRtPipelines(passed, count, pipelines, false);
            return passed;
        }
        PipelineEdits edits;
        for (uint32_t i = 0; i < count; ++i)
        {
            edits.Flags(infos[i].pNext);
            edits.Stages(infos[i].pStages, infos[i].stageCount, "vkCreateRayTracingPipelinesKHR");
        }
        const VkResult r = original(device, deferred, cache, count, infos, allocator, pipelines);
        if (deferred && r == VK_OPERATION_DEFERRED_KHR) edits.Keep();
        NoteRtPipelines(r, count, pipelines, true);
        return r;
    }
};

// vkhairchain.1: the result of a pipeline built on a deferred operation only exists here.
struct DeferredResultTag
{
    static VkResult Call(PFN_vkGetDeferredOperationResultKHR original, VkDevice device, VkDeferredOperationKHR operation)
    {
        const VkResult r = original(device, operation);
        if (r < 0 && Below(gPipelineFailureLogs, 8)) Log("RTX_HAIR deferred operation failed (Vulkan): result " + std::to_string(static_cast<int>(r)));
        return r;
    }
};

struct CreateComputePipelinesTag
{
    static VkResult Call(PFN_vkCreateComputePipelines original, VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos,
        const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
    {
        if (!Hair(device) || !infos) return original(device, cache, count, infos, allocator, pipelines);
        PipelineEdits edits;
        for (uint32_t i = 0; i < count; ++i)
        {
            edits.Flags(infos[i].pNext);
            edits.Stages(&infos[i].stage, 1, "vkCreateComputePipelines");
        }
        return original(device, cache, count, infos, allocator, pipelines);
    }
};

struct CreateGraphicsPipelinesTag
{
    static VkResult Call(PFN_vkCreateGraphicsPipelines original, VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
        const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
    {
        if (!Hair(device) || !infos) return original(device, cache, count, infos, allocator, pipelines);
        PipelineEdits edits;
        for (uint32_t i = 0; i < count; ++i)
        {
            edits.Flags(infos[i].pNext);
            edits.Stages(infos[i].pStages, infos[i].stageCount, "vkCreateGraphicsPipelines");
        }
        return original(device, cache, count, infos, allocator, pipelines);
    }
};

struct BuildSizesTag
{
    static void Call(PFN_vkGetAccelerationStructureBuildSizesKHR original, VkDevice device, VkAccelerationStructureBuildTypeKHR type,
        const VkAccelerationStructureBuildGeometryInfoKHR* info, const uint32_t* counts, VkAccelerationStructureBuildSizesInfoKHR* sizes)
    {
        HairDevice* d = Hair(device);
        if (!d) return original(device, type, info, counts, sizes);
        d->builder.BuildSizes(original, type, info, counts, sizes);
    }
};

void NoteBuild(uint32_t count, const VkAccelerationStructureBuildGeometryInfoKHR* infos)
{
    for (uint32_t i = 0; infos && i < count; ++i)
        if (DotsBuilderVk::HasLss(infos[i]))
        {
            gLastBuild.store(GetTickCount64(), std::memory_order_relaxed);
            if (Below(gBuildLogs, 1)) Log("RTX_HAIR first hair BLAS recorded as DOTS triangles (Vulkan; GPU completion and game image not yet verified)");
            return;
        }
}

struct CmdBuildTag
{
    static void Call(PFN_vkCmdBuildAccelerationStructuresKHR original, VkCommandBuffer list, uint32_t count,
        const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkAccelerationStructureBuildRangeInfoKHR* const* ranges)
    {
        HairDevice* d = HairOf(list);
        if (!d) return original(list, count, infos, ranges);
        NoteBuild(count, infos);
        d->builder.CmdBuild(original, list, count, infos, ranges, BoundCompute(list));
    }
};

struct CmdBuildIndirectTag
{
    static void Call(PFN_vkCmdBuildAccelerationStructuresIndirectKHR original, VkCommandBuffer list, uint32_t count,
        const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkDeviceAddress* indirect, const uint32_t* strides, const uint32_t* const* maxCounts)
    {
        HairDevice* d = HairOf(list);
        if (!d) return original(list, count, infos, indirect, strides, maxCounts);
        if (Below(gCensusLogs, 8)) Log("RTX_HAIR census: indirect acceleration-structure builds (LSS ones are built from inactive triangles)");
        NoteBuild(count, infos);
        d->builder.CmdBuildIndirect(original, list, count, infos, indirect, strides, maxCounts, BoundCompute(list));
    }
};

struct HostBuildTag
{
    static VkResult Call(PFN_vkBuildAccelerationStructuresKHR original, VkDevice device, VkDeferredOperationKHR deferred, uint32_t count,
        const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkAccelerationStructureBuildRangeInfoKHR* const* ranges)
    {
        if (!Hair(device) || !infos) return original(device, deferred, count, infos, ranges);
        for (uint32_t i = 0; i < count; ++i)
            if (DotsBuilderVk::HasLss(infos[i]))
            {
                ++gHostRefused;
                if (Below(gCensusLogs, 8)) Log("RTX_HAIR census: a host LSS build is refused (not converted)");
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        // Bottom levels carry ALLOW_DATA_ACCESS, as their size queries did. A deferred operation may read the
        // infos after this call returns, so its copies are kept.
        static std::mutex keptLock;
        static auto* kept = new std::deque<std::vector<VkAccelerationStructureBuildGeometryInfoKHR>>;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> local(infos, infos + count);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR>* copies = &local;
        if (deferred)
        {
            std::lock_guard guard(keptLock);
            copies = &kept->emplace_back(std::move(local));
        }
        for (auto& c : *copies)
            if (c.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) c.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
        return original(device, deferred, count, copies->data(), ranges);
    }
};

// hairmem.1: the game's size for each acceleration structure, so the memory summary can say what its hair BLASes
// (and all its bottom levels) hold.
struct CreateAccelerationStructureTag
{
    static VkResult Call(PFN_vkCreateAccelerationStructureKHR original, VkDevice device, const VkAccelerationStructureCreateInfoKHR* info,
        const VkAllocationCallbacks* allocator, VkAccelerationStructureKHR* structure)
    {
        const VkResult r = original(device, info, allocator, structure);
        if (r == VK_SUCCESS && info && structure && *structure)
            if (HairDevice* d = Hair(device)) d->builder.Created(*structure, info->type, info->size);
        return r;
    }
};

struct DestroyAccelerationStructureTag
{
    static void Call(PFN_vkDestroyAccelerationStructureKHR original, VkDevice device, VkAccelerationStructureKHR structure, const VkAllocationCallbacks* allocator)
    {
        if (HairDevice* d = Hair(device); d && structure) d->builder.Destroyed(structure);
        original(device, structure, allocator);
    }
};

struct CmdCopyAccelerationStructureTag
{
    static void Call(PFN_vkCmdCopyAccelerationStructureKHR original, VkCommandBuffer list, const VkCopyAccelerationStructureInfoKHR* info)
    {
        if (HairDevice* d = HairOf(list); d && info) d->builder.Copied(info->src, info->dst);
        original(list, info);
    }
};

struct BindPipelineTag
{
    static void Call(PFN_vkCmdBindPipeline original, VkCommandBuffer list, VkPipelineBindPoint point, VkPipeline pipeline)
    {
        if (point == VK_PIPELINE_BIND_POINT_COMPUTE && HairOf(list))
        {
            Shard& shard = ShardOf(list);
            std::lock_guard lock(shard.lock);
            shard.bound[list] = pipeline;
        }
        original(list, point, pipeline);
    }
};

struct BeginCommandBufferTag
{
    static VkResult Call(PFN_vkBeginCommandBuffer original, VkCommandBuffer list, const VkCommandBufferBeginInfo* info)
    {
        if (HairOf(list))
        {
            Shard& shard = ShardOf(list);
            std::lock_guard lock(shard.lock);
            shard.bound.erase(list);
        }
        return original(list, info);
    }
};
} // namespace

struct DeviceEdit
{
    VkDeviceCreateInfo info{};
    std::vector<const char*> extensions;
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR fetch{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    Admission admission;
};

void DeviceEditDeleter::operator()(DeviceEdit* edit) const noexcept
{
    delete edit;
}

void InstanceCreated(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) noexcept
{
    if (!instance || !gipa) return;
    try
    {
        std::lock_guard lock(S().lock);
        S().seen = true;
        S().instances.emplace_back(instance, gipa);
    }
    catch (...)
    {
    }
}

void InstanceDestroyed(VkInstance instance) noexcept
{
    try
    {
        std::lock_guard lock(S().lock);
        auto& list = S().instances;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& r) { return r.first == instance; }), list.end());
        for (auto it = S().admissions.begin(); it != S().admissions.end();)
            it = it->second.instance == instance ? S().admissions.erase(it) : std::next(it);
    }
    catch (...)
    {
    }
}

DeviceEditPtr PrepareDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info) noexcept
{
    try
    {
        {
            std::lock_guard lock(S().lock);
            S().seen = true;
        }
        if (!physical || !info || !universal::Requested()) return nullptr;
        bool lss = false, fetch = false;
        std::string names;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        {
            const char* name = info->ppEnabledExtensionNames[i];
            if (!name) continue;
            names += (i ? " " : "") + std::string(name);
            lss = lss || std::strcmp(name, kLssExtension) == 0;
            fetch = fetch || std::strcmp(name, kFetchExtension) == 0;
        }
        auto* lssFeatures = FindChain<VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV>(info->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV);
        // Census: what the game enables, so its log shows whether it asks for LSS at all. The list goes in parts (a log
        // line holds 768 characters).
        if (Below(gDeviceLogs, 4))
        {
            Log("RTX_HAIR census: vkCreateDevice with " + std::to_string(info->enabledExtensionCount) + " extensions: LSS " +
                (lss ? "listed" : "not listed") + ", position fetch " + (fetch ? "listed" : "not listed") + ", LSS feature " +
                (!lssFeatures ? "not chained" : lssFeatures->linearSweptSpheres ? "chained on" : "chained off"));
            constexpr size_t kPart = 640, kLimit = 4000;
            for (size_t at = 0; at < names.size() && at < kLimit;)
            {
                size_t end = at + kPart;
                if (end >= names.size()) end = names.size();
                else if (const size_t space = names.rfind(' ', end); space != std::string::npos && space > at) end = space;
                Log("RTX_HAIR census: vkCreateDevice extensions: " + names.substr(at, end - at));
                at = end + 1;
            }
        }
        // vkhairchain.1: a game can chain an LSS features structure with LSS on without listing the extension (and still
        // take its LSS path), so that device is converted as if it were listed. The driver is never asked for LSS.
        const bool chainedOn = lssFeatures && (lssFeatures->linearSweptSpheres || lssFeatures->spheres);
        if (!lss && !chainedOn) return nullptr;
        Admission admission;
        {
            std::lock_guard lock(S().lock);
            admission = Admit(physical);
            S().requested = S().requested || admission.admitted;
        }
        if (!admission.admitted) return nullptr; // its own LSS (RTX 50), or a GPU it was not reported on
        // The converter reads through buffer device addresses: the game must enable them (every RT game does).
        bool addresses = false;
        if (auto* f12 = FindChain<VkPhysicalDeviceVulkan12Features>(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))
            addresses = f12->bufferDeviceAddress;
        if (auto* bda = FindChain<VkPhysicalDeviceBufferDeviceAddressFeatures>(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES))
            addresses = addresses || bda->bufferDeviceAddress;
        auto* fetchFeatures = FindChain<VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR>(info->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR);
        std::string refused;
        if (!addresses) refused = "the game's device has no buffer device addresses";
        else if (lssFeatures && !Writable(lssFeatures, sizeof(*lssFeatures))) refused = "the game's LSS feature structure is read-only";
        else if (fetchFeatures && !Writable(fetchFeatures, sizeof(*fetchFeatures))) refused = "the game's position-fetch feature structure is read-only";
        if (!refused.empty())
        {
            // Left unconverted, but the driver still isn't asked for a feature it doesn't have.
            if (!lss && lssFeatures && Writable(lssFeatures, sizeof(*lssFeatures)))
            {
                lssFeatures->linearSweptSpheres = VK_FALSE;
                lssFeatures->spheres = VK_FALSE;
            }
            std::lock_guard lock(S().lock);
            S().failed = true;
            S().reason = "conversion could not start: " + refused;
            Log("RTX_HAIR universal hair failed (Vulkan): " + S().reason);
            return nullptr;
        }
        auto edit = DeviceEditPtr(new DeviceEdit);
        edit->physical = physical;
        edit->admission = admission;
        edit->info = *info;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            if (info->ppEnabledExtensionNames[i] && std::strcmp(info->ppEnabledExtensionNames[i], kLssExtension) != 0)
                edit->extensions.push_back(info->ppEnabledExtensionNames[i]);
        if (!fetch) edit->extensions.push_back(kFetchExtension);
        edit->info.enabledExtensionCount = static_cast<uint32_t>(edit->extensions.size());
        edit->info.ppEnabledExtensionNames = edit->extensions.data();
        // In the game's own structures (as the XeMFG host sets its bits): LSS off, position fetch on.
        if (lssFeatures)
        {
            lssFeatures->linearSweptSpheres = VK_FALSE;
            lssFeatures->spheres = VK_FALSE;
        }
        if (fetchFeatures) fetchFeatures->rayTracingPositionFetch = VK_TRUE;
        else
        {
            edit->fetch.rayTracingPositionFetch = VK_TRUE;
            edit->fetch.pNext = const_cast<void*>(info->pNext);
            edit->info.pNext = &edit->fetch;
        }
        Log(std::string(lss ? "RTX_HAIR the game enabled LSS at vkCreateDevice" : "RTX_HAIR the game chained the LSS feature without the extension")
            + ": it is replaced by VK_KHR_ray_tracing_position_fetch on " + admission.name);
        return edit;
    }
    catch (...)
    {
        return nullptr;
    }
}

const VkDeviceCreateInfo* EditedInfo(const DeviceEdit* edit, const VkDeviceCreateInfo* original) noexcept
{
    return edit ? &edit->info : original;
}

void DeviceCreated(DeviceEditPtr edit, VkDevice device) noexcept
{
    if (!edit || !device) return;
    try
    {
        auto d = std::make_unique<HairDevice>();
        d->device = device;
        d->physical = edit->physical;
        d->key = *reinterpret_cast<void* const*>(device);
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(edit->admission.gipa(edit->admission.instance, "vkGetDeviceProcAddr"));
        std::string error;
        if (!gdpa || !LoadDeviceFunctions(device, gdpa, d->functions)) error = "device functions missing";
        else
        {
            d->builder.SetLog(&Log);
            d->builder.SetOptions(HairOptions());
            if (edit->admission.budget) d->builder.SetBudgetQuery(edit->admission.budget, edit->physical);
            if (d->builder.Initialize(device, d->functions, edit->admission.memory, error)) d->ready = true;
        }
        HairDevice* raw = d.get();
        {
            std::lock_guard lock(S().lock);
            S().devices.push_back(std::move(d));
            if (!raw->ready)
            {
                S().failed = true;
                S().reason = "conversion could not start: " + error;
            }
        }
        if (!raw->ready)
        {
            Log("RTX_HAIR universal hair failed (Vulkan): conversion could not start: " + error);
            return;
        }
        HairDevice* expected = nullptr;
        if (!gHair.compare_exchange_strong(expected, raw, std::memory_order_acq_rel))
        {
            Log("RTX_HAIR a second LSS device (Vulkan): only the first one is converted");
            return;
        }
        {
            std::lock_guard lock(S().lock);
            S().prepared = true;
        }
        const DotsOptions& options = HairOptions();
        Log("RTX_HAIR universal hair prepared (Vulkan) on " + edit->admission.name
            + ": LSS shaders are rewritten to position fetch, LSS builds become DOTS triangles (" + std::to_string(options.ribbons)
            + " ribbons, " + std::to_string(options.ribbons * 2) + " triangles per segment" + (options.fastBuild ? ", fast build" : "") + ")");
    }
    catch (...)
    {
    }
}

void DeviceDestroyed(VkDevice device) noexcept
{
    try
    {
        HairDevice* d = gHair.load(std::memory_order_acquire);
        std::lock_guard lock(S().lock);
        auto& list = S().devices;
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if ((*it)->device != device) continue;
            if (it->get() == d) gHair.store(nullptr, std::memory_order_release);
            (*it)->builder.Shutdown();
            // A wrapper that loaded gHair just before may still use it: the object stays (its device is gone).
            (*it)->ready = false;
            break;
        }
        for (auto& shard : Shards())
        {
            std::lock_guard guard(shard.lock);
            shard.bound.clear();
        }
    }
    catch (...)
    {
    }
}

PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction original) noexcept
{
    if (!name || !original || name[0] != 'v' || name[1] != 'k' || !universal::Requested()) return original;
#define MFG_HAIR_WRAP(nameLiteral, Tag, Result, ...) \
    if (std::strcmp(name, nameLiteral) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>(HookFamily<Tag, Result, __VA_ARGS__>::Bind(reinterpret_cast<void*>(original), false));
    MFG_HAIR_WRAP("vkEnumerateDeviceExtensionProperties", EnumerateExtensionsTag, VkResult, VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*)
    MFG_HAIR_WRAP("vkGetPhysicalDeviceFeatures2", Features2Tag, void, VkPhysicalDevice, VkPhysicalDeviceFeatures2*)
    MFG_HAIR_WRAP("vkGetPhysicalDeviceFeatures2KHR", Features2Tag, void, VkPhysicalDevice, VkPhysicalDeviceFeatures2*)
    MFG_HAIR_WRAP("vkCreateShaderModule", CreateShaderModuleTag, VkResult, VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*)
    MFG_HAIR_WRAP("vkCreateShadersEXT", CreateShadersTag, VkResult, VkDevice, uint32_t, const VkShaderCreateInfoEXT*, const VkAllocationCallbacks*, VkShaderEXT*)
    MFG_HAIR_WRAP("vkCreateRayTracingPipelinesKHR", CreateRtPipelinesTag, VkResult, VkDevice, VkDeferredOperationKHR, VkPipelineCache, uint32_t,
        const VkRayTracingPipelineCreateInfoKHR*, const VkAllocationCallbacks*, VkPipeline*)
    MFG_HAIR_WRAP("vkGetDeferredOperationResultKHR", DeferredResultTag, VkResult, VkDevice, VkDeferredOperationKHR)
    MFG_HAIR_WRAP("vkCreateComputePipelines", CreateComputePipelinesTag, VkResult, VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*,
        const VkAllocationCallbacks*, VkPipeline*)
    MFG_HAIR_WRAP("vkCreateGraphicsPipelines", CreateGraphicsPipelinesTag, VkResult, VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*,
        const VkAllocationCallbacks*, VkPipeline*)
    MFG_HAIR_WRAP("vkGetAccelerationStructureBuildSizesKHR", BuildSizesTag, void, VkDevice, VkAccelerationStructureBuildTypeKHR,
        const VkAccelerationStructureBuildGeometryInfoKHR*, const uint32_t*, VkAccelerationStructureBuildSizesInfoKHR*)
    MFG_HAIR_WRAP("vkCmdBuildAccelerationStructuresKHR", CmdBuildTag, void, VkCommandBuffer, uint32_t, const VkAccelerationStructureBuildGeometryInfoKHR*,
        const VkAccelerationStructureBuildRangeInfoKHR* const*)
    MFG_HAIR_WRAP("vkCmdBuildAccelerationStructuresIndirectKHR", CmdBuildIndirectTag, void, VkCommandBuffer, uint32_t,
        const VkAccelerationStructureBuildGeometryInfoKHR*, const VkDeviceAddress*, const uint32_t*, const uint32_t* const*)
    MFG_HAIR_WRAP("vkBuildAccelerationStructuresKHR", HostBuildTag, VkResult, VkDevice, VkDeferredOperationKHR, uint32_t,
        const VkAccelerationStructureBuildGeometryInfoKHR*, const VkAccelerationStructureBuildRangeInfoKHR* const*)
    MFG_HAIR_WRAP("vkCreateAccelerationStructureKHR", CreateAccelerationStructureTag, VkResult, VkDevice, const VkAccelerationStructureCreateInfoKHR*,
        const VkAllocationCallbacks*, VkAccelerationStructureKHR*)
    MFG_HAIR_WRAP("vkDestroyAccelerationStructureKHR", DestroyAccelerationStructureTag, void, VkDevice, VkAccelerationStructureKHR, const VkAllocationCallbacks*)
    MFG_HAIR_WRAP("vkCmdCopyAccelerationStructureKHR", CmdCopyAccelerationStructureTag, void, VkCommandBuffer, const VkCopyAccelerationStructureInfoKHR*)
    MFG_HAIR_WRAP("vkCmdBindPipeline", BindPipelineTag, void, VkCommandBuffer, VkPipelineBindPoint, VkPipeline)
    MFG_HAIR_WRAP("vkBeginCommandBuffer", BeginCommandBufferTag, VkResult, VkCommandBuffer, const VkCommandBufferBeginInfo*)
#undef MFG_HAIR_WRAP
    return original;
}

Status ReadStatus() noexcept
{
    Status s;
    try
    {
        {
            std::lock_guard lock(S().lock);
            s.seen = S().seen;
            s.admitted = S().admitted;
            s.requested = S().requested;
            s.prepared = S().prepared && gHair.load(std::memory_order_acquire);
            s.failed = S().failed;
            strncpy_s(s.reason, S().reason.c_str(), _TRUNCATE);
        }
        s.extensionReports = gExtensionReports.load(std::memory_order_relaxed);
        s.featureReports = gFeatureReports.load(std::memory_order_relaxed);
        s.pipelineFlagsStripped = gFlagsStripped.load(std::memory_order_relaxed);
        s.shadersRewritten = gRewritten.load(std::memory_order_relaxed);
        s.shadersNeutral = gNeutral.load(std::memory_order_relaxed);
        s.shadersFailed = gRewriteFailed.load(std::memory_order_relaxed);
        s.hostBuildsRefused = gHostRefused.load(std::memory_order_relaxed);
        s.lastBuildTick = gLastBuild.load(std::memory_order_relaxed);
        if (HairDevice* d = gHair.load(std::memory_order_acquire))
        {
            const DotsStats stats = d->builder.Stats();
            s.sizeQueries = stats.sizeQueries;
            s.builds = stats.builds;
            s.updates = stats.updates;
            s.rebuilds = stats.rebuilds;
            s.invalidGeometries = stats.invalidGeometries;
            s.indirectBuilds = stats.indirectBuilds;
            s.failures = stats.failures;
            s.topLevelPatched = stats.topLevelPatched;
            s.topLevelCopyBytes = stats.topLevelCopyBytes;
            s.hairBlas = stats.hairBlas;
            s.poolBuffers = stats.poolBuffers;
            s.poolBytes = stats.poolBytes;
            s.evictions = stats.evictions;
            s.spareBytes = stats.spareBytes;
            s.retiredBytes = stats.retiredBytes;
            s.reuses = stats.reuses;
            s.allocations = stats.allocations;
            s.hairDestroyed = stats.hairDestroyed;
            s.gameHairBlasBytes = stats.gameHairBlasBytes;
            s.gameBlasBytes = stats.gameBlasBytes;
            s.ribbons = d->builder.Options().ribbons;
            s.fastBuild = d->builder.Options().fastBuild;
            strncpy_s(s.lastError, d->builder.LastError().c_str(), _TRUNCATE);
        }
    }
    catch (...)
    {
    }
    return s;
}
}
