#include "ampere_backend.h"
#include "fault_capture.h"
#include "ngx_runtime_dispatch.h"
#include "gpu_dispatch.h"
#include "ampere_patterns.h"
#include "ampere_provider_arch.h"
#include "adapter_discovery.h"
#include "ampere_host_contracts.h"
#include "ampere_policy.h"
#include "ampere_startup_requirements.h"
#include "ampere_flip_metering.h"
#include "ampere_vulkan_output.h"
#include "dlssg_provider_policy.h"
#include "dlssg_provider_selection.h"
#include "dlssg_preset.h"
#include "dlss_sr.h"
#include "dlss5.h"
#include "dlss5_d3d11.h"
#include "dlss5_vulkan.h"
#include "ngx_initialization.h"
#include "fg_lifetime_tracker.h"
#include "xess/vulkan/vk_host.h"
#include "xess/d3d12_host.h"
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
#include "midpoint_fix.h"
#endif
#include "protected_pointer.h"
#include "third_party/minhook/src/hde/hde64.h"
#include <d3d12.h>
#include <dxgi.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <intrin.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace ampere_backend
{
namespace
{
using namespace ampere_patterns;
constexpr size_t kRoutes = 8;
constexpr auto kRejected = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using RequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PresetFn = bool(*)(uint32_t, uint32_t*);
// NGX Vulkan entries (VkCommandBuffer, VkDevice, VkInstance and VkPhysicalDevice
// are opaque here; no Vulkan SDK header is added to the core).
using VkCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkCreate1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using VkRequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
using VulkanOutput = ampere_vulkan_output::Output;

struct Image
{
    HMODULE module{};
    uintptr_t base{};
    uint32_t size{};
    const IMAGE_NT_HEADERS64* nt{};
    bool Open(HMODULE value) noexcept
    {
        module = value; base = reinterpret_cast<uintptr_t>(value);
        __try
        {
            const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(value);
            if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
            nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
            size = nt->OptionalHeader.SizeOfImage;
            const uintptr_t sectionEnd = reinterpret_cast<uintptr_t>(IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections);
            return size >= 4096 && size < 1024u * 1024u * 1024u && sectionEnd >= base && sectionEnd - base <= size;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Contains(uintptr_t address, size_t bytes = 1) const noexcept
    { return address >= base && address - base < size && bytes <= size - (address - base); }
    bool Executable(uintptr_t address) const noexcept
    {
        MEMORY_BASIC_INFORMATION m{};
        return Contains(address) && VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof(m)) == sizeof(m)
            && m.AllocationBase == module && m.Type == MEM_IMAGE && m.State == MEM_COMMIT
            && (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
            && ((m.Protect & 0xff) == PAGE_EXECUTE_READ || (m.Protect & 0xff) == PAGE_EXECUTE);
    }
};

std::recursive_mutex gCalls;
std::mutex gInstall;
std::atomic<LogCallback> gLog{nullptr};
std::atomic<bool> gFatal{false}, gStartupComplete{false};
std::atomic<bool> gEarlyInit{false};
std::atomic<bool> gD3D12Confirmed{false};
// Vulkan: the verified VkPhysicalDevice confirms the adapter; gVulkanDevice is
// the host's VkDevice on it (identity only; the host owns its lifetime).
std::atomic<bool> gVulkanConfirmed{false};
std::atomic<uintptr_t> gVulkanDevice{0};
bool DeviceConfirmed() noexcept { return gD3D12Confirmed.load() || gVulkanConfirmed.load(); }
std::atomic<uint32_t> gFailure{100};
std::atomic<uint32_t> gFatalFailure{0};
std::atomic<uint64_t> gPresetQueries{0}, gEvaluations{0}, gRejections{0};
// The prepared (pinned) provider whose preset read this route
// recorded last, for the status preset fields (PresetProvider).
std::atomic<uintptr_t> gPresetProvider{0};
struct LegacyPresetRouteProof
{
    std::atomic<HMODULE> provider{nullptr};
    std::atomic<uint64_t> generation{0};
};
std::array<LegacyPresetRouteProof, kRoutes> gLegacyPresetRoutes{};
std::atomic<uint32_t> gNativeMaximum{0};
std::atomic<uint32_t> gNativeMaximumResult{static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_NotInitialized)};
std::atomic<uint32_t> gStartupMaximum{0}, gPresentationBuffers{0};
std::atomic<uint32_t> gCertifiedMaximum{0};
// The ring (generated frames) the last accepted Create certified on
// the active wrapper generation; it outlives a refused evaluation, not the wrapper.
std::atomic<uint32_t> gRingMaximum{0};
std::atomic<uint64_t> gRingWrapperGeneration{0};
std::atomic<uint32_t> gStartupFailureMask{0};
std::atomic<uintptr_t> gActiveWrapper{0};
std::atomic<uint64_t> gActiveWrapperGeneration{0};
std::atomic_flag gWrapperWriter = ATOMIC_FLAG_INIT;
std::atomic<int32_t> gLastCreateCount{-1};
std::atomic<uint64_t> gCreatedFeatures{0}, gSubmittedBatches{0};
std::atomic<uint64_t> gFailedBatches{0}, gRecoveredBatches{0}, gOutputReuseLatches{0}, gFlipMeteredRepeats{0};
std::atomic<uint64_t> gBufferRefreshes{0};
std::atomic<uint64_t> gCreateAttempts{0}, gCreateBlockedBeforeProvider{0}, gEvaluateAttempts{0};
std::atomic<bool> gPipelineCreateObserved{false};
std::atomic<uint64_t> gCandidateProviderVersion{0};
// Live preset switching: FG lifetime on the runtime route.
fg_lifetime::Tracker<> gFgLifetime;
// The runtime Vulkan entry the patcher's gate diverted on
// this thread (DivertNativeVulkanCall). Its route thunk consumes it on entry;
// the relay jumps there directly, with no other call in between.
thread_local entry_detour::Handle gNativeVulkanDivert{};
ampere_diagnostics::Failures gFailures;
// FG requirement queries during slInit/startup on an RTX 30/20 system. The
// first few are logged: Streamline keeps or drops sl.dlss_g on this answer.
constexpr uint32_t kStartupRequirementReports = 8;
std::atomic<uint32_t> gStartupRequirementQueries{0}, gInitRequirementQueries{0};
thread_local unsigned gStartupDepth = 0;
thread_local unsigned gPreparationDepth = 0;
std::atomic<PreparationBoundaryResolver> gPreparationBoundaryResolver{nullptr};
thread_local unsigned gInitDepth = 0;
thread_local unsigned gInitLockDepth = 0;
thread_local unsigned gCreateDepth = 0;
void Report(const wchar_t* text) noexcept { if (auto log = gLog.load()) log(text); }
bool Reject(uint32_t failure) noexcept
{
    if (failure == 151 && ampere_gpu::FailureCode())
        gFailures.Record(ampere_gpu::FailureCode());
    gFailures.Record(failure);
    gCertifiedMaximum.store(0, std::memory_order_release);
    if (gFatal.load())
    {
        uint32_t empty = 0;
        gFatalFailure.compare_exchange_strong(empty, failure);
        if (empty) { gRejections.fetch_add(1); return false; }
    }
    if (gFailure.exchange(failure) != failure)
    {
        wchar_t message[96]{};
        swprintf_s(message, L"Ampere boundary rejected: failure=%u", failure);
        Report(message);
    }
    gRejections.fetch_add(1);
    return false;
}

bool Pin(HMODULE module) noexcept
{
    HMODULE pinned = nullptr;
    return module && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module;
}

// Signature masks permit relocation operands, not opcode or layout changes.
// Decode the whole window and require every relative call/LEA and branch to
// belong to the same image. No driver version or fixed RVA grants admission.
bool DecodedWindow(const Image& image, uintptr_t address, size_t length) noexcept
{
    size_t offset = 0;
    while (offset < length)
    {
        hde64s ins{};
        const unsigned n = hde64_disasm(reinterpret_cast<void*>(address + offset), &ins);
        if (!n || (ins.flags & F_ERROR) || n > length - offset) return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(address + offset);
        if (ins.opcode == 0xe8)
        {
            int32_t displacement = 0; memcpy(&displacement, bytes + n - 4, 4);
            if (!image.Executable(address + offset + n + displacement)) return false;
        }
        if (ins.opcode == 0x8d && ins.modrm_mod == 0 && ins.modrm_rm == 5)
        {
            int32_t displacement = 0; memcpy(&displacement, bytes + n - 4, 4);
            if (!image.Contains(address + offset + n + displacement)) return false;
        }
        offset += n;
    }
    return offset == length;
}

uintptr_t FindPattern(const Image& image, const uint8_t* pattern, const uint8_t* mask,
    size_t length, size_t branchOffset = SIZE_MAX, uint32_t minimum = 0, uint32_t maximum = 0,
    unsigned* matches = nullptr) noexcept
{
    if (matches) *matches = 0;
    uintptr_t match = 0;
    const auto* sections = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned i = 0; i < image.nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = sections[i];
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || s.VirtualAddress >= image.size) continue;
        const uint32_t begin = std::max<uint32_t>(s.VirtualAddress, minimum);
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(
            std::min<uint64_t>(image.size, maximum ? maximum : image.size),
            uint64_t{s.VirtualAddress} + std::max(s.Misc.VirtualSize, s.SizeOfRawData)));
        for (uint64_t offset = begin; offset + length <= end; ++offset)
        {
            const auto* candidate = reinterpret_cast<const uint8_t*>(image.base + offset);
            bool same = true;
            for (size_t j = 0; same && j < length; ++j) same = !mask[j] || candidate[j] == pattern[j];
            if (!same || !DecodedWindow(image, image.base + offset, length)) continue;
            if (branchOffset != SIZE_MAX)
            {
                const int displacement = static_cast<int8_t>(candidate[branchOffset + 1]);
                if (displacement <= 0 || !image.Executable(image.base + offset + branchOffset + 2 + displacement)) continue;
            }
            if (matches) ++*matches;
            if (match) return 0;
            match = image.base + offset;
        }
    }
    return match;
}

bool UnwindRoot(const Image& image, RUNTIME_FUNCTION function, RUNTIME_FUNCTION& root) noexcept
{
    for (unsigned depth = 0; depth != 8; ++depth)
    {
        const uintptr_t unwind = image.base + function.UnwindData;
        if (!image.Contains(unwind, 4)) return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(unwind);
        if ((bytes[0] & 7u) != 1u) return false;
        const unsigned flags = bytes[0] >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) { root = function; return true; }
        if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) return false;
        const size_t offset = (4u + 2u * bytes[2] + 3u) & ~size_t{3};
        if (!image.Contains(unwind, offset + sizeof(RUNTIME_FUNCTION))) return false;
        RUNTIME_FUNCTION parent{};
        memcpy(&parent, bytes + offset, sizeof(parent));
        DWORD64 owner = 0;
        const auto* actual = RtlLookupFunctionEntry(image.base + parent.BeginAddress, &owner, nullptr);
        if (!actual || owner != image.base
            || !image.Contains(reinterpret_cast<uintptr_t>(actual), sizeof(*actual))
            || memcmp(actual, &parent, sizeof(parent))) return false;
        function = parent;
    }
    return false;
}

bool FunctionBounds(const Image& image, uintptr_t entry, uint32_t& begin, uint32_t& end) noexcept
{
    DWORD64 owner = 0;
    const auto* function = RtlLookupFunctionEntry(entry, &owner, nullptr);
    if (!function || owner != image.base
        || !image.Contains(reinterpret_cast<uintptr_t>(function), sizeof(*function))
        || function->BeginAddress >= function->EndAddress
        || function->EndAddress > image.size
        || image.base + function->BeginAddress != entry
        || function->EndAddress - function->BeginAddress > 0x4000
        || !image.Executable(entry) || !image.Executable(image.base + function->EndAddress - 1))
        return false;
    begin = function->BeginAddress;
    end = function->EndAddress;
    RUNTIME_FUNCTION root{};
    if (!UnwindRoot(image, *function, root) || root.BeginAddress != begin) return false;
    // MSVC splits one function's unwind coverage when a nonvolatile register
    // is saved only on one branch. Adjacent entries with an exact chained
    // unwind parent are continuations, not independent exported functions.
    for (unsigned fragments = 0; fragments != 32; ++fragments)
    {
        owner = 0;
        const auto* next = RtlLookupFunctionEntry(image.base + end, &owner, nullptr);
        if (!next || owner != image.base
            || !image.Contains(reinterpret_cast<uintptr_t>(next), sizeof(*next))
            || next->BeginAddress != end) return true;
        RUNTIME_FUNCTION nextRoot{};
        if (!UnwindRoot(image, *next, nextRoot)) return false;
        if (memcmp(&root, &nextRoot, sizeof(root))) return true;
        if (next->EndAddress <= end || next->EndAddress > image.size
            || next->EndAddress - begin > 0x4000
            || !image.Executable(image.base + next->EndAddress - 1)) return false;
        end = next->EndAddress;
    }
    return false;
}

struct Patch
{
    uintptr_t address = 0;
    uint8_t original = 0;
    bool active = false;
    bool privatePage = false;
    bool Set(bool enable) noexcept
    {
        if (!address) return false;
        if (active == enable) return *reinterpret_cast<const uint8_t*>(address) == (enable ? 0xeb : original);
        const uint8_t expected = enable ? original : 0xeb;
        const uint8_t replacement = enable ? 0xeb : original;
        // An entry detour can already have made this image page private.
        // Re-arming WRITECOPY on such a page fails on Windows; establish its
        // working-set ownership before selecting the private-page route.
        PSAPI_WORKING_SET_EX_INFORMATION working{};
        working.VirtualAddress = reinterpret_cast<void*>(address);
        const bool alreadyPrivate = QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
            && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
        const auto result = protected_pointer::ReplaceProtectedBytes(address,
            &expected, &replacement, 1, PAGE_EXECUTE_READ, &VirtualProtect,
            &FlushInstructionCache, privatePage || alreadyPrivate ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY);
        privatePage |= result.replacementWasPublished;
        if (result.disposition != protected_pointer::PublishDisposition::ePublishedRestored)
        { gFatal.store(true); return Reject(110); }
        active = enable;
        return true;
    }
};

struct Route
{
    HMODULE module{};
    uint64_t generation{};
    bool runtime = false;
    bool prepared = false;
    bool runtimeInspected = false;
    bool runtimeDriverOwned = false;
    bool deviceGateReady = false;
    bool legacyPresetReady = false;
    uint32_t nativeMaximum = 0;
    uint32_t startupMaximum = 0;
    bool wrapperInspected = false;
    bool candidateLogged = false;
    ampere_wrapper::Layout wrapperLayout{};
    entry_detour::Handle create{}, evaluate{}, release{}, requirements{}, parameters{}, preset{};
    entry_detour::ForwardPreCall beforeCreate{}, beforeEvaluate{};
    // NGX Vulkan entries of the same module. Create/Create1/Evaluate are the
    // patcher's forwarding entries (its pre-calls are kept for the RTX 30 thunks).
    entry_detour::Handle vkCreate{}, vkCreate1{}, vkEvaluate{}, vkRelease{}, vkRequirements{}, vkParameters{};
    entry_detour::ForwardPreCall vkBeforeCreate{}, vkBeforeCreate1{}, vkBeforeEvaluate{};
    // dx11nr.1: the runtime's D3D11 Create, Evaluate and Release (DLSS 5 on DirectX 11 games, dlss5_d3d11.h).
    entry_detour::Handle d3d11Create{}, d3d11Evaluate{}, d3d11Release{};
    Patch availability{};
    std::array<ampere_policy::Feature, 16> features{};
    uint64_t nextFeatureLifetime = 0;
    struct OtherFeature
    {
        enum class State : uint8_t { eFree, eCreating, eActive, eReleasing };
        uintptr_t handle = 0;
        uint64_t lifetime = 0;
        uint32_t activeCalls = 0;
        State state = State::eFree;

        OtherFeature() = default;
        OtherFeature(uintptr_t value) noexcept { *this = value; }
        OtherFeature& operator=(uintptr_t value) noexcept
        {
            handle = value;
            lifetime = value ? 1 : 0;
            activeCalls = 0;
            state = !value ? State::eFree
                : value == UINTPTR_MAX ? State::eCreating : State::eActive;
            return *this;
        }
        friend bool operator==(const OtherFeature& feature, uintptr_t value) noexcept
        { return feature.handle == value && (value == 0 || feature.state != State::eFree); }
        explicit operator bool() const noexcept
        { return state != State::eFree && handle != 0; }
        bool Free() const noexcept { return state == State::eFree; }
        bool Active(uintptr_t value) const noexcept
        { return state == State::eActive && handle == value && lifetime != 0; }
        void Clear() noexcept { *this = {}; }
    };
    std::vector<OtherFeature> otherFeatures;
    uint64_t nextOtherFeatureLifetime = 0;
};
std::array<Route, kRoutes> gRoutes{};

// Create entry kinds of one API; Vulkan hosts may enter either Create or Create1.
bool RuntimeCreateKind(entry_detour::Kind kind, GraphicsApi api) noexcept
{
    return api == GraphicsApi::eVulkan
        ? kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature || kind == entry_detour::Kind::eNgxRuntimeVulkanCreateFeature1
        : kind == entry_detour::Kind::eNgxRuntimeD3D12CreateFeature;
}
bool ProviderCreateKind(entry_detour::Kind kind, GraphicsApi api) noexcept
{
    return api == GraphicsApi::eVulkan
        ? kind == entry_detour::Kind::eNgxVulkanCreateFeature || kind == entry_detour::Kind::eNgxVulkanCreateFeature1
        : kind == entry_detour::Kind::eNgxD3D12CreateFeature;
}
// The single current Create entry of `owner` for this API (Vulkan: Create1 is
// preferred, as Streamline enters it first). An empty snapshot when ambiguous.
entry_detour::Snapshot CreateEntry(HMODULE owner, bool runtime, GraphicsApi api) noexcept
{
    using entry_detour::Kind;
    if (api != GraphicsApi::eVulkan)
        return entry_detour::ReadSnapshot(runtime ? Kind::eNgxRuntimeD3D12CreateFeature : Kind::eNgxD3D12CreateFeature, owner);
    const auto create1 = entry_detour::ReadSnapshot(
        runtime ? Kind::eNgxRuntimeVulkanCreateFeature1 : Kind::eNgxVulkanCreateFeature1, owner);
    return create1.current ? create1 : entry_detour::ReadSnapshot(
        runtime ? Kind::eNgxRuntimeVulkanCreateFeature : Kind::eNgxVulkanCreateFeature, owner);
}
struct CreateAttemptProof
{
    entry_detour::Handle runtimeCreate{}, providerCreate{};
    uintptr_t runtime = 0, provider = 0, wrapper = 0;
    uint64_t runtimeGeneration = 0, providerGeneration = 0, wrapperGeneration = 0;
    uint64_t token = 0, publication = 0, luid = 0;
    ampere_wrapper::Layout layout{};
    ampere_wrapper::Capacity capacity{};
    bool active = false, valid = false;
    GraphicsApi api = GraphicsApi::eD3D12;
};
std::mutex gCreateAttemptMutex;
std::array<CreateAttemptProof, 64> gCreateAttemptsInProgress{};
thread_local size_t gCurrentCreateAttempt = SIZE_MAX;

bool CreateAttemptProofCurrent(const CreateAttemptProof& proof) noexcept
{
    if (!Ready() || !proof.valid || !proof.token || !proof.runtime || !proof.provider || !proof.wrapper
        || !proof.runtimeGeneration || !proof.providerGeneration || !proof.wrapperGeneration
        || !proof.publication || !proof.luid) return false;
    const auto runtime = entry_detour::ReadSnapshot(proof.runtimeCreate);
    const auto provider = entry_detour::ReadSnapshot(proof.providerCreate);
    return runtime.current && RuntimeCreateKind(runtime.kind, proof.api)
        && reinterpret_cast<uintptr_t>(runtime.owner) == proof.runtime
        && runtime.generation == proof.runtimeGeneration
        && provider.current && ProviderCreateKind(provider.kind, proof.api)
        && reinterpret_cast<uintptr_t>(provider.owner) == proof.provider
        && provider.generation == proof.providerGeneration
        && proof.provider == reinterpret_cast<uintptr_t>(ampere_gpu::Provider())
        && proof.publication == ampere_gpu::Publication() && proof.luid == ampere_gpu::AdapterLuid()
        && proof.wrapper == gActiveWrapper.load(std::memory_order_acquire)
        && proof.wrapperGeneration == gActiveWrapperGeneration.load(std::memory_order_acquire)
        && proof.layout.module == reinterpret_cast<HMODULE>(proof.wrapper)
        && ampere_wrapper::Matches(proof.layout, proof.capacity);
}

// Native calls remain serialized by gCalls. These bounded snapshots let a
// reentrant/control thread distinguish a proven unfinished Create from stale
// retained ownership without acquiring gCalls under a Streamline callback.
struct CreateAttemptScope
{
    size_t slot = SIZE_MAX;
    explicit CreateAttemptScope(const Route& runtime) noexcept
        : CreateAttemptScope(runtime, runtime.create, GraphicsApi::eD3D12) {}
    CreateAttemptScope(const Route& runtime, entry_detour::Handle create, GraphicsApi api) noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (gCurrentCreateAttempt != SIZE_MAX) return;
        for (size_t i = 0; i < gCreateAttemptsInProgress.size(); ++i)
        {
            auto& proof = gCreateAttemptsInProgress[i];
            if (proof.active) continue;
            proof = {};
            proof.runtimeCreate = create;
            proof.runtime = reinterpret_cast<uintptr_t>(runtime.module);
            proof.runtimeGeneration = runtime.generation;
            proof.publication = ampere_gpu::Publication();
            proof.luid = ampere_gpu::AdapterLuid();
            proof.api = api;
            proof.active = proof.valid = true;
            slot = gCurrentCreateAttempt = i;
            break;
        }
    }
    CreateAttemptScope(const CreateAttemptScope&) = delete;
    CreateAttemptScope& operator=(const CreateAttemptScope&) = delete;
    ~CreateAttemptScope() { Finish(); }
    explicit operator bool() const noexcept { return slot != SIZE_MAX; }
    void BindCapacity(const Route& wrapper, const ampere_wrapper::Capacity& capacity) noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (slot == SIZE_MAX) return;
        auto& proof = gCreateAttemptsInProgress[slot];
        proof.wrapper = reinterpret_cast<uintptr_t>(wrapper.module);
        proof.wrapperGeneration = wrapper.generation;
        proof.layout = wrapper.wrapperLayout;
        proof.capacity = capacity;
    }
    CreateAttemptProof Read() const noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        return slot == SIZE_MAX ? CreateAttemptProof{} : gCreateAttemptsInProgress[slot];
    }
    void Finish() noexcept
    {
        std::lock_guard lock(gCreateAttemptMutex);
        if (slot == SIZE_MAX) return;
        gCreateAttemptsInProgress[slot] = {};
        if (gCurrentCreateAttempt == slot) gCurrentCreateAttempt = SIZE_MAX;
        slot = SIZE_MAX;
    }
};

void AbortCurrentCreateAttempt() noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return;
    gCreateAttemptsInProgress[gCurrentCreateAttempt] = {};
    gCurrentCreateAttempt = SIZE_MAX;
}

bool BindCurrentCreateProvider(const Route& provider, entry_detour::Handle create) noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return false;
    auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    if (!proof.active || !proof.valid) return false;
    const auto owner = reinterpret_cast<uintptr_t>(provider.module);
    if (proof.provider && (proof.provider != owner || proof.providerGeneration != provider.generation
        || proof.providerCreate != create))
    { proof.valid = false; return false; }
    const uint64_t publication = ampere_gpu::Publication();
    if (ampere_gpu::Provider() != provider.module || !publication
        || proof.luid != ampere_gpu::AdapterLuid()
        || (proof.provider && proof.publication != publication))
    { proof.valid = false; return false; }
    // The first concrete provider binding selects one of the already-published
    // candidates. After that selection its immutable program cannot change.
    proof.publication = publication;
    proof.provider = owner;
    proof.providerGeneration = provider.generation;
    proof.providerCreate = create;
    return true;
}

uint64_t CurrentCreateAttemptToken() noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return 0;
    const auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    return proof.active && proof.valid ? proof.token : 0;
}

struct PendingFeatureLifetimeEvent
{
    FeatureLifetimeEvent event{};
    CreateAttemptProof create{};
};
std::atomic<FeatureLifetimeCallback> gFeatureLifetimeCallback{nullptr};
std::mutex gFeatureLifetimeDeliveryMutex;
std::deque<PendingFeatureLifetimeEvent> gPendingFeatureLifetimeEvents;
PendingFeatureLifetimeEvent gDeliveringFeatureLifetimeEvent{};
bool gFeatureLifetimeDeliveryActive = false;
thread_local Route* gCallingRuntime = nullptr;
thread_local Route* gCreatingProvider = nullptr;
thread_local ngx_runtime_dispatch::Selection gCreatingDispatch{};
thread_local Route* gCallingWrapper = nullptr;
thread_local ampere_wrapper::Capacity gCreatingCapacity{};
thread_local ampere_policy::Feature* gCallingFeature = nullptr;
thread_local unsigned gEvaluateDepth = 0, gReleaseDepth = 0;
thread_local bool gProviderEvaluated = false;
struct EvaluationInput
{
    int count = 0, index = 0;
    uint64_t frame = 0;
    // Output identity: the ID3D12Resource, or for Vulkan the VkImage behind
    // the NVSDK_NGX_Resource_VK view (its descriptor can be per call).
    void* output = nullptr;
    bool operator==(const EvaluationInput&) const = default;
};
struct EvaluationReads
{
    uint32_t count = 0, index = 0, frame = 0, output = 0;
    bool exception = false;
};
thread_local EvaluationInput gEvaluationInput{};

// The D3D12 and Vulkan calls differ only in the NGX entry call (`forward`).
// Ownership TLS, SEH completion and batch failure bookkeeping are the same.
template<class Forward>
NVSDK_NGX_Result InvokeCreate(Route& route, Route*& provider, Route& wrapper,
    const ampere_wrapper::Capacity& capacity, uint64_t& createAttemptToken, Forward forward)
{
    NVSDK_NGX_Result result = kRejected;
    bool completed = false;
    createAttemptToken = 0;
    ++gCreateDepth;
    gCallingRuntime = &route; gCreatingProvider = nullptr;
    gCallingWrapper = &wrapper; gCreatingCapacity = capacity;
    __try
    {
        result = forward();
        createAttemptToken = CurrentCreateAttemptToken();
        completed = true;
    }
    __finally
    {
        --gCreateDepth;
        provider = gCreatingProvider;
        gCallingRuntime = nullptr; gCreatingProvider = nullptr;
        gCreatingDispatch = {};
        gCallingWrapper = nullptr; gCreatingCapacity = {};
        // /EHsc does not unwind an outer C++ scope for a caught SEH exception.
        if (!completed) AbortCurrentCreateAttempt();
    }
    return result;
}

NVSDK_NGX_Result InvokeRelease(Route& route, ReleaseFn original, NVSDK_NGX_Handle* handle,
    ampere_policy::Feature& feature)
{
    gCallingRuntime = &route; gCallingFeature = &feature; ++gReleaseDepth;
    __try { return original(handle); }
    __finally { --gReleaseDepth; gCallingRuntime = nullptr; gCallingFeature = nullptr; }
}

// EvaluateFn with an ID3D12GraphicsCommandList, or VkEvaluateFn with a VkCommandBuffer.
template<class EvaluateEntry, class Command>
NVSDK_NGX_Result InvokeEvaluate(Route& route, EvaluateEntry original, Command* command,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
    ampere_policy::Feature& feature, bool& providerExecuted)
{
    gCallingRuntime = &route; gCallingFeature = &feature; ++gEvaluateDepth; gProviderEvaluated = false;
    NVSDK_NGX_Result result = kRejected;
    __try { result = original(command, handle, parameters, callback); return result; }
    __finally
    {
        providerExecuted = gProviderEvaluated;
        if (result != NVSDK_NGX_Result_Success || !providerExecuted)
            feature.batch.Fail(gEvaluationInput.frame, gEvaluationInput.count, gEvaluationInput.index);
        gEvaluationInput = {};
        --gEvaluateDepth; gCallingRuntime = nullptr; gCallingFeature = nullptr;
    }
}

