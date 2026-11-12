// vkprov.1, pluginprov.1: the XeMFG DLSS-G provider's nvngx_dlssg entry points, inside MFGUnlocker. GetProc below hands
// them to OptiScaler's RegisterProvider (FGOutput DLSSG, FGNvngxReplacement = MFGUnlocker); the work happens in
// ngx_provider (MfgXeMFGProviderGetApi, mfg_xemfg_provider.h).
// ngxprov.1: Vulkan and D3D12. GetProc hands out nothing when the provider can't run here (Refusal): OptiScaler then
// falls back to another provider. Handed out anyway, every feature would fail, and OptiScaler's Vulkan input
// dereferences the handle a failed CreateFeature never wrote.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "mfg_xemfg_provider.h"
#include "xess/ngx_provider.h"
#include "xess/ngx_provider_registration.h"
#include <cstring>
#include <nvsdk_ngx_params.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Whether the running MFGUnlocker's API has `field` (version 1 is append-only; vkprov.1-.4 end at MaxGeneratedFrames).
#define MFG_PROVIDES(api, field) \
    ((api) && (api)->size >= offsetof(MfgXeMFGProviderApi, field) + sizeof(void*) && (api)->field)

namespace
{
constexpr int kFeatureFrameGeneration = 11;   // NVSDK_NGX_Feature_FrameGeneration

struct Handle   // NVSDK_NGX_Handle's layout (its Id first), plus the provider's feature
{
    unsigned int Id;
    void* feature;
    bool d3d12;
};

std::atomic<unsigned int> gNextId{1};

const MfgXeMFGProviderApi* Api() { return ngx_provider::GetApi(MFG_XEMFG_PROVIDER_API_VERSION); }

const MfgXeMFGProviderApi* D3d12Api()
{
    const auto* api = Api();
    return MFG_PROVIDES(api, D3D12Shutdown) && api->D3D12Init && api->D3D12CreateFeature && api->D3D12Evaluate &&
                   api->D3D12ReleaseFeature
               ? api
               : nullptr;
}

NVSDK_NGX_Result Result(int32_t value) { return static_cast<NVSDK_NGX_Result>(value); }

NVSDK_NGX_Result Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                      PFN_vkGetDeviceProcAddr gdpa)
{
    const auto* api = Api();
    return api ? Result(api->VulkanInit(instance, physical, device, gipa, gdpa)) : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

// vkprov.3: the two DLSS-G callbacks a provider publishes (OptiScaler's own provider, Nvngx_FFX, sets the same).
// Streamline asks the first whether it must evaluate every frame, the second for the feature's memory.
NVSDK_NGX_Result NVSDK_CONV CurrentSettings(const void*, NVSDK_NGX_Parameter* parameters)
{
    if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    parameters->Set("DLSSG.MustCallEval", 1);
    parameters->Set("DLSSG.BurstCaptureRunning", 0);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV EstimateVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         size_t* size)
{
    // 4K at 6x: two input frames, five outputs and XeFG's own buffers.
    if (size) *size = size_t(640) << 20;
    return NVSDK_NGX_Result_Success;
}

// MultiFrameCountMax 0 (and FrameGeneration.Available 0) once MFGUnlocker refuses: OptiScaler then falls back to
// another provider.
void Populate(NVSDK_NGX_Parameter* parameters)
{
    const auto* api = Api();
    const unsigned int generated = api ? api->MaxGeneratedFrames() : 0;
    parameters->Set("FrameGeneration.Available", generated ? 1 : 0);
    parameters->Set("FrameGeneration.NeedsUpdatedDriver", 0);
    parameters->Set("FrameGeneration.FeatureInitResult", int(generated ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported));
    parameters->Set("DLSSG.MultiFrameCountMax", generated);
    if (generated) {
        parameters->Set("DLSSG.GetCurrentSettingsCallback", reinterpret_cast<void*>(&CurrentSettings));
        parameters->Set("DLSSG.EstimateVRAMCallback", reinterpret_cast<void*>(&EstimateVram));
    }
}

void Requirement(NVSDK_NGX_FeatureRequirement* requirement, bool supported)
{
    *requirement = {};
    requirement->FeatureSupported = supported ? NVSDK_NGX_FeatureSupportResult_Supported : NVSDK_NGX_FeatureSupportResult_NotImplemented;
}

NVSDK_NGX_Handle* Wrap(void* feature, bool d3d12)
{
    return reinterpret_cast<NVSDK_NGX_Handle*>(new Handle{gNextId++, feature, d3d12});
}
}

