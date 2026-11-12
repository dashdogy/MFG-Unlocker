#pragma once
#include "dlss5_parameters.h"
#include <string>
namespace dlss5 {
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using AllocateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
struct Api {
    HMODULE runtime = nullptr;
    CreateFn create = nullptr;
    EvaluateFn evaluate = nullptr;
    ReleaseFn release = nullptr;
    AllocateFn allocate = nullptr;
    DestroyFn destroy = nullptr;
    bool Complete() const noexcept { return runtime && create && evaluate && release && allocate && destroy; }
};
// A direct provider owns a retained module and an import hook for the process.
// Its public API dispatch and caller handling are confined to this object's calls.
class DirectProvider {
public:
    bool Open(const wchar_t* absolutePath, HMODULE runtime) noexcept;
    NVSDK_NGX_Result Initialize(ID3D12Device*, NVSDK_NGX_Parameter*) noexcept;
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) noexcept;
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) noexcept;
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle*) noexcept;
    HMODULE Module() const noexcept { return module_; }
    bool Faulted() const noexcept { return faulted_; }
private:
    HMODULE module_ = nullptr, runtime_ = nullptr;
    using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    InitFn init_ = nullptr;
    CreateFn create_ = nullptr;
    EvaluateFn evaluate_ = nullptr;
    ReleaseFn release_ = nullptr;
    ID3D12Device* device_ = nullptr; // retained; never shut down somebody else's NGX instance
    bool initialized_ = false, faulted_ = false;
};
// The same direct route through the provider's Vulkan
// entries (its Init_Ext2, CreateFeature1, EvaluateFeature, ReleaseFeature).
// Vulkan handles and loader entry points stay opaque here: VkInstance,
// VkPhysicalDevice, VkDevice, VkCommandBuffer, PFN_vkGet*ProcAddr.
class VulkanDirectProvider {
public:
    bool Open(const wchar_t* absolutePath, HMODULE runtime) noexcept;
    NVSDK_NGX_Result Initialize(void* instance, void* physical, void* device, void* gipa, void* gdpa,
        NVSDK_NGX_Parameter*) noexcept;
    NVSDK_NGX_Result Create(void* device, void* command, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) noexcept;
    NVSDK_NGX_Result Evaluate(void* command, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) noexcept;
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle*) noexcept;
    HMODULE Module() const noexcept { return module_; }
    bool Faulted() const noexcept { return faulted_; }
private:
    using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, void*, void*, void*, void*, void*,
        NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
        PFN_NVSDK_NGX_ProgressCallback);
    HMODULE module_ = nullptr, runtime_ = nullptr;
    InitFn init_ = nullptr;
    CreateFn create_ = nullptr;
    EvaluateFn evaluate_ = nullptr;
    ReleaseFn release_ = nullptr;
    void* device_ = nullptr; // the VkDevice it was initialized on; never shut down
    bool initialized_ = false, faulted_ = false;
};
inline constexpr uint32_t kProviderFault = 0xBAD000FFu;
}
