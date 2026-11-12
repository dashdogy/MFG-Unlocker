#pragma once

#include "dlss_sr_policy.h"

#include <Windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <cstddef>
#include <cstdint>

// DLSS Super Resolution and Ray Reconstruction detection and live preset
// switching on the NGX runtime D3D12 and Vulkan routes (see dlss_sr_policy.h).
// ampere_backend's runtime-route Create/Evaluate/Release thunks call in here
// for SuperSampling and RayReconstruction features only.
namespace dlss_sr
{
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
// NVSDK_NGX_VULKAN_CreateFeature (VkCommandBuffer) and _CreateFeature1
// (VkDevice, VkCommandBuffer); the Vulkan handles stay opaque here.
using VkCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using VkCreate1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// One route's Create entries on the game's own recording.
// D3D12 uses `d3d12` with an ID3D12GraphicsCommandList; Vulkan uses `vulkan`
// (CreateFeature) or `vulkan1` (CreateFeature1, with `device`) with a
// VkCommandBuffer. `api` names the entry of a Create; for an Evaluate it names
// the API, and the re-create enters the entry the game's own Create used.
struct CreateCall
{
    CreateApi api = CreateApi::eD3D12;
    CreateFn d3d12 = nullptr;
    VkCreateFn vulkan = nullptr;
    VkCreate1Fn vulkan1 = nullptr;
    void* device = nullptr;
    void* command = nullptr;
};
using LogCallback = void (*)(const wchar_t*);
// Installs the NvAPI DRS read hook for the game's D3D12 device (null on
// Vulkan, whose VkPhysicalDevice already selected the GPU); true once it is in place.
using DrsHookInstaller = bool (*)(ID3D12Device* device) noexcept;
using RuntimeEligibilityProbe = bool (*)() noexcept;

// SR: 0 = game/driver, 10..13 = presets J..M. RR: 0 = game/driver, 4..6 =
// presets D..F. Any other value selects 0.
void SetRequested(uint32_t preset) noexcept;
uint32_t Requested() noexcept;
void SetRequestedRr(uint32_t preset) noexcept;
uint32_t RequestedRr() noexcept;
uint32_t RequestedFor(Kind kind) noexcept;
// Reads the saved selections when NGX or a Create comes before the control
// worker published them. Called at most until SetRequested* runs, outside every lock.
using InitialSelectionLoader = uint32_t (*)() noexcept;
void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept;
void SetInitialRrSelectionLoader(InitialSelectionLoader loader) noexcept;
void SetLogCallback(LogCallback callback) noexcept;
void SetDrsHookInstaller(DrsHookInstaller installer) noexcept;
void SetRuntimeEligibilityProbe(RuntimeEligibilityProbe probe) noexcept;

// The providers read their DRS preset keys when NGX initializes them, so the
// read hook must be in place before NVSDK_NGX_D3D12_Init: call this from that
// boundary with the game's device (Creates call it again, for a later NGX
// initialization). Idempotent; outside every lock.
void PrepareForDevice(ID3D12Device* device) noexcept;
// The same before NVSDK_NGX_VULKAN_Init*, once the host's verified
// VkPhysicalDevice has selected the GPU family (the installer gets no device).
void PrepareForVulkan() noexcept;

// Any SR/RR feature or replaced handle is tracked (cheap, lock-free).
bool Active() noexcept;

constexpr bool Handles(NVSDK_NGX_Feature feature) noexcept
{
    return feature == NVSDK_NGX_Feature_SuperSampling || feature == NVSDK_NGX_Feature_RayReconstruction;
}

// A runtime-route Create of SR or RR through `call` (its entry and recording).
NVSDK_NGX_Result Create(size_t route, const CreateCall& call, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) noexcept;

// The handle a runtime-route Evaluate passes on: the game's own, or the
// feature of ours that replaced it. May re-create (with the route's own
// Create of the game's entry, on this Evaluate's command list or buffer) or
// release due replaced handles (with this API's own Release) first, on the
// calling render thread. Untracked handles come back unchanged.
const NVSDK_NGX_Handle* Evaluate(size_t route, const CreateCall& create, ReleaseFn release,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters) noexcept;

// The D3D12 forms of Create and Evaluate.
inline NVSDK_NGX_Result Create(size_t route, CreateFn original, ID3D12GraphicsCommandList* command,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) noexcept
{
    CreateCall call{};
    call.d3d12 = original;
    call.command = command;
    return Create(route, call, feature, parameters, output);
}
inline const NVSDK_NGX_Handle* Evaluate(size_t route, CreateFn create, ReleaseFn release,
    ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters) noexcept
{
    CreateCall call{};
    call.d3d12 = create;
    call.command = command;
    return Evaluate(route, call, release, handle, parameters);
}

// Around the game's own runtime-route Release: our replacements go first.
void BeforeRelease(size_t route, ReleaseFn release, NVSDK_NGX_Handle* handle) noexcept;
void AfterRelease(size_t route, NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result) noexcept;

// NvAPI DRS_GetSetting filter for the SR and RR preset keys, after the driver
// answered. Acts only on reads made by an NGX feature provider (a module that
// exports NVSDK_NGX_GetSnippetVersion); returns the result the caller receives.
int32_t FilterDrsRead(uint32_t id, int32_t result, dlssg_preset::Setting* setting, bool writable,
    HMODULE caller) noexcept;

// One provider's DRS preset read per session: seen, the driver's (NVIDIA app)
// value, answered "not set" because a preset was selected, and whether that
// value fixes the preset while a verified runtime selector is unavailable.
struct DrsView
{
    bool observed = false;
    bool driverValueValid = false;
    uint32_t driverValue = 0;
    bool suppressed = false;
    bool fixedByDriver = false;
    bool overrideCached = false;
    uint64_t reads = 0;
    uint64_t suppressedReads = 0;
};

struct Snapshot
{
    uint32_t requested = 0;
    uint32_t requestedRr = 0;
    bool detected = false;
    bool rrDetected = false;
    bool running = false;
    bool rayReconstructionRunning = false;
    Counters counters{};
    FeatureView primary{};
    FeatureView rrPrimary{};
    DrsView sr{};
    DrsView rr{};
    // SR/RR preset key reads left alone because they came from no NGX provider
    // or the buffer was not owned.
    uint64_t drsNotProvider = 0;
    uint64_t drsNotWritable = 0;
    // Game Creates DLSS refused with our preset and accepted with the game's own.
    uint64_t createRetries = 0;
};
Snapshot Read() noexcept;

#if defined(MFG_DLSS_SR_TESTING)
namespace testing
{
using Clock = uint64_t (*)() noexcept;
void SetClock(Clock clock) noexcept;
using ProviderCheck = bool (*)(HMODULE) noexcept;
void SetProviderCheck(ProviderCheck check) noexcept;
void Reset() noexcept;
}
#endif
}