struct VulkanCreateCall
{
    VkCreateFn create{};
    VkCreate1Fn create1{};
    void* device = nullptr;
    void* command = nullptr;
    NVSDK_NGX_Feature feature{};
    const NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle** output = nullptr;
    NVSDK_NGX_Result Forward() const
    {
        return create1 ? create1(device, command, feature, parameters, output)
            : create(command, feature, parameters, output);
    }
};

template<class F> F Original(entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return state.current ? reinterpret_cast<F>(state.original) : nullptr;
}

template<class F> F RouteOriginal(const Route& route, entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    // Entry owners and trampolines are pinned for the process lifetime. Take
    // the exact route/generation snapshot while gCalls protects route metadata,
    // before neutral forwarding drops the Ampere lifecycle lock.
    return state.current && state.owner == route.module && state.generation == route.generation
        ? reinterpret_cast<F>(state.original) : nullptr;
}

// Another hook installed over one of our runtime entries (GitHub issue 115)
// leaves it not current, while the calls still reach us through that hook. Off
// the translated (RTX 30/20) program, NVIDIA's own call then continues through
// this entry's trampoline when entry_detour proves the other hook replaced only
// our entry jump. Otherwise, and always for the translated program, the entry
// fails closed.
template<class F> F NativeOriginal(const Route& route, entry_detour::Handle handle) noexcept
{
    if (const auto original = RouteOriginal<F>(route, handle)) return original;
    if (gpu_dispatch::UsesTranslatedProgram()) return nullptr;
    const auto state = entry_detour::ReadSnapshot(handle);
    if (!state.overlaid || !state.original || !route.module || state.owner != route.module
        || state.generation != route.generation) return nullptr;
    ReportOverlaidEntry(state);
    return reinterpret_cast<F>(state.original);
}

bool RouteEntryCurrent(const Route& route, entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return route.module && route.generation && state.current
        && state.owner == route.module && state.generation == route.generation;
}

size_t ReserveOtherFeature(Route& route) noexcept
{
    // Reserve before calling native Create: it may reenter Create and grow the
    // vector. A stable index, rather than an iterator, survives that growth.
    uint64_t lifetime = ++route.nextOtherFeatureLifetime;
    if (!lifetime) lifetime = ++route.nextOtherFeatureLifetime;
    const auto free = std::find_if(route.otherFeatures.begin(), route.otherFeatures.end(),
        [](const Route::OtherFeature& feature) { return feature.Free(); });
    if (free != route.otherFeatures.end())
    {
        const size_t index = static_cast<size_t>(free - route.otherFeatures.begin());
        free->handle = 0;
        free->lifetime = lifetime;
        free->activeCalls = 0;
        free->state = Route::OtherFeature::State::eCreating;
        return index;
    }
    try
    {
        route.otherFeatures.emplace_back();
        auto& feature = route.otherFeatures.back();
        feature.lifetime = lifetime;
        feature.state = Route::OtherFeature::State::eCreating;
    }
    catch (...) { return SIZE_MAX; }
    return route.otherFeatures.size() - 1;
}

struct OtherFeatureLease
{
    size_t route = SIZE_MAX;
    HMODULE module = nullptr;
    uint64_t generation = 0;
    size_t slot = SIZE_MAX;
    uint64_t lifetime = 0;
    uintptr_t handle = 0;
};

OtherFeatureLease SnapshotOtherFeature(size_t routeIndex, const Route& route,
    size_t slot, uintptr_t handle = 0) noexcept
{
    return {routeIndex, route.module, route.generation, slot,
        slot < route.otherFeatures.size() ? route.otherFeatures[slot].lifetime : 0,
        handle};
}

Route::OtherFeature* CurrentOtherFeature(const OtherFeatureLease& lease) noexcept
{
    if (lease.route >= gRoutes.size()) return nullptr;
    auto& route = gRoutes[lease.route];
    if (route.module != lease.module || route.generation != lease.generation
        || lease.slot >= route.otherFeatures.size()) return nullptr;
    auto& feature = route.otherFeatures[lease.slot];
    return feature.lifetime == lease.lifetime && lease.lifetime ? &feature : nullptr;
}

size_t FindActiveOtherFeature(const Route& route, uintptr_t handle) noexcept
{
    if (!handle) return SIZE_MAX;
    size_t found = SIZE_MAX;
    for (size_t i = 0; i != route.otherFeatures.size(); ++i)
    {
        const auto& feature = route.otherFeatures[i];
        // An old Release may still be executing while a nested Create receives
        // the same numeric handle. Do not send another operation for that value
        // until the old native completion has reconciled its exact lifetime.
        if (feature.state == Route::OtherFeature::State::eReleasing
            && feature.handle == handle) return SIZE_MAX;
        if (!feature.Active(handle)) continue;
        if (found != SIZE_MAX) return SIZE_MAX;
        found = i;
    }
    return found;
}

FeatureLifetimeEvent LifetimeEvent(FeatureLifetimePhase phase,
    const ampere_policy::Feature& feature) noexcept
{
    // D3D12 features predate the API field and keep its zero value.
    const GraphicsApi api = feature.api == static_cast<uint32_t>(GraphicsApi::eVulkan)
        ? GraphicsApi::eVulkan : GraphicsApi::eD3D12;
    return {phase, feature.handle, feature.lifetime, feature.owner, feature.generation,
        feature.provider, feature.providerGeneration, feature.wrapper,
        feature.wrapperGeneration, feature.publication, feature.luid,
        feature.createdGeneratedFrames, feature.createAttemptToken, static_cast<uint32_t>(api)};
}

