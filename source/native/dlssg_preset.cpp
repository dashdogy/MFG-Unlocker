#include "dlssg_preset.h"
#include "dlssg_preset_policy.h"
#include "dlssg_provider_policy.h"
#include "entry_detour.h"
#include "dlssg_preset_config.h"

#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace dlssg_preset
{
namespace
{
using GetSetting = int32_t (__cdecl*)(void*, void*, uint32_t, Setting*);
using QueryInterface = void* (__cdecl*)(uint32_t);
constexpr uint32_t kGetSettingInterface = 0x73BF8338u;
std::atomic<GetSetting> gOriginal{nullptr};
std::array<std::atomic<uint32_t>, 4> gFilterIds{};
std::atomic<SettingFilter> gFilter{nullptr};

bool Filtered(uint32_t id) noexcept
{
    if (!id) return false;
    for (const auto& filtered : gFilterIds)
        if (filtered.load(std::memory_order_acquire) == id) return true;
    return false;
}
std::atomic<bool> gInstalled{false};
std::atomic<uint32_t> gFailure{0};
std::mutex gInstallMutex;
std::mutex gReadMutex;
std::mutex gSelectionMutex;
SelectionSnapshot gSelection;
bool gSelectionInitialized = false;
std::once_flag gInitialSelectionOnce;
std::atomic<InitialSelectionLoader> gInitialSelectionLoader{nullptr};
enum class ReadSource : uint8_t { eNvapi, eCached, eRoute, eConfiguration, eReport };
struct ProviderRead
{
    HMODULE module = nullptr;
    uint64_t count = 0;
    uint64_t overrides = 0;
    uint64_t configurationReads = 0;
    uint64_t reportReads = 0;
    uint32_t value = 0;
    bool valueValid = false;
};
std::array<ProviderRead, 64> gReads{};
bool gReadOverflow = false; // Protected by gReadMutex; unknown readers deny reset.
// The driver's own answer to a provider's NVAPI preset query that this process
// overrode. Cached-reader providers keep that answer in a per-process DRS cache
// filled once, so after an override the cache holds the override; a later
// Game / driver selection replays the driver's answer instead.
struct DriverRead
{
    HMODULE module = nullptr;
    int32_t result = 0;
    uint32_t value = 0;
    bool overridden = false;
};
std::array<DriverRead, 64> gDriverReads{};
std::atomic<CreateQuiescenceProbe> gQuiescenceProbe{nullptr};
std::atomic<bool> gResetDenied{false};
using CachedGetSetting = bool (*)(uint32_t, uint32_t*);
struct CachedReader
{
    std::atomic<HMODULE> module{nullptr};
    std::atomic<void*> target{nullptr};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint32_t> failure{0};
    config::Layout config{}; // Immutable before module's release publication.
    std::atomic<bool> configReady{false};
    std::atomic<uint64_t> resets{0};
    // Another route's detour owns the reader (RTX 30/20 translated
    // route); immutable before module's release publication.
    bool routeOwned = false;
    entry_detour::Handle routeHook{};
};
std::array<CachedReader, 64> gCachedReaders{};
std::mutex gCachedInstallMutex;
// Covers every endpoint, including EstimateVRAM's temporary endpoint and all
// graphics APIs. Reset never waits for this gate while holding the selection
// lock: a failed try-lock leaves the saved request pending for the next call.
std::shared_mutex gEndpointGate;
thread_local uint32_t gEndpointDepth = 0;
struct EndpointCallScope
{
    std::shared_lock<std::shared_mutex> lock{gEndpointGate, std::defer_lock};
    EndpointCallScope()
    {
        if (!gEndpointDepth) lock.lock();
        ++gEndpointDepth;
    }
    ~EndpointCallScope() { --gEndpointDepth; }
};
// Preset C reaches only providers that implement it. Providers are retained for the
// process, so a module handle keeps one answer.
struct PresetCSupport
{
    HMODULE module = nullptr;
    bool supported = false;
};
std::array<PresetCSupport, 64> gPresetC{};
std::mutex gPresetCMutex;

void RecordRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden,
    ReadSource source = ReadSource::eNvapi) noexcept
{
    std::lock_guard lock(gReadMutex);
    for (auto& read : gReads)
    {
        if (read.module && read.module != provider) continue;
        read.module = provider;
        ++read.count;
        read.overrides += overridden ? 1 : 0;
        read.configurationReads += source == ReadSource::eConfiguration ? 1 : 0;
        read.reportReads += source == ReadSource::eReport ? 1 : 0;
        // NVAPI fills a DRS cache, and ReportOverrideStates only reports. Neither
        // proves that EndpointConfiguration consumed the returned preset.
        if (source == ReadSource::eConfiguration)
        {
            read.value = valueValid ? value : 0;
            read.valueValid = valueValid;
        }
        return;
    }
    gReadOverflow = true;
}

void RecordDriverRead(HMODULE provider, int32_t result, uint32_t value, bool overridden) noexcept
{
    std::lock_guard lock(gReadMutex);
    for (auto& read : gDriverReads)
    {
        if (read.module && read.module != provider) continue;
        read = {provider, result, value, overridden};
        break;
    }
}

// Game / driver on a provider whose DRS cache holds this process's override:
// the driver's recorded answer. False when the cache holds the driver's value.
bool ReplayDriverRead(HMODULE provider, bool& found, uint32_t& value) noexcept
{
    std::lock_guard lock(gReadMutex);
    for (const auto& read : gDriverReads)
    {
        if (read.module != provider) continue;
        if (!read.overridden) return false;
        found = read.result == kOk;
        value = read.value;
        return true;
    }
    return false;
}

bool PresetCSupported(HMODULE provider) noexcept
{
    if (!provider) return false;
    {
        std::lock_guard lock(gPresetCMutex);
        for (const auto& entry : gPresetC)
            if (entry.module == provider) return entry.supported;
    }
    const bool supported = dlssg_provider_policy::ProviderImplementsPresetC(provider);
    std::lock_guard lock(gPresetCMutex);
    for (auto& entry : gPresetC)
    {
        if (entry.module == provider) return entry.supported;
        if (!entry.module)
        {
            entry = {provider, supported};
            break;
        }
    }
    return supported;
}

// The selection this provider receives: C becomes the game/driver value on providers without it.
uint32_t EffectivePreset(uint32_t preset, HMODULE provider) noexcept
{
    return preset == kPresetC && !PresetCSupported(provider) ? kGameOrDriver : preset;
}

HMODULE Owner(const void* address) noexcept
{
    HMODULE module = nullptr;
    return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        ? module : nullptr;
}

bool IsDriverImplementation(HMODULE implementation) noexcept
{
    wchar_t prefix[32768]{};
    const UINT count = GetSystemDirectoryW(prefix, static_cast<UINT>(std::size(prefix)));
    if (!count || count >= std::size(prefix) - 64
        || wcscat_s(prefix, L"\\DriverStore\\FileRepository\\"))
        return false;
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(implementation, path, static_cast<DWORD>(std::size(path)));
    const wchar_t* name = length && length < std::size(path) ? wcsrchr(path, L'\\') : nullptr;
    return name && _wcsicmp(name + 1, L"nvapi64_impl.dll") == 0
        && _wcsnicmp(path, prefix, wcslen(prefix)) == 0;
}

bool Writable(const void* pointer, size_t bytes) noexcept
{
    uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
    if (!cursor || bytes > std::numeric_limits<uintptr_t>::max() - cursor)
        return false;
    const uintptr_t end = cursor + bytes;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory))
            || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            || !(memory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY
                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize > std::numeric_limits<uintptr_t>::max() - base)
            return false;
        const uintptr_t next = base + memory.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool OwnedExecutable(HMODULE module, const void* address, size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    if (!module || !address || !size || !VirtualQuery(address, &memory, sizeof(memory))
        || memory.AllocationBase != module || memory.Type != MEM_IMAGE
        || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return value >= region && value - region < memory.RegionSize
        && size <= memory.RegionSize - (value - region);
}

void* FindCachedReader(HMODULE module) noexcept
{
    __try
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        if (imageSize < 4096 || imageSize > 1024u*1024u*1024u
            || reinterpret_cast<uintptr_t>(sections + nt->FileHeader.NumberOfSections) - base > imageSize) return nullptr;
        // mov ecx,FG preset key; call bool(uint32_t,uint32_t*); test al; jz.
        constexpr uint8_t prefix[]{0xb9,0xf1,0x1d,0xe4,0x10,0xe8};
        constexpr uint8_t suffix[]{0x84,0xc0,0x0f,0x84};
        constexpr uint8_t prologue[]{0x48,0x89,0x5c,0x24,0x10,0x89,0x4c,0x24,0x08,
            0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x8b,0xd9};
        void* selected = nullptr;
        for (unsigned i = 0; i != nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= imageSize) continue;
            const size_t length = std::min<size_t>(section.Misc.VirtualSize, imageSize-section.VirtualAddress);
            const auto* start = reinterpret_cast<const uint8_t*>(base+section.VirtualAddress);
            for (size_t offset = 0; offset+18 <= length; ++offset)
            {
                const auto* call = start+offset;
                if (memcmp(call, prefix, sizeof(prefix)) || memcmp(call+10, suffix, sizeof(suffix))) continue;
                int32_t callOffset = 0, branchOffset = 0;
                memcpy(&callOffset, call+6, 4); memcpy(&branchOffset, call+14, 4);
                const uintptr_t target = reinterpret_cast<uintptr_t>(call+10)+callOffset;
                const uintptr_t branch = reinterpret_cast<uintptr_t>(call+18)+branchOffset;
                if (target < base || target-base >= imageSize || sizeof(prologue) > imageSize-(target-base)
                    || !OwnedExecutable(module, call, 18)
                    || !OwnedExecutable(module, reinterpret_cast<void*>(target), sizeof(prologue))
                    || !OwnedExecutable(module, reinterpret_cast<void*>(branch), 1)
                    || memcmp(reinterpret_cast<void*>(target), prologue, sizeof(prologue))) return nullptr;
                DWORD64 owner = 0;
                const auto* function = RtlLookupFunctionEntry(target, &owner, nullptr);
                const uintptr_t pdata = reinterpret_cast<uintptr_t>(function);
                if (!function || owner != base || pdata < base || pdata-base >= imageSize
                    || sizeof(*function) > imageSize-(pdata-base) || Owner(function) != module
                    || base+function->BeginAddress != target
                    || function->EndAddress <= function->BeginAddress || function->EndAddress > imageSize
                    || function->UnwindData >= imageSize
                    || (selected && selected != reinterpret_cast<void*>(target))) return nullptr;
                // 310.9.16 reads the key at two call sites (settings and their report); one reader only.
                selected = reinterpret_cast<void*>(target);
            }
        }
        return selected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Caller-classified record of one read through `reader`, whichever hook owns it.
void RecordReaderRead(const CachedReader& reader, HMODULE provider, const void* caller,
    bool valueValid, uint32_t value, bool overridden, ReadSource fallback) noexcept
{
    const auto address = reinterpret_cast<uintptr_t>(caller);
    const auto source = address == reader.config.configurationReturn ? ReadSource::eConfiguration
        : reader.config.report.Contains(address) ? ReadSource::eReport : fallback;
    RecordRead(provider, valueValid, valueValid ? value : 0, overridden, source);
}

template <size_t Index>
__declspec(noinline) bool HookCachedReader(uint32_t id, uint32_t* value) noexcept
{
    const void* caller = _ReturnAddress();
    auto& reader = gCachedReaders[Index];
    HMODULE provider = reader.module.load(std::memory_order_acquire);
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, provider);
    auto original = reinterpret_cast<CachedGetSetting>(state.original);
    if (!original) return false;
    if (state.current && state.generation == reader.generation.load(std::memory_order_acquire)
        && state.target == reader.target.load(std::memory_order_acquire)
        && Owner(caller) == provider && id == kSettingId && Writable(value, sizeof(*value)))
    {
        const uint32_t preset = EffectivePreset(FreezeSelection(), provider);
        const bool overridden = IsOverride(preset);
        bool result = true;
        uint32_t driverValue = 0;
        if (overridden) *value = preset;
        else if (ReplayDriverRead(provider, result, driverValue))
        {
            // The DRS cache holds an override from the selection that froze before.
            if (result) *value = driverValue;
        }
        else result = original(id, value);
        reader.reads.fetch_add(1, std::memory_order_relaxed);
        const bool valueValid = result && Writable(value, sizeof(*value));
        RecordReaderRead(reader, provider, caller, valueValid, valueValid ? *value : 0, overridden,
            ReadSource::eCached);
        return result;
    }
    return original(id, value);
}

