#pragma once
#include "dlss5_provider.h"
#include "dlss_sr_policy.h"
#include <Windows.h>
#include <nvsdk_ngx.h>
#include <cstddef>

// DLSS 5 (DLSSNR) on the game's Vulkan DLSS SR/DLAA/RR
// evaluations, After upscaling (and Hook Method Auto). The stage reads the
// evaluation's output, runs the D3D12 stage's encode/network/decode passes
// (the same HLSL compiled to SPIR-V, dlss5_vk_spirv.h) and writes the result
// back into the output, all on the game's command buffer right after its
// evaluation. The DLSSNR feature goes through the NGX route the game's DLSS
// uses (its Vulkan CreateFeature/EvaluateFeature), or the provider beside the
// game directly (its own NVSDK_NGX_VULKAN_Init_Ext2) when NGX refuses it.
//
// Completion is proven without a queue hook: every recording ends with a
// sequence number the GPU writes into host-visible memory of ours
// (vkCmdUpdateBuffer after the work, then a host barrier). Features, textures
// and parameters are released, and a measurement set reused, only once that
// number arrived; a recording that never executes keeps them forever.
// Settings, state and counters are dlss5.cpp's (dlss5::shared). Vulkan
// handles stay opaque here.
namespace dlss5::vulkan {
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void* command, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using Create1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(void* device, void* command, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void* command, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
// Release and the parameter entries have the D3D12 route's types (dlss5_provider.h).
using dlss5::ReleaseFn, dlss5::AllocateFn, dlss5::DestroyFn;
// The NGX route's Vulkan entries (the originals behind our hooks).
struct Api {
    HMODULE runtime = nullptr;
    CreateFn create = nullptr;
    Create1Fn create1 = nullptr;
    EvaluateFn evaluate = nullptr;
    ReleaseFn release = nullptr;
    AllocateFn allocate = nullptr;
    DestroyFn destroy = nullptr;
    bool Complete() const noexcept { return runtime && (create || create1) && evaluate && release && allocate && destroy; }
};
// The NGX Vulkan Init pre-calls: the game's VkInstance, VkPhysicalDevice and
// (base Init) VkDevice. Null arguments keep what was observed before.
void ObserveInit(void* instance, void* physical, void* device) noexcept;
// A game DLSS SR/RR Create on the Vulkan runtime route succeeded; `device` is
// CreateFeature1's VkDevice (null for CreateFeature).
void GameCreated(size_t route, const NVSDK_NGX_Handle* handle, const dlss_sr::CreateKeys& keys, void* device) noexcept;
// After the game's Vulkan evaluation of `handle` on `command` returned `result`.
void AfterEvaluate(size_t route, const Api& api, void* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Result result) noexcept;
void BeforeRelease(size_t route, const NVSDK_NGX_Handle* handle) noexcept;
void AfterRelease(size_t route, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) noexcept;
}