void NotifyFeatureLifetime(const FeatureLifetimeEvent& event) noexcept
{
    if (const auto callback = gFeatureLifetimeCallback.load(std::memory_order_acquire))
    {
        __try { callback(event); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool QueueFeatureLifetime(const FeatureLifetimeEvent& event,
    const CreateAttemptProof& create = {}) noexcept
{
    // Callers enqueue while gCalls still serializes the native state change.
    // Delivery is deliberately separate: an observer may call back into a
    // Streamline path and must never inherit the Ampere lifecycle lock.
    try
    {
        std::lock_guard lock(gFeatureLifetimeDeliveryMutex);
        gPendingFeatureLifetimeEvents.push_back({event, create});
        return true;
    }
    catch (...)
    {
        gFatal.store(true, std::memory_order_release);
        Reject(172);
        return false;
    }
}

void DrainFeatureLifetimeEvents() noexcept
{
    std::unique_lock lock(gFeatureLifetimeDeliveryMutex);
    if (gFeatureLifetimeDeliveryActive)
        return;
    gFeatureLifetimeDeliveryActive = true;
    while (!gPendingFeatureLifetimeEvents.empty())
    {
        const PendingFeatureLifetimeEvent pending =
            gPendingFeatureLifetimeEvents.front();
        gPendingFeatureLifetimeEvents.pop_front();
        gDeliveringFeatureLifetimeEvent = pending;
        lock.unlock();
        NotifyFeatureLifetime(pending.event);
        lock.lock();
        gDeliveringFeatureLifetimeEvent = {};
    }
    gFeatureLifetimeDeliveryActive = false;
}

uintptr_t CreatedHandle(NVSDK_NGX_Result result, NVSDK_NGX_Handle** output) noexcept
{
    if (result != NVSDK_NGX_Result_Success) return 0;
    __try { return output && *output ? reinterpret_cast<uintptr_t>(*output) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ParameterCount(const NVSDK_NGX_Parameter* parameters, int& count) noexcept
{
    if (!parameters) return false;
    __try { return parameters->Get("DLSSG.MultiFrameCount", &count) == NVSDK_NGX_Result_Success; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ObserveCreationCount(const NVSDK_NGX_Parameter* parameters) noexcept
{
    int count = -1;
    const bool read = ParameterCount(parameters, count);
    gLastCreateCount.store(read ? count : -1, std::memory_order_relaxed);
}

Route* ActiveWrapper() noexcept
{
    const uint64_t generation = gActiveWrapperGeneration.load(std::memory_order_acquire);
    const auto module = reinterpret_cast<HMODULE>(gActiveWrapper.load(std::memory_order_acquire));
    if (!generation || generation != gActiveWrapperGeneration.load(std::memory_order_acquire)) return nullptr;
    for (auto& route : gRoutes)
        if (route.module == module && route.generation == generation && route.wrapperLayout.module == module) return &route;
    return nullptr;
}
// Evidence for failure 171. The first refusal and every change in
// the wrapper's allocation view get a full line; unchanged repeats are counted
// and summarised at most every 10 s. Reporting only; the gate is unchanged.
struct AllocationRejection
{
    const wchar_t* api = nullptr;
    const char* stage = nullptr;
    uintptr_t wrapper = 0;
    uint64_t generation = 0;
    uint32_t profile = 0, layoutMaximum = 0, startupMaximum = 0, readMaximum = 0;
    bool caller = false, buffers = false;
    ampere_wrapper::Observation observed{};
    bool Same(const AllocationRejection& other) const noexcept
    {
        return api == other.api && stage == other.stage && wrapper == other.wrapper && generation == other.generation
            && profile == other.profile && layoutMaximum == other.layoutMaximum && startupMaximum == other.startupMaximum
            && readMaximum == other.readMaximum && caller == other.caller && buffers == other.buffers
            && observed.contextReadable == other.observed.contextReadable
            && observed.buffersReadable == other.observed.buffersReadable && observed.maximum == other.observed.maximum
            && observed.bufferCount == other.observed.bufferCount && observed.capacity == other.observed.capacity;
    }
};
constexpr uint32_t kAllocationRejectionDetailLines = 32;
constexpr ULONGLONG kAllocationRejectionSummaryMs = 10000;
std::mutex gAllocationRejectionMutex;
AllocationRejection gLastAllocationRejection{};
uint64_t gAllocationRejections = 0, gAllocationRejectionRepeats = 0, gAllocationRejectionChangesUnlogged = 0;
uint32_t gAllocationRejectionDetails = 0;
ULONGLONG gAllocationRejectionLastLine = 0;

void ReportAllocationRejection(const wchar_t* api, const Route* wrapper, uint32_t startupMaximum,
    bool callerProven, bool buffersProven, const ampere_wrapper::Capacity& capacity) noexcept
{
    AllocationRejection current{};
    current.api = api;
    current.startupMaximum = startupMaximum;
    current.caller = callerProven;
    current.buffers = buffersProven;
    current.readMaximum = capacity.maximumGeneratedFrames;
    uintptr_t slotRva = 0;
    if (wrapper)
    {
        const auto& layout = wrapper->wrapperLayout;
        current.wrapper = reinterpret_cast<uintptr_t>(wrapper->module);
        current.generation = wrapper->generation;
        current.profile = static_cast<uint32_t>(layout.profile);
        current.layoutMaximum = layout.maximumGeneratedFrames;
        current.observed = ampere_wrapper::InspectFailure(layout);
        current.stage = buffersProven ? (capacity.maximumGeneratedFrames > startupMaximum ? "maximum-above-startup"
            : "readable") : ampere_wrapper::ReadFailure(layout);
        slotRva = layout.contextSlot - reinterpret_cast<uintptr_t>(layout.module);
    }
    else current.stage = "no-active-wrapper";
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(gAllocationRejectionMutex);
    const uint64_t refusal = ++gAllocationRejections;
    const bool changed = refusal == 1 || !current.Same(gLastAllocationRejection);
    if (changed && gAllocationRejectionDetails < kAllocationRejectionDetailLines)
    {
        ++gAllocationRejectionDetails;
        const auto& value = current.observed.capacity;
        wchar_t message[900]{};
        swprintf_s(message, L"Ampere MFG allocation rejected (%ls) refusal=%llu repeatsBefore=%llu: wrapper=%p generation=%llu "
            L"caller=%u buffers=%u stage=%hs profile=%u slot=+0x%llX maximum(layout/startup/read/context)=%u/%u/%u/%u "
            L"readable(context/vector)=%u/%u count=%u context=%p swapchain=%p ring=%p vector=%p:%p:%p "
            L"entries=%p:%p:%p:%p:%p:%p",
            api, refusal, gAllocationRejectionRepeats, reinterpret_cast<void*>(current.wrapper), current.generation,
            static_cast<unsigned>(callerProven), static_cast<unsigned>(buffersProven), current.stage, current.profile,
            static_cast<unsigned long long>(slotRva), current.layoutMaximum, startupMaximum, current.readMaximum,
            current.observed.maximum, static_cast<unsigned>(current.observed.contextReadable),
            static_cast<unsigned>(current.observed.buffersReadable), current.observed.bufferCount,
            reinterpret_cast<void*>(value.context), reinterpret_cast<void*>(value.swapchain),
            reinterpret_cast<void*>(value.ringOwner), reinterpret_cast<void*>(value.buffersBegin),
            reinterpret_cast<void*>(value.buffersEnd), reinterpret_cast<void*>(value.buffersCapacity),
            reinterpret_cast<void*>(value.buffers[0]), reinterpret_cast<void*>(value.buffers[1]),
            reinterpret_cast<void*>(value.buffers[2]), reinterpret_cast<void*>(value.buffers[3]),
            reinterpret_cast<void*>(value.buffers[4]), reinterpret_cast<void*>(value.buffers[5]));
        Report(message);
        if (gAllocationRejectionDetails == kAllocationRejectionDetailLines)
            Report(L"Ampere MFG allocation rejected: detail line limit reached; later changes are counted in the 10 s summaries");
        gLastAllocationRejection = current;
        gAllocationRejectionRepeats = 0;
        gAllocationRejectionChangesUnlogged = 0;
        gAllocationRejectionLastLine = now;
        return;
    }
    if (changed)
    {
        gLastAllocationRejection = current;
        ++gAllocationRejectionChangesUnlogged;
    }
    ++gAllocationRejectionRepeats;
    if (now - gAllocationRejectionLastLine < kAllocationRejectionSummaryMs) return;
    wchar_t message[320]{};
    swprintf_s(message, L"Ampere MFG allocation rejected (%ls) %llu more times in %.1f s (total %llu, %llu with a changed "
        L"view after the detail limit); last: caller=%u buffers=%u stage=%hs count=%u swapchain=%p",
        api, gAllocationRejectionRepeats, static_cast<double>(now - gAllocationRejectionLastLine) / 1000.0, refusal,
        gAllocationRejectionChangesUnlogged, static_cast<unsigned>(callerProven), static_cast<unsigned>(buffersProven),
        current.stage, current.observed.bufferCount, reinterpret_cast<void*>(current.observed.capacity.swapchain));
    Report(message);
    gAllocationRejectionRepeats = 0;
    gAllocationRejectionChangesUnlogged = 0;
    gAllocationRejectionLastLine = now;
}
// A Create certified the ring the wrapper allocated. Log a ring
// below the startup maximum once per wrapper generation and value; the patcher
// offers and submits no more than it holds (RingMaximumGeneratedFrames).
const wchar_t* CertifiedMode(uint32_t generated, uint32_t startupMaximum) noexcept
{
    return startupMaximum == 1 ? L"legacy-2x" : generated < startupMaximum ? L"MFG ring-capped" : L"MFG";
}
void RecordCertifiedRing(const Route& wrapper, uint32_t generated, uint32_t startupMaximum) noexcept
{
    const uint32_t previous = gRingMaximum.exchange(generated, std::memory_order_acq_rel);
    const uint64_t previousGeneration = gRingWrapperGeneration.exchange(wrapper.generation, std::memory_order_acq_rel);
    if (generated >= startupMaximum || (previous == generated && previousGeneration == wrapper.generation)) return;
    wchar_t message[256]{};
    swprintf_s(message, L"Ampere MFG ring capacity: the DLSS-G plug-in allocated %u generated frames (%u buffers) below the "
        L"startup maximum %u; modes capped at %ux (wrapper=%p generation=%llu)",
        generated, generated + 1, startupMaximum, generated + 1, wrapper.module,
        static_cast<unsigned long long>(wrapper.generation));
    Report(message);
}
bool FeatureCapacityCurrent(const ampere_policy::Feature& feature) noexcept
{
    auto* wrapper = ActiveWrapper();
    return wrapper && ampere_policy::CertifiedCapacityValid(feature.createdGeneratedFrames)
        && feature.capacity.maximumGeneratedFrames == feature.createdGeneratedFrames
        && reinterpret_cast<uintptr_t>(wrapper->module) == feature.wrapper
        && wrapper->generation == feature.wrapperGeneration
        && ampere_wrapper::Matches(wrapper->wrapperLayout, feature.capacity);
}

// The wrapper re-acquires its presentation buffers when the game resizes the
// swap chain or changes its fullscreen state, without a new Create (Star Wars
// Outlaws does this before its first FG frame). The exact feature keeps its
// allocation proof only between intervals, when a fresh stable read of the same
// ring differs from the stored one in the buffer entries alone. Any other
// allocation change still fails closed until Release and a new Create.
void RefreshPresentationBuffers(const Route& route, ampere_policy::Feature& feature,
    uint64_t publication, uint64_t adapter) noexcept
{
    const auto* wrapper = ActiveWrapper();
    if (gCreateDepth || gEvaluateDepth || gReleaseDepth || !Ready() || !wrapper || !feature.batch.BetweenIntervals()
        || !feature.Matches(feature.handle, reinterpret_cast<uintptr_t>(route.module), route.generation, publication, adapter)
        || reinterpret_cast<uintptr_t>(wrapper->module) != feature.wrapper || wrapper->generation != feature.wrapperGeneration
        || !ampere_policy::CertifiedCapacityValid(feature.createdGeneratedFrames)
        || feature.capacity.maximumGeneratedFrames != feature.createdGeneratedFrames
        || ampere_wrapper::Matches(wrapper->wrapperLayout, feature.capacity)) return;
    ampere_wrapper::Capacity current{};
    if (!ampere_wrapper::Read(wrapper->wrapperLayout, current)) return;
    const uint32_t changed = ampere_wrapper::RefreshedBuffers(feature.capacity, current);
    if (!changed) return;
    feature.capacity = current;
    const uint64_t refreshes = gBufferRefreshes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (refreshes > 8) return;
    wchar_t detail[192]{};
    swprintf_s(detail, L"Ampere wrapper presentation buffers re-acquired for the same swap chain: changed=%u/%u "
        L"refreshes=%llu; exact feature kept", changed, current.bufferCount, static_cast<unsigned long long>(refreshes));
    Report(detail);
}

bool ReadEvaluationInput(const NVSDK_NGX_Parameter* parameters, EvaluationInput& input,
    EvaluationReads* observation = nullptr, uint64_t legacySerial = 0) noexcept
{
    input = {};
    EvaluationReads reads{};
    if (observation) *observation = reads;
    if (!parameters) return false;
    __try
    {
        // The compiled wrapper uses Set(void*) for both opaque values.
        // In this MSVC ABI that setter is vtable +0x00, not the first
        // overload in source declaration order. Integer reads fail in NGX.
        void* frame = nullptr;
        void* output = nullptr;
        reads.count = parameters->Get("DLSSG.MultiFrameCount", &input.count);
        reads.index = parameters->Get("DLSSG.MultiFrameIndex", &input.index);
        reads.frame = parameters->Get("DLSSG.BackbufferFrameID", &frame);
        reads.output = parameters->Get("DLSSG.OutputInterpolated", &output);
        if (observation) *observation = reads;
        input.frame = reinterpret_cast<uintptr_t>(frame);
        // The certified old pacer ABI predates BackbufferFrameID. Its fixed
        // one-output calls use a feature-local ordinal for batch bookkeeping;
        // NGX parameters and the provider's temporal inputs are not rewritten.
        const bool legacyFrame = reads.frame != NVSDK_NGX_Result_Success && legacySerial != 0;
        if (legacyFrame) input.frame = legacySerial;
        input.output = output;
        return reads.count == NVSDK_NGX_Result_Success && reads.index == NVSDK_NGX_Result_Success
            && (reads.frame == NVSDK_NGX_Result_Success || legacyFrame) && reads.output == NVSDK_NGX_Result_Success
            && ampere_policy::EvaluationCountValid(input.count) && input.output != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        reads.exception = true;
        if (observation) *observation = reads;
        return false;
    }
}

// The same parameter reads; the output descriptor is resolved to its VkImage
// (ampere_vulkan_output.h). Sizes and format must then stay fixed per feature.
bool ReadVulkanEvaluationInput(const NVSDK_NGX_Parameter* parameters, EvaluationInput& input,
    VulkanOutput* view = nullptr, EvaluationReads* observation = nullptr, uint64_t legacySerial = 0) noexcept
{
    VulkanOutput resolved{};
    if (view) *view = {};
    if (!ReadEvaluationInput(parameters, input, observation, legacySerial)
        || !ampere_vulkan_output::Read(input.output, resolved)) return false;
    input.output = reinterpret_cast<void*>(static_cast<uintptr_t>(resolved.image));
    if (view) *view = resolved;
    return true;
}

void ReportEvaluationRejection(const Route& route, const ampere_policy::Feature& feature,
    const EvaluationInput& input, const EvaluationReads& reads, uint32_t mask,
    uint64_t publication, uint64_t luid) noexcept
{
    // One bounded snapshot per poisoned feature, before the failed flag is set.
    // It explains rejection; none of these observations relax an admission gate.
    wchar_t message[1024]{};
    swprintf_s(message, L"Ampere first evaluation rejection: mask=0x%02X "
        L"get(count/index/frame/output)=%08X/%08X/%08X/%08X exception=%u "
        L"count=%d index=%d frame=%llu output=%p "
        L"batch(started/submitted/failed)=%u/%u/%u previousFrame=%llu firstOutput=%p "
        L"depth(create/evaluate/release)=%u/%u/%u ready(device/adapter/program/startup/fatal)=%u/%u/%u/%u/%u",
        mask, reads.count, reads.index, reads.frame, reads.output, static_cast<unsigned>(reads.exception),
        input.count, input.index, input.frame, input.output,
        static_cast<unsigned>(feature.batch.started), feature.batch.submitted, static_cast<unsigned>(feature.batch.failed),
        feature.batch.frame, reinterpret_cast<void*>(feature.batch.outputs[0]), gCreateDepth, gEvaluateDepth, gReleaseDepth,
        static_cast<unsigned>(DeviceConfirmed()), static_cast<unsigned>(AdapterVerified()),
        static_cast<unsigned>(ampere_gpu::Ready()), static_cast<unsigned>(gStartupComplete.load()),
        static_cast<unsigned>(gFatal.load()));
    Report(message);
    swprintf_s(message, L"Ampere first evaluation ownership: handle=%p runtime=%p/%p generation=%llu/%llu "
        L"program=0x%llX/0x%llX luid=0x%llX/0x%llX createdCount=%u "
        L"wrapper=%p/%p generation=%llu/%llu (stored/current)",
        reinterpret_cast<void*>(feature.handle), reinterpret_cast<void*>(feature.owner), route.module,
        feature.generation, route.generation, feature.publication, publication, feature.luid, luid,
        feature.createdGeneratedFrames, reinterpret_cast<void*>(feature.wrapper),
        reinterpret_cast<void*>(gActiveWrapper.load()), feature.wrapperGeneration, gActiveWrapperGeneration.load());
    Report(message);
    if (const auto* wrapper = ActiveWrapper())
    {
        const auto observed = ampere_wrapper::InspectFailure(wrapper->wrapperLayout);
        const auto& current = observed.capacity;
        const auto& stored = feature.capacity;
        swprintf_s(message, L"Ampere first evaluation allocation: readable(context/buffers)=%u/%u maximum=%u count=%u "
            L"context=%p/%p swapchain=%p/%p vector=%p:%p:%p/%p:%p:%p "
            L"buffers=%p:%p:%p:%p:%p:%p/%p:%p:%p:%p:%p:%p (stored/current)",
            static_cast<unsigned>(observed.contextReadable), static_cast<unsigned>(observed.buffersReadable),
            observed.maximum, observed.bufferCount,
            reinterpret_cast<void*>(stored.context), reinterpret_cast<void*>(current.context),
            reinterpret_cast<void*>(stored.swapchain), reinterpret_cast<void*>(current.swapchain),
            reinterpret_cast<void*>(stored.buffersBegin), reinterpret_cast<void*>(stored.buffersEnd),
            reinterpret_cast<void*>(stored.buffersCapacity), reinterpret_cast<void*>(current.buffersBegin),
            reinterpret_cast<void*>(current.buffersEnd), reinterpret_cast<void*>(current.buffersCapacity),
            reinterpret_cast<void*>(stored.buffers[0]), reinterpret_cast<void*>(stored.buffers[1]),
            reinterpret_cast<void*>(stored.buffers[2]), reinterpret_cast<void*>(stored.buffers[3]),
            reinterpret_cast<void*>(stored.buffers[4]), reinterpret_cast<void*>(stored.buffers[5]),
            reinterpret_cast<void*>(current.buffers[0]), reinterpret_cast<void*>(current.buffers[1]),
            reinterpret_cast<void*>(current.buffers[2]), reinterpret_cast<void*>(current.buffers[3]),
            reinterpret_cast<void*>(current.buffers[4]), reinterpret_cast<void*>(current.buffers[5]));
        Report(message);
    }
}

bool OutputMatchesDevice(ID3D12Resource* output, ID3D12Device* device, D3D12_RESOURCE_DESC& desc) noexcept
{
    ID3D12Device* owner = nullptr;
    bool valid = false;
    __try
    {
        if (output && device && SUCCEEDED(output->GetDevice(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&owner))) && owner == device)
        {
            desc = output->GetDesc();
            valid = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
                && desc.Width && desc.Height && desc.DepthOrArraySize == 1
                && desc.MipLevels == 1 && desc.SampleDesc.Count == 1
                && desc.Format != DXGI_FORMAT_UNKNOWN;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    if (owner) owner->Release();
    return valid;
}

void ReportEvaluationSubmissionFailure(const Route& route, const ampere_policy::Feature& feature,
    ID3D12GraphicsCommandList* command, const EvaluationInput& input, const EvaluationReads& reads,
    const D3D12_RESOURCE_DESC& outputDesc, NVSDK_NGX_Result result, bool providerEntered) noexcept
{
    // One snapshot per exact feature lifetime, even if later batches recover.
    // Keep all additional COM queries off the successful evaluation path.
    if (result == NVSDK_NGX_Result_Success && providerEntered) return;
    uint32_t commandType = UINT32_MAX;
    HRESULT deviceResult = E_POINTER, removedReason = E_PENDING;
    ID3D12Device* device = nullptr;
    uintptr_t deviceIdentity = 0;
    bool deviceMatches = false, removedReasonRead = false, queryException = false;
    __try
    {
        if (command)
        {
            commandType = static_cast<uint32_t>(command->GetType());
            deviceResult = command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
            deviceIdentity = reinterpret_cast<uintptr_t>(device);
            deviceMatches = SUCCEEDED(deviceResult) && device && deviceIdentity == feature.device;
            if (deviceMatches)
            {
                removedReason = device->GetDeviceRemovedReason();
                removedReasonRead = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { queryException = true; }
    __try { if (device) device->Release(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { queryException = true; }

    wchar_t message[1024]{};
    swprintf_s(message, L"Ampere first evaluation submission failure: call=NVSDK_NGX_D3D12_EvaluateFeature "
        L"result=0x%08X platformError=%u providerEntered=%u count=%d index=%d frame=%llu output=%p boundary=%u",
        static_cast<uint32_t>(result), static_cast<unsigned>(result == NVSDK_NGX_Result_FAIL_PlatformError),
        static_cast<unsigned>(providerEntered), input.count, input.index,
        input.frame, input.output, FailureCode());
    Report(message);
    for (HMODULE owner : {route.module, reinterpret_cast<HMODULE>(feature.provider), reinterpret_cast<HMODULE>(feature.wrapper)})
    {
        wchar_t path[2048]{};
        dlssg_provider_policy::VersionTriplet version{};
        const DWORD length = GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path)));
        const bool versionRead = length && length < std::size(path) && dlssg_provider_policy::ReadProviderVersion(path, version);
        swprintf_s(message, L"Ampere evaluation failure module: owner=%p version=%u.%u.%u versionRead=%u path=%.800ls",
            owner, version.major, version.minor, version.build, static_cast<unsigned>(versionRead), path);
        Report(message);
    }
    swprintf_s(message, L"Ampere evaluation failure device: command=%p type=%u getDevice=0x%08X "
        L"device=%p expectedDevice=%p matches=%u removedReasonRead=%u removedReason=0x%08X exception=%u "
        L"luid=0x%llX handle=%p lifetime=%llu runtime=%p/%llu provider=%p/%llu wrapper=%p/%llu publication=0x%llX",
        command, commandType, static_cast<uint32_t>(deviceResult), reinterpret_cast<void*>(deviceIdentity),
        reinterpret_cast<void*>(feature.device), static_cast<unsigned>(deviceMatches),
        static_cast<unsigned>(removedReasonRead), static_cast<uint32_t>(removedReason), static_cast<unsigned>(queryException),
        feature.luid, reinterpret_cast<void*>(feature.handle), feature.lifetime, route.module, route.generation,
        reinterpret_cast<void*>(feature.provider), feature.providerGeneration,
        reinterpret_cast<void*>(feature.wrapper), feature.wrapperGeneration, feature.publication);
    Report(message);
    // Reuse the descriptor and parameter-read results already validated before
    // native Evaluate. Optional input pointers are not dereferenced, resource
    // state is not inferred, and no GPU readback or synchronization is started.
    swprintf_s(message, L"Ampere evaluation failure output: resource=%p dimension=%u size=%llux%u "
        L"array=%u mips=%u format=%u sampleCount=%u sampleQuality=%u layout=%u flags=0x%X "
        L"get(count/index/frame/output)=%08X/%08X/%08X/%08X",
        input.output, static_cast<unsigned>(outputDesc.Dimension), outputDesc.Width, outputDesc.Height,
        outputDesc.DepthOrArraySize, outputDesc.MipLevels, static_cast<unsigned>(outputDesc.Format),
        outputDesc.SampleDesc.Count, outputDesc.SampleDesc.Quality, static_cast<unsigned>(outputDesc.Layout),
        static_cast<unsigned>(outputDesc.Flags), reads.count, reads.index, reads.frame, reads.output);
    Report(message);
}

bool WritablePresetValue(const uint32_t* value) noexcept
{
    uintptr_t cursor = reinterpret_cast<uintptr_t>(value);
    if (!cursor || cursor > UINTPTR_MAX - sizeof(*value)) return false;
    const uintptr_t end = cursor + sizeof(*value);
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)
            || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = memory.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY
            && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > UINTPTR_MAX - base || base + memory.RegionSize <= cursor) return false;
        cursor = (std::min)(end, base + memory.RegionSize);
    }
    return true;
}

bool AccessPresetValue(uint32_t* value, uint32_t& observation, bool write) noexcept
{
    if (!WritablePresetValue(value)) return false;
    __try
    {
        if (write) *value = observation;
        else observation = *value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template<size_t I> __declspec(noinline) bool Preset(uint32_t setting, uint32_t* value)
{
    const void* caller = _ReturnAddress();
    PresetFn original = nullptr;
    HMODULE provider = nullptr;
    bool eligible = false, prepared = false;
    {
        std::lock_guard lock(gCalls);
        const Route& route = gRoutes[I];
        original = RouteOriginal<PresetFn>(route, route.preset);
        provider = route.module;
        prepared = route.prepared;
        eligible = AdapterVerified() && !gFatal.load();
    }
    if (!original) return false;
    MEMORY_BASIC_INFORMATION owner{};
    const bool providerCaller = provider
        && VirtualQuery(caller, &owner, sizeof(owner)) == sizeof(owner)
        && owner.AllocationBase == provider && owner.Type == MEM_IMAGE && owner.State == MEM_COMMIT
        && (owner.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
        && ((owner.Protect & 0xff) == PAGE_EXECUTE_READ || (owner.Protect & 0xff) == PAGE_EXECUTE);
    if (setting != ampere_policy::kPresetSetting || !eligible || !providerCaller || !WritablePresetValue(value))
        return original(setting, value);

    // Route metadata is no longer locked when the first read loads config or
    // calls the provider. Image ownership avoids acquiring the loader lock.
    uint32_t observed = dlssg_preset::EffectiveSelection(dlssg_preset::FreezeSelection(), provider);
    const bool overridden = observed != 0;
    bool result = false;
    if (overridden)
    {
        if (!AccessPresetValue(value, observed, true)) return original(setting, value);
        result = true;
    }
    else
    {
        result = original(setting, value);
        if (result ? !AccessPresetValue(value, observed, false) : !WritablePresetValue(value)) return result;
    }
    // Classified by caller (configuration or report read) once the route's
    // reader is adopted for live switching (AdoptLivePresetReader).
    dlssg_preset::RecordRouteRead(provider, caller, result, result ? observed : 0, overridden);
    if (prepared) gPresetProvider.store(reinterpret_cast<uintptr_t>(provider), std::memory_order_release);
    gPresetQueries.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void ObserveCreateDevice(ID3D12GraphicsCommandList* command) noexcept
{
    if (!command) return;
    ID3D12Device* device = nullptr;
    __try
    {
        if (SUCCEEDED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device)
            gpu_dispatch::ObserveD3D12Device(device);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (device) device->Release();
}

// nrouter.1: set while a game call into OptiScaler's own D3D12 NGX exports owns DLSS 5 on this thread
// (InstallOptiScalerNgxRoute); the runtime route under it then skips DLSS 5.
thread_local uint32_t tOptiScalerDlss5 = 0;

struct OptiScalerDlss5Scope
{
    const bool held;
    explicit OptiScalerDlss5Scope(bool hold) noexcept : held(hold) { if (held) ++tOptiScalerDlss5; }
    ~OptiScalerDlss5Scope() { if (held) --tOptiScalerDlss5; }
    OptiScalerDlss5Scope(const OptiScalerDlss5Scope&) = delete;
    OptiScalerDlss5Scope& operator=(const OptiScalerDlss5Scope&) = delete;
};

NVSDK_NGX_Result CreateDlssSource(size_t route,CreateFn original,ID3D12GraphicsCommandList* command,
    NVSDK_NGX_Feature feature,const NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle** output) noexcept
{
    const auto keys=dlss5::ReadCreate(parameters);
    const auto result=dlss_sr::Create(route,original,command,feature,parameters,output);
    const auto handle=CreatedHandle(result,output);
    if(handle && !tOptiScalerDlss5)dlss5::GameCreated(route,reinterpret_cast<const NVSDK_NGX_Handle*>(handle),keys);
    return result;
}
dlss5::Api Dlss5Api(const Route& route,CreateFn create,EvaluateFn evaluate,ReleaseFn release) noexcept
{
    dlss5::Api api{};api.runtime=route.module;api.create=create;api.evaluate=evaluate;api.release=release;
    api.allocate=reinterpret_cast<dlss5::AllocateFn>(GetProcAddress(route.module,"NVSDK_NGX_D3D12_AllocateParameters"));
    api.destroy=reinterpret_cast<dlss5::DestroyFn>(GetProcAddress(route.module,"NVSDK_NGX_D3D12_DestroyParameters"));
    return api;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* command,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    std::unique_lock lock(gCalls);
    const bool frameGeneration = feature == NVSDK_NGX_Feature_FrameGeneration;
    const bool runtimeRoute=gRoutes[I].runtime;
    const HMODULE routeModule=gRoutes[I].module; // for ForeignCreated's log
    const bool trackedRoute = frameGeneration && runtimeRoute;
    const bool runtimeOther = !frameGeneration && gRoutes[I].runtime && dlss_sr::Handles(feature);
    fg_lifetime::CreateScope inFlight(gFgLifetime, trackedRoute);
    if (frameGeneration) gPipelineCreateObserved.store(true, std::memory_order_release);
    auto original = RouteOriginal<CreateFn>(gRoutes[I], gRoutes[I].create);
    // The command list identifies the actual device even when a game does not
    // expose slSetD3DDevice. No backend is chosen by GPU name or enumeration order.
    if (frameGeneration) ObserveCreateDevice(command);
    if (!gpu_dispatch::UsesTranslatedProgram())
    {
        if (!original) original = NativeOriginal<CreateFn>(gRoutes[I], gRoutes[I].create);
        if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
        if (frameGeneration && gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return kRejected;
        // RTX 50 takes RTX 40's pass-through: its pre-call selects the provider
        // and records the controls' evidence, but publishes no patch there.
        if (frameGeneration || gpu_dispatch::UsesNativeProgram())
        {
            const auto before = frameGeneration && gpu_dispatch::UsesNativeProgram() ? gRoutes[I].beforeCreate : nullptr;
            const auto entry = gRoutes[I].create;
            lock.unlock();
            if (before) before(command, static_cast<uintptr_t>(feature), parameters, output,
                0, 0, entry, _ReturnAddress());
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
            if (frameGeneration && midpoint_fix::OutputPullMaskRequiresRestart()) return kRejected;
#endif
            // Native NGX may wait for a provider callback on another thread.
            // Ada/unmodified calls must not inherit Ampere's TLS lifecycle lock.
            fault_capture::NgxCall call(entry, _ReturnAddress(), command,
                static_cast<uintptr_t>(feature), parameters);
            // DLSS SR/RR on the runtime route: detection and the SR preset selection.
            const auto result = runtimeOther
                ? CreateDlssSource(I, original, command, feature, parameters, output)
                : original(command, feature, parameters, output);
            if(runtimeRoute && static_cast<uint32_t>(feature)==dlss5::kFeature && CreatedHandle(result,output))dlss5::ForeignCreated(_ReturnAddress(),routeModule);
            call.Complete(static_cast<uint32_t>(result), reinterpret_cast<void* const*>(output));
            if (trackedRoute) gFgLifetime.Created(I, CreatedHandle(result, output));
            return result;
        }
        // During pre-device startup, retain non-FG handles for Ampere's later
        // independent SR/RR pass-through. No parameter keys are used as identity.
    }
    if (frameGeneration) gCreateAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!original)
    {
        if (frameGeneration)
        {
            gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed);
            gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, NVSDK_NGX_Result_FAIL_NotInitialized);
        }
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    }
    if (feature != NVSDK_NGX_Feature_FrameGeneration)
    {
        // SR/RR can reuse a parameter object containing FG keys. Establish
        // their identity at Create rather than guessing from Evaluate options.
        // The installed entry pins its owner and trampoline. Reserve an exact
        // route/generation/lifetime before dropping gCalls; a nested Create may
        // grow the vector or return a numerically reused handle.
        OtherFeatureLease reservation{};
        {
            auto& route = gRoutes[I];
            if (!route.runtime || !output) { Reject(126); return kRejected; }
            const size_t slot = ReserveOtherFeature(route);
            if (slot == SIZE_MAX) { Reject(126); return kRejected; }
            reservation = SnapshotOtherFeature(I, route, slot);
        }
        lock.unlock();
        const auto result = runtimeOther
            ? CreateDlssSource(I, original, command, feature, parameters, output)
            : original(command, feature, parameters, output);
        const uintptr_t created = CreatedHandle(result, output);
        if(runtimeRoute && static_cast<uint32_t>(feature)==dlss5::kFeature && created)dlss5::ForeignCreated(_ReturnAddress(),routeModule);
        lock.lock();
        if (auto* reserved = CurrentOtherFeature(reservation);
            reserved && reserved->state == Route::OtherFeature::State::eCreating)
        {
            if (result == NVSDK_NGX_Result_Success && created)
            {
                reserved->handle = created;
                reserved->state = Route::OtherFeature::State::eActive;
            }
            else reserved->Clear();
        }
        return result;
    }
    Route& route = gRoutes[I];
    struct Attempt
    {
        bool forwarded = false;
        ~Attempt() { if (!forwarded) gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed); }
    } attempt;
    if (!output || !command || gCreateDepth || gEvaluateDepth || gReleaseDepth
        || gCurrentCreateAttempt != SIZE_MAX
        || !gStartupComplete.load() || gFatal.load()) { Reject(120); return kRejected; }
    gCertifiedMaximum.store(0, std::memory_order_release);
    *output = nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device)
    { Reject(121); return kRejected; }
    const uintptr_t deviceIdentity = reinterpret_cast<uintptr_t>(device);
    const LUID luid = device->GetAdapterLuid();
    device->Release();
    uint64_t packed = 0; memcpy(&packed, &luid, sizeof(packed));
    if (!AdapterVerified() || packed != ampere_gpu::AdapterLuid()) { Reject(121); return kRejected; }
    CreateAttemptScope createAttempt(route);
    if (!createAttempt) { Reject(120); return kRejected; }
    if (route.beforeCreate) route.beforeCreate(command, static_cast<uintptr_t>(feature), parameters, output,
        0, 0, route.create, _ReturnAddress());
    // MultiFrameCount is an evaluation input in the target wrapper, not its
    // creation/allocation contract. Observe it without rewriting or gating on it.
    ObserveCreationCount(parameters);
    if (!Ready() || !ampere_policy::CapacityValid(route.startupMaximum)
        || !parameters) { Reject(122); return kRejected; }
    auto* wrapper = ActiveWrapper();
    ampere_wrapper::Capacity capacity{};
    const bool callerProven = wrapper && ampere_wrapper::CreationOnStack(wrapper->wrapperLayout);
    const bool buffersProven = wrapper && ampere_wrapper::Read(wrapper->wrapperLayout, capacity);
    if (!callerProven || !buffersProven || capacity.maximumGeneratedFrames > route.startupMaximum)
    {
        ReportAllocationRejection(L"D3D12", wrapper, route.startupMaximum, callerProven, buffersProven, capacity);
        gPresentationBuffers.store(0); Reject(171); return kRejected;
    }
    createAttempt.BindCapacity(*wrapper, capacity);
    gPresentationBuffers.store(capacity.bufferCount);
    auto free = std::find_if(route.features.begin(), route.features.end(), [](const auto& f) { return !f.handle; });
    if (free == route.features.end()) { Reject(123); return kRejected; }
    Route* provider = nullptr;
    attempt.forwarded = true;
    uint64_t createAttemptToken = 0;
    const NVSDK_NGX_Result result = InvokeCreate(route, provider, *wrapper, capacity, createAttemptToken,
        [&]() -> NVSDK_NGX_Result {
            const auto dispatch = route.runtime ? ngx_runtime_dispatch::Read(entry_detour::ReadSnapshot(route.create),
                static_cast<uint32_t>(feature), gCreatingDispatch) : ngx_runtime_dispatch::ReadResult::eUnrecognized;
            if (dispatch != ngx_runtime_dispatch::ReadResult::eInvalid) return original(command, feature, parameters, output);
            Reject(ampere_diagnostics::kRuntimeDispatchFailure);
            return kRejected;
        });
    FeatureLifetimeEvent createdEvent{};
    bool notifyCreated = false;
    if (result == NVSDK_NGX_Result_Success && *output)
    {
        uint64_t lifetime = ++route.nextFeatureLifetime;
        if (!lifetime) lifetime = ++route.nextFeatureLifetime;
        *free = {reinterpret_cast<uintptr_t>(*output), reinterpret_cast<uintptr_t>(route.module),
            route.generation, ampere_gpu::Publication(), packed, false};
        free->lifetime = lifetime;
        free->provider = provider ? reinterpret_cast<uintptr_t>(provider->module) : 0;
        free->providerGeneration = provider ? provider->generation : 0;
        free->device = deviceIdentity;
        free->createdGeneratedFrames = capacity.maximumGeneratedFrames;
        free->wrapper = reinterpret_cast<uintptr_t>(wrapper->module);
        free->wrapperGeneration = wrapper->generation;
        free->capacity = capacity;
        free->createAttemptToken = createAttemptToken;
        if (!provider || !createAttemptToken
            || (!provider->legacyPresetReady && gPresetQueries.load() == 0)
            || !FeatureCapacityCurrent(*free))
        {
            createAttempt.Finish();
            gFatal.store(true);
            Reject(125);
            if (auto release = Original<ReleaseFn>(route.release))
                free->Release(InvokeRelease(route, release, *output, *free) == NVSDK_NGX_Result_Success);
            *output = nullptr;
            return kRejected;
        }
        gCreatedFeatures.fetch_add(1);
        if (trackedRoute) gFgLifetime.Created(I, free->handle);
        // The owned legacy Create contract accepts only its sole value 1.
        // A successful real Create establishes the read; discovery alone does
        // not increment query counters or claim the modern B preset was used.
        if (provider->legacyPresetReady)
        {
            dlssg_preset::RecordProviderRead(provider->module, true, 1, false);
            gPresetProvider.store(reinterpret_cast<uintptr_t>(provider->module), std::memory_order_release);
        }
        gFailures.Recover();
        gFailure.store(0, std::memory_order_release);
        gCertifiedMaximum.store(free->createdGeneratedFrames, std::memory_order_release);
        RecordCertifiedRing(*wrapper, free->createdGeneratedFrames, route.startupMaximum);
        wchar_t detail[192]{};
        swprintf_s(detail, L"Ampere FG Create accepted: generatedCapacity=%u presentationBuffers=%u mode=%ls; exact feature tracked",
            free->createdGeneratedFrames, capacity.bufferCount, CertifiedMode(free->createdGeneratedFrames, route.startupMaximum));
        Report(detail);
        createdEvent = LifetimeEvent(FeatureLifetimePhase::eCreated, *free);
        notifyCreated = QueueFeatureLifetime(createdEvent, createAttempt.Read());
    }
    else if (result == NVSDK_NGX_Result_Success) { Reject(124); return kRejected; }
    else gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, static_cast<uint32_t>(result));
    // Queued delivery owns its immutable proof before another native Create
    // can begin. Reentrant delivery must get a fresh TLS operation, not ours.
    createAttempt.Finish();
    if (notifyCreated)
    {
        lock.unlock();
        DrainFeatureLifetimeEvents();
    }
    return result;
}

// The route's own native D3D12 or Vulkan Release (`entry`), with gCalls held.
// ReadSnapshot().current re-checks the entry's first bytes and can read stale while the hook is live; a missing
// original would skip NVIDIA's release and leak the feature's VRAM.
ReleaseFn RouteRelease(const Route& route, entry_detour::Handle entry) noexcept
{
    auto original = RouteOriginal<ReleaseFn>(route, entry);
    if (!original && !gpu_dispatch::UsesTranslatedProgram())
    {
        const auto state = entry_detour::ReadSnapshot(entry);
        if (state.installed && state.original && state.owner == route.module && state.generation == route.generation)
            original = reinterpret_cast<ReleaseFn>(state.original);
    }
    return original;
}

// nrouter.1: OptiScaler's own D3D12 NGX exports, which take the game's DLSS calls. Written under gInstall before
// any entry goes live; installed entries pin OptiScaler for the process lifetime.
struct OptiScalerNgx
{
    HMODULE module{};
    entry_detour::Handle create{}, evaluate{}, release{};
};
OptiScalerNgx gOptiScalerNgx{};
std::atomic<bool> gOptiScalerNgxReported{false};

// The entry's trampoline. A running hook was reached through its entry, so this holds even when another hook
// replaced only our jump; a missing original would drop the game's call into OptiScaler.
template<class F> F OptiScalerOriginal(entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return state.installed && state.original && state.owner == gOptiScalerNgx.module
        ? reinterpret_cast<F>(state.original) : nullptr;
}

// DLSS 5 takes an OptiScaler handle only while Create, Evaluate and Release are all ours.
bool OptiScalerNgxCurrent() noexcept
{
    for (const auto handle : {gOptiScalerNgx.create, gOptiScalerNgx.evaluate, gOptiScalerNgx.release})
        if (!entry_detour::ReadSnapshot(handle).current) return false;
    return true;
}

// The D3D12 runtime route's entries for DLSS 5's own NGX calls: their trampolines, as that route passes them, so
// DLSS 5's feature never enters our runtime hooks.
bool RuntimeDlss5Api(dlss5::Api& api) noexcept
{
    std::lock_guard lock(gCalls);
    const bool translated = gpu_dispatch::UsesTranslatedProgram();
    for (const Route& route : gRoutes)
    {
        if (!route.runtime || !route.module) continue;
        const auto create = RouteOriginal<CreateFn>(route, route.create);
        const auto evaluate = translated ? RouteOriginal<EvaluateFn>(route, route.evaluate)
            : NativeOriginal<EvaluateFn>(route, route.evaluate);
        const auto release = RouteRelease(route, route.release);
        if (!create || !evaluate || !release) continue;
        api = Dlss5Api(route, create, evaluate, release);
        if (api.Complete()) return true;
    }
    return false;
}

// The game's SR/RR Create into OptiScaler. OptiScaler may create its own upscaler inside; DLSS 5 belongs to the
// game's handle instead.
NVSDK_NGX_Result NVSDK_CONV OptiScalerCreate(ID3D12GraphicsCommandList* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    const auto original = OptiScalerOriginal<CreateFn>(gOptiScalerNgx.create);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (!dlss_sr::Handles(feature) || !OptiScalerNgxCurrent()) return original(command, feature, parameters, output);
    const auto keys = dlss5::ReadCreate(parameters);
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    {
        OptiScalerDlss5Scope scope(true);
        result = original(command, feature, parameters, output);
    }
    if (const auto handle = CreatedHandle(result, output))
        dlss5::GameCreated(dlss5::kOptiScalerRoute, reinterpret_cast<const NVSDK_NGX_Handle*>(handle), keys);
    return result;
}

// The game's Evaluate into OptiScaler: DLSS 5 around OptiScaler's whole upscaling call, so after OptiScaler's own
// upscaler timer and its sharpening and output scaling. A handle DLSS 5 does not own passes through unchanged.
NVSDK_NGX_Result NVSDK_CONV OptiScalerEvaluate(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    const auto original = OptiScalerOriginal<EvaluateFn>(gOptiScalerNgx.evaluate);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    dlss5::Api api{};
    if (!handle || !dlss5::Owns(dlss5::kOptiScalerRoute, handle) || !RuntimeDlss5Api(api))
        return original(command, handle, parameters, callback);
    if (!gOptiScalerNgxReported.exchange(true, std::memory_order_relaxed))
        Report(L"DLSS 5 runs on the game's DLSS call into OptiScaler, after OptiScaler's upscaling "
            L"(outside OptiScaler's Upscaler GPU time)");
    OptiScalerDlss5Scope scope(true);
    const auto input = dlss5::BeforeEvaluate(dlss5::kOptiScalerRoute, api, command, handle, parameters);
    const auto result = original(command, handle, parameters, callback);
    dlss5::AfterEvaluate(dlss5::kOptiScalerRoute, api, command, handle, parameters, result, input);
    return result;
}

NVSDK_NGX_Result NVSDK_CONV OptiScalerRelease(NVSDK_NGX_Handle* handle)
{
    const auto original = OptiScalerOriginal<ReleaseFn>(gOptiScalerNgx.release);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    dlss5::BeforeRelease(dlss5::kOptiScalerRoute, handle);
    const auto result = original(handle);
    dlss5::AfterRelease(dlss5::kOptiScalerRoute, handle, result);
    return result;
}

// dx11nr.1: the runtime route's D3D11 entries, for DLSS 5 on DirectX 11 games (dlss5_d3d11.h). NVIDIA's calls always
// continue natively (no frame generation or SR override on D3D11), through the trampoline as in OptiScalerOriginal.
template<class F> F D3D11Original(const Route& route, entry_detour::Handle handle) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    return state.installed && state.original && route.module && state.owner == route.module
        && state.generation == route.generation ? reinterpret_cast<F>(state.original) : nullptr;
}

// DLSS 5 takes a D3D11 handle only while Create, Evaluate and Release are all ours.
bool D3D11RouteCurrent(const Route& route) noexcept
{
    return RouteEntryCurrent(route, route.d3d11Create) && RouteEntryCurrent(route, route.d3d11Evaluate)
        && RouteEntryCurrent(route, route.d3d11Release);
}

// The runtime's D3D12 Init_Ext for the bridge's own device: past the NGX initialization observer (its trampoline),
// so that device is never prepared as the game's (ngx_initialization.h, PrepareNgxInitialization).
dlss5::d3d11::InitFn D3D12InitEntry(HMODULE runtime) noexcept
{
    void* target = runtime ? reinterpret_cast<void*>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_Init_Ext")) : nullptr;
    if (!target) return nullptr;
    std::lock_guard lock(ngx_initialization::gInstall);
    for (const auto& owner : ngx_initialization::gOwners)
    {
        if (owner.module != runtime) continue;
        for (const auto entry : owner.entries)
        {
            if (!entry) continue;
            const auto state = entry_detour::ReadSnapshot(entry);
            if (state.installed && state.target == target && state.original)
                return reinterpret_cast<dlss5::d3d11::InitFn>(state.original);
        }
    }
    return reinterpret_cast<dlss5::d3d11::InitFn>(target);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV D3D11Create(ID3D11DeviceContext* context, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    std::unique_lock lock(gCalls);
    const auto original = D3D11Original<dlss5::d3d11::CreateFn>(gRoutes[I], gRoutes[I].d3d11Create);
    const HMODULE routeModule = gRoutes[I].module; // for ForeignCreated's log
    const bool covered = D3D11RouteCurrent(gRoutes[I]);
    lock.unlock();
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    // Keys read before NVIDIA's call, as on D3D12 (CreateDlssSource).
    const bool reconstruction = covered && dlss_sr::Handles(feature);
    const auto keys = reconstruction ? dlss5::ReadCreate(parameters) : dlss_sr::CreateKeys{};
    const auto result = original(context, feature, parameters, output);
    const uintptr_t created = CreatedHandle(result, output);
    // Another caller's DLSSNR stops DLSS 5, as on the D3D12 route.
    if (static_cast<uint32_t>(feature) == dlss5::kFeature && created) dlss5::ForeignCreated(_ReturnAddress(), routeModule);
    else if (reconstruction && created)
        dlss5::d3d11::GameCreated(reinterpret_cast<const NVSDK_NGX_Handle*>(created), keys);
    // Another hook over one of the D3D11 entries: the handle's evaluation or release could pass DLSS 5 by.
    else if (!covered && dlss_sr::Handles(feature) && created) dlss5::d3d11::UntrackedCreate();
    return result;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV D3D11Evaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    std::unique_lock lock(gCalls);
    const Route& route = gRoutes[I];
    dlss5::d3d11::Api api{};
    api.runtime = route.module;
    api.evaluate = D3D11Original<dlss5::d3d11::EvaluateFn>(route, route.d3d11Evaluate);
    api.init = &D3D12InitEntry;
    const auto create = RouteOriginal<CreateFn>(route, route.create);
    const auto evaluate = gpu_dispatch::UsesTranslatedProgram() ? RouteOriginal<EvaluateFn>(route, route.evaluate)
        : NativeOriginal<EvaluateFn>(route, route.evaluate);
    const auto release = RouteRelease(route, route.release);
    if (create && evaluate && release) api.d3d12 = Dlss5Api(route, create, evaluate, release);
    lock.unlock();
    if (!api.evaluate) return NVSDK_NGX_Result_FAIL_NotInitialized;
    return dlss5::d3d11::Evaluate(api, context, handle, parameters, callback);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV D3D11Release(NVSDK_NGX_Handle* handle)
{
    std::unique_lock lock(gCalls);
    const auto original = D3D11Original<ReleaseFn>(gRoutes[I], gRoutes[I].d3d11Release);
    lock.unlock();
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const bool owned = dlss5::d3d11::BeforeRelease(handle);
    const auto result = original(handle);
    dlss5::d3d11::AfterRelease(handle, result, owned);
    return result;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* command,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    std::unique_lock lock(gCalls);
    const bool translated = gpu_dispatch::UsesTranslatedProgram();
    auto original = translated ? RouteOriginal<EvaluateFn>(gRoutes[I], gRoutes[I].evaluate)
        : NativeOriginal<EvaluateFn>(gRoutes[I], gRoutes[I].evaluate);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (!translated)
    {
        if (gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict) return kRejected;
        const auto before = gpu_dispatch::UsesNativeProgram() ? gRoutes[I].beforeEvaluate : nullptr;
        const auto entry = gRoutes[I].evaluate;
        const bool runtime = gRoutes[I].runtime && dlss_sr::Active();
        const auto create = runtime ? RouteOriginal<CreateFn>(gRoutes[I], gRoutes[I].create) : nullptr;
        const auto release = runtime ? RouteRelease(gRoutes[I], gRoutes[I].release) : nullptr;
        const auto nrApi=runtime ? Dlss5Api(gRoutes[I],create,original,release) : dlss5::Api{};
        lock.unlock();
        if (before) before(command, reinterpret_cast<uintptr_t>(handle), parameters,
            reinterpret_cast<void*>(callback), 0, 0, entry, _ReturnAddress());
        // A DLSS SR feature of the game may be evaluated through our replacement.
        const NVSDK_NGX_Handle* target = runtime
            ? dlss_sr::Evaluate(I, create, release, command, handle, parameters) : handle;
        // XeMFG standalone's DLSS inputs, tagged on this list before DLSS reads them.
        if (gRoutes[I].runtime) xess_d3d12::CaptureDlssEvaluate(command, parameters);
        // DLSS 5 Render stage: our processed copy is the input for this call only.
        // Under an OptiScaler call that owns DLSS 5, that call runs it instead (nrouter.1).
        const bool nr = runtime && !tOptiScalerDlss5;
        const auto nrInput = nr ? dlss5::BeforeEvaluate(I,nrApi,command,handle,parameters) : dlss5::Substitution{};
        fault_capture::NgxCall call(entry, _ReturnAddress(), command,
            reinterpret_cast<uintptr_t>(handle), parameters);
        const auto result = original(command, target, parameters, callback);
        if(nr)dlss5::AfterEvaluate(I,nrApi,command,handle,parameters,result,nrInput);
        call.Complete(static_cast<uint32_t>(result));
        return result;
    }
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
    size_t frameGenerationSlot = SIZE_MAX;
    {
        const auto& route = gRoutes[I];
        for (size_t slot = 0; slot != route.features.size(); ++slot)
        {
            if (route.features[slot].handle && route.features[slot].handle == handleValue)
            { frameGenerationSlot = slot; break; }
        }
    }
    if (frameGenerationSlot == SIZE_MAX)
    {
        OtherFeatureLease lease{};
        {
            auto& route = gRoutes[I];
            const size_t slot = FindActiveOtherFeature(route, handleValue);
            if (slot != SIZE_MAX && route.otherFeatures[slot].activeCalls != UINT32_MAX)
            {
                ++route.otherFeatures[slot].activeCalls;
                lease = SnapshotOtherFeature(I, route, slot, handleValue);
            }
        }
        if (lease.slot != SIZE_MAX)
        {
            const auto create = RouteOriginal<CreateFn>(gRoutes[I], gRoutes[I].create);
            const auto release = RouteRelease(gRoutes[I], gRoutes[I].release);
            const auto nrApi=Dlss5Api(gRoutes[I],create,original,release);
            lock.unlock();
            const NVSDK_NGX_Handle* target = dlss_sr::Evaluate(I, create, release, command, handle, parameters);
            xess_d3d12::CaptureDlssEvaluate(command, parameters);   // as above
            const bool nr = !tOptiScalerDlss5;   // as above
            const auto nrInput = nr ? dlss5::BeforeEvaluate(I,nrApi,command,handle,parameters) : dlss5::Substitution{};
            const auto result = original(command, target, parameters, callback);
            if (nr) dlss5::AfterEvaluate(I,nrApi,command,handle,parameters,result,nrInput);
            lock.lock();
            if (auto* current = CurrentOtherFeature(lease);
                current && current->Active(lease.handle) && current->activeCalls)
                --current->activeCalls;
            return result;
        }
        Reject(130); return kRejected;
    }
    Route& route = gRoutes[I];
    auto* found = &route.features[frameGenerationSlot];
    gEvaluateAttempts.fetch_add(1, std::memory_order_relaxed);
    EvaluationInput input{};
    EvaluationReads reads{};
    const uint64_t publication = ampere_gpu::Publication(), adapter = ampere_gpu::AdapterLuid();
    const auto* wrapper = ActiveWrapper();
    const bool legacyFrame = wrapper && !wrapper->wrapperLayout.frameIdAvailable
        && found->createdGeneratedFrames == 1 && found->evaluationSerial != UINT64_MAX;
    const uint64_t serial = found->evaluationSerial == UINT64_MAX ? 0 : ++found->evaluationSerial;
    const bool inputsValid = ReadEvaluationInput(parameters, input, &reads, legacyFrame ? serial : 0);
    if (inputsValid && input.index == 1) RefreshPresentationBuffers(route, *found, publication, adapter);
    // Read once, so admission and completion of this call agree (flip-metering path only).
    const bool flipMetered = ampere_flip_metering::Active();
    const bool repeatedOutput = inputsValid && found->batch.RepeatsOutput(reinterpret_cast<uintptr_t>(input.output));
    const uint32_t rejected = ((gCreateDepth || gEvaluateDepth || gReleaseDepth) ? 0x01u : 0)
        | (!Ready() ? 0x02u : 0)
        | (!inputsValid ? 0x04u : 0)
        | (!FeatureCapacityCurrent(*found) ? 0x08u : 0)
        | (!ampere_policy::EvaluationFitsCapacity(input.count, found->createdGeneratedFrames) ? 0x10u : 0)
        | (!found->batch.Accepts(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output),
            flipMetered) ? 0x20u : 0)
        | (!found->Matches(reinterpret_cast<uintptr_t>(handle), reinterpret_cast<uintptr_t>(route.module),
            route.generation, publication, adapter) ? 0x40u : 0);
    if (rejected)
    {
        gCertifiedMaximum.store(0, std::memory_order_release);
        // A wrapper that shares one output across the batch cannot present
        // distinct generated frames. Keep this feature at 2x instead of
        // re-certifying MFG on the next 2x batch (a 2x/3x alternation).
        // With the flip-metering path observing SetFlipConfig, a refused
        // repeat is the batch before the wrapper's first metered call; the
        // next metered batch is admitted, so the feature is not latched
        // (within a small allowance, ampere_policy::Feature).
        if (rejected == 0x20u && !found->outputReuseLatched
            && found->batch.DuplicateOutput(input.frame, input.count, input.index,
                reinterpret_cast<uintptr_t>(input.output))
            && found->LatchRepeatedOutput(ampere_flip_metering::Observing()))
        {
            found->outputReuseLatched = true;
            gOutputReuseLatches.fetch_add(1, std::memory_order_relaxed);
            wchar_t detail[224]{};
            swprintf_s(detail, L"Ampere wrapper reused one output for generated frames 1..%d (count=%d frame=%llu); "
                L"this feature stays at 2x until it is recreated", input.index, input.count,
                static_cast<unsigned long long>(input.frame));
            Report(detail);
        }
        if (!found->rejectionReported)
        {
            ReportEvaluationRejection(route, *found, input, reads, rejected, publication, adapter);
            found->rejectionReported = true;
        }
        // Guard refusals do not consume a newer frame or erase the native
        // failure. The next properly owned index-1 call can retry the batch.
        found->batch.failed = true; Reject(131); return kRejected;
    }
    ID3D12Device* device = nullptr;
    if (!command || FAILED(command->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device)
    { found->batch.Fail(input.frame, input.count, input.index); Reject(132); return kRejected; }
    const LUID luid = device->GetAdapterLuid();
    D3D12_RESOURCE_DESC desc{};
    const bool outputValid = reinterpret_cast<uintptr_t>(device) == found->device
        && OutputMatchesDevice(static_cast<ID3D12Resource*>(input.output), device, desc)
        && (!found->outputWidth || (found->outputWidth == desc.Width
            && found->outputHeight == desc.Height && found->outputFormat == desc.Format));
    device->Release();
    uint64_t packed = 0; memcpy(&packed, &luid, sizeof(packed));
    if (packed != found->luid || !outputValid)
    { found->batch.Fail(input.frame, input.count, input.index); Reject(132); return kRejected; }
    found->outputWidth = desc.Width; found->outputHeight = desc.Height; found->outputFormat = desc.Format;
    if (route.beforeEvaluate) route.beforeEvaluate(command, reinterpret_cast<uintptr_t>(handle), parameters,
        reinterpret_cast<void*>(callback), 0, 0, route.evaluate, _ReturnAddress());
    bool providerExecuted = false;
    gEvaluationInput = input;
    const auto result = InvokeEvaluate(route, original, command, handle, parameters, callback, *found, providerExecuted);
    if (result != NVSDK_NGX_Result_Success || !providerExecuted)
    {
        if (result != NVSDK_NGX_Result_Success)
            gFailures.Record(ampere_diagnostics::kEvaluationSubmissionFailure, static_cast<uint32_t>(result));
        gFailedBatches.fetch_add(1, std::memory_order_relaxed);
        found->recoveryPending = true;
        if (!found->submissionFailureReported)
        {
            ReportEvaluationSubmissionFailure(route, *found, command, input, reads, desc, result, providerExecuted);
            found->submissionFailureReported = true;
        }
    }
    if (!FeatureCapacityCurrent(*found))
    { found->batch.Fail(input.frame, input.count, input.index); Reject(171); return kRejected; }
    found->batch.Complete(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output),
        result == NVSDK_NGX_Result_Success && providerExecuted, flipMetered);
    if (result == NVSDK_NGX_Result_Success)
    {
        if (!providerExecuted) { Reject(134); return kRejected; }
        if (found->batch.failed) { Reject(137); return kRejected; }
        found->evaluated = true; gEvaluations.fetch_add(1);
        if (repeatedOutput && gFlipMeteredRepeats.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            wchar_t detail[192]{};
            swprintf_s(detail, L"Ampere flip-metered batch admitted: index %d reused an earlier output "
                L"(count=%d frame=%llu); repeated outputs are admitted while SetFlipConfig reports metering",
                input.index, input.count, static_cast<unsigned long long>(input.frame));
            Report(detail);
        }
        gCertifiedMaximum.store(found->outputReuseLatched ? 1u : found->createdGeneratedFrames,
            std::memory_order_release);
        if (input.index == input.count)
        {
            gSubmittedBatches.fetch_add(1);
            if (found->recoveryPending)
            {
                gRecoveredBatches.fetch_add(1, std::memory_order_relaxed);
                found->recoveryPending = false;
            }
            gFailures.Recover(); gFailure.store(0, std::memory_order_release);
        }
    }
    // Preserve genuine failures. There is no output copy, forged success, or
    // reuse of a possibly stale/black OutputReal surface in this build.
    return result;
}

// RTX 30 Release of an exact tracked FG feature, or of a tracked SR/RR feature,
// for the D3D12 and the Vulkan ReleaseFeature of route I. gCalls is held.
NVSDK_NGX_Result AmpereRelease(size_t I, std::unique_lock<std::recursive_mutex>& lock,
    ReleaseFn original, NVSDK_NGX_Handle* handle)
{
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
    Route& route = gRoutes[I];
    for (auto& f : route.features)
    {
        if (f.handle && f.handle == handleValue)
        {
            if (gCreateDepth || gEvaluateDepth || gReleaseDepth) { Reject(136); return kRejected; }
            if (!f.Matches(handleValue, reinterpret_cast<uintptr_t>(route.module),
                    route.generation, ampere_gpu::Publication(), ampere_gpu::AdapterLuid()))
            { Reject(135); return kRejected; }
            const auto result = InvokeRelease(route, original, handle, f);
            const FeatureLifetimeEvent event = LifetimeEvent(FeatureLifetimePhase::eReleased, f);
            if (f.Release(result == NVSDK_NGX_Result_Success))
            {
                if (route.runtime) gFgLifetime.Released(I, handleValue);
                gCertifiedMaximum.store(0, std::memory_order_release);
                gPresentationBuffers.store(0, std::memory_order_release);
                const bool notifyReleased = QueueFeatureLifetime(event);
                lock.unlock();
                if (notifyReleased)
                    DrainFeatureLifetimeEvents();
            }
            return result;
        }
    }
    OtherFeatureLease lease{};
    {
        const size_t slot = FindActiveOtherFeature(route, handleValue);
        if (slot == SIZE_MAX || route.otherFeatures[slot].activeCalls)
        { Reject(136); return kRejected; }
        route.otherFeatures[slot].state = Route::OtherFeature::State::eReleasing;
        lease = SnapshotOtherFeature(I, route, slot, handleValue);
    }
    // No reference or iterator into the growable table crosses this native
    // boundary. A concurrent Create can safely grow it or reuse the number.
    lock.unlock();
    dlss5::BeforeRelease(I,handle);
    dlss5::vulkan::BeforeRelease(I, handle);
    dlss_sr::BeforeRelease(I, original, handle);
    const auto result = original(handle);
    dlss_sr::AfterRelease(I, handle, result);
    dlss5::AfterRelease(I,handle,result);
    dlss5::vulkan::AfterRelease(I, handle, result);
    lock.lock();
    if (auto* current = CurrentOtherFeature(lease);
        current && current->state == Route::OtherFeature::State::eReleasing
            && current->handle == lease.handle)
    {
        if (result == NVSDK_NGX_Result_Success) current->Clear();
        else current->state = Route::OtherFeature::State::eActive;
    }
    return result;
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle)
{
    std::unique_lock lock(gCalls);
    const auto original = RouteRelease(gRoutes[I], gRoutes[I].release);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
    if (!gpu_dispatch::UsesTranslatedProgram())
    {
        OtherFeatureLease lease{};
        const auto entry = gRoutes[I].release;
        {
            auto& route = gRoutes[I];
            const size_t slot = FindActiveOtherFeature(route, handleValue);
            if (slot != SIZE_MAX && route.otherFeatures[slot].activeCalls == 0)
            {
                route.otherFeatures[slot].state = Route::OtherFeature::State::eReleasing;
                lease = SnapshotOtherFeature(I, route, slot, handleValue);
            }
        }
        const bool runtimeRoute = gRoutes[I].runtime;
        lock.unlock();
        if (runtimeRoute) {dlss5::BeforeRelease(I,handle);dlss_sr::BeforeRelease(I, original, handle);}
        fault_capture::NgxCall call(entry, _ReturnAddress(), nullptr, handleValue, nullptr);
        const auto result = original(handle);
        call.Complete(static_cast<uint32_t>(result));
        if (runtimeRoute) {dlss_sr::AfterRelease(I, handle, result);dlss5::AfterRelease(I,handle,result);}
        if (runtimeRoute && result == NVSDK_NGX_Result_Success) gFgLifetime.Released(I, handleValue);
        if (lease.slot != SIZE_MAX)
        {
            lock.lock();
            if (auto* current = CurrentOtherFeature(lease);
                current && current->state == Route::OtherFeature::State::eReleasing
                    && current->handle == lease.handle)
            {
                if (result == NVSDK_NGX_Result_Success) current->Clear();
                else current->state = Route::OtherFeature::State::eActive;
            }
        }
        return result;
    }
    return AmpereRelease(I, lock, original, handle);
}

NVSDK_NGX_Result NVSDK_CONV RejectProviderCall(void*, uintptr_t, const void*, void*) noexcept { return kRejected; }

bool WINAPI ProviderGate(void* arg1, uintptr_t arg2, const void* arg3, void* arg4,
    uintptr_t arg5, uintptr_t arg6, entry_detour::Handle entry, const void* caller) noexcept
{
    const auto attemptedEntry = entry_detour::ReadSnapshot(entry);
    if (attemptedEntry.current && attemptedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
        && arg2 == NVSDK_NGX_Feature_FrameGeneration)
        gPipelineCreateObserved.store(true, std::memory_order_release);
    std::unique_lock callLock(gCalls);
    const auto selectedEntry = entry_detour::ReadSnapshot(entry);
    if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
        && arg2 == NVSDK_NGX_Feature_FrameGeneration)
        ObserveCreateDevice(static_cast<ID3D12GraphicsCommandList*>(arg1));
    if (!gpu_dispatch::UsesTranslatedProgram())
    {
        const auto selectedRoute = std::find_if(gRoutes.begin(), gRoutes.end(), [&](const Route& candidate) {
            return !candidate.runtime && candidate.module == selectedEntry.owner
                && candidate.generation == selectedEntry.generation; });
        if (!selectedEntry.current || selectedRoute == gRoutes.end()) return false;
        if (gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict)
            return selectedEntry.kind == entry_detour::Kind::eAmpereRelease;
        entry_detour::ForwardPreCall before = nullptr;
        // A provider FG Create outside every tracked runtime Create can leave a live
        // feature the lifetime tracker never sees.
        if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
            && arg2 == NVSDK_NGX_Feature_FrameGeneration)
            gFgLifetime.ObserveProviderCreate();
        if (gpu_dispatch::UsesNativeProgram())
        {
            if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
                && arg2 == NVSDK_NGX_Feature_FrameGeneration && selectedRoute->beforeCreate)
                before = selectedRoute->beforeCreate;
            else if (selectedEntry.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature && selectedRoute->beforeEvaluate)
                before = selectedRoute->beforeEvaluate;
        }
        callLock.unlock();
        if (before) before(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
#if MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
        if (((selectedEntry.kind == entry_detour::Kind::eNgxD3D12CreateFeature
                && arg2 == NVSDK_NGX_Feature_FrameGeneration)
            || selectedEntry.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature)
            && midpoint_fix::OutputPullMaskRequiresRestart()) return false;
#endif
        fault_capture::ProviderForward(entry, caller, arg1, arg2, arg3);
        return true;
    }
    // Tail forwarding keeps NVIDIA's real runtime caller on the stack.
    // Only a nested call belonging to our tracked runtime operation is admitted.
    HMODULE callerModule = nullptr;
    if (!gCallingRuntime || !caller || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(caller), &callerModule)
        || callerModule != gCallingRuntime->module) return Reject(160);
    const auto snapshot = entry_detour::ReadSnapshot(entry);
    auto found = std::find_if(gRoutes.begin(), gRoutes.end(), [&](const Route& route) {
        return !route.runtime && route.module == snapshot.owner && route.generation == snapshot.generation; });
    if (!snapshot.current || found == gRoutes.end() || !found->prepared) return Reject(161);
    auto& route = *found;
    if (snapshot.kind == entry_detour::Kind::eNgxD3D12CreateFeature)
    {
        ngx_runtime_dispatch::Selection selected{};
        const auto dispatch = ngx_runtime_dispatch::Read(entry_detour::ReadSnapshot(gCallingRuntime->create),
            NVSDK_NGX_Feature_FrameGeneration, selected);
        if (dispatch == ngx_runtime_dispatch::ReadResult::eInvalid
            || (gCreatingDispatch && (!ngx_runtime_dispatch::StillCurrent(gCreatingDispatch)
                || gCreatingDispatch.provider != snapshot.owner
                || gCreatingDispatch.target != reinterpret_cast<uintptr_t>(snapshot.target)))
            || (dispatch == ngx_runtime_dispatch::ReadResult::eSelected
                && (selected.provider != snapshot.owner || selected.target != reinterpret_cast<uintptr_t>(snapshot.target)
                    || !ngx_runtime_dispatch::StillCurrent(selected))))
            return Reject(ampere_diagnostics::kRuntimeDispatchFailure);
        if (!gCreateDepth || arg2 != NVSDK_NGX_Feature_FrameGeneration || !Ready() || !arg3
            || !gCallingWrapper || !ampere_wrapper::Matches(gCallingWrapper->wrapperLayout, gCreatingCapacity)
            || (gCreatingProvider && gCreatingProvider != &route)) return Reject(162);
        if (!ampere_gpu::BindProvider(route.module)) return Reject(161);
        if (!BindCurrentCreateProvider(route, route.create)) return Reject(162);
        gCreatingProvider = &route;
        if (route.beforeCreate) route.beforeCreate(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
        return true;
    }
    auto* feature = gCallingFeature;
    if (route.module != ampere_gpu::Provider() || !feature
        || feature->provider != reinterpret_cast<uintptr_t>(route.module)
        || feature->providerGeneration != route.generation) return Reject(163);
    if (snapshot.kind == entry_detour::Kind::eNgxD3D12EvaluateFeature)
    {
        EvaluationInput input{};
        if (!gEvaluateDepth || !arg2 || !Ready() || gProviderEvaluated || !FeatureCapacityCurrent(*feature)
            || !ReadEvaluationInput(static_cast<const NVSDK_NGX_Parameter*>(arg3), input, nullptr,
                feature->createdGeneratedFrames == 1 && ActiveWrapper()
                    && !ActiveWrapper()->wrapperLayout.frameIdAvailable ? gEvaluationInput.frame : 0)
            || !(input == gEvaluationInput)
            || (feature->providerHandle && feature->providerHandle != arg2)) return Reject(164);
        // The inner alias is learned only inside an exact, successfully
        // created outer handle's synchronous runtime call, never from options.
        feature->providerHandle = arg2;
        gProviderEvaluated = true;
        if (route.beforeEvaluate) route.beforeEvaluate(arg1, arg2, arg3, arg4, arg5, arg6, entry, caller);
        return true;
    }
    if (snapshot.kind == entry_detour::Kind::eAmpereRelease)
        return gReleaseDepth && arg1 && (!feature->providerHandle
            || feature->providerHandle == reinterpret_cast<uintptr_t>(arg1)) ? true : Reject(165);
    return Reject(166);
}

// The RTX 30/20 predicates of one FG requirements query during slInit/startup.
struct StartupRequirementScope
{
    // native: RTX 40/50, whose provider runs its own program and needs no answer here.
    bool native = false, conflict = false, adapter = false, fatal = false;
    bool Scoped() const noexcept { return !native && !conflict && adapter && !fatal; }
};

template<typename AdapterCheck>
StartupRequirementScope ObserveStartupRequirementScope(AdapterCheck&& adapterMatches) noexcept
{
    StartupRequirementScope scope{};
    scope.native = gpu_dispatch::UsesNativeProgram();
    scope.conflict = gpu_dispatch::Selected() == gpu_dispatch::Family::eConflict;
    scope.adapter = !scope.native && !scope.conflict && adapterMatches();
    scope.fatal = gFatal.load();
    return scope;
}

// The patcher's pre-slInit OTA scan selected an NVIDIA-signed provider whose
// embedded version this route admits. It is mapped and verified at device setup.
bool EligibleOtaProviderSelected() noexcept
{
    const auto selection = dlssg_provider_selection::Read();
    return selection.prepared && selection.selected && selection.selected.nvidiaSigned
        && dlssg_provider_policy::IsSupportedAmpereVersion(selection.selected.version);
}

ampere_startup_requirements::Answer DecideStartupRequirements(NVSDK_NGX_Result result,
    const NVSDK_NGX_FeatureRequirement& native) noexcept
{
    static_assert(ampere_startup_requirements::kResultSuccess == static_cast<uint32_t>(NVSDK_NGX_Result_Success)
        && ampere_startup_requirements::kResultFeatureNotSupported == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported)
        && ampere_startup_requirements::kResultFeatureNotFound == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotFound)
        && ampere_startup_requirements::kResultInvalidParameter == static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_InvalidParameter));
    ampere_startup_requirements::Query query{};
    ampere_startup_requirements::ClassifyResult(static_cast<uint32_t>(result), query);
    query.adapterAnswer = native.FeatureSupported == NVSDK_NGX_FeatureSupportResult_AdapterUnsupported
        || native.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
    query.providerReady = ampere_gpu::Ready();
    query.slInit = gInitDepth != 0;
    query.eligibleOta = (query.featureMissing || query.identifierRejected) && !query.providerReady && query.slInit
        && EligibleOtaProviderSelected();
    return ampere_startup_requirements::Decide(query);
}

NVSDK_NGX_Result AnswerStartupRequirements(NVSDK_NGX_Result result, NVSDK_NGX_FeatureRequirement* requirements,
    ampere_startup_requirements::Answer answer) noexcept
{
    using ampere_startup_requirements::Answer;
    if (answer == Answer::eAdapterSupported)
    {
        requirements->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        requirements->MinHWArchitecture = 0;
        return result;
    }
    if (answer == Answer::eReadyProvider || answer == Answer::eEligibleOta)
    {
        // Restrict the requirements override to this verified FG startup.
        *requirements = {};
        requirements->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        return NVSDK_NGX_Result_Success;
    }
    return result;
}

// The application identifier NGX's identifier check sees, without its text;
// -1 = not a ProjectId identifier (or no project ID string, or unreadable).
// ApplicationDataPath is never read: it holds the user's name.
struct DiscoveryIdentifier
{
    int identifierType = -1, engineType = -1, projectIdLength = -1, projectIdGuid = -1, engineVersion = -1;
};

DiscoveryIdentifier ReadDiscoveryIdentifier(const NVSDK_NGX_FeatureDiscoveryInfo* discovery) noexcept
{
    DiscoveryIdentifier identifier{};
    if (!discovery) return identifier;
    __try
    {
        const auto& id = discovery->Identifier;
        identifier.identifierType = static_cast<int>(id.IdentifierType);
        // The union holds a project description only for ProjectId identifiers.
        if (id.IdentifierType == NVSDK_NGX_Application_Identifier_Type_Project_Id)
        {
            identifier.engineType = static_cast<int>(id.v.ProjectDesc.EngineType);
            identifier.engineVersion = id.v.ProjectDesc.EngineVersion && id.v.ProjectDesc.EngineVersion[0];
            if (const char* project = id.v.ProjectDesc.ProjectId)
            {
                const size_t length = strnlen(project, 256);
                identifier.projectIdLength = static_cast<int>(length);
                identifier.projectIdGuid = ampere_startup_requirements::ProjectIdPassesDriverCheck(project, length);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { identifier = {}; }
    return identifier;
}

// Streamline unloads sl.dlss_g during slInit when this answer is unsupported
// on every adapter; the log line is the only record of that decision.
void ReportStartupRequirements(const wchar_t* api, const StartupRequirementScope& scope, NVSDK_NGX_Result result,
    const NVSDK_NGX_FeatureRequirement& native, ampere_startup_requirements::Answer answer,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery) noexcept
{
    if (!scope.Scoped() && !ampere_provider_arch::Armed()) return;
    if (gInitDepth) gInitRequirementQueries.fetch_add(1, std::memory_order_relaxed);
    if (gStartupRequirementQueries.fetch_add(1, std::memory_order_relaxed) >= kStartupRequirementReports) return;
    // identifierType 1 = ProjectId, 0 = ApplicationId; engineType 0 = CUSTOM;
    // projectIdGuid 1 = passes the driver's GUID check (617.42 refuses
    // ProjectId + CUSTOM with InvalidParameter otherwise); lengths stop at 256.
    const auto identifier = ReadDiscoveryIdentifier(discovery);
    wchar_t detail[512]{};
    swprintf_s(detail, L"Ampere FG requirements (%ls, %ls): result=0x%08X featureSupported=%d minArchitecture=0x%X "
        L"scoped=%u native=%u conflict=%u adapter=%u fatal=%u providerReady=%u answer=%hs "
        L"identifierType=%d engineType=%d projectIdLength=%d projectIdGuid=%d engineVersion=%d",
        api, gInitDepth ? L"slInit" : L"startup", static_cast<uint32_t>(result), static_cast<int>(native.FeatureSupported),
        native.MinHWArchitecture, static_cast<unsigned>(scope.Scoped()), static_cast<unsigned>(scope.native),
        static_cast<unsigned>(scope.conflict), static_cast<unsigned>(scope.adapter), static_cast<unsigned>(scope.fatal),
        static_cast<unsigned>(ampere_gpu::Ready()), ampere_startup_requirements::AnswerName(answer),
        identifier.identifierType, identifier.engineType, identifier.projectIdLength, identifier.projectIdGuid,
        identifier.engineVersion);
    Report(detail);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV Requirements(IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* requirements)
{
    std::unique_lock lock(gCalls);
    auto original = NativeOriginal<RequirementsFn>(gRoutes[I], gRoutes[I].requirements);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    // Streamline asks this during slInit, before a D3D12 device exists.
    // Verify only the adapter supplied to this FG query. Actual device LUID
    // confirmation remains mandatory before program publication and Create.
    const bool startupQuery = (gInitDepth || gStartupDepth) && requirements && discovery
        && discovery->FeatureID == NVSDK_NGX_Feature_FrameGeneration;
    if (!startupQuery)
    {
        // Non-FG and ordinary capability discovery is pure pass-through. It
        // must not serialize native code with feature callbacks on gCalls.
        lock.unlock();
        return original(adapter, discovery, requirements);
    }
    const auto scope = ObserveStartupRequirementScope([&] { return ampere_gpu::ObserveAdapter(adapter); });
    if (!scope.Scoped())
    {
        lock.unlock();
        const auto result = original(adapter, discovery, requirements);
        ReportStartupRequirements(L"d3d12", scope, result, *requirements, ampere_startup_requirements::Answer::eNative,
            discovery);
        return result;
    }
    const auto result = original(adapter, discovery, requirements);
    const auto native = *requirements;
    const auto answer = gFatal.load() ? ampere_startup_requirements::Answer::eNative
        : DecideStartupRequirements(result, native);
    ReportStartupRequirements(L"d3d12", scope, result, native, answer, discovery);
    return AnswerStartupRequirements(result, requirements, answer);
}

bool PublishStartupCapabilities(Route& route, NVSDK_NGX_Parameter* parameters) noexcept
{
    if (!parameters || gCreatedFeatures.load()) return Reject(170);
    // Each Streamline instance caches the maximum at its own plug-in startup.
    // A second instance (OptiScaler's private DLSS-G output beside the game's)
    // runs a later bracket; before the first FG Create it publishes the same
    // bootstrap, after one it still refuses.
    if (gStartupComplete.load() && !BeforeFirstFeatureCreate()) return Reject(170);
    // This is an early capability bootstrap, not permission to create FG.
    // A recognized wrapper must still be before swap-chain allocation; the
    // actual caller, generation and allocated buffers are checked at Create.
    uint32_t bootstrapMaximum = 0;
    for (const auto& candidate : gRoutes)
        if (candidate.wrapperLayout.module && ampere_wrapper::BeforeSwapchain(candidate.wrapperLayout))
            bootstrapMaximum = bootstrapMaximum ? std::min(bootstrapMaximum, candidate.wrapperLayout.maximumGeneratedFrames)
                : candidate.wrapperLayout.maximumGeneratedFrames;
    if (!ampere_policy::CapacityValid(bootstrapMaximum))
    {
        // No DLSS-G wrapper reached device setup at all: Streamline dropped
        // sl.dlss_g during slInit (or never loaded it). An unrecognized
        // wrapper layout is reported when it is inspected and stays 170.
        if (std::none_of(gRoutes.begin(), gRoutes.end(),
                [](const Route& candidate) { return candidate.wrapperInspected; }))
        {
            wchar_t detail[256]{};
            swprintf_s(detail, L"Ampere capability bootstrap refused: no Streamline DLSS-G plug-in reached device setup; "
                L"RTX 30/20 FG requirements queries this run=%u",
                gStartupRequirementQueries.load(std::memory_order_relaxed));
            Report(detail);
            return Reject(ampere_diagnostics::kStartupWrapperMissingFailure);
        }
        // Bounded startup evidence: distinguish an absent pacer from a late,
        // unreadable or already allocated wrapper if the rig still refuses.
        for (const auto& candidate : gRoutes)
        {
            if (!candidate.wrapperLayout.module) continue;
            const auto observed = ampere_wrapper::InspectFailure(candidate.wrapperLayout);
            wchar_t detail[320]{};
            swprintf_s(detail, L"Ampere capability bootstrap refused: wrapper=%p profile=%u contextReadable=%u "
                L"context=%p swapchain=%p ring=%p begin=%p end=%p buffers=%u maximum=%u",
                candidate.wrapperLayout.module, static_cast<unsigned>(candidate.wrapperLayout.profile),
                static_cast<unsigned>(observed.contextReadable), reinterpret_cast<void*>(observed.capacity.context),
                reinterpret_cast<void*>(observed.capacity.swapchain), reinterpret_cast<void*>(observed.capacity.ringOwner),
                reinterpret_cast<void*>(observed.capacity.buffersBegin), reinterpret_cast<void*>(observed.capacity.buffersEnd),
                observed.bufferCount, observed.maximum);
            Report(detail);
        }
        return Reject(170);
    }
    __try
    {
        unsigned int maximum = 0, available = 0;
        const auto nativeResult = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
        if (!route.startupMaximum)
        {
            route.nativeMaximum = nativeResult == NVSDK_NGX_Result_Success ? maximum : 0;
            gNativeMaximum.store(route.nativeMaximum);
            gNativeMaximumResult.store(nativeResult);
        }
        if (nativeResult == NVSDK_NGX_Result_Success && maximum > 5) return Reject(154);
        // Ampere's one-frame (or absent, when unavailable) capability is the
        // reason for this bootstrap. It is not evidence of buffer allocation.
        parameters->Set("FrameGeneration.Available", 1u);
        parameters->Set("DLSSG.MultiFrameCountMax", bootstrapMaximum);
        if (parameters->Get("FrameGeneration.Available", &available) != NVSDK_NGX_Result_Success
            || parameters->Get("DLSSG.MultiFrameCountMax", &maximum) != NVSDK_NGX_Result_Success
            || available != 1 || maximum != bootstrapMaximum) return Reject(154);
        route.startupMaximum = maximum;
        gStartupMaximum.store(maximum);
        wchar_t message[180]{};
        swprintf_s(message, L"Ampere MFG capability bootstrap: nativeGet=0x%08X nativeMax=%u startupMax=%u; allocation still required at Create",
            static_cast<unsigned>(gNativeMaximumResult.load()), route.nativeMaximum, maximum);
        Report(message);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Reject(154); }
}

// NVSDK_NGX_D3D12_GetCapabilityParameters and NVSDK_NGX_VULKAN_GetCapabilityParameters
// share their signature and the RTX 30 startup bootstrap.
template<size_t I, bool Vulkan = false> NVSDK_NGX_Result NVSDK_CONV Parameters(NVSDK_NGX_Parameter** output)
{
    std::unique_lock lock(gCalls);
    // The RTX 30/20 bootstrap below needs a current entry (NativeOriginal is strict there).
    auto original = NativeOriginal<ParametersFn>(gRoutes[I], Vulkan ? gRoutes[I].vkParameters : gRoutes[I].parameters);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const bool publishStartup = output && gStartupDepth
        && ampere_gpu::Ready() && AdapterVerified() && !gFatal.load()
        && gpu_dispatch::UsesTranslatedProgram()
        ;
    if (!publishStartup)
    {
        // Capability pass-through is itself external native work. Snapshot the
        // pinned route/generation above, then release gCalls so a provider can
        // synchronously wait for another intercepted operation.
        lock.unlock();
        return original(output);
    }
    const auto result = original(output);
    if (result == NVSDK_NGX_Result_Success && *output)
    {
        if (!PublishStartupCapabilities(gRoutes[I], *output))
        {
            const uint32_t failure = gFailure.load();
            gFatal.store(true);
            Reject(failure); // Preserve the first fatal bootstrap reason.
        }
    }
    return result;
}

// RTX 30 Vulkan route. The runtime's NGX Vulkan Create/Create1/Evaluate are the
// patcher's forwarding entries; on RTX 30 its gate diverts every call to these
// per-route thunks (the entries' rejected targets), which apply the D3D12
// route's create, capacity, batch and lifetime rules. A VkCommandBuffer names
// no device: the VkDevice observed on the verified VkPhysicalDevice takes the
// command list's place, and the output view's VkImage the output resource's.
void ReportVulkanEvaluationSubmissionFailure(const Route& route, const ampere_policy::Feature& feature,
    void* command, const EvaluationInput& input, const EvaluationReads& reads, const VulkanOutput& output,
    NVSDK_NGX_Result result, bool providerEntered) noexcept
{
    // One snapshot per exact feature lifetime, as for D3D12.
    if (result == NVSDK_NGX_Result_Success && providerEntered) return;
    wchar_t message[1024]{};
    swprintf_s(message, L"Ampere first evaluation submission failure: call=NVSDK_NGX_VULKAN_EvaluateFeature "
        L"result=0x%08X platformError=%u providerEntered=%u count=%d index=%d frame=%llu image=%p boundary=%u",
        static_cast<uint32_t>(result), static_cast<unsigned>(result == NVSDK_NGX_Result_FAIL_PlatformError),
        static_cast<unsigned>(providerEntered), input.count, input.index, input.frame, input.output, FailureCode());
    Report(message);
    for (HMODULE owner : {route.module, reinterpret_cast<HMODULE>(feature.provider), reinterpret_cast<HMODULE>(feature.wrapper)})
    {
        wchar_t path[2048]{};
        dlssg_provider_policy::VersionTriplet version{};
        const DWORD length = GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path)));
        const bool versionRead = length && length < std::size(path) && dlssg_provider_policy::ReadProviderVersion(path, version);
        swprintf_s(message, L"Ampere evaluation failure module: owner=%p version=%u.%u.%u versionRead=%u path=%.800ls",
            owner, version.major, version.minor, version.build, static_cast<unsigned>(versionRead), path);
        Report(message);
    }
    swprintf_s(message, L"Ampere Vulkan evaluation failure device: commandBuffer=%p device=%p expectedDevice=%p "
        L"luid=0x%llX handle=%p lifetime=%llu runtime=%p/%llu provider=%p/%llu wrapper=%p/%llu publication=0x%llX",
        command, reinterpret_cast<void*>(gVulkanDevice.load()), reinterpret_cast<void*>(feature.device),
        feature.luid, reinterpret_cast<void*>(feature.handle), feature.lifetime, route.module, route.generation,
        reinterpret_cast<void*>(feature.provider), feature.providerGeneration,
        reinterpret_cast<void*>(feature.wrapper), feature.wrapperGeneration, feature.publication);
    Report(message);
    swprintf_s(message, L"Ampere Vulkan evaluation failure output: image=0x%llX size=%ux%u format=%u "
        L"get(count/index/frame/output)=%08X/%08X/%08X/%08X",
        static_cast<unsigned long long>(output.image), output.width, output.height, output.format,
        reads.count, reads.index, reads.frame, reads.output);
    Report(message);
}

// SR/RR through the game's own Vulkan entry and recording.
dlss_sr::CreateCall SrCreateCall(const VulkanCreateCall& call) noexcept
{
    dlss_sr::CreateCall sr{};
    sr.api = call.create1 ? dlss_sr::CreateApi::eVulkan1 : dlss_sr::CreateApi::eVulkan;
    sr.vulkan = call.create;
    sr.vulkan1 = call.create1;
    sr.device = call.device;
    sr.command = call.command;
    return sr;
}

// A Vulkan Evaluate's re-create entries: both of the route's Create entries,
// on that Evaluate's command buffer (dlss_sr picks the game's). gCalls is held.
dlss_sr::CreateCall SrEvaluateCall(const Route& route, void* command) noexcept
{
    dlss_sr::CreateCall sr{};
    sr.api = dlss_sr::CreateApi::eVulkan;
    sr.vulkan = RouteOriginal<VkCreateFn>(route, route.vkCreate);
    sr.vulkan1 = RouteOriginal<VkCreate1Fn>(route, route.vkCreate1);
    sr.command = command;
    return sr;
}

// The route's Vulkan entries for the DLSS 5 Vulkan stage (originals
// behind our hooks; gCalls is held).
dlss5::vulkan::Api Dlss5VulkanApi(const Route& route, VkEvaluateFn evaluate, ReleaseFn release) noexcept
{
    dlss5::vulkan::Api api{};
    api.runtime = route.module;
    api.create = RouteOriginal<VkCreateFn>(route, route.vkCreate);
    api.create1 = RouteOriginal<VkCreate1Fn>(route, route.vkCreate1);
    api.evaluate = evaluate;
    api.release = release;
    if (route.module)
    {
        api.allocate = reinterpret_cast<dlss5::vulkan::AllocateFn>(GetProcAddress(route.module, "NVSDK_NGX_VULKAN_AllocateParameters"));
        api.destroy = reinterpret_cast<dlss5::vulkan::DestroyFn>(GetProcAddress(route.module, "NVSDK_NGX_VULKAN_DestroyParameters"));
    }
    return api;
}

NVSDK_NGX_Result VulkanCreate(size_t I, VulkanCreateCall call, bool create1, const void* caller)
{
    std::unique_lock lock(gCalls);
    Route& route = gRoutes[I];
    const bool frameGeneration = call.feature == NVSDK_NGX_Feature_FrameGeneration;
    // Vulkan FG lifetime, as the D3D12 thunk's (spans every early rejection).
    fg_lifetime::CreateScope inFlight(gFgLifetime, frameGeneration && route.runtime);
    if (frameGeneration) gPipelineCreateObserved.store(true, std::memory_order_release);
    const entry_detour::Handle entry = create1 ? route.vkCreate1 : route.vkCreate;
    const entry_detour::ForwardPreCall before = create1 ? route.vkBeforeCreate1 : route.vkBeforeCreate;
    if (create1) call.create1 = RouteOriginal<VkCreate1Fn>(route, entry);
    else call.create = RouteOriginal<VkCreateFn>(route, entry);
    if (frameGeneration) gCreateAttempts.fetch_add(1, std::memory_order_relaxed);
    if (create1 ? !call.create1 : !call.create)
    {
        if (frameGeneration)
        {
            gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed);
            gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, NVSDK_NGX_Result_FAIL_NotInitialized);
        }
        return NVSDK_NGX_Result_FAIL_NotInitialized;
    }
    if (!frameGeneration)
    {
        // SR/RR identity is established at Create, as on D3D12.
        OtherFeatureLease reservation{};
        {
            if (!route.runtime || !call.output) { Reject(126); return kRejected; }
            const size_t slot = ReserveOtherFeature(route);
            if (slot == SIZE_MAX) { Reject(126); return kRejected; }
            reservation = SnapshotOtherFeature(I, route, slot);
        }
        lock.unlock();
        const bool reconstruction = dlss_sr::Handles(call.feature);
        const auto nrKeys = reconstruction ? dlss5::ReadCreate(call.parameters) : dlss_sr::CreateKeys{};
        // DLSS SR/RR detection and the preset selection, as on D3D12.
        const auto result = reconstruction
            ? dlss_sr::Create(I, SrCreateCall(call), call.feature, call.parameters, call.output)
            : call.Forward();
        const uintptr_t created = CreatedHandle(result, call.output);
        if (reconstruction && created)
            dlss5::vulkan::GameCreated(I, reinterpret_cast<const NVSDK_NGX_Handle*>(created), nrKeys, create1 ? call.device : nullptr);
        lock.lock();
        if (auto* reserved = CurrentOtherFeature(reservation);
            reserved && reserved->state == Route::OtherFeature::State::eCreating)
        {
            if (result == NVSDK_NGX_Result_Success && created)
            {
                reserved->handle = created;
                reserved->state = Route::OtherFeature::State::eActive;
            }
            else reserved->Clear();
        }
        return result;
    }
    struct Attempt
    {
        bool forwarded = false;
        ~Attempt() { if (!forwarded) gCreateBlockedBeforeProvider.fetch_add(1, std::memory_order_relaxed); }
    } attempt;
    if (!call.output || !call.command || gCreateDepth || gEvaluateDepth || gReleaseDepth
        || gCurrentCreateAttempt != SIZE_MAX
        || !gStartupComplete.load() || gFatal.load()) { Reject(120); return kRejected; }
    gCertifiedMaximum.store(0, std::memory_order_release);
    *call.output = nullptr;
    // CreateFeature1 names its VkDevice; CreateFeature uses the device the host
    // initialized NGX with. Either must be the one observed on the adapter.
    const uintptr_t device = gVulkanDevice.load(std::memory_order_acquire);
    if (!gVulkanConfirmed.load() || !AdapterVerified() || !device || device == UINTPTR_MAX
        || (create1 && reinterpret_cast<uintptr_t>(call.device) != device))
    { Reject(121); return kRejected; }
    const uint64_t packed = ampere_gpu::AdapterLuid();
    CreateAttemptScope createAttempt(route, entry, GraphicsApi::eVulkan);
    if (!createAttempt) { Reject(120); return kRejected; }
    if (before)
    {
        if (create1)
            before(call.device, reinterpret_cast<uintptr_t>(call.command),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(call.feature)),
                const_cast<NVSDK_NGX_Parameter*>(call.parameters), reinterpret_cast<uintptr_t>(call.output), 0,
                entry, caller);
        else
            before(call.command, static_cast<uintptr_t>(call.feature), call.parameters, call.output, 0, 0,
                entry, caller);
    }
    ObserveCreationCount(call.parameters);
    if (!Ready() || !ampere_policy::CapacityValid(route.startupMaximum)
        || !call.parameters) { Reject(122); return kRejected; }
    auto* wrapper = ActiveWrapper();
    ampere_wrapper::Capacity capacity{};
    const bool callerProven = wrapper && ampere_wrapper::CreationOnStack(wrapper->wrapperLayout);
    const bool buffersProven = wrapper && ampere_wrapper::Read(wrapper->wrapperLayout, capacity);
    if (!callerProven || !buffersProven || capacity.maximumGeneratedFrames > route.startupMaximum)
    {
        ReportAllocationRejection(L"Vulkan", wrapper, route.startupMaximum, callerProven, buffersProven, capacity);
        gPresentationBuffers.store(0); Reject(171); return kRejected;
    }
    createAttempt.BindCapacity(*wrapper, capacity);
    gPresentationBuffers.store(capacity.bufferCount);
    auto free = std::find_if(route.features.begin(), route.features.end(), [](const auto& f) { return !f.handle; });
    if (free == route.features.end()) { Reject(123); return kRejected; }
    Route* provider = nullptr;
    attempt.forwarded = true;
    uint64_t createAttemptToken = 0;
    // The runtime's Vulkan dispatch table has no recognized layout; the nested
    // provider gate and its prepared-route checks bind the provider.
    const NVSDK_NGX_Result result = InvokeCreate(route, provider, *wrapper, capacity, createAttemptToken,
        [&] { return call.Forward(); });
    FeatureLifetimeEvent createdEvent{};
    bool notifyCreated = false;
    if (result == NVSDK_NGX_Result_Success && *call.output)
    {
        uint64_t lifetime = ++route.nextFeatureLifetime;
        if (!lifetime) lifetime = ++route.nextFeatureLifetime;
        *free = {reinterpret_cast<uintptr_t>(*call.output), reinterpret_cast<uintptr_t>(route.module),
            route.generation, ampere_gpu::Publication(), packed, false};
        free->lifetime = lifetime;
        free->provider = provider ? reinterpret_cast<uintptr_t>(provider->module) : 0;
        free->providerGeneration = provider ? provider->generation : 0;
        free->device = device;
        free->api = static_cast<uint32_t>(GraphicsApi::eVulkan);
        free->createdGeneratedFrames = capacity.maximumGeneratedFrames;
        free->wrapper = reinterpret_cast<uintptr_t>(wrapper->module);
        free->wrapperGeneration = wrapper->generation;
        free->capacity = capacity;
        free->createAttemptToken = createAttemptToken;
        if (!provider || !createAttemptToken
            || (!provider->legacyPresetReady && gPresetQueries.load() == 0)
            || !FeatureCapacityCurrent(*free))
        {
            createAttempt.Finish();
            gFatal.store(true);
            Reject(125);
            if (auto release = Original<ReleaseFn>(route.vkRelease))
                free->Release(InvokeRelease(route, release, *call.output, *free) == NVSDK_NGX_Result_Success);
            *call.output = nullptr;
            return kRejected;
        }
        gCreatedFeatures.fetch_add(1);
        if (route.runtime) gFgLifetime.Created(I, free->handle);
        if (provider->legacyPresetReady)
        {
            dlssg_preset::RecordProviderRead(provider->module, true, 1, false);
            gPresetProvider.store(reinterpret_cast<uintptr_t>(provider->module), std::memory_order_release);
        }
        gFailures.Recover();
        gFailure.store(0, std::memory_order_release);
        gCertifiedMaximum.store(free->createdGeneratedFrames, std::memory_order_release);
        RecordCertifiedRing(*wrapper, free->createdGeneratedFrames, route.startupMaximum);
        wchar_t detail[224]{};
        swprintf_s(detail, L"Ampere FG Create accepted (Vulkan): generatedCapacity=%u presentationBuffers=%u mode=%ls; "
            L"exact feature tracked", free->createdGeneratedFrames, capacity.bufferCount,
            CertifiedMode(free->createdGeneratedFrames, route.startupMaximum));
        Report(detail);
        createdEvent = LifetimeEvent(FeatureLifetimePhase::eCreated, *free);
        notifyCreated = QueueFeatureLifetime(createdEvent, createAttempt.Read());
    }
    else if (result == NVSDK_NGX_Result_Success) { Reject(124); return kRejected; }
    else gFailures.Record(ampere_diagnostics::kCreateSubmissionFailure, static_cast<uint32_t>(result));
    createAttempt.Finish();
    if (notifyCreated)
    {
        lock.unlock();
        DrainFeatureLifetimeEvents();
    }
    return result;
}