template <size_t... Index>
constexpr auto CachedThunks(std::index_sequence<Index...>) noexcept
{
    return std::array<CachedGetSetting, sizeof...(Index)>{&HookCachedReader<Index>...};
}
constexpr auto kCachedThunks = CachedThunks(std::make_index_sequence<64>{});

using EndpointCreate = uint32_t (*)(void*, void*, void*, void*, uintptr_t, uintptr_t);
using EndpointEvaluate = uint32_t (*)(void*, void*, void*, void*);

template<size_t Index>
uint32_t HookEndpointCreate(void* a, void* b, void* c, void* d, uintptr_t e, uintptr_t f)
{
    EndpointCallScope gate;
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgEndpointCreate,
        gCachedReaders[Index].module.load(std::memory_order_acquire));
    const auto original = reinterpret_cast<EndpointCreate>(state.original);
    return original ? original(a, b, c, d, e, f) : 0xbad00001u;
}

template<size_t Index>
uint32_t HookEndpointEvaluate(void* a, void* b, void* c, void* d)
{
    EndpointCallScope gate;
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgEndpointEvaluate,
        gCachedReaders[Index].module.load(std::memory_order_acquire));
    const auto original = reinterpret_cast<EndpointEvaluate>(state.original);
    return original ? original(a, b, c, d) : 0xbad00001u;
}