// ---------------------------------------------------------------- Vulkan
static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Init(unsigned long long, const wchar_t*, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, const void*, int)
{
    return Init(instance, physical, device, gipa, gdpa);
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Init_Ext(unsigned long long, const wchar_t*, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, int, const void*)
{
    return Init(instance, physical, device, nullptr, nullptr);
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Init_Ext2(unsigned long long, const wchar_t*, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, int, const void*)
{
    return Init(instance, physical, device, gipa, gdpa);
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown()
{
    const auto* api = Api();
    return api ? Result(api->Shutdown()) : NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown1(VkDevice) { return NVSDK_NGX_VULKAN_Shutdown(); }

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_GetScratchBufferSize(int, const NVSDK_NGX_Parameter*, size_t* size)
{
    if (!size) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *size = 0;
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_GetFeatureRequirements(VkInstance, VkPhysicalDevice, const void*,
    NVSDK_NGX_FeatureRequirement* requirement)
{
    if (!requirement) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto* api = Api();
    Requirement(requirement, api && api->MaxGeneratedFrames());
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_CreateFeature1(VkDevice device, VkCommandBuffer command, int feature,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    if (!handle || !parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *handle = nullptr;
    const auto* api = Api();
    if (!api || feature != kFeatureFrameGeneration) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    void* created = nullptr;
    const NVSDK_NGX_Result result = Result(api->VulkanCreateFeature(device, command, parameters, &created));
    if (result != NVSDK_NGX_Result_Success || !created) return result == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : result;
    *handle = Wrap(created, false);
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer command, int feature,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    return NVSDK_NGX_VULKAN_CreateFeature1(VK_NULL_HANDLE, command, feature, parameters, handle);
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* wrapped = reinterpret_cast<Handle*>(handle);
    if (wrapped->d3d12) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const auto* api = Api();
    const NVSDK_NGX_Result result = api ? Result(api->ReleaseFeature(wrapped->feature)) : NVSDK_NGX_Result_Success;
    delete wrapped;
    return result;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_EvaluateFeature(VkCommandBuffer command, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters, void*)
{
    if (!handle || !parameters || !command) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto* wrapped = reinterpret_cast<const Handle*>(handle);
    if (wrapped->d3d12) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const auto* api = Api();
    if (!api) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return Result(api->VulkanEvaluate(command, wrapped->feature, parameters));
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    Populate(parameters);
    return NVSDK_NGX_Result_Success;
}

// ---------------------------------------------------------------- D3D12 (ngxprov.1)
// Device and command-list pointers stay void* here: these forward them, they never call them.
static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, void* device, const void*, int)
{
    const auto* api = D3d12Api();
    if (!api) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return Result(api->D3D12Init(static_cast<ID3D12Device*>(device)));
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, void* device, int, const void*)
{
    const auto* api = D3d12Api();
    if (!api) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return Result(api->D3D12Init(static_cast<ID3D12Device*>(device)));
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown()
{
    const auto* api = D3d12Api();
    return api ? Result(api->D3D12Shutdown()) : NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(void*) { return NVSDK_NGX_D3D12_Shutdown(); }

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(void*, const void*, NVSDK_NGX_FeatureRequirement* requirement)
{
    if (!requirement) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto* api = D3d12Api();
    Requirement(requirement, api && api->MaxGeneratedFrames());
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(void* list, int feature, NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** handle)
{
    if (!handle || !parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *handle = nullptr;
    const auto* api = D3d12Api();
    if (!api || feature != kFeatureFrameGeneration) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    void* created = nullptr;
    const NVSDK_NGX_Result result = Result(api->D3D12CreateFeature(static_cast<ID3D12GraphicsCommandList*>(list), parameters, &created));
    if (result != NVSDK_NGX_Result_Success || !created) return result == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_Fail : result;
    *handle = Wrap(created, true);
    return NVSDK_NGX_Result_Success;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* wrapped = reinterpret_cast<Handle*>(handle);
    if (!wrapped->d3d12) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const auto* api = D3d12Api();
    const NVSDK_NGX_Result result = api ? Result(api->D3D12ReleaseFeature(wrapped->feature)) : NVSDK_NGX_Result_Success;
    delete wrapped;
    return result;
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(void* list, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters, void*)
{
    if (!handle || !parameters || !list) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto* wrapped = reinterpret_cast<const Handle*>(handle);
    if (!wrapped->d3d12) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const auto* api = D3d12Api();
    if (!api) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return Result(api->D3D12Evaluate(static_cast<ID3D12GraphicsCommandList*>(list), wrapped->feature, parameters));
}

static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(int, const NVSDK_NGX_Parameter*, size_t* size)
{
    if (size) *size = 0;
    return NVSDK_NGX_Result_Success;
}

// OptiScaler reads the provider's maximum through this entry whenever the provider has it, on Vulkan too.
static NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    Populate(parameters);
    return NVSDK_NGX_Result_Success;
}

namespace ngx_provider_registration
{
namespace
{
std::atomic<bool> gRegistered{false};
}

void NoteRegistered() noexcept { gRegistered.store(true, std::memory_order_release); }
bool Registered() noexcept { return gRegistered.load(std::memory_order_acquire); }

void* GetProc(void*, const char* exportName) noexcept
{
    if (!exportName) return nullptr;
    // With every entry null OptiScaler uses another provider. Refusal is lock-free.
    const auto* api = Api();
    if (!api) return nullptr;
    if (MFG_PROVIDES(api, Refusal)) {
        char why[96]{};
        if (api->Refusal(why, sizeof(why))) return nullptr;
    }
#define MFG_NGX_ENTRY(name) {#name, reinterpret_cast<void*>(&name)}
    static const struct { const char* name; void* entry; } kEntries[] = {
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_Init),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_Init_Ext),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_Init_Ext2),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_Shutdown),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_Shutdown1),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_GetScratchBufferSize),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_GetFeatureRequirements),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_CreateFeature1),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_CreateFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_ReleaseFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_EvaluateFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_VULKAN_PopulateParameters_Impl),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_Init),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_Init_Ext),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_Shutdown),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_Shutdown1),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_GetFeatureRequirements),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_CreateFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_ReleaseFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_EvaluateFeature),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_GetScratchBufferSize),
        MFG_NGX_ENTRY(NVSDK_NGX_D3D12_PopulateParameters_Impl),
    };
#undef MFG_NGX_ENTRY
    for (const auto& item : kEntries)
        if (std::strcmp(item.name, exportName) == 0) return item.entry;
    return nullptr;
}
}