NVSDK_NGX_Result VulkanEvaluate(size_t I, void* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback, const void* caller)
{
    std::unique_lock lock(gCalls);
    auto original = RouteOriginal<VkEvaluateFn>(gRoutes[I], gRoutes[I].vkEvaluate);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
    size_t frameGenerationSlot = SIZE_MAX;
    {
        const auto& route = gRoutes[I];
        for (size_t slot = 0; slot != route.features.size(); ++slot)
        {
            if (route.features[slot].handle && route.features[slot].handle == handleValue)
            { frameGenerationSlot = slot; break; }
        }
    }
    if (frameGenerationSlot == SIZE_MAX)
    {
        OtherFeatureLease lease{};
        {
            auto& route = gRoutes[I];
            const size_t slot = FindActiveOtherFeature(route, handleValue);
            if (slot != SIZE_MAX && route.otherFeatures[slot].activeCalls != UINT32_MAX)
            {
                ++route.otherFeatures[slot].activeCalls;
                lease = SnapshotOtherFeature(I, route, slot, handleValue);
            }
        }
        if (lease.slot != SIZE_MAX)
        {
            // A game SR/RR feature may be evaluated through our replacement.
            const auto create = SrEvaluateCall(gRoutes[I], command);
            const auto release = RouteRelease(gRoutes[I], gRoutes[I].vkRelease);
            const auto nrApi = Dlss5VulkanApi(gRoutes[I], original, release);
            lock.unlock();
            const NVSDK_NGX_Handle* target = dlss_sr::Evaluate(I, create, release, handle, parameters);
            // The XeMFG Vulkan host's inputs, copied on this command buffer before DLSS reads them.
            xess_vulkan::CaptureDlssEvaluate(command, parameters);
            const auto result = original(command, target, parameters, callback);
            dlss5::vulkan::AfterEvaluate(I, nrApi, command, handle, parameters, result);
            lock.lock();
            if (auto* current = CurrentOtherFeature(lease);
                current && current->Active(lease.handle) && current->activeCalls)
                --current->activeCalls;
            return result;
        }
        Reject(130); return kRejected;
    }
    Route& route = gRoutes[I];
    auto* found = &route.features[frameGenerationSlot];
    gEvaluateAttempts.fetch_add(1, std::memory_order_relaxed);
    EvaluationInput input{};
    EvaluationReads reads{};
    VulkanOutput view{};
    const uint64_t publication = ampere_gpu::Publication(), adapter = ampere_gpu::AdapterLuid();
    const auto* wrapper = ActiveWrapper();
    const bool legacyFrame = wrapper && !wrapper->wrapperLayout.frameIdAvailable
        && found->createdGeneratedFrames == 1 && found->evaluationSerial != UINT64_MAX;
    const uint64_t serial = found->evaluationSerial == UINT64_MAX ? 0 : ++found->evaluationSerial;
    const bool inputsValid = ReadVulkanEvaluationInput(parameters, input, &view, &reads, legacyFrame ? serial : 0);
    if (inputsValid && input.index == 1) RefreshPresentationBuffers(route, *found, publication, adapter);
    const bool flipMetered = ampere_flip_metering::Active();
    const bool repeatedOutput = inputsValid && found->batch.RepeatsOutput(reinterpret_cast<uintptr_t>(input.output));
    const uint32_t rejected = ((gCreateDepth || gEvaluateDepth || gReleaseDepth) ? 0x01u : 0)
        | (!Ready() ? 0x02u : 0)
        | (!inputsValid ? 0x04u : 0)
        | (!FeatureCapacityCurrent(*found) ? 0x08u : 0)
        | (!ampere_policy::EvaluationFitsCapacity(input.count, found->createdGeneratedFrames) ? 0x10u : 0)
        | (!found->batch.Accepts(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output),
            flipMetered) ? 0x20u : 0)
        | (!found->Matches(reinterpret_cast<uintptr_t>(handle), reinterpret_cast<uintptr_t>(route.module),
            route.generation, publication, adapter) ? 0x40u : 0);
    if (rejected)
    {
        gCertifiedMaximum.store(0, std::memory_order_release);
        if (rejected == 0x20u && !found->outputReuseLatched
            && found->batch.DuplicateOutput(input.frame, input.count, input.index,
                reinterpret_cast<uintptr_t>(input.output))
            && found->LatchRepeatedOutput(ampere_flip_metering::Observing()))
        {
            found->outputReuseLatched = true;
            gOutputReuseLatches.fetch_add(1, std::memory_order_relaxed);
            wchar_t detail[224]{};
            swprintf_s(detail, L"Ampere wrapper reused one output for generated frames 1..%d (count=%d frame=%llu); "
                L"this feature stays at 2x until it is recreated", input.index, input.count,
                static_cast<unsigned long long>(input.frame));
            Report(detail);
        }
        if (!found->rejectionReported)
        {
            ReportEvaluationRejection(route, *found, input, reads, rejected, publication, adapter);
            found->rejectionReported = true;
        }
        found->batch.failed = true; Reject(131); return kRejected;
    }
    // The feature's VkDevice is still the host's, on the same adapter, and the
    // output image keeps the size and format of the feature's first output.
    const bool outputValid = command && found->device == gVulkanDevice.load(std::memory_order_acquire)
        && (!found->outputWidth || (found->outputWidth == view.width
            && found->outputHeight == view.height && found->outputFormat == view.format));
    if (found->luid != adapter || !outputValid)
    { found->batch.Fail(input.frame, input.count, input.index); Reject(132); return kRejected; }
    found->outputWidth = view.width; found->outputHeight = view.height; found->outputFormat = view.format;
    if (route.vkBeforeEvaluate) route.vkBeforeEvaluate(command, reinterpret_cast<uintptr_t>(handle), parameters,
        reinterpret_cast<void*>(callback), 0, 0, route.vkEvaluate, caller);
    bool providerExecuted = false;
    gEvaluationInput = input;
    const auto result = InvokeEvaluate(route, original, command, handle, parameters, callback,
        *found, providerExecuted);
    if (result != NVSDK_NGX_Result_Success || !providerExecuted)
    {
        if (result != NVSDK_NGX_Result_Success)
            gFailures.Record(ampere_diagnostics::kEvaluationSubmissionFailure, static_cast<uint32_t>(result));
        gFailedBatches.fetch_add(1, std::memory_order_relaxed);
        found->recoveryPending = true;
        if (!found->submissionFailureReported)
        {
            ReportVulkanEvaluationSubmissionFailure(route, *found, command, input, reads, view, result, providerExecuted);
            found->submissionFailureReported = true;
        }
    }
    if (!FeatureCapacityCurrent(*found))
    { found->batch.Fail(input.frame, input.count, input.index); Reject(171); return kRejected; }
    found->batch.Complete(input.frame, input.count, input.index, reinterpret_cast<uintptr_t>(input.output),
        result == NVSDK_NGX_Result_Success && providerExecuted, flipMetered);
    if (result == NVSDK_NGX_Result_Success)
    {
        if (!providerExecuted) { Reject(134); return kRejected; }
        if (found->batch.failed) { Reject(137); return kRejected; }
        found->evaluated = true; gEvaluations.fetch_add(1);
        if (repeatedOutput && gFlipMeteredRepeats.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            wchar_t detail[192]{};
            swprintf_s(detail, L"Ampere flip-metered batch admitted: index %d reused an earlier output "
                L"(count=%d frame=%llu); repeated outputs are admitted while SetFlipConfig reports metering",
                input.index, input.count, static_cast<unsigned long long>(input.frame));
            Report(detail);
        }
        gCertifiedMaximum.store(found->outputReuseLatched ? 1u : found->createdGeneratedFrames,
            std::memory_order_release);
        if (input.index == input.count)
        {
            gSubmittedBatches.fetch_add(1);
            if (found->recoveryPending)
            {
                gRecoveredBatches.fetch_add(1, std::memory_order_relaxed);
                found->recoveryPending = false;
            }
            gFailures.Recover(); gFailure.store(0, std::memory_order_release);
        }
    }
    return result;
}