template<size_t... I>
constexpr auto CreateThunks(std::index_sequence<I...>) noexcept
{ return std::array<EndpointCreate, sizeof...(I)>{&HookEndpointCreate<I>...}; }
template<size_t... I>
constexpr auto EvaluateThunks(std::index_sequence<I...>) noexcept
{ return std::array<EndpointEvaluate, sizeof...(I)>{&HookEndpointEvaluate<I>...}; }
constexpr auto kCreateThunks = CreateThunks(std::make_index_sequence<64>{});
constexpr auto kEvaluateThunks = EvaluateThunks(std::make_index_sequence<64>{});

bool ConfigHooksCurrent(const CachedReader& reader) noexcept
{
    if (!reader.configReady.load(std::memory_order_acquire)) return false;
    const auto module = reader.module.load(std::memory_order_acquire);
    const auto generation = reader.generation.load(std::memory_order_acquire);
    for (auto kind : {entry_detour::Kind::eDlssgCachedPresetRead,
        entry_detour::Kind::eDlssgEndpointCreate, entry_detour::Kind::eDlssgEndpointEvaluate})
    {
        const bool routeReader = reader.routeOwned && kind == entry_detour::Kind::eDlssgCachedPresetRead;
        // The owning route's exact entry, not an aggregate of its kind.
        const auto hook = routeReader ? entry_detour::ReadSnapshot(reader.routeHook)
            : entry_detour::ReadSnapshot(kind, module);
        if (routeReader && (hook.owner != module || hook.kind != entry_detour::Kind::eAmperePreset)) return false;
        const auto target = kind == entry_detour::Kind::eDlssgCachedPresetRead
            ? reinterpret_cast<uintptr_t>(reader.target.load(std::memory_order_acquire))
            : kind == entry_detour::Kind::eDlssgEndpointCreate ? reader.config.create.begin : reader.config.evaluate.begin;
        if (!hook.current || hook.generation != generation || reinterpret_cast<uintptr_t>(hook.target) != target)
            return false;
    }
    return true;
}

