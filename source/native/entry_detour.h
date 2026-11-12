#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace entry_detour
{
// A Kind describes the ABI at an entry, not a unique process-wide slot.
// Multiple implementations of the same Kind may be installed concurrently.
enum class Kind : uint32_t
{
    eNgxD3D12CreateFeature = 0,
    eDlssgSetOptions = 1,
    eDlssgGetState = 2,
    eNgxD3D12EvaluateFeature = 3,
    eSlInit = 4,
    eNgxRuntimeD3D12CreateFeature = 5,
    eNgxRuntimeD3D12EvaluateFeature = 6,
    eSlSetData = 7,
    eSlGetData = 8,
    eSlFreeResources = 9,
    eNgxVulkanCreateFeature = 10,
    eNgxVulkanCreateFeature1 = 11,
    eNgxVulkanEvaluateFeature = 12,
    eNgxRuntimeVulkanCreateFeature = 13,
    eNgxRuntimeVulkanCreateFeature1 = 14,
    eNgxRuntimeVulkanEvaluateFeature = 15,
    eNgxVulkanAdapterInit = 16,
    eDlssgPresetBootstrap = 17,
    eDlssgPresetRead = 18,
    eDlssgCachedPresetRead = 19,
    eAmpereRelease = 20,
    eAmpereRequirements = 21,
    eAmpereParameters = 22,
    eAmperePreset = 23,
    eAmpereD3DDevice = 24,
    eReflexSetOptions = 25,
    eReflexSetData = 26,
    eReflexSleep = 27,
    eReflexGetState = 28,
    eNgxD3D12Init = 29,
    eNgxD3D12InitProject = 30,
    eSlUpgradeInterface = 31,
    eSlSetD3DDevice = 32,
    // OptiScaler's private interposer: hotpatch profile, installed at load.
    eOptiScalerSlSetD3DDevice = 33,
    eDlssgEndpointCreate = 34,
    eDlssgEndpointEvaluate = 35,
    // RTX 30 Vulkan route: NGX Vulkan ReleaseFeature (provider and runtime),
    // the runtime's Vulkan requirements/capability parameters, and the
    // interposer's device setup entries (slSetVulkanInfo, vkCreateDevice).
    eAmpereVulkanRelease = 36,
    eAmpereVulkanRequirements = 37,
    eAmpereVulkanParameters = 38,
    eSlSetVulkanInfo = 39,
    eSlVulkanCreateDevice = 40,
    eSrPresetSelector = 41,
    eSrPresetReader = 42,
    eSrPresetReport = 43,
    // nrouter.1: OptiScaler's own D3D12 NGX exports, serving the game's DLSS calls (relocation path).
    eOptiScalerNgxD3D12Create = 44,
    eOptiScalerNgxD3D12Evaluate = 45,
    eOptiScalerNgxD3D12Release = 46,
    // dx11nr.1: the NGX runtime's D3D11 entries, carrying DLSS 5 on DirectX 11 games (relocation path).
    eNgxRuntimeD3D11Create = 47,
    eNgxRuntimeD3D11Evaluate = 48,
    eNgxRuntimeD3D11Release = 49,
    eCount,
};

enum class Method : uint32_t
{
    eUnavailable = 0,
    eHotpatch = 1,
    eRelocated = 2,
};

enum class Failure : uint32_t
{
    eNone = 0,
    eInvalidArgument = 1,
    eRegistryFull = 2,
    eUnsupportedMitigation = 3,
    eOwnerMismatch = 4,
    eUnsupportedEntry = 5,
    eEntryNotHotpatchable = 6,
    eTargetPinFailed = 7,
    eRelayAllocationFailed = 8,
    eProtectionChangeFailed = 9,
    eAtomicPublicationFailed = 10,
    eProtectionRestoreFailed = 11,
    ePostWriteVerificationFailed = 12,
    eHookConflict = 13,
    eRelocatorInitializeFailed = 14,
    eRelocatorCreateFailed = 15,
    eRelocatorEnableFailed = 16,
    eForwardRelayFailed = 17,
};

// Handles remain valid for the lifetime of the process. Installed entries pin
// their owner because already-cached function pointers may outlive discovery.
struct Handle
{
    uint32_t slot = UINT32_MAX;
    uint32_t serial = 0;

    explicit operator bool() const noexcept
    {
        return slot != UINT32_MAX && serial != 0;
    }

    friend bool operator==(Handle left, Handle right) noexcept
    {
        return left.slot == right.slot && left.serial == right.serial;
    }
};

struct InstallOptions
{
    // Monotonic load generation assigned by module discovery. It is part of
    // the registry key, so a stale discovery record cannot impersonate a new
    // module instance at a recycled base address.
    uint64_t generation = 0;

    // When false, installation is limited to the atomic hotpatch-entry form.
    // When true, a pinned MinHook/HDE64 relocation fallback is permitted after
    // the strict hotpatch profile has been rejected.
    bool allowRelocated = false;

    // Optional fast-path filter for forwarding detours. When enabled, calls
    // whose second register argument does not match this value jump directly
    // to the original trampoline without entering the pre-call callback. This
    // keeps shared NGX runtime entries transparent to unrelated features.
    bool filterForwardArg2 = false;
    uintptr_t requiredForwardArg2 = 0;

    // For loader-notification callers that hold the loader lock: the install
    // never waits for the registry lock (it returns false if another install
    // is in progress) and never uses the relocation path, whose thread
    // suspension must not run under the loader lock.
    bool nonBlocking = false;
};

struct Snapshot
{
    bool installed = false;
    bool current = false;
    bool cachedPointersCovered = false;
    bool forwarding = false;
    // Not current, but the entry starts with a jump (another hook's
    // over our relocated five-byte jump, or ours back after one) and the bytes
    // after it are unchanged; `original` still forwards to the target.
    bool overlaid = false;
    Failure failure = Failure::eNone;
    Method method = Method::eUnavailable;
    Kind kind = Kind::eCount;
    Handle handle{};
    HMODULE owner = nullptr;
    void* target = nullptr;
    void* original = nullptr;
    uint32_t targetRva = 0;
    uint64_t generation = 0;
    uint32_t matchingEntries = 0;
    uint32_t currentEntries = 0;
};

// Installs one code-entry detour keyed by kind + target + owner + generation.
// The returned trampoline is unique to this target. With allowRelocated=false
// this uses the two-byte atomic hotpatch publication, covering cached pointers.
bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline, const InstallOptions& options,
    Handle* installedHandle = nullptr) noexcept;