// RTX 40/50: a runtime Vulkan Create the patcher's gate diverted
// here (DivertNativeVulkanCall), forwarded natively in place of its relay: the
// entry's pre-call, then NVIDIA's Create (SR/RR through dlss_sr), with FG
// creates counted and recorded for live preset switching as on D3D12.
NVSDK_NGX_Result NativeVulkanCreate(size_t I, VulkanCreateCall call, bool create1, const void* caller)
{
    std::unique_lock lock(gCalls);
    Route& route = gRoutes[I];
    const entry_detour::Handle entry = create1 ? route.vkCreate1 : route.vkCreate;
    const entry_detour::Handle armed = std::exchange(gNativeVulkanDivert, entry_detour::Handle{});
    // Any call the gate did not divert here stays a refusal.
    if (!route.runtime || !entry || !(armed == entry) || !gpu_dispatch::UsesNativeProgram()) return kRejected;
    if (create1) call.create1 = RouteOriginal<VkCreate1Fn>(route, entry);
    else call.create = RouteOriginal<VkCreateFn>(route, entry);
    if (create1 ? !call.create1 : !call.create) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const entry_detour::ForwardPreCall before = create1 ? route.vkBeforeCreate1 : route.vkBeforeCreate;
    lock.unlock();
    const bool frameGeneration = call.feature == NVSDK_NGX_Feature_FrameGeneration;
    fg_lifetime::CreateScope inFlight(gFgLifetime, frameGeneration);
    if (frameGeneration) gPipelineCreateObserved.store(true, std::memory_order_release);
    // The relay's argument shapes (CreateFeature1: the handle output is the fifth argument).
    if (before)
    {
        if (create1)
            before(call.device, reinterpret_cast<uintptr_t>(call.command),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(call.feature)),
                const_cast<NVSDK_NGX_Parameter*>(call.parameters), reinterpret_cast<uintptr_t>(call.output), 0,
                entry, caller);
        else
            before(call.command, static_cast<uintptr_t>(call.feature), call.parameters, call.output, 0, 0,
                entry, caller);
    }
    // The output-pull-mask build's gate ran its pre-call and publication check
    // before diverting (its entries carry no pre-call).
    fault_capture::NgxCall ngx(entry, caller, call.command, static_cast<uintptr_t>(call.feature), call.parameters);
    const bool reconstruction = dlss_sr::Handles(call.feature);
    const auto nrKeys = reconstruction ? dlss5::ReadCreate(call.parameters) : dlss_sr::CreateKeys{};
    const auto result = reconstruction
        ? dlss_sr::Create(I, SrCreateCall(call), call.feature, call.parameters, call.output)
        : call.Forward();
    ngx.Complete(static_cast<uint32_t>(result), reinterpret_cast<void* const*>(call.output));
    if (frameGeneration) gFgLifetime.Created(I, CreatedHandle(result, call.output));
    if (reconstruction)
        if (const uintptr_t created = CreatedHandle(result, call.output))
            dlss5::vulkan::GameCreated(I, reinterpret_cast<const NVSDK_NGX_Handle*>(created), nrKeys, create1 ? call.device : nullptr);
    return result;
}