void InstallCachedReader(HMODULE module, uint64_t generation) noexcept
{
    std::lock_guard lock(gCachedInstallMutex);
    for (size_t index = 0; index != gCachedReaders.size(); ++index)
    {
        auto& reader = gCachedReaders[index];
        if (reader.module.load(std::memory_order_acquire) == module) return;
        if (reader.module.load(std::memory_order_acquire)) continue;
        void* target = FindCachedReader(module);
        PresetCSupported(module); // Settle Preset C support outside the provider's own reads.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned)
            || pinned != module) return;
        reader.config = config::Discover(module, target);
        reader.target.store(target, std::memory_order_relaxed);
        reader.generation.store(generation, std::memory_order_relaxed);
        reader.failure.store(target ? 0 : 4, std::memory_order_relaxed);
        reader.module.store(module, std::memory_order_release);
        if (!target) return; // Record unknown layout; retain the public NVAPI route.
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* original = nullptr;
        const bool installed = entry_detour::Install(entry_detour::Kind::eDlssgCachedPresetRead, module, target,
            reinterpret_cast<void*>(kCachedThunks[index]), original, options);
        const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, module);
        reader.failure.store(installed && state.current ? 0 : 100u+static_cast<uint32_t>(state.failure),
            std::memory_order_release);
        if (!installed || !state.current || !reader.config.slot) return;
        // The module is already pinned. Discover both entries before installing
        // either; partial installation stays transparent and cannot enable reset.
        bool create = entry_detour::Install(entry_detour::Kind::eDlssgEndpointCreate, module,
            reinterpret_cast<void*>(reader.config.create.begin), reinterpret_cast<void*>(kCreateThunks[index]),
            original, options);
        bool evaluate = entry_detour::Install(entry_detour::Kind::eDlssgEndpointEvaluate, module,
            reinterpret_cast<void*>(reader.config.evaluate.begin), reinterpret_cast<void*>(kEvaluateThunks[index]),
            original, options);
        reader.configReady.store(create && evaluate, std::memory_order_release);
        return;
    }
}