// Compatibility overload for callers which only need one strict hotpatch
// entry. New routing code should retain the returned Handle and query it.
bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline) noexcept;

// Provider/runtime forwarding preserves the original caller return address.
// The callback receives the four original register arguments, the first two
// stack arguments, the exact detour handle and that caller address. The
// assembly dispatcher then tail-forwards through this entry's own trampoline.
using ForwardPreCall = void (WINAPI*)(void* arg1, uintptr_t arg2,
    const void* arg3, void* arg4, uintptr_t arg5, uintptr_t arg6,
    Handle handle,
    const void* originalCaller) noexcept;

using ForwardGate = bool (WINAPI*)(void*, uintptr_t, const void*, void*,
    uintptr_t, uintptr_t, Handle, const void*) noexcept;

bool InstallForwarding(Kind kind, HMODULE owner, void* target,
    ForwardPreCall preCall, void*& originalTrampoline,
    const InstallOptions& options, Handle* installedHandle = nullptr,
    ForwardGate gate = nullptr, void* rejectedTarget = nullptr) noexcept;

Snapshot ReadSnapshot(Handle handle) noexcept;

// Aggregate compatibility view. It reports the newest current entry of this
// kind and includes counts; readiness decisions should use the exact Handle.
Snapshot ReadSnapshot(Kind kind) noexcept;

// Owner-scoped aggregate used to pair Create/Evaluate entries from the same
// provider or runtime module before the first Evaluate call occurs.
Snapshot ReadSnapshot(Kind kind, HMODULE owner) noexcept;

size_t RegistryCapacity() noexcept;
const char* MethodName(Method method) noexcept;
const char* FailureName(Failure failure) noexcept;
}