// RTX 40/50: a diverted runtime Vulkan Evaluate (only while an
// SR/RR feature is tracked): the pre-call, then NVIDIA's Evaluate of the game's
// handle or of our replacement. FG evaluations pass unchanged, as on D3D12.
NVSDK_NGX_Result NativeVulkanEvaluate(size_t I, void* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback, const void* caller)
{
    std::unique_lock lock(gCalls);
    Route& route = gRoutes[I];
    const entry_detour::Handle entry = route.vkEvaluate;
    const entry_detour::Handle armed = std::exchange(gNativeVulkanDivert, entry_detour::Handle{});
    if (!route.runtime || !entry || !(armed == entry) || !gpu_dispatch::UsesNativeProgram()) return kRejected;
    const auto original = RouteOriginal<VkEvaluateFn>(route, entry);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    const entry_detour::ForwardPreCall before = route.vkBeforeEvaluate;
    const auto create = SrEvaluateCall(route, command);
    const auto release = RouteRelease(route, route.vkRelease);
    const auto nrApi = Dlss5VulkanApi(route, original, release);
    lock.unlock();
    if (before) before(command, reinterpret_cast<uintptr_t>(handle), parameters,
        reinterpret_cast<void*>(callback), 0, 0, entry, caller);
    const NVSDK_NGX_Handle* target = dlss_sr::Evaluate(I, create, release, handle, parameters);
    xess_vulkan::CaptureDlssEvaluate(command, parameters);
    fault_capture::NgxCall ngx(entry, caller, command, reinterpret_cast<uintptr_t>(handle), parameters);
    const auto result = original(command, target, parameters, callback);
    ngx.Complete(static_cast<uint32_t>(result));
    dlss5::vulkan::AfterEvaluate(I, nrApi, command, handle, parameters, result);
    return result;
}

// These thunks are the runtime entries' rejected targets: the patcher's gate
// diverts here on RTX 30, and on RTX 40/50 for the calls DivertNativeVulkanCall
// armed; any other call reaching them is a refusal.
template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkCreate(void* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    VulkanCreateCall call{};
    call.command = command; call.feature = feature; call.parameters = parameters; call.output = output;
    if (!gpu_dispatch::UsesTranslatedProgram()) return NativeVulkanCreate(I, call, false, _ReturnAddress());
    return VulkanCreate(I, call, false, _ReturnAddress());
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkCreate1(void* device, void* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    VulkanCreateCall call{};
    call.device = device; call.command = command; call.feature = feature;
    call.parameters = parameters; call.output = output;
    if (!gpu_dispatch::UsesTranslatedProgram()) return NativeVulkanCreate(I, call, true, _ReturnAddress());
    return VulkanCreate(I, call, true, _ReturnAddress());
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkEvaluate(void* command, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    if (!gpu_dispatch::UsesTranslatedProgram())
        return NativeVulkanEvaluate(I, command, handle, parameters, callback, _ReturnAddress());
    return VulkanEvaluate(I, command, handle, parameters, callback, _ReturnAddress());
}

// The runtime's Vulkan Release: RTX 30 systems, and RTX 40/50 for live preset
// switching, where it forwards natively and records FG and SR/RR
// releases as the D3D12 Release does.
template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkRelease(NVSDK_NGX_Handle* handle)
{
    std::unique_lock lock(gCalls);
    const auto original = RouteRelease(gRoutes[I], gRoutes[I].vkRelease);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (!gpu_dispatch::UsesTranslatedProgram())
    {
        const auto entry = gRoutes[I].vkRelease;
        const bool runtimeRoute = gRoutes[I].runtime;
        lock.unlock();
        const uintptr_t handleValue = reinterpret_cast<uintptr_t>(handle);
        if (runtimeRoute) { dlss5::vulkan::BeforeRelease(I, handle); dlss_sr::BeforeRelease(I, original, handle); }
        fault_capture::NgxCall call(entry, _ReturnAddress(), nullptr, handleValue, nullptr);
        const auto result = original(handle);
        call.Complete(static_cast<uint32_t>(result));
        if (runtimeRoute) { dlss_sr::AfterRelease(I, handle, result); dlss5::vulkan::AfterRelease(I, handle, result); }
        if (runtimeRoute && result == NVSDK_NGX_Result_Success) gFgLifetime.Released(I, handleValue);
        return result;
    }
    return AmpereRelease(I, lock, original, handle);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkRequirements(void* instance, void* physicalDevice,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* requirements)
{
    std::unique_lock lock(gCalls);
    auto original = NativeOriginal<VkRequirementsFn>(gRoutes[I], gRoutes[I].vkRequirements);
    if (!original) return NVSDK_NGX_Result_FAIL_NotInitialized;
    // sl.common asks this during slInit with its own temporary instance. As
    // for D3D12, only the queried adapter is verified here; the device LUID is
    // still confirmed before program publication and Create.
    const bool startupQuery = (gInitDepth || gStartupDepth) && requirements && discovery
        && discovery->FeatureID == NVSDK_NGX_Feature_FrameGeneration;
    if (!startupQuery)
    {
        lock.unlock();
        return original(instance, physicalDevice, discovery, requirements);
    }
    const auto scope = ObserveStartupRequirementScope([&] { return ampere_gpu::ObserveVulkanAdapter(physicalDevice); });
    if (!scope.Scoped())
    {
        lock.unlock();
        const auto result = original(instance, physicalDevice, discovery, requirements);
        ReportStartupRequirements(L"vulkan", scope, result, *requirements, ampere_startup_requirements::Answer::eNative,
            discovery);
        return result;
    }
    const auto result = original(instance, physicalDevice, discovery, requirements);
    const auto native = *requirements;
    const auto answer = gFatal.load() ? ampere_startup_requirements::Answer::eNative
        : DecideStartupRequirements(result, native);
    ReportStartupRequirements(L"vulkan", scope, result, native, answer, discovery);
    return AnswerStartupRequirements(result, requirements, answer);
}

template<size_t I> NVSDK_NGX_Result NVSDK_CONV VkParameters(NVSDK_NGX_Parameter** output)
{ return Parameters<I, true>(output); }

#define AMPERE_THUNKS(F) F<0>, F<1>, F<2>, F<3>, F<4>, F<5>, F<6>, F<7>
constexpr std::array<VkCreateFn, kRoutes> kVkCreates{AMPERE_THUNKS(VkCreate)};
constexpr std::array<VkCreate1Fn, kRoutes> kVkCreate1s{AMPERE_THUNKS(VkCreate1)};
constexpr std::array<VkEvaluateFn, kRoutes> kVkEvaluates{AMPERE_THUNKS(VkEvaluate)};
constexpr std::array<ReleaseFn, kRoutes> kVkReleases{AMPERE_THUNKS(VkRelease)};
constexpr std::array<VkRequirementsFn, kRoutes> kVkRequirements{AMPERE_THUNKS(VkRequirements)};
constexpr std::array<ParametersFn, kRoutes> kVkParameters{AMPERE_THUNKS(VkParameters)};
constexpr std::array<CreateFn, kRoutes> kCreates{AMPERE_THUNKS(Create)};
constexpr std::array<EvaluateFn, kRoutes> kEvaluates{AMPERE_THUNKS(Evaluate)};
constexpr std::array<ReleaseFn, kRoutes> kReleases{AMPERE_THUNKS(Release)};
constexpr std::array<RequirementsFn, kRoutes> kRequirements{AMPERE_THUNKS(Requirements)};
constexpr std::array<ParametersFn, kRoutes> kParameters{AMPERE_THUNKS(Parameters)};
constexpr std::array<PresetFn, kRoutes> kPresets{AMPERE_THUNKS(Preset)};
constexpr std::array<dlss5::d3d11::CreateFn, kRoutes> kD3D11Creates{AMPERE_THUNKS(D3D11Create)};
constexpr std::array<dlss5::d3d11::EvaluateFn, kRoutes> kD3D11Evaluates{AMPERE_THUNKS(D3D11Evaluate)};
constexpr std::array<ReleaseFn, kRoutes> kD3D11Releases{AMPERE_THUNKS(D3D11Release)};
#undef AMPERE_THUNKS

Route* FindRoute(HMODULE module, uint64_t generation) noexcept
{
    for (auto& route : gRoutes) if (route.module == module && route.generation == generation) return &route;
    for (auto& route : gRoutes) if (!route.module) { route.module = module; route.generation = generation; return &route; }
    return nullptr;
}

bool InstallExport(Route& route, const char* name, entry_detour::Kind kind, void* hook, entry_detour::Handle& handle) noexcept
{
    if (handle) return entry_detour::ReadSnapshot(handle).current;
    void* target = reinterpret_cast<void*>(GetProcAddress(route.module, name));
    if (!target) return false;
    void* trampoline = nullptr;
    entry_detour::InstallOptions options{}; options.generation = route.generation; options.allowRelocated = true;
    if (!route.runtime && (kind == entry_detour::Kind::eNgxD3D12CreateFeature
            || kind == entry_detour::Kind::eNgxD3D12EvaluateFeature || kind == entry_detour::Kind::eAmpereRelease))
        return entry_detour::InstallForwarding(kind, route.module, target, nullptr, trampoline,
            options, &handle, &ProviderGate, reinterpret_cast<void*>(&RejectProviderCall));
    // The provider's Vulkan Release keeps NVIDIA's runtime caller too.
    if (!route.runtime && kind == entry_detour::Kind::eAmpereVulkanRelease)
        return entry_detour::InstallForwarding(kind, route.module, target, nullptr, trampoline,
            options, &handle, &VulkanProviderGate, reinterpret_cast<void*>(&RejectProviderCall));
    return entry_detour::Install(kind, route.module, target, hook, trampoline, options, &handle);
}

// RTX 30 systems: the runtime's Vulkan Release (exact feature lifetime), its
// Vulkan requirements and capability parameters (startup bootstrap). True when
// every present export is covered; on other systems nothing is installed.
bool InstallVulkanRuntime(Route& route, size_t index) noexcept
{
    if (!ampere_provider_arch::Armed()) return false;
    bool covered = true;
    if (GetProcAddress(route.module, "NVSDK_NGX_VULKAN_ReleaseFeature"))
        covered = InstallExport(route, "NVSDK_NGX_VULKAN_ReleaseFeature", entry_detour::Kind::eAmpereVulkanRelease,
            reinterpret_cast<void*>(kVkReleases[index]), route.vkRelease) && covered;
    if (GetProcAddress(route.module, "NVSDK_NGX_VULKAN_GetFeatureRequirements"))
        covered = InstallExport(route, "NVSDK_NGX_VULKAN_GetFeatureRequirements", entry_detour::Kind::eAmpereVulkanRequirements,
            reinterpret_cast<void*>(kVkRequirements[index]), route.vkRequirements) && covered;
    if (GetProcAddress(route.module, "NVSDK_NGX_VULKAN_GetCapabilityParameters"))
        covered = InstallExport(route, "NVSDK_NGX_VULKAN_GetCapabilityParameters", entry_detour::Kind::eAmpereVulkanParameters,
            reinterpret_cast<void*>(kVkParameters[index]), route.vkParameters) && covered;
    return covered;
}

// RTX 30 systems: the provider's Vulkan Release, nested in a runtime Release.
bool InstallVulkanProvider(Route& route) noexcept
{
    if (!ampere_provider_arch::Armed()) return false;
    return !GetProcAddress(route.module, "NVSDK_NGX_VULKAN_ReleaseFeature")
        || InstallExport(route, "NVSDK_NGX_VULKAN_ReleaseFeature", entry_detour::Kind::eAmpereVulkanRelease,
            nullptr, route.vkRelease);
}

// dx11nr.1: the runtime's D3D11 entries (DLSS 5 on DirectX 11 games), on every GPU family. Release and Evaluate
// first, so no handle DLSS 5 takes at Create escapes its evaluation or release. Optional: a runtime without them,
// or a failed install, keeps its D3D12 route; DLSS 5 then does not run on DirectX 11.
bool InstallD3D11Route(Route& route, size_t index) noexcept
{
    if (!GetProcAddress(route.module, "NVSDK_NGX_D3D11_CreateFeature") || !GetProcAddress(route.module, "NVSDK_NGX_D3D11_EvaluateFeature")
        || !GetProcAddress(route.module, "NVSDK_NGX_D3D11_ReleaseFeature")) return false;
    const bool installed = InstallExport(route, "NVSDK_NGX_D3D11_ReleaseFeature", entry_detour::Kind::eNgxRuntimeD3D11Release,
            reinterpret_cast<void*>(kD3D11Releases[index]), route.d3d11Release)
        && InstallExport(route, "NVSDK_NGX_D3D11_EvaluateFeature", entry_detour::Kind::eNgxRuntimeD3D11Evaluate,
            reinterpret_cast<void*>(kD3D11Evaluates[index]), route.d3d11Evaluate)
        && InstallExport(route, "NVSDK_NGX_D3D11_CreateFeature", entry_detour::Kind::eNgxRuntimeD3D11Create,
            reinterpret_cast<void*>(kD3D11Creates[index]), route.d3d11Create);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
    {
        wchar_t detail[160]{};
        swprintf_s(detail, L"DLSS 5 DirectX 11 route: NGX runtime D3D11 entries hooked=%d", installed ? 1 : 0);
        Report(detail);
    }
    return installed;
}

bool ReadOnlyImageSpan(const Image& image, uintptr_t address, size_t bytes) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!image.Contains(address, bytes)
        || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != image.module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || memory.Protect != PAGE_READONLY) return false;
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= region && address - region < memory.RegionSize
        && bytes <= memory.RegionSize - (address - region);
}

uintptr_t HostRelativeTarget(uintptr_t start, size_t offset) noexcept
{
    int32_t displacement = 0;
    memcpy(&displacement, reinterpret_cast<const void*>(start + offset), 4);
    return start + offset + 4 + displacement;
}

bool OwnedCfgDispatch(const Image& image, uintptr_t slot) noexcept
{
    const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    constexpr size_t required = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer) + sizeof(uint64_t);
    if (!directory.VirtualAddress || directory.Size < required
        || !ReadOnlyImageSpan(image, image.base + directory.VirtualAddress, required)) return false;
    const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(image.base + directory.VirtualAddress);
    if (config->Size < required || config->GuardCFDispatchFunctionPointer != slot
        || !ReadOnlyImageSpan(image, slot, sizeof(uintptr_t))) return false;
    uintptr_t callable = 0;
    memcpy(&callable, reinterpret_cast<const void*>(slot), sizeof(callable));
    MEMORY_BASIC_INFORMATION memory{};
    if (!callable || VirtualQuery(reinterpret_cast<const void*>(callable), &memory, sizeof(memory)) != sizeof(memory)
        || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || (memory.Protect != PAGE_EXECUTE_READ && memory.Protect != PAGE_EXECUTE)) return false;
    // Windows may replace the provider's default CFG dispatcher with its own
    // system-image implementation. Bind the declared immutable slot and retain
    // the actual executable owner without substituting a callable.
    return Pin(static_cast<HMODULE>(memory.AllocationBase));
}

template<size_t N> bool ValidateHostOperands(const Image& image, uintptr_t start,
    const std::array<ampere_host_contracts::Operand, N>& operands) noexcept
{
    using Kind = ampere_host_contracts::OperandKind;
    uintptr_t logger = 0;
    for (const auto& operand : operands)
    {
        const uintptr_t target = HostRelativeTarget(start, operand.offset);
        if (operand.kind == Kind::eLogger)
        {
            uint32_t begin = 0, end = 0;
            if ((logger && target != logger) || !FunctionBounds(image, target, begin, end)) return false;
            logger = target;
            continue;
        }
        if (operand.kind == Kind::eCfgSlot && !OwnedCfgDispatch(image, target)) return false;
        const size_t bytes = operand.anchor ? strlen(operand.anchor) + 1
            : operand.kind == Kind::eCfgSlot ? sizeof(uintptr_t) : 4;
        if (!ReadOnlyImageSpan(image, target, bytes)
            || (operand.anchor && memcmp(reinterpret_cast<const void*>(target), operand.anchor, bytes))) return false;
    }
    return logger != 0;
}

template<size_t N, size_t R> uintptr_t FindHostContract(const Image& image,
    const std::array<uint8_t, N>& body, size_t bytes, size_t ownerOffset, size_t ownerBytes,
    const std::array<ampere_host_contracts::Operand, R>& operands, unsigned& matches) noexcept
{
    std::array<uint8_t, N> mask{};
    mask.fill(255);
    for (const auto& operand : operands)
    {
        if (operand.offset + 4 > bytes) return 0;
        memset(mask.data() + operand.offset, 0, 4);
    }
    unsigned found = 0;
    const uintptr_t start = FindPattern(image, body.data(), mask.data(), bytes, SIZE_MAX, 0, 0, &found);
    matches += found;
    uint32_t begin = 0, end = 0;
    if (!start || start < image.base + ownerOffset
        || !FunctionBounds(image, start - ownerOffset, begin, end) || end - begin != ownerBytes
        || ownerOffset + bytes > ownerBytes || !ValidateHostOperands(image, start, operands)) return 0;
    return start;
}

bool LegacyPresetContract(const Image& image, unsigned& matches) noexcept
{
    uintptr_t selected = 0;
    for (const auto& profile : ampere_host_contracts::kPresets)
        if (const auto found = FindHostContract(image, profile.body, profile.bytes,
                profile.ownerOffset, profile.ownerBytes, profile.operands, matches)) selected = found;
    return selected != 0 && matches == 1;
}

void RetainLegacyPresetContract(size_t index, const Route& route) noexcept
{
    if (index >= gLegacyPresetRoutes.size()) return;
    auto& proof = gLegacyPresetRoutes[index];
    const bool known = route.prepared && route.legacyPresetReady && !route.runtime
        && route.module && route.generation;
    proof.generation.store(0);
    proof.provider.store(known ? route.module : nullptr);
    if (known) proof.generation.store(route.generation);
}

// The route's Create entry of one API (Vulkan: Create1 when current, else Create).
entry_detour::Handle RouteCreate(const Route& route, GraphicsApi api) noexcept
{
    if (api != GraphicsApi::eVulkan) return route.create;
    return RouteEntryCurrent(route, route.vkCreate1) ? route.vkCreate1 : route.vkCreate;
}

// Create, Evaluate and Release of the device's API all still belong to this route.
bool RouteEntriesCurrent(const Route& route, GraphicsApi api) noexcept
{
    if (api == GraphicsApi::eVulkan)
        return (RouteEntryCurrent(route, route.vkCreate) || RouteEntryCurrent(route, route.vkCreate1))
            && RouteEntryCurrent(route, route.vkEvaluate) && RouteEntryCurrent(route, route.vkRelease);
    return route.create && route.evaluate && route.release
        && RouteEntryCurrent(route, route.create)
        && RouteEntryCurrent(route, route.evaluate)
        && RouteEntryCurrent(route, route.release);
}

bool RoutePresetCurrent(const Route& route) noexcept
{
    if (!route.prepared || route.runtime || !route.module || !route.generation) return false;
    if (route.preset) return RouteEntryCurrent(route, route.preset);
    if (!route.legacyPresetReady || !RouteEntryCurrent(route, RouteCreate(route, DeviceApi()))) return false;
    // Admission belongs to this retained route, independently of which prepared
    // provider is currently selected for status or later feature creation.
    for (size_t i = 0; i < gRoutes.size(); ++i)
    {
        if (&gRoutes[i] != &route) continue;
        const auto& proof = gLegacyPresetRoutes[i];
        const uint64_t generation = proof.generation.load();
        return generation == route.generation && proof.provider.load() == route.module
            && generation == proof.generation.load();
    }
    return false;
}

// The settings reader called with the DLSS-FG preset ID. 310.9.16 reads it at
// two decoded call sites (the setting and its report); several sites are
// admitted only when every one calls the same reader, as on RTX 40.
uintptr_t FindPresetReaderTarget(const Image& image, unsigned& matches) noexcept
{
    // mov ecx,DLSS-FG preset ID; call settings reader; test al,al; jz rel32.
    constexpr uint8_t pattern[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8,0,0,0,0,0x84,0xc0,0x0f,0x84,0,0,0,0};
    constexpr uint8_t mask[]{255,255,255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0};
    const uintptr_t match = FindPattern(image, pattern, mask, sizeof(pattern), SIZE_MAX, 0, 0, &matches);
    if (!match) return matches > 1 ? reinterpret_cast<uintptr_t>(dlssg_preset::FindPresetReader(image.module)) : 0;
    int32_t displacement = 0; memcpy(&displacement, reinterpret_cast<void*>(match + 6), 4);
    return match + 10 + displacement;
}

bool InstallPreset(Route& route, const Image& image) noexcept
{
    if (route.preset) return entry_detour::ReadSnapshot(route.preset).current;
    if (route.legacyPresetReady) return true;
    unsigned matches = 0;
    const uintptr_t target = FindPresetReaderTarget(image, matches);
    unsigned legacyMatches = 0;
    const bool legacy = LegacyPresetContract(image, legacyMatches);
    if (legacy && matches == 0 && Pin(image.module))
    {
        route.legacyPresetReady = true;
        Report(L"Ampere FG preset contract: provider accepts sole legacy preset 1; modern preset selection is unavailable");
        return true;
    }
    if (legacyMatches) return Reject(140);
    if (!target)
    {
        wchar_t detail[192]{};
        swprintf_s(detail, L"Ampere FG preset discovery rejected: decoded call-site matches=%u (%s); module=%p generation=%llu",
            matches, matches ? L"ambiguous" : L"missing", route.module, route.generation);
        Report(detail);
        return Reject(140);
    }
    if (matches > 1)
    {
        wchar_t detail[160]{};
        swprintf_s(detail, L"Ampere FG preset discovery: %u decoded call sites share one reader at RVA 0x%llX",
            matches, static_cast<unsigned long long>(target - image.base));
        Report(detail);
    }
    // The known reader takes uint32_t ID in ecx, uint32_t* in rdx and returns
    // bool in al. Require its complete argument-capture prologue, independently
    // of the eligibility version. This rejects different private ABIs.
    constexpr uint8_t prologue[]{0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9};
    if (!image.Executable(target) || memcmp(reinterpret_cast<void*>(target), prologue, sizeof(prologue)) != 0) return Reject(141);
    void* trampoline = nullptr;
    entry_detour::InstallOptions options{}; options.generation = route.generation; options.allowRelocated = true;
    const size_t i = &route - gRoutes.data();
    if (!entry_detour::Install(entry_detour::Kind::eAmperePreset, route.module, reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(kPresets[i]), trampoline, options, &route.preset)) return Reject(142);
    uint32_t privateFlags = 0;
    auto reader = Original<PresetFn>(route.preset);
    if (!reader || (reader(0x10e41df6u, &privateFlags) && (privateFlags & 4u)))
    { gFatal.store(true); return Reject(143); }
    Report(L"Ampere FG preset query hook installed; selection is fixed at the first query; driver profiles unchanged");
    return true;
}

// Live DLSS-G preset switching on a prepared route. Its own reader hook
// stays the reader's only owner; dlssg_preset adds the endpoint gates and the
// configuration reset. Legacy (sole preset 1) routes stay restart-only.
void AdoptLivePresetReader(const Route& route) noexcept
{
    if (!route.preset || route.legacyPresetReady || route.runtime) return;
    const auto reader = entry_detour::ReadSnapshot(route.preset);
    const uint64_t start = GetTickCount64();
    const bool adopted = reader.current && dlssg_preset::AdoptRouteReader(route.module, reader.target,
        route.generation, route.preset);
    wchar_t detail[224]{};
    swprintf_s(detail, L"Ampere FG preset live switch: %ls (module=%p generation=%llu, %llu ms)",
        adopted ? L"configuration reset available" : L"configuration not established; a new preset applies after a restart",
        route.module, static_cast<unsigned long long>(route.generation),
        static_cast<unsigned long long>(GetTickCount64() - start));
    Report(detail);
}