std::atomic<bool> gNvapiFrameGenerationReads{true};

__declspec(noinline) int32_t __cdecl HookGetSetting(
    void* session, void* profile, uint32_t id, Setting* setting) noexcept
{
    const void* caller = _ReturnAddress();
    const auto detour = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    GetSetting original = gOriginal.load(std::memory_order_acquire);
    // The registry publishes its original pointer before enabling the entry;
    // use it for calls racing publication of the fast-path pointer.
    if (!original) original = reinterpret_cast<GetSetting>(detour.original);
    if (!original) return -1;
    if (const auto filter = gFilter.load(std::memory_order_acquire);
        filter && id != kSettingId && Filtered(id))
    {
        const int32_t result = original(session, profile, id, setting);
        return filter(id, result, setting, detour.current && Writable(setting, sizeof(*setting)), Owner(caller));
    }
    const HMODULE provider = Owner(caller);
    const bool eligible = detour.current && id == kSettingId
        && gNvapiFrameGenerationReads.load(std::memory_order_acquire)
        && dlssg_provider_policy::IsSupportedRetainedProvider(provider)
        && Writable(setting, sizeof(*setting));
    // Freeze before entering the driver. A simultaneous config reload must
    // not change which process selection this first provider query receives.
    const uint32_t preset = eligible ? EffectivePreset(FreezeSelection(), provider) : kGameOrDriver;
    const int32_t result = original(session, profile, id, setting);
    if (!eligible || !Writable(setting, sizeof(*setting)) || !CompatibleRead(id, result, *setting))
        return result;
    // The result remains owned by the calling provider. Pin before recording
    // provenance so a later load at the same address cannot inherit it.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(caller), &pinned)
        || pinned != provider)
        return result;
    const uint32_t driverValue = result == kOk ? setting->current.dword : 0;
    const bool overridden = OverrideRead(id, result, *setting, preset);
    RecordDriverRead(provider, result, driverValue, overridden);
    const bool valueValid = overridden || result == kOk;
    RecordRead(provider, valueValid, valueValid ? setting->current.dword : 0, overridden);
    return overridden ? kOk : result;
}

void WINAPI BeforeBootstrap(void*, uintptr_t, const void*, void*,
    uintptr_t, uintptr_t, entry_detour::Handle handle, const void*) noexcept
{
    const auto state = entry_detour::ReadSnapshot(handle);
    if (state.current && dlssg_provider_policy::IsSupportedRetainedProvider(state.owner))
        Prepare();
}
}

bool SetRequested(uint32_t preset) noexcept
{
    if (!IsValidSelection(preset)) return false;
    std::lock_guard lock(gSelectionMutex);
    gSelectionInitialized = true;
    gSelection.requested = preset;
    if (!gSelection.selectionFrozen) gSelection.latched = preset;
    gSelection.restartRequired = gSelection.selectionFrozen && gSelection.requested != gSelection.latched;
    return true;
}

void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept
{
    gInitialSelectionLoader.store(loader, std::memory_order_release);
}

uint32_t FreezeSelection() noexcept
{
    std::call_once(gInitialSelectionOnce, [] {
        {
            std::lock_guard lock(gSelectionMutex);
            if (gSelectionInitialized) return;
        }
        const auto loader = gInitialSelectionLoader.load(std::memory_order_acquire);
        const uint32_t loaded = loader ? loader() : kPresetB;
        // The callback may wait on config I/O. A concurrent control update owns
        // the newer request and must win over the callback's older snapshot.
        std::lock_guard lock(gSelectionMutex);
        if (!gSelectionInitialized)
        {
            gSelection.requested = IsValidSelection(loaded) ? loaded : kPresetB;
            gSelection.latched = gSelection.requested;
            gSelectionInitialized = true;
        }
    });
    std::lock_guard lock(gSelectionMutex);
    gSelection.selectionFrozen = true;
    gSelection.restartRequired = gSelection.requested != gSelection.latched;
    return gSelection.latched;
}

uint32_t EffectiveSelection(uint32_t preset, HMODULE provider) noexcept
{
    return EffectivePreset(preset, provider);
}

SelectionSnapshot ReadSelection() noexcept
{
    std::lock_guard lock(gSelectionMutex);
    return gSelection;
}

void RecordProviderRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept
{
    if (provider) RecordRead(provider, valueValid, value, overridden, ReadSource::eRoute);
}

void SetCreateQuiescenceProbe(CreateQuiescenceProbe probe) noexcept
{
    gQuiescenceProbe.store(probe, std::memory_order_release);
}

bool RelatchPending() noexcept
{
    const auto probe = gQuiescenceProbe.load(std::memory_order_acquire);
    if (gEndpointDepth) return false;
    std::unique_lock gate(gEndpointGate, std::try_to_lock);
    if (!gate.owns_lock()) return false;
    std::lock_guard lock(gSelectionMutex);
    if (!gSelection.selectionFrozen || gSelection.requested == gSelection.latched || !probe || !probe())
        return false;
    // Validate every provider that has consumed a preset before any write.
    // Holding the endpoint gate excludes new Create/Evaluate calls even when
    // they bypass the runtime tracker (for example the VRAM estimate callback).
    std::array<CachedReader*, 64> selected{};
    std::array<config::ResetState, 64> states{};
    size_t count = 0;
    std::lock_guard reads(gReadMutex);
    if (gReadOverflow) { gResetDenied.store(true, std::memory_order_release); return false; }
    for (const auto& read : gReads)
    {
        if (!read.module || !read.count) continue;
        CachedReader* found = nullptr;
        for (auto& reader : gCachedReaders)
            if (reader.module.load(std::memory_order_acquire) == read.module) { found = &reader; break; }
        if (!found || !ConfigHooksCurrent(*found) || !config::ReadResetState(found->config, states[count]))
        { gResetDenied.store(true, std::memory_order_release); return false; }
        selected[count++] = found;
    }
    for (size_t i = 0; i < count; ++i)
    {
        // Once a reset is attempted, old configuration evidence must not remain
        // valid even if readback detects an external writer.
        for (auto& read : gReads)
            if (read.module == selected[i]->config.module) read.valueValid = false;
        if (!config::Reset(selected[i]->config, states[i]))
        { gResetDenied.store(true, std::memory_order_release); return false; }
        if (states[i].object) selected[i]->resets.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& read : gReads) read.valueValid = false;
    gSelection.latched = gSelection.requested;
    gSelection.selectionFrozen = false;
    gSelection.restartRequired = false;
    return true;
}

bool ProviderSupportsLiveSwitch(HMODULE provider) noexcept
{
    if (gResetDenied.load(std::memory_order_acquire)) return false;
    for (const auto& reader : gCachedReaders)
    {
        if (reader.module.load(std::memory_order_acquire) != provider) continue;
        return ConfigHooksCurrent(reader);
    }
    return false;
}

bool ReadProvidersSupportLiveSwitch() noexcept
{
    std::array<HMODULE, 64> providers{};
    size_t count = 0;
    {
        std::lock_guard lock(gReadMutex);
        if (gReadOverflow) return false;
        for (const auto& read : gReads)
            if (read.module && read.count) providers[count++] = read.module;
    }
    for (size_t i = 0; i != count; ++i)
        if (!ProviderSupportsLiveSwitch(providers[i])) return false;
    return count != 0;
}

bool AdoptRouteReader(HMODULE module, void* reader, uint64_t generation,
    entry_detour::Handle readerHook) noexcept
{
    if (!module || !reader || !generation || !readerHook) return false;
    const auto hook = entry_detour::ReadSnapshot(readerHook);
    if (!hook.current || hook.owner != module || hook.kind != entry_detour::Kind::eAmperePreset
        || hook.target != reader || hook.generation != generation)
        return false;
    std::lock_guard lock(gCachedInstallMutex);
    for (size_t index = 0; index != gCachedReaders.size(); ++index)
    {
        auto& entry = gCachedReaders[index];
        if (const auto owner = entry.module.load(std::memory_order_acquire); owner == module)
            return entry.routeOwned && entry.routeHook == readerHook && ConfigHooksCurrent(entry);
        else if (owner) continue;
        PresetCSupported(module); // Settle Preset C support outside the provider's own reads.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned)
            || pinned != module) return false;
        entry.config = config::Discover(module, reader);
        entry.routeOwned = true;
        entry.routeHook = readerHook;
        entry.target.store(reader, std::memory_order_relaxed);
        entry.generation.store(generation, std::memory_order_relaxed);
        entry.failure.store(entry.config.slot ? 0 : 5, std::memory_order_relaxed);
        // Published even for an unknown layout: the route's reads stay classified
        // as route reads and the provider stays restart-only.
        entry.module.store(module, std::memory_order_release);
        if (!entry.config.slot) return false;
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* original = nullptr;
        // As in InstallCachedReader: both endpoint entries or no reset.
        const bool create = entry_detour::Install(entry_detour::Kind::eDlssgEndpointCreate, module,
            reinterpret_cast<void*>(entry.config.create.begin), reinterpret_cast<void*>(kCreateThunks[index]),
            original, options);
        const bool evaluate = entry_detour::Install(entry_detour::Kind::eDlssgEndpointEvaluate, module,
            reinterpret_cast<void*>(entry.config.evaluate.begin), reinterpret_cast<void*>(kEvaluateThunks[index]),
            original, options);
        entry.configReady.store(create && evaluate, std::memory_order_release);
        return ConfigHooksCurrent(entry);
    }
    return false;
}