uintptr_t DirectMaximumDeviceGate(const Image& image, unsigned& matches,
    uint8_t* nativeMaximum = nullptr) noexcept
{
    // Providers validate MultiFrameCount against a direct maximum of three
    // or five. Require the complete ComputeAndValidateTimeFactor body: nonzero
    // count, supported/unsupported device branches, index bounds, flags and
    // time-factor stores. Only image-relative data/call operands may move.
    // This profile neither changes the neural factory nor grants eligibility.
    constexpr uint8_t body[]{
        0x48,0x83,0xec,0x38,0x44,0x8b,0x81,0x18,0x05,0x00,0x00,0x4c,0x8b,0xc9,0x45,0x85,
        0xc0,0x75,0x29,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0xba,0x7a,0x01,0x00,0x00,0x4c,
        0x8d,0x05,0x00,0x00,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,
        0x00,0x00,0xb8,0x05,0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x84,0xd2,0x0f,0x84,
        0x93,0x00,0x00,0x00,0x41,0x83,0xf8,0x05,0x76,0x1e,0xc7,0x44,0x24,0x28,0x05,0x00,
        0x00,0x00,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0x44,0x89,0x44,0x24,0x20,0xba,0x83,
        0x01,0x00,0x00,0xe9,0xf3,0x00,0x00,0x00,0x8b,0x89,0x14,0x05,0x00,0x00,0x41,0x80,
        0xb9,0x10,0x05,0x00,0x00,0x00,0x41,0x8d,0x40,0x01,0x41,0x0f,0x44,0xc0,0x85,0xc9,
        0x0f,0x84,0xc1,0x00,0x00,0x00,0x3b,0xc8,0x0f,0x87,0xb9,0x00,0x00,0x00,0x0f,0x94,
        0xc0,0x0f,0x57,0xc0,0x41,0x88,0x81,0x21,0x05,0x00,0x00,0x0f,0x57,0xc9,0x41,0x83,
        0xf8,0x01,0xf3,0x49,0x0f,0x2a,0xc0,0x0f,0x97,0xc0,0x41,0x88,0x81,0x20,0x05,0x00,
        0x00,0x8b,0xc1,0xf3,0x0f,0x58,0x05,0x00,0x00,0x00,0x00,0xf3,0x48,0x0f,0x2a,0xc8,
        0xb8,0x01,0x00,0x00,0x00,0xf3,0x0f,0x5e,0xc8,0xf3,0x41,0x0f,0x11,0x89,0x1c,0x05,
        0x00,0x00,0x48,0x83,0xc4,0x38,0xc3,0x41,0x83,0xf8,0x01,0x74,0x2e,0x44,0x89,0x44,
        0x24,0x20,0x4c,0x8d,0x0d,0x00,0x00,0x00,0x00,0x4c,0x8d,0x05,0x00,0x00,0x00,0x00,
        0xba,0x8c,0x01,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,
        0x00,0xb8,0x05,0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x8b,0x89,0x14,0x05,0x00,
        0x00,0x83,0xf9,0x01,0x0f,0x84,0x54,0xff,0xff,0xff,0x89,0x4c,0x24,0x20,0x4c,0x8d,
        0x0d,0x00,0x00,0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xba,0x91,0x01,0x00,
        0x00,0x4c,0x8d,0x05,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0xb8,0x05,0x00,
        0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,0x89,0x44,0x24,0x28,0x4c,0x8d,0x0d,0x00,0x00,
        0x00,0x00,0x89,0x4c,0x24,0x20,0xba,0x9a,0x01,0x00,0x00,0x4c,0x8d,0x05,0x00,0x00,
        0x00,0x00,0x48,0x8d,0x0d,0x00,0x00,0x00,0x00,0xe8,0x00,0x00,0x00,0x00,0xb8,0x05,
        0x00,0xd0,0xba,0x48,0x83,0xc4,0x38,0xc3,
    };
    constexpr size_t operands[]{0x16,0x22,0x29,0x2e,0x55,0xb7,0xe5,0xec,0xf8,
        0xfd,0x121,0x128,0x134,0x139,0x14e,0x15e,0x165,0x16a};
    std::array<uint8_t, sizeof(body)> mask{};
    mask.fill(255);
    for (const size_t offset : operands) memset(mask.data() + offset, 0, 4);
    struct Layout { uint32_t countField, zeroLine; uint8_t maximum; };
    constexpr Layout layouts[]{ {0x518,0x17a,5}, {0x500,0x122,3},
        {0x500,0x11f,3}, {0x658,0x11f,3} };
    uintptr_t start = 0;
    uint8_t selectedMaximum = 0;
    matches = 0;
    for (const auto& layout : layouts)
    {
        std::array<uint8_t, sizeof(body)> exact{};
        std::copy(std::begin(body), std::end(body), exact.begin());
        const auto set = [&exact](size_t offset, uint32_t value) noexcept
        { memcpy(exact.data() + offset, &value, sizeof(value)); };
        set(0x07, layout.countField);
        set(0x6a, layout.countField - 4);
        set(0x71, layout.countField - 8);
        set(0x97, layout.countField + 9);
        set(0xad, layout.countField + 8);
        set(0xce, layout.countField + 4);
        set(0x10d, layout.countField - 4);
        exact[0x47] = layout.maximum;
        set(0x4e, layout.maximum);
        set(0x1b, layout.zeroLine);
        set(0x5f, layout.zeroLine + 9);
        set(0xf1, layout.zeroLine + 0x12);
        set(0x12d, layout.zeroLine + 0x17);
        set(0x157, layout.zeroLine + 0x20);
        unsigned found = 0;
        const uintptr_t candidate = FindPattern(image, exact.data(), mask.data(), exact.size(), SIZE_MAX, 0, 0, &found);
        matches += found;
        if (candidate) { start = candidate; selectedMaximum = layout.maximum; }
    }
    uint32_t begin = 0, end = 0;
    if (!start || matches != 1 || !FunctionBounds(image, start, begin, end) || end - begin != sizeof(body)) return 0;
    const auto target = [start](size_t offset) noexcept
    {
        int32_t displacement = 0;
        memcpy(&displacement, reinterpret_cast<const void*>(start + offset), 4);
        return start + offset + 4 + displacement;
    };
    // Every masked operand still belongs to this exact image; data must be
    // immutable. All four failure paths use the same owned logging function.
    const uintptr_t logger = target(0x2e);
    uint32_t loggerBegin = 0, loggerEnd = 0;
    if (!FunctionBounds(image, logger, loggerBegin, loggerEnd)) return 0;
    const auto readonlyData = [&image](uintptr_t address, size_t bytes) noexcept
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!image.Contains(address, bytes)
            || VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory)
            || memory.AllocationBase != image.module || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
            || memory.Protect != PAGE_READONLY) return false;
        const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        return address >= region && address - region < memory.RegionSize
            && bytes <= memory.RegionSize - (address - region);
    };
    for (const size_t offset : operands)
    {
        const uintptr_t address = target(offset);
        if (offset == 0x2e || offset == 0xfd || offset == 0x139 || offset == 0x16a)
        { if (address != logger) return 0; continue; }
        if (!readonlyData(address, 4)) return 0;
    }
    constexpr char functionName[] = "EndpointCoreInputs::ComputeAndValidateTimeFactor";
    for (const size_t offset : {size_t{0x22}, size_t{0xec}, size_t{0x134}, size_t{0x15e}})
        if (!readonlyData(target(offset), sizeof(functionName))
            || memcmp(reinterpret_cast<const void*>(target(offset)), functionName, sizeof(functionName))) return 0;
    uint32_t denominatorOffset = 0;
    memcpy(&denominatorOffset, reinterpret_cast<const void*>(target(0xb7)), sizeof(denominatorOffset));
    if (denominatorOffset != 0x3f800000u) return 0; // index / (generatedCount + 1.0f)
    // All control-flow operands are exact in the body above. In particular,
    // JZ +0x93 selects the count==1 route; fall-through retains the discovered
    // bounded count, index checks and output writes in this owning function.
    if (nativeMaximum) *nativeMaximum = selectedMaximum;
    return start + 0x3c;
}

uintptr_t InlineMaximumDeviceGate(const Image& image, unsigned& matches) noexcept
{
    using Kind = ampere_host_contracts::OperandKind;
    constexpr std::array<ampere_host_contracts::Operand, 4> tailOperands{{
        {0x0c,Kind::eData,nullptr},
        {0x18,Kind::eData,"EndpointCore::ValidateMultiFrameCount"},
        {0x1f,Kind::eData,nullptr}, {0x24,Kind::eLogger,nullptr},
    }};
    uintptr_t selected = 0;
    for (const auto& profile : ampere_host_contracts::kInlineCounts)
    {
        const uintptr_t start = FindHostContract(image, profile.body, profile.body.size(),
            profile.ownerOffset, profile.ownerBytes, profile.operands, matches);
        if (!start) continue;
        const uintptr_t tail = start + profile.tailOffset;
        if (profile.ownerOffset + profile.tailOffset + profile.tail.size() > profile.ownerBytes
            || !image.Executable(tail) || !image.Executable(tail + profile.tail.size() - 1)) continue;
        bool same = true;
        for (size_t i = 0; i < profile.tail.size(); ++i)
        {
            bool operand = false;
            for (const auto& relative : tailOperands)
                operand |= i >= relative.offset && i < relative.offset + 4;
            if (!operand && reinterpret_cast<const uint8_t*>(tail)[i] != profile.tail[i]) { same = false; break; }
        }
        if (!same || !DecodedWindow(image, tail, profile.tail.size())
            || !ValidateHostOperands(image, tail, tailOperands)
            || HostRelativeTarget(tail, 0x24) != HostRelativeTarget(start, 0x2c)
            || HostRelativeTarget(tail, 0x18) != HostRelativeTarget(start, 0x1b)) continue;
        // Both final index checks retain the exact shared error tail. The
        // architecture query remains intact; only its following policy branch
        // is eligible for replacement. Count/index bounds remain mandatory.
        selected = start;
    }
    return matches == 1 ? selected : 0;
}

struct DeviceGateWrite
{
    uintptr_t address = 0;
    std::array<uint8_t, 2> expected{}, replacement{};
    size_t bytes = 0;
};
struct DeviceGatePlan
{
    std::array<DeviceGateWrite, 3> writes{};
    size_t count = 0;
    uint8_t nativeMaximum = 0;
    bool direct = false, inlined = false;
};

bool FindDeviceGatePlan(const Image& image, DeviceGatePlan& plan) noexcept
{
    constexpr uint8_t pattern[]{0x84,0xd2,0x0f,0x84,0x03,0x01,0,0,0xbe,5,0,0,0};
    constexpr uint8_t mask[]{255,255,255,255,255,255,255,255,255,255,255,255,255};
    unsigned modernMatches = 0, directMatches = 0, inlineMatches = 0;
    uint8_t directMaximum = 0;
    const uintptr_t modern = FindPattern(image, pattern, mask, sizeof(pattern), SIZE_MAX, 0, 0, &modernMatches);
    const uintptr_t direct = DirectMaximumDeviceGate(image, directMatches, &directMaximum);
    const uintptr_t inlined = InlineMaximumDeviceGate(image, inlineMatches);
    if ((!modern && !direct && !inlined) || modernMatches + directMatches + inlineMatches != 1) return false;
    plan = {};
    plan.nativeMaximum = inlined ? 3 : direct ? directMaximum : 5;
    plan.direct = direct != 0;
    plan.inlined = inlined != 0;
    if (plan.nativeMaximum == 3)
    {
        const uintptr_t comparison = inlined ? inlined + 0x4f : direct + 0x0b;
        const uintptr_t diagnostic = inlined ? inlined + 0x56 : direct + 0x12;
        // The full owning contracts prove these scalar immediates and their
        // zero upper bytes. Retain the bounded count/index checks and the
        // per-Evaluate output contract; six buffers are independently required
        // at Create and every Evaluate before reaching this provider path.
        plan.writes[plan.count++] = {comparison,{3,0},{5,0},1};
        plan.writes[plan.count++] = {diagnostic,{3,0},{5,0},1};
    }
    if (inlined)
    {
        // Retain the real device query and conditional opcode. Changing its
        // one-byte displacement to zero makes either outcome reach the same
        // bounded count path, without an unaligned two-byte publication.
        plan.writes[plan.count++] = {inlined + 0x4c,{0x22,0},{0,0},1};
    }
    else
    {
        const uintptr_t match = modern ? modern : direct;
        if (!image.Executable(match + 8 + (modern ? 0x103 : 0x93))) return false;
        // JMP +4 skips only the old JZ's relative operand.
        plan.writes[plan.count++] = {match + 2,{0x0f,0x84},{0xeb,0x04},2};
    }
    return true;
}

bool PublishDeviceGatePlan(const Image& image, const DeviceGatePlan& plan,
    protected_pointer::ProtectMemoryFn protectMemory = &VirtualProtect,
    protected_pointer::FlushInstructionCacheFn flush = &FlushInstructionCache) noexcept
{
    if (!plan.count || plan.count > plan.writes.size()) return false;
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto& write = plan.writes[i];
        if (!image.Contains(write.address, write.bytes) || !image.Executable(write.address)
            || (write.bytes != 1 && write.bytes != 2) || (write.address & (write.bytes - 1))
            || memcmp(reinterpret_cast<const void*>(write.address), write.expected.data(), write.bytes)) return false;
    }
    const auto replace = [protectMemory, flush](const DeviceGateWrite& write, bool restore) noexcept
    {
        PSAPI_WORKING_SET_EX_INFORMATION working{};
        working.VirtualAddress = reinterpret_cast<void*>(write.address);
        const bool privatePage = QueryWorkingSetEx(GetCurrentProcess(), &working, sizeof(working))
            && working.VirtualAttributes.Valid && !working.VirtualAttributes.Shared;
        return protected_pointer::ReplaceProtectedBytes(write.address,
            restore ? write.replacement.data() : write.expected.data(),
            restore ? write.expected.data() : write.replacement.data(), write.bytes,
            PAGE_EXECUTE_READ, protectMemory, flush, privatePage ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY);
    };
    // Count and diagnostic limits are published first, the device branch last.
    // The caller proves the pre-first-Create boundary and retains its owner.
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto result = replace(plan.writes[i], false);
        if (result.disposition == protected_pointer::PublishDisposition::ePublishedRestored) continue;
        bool restored = true;
        for (size_t undo = i + 1; undo != 0; --undo)
        {
            const auto& write = plan.writes[undo - 1];
            if (!memcmp(reinterpret_cast<const void*>(write.address), write.expected.data(), write.bytes)
                && protected_pointer::ProtectionMatches(write.address, PAGE_EXECUTE_READ, write.bytes)) continue;
            restored &= replace(write, true).disposition == protected_pointer::PublishDisposition::ePublishedRestored;
        }
        Report(restored ? L"Ampere FG host policy publication failed; complete original contract restored"
            : L"Ampere FG host policy publication failed; rollback incomplete, provider remains blocked");
        return false;
    }
    return true;
}

bool DeviceGate(const Image& image) noexcept
{
    DeviceGatePlan plan{};
    if (!FindDeviceGatePlan(image, plan)) return Reject(144);
    if (!Pin(image.module)) return Reject(145);
    if (!PublishDeviceGatePlan(image, plan)) { gFatal.store(true); return Reject(146); }
    if (plan.nativeMaximum == 3)
        Report(L"Ampere FG host policy: native generated maximum 3 extended to bounded 5; index/resource validation retained");
    else if (plan.direct)
        Report(L"Ampere FG device policy: direct maximum 5 profile published; count/index bounds and time-factor stores verified");
    return true;
}
} // namespace

void SetLogCallback(LogCallback callback) noexcept { gLog.store(callback); ampere_gpu::SetLogCallback(callback); }
void SetFeatureLifetimeCallback(FeatureLifetimeCallback callback) noexcept
{ gFeatureLifetimeCallback.store(callback, std::memory_order_release); }
void SetCurrentFeatureCreateAttemptToken(uint64_t token) noexcept
{
    std::lock_guard lock(gCreateAttemptMutex);
    if (gCurrentCreateAttempt == SIZE_MAX) return;
    auto& proof = gCreateAttemptsInProgress[gCurrentCreateAttempt];
    if (!proof.active || !proof.valid) return;
    if (!token || (proof.token && proof.token != token))
    { proof.valid = false; return; }
    proof.token = token;
}

FeatureCreatePendingState FeatureCreatePendingStatus(uint64_t token) noexcept
{
    using State = FeatureCreatePendingState;
    if (!token) return State::eNone;
    CreateAttemptProof proof{};
    {
        std::unique_lock lock(gCreateAttemptMutex, std::try_to_lock);
        if (!lock.owns_lock()) return State::eReadBusy;
        for (const auto& candidate : gCreateAttemptsInProgress)
        {
            if (!candidate.active || candidate.token != token) continue;
            if (proof.active) return State::eInvalid;
            proof = candidate;
        }
    }
    if (proof.active)
    {
        if (!proof.valid) return State::eInvalid;
        // The scoped runtime operation may publish its token before it has
        // reached the provider/capacity boundary. Its current runtime entry
        // proves unfinished work only; defer without certifying a feature.
        if (!proof.provider || !proof.wrapper)
        {
            const auto runtime = entry_detour::ReadSnapshot(proof.runtimeCreate);
            return Ready() && runtime.current && RuntimeCreateKind(runtime.kind, proof.api)
                && reinterpret_cast<uintptr_t>(runtime.owner) == proof.runtime
                && runtime.generation == proof.runtimeGeneration ? State::eReadBusy : State::eInvalid;
        }
        return CreateAttemptProofCurrent(proof) ? State::eCurrent : State::eInvalid;
    }
    bool released = false;
    FeatureLifetimeEvent selected{};
    {
        std::unique_lock lock(gFeatureLifetimeDeliveryMutex, std::try_to_lock);
        if (!lock.owns_lock()) return State::eReadBusy;
        bool invalid = false;
        const auto select = [&](const PendingFeatureLifetimeEvent& pending) {
            const auto& event = pending.event;
            const auto& candidate = pending.create;
            if ((event.phase != FeatureLifetimePhase::eCreated && event.phase != FeatureLifetimePhase::eReleased)
                || event.createAttemptToken != token)
                return false;
            if (!event.handle || !event.lifetime || !event.runtime || !event.runtimeGeneration
                || !event.provider || !event.providerGeneration || !event.wrapper || !event.wrapperGeneration
                || !event.publication || !event.adapterLuid
                || !ampere_policy::CertifiedCapacityValid(event.generatedFrameCapacity))
            { invalid = true; return false; }
            if (selected.handle)
            {
                // A successful Release can queue behind this exact Created
                // notification. Preserve FIFO and wait for both transitions;
                // another handle/lifetime may never reuse its operation token.
                if (released || event.phase != FeatureLifetimePhase::eReleased
                    || selected.handle != event.handle || selected.lifetime != event.lifetime
                    || selected.runtime != event.runtime || selected.runtimeGeneration != event.runtimeGeneration
                    || selected.provider != event.provider || selected.providerGeneration != event.providerGeneration
                    || selected.wrapper != event.wrapper || selected.wrapperGeneration != event.wrapperGeneration
                    || selected.publication != event.publication || selected.adapterLuid != event.adapterLuid)
                { invalid = true; return false; }
            }
            if (event.phase == FeatureLifetimePhase::eReleased)
            {
                // This immutable event exists only after a successful native
                // Release. Its old capacity need not survive teardown; wait
                // for the observer to retire that exact retained identity.
                selected = event;
                released = true;
                return true;
            }
            if (event.createAttemptToken != candidate.token
                || event.runtime != candidate.runtime || event.runtimeGeneration != candidate.runtimeGeneration
                || event.provider != candidate.provider || event.providerGeneration != candidate.providerGeneration
                || event.wrapper != candidate.wrapper || event.wrapperGeneration != candidate.wrapperGeneration
                || event.publication != candidate.publication || event.adapterLuid != candidate.luid
                || event.generatedFrameCapacity != candidate.capacity.maximumGeneratedFrames)
            { invalid = true; return false; }
            selected = event;
            proof = candidate;
            return true;
        };
        bool found = select(gDeliveringFeatureLifetimeEvent);
        size_t inspected = 0;
        for (const auto& pending : gPendingFeatureLifetimeEvents)
        {
            if (++inspected > 64) return State::eInvalid;
            found = select(pending) || found;
        }
        if (invalid) return State::eInvalid;
        if (!found) return State::eNone;
    }
    if (released)
    {
        const GraphicsApi api = selected.api == static_cast<uint32_t>(GraphicsApi::eVulkan)
            ? GraphicsApi::eVulkan : GraphicsApi::eD3D12;
        const auto runtime = CreateEntry(reinterpret_cast<HMODULE>(selected.runtime), true, api);
        const auto provider = CreateEntry(reinterpret_cast<HMODULE>(selected.provider), false, api);
        return runtime.current && runtime.currentEntries == 1 && runtime.generation == selected.runtimeGeneration
            && provider.current && provider.currentEntries == 1 && provider.generation == selected.providerGeneration
            && selected.provider == reinterpret_cast<uintptr_t>(ampere_gpu::Provider())
            && selected.publication == ampere_gpu::Publication() && selected.adapterLuid == ampere_gpu::AdapterLuid()
            && selected.wrapper == gActiveWrapper.load(std::memory_order_acquire)
            && selected.wrapperGeneration == gActiveWrapperGeneration.load(std::memory_order_acquire)
            ? State::eCurrent : State::eInvalid;
    }
    return CreateAttemptProofCurrent(proof) ? State::eCurrent : State::eInvalid;
}

bool FeatureCreatePendingCurrent(uint64_t token) noexcept
{ return FeatureCreatePendingStatus(token) == FeatureCreatePendingState::eCurrent; }
bool ObserveD3D12Device(void* device) noexcept
{
    const bool verified = ampere_gpu::ObserveD3D12Device(device);
    gD3D12Confirmed.store(verified);
    return verified;
}
bool ObserveVulkanPhysicalDevice(void* physicalDevice) noexcept
{
    const bool verified = ampere_gpu::ObserveVulkanPhysicalDevice(physicalDevice);
    gVulkanConfirmed.store(verified);
    return verified;
}
void ObserveVulkanDevice(void* device, bool startup) noexcept
{
    // Only a device created on (or handed over with) the verified physical
    // device is recorded. A startup bracket owns the host's current device; an
    // NGX Init only fills an unknown one. Features keep their own identity, so
    // a replaced device refuses their later Evaluate.
    if (!device || !gVulkanConfirmed.load() || !AdapterVerified()) return;
    const auto value = reinterpret_cast<uintptr_t>(device);
    if (startup) { gVulkanDevice.store(value, std::memory_order_release); return; }
    uintptr_t expected = 0;
    if (!gVulkanDevice.compare_exchange_strong(expected, value, std::memory_order_acq_rel) && expected != value)
    {
        wchar_t detail[160]{};
        swprintf_s(detail, L"Ampere Vulkan NGX Init device %p differs from the host's device %p; RTX 30 Create keeps the host's",
            device, reinterpret_cast<void*>(expected));
        Report(detail);
    }
}
GraphicsApi DeviceApi() noexcept
{
    // One process uses one API; D3D12 keeps precedence if both were observed.
    return gD3D12Confirmed.load() ? GraphicsApi::eD3D12
        : gVulkanConfirmed.load() ? GraphicsApi::eVulkan : GraphicsApi::eNone;
}
bool AdapterVerified() noexcept { return ampere_gpu::AdapterVerified(); }
bool Ready() noexcept { return DeviceConfirmed() && AdapterVerified() && ampere_gpu::Ready() && gStartupComplete.load() && !gFatal.load(); }
uint32_t FailureCode() noexcept
{
    if (const auto fatal = gFatalFailure.load()) return fatal;
    if (const auto primary = gFailures.Primary().code) return primary;
    return ampere_gpu::FailureCode() ? ampere_gpu::FailureCode() : gFailure.load();
}
ampere_diagnostics::Snapshot Diagnostics() noexcept
{
    return {gFailures.First(), gFailures.Primary(), gFailures.Last(),
        gCreateAttempts.load(), gCreateBlockedBeforeProvider.load(), gEvaluateAttempts.load(),
        gCandidateProviderVersion.load(), ampere_gpu::PreparationStage(), gStartupFailureMask.load()};
}
bool EarlyInitObserved() noexcept { return gEarlyInit.load(std::memory_order_acquire); }
// The Create gate's own startup terms. Before a startup has finished the mask is
// still 0: admission is not known yet, so nothing is held.
bool AdmissionUnavailable() noexcept
{
    return gFatal.load() || (!gStartupComplete.load()
        && gStartupFailureMask.load(std::memory_order_acquire) != 0);
}
uint64_t PresetQueries() noexcept { return gPresetQueries.load(); }
HMODULE PresetProvider() noexcept
{ return reinterpret_cast<HMODULE>(gPresetProvider.load(std::memory_order_acquire)); }
fg_lifetime::Snapshot ReadFrameGenerationLiveness() noexcept { return gFgLifetime.Read(); }
bool FrameGenerationQuiescent() noexcept { return gFgLifetime.Quiescent(); }
bool LegacySinglePreset() noexcept
{
    if (gFatal.load()) return false;
    const HMODULE provider = ampere_gpu::Provider();
    if (!provider) return false;
    const auto current = CreateEntry(provider, false, DeviceApi());
    if (!current.current || current.currentEntries != 1 || current.owner != provider || !current.generation) return false;
    // Every retained route has its own immutable capability proof. Preparing
    // another provider must not replace the currently selected program's
    // status, and a later Bind may select either already-prepared program.
    for (const auto& proof : gLegacyPresetRoutes)
    {
        const uint64_t generation = proof.generation.load();
        if (generation != current.generation || proof.provider.load() != provider
            || generation != proof.generation.load()) continue;
        return provider == ampere_gpu::Provider() && !gFatal.load();
    }
    return false;
}
uint64_t Evaluations() noexcept { return gEvaluations.load(); }
uint64_t Rejections() noexcept { return gRejections.load(); }
uint32_t NativeMaximumGeneratedFrames() noexcept { return gNativeMaximum.load(); }
uint32_t NativeMaximumReadResult() noexcept { return gNativeMaximumResult.load(); }
uint32_t StartupMaximumGeneratedFrames() noexcept { return gStartupMaximum.load(); }
uint32_t PresentationBuffers() noexcept { return gPresentationBuffers.load(); }
uint32_t CertifiedMaximumGeneratedFrames() noexcept { return gCertifiedMaximum.load(std::memory_order_acquire); }
uint32_t RingMaximumGeneratedFrames() noexcept
{
    const uint64_t generation = gActiveWrapperGeneration.load(std::memory_order_acquire);
    const uint32_t ring = gRingMaximum.load(std::memory_order_acquire);
    return generation && gRingWrapperGeneration.load(std::memory_order_acquire) == generation ? ring : 0;
}
void ObserveActiveWrapper(HMODULE module, uint64_t generation) noexcept
{
    // The control path may hold its own locks. Publish without taking gCalls
    // so a concurrent NGX callback cannot introduce a lock-order inversion.
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (gActiveWrapper.load() == base && gActiveWrapperGeneration.load() == generation) return;
    if (gWrapperWriter.test_and_set(std::memory_order_acquire)) return;
    gCertifiedMaximum.store(0, std::memory_order_release);
    gPresentationBuffers.store(0, std::memory_order_release);
    gActiveWrapperGeneration.store(0, std::memory_order_release);
    gActiveWrapper.store(base, std::memory_order_release);
    gActiveWrapperGeneration.store(generation, std::memory_order_release);
    gWrapperWriter.clear(std::memory_order_release);
}
int32_t LastCreateCount() noexcept { return gLastCreateCount.load(); }
uint64_t CreatedFeatures() noexcept { return gCreatedFeatures.load(); }
uint64_t SubmittedBatches() noexcept { return gSubmittedBatches.load(); }
uint64_t FailedBatches() noexcept { return gFailedBatches.load(); }
uint64_t RecoveredBatches() noexcept { return gRecoveredBatches.load(); }
uint64_t OutputReuseLatches() noexcept { return gOutputReuseLatches.load(); }

bool PatchProvider(HMODULE module, const wchar_t* path) noexcept
{
    std::lock_guard callLock(gCalls);
    if (!AdapterVerified() || gFatal.load() || (!gStartupDepth && !gStartupComplete.load())) return false;
    for (const auto& route : gRoutes)
        if (route.module == module && RoutePresetCurrent(route)
            && RouteEntriesCurrent(route, DeviceApi()) && ampere_gpu::PreparedProvider(module))
            return ampere_gpu::PatchProvider(module, path);
    return false;
}

void ObserveEarlyInit() noexcept { gEarlyInit.store(true); }
void SetPreparationBoundaryResolver(PreparationBoundaryResolver resolver) noexcept
{ gPreparationBoundaryResolver.store(resolver, std::memory_order_release); }
void BeginInit() noexcept
{
    if (++gInitDepth == 1) gInitRequirementQueries.store(0, std::memory_order_relaxed);
    // Unknown/Ada slInit is a native call and can wait on another thread's NGX
    // callbacks. Unknown Ampere requirement overrides acquire gCalls inside
    // Requirements/ObserveModule, covering each actual mutation and restore.
    // Remember the depth that acquired a persistent Ampere lock: selection can
    // change inside slInit, and nested scopes must unwind the original owner.
    if (!gInitLockDepth
        && gpu_dispatch::UsesTranslatedProgram()
        )
    {
        gCalls.lock();
        gInitLockDepth = gInitDepth;
    }
}
void EndInit() noexcept
{
    if (!gInitDepth) return;
    std::lock_guard lock(gCalls);
    for (auto& route : gRoutes)
        if (route.availability.active) route.availability.Set(false);
    // Without a supported FG answer here, Streamline has already unloaded
    // sl.dlss_g and the startup bootstrap will find no wrapper (177).
    if (gInitDepth == 1 && ampere_provider_arch::Armed() && !gInitRequirementQueries.load(std::memory_order_relaxed))
        Report(L"Ampere FG requirements: no FrameGeneration requirements query reached the NGX runtime hook during slInit");
    if (gInitLockDepth == gInitDepth)
    {
        gInitLockDepth = 0;
        gCalls.unlock();
    }
    --gInitDepth;
}
bool BeforeFirstFeatureCreate() noexcept
{ return !gPipelineCreateObserved.load(std::memory_order_acquire) && !gCreateAttempts.load() && !gCreatedFeatures.load(); }
bool BeginNativePreparation() noexcept
{
    // Init forwarding owns its short pre-call boundary lock. Never wait for
    // another thread that may already hold gCalls around native startup and
    // require this same forwarding entry. Missing the early window fails closed.
    if (!gCalls.try_lock()) return false;
    if (!BeforeFirstFeatureCreate() || gStartupComplete.load() || gFatal.load())
    { gCalls.unlock(); return false; }
    ++gPreparationDepth;
    return true;
}
void EndNativePreparation() noexcept
{
    if (!gPreparationDepth) return;
    --gPreparationDepth;
    gCalls.unlock();
}
void BeginStartup() noexcept
{
    gCalls.lock(); ++gStartupDepth;
    if (GetModuleHandleW(L"RTX30MFGCore.dll") || GetModuleHandleW(L"RTX40MFGCore.dll"))
    { gFatal.store(true); Reject(156); }
    if (!gEarlyInit.load()) { gFatal.store(true); Reject(155); }
}
void EndStartup(bool accepted) noexcept
{
    for (auto& route : gRoutes)
        if (route.availability.active) route.availability.Set(false);
    bool runtimeCovered = false, providerCovered = false;
    const GraphicsApi api = DeviceApi();
    for (const auto& route : gRoutes)
    {
        // The confirming device's API names the entries every FG call takes.
        const bool covered = route.prepared && RouteEntriesCurrent(route, api);
        runtimeCovered |= covered && route.runtime;
        providerCovered |= covered && RoutePresetCurrent(route)
            && ampere_gpu::PreparedProvider(route.module);
    }
    const uint32_t missing = (!accepted ? 0x01u : 0)
        | (!runtimeCovered ? 0x02u : 0) | (!providerCovered ? 0x04u : 0)
        | (!ampere_policy::CapacityValid(gStartupMaximum.load()) ? 0x08u : 0)
        | (!DeviceConfirmed() ? 0x10u : 0) | (!AdapterVerified() ? 0x20u : 0)
        | (!ampere_gpu::Ready() ? 0x40u : 0) | (gFatal.load() ? 0x80u : 0);
    const bool ready = missing == 0;
    gStartupFailureMask.store(missing, std::memory_order_release);
    gStartupComplete.store(ready);
    if (ready) { gFailure.store(0); gFailures.Recover(); }
    else
    {
        Reject(ampere_diagnostics::kStartupAdmissionFailure);
        wchar_t detail[320]{};
        swprintf_s(detail, L"Ampere startup admission incomplete: mask=0x%02X slStartupAccepted=%u runtimeCovered=%u providerCovered=%u "
            L"startupMaximum=%u device=%ls adapter=%u program=%u fatal=%u",
            missing, static_cast<unsigned>(accepted), static_cast<unsigned>(runtimeCovered), static_cast<unsigned>(providerCovered),
            gStartupMaximum.load(), api == GraphicsApi::eD3D12 ? L"d3d12" : api == GraphicsApi::eVulkan ? L"vulkan" : L"none",
            static_cast<unsigned>(AdapterVerified()),
            static_cast<unsigned>(ampere_gpu::Ready()), static_cast<unsigned>(gFatal.load()));
        Report(detail);
    }
    Report(ready ? (gStartupMaximum.load() == 1
        ? L"Ampere startup complete; legacy wrapper restricted to 2x; program prepared"
        : L"Ampere startup complete; temporary gates restored; MFG program prepared")
        : L"Ampere startup incomplete; temporary gates restored; MFG admission unavailable");
    --gStartupDepth; gCalls.unlock();
}

// The NGX core's own architecture gates read the DLSS-G provider's declared
// architecture (see ampere_provider_arch), so no private branch of the driver's
// runtime is patched. Ownership still distinguishes NVIDIA's installed runtime
// from game-local NGX intermediaries, which keep their native forwarding.
bool InspectRuntimeLayout(Route& route, const Image&, const wchar_t* path) noexcept
{
    if (!route.runtimeInspected)
    {
        route.runtimeInspected = true;
        route.runtimeDriverOwned = adapter_discovery::DriverNgxRuntimePath(path) && Pin(route.module);
        wchar_t detail[2048]{};
        swprintf_s(detail, L"Ampere NGX interface: driverOwned=%u role=%s; architecture gates follow the DLSS-G declaration path=%.1500ls",
            static_cast<unsigned>(route.runtimeDriverOwned),
            route.runtimeDriverOwned ? L"verified-runtime" : L"native-forwarding-unhooked", path ? path : L"");
        Report(detail);
    }
    return route.runtimeDriverOwned;
}

void ObserveModule(HMODULE module, const wchar_t* path, uint64_t generation,
    bool wrapper, bool provider, bool runtime) noexcept
{
    // Streamline can probe, unload and replace plugins during slInit, before
    // the device establishes the GPU family. Pinning a probe wrapper here can
    // preserve its pointers into a common plugin which the host then unloads.
    // Confirm Ampere before claiming routes or retaining any discovery owner.
    // BeginStartup re-inspects the surviving modules after device selection.
    if (!gpu_dispatch::UsesTranslatedProgram()) return;
    if (!module || !generation || (!wrapper && !provider && !runtime) || gFatal.load()) return;
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    Image image{};
    if (!image.Open(module)) return;
    Route* route = FindRoute(module, generation);
    if (!route) { Reject(150); return; }
    route->runtime = runtime;
    const size_t index = route - gRoutes.data();
    if (provider && !route->candidateLogged)
    {
        route->candidateLogged = true;
        dlssg_provider_policy::VersionTriplet version{};
        const bool read = dlssg_provider_policy::ReadProviderVersion(path, version);
        gCandidateProviderVersion.store(read ? (uint64_t{version.major} << 32)
            | (uint64_t{version.minor} << 16) | version.build : 0, std::memory_order_release);
        wchar_t detail[2048]{};
        swprintf_s(detail, L"Ampere provider candidate: version=%u.%u.%u versionRead=%u eligible=%u generation=%llu path=%.1600ls",
            version.major, version.minor, version.build, static_cast<unsigned>(read),
            static_cast<unsigned>(dlssg_provider_policy::IsSupportedAmpereProvider(module, path)), generation, path ? path : L"");
        Report(detail);
    }
    if (runtime) InspectRuntimeLayout(*route, image, path);
    if (wrapper && !route->wrapperInspected)
    {
        route->wrapperInspected = true;
        ampere_wrapper::Layout layout{};
        if (ampere_wrapper::Discover(module, layout) && Pin(module))
        {
            route->wrapperLayout = layout;
            Report(layout.maximumGeneratedFrames == 1
                ? L"Ampere legacy wrapper allocation layout recognized; 2x fallback requires early bootstrap and actual buffers"
                : L"Ampere MFG wrapper allocation layout recognized; waiting for early capability bootstrap and actual buffers");
        }
        else
            Report(L"Ampere MFG wrapper allocation layout unrecognized; startup capability bootstrap remains blocked");
    }
    if (provider && !route->prepared && (gStartupDepth || gPreparationDepth)
        && BeforeFirstFeatureCreate()
        && DeviceConfirmed() && AdapterVerified())
    {
        if (!dlssg_provider_policy::IsSupportedAmpereProvider(module, path)) { Reject(3); return; }
        // NGX validates the provider against the GPU when it maps the snippet.
        // An undeclared provider is refused by NGX itself; fail closed here too.
        if (const auto declared = ampere_provider_arch::Declare(module);
            declared != ampere_provider_arch::Outcome::eDeclared
            && declared != ampere_provider_arch::Outcome::eAlreadyDeclared)
        {
            // not-armed: RTX 30 detection failed. not-provider or shape-mismatch:
            // a provider another mod replaced or changed (the candidate line has its path).
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true, std::memory_order_relaxed))
            {
                wchar_t detail[160]{};
                swprintf_s(detail, L"Ampere DLSS-G provider not declared for RTX 30: outcome=%hs",
                    ampere_provider_arch::OutcomeName(declared));
                Report(detail);
            }
            Reject(157); return;
        }
        const auto resolveBoundary = gPreparationBoundaryResolver.load(std::memory_order_acquire);
        auto boundary = resolveBoundary ? resolveBoundary(module, generation) : ampere_gpu::PreparationBoundary{};
        // Prove the early owner before installing a preset reader or changing
        // the private device gate, not merely before descriptor publication.
        if (!boundary.Proven()) { Reject(15); return; }
        if (!InstallPreset(*route, image)) return;
        boundary = resolveBoundary(module, generation);
        if (!boundary.Proven()) { Reject(15); return; }
        if (!route->deviceGateReady)
            route->deviceGateReady = DeviceGate(image);
        boundary = resolveBoundary(module, generation);
        if (!route->deviceGateReady || !ampere_gpu::PatchProvider(module, path, boundary)) { Reject(151); return; }
        route->prepared = true;
        RetainLegacyPresetContract(index, *route);
        AdoptLivePresetReader(*route);
    }
    if (runtime && !route->prepared && (gInitDepth || gStartupDepth || gPreparationDepth))
    {
        // Owned NGX exports also occur in game-local intermediaries. Discovery
        // of one is not a failed driver admission; leave its forwarding intact.
        if (!route->runtimeDriverOwned) return;
        if (!InstallExport(*route, "NVSDK_NGX_D3D12_GetFeatureRequirements", entry_detour::Kind::eAmpereRequirements,
                reinterpret_cast<void*>(kRequirements[index]), route->requirements)
            || !InstallExport(*route, "NVSDK_NGX_D3D12_GetCapabilityParameters", entry_detour::Kind::eAmpereParameters,
                reinterpret_cast<void*>(kParameters[index]), route->parameters)) { Reject(153); return; }
        // A Vulkan host needs the runtime's Vulkan capability, requirement and
        // release entries as well; a D3D12 host keeps its admission unchanged.
        if (!InstallVulkanRuntime(*route, index) && DeviceApi() == GraphicsApi::eVulkan) { Reject(153); return; }
        route->prepared = true;
    }
    if (wrapper && (gInitDepth || gStartupDepth) && !route->availability.address)
    {
        const uintptr_t modern = FindPattern(image, kAmpereSlDlssgAvailabilityPattern.data(), kAmpereSlDlssgAvailabilityPatternMask.data(),
            kAmpereSlDlssgAvailabilityPattern.size(), kAmpereSlDlssgAvailabilityBranchOffset);
        const uintptr_t legacy = FindPattern(image, kAmpereSlDlssgAvailabilityLegacyPattern.data(), kAmpereSlDlssgAvailabilityLegacyPatternMask.data(),
            kAmpereSlDlssgAvailabilityLegacyPattern.size(), kAmpereSlDlssgAvailabilityLegacyBranchOffset);
        if ((modern != 0) != (legacy != 0))
        {
            if (!Pin(module)) return;
            route->availability = {(modern ? modern + kAmpereSlDlssgAvailabilityBranchOffset
                : legacy + kAmpereSlDlssgAvailabilityLegacyBranchOffset), kAmpereSlDlssgAvailabilityOriginal};
        }
    }
    if (gStartupDepth && DeviceConfirmed() && AdapterVerified() && route->availability.address)
        route->availability.Set(true);
}

bool InstallRoute(HMODULE module, const wchar_t* path, uint64_t generation, bool runtime,
    entry_detour::ForwardPreCall beforeCreate, entry_detour::ForwardPreCall beforeEvaluate) noexcept
{
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    if (!module || !generation) return false;
    // Provider entry routes serve every GPU: RTX 40 and RTX 50 keep the shared eligibility (310.9.16
    // included), RTX 30 its own (Ada-only builds excluded).
    if (!runtime && !(gpu_dispatch::UsesNativeProgram()
            ? dlssg_provider_policy::IsSupportedProvider(module, path)
            : dlssg_provider_policy::IsSupportedAmpereProvider(module, path))) return false;
    auto* route = FindRoute(module, generation);
    if (!route) return false;
    route->runtime = runtime;
    // Prove the private driver layout before installing any mutating callback.
    // A native intermediary continues to the separately verified runtime. If
    // that runtime is absent, startup and provider admission still fail closed.
    // Selected RTX 40 and RTX 50 callbacks are native pass-throughs and need no Ampere layout.
    if (runtime
        && !gpu_dispatch::UsesNativeProgram()
        )
    {
        Image image{};
        if (!image.Open(module) || !InspectRuntimeLayout(*route, image, path)) return false;
    }
    if (beforeCreate) route->beforeCreate = beforeCreate;
    if (beforeEvaluate) route->beforeEvaluate = beforeEvaluate;
    const size_t i = route - gRoutes.data();
    if (runtime && (!InstallExport(*route, "NVSDK_NGX_D3D12_GetFeatureRequirements", entry_detour::Kind::eAmpereRequirements,
            reinterpret_cast<void*>(kRequirements[i]), route->requirements)
        || !InstallExport(*route, "NVSDK_NGX_D3D12_GetCapabilityParameters", entry_detour::Kind::eAmpereParameters,
            reinterpret_cast<void*>(kParameters[i]), route->parameters))) return false;
    // Install Release and Evaluate first. No successful FG Create can escape
    // before its exact handle lifetime and evaluation boundary are covered.
    if (!InstallExport(*route, "NVSDK_NGX_D3D12_ReleaseFeature", entry_detour::Kind::eAmpereRelease,
            reinterpret_cast<void*>(kReleases[i]), route->release)
        || !InstallExport(*route, "NVSDK_NGX_D3D12_EvaluateFeature", runtime ? entry_detour::Kind::eNgxRuntimeD3D12EvaluateFeature : entry_detour::Kind::eNgxD3D12EvaluateFeature,
            reinterpret_cast<void*>(kEvaluates[i]), route->evaluate)) return false;
    const bool created = InstallExport(*route, "NVSDK_NGX_D3D12_CreateFeature", runtime ? entry_detour::Kind::eNgxRuntimeD3D12CreateFeature : entry_detour::Kind::eNgxD3D12CreateFeature,
        reinterpret_cast<void*>(kCreates[i]), route->create);
    // dx11nr.1: the D3D11 entries once the D3D12 ones are ours (the bridge runs DLSS 5 through their trampolines).
    if (runtime && created) InstallD3D11Route(*route, i);
    return created;
}

OptiScalerNgxRoute InstallOptiScalerNgxRoute(HMODULE optiscaler) noexcept
{
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    const bool runtime = std::any_of(gRoutes.begin(), gRoutes.end(), [](const Route& route) {
        return route.runtime && RouteEntryCurrent(route, route.create) && RouteEntryCurrent(route, route.evaluate)
            && RouteEntryCurrent(route, route.release); });
    if (!runtime) return OptiScalerNgxRoute::eWaiting;
    if (!optiscaler || (gOptiScalerNgx.module && gOptiScalerNgx.module != optiscaler))
        return OptiScalerNgxRoute::eFailed;
    gOptiScalerNgx.module = optiscaler;
    const auto install = [optiscaler](const char* name, entry_detour::Kind kind, void* hook,
        entry_detour::Handle& handle) noexcept
    {
        if (handle) return entry_detour::ReadSnapshot(handle).current;
        void* target = reinterpret_cast<void*>(GetProcAddress(optiscaler, name));
        // OptiScaler's own code only: a forwarded export is another module's entry.
        HMODULE owner = nullptr;
        if (!target || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(target), &owner)
            || owner != optiscaler) return false;
        void* trampoline = nullptr;
        entry_detour::InstallOptions options{}; options.allowRelocated = true;
        return entry_detour::Install(kind, optiscaler, target, hook, trampoline, options, &handle);
    };
    // Release and Evaluate first: a handle DLSS 5 takes at Create never escapes its evaluation or release.
    const bool installed = install("NVSDK_NGX_D3D12_ReleaseFeature", entry_detour::Kind::eOptiScalerNgxD3D12Release,
            reinterpret_cast<void*>(&OptiScalerRelease), gOptiScalerNgx.release)
        && install("NVSDK_NGX_D3D12_EvaluateFeature", entry_detour::Kind::eOptiScalerNgxD3D12Evaluate,
            reinterpret_cast<void*>(&OptiScalerEvaluate), gOptiScalerNgx.evaluate)
        && install("NVSDK_NGX_D3D12_CreateFeature", entry_detour::Kind::eOptiScalerNgxD3D12Create,
            reinterpret_cast<void*>(&OptiScalerCreate), gOptiScalerNgx.create);
    return installed ? OptiScalerNgxRoute::eInstalled : OptiScalerNgxRoute::eFailed;
}

bool InstallVulkanEntry(HMODULE owner, uint64_t generation, bool runtime, entry_detour::Kind kind,
    void* target, entry_detour::ForwardPreCall preCall, entry_detour::ForwardGate gate, void* rejected,
    const entry_detour::InstallOptions& options, entry_detour::Handle* handle) noexcept
{
    using entry_detour::Kind;
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    if (handle) *handle = {};
    Route* route = owner && generation ? FindRoute(owner, generation) : nullptr;
    void* rejectedTarget = rejected;
    if (route && runtime)
    {
        const size_t i = route - gRoutes.data();
        route->runtime = true;
        if (kind == Kind::eNgxRuntimeVulkanCreateFeature) rejectedTarget = reinterpret_cast<void*>(kVkCreates[i]);
        else if (kind == Kind::eNgxRuntimeVulkanCreateFeature1) rejectedTarget = reinterpret_cast<void*>(kVkCreate1s[i]);
        else if (kind == Kind::eNgxRuntimeVulkanEvaluateFeature) rejectedTarget = reinterpret_cast<void*>(kVkEvaluates[i]);
    }
    void* trampoline = nullptr;
    entry_detour::Handle installed{};
    const bool result = entry_detour::InstallForwarding(kind, owner, target, preCall, trampoline,
        options, &installed, gate, rejectedTarget);
    if (handle) *handle = installed;
    if (!result || !route) return result;
    switch (kind)
    {
    case Kind::eNgxVulkanCreateFeature: case Kind::eNgxRuntimeVulkanCreateFeature:
        route->vkCreate = installed; route->vkBeforeCreate = preCall; break;
    case Kind::eNgxVulkanCreateFeature1: case Kind::eNgxRuntimeVulkanCreateFeature1:
        route->vkCreate1 = installed; route->vkBeforeCreate1 = preCall; break;
    case Kind::eNgxVulkanEvaluateFeature: case Kind::eNgxRuntimeVulkanEvaluateFeature:
        route->vkEvaluate = installed; route->vkBeforeEvaluate = preCall; break;
    default: break;
    }
    return result;
}

bool InstallVulkanRoute(HMODULE module, const wchar_t* path, uint64_t generation, bool runtime) noexcept
{
    std::lock_guard callLock(gCalls);
    std::lock_guard lock(gInstall);
    if (!module || !generation) return false;
    // RTX 40/50 take only the runtime's Vulkan Release, a native
    // pass-through that needs no RTX 30 layout (as their D3D12 runtime route).
    if (gpu_dispatch::UsesNativeProgram())
    {
        if (!runtime) return false;
        auto* route = FindRoute(module, generation);
        if (!route) return false;
        route->runtime = true;
        return !GetProcAddress(module, "NVSDK_NGX_VULKAN_ReleaseFeature")
            || InstallExport(*route, "NVSDK_NGX_VULKAN_ReleaseFeature", entry_detour::Kind::eAmpereVulkanRelease,
                reinterpret_cast<void*>(kVkReleases[route - gRoutes.data()]), route->vkRelease);
    }
    if (!ampere_provider_arch::Armed()) return false;
    if (!runtime && !dlssg_provider_policy::IsSupportedAmpereProvider(module, path)) return false;
    auto* route = FindRoute(module, generation);
    if (!route) return false;
    route->runtime = runtime;
    if (!runtime) return InstallVulkanProvider(*route);
    // As for D3D12: only NVIDIA's installed runtime is hooked; a game-local
    // intermediary keeps native forwarding to it.
    Image image{};
    if (!image.Open(module) || !InspectRuntimeLayout(*route, image, path)) return false;
    return InstallVulkanRuntime(*route, route - gRoutes.data());
}

bool WINAPI VulkanProviderGate(void* arg1, uintptr_t arg2, const void* arg3, void* arg4,
    uintptr_t, uintptr_t, entry_detour::Handle entry, const void* caller) noexcept
{
    using entry_detour::Kind;
    const auto attempted = entry_detour::ReadSnapshot(entry);
    const bool create1 = attempted.kind == Kind::eNgxVulkanCreateFeature1;
    const bool create = create1 || attempted.kind == Kind::eNgxVulkanCreateFeature;
    const bool release = attempted.kind == Kind::eAmpereVulkanRelease;
    // CreateFeature1 is (VkDevice, VkCommandBuffer, feature, parameters, handle**).
    const uintptr_t feature = create1 ? reinterpret_cast<uintptr_t>(arg3) : arg2;
    const void* parameters = create1 ? static_cast<const void*>(arg4) : arg3;
    if (create && feature == NVSDK_NGX_Feature_FrameGeneration)
        gPipelineCreateObserved.store(true, std::memory_order_release);
    // Release forwards natively on other GPUs. The patcher's gate calls this
    // for Create/Evaluate only on RTX 30; anything else is refused.
    if (!gpu_dispatch::UsesTranslatedProgram()) return release;
    std::unique_lock callLock(gCalls);
    // Tail forwarding keeps NVIDIA's real runtime caller on the stack. Only a
    // nested call belonging to our tracked runtime operation is admitted.
    HMODULE callerModule = nullptr;
    if (!gCallingRuntime || !caller || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(caller), &callerModule)
        || callerModule != gCallingRuntime->module) return Reject(160);
    const auto snapshot = entry_detour::ReadSnapshot(entry);
    auto found = std::find_if(gRoutes.begin(), gRoutes.end(), [&](const Route& route) {
        return !route.runtime && route.module == snapshot.owner && route.generation == snapshot.generation; });
    if (!snapshot.current || found == gRoutes.end() || !found->prepared) return Reject(161);
    auto& route = *found;
    if (create)
    {
        if (!gCreateDepth || feature != NVSDK_NGX_Feature_FrameGeneration || !Ready() || !parameters
            || !gCallingWrapper || !ampere_wrapper::Matches(gCallingWrapper->wrapperLayout, gCreatingCapacity)
            || (gCreatingProvider && gCreatingProvider != &route)) return Reject(162);
        if (!ampere_gpu::BindProvider(route.module)) return Reject(161);
        if (!BindCurrentCreateProvider(route, entry)) return Reject(162);
        gCreatingProvider = &route;
        return true; // The relay runs the patcher's pre-call, then the provider.
    }
    auto* tracked = gCallingFeature;
    if (route.module != ampere_gpu::Provider() || !tracked
        || tracked->provider != reinterpret_cast<uintptr_t>(route.module)
        || tracked->providerGeneration != route.generation) return Reject(163);
    if (snapshot.kind == Kind::eNgxVulkanEvaluateFeature)
    {
        EvaluationInput input{};
        if (!gEvaluateDepth || !arg2 || !Ready() || gProviderEvaluated || !FeatureCapacityCurrent(*tracked)
            || !ReadVulkanEvaluationInput(static_cast<const NVSDK_NGX_Parameter*>(arg3), input, nullptr, nullptr,
                tracked->createdGeneratedFrames == 1 && ActiveWrapper()
                    && !ActiveWrapper()->wrapperLayout.frameIdAvailable ? gEvaluationInput.frame : 0)
            || !(input == gEvaluationInput)
            || (tracked->providerHandle && tracked->providerHandle != arg2)) return Reject(164);
        tracked->providerHandle = arg2;
        gProviderEvaluated = true;
        return true;
    }
    if (release)
        return gReleaseDepth && arg1 && (!tracked->providerHandle
            || tracked->providerHandle == reinterpret_cast<uintptr_t>(arg1)) ? true : Reject(165);
    return Reject(166);
}

bool VulkanProviderPrepared(HMODULE module) noexcept
{
    std::lock_guard callLock(gCalls);
    for (const auto& route : gRoutes)
        if (module && route.module == module && !route.runtime && route.prepared
            && RouteEntriesCurrent(route, GraphicsApi::eVulkan) && ampere_gpu::PreparedProvider(module))
            return true;
    return false;
}

bool DivertNativeVulkanCall(entry_detour::Handle entry, bool create, uintptr_t feature) noexcept
{
    gNativeVulkanDivert = {};
    if (!entry || !gpu_dispatch::UsesNativeProgram()) return false;
    if (create ? feature != NVSDK_NGX_Feature_FrameGeneration
            && !dlss_sr::Handles(static_cast<NVSDK_NGX_Feature>(feature))
        : !dlss_sr::Active())
        return false;
    std::lock_guard lock(gCalls);
    for (const auto& route : gRoutes)
    {
        if (!route.runtime || !route.module
            || !(create ? route.vkCreate == entry || route.vkCreate1 == entry : route.vkEvaluate == entry))
            continue;
        // A handle this Create returns must reach the route's own Release.
        if (!RouteEntryCurrent(route, entry) || !RouteEntryCurrent(route, route.vkRelease)) return false;
        gNativeVulkanDivert = entry;
        return true;
    }
    return false;
}

void ObserveProviderFrameGenerationCreate() noexcept { gFgLifetime.ObserveProviderCreate(); }

void ReportOverlaidEntry(const entry_detour::Snapshot& state) noexcept
{
    static std::atomic<uint64_t> reported{0};
    const auto kind = static_cast<uint32_t>(state.kind);
    const uint64_t bit = kind < 64 ? uint64_t{1} << kind : 0;
    if (!bit || (reported.fetch_or(bit, std::memory_order_acq_rel) & bit)) return;
    uint8_t bytes[5]{};
    const uint8_t* destination = nullptr;
    __try
    {
        const auto* entry = static_cast<const uint8_t*>(state.target);
        std::memcpy(bytes, entry, sizeof(bytes));
        int32_t displacement = 0;
        if (entry[0] == 0xE9) { std::memcpy(&displacement, entry + 1, 4); destination = entry + 5 + displacement; }
        else if (entry[0] == 0xEB) destination = entry + 2 + static_cast<int8_t>(entry[1]);
        // A hot-patch short jump lands on its rel32 jump above the entry.
        if (entry[0] == 0xEB && destination[0] == 0xE9)
        { std::memcpy(&displacement, destination + 1, 4); destination += 5 + displacement; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { destination = nullptr; }
    HMODULE module = nullptr;
    wchar_t owner[MAX_PATH]{}, hook[MAX_PATH]{};
    if (state.owner) GetModuleFileNameW(state.owner, owner, MAX_PATH);
    if (destination && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(destination), &module))
        GetModuleFileNameW(module, hook, MAX_PATH);
    wchar_t text[1024]{};
    swprintf_s(text, L"NGX native forward: entry kind %u at %ls+0x%X starts %02X %02X %02X %02X %02X, a jump to %ls+0x%llX "
        L"(another hook over ours, or ours back after one); NVIDIA's call continues through our trampoline "
        L"(reported once per entry kind)",
        kind, owner, state.targetRva, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
        module ? hook : (destination ? L"private memory" : L"unreadable"),
        static_cast<unsigned long long>(module ? reinterpret_cast<uintptr_t>(destination) - reinterpret_cast<uintptr_t>(module)
            : reinterpret_cast<uintptr_t>(destination)));
    Report(text);
}

bool VulkanLifetimeCovered() noexcept
{
    std::lock_guard lock(gCalls);
    bool covered = false;
    for (const auto& route : gRoutes)
    {
        if (!route.runtime || !route.module
            || !(RouteEntryCurrent(route, route.vkCreate) || RouteEntryCurrent(route, route.vkCreate1)))
            continue;
        // Every runtime whose Vulkan Create entries this process sees must also
        // have its Vulkan Evaluate and Release covered.
        if (!RouteEntriesCurrent(route, GraphicsApi::eVulkan)) return false;
        covered = true;
    }
    return covered;
}
} // namespace ampere_backend