void RecordRouteRead(HMODULE provider, const void* caller, bool valueValid, uint32_t value,
    bool overridden) noexcept
{
    if (!provider) return;
    for (const auto& reader : gCachedReaders)
    {
        if (reader.module.load(std::memory_order_acquire) != provider) continue;
        if (reader.routeOwned)
        {
            RecordReaderRead(reader, provider, caller, valueValid, value, overridden, ReadSource::eRoute);
            return;
        }
        break;
    }
    RecordRead(provider, valueValid, value, overridden, ReadSource::eRoute);
}

void SetNvapiFrameGenerationReads(bool enabled) noexcept
{
    gNvapiFrameGenerationReads.store(enabled, std::memory_order_release);
}

void SetSettingFilter(uint32_t id, SettingFilter filter) noexcept
{
    if (!filter)
    {
        gFilter.store(nullptr, std::memory_order_release);
        for (auto& filtered : gFilterIds) filtered.store(0, std::memory_order_release);
        return;
    }
    for (auto& filtered : gFilterIds)
    {
        uint32_t empty = 0;
        if (filtered.load(std::memory_order_acquire) == id
            || filtered.compare_exchange_strong(empty, id, std::memory_order_acq_rel)) break;
    }
    gFilter.store(filter, std::memory_order_release);
}

bool Prepare() noexcept
{
    if (gInstalled.load(std::memory_order_acquire)) return ReadSnapshot(nullptr).installed;
    std::lock_guard lock(gInstallMutex);
    if (gInstalled.load(std::memory_order_acquire)) return ReadSnapshot(nullptr).installed;
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(std::size(path)));
    if (!length || length >= std::size(path) - 32 || wcscat_s(path, L"\\nvapi64.dll"))
    {
        gFailure.store(1, std::memory_order_release);
        return false;
    }
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
    {
        gFailure.store(2, std::memory_order_release);
        return false;
    }
    // Even a failed attempt must not unmap NVAPI (see nvidia_mfg_policy).
    HMODULE retained = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &retained);
    wchar_t loaded[32768]{};
    const DWORD loadedLength = GetModuleFileNameW(module, loaded, static_cast<DWORD>(std::size(loaded)));
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(module, "nvapi_QueryInterface"));
    void* target = loadedLength && loadedLength < std::size(loaded)
            && _wcsicmp(path, loaded) == 0 && Owner(reinterpret_cast<void*>(query)) == module
        ? query(kGetSettingInterface) : nullptr;
    const HMODULE implementation = Owner(target);
    HMODULE pinnedDispatcher = nullptr;
    HMODULE pinned = nullptr;
    if (!target || !implementation
        || (implementation != module && !IsDriverImplementation(implementation))
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(query), &pinnedDispatcher)
        || pinnedDispatcher != module
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(target), &pinned)
        || pinned != implementation)
    {
        FreeLibrary(module);
        gFailure.store(3, std::memory_order_release);
        return false;
    }
    FreeLibrary(module); // The hook's process-lifetime pin owns the image.
    entry_detour::InstallOptions options{};
    options.generation = 1; // The dispatcher and exact returned implementation are pinned.
    options.allowRelocated = true;
    void* original = nullptr;
    const bool installed = entry_detour::Install(entry_detour::Kind::eDlssgPresetRead,
        implementation, target, reinterpret_cast<void*>(&HookGetSetting), original, options);
    if (original) gOriginal.store(reinterpret_cast<GetSetting>(original), std::memory_order_release);
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    if (!installed || !state.current)
    {
        gFailure.store(100u + static_cast<uint32_t>(state.failure), std::memory_order_release);
        return false;
    }
    gFailure.store(0, std::memory_order_release);
    gInstalled.store(true, std::memory_order_release);
    return true;
}

void PrepareForExport(HMODULE module, const char* name) noexcept
{
    if (name && reinterpret_cast<uintptr_t>(name) > 0xFFFFu
        && std::strncmp(name, "NVSDK_NGX_", 10) == 0
        && dlssg_provider_policy::IsSupportedRetainedProvider(module))
    {
        // The host can resolve and call Init before the discovery worker sees
        // the image. Cover the cached reader at this same API boundary. Its
        // process-lifetime pin gives this private hook one immutable generation.
        InstallCachedReader(module, 1);
        Prepare();
    }
}

void ObserveProvider(HMODULE module, uint64_t generation) noexcept
{
    if (!generation || !dlssg_provider_policy::IsSupportedRetainedProvider(module))
        return;
    InstallCachedReader(module, generation);
    constexpr const char* names[] = {
        "NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_Init_Ext",
        "NVSDK_NGX_D3D12_GetFeatureRequirements", "NVSDK_NGX_VULKAN_GetFeatureRequirements",
        "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl", "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl"};
    constexpr const char* vulkanInitializers[] = {
        "NVSDK_NGX_VULKAN_Init", "NVSDK_NGX_VULKAN_Init_Ext", "NVSDK_NGX_VULKAN_Init_Ext2"};
    for (const char* name : names)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (!target || Owner(target) != module) continue;
        bool adapterEntry = false;
        for (const char* vulkanName : vulkanInitializers)
            adapterEntry |= target == reinterpret_cast<void*>(GetProcAddress(module, vulkanName));
        // Some unused legacy exports share a stub with Vulkan Init. The
        // existing Vulkan adapter pre-call prepares the preset hook as well.
        if (adapterEntry) continue;
        entry_detour::InstallOptions options{};
        options.generation = generation;
        options.allowRelocated = true;
        void* original = nullptr;
        entry_detour::InstallForwarding(entry_detour::Kind::eDlssgPresetBootstrap,
            module, target, &BeforeBootstrap, original, options);
    }
}

void* FindPresetReader(HMODULE module) noexcept
{
    return FindCachedReader(module);
}

Snapshot ReadSnapshot(HMODULE provider) noexcept
{
    const auto state = entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgPresetRead);
    Snapshot result{state.current, gFailure.load(std::memory_order_acquire), 0};
    const auto selection = ReadSelection();
    result.requested = selection.requested;
    result.latched = selection.latched;
    result.selectionFrozen = selection.selectionFrozen;
    result.restartRequired = selection.restartRequired;
    if (state.installed && !state.current)
        result.failure = 100u + static_cast<uint32_t>(entry_detour::Failure::eHookConflict);
    if (!provider) return result;
    result.presetCAvailable = PresetCSupported(provider);
    for (const auto& reader : gCachedReaders)
    {
        if (reader.module.load(std::memory_order_acquire) != provider) continue;
        const auto cached = reader.routeOwned ? entry_detour::ReadSnapshot(reader.routeHook)
            : entry_detour::ReadSnapshot(entry_detour::Kind::eDlssgCachedPresetRead, provider);
        result.cachedReaderInstalled = cached.current
            && cached.generation == reader.generation.load(std::memory_order_acquire)
            && cached.target == reader.target.load(std::memory_order_acquire);
        result.cachedReaderFailure = reader.failure.load(std::memory_order_acquire);
        if (cached.installed && !cached.current)
            result.cachedReaderFailure = 100u+static_cast<uint32_t>(entry_detour::Failure::eHookConflict);
        result.cachedReaderRva = cached.targetRva;
        result.cachedReaderReads = reader.reads.load(std::memory_order_acquire);
        result.configurationResetAvailable = ConfigHooksCurrent(reader);
        result.configurationResets = reader.resets.load(std::memory_order_relaxed);
        result.configurationSlotRva = reader.config.slot
            ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reader.config.slot) - reinterpret_cast<uintptr_t>(provider)) : 0;
        result.installed |= result.cachedReaderInstalled;
        if (result.cachedReaderInstalled) result.failure = 0;
        break;
    }
    std::lock_guard lock(gReadMutex);
    for (const auto& read : gReads)
        if (read.module == provider)
        {
            result.providerReads = read.count;
            result.overrideReads = read.overrides;
            result.configurationReads = read.configurationReads;
            result.reportReads = read.reportReads;
            result.observedPreset = read.value;
            result.observedPresetValid = read.valueValid;
            break;
        }
    return result;
}
}
