#include "entry_detour.h"
#include "process_mitigations.h"

#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

extern "C" void MfgUnlockForwardDispatch();

namespace entry_detour
{
namespace
{
// The registry is intentionally bounded. Installed entries are never recycled:
// their owner and trampoline must remain live for already-cached pointers.
// Sixteen control-route records can each consume five entries (public set/get,
// internal set/get, and release). Leave enough bounded space for all of those
// plus simultaneous provider/runtime and bootstrap entries.
constexpr size_t kRegistryCapacity = 128;
constexpr size_t kHotpatchPrefixBytes = 5;
constexpr size_t kAbsoluteJumpBytes = 14;
constexpr size_t kForwardRelayBytes = 26;
// MinHook publishes either a five-byte entry branch or a two-byte branch to a
// five-byte hotpatch region. Five entry bytes are sufficient to detect later
// replacement without reaching into an adjacent short function.
constexpr size_t kVerificationBytes = 5;
// The bytes after a relocated entry's patch. The trampoline resumes
// inside them (a relocated prologue is at most 5 + 14 bytes), so a later hook that
// changed none of them left the trampoline's continuation intact.
constexpr size_t kTailBytes = 19;

constexpr std::array<uint8_t, 6> kNgxCreateEntry{
    0x40, 0x53, 0x55, 0x56, 0x41, 0x56};
constexpr std::array<uint8_t, 4> kNgxEvaluateEntry{
    0x40, 0x53, 0x56, 0x57};
constexpr std::array<uint8_t, 5> kNgxRuntimeCreateEntry{
    0x48, 0x89, 0x6C, 0x24, 0x20};
constexpr std::array<uint8_t, 5> kNgxRuntimeEvaluateEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::array<uint8_t, 4> kDlssgPublicEntry{
    0x48, 0x83, 0xEC, 0x58};
constexpr std::array<uint8_t, 5> kSlInitEntry{
    0x48, 0x89, 0x54, 0x24, 0x10};
// slSetD3DDevice of the Streamline 2.14 interposer: mov [rsp+8], rbx.
constexpr std::array<uint8_t, 5> kSlSetD3DDeviceEntry{
    0x48, 0x89, 0x5C, 0x24, 0x08};

struct Slot
{
    std::atomic<bool> claimed{false};
    std::atomic<bool> installed{false};
    std::atomic<uint32_t> serial{0};
    std::atomic<uint32_t> kind{static_cast<uint32_t>(Kind::eCount)};
    std::atomic<uint32_t> method{static_cast<uint32_t>(Method::eUnavailable)};
    std::atomic<uint32_t> failure{static_cast<uint32_t>(Failure::eNone)};
    std::atomic<uint64_t> generation{0};
    std::atomic<HMODULE> owner{nullptr};
    std::atomic<void*> target{nullptr};
    std::atomic<void*> hook{nullptr};
    std::atomic<void*> original{nullptr};
    std::atomic<void*> branchRelay{nullptr};
    std::atomic<void*> forwardRelay{nullptr};
    std::atomic<HMODULE> retainedOwner{nullptr};
    std::atomic<ForwardPreCall> preCall{nullptr};
    std::atomic<ForwardGate> forwardGate{nullptr};
    std::atomic<void*> rejectedForwardTarget{nullptr};
    std::atomic<bool> filterForwardArg2{false};
    std::atomic<uintptr_t> requiredForwardArg2{0};
    std::array<uint8_t, kVerificationBytes> relocatedBytes{};
    std::array<uint8_t, kTailBytes> tailBytes{};
    std::atomic<bool> tailRecorded{false};
};

std::array<Slot, kRegistryCapacity> gSlots{};
std::mutex gInstallMutex;
std::atomic<uint32_t> gNextSerial{1};
bool gMinHookInitializationAttempted = false;
bool gMinHookReady = false;

bool IsKindValid(Kind kind) noexcept
{
    return static_cast<uint32_t>(kind)
        < static_cast<uint32_t>(Kind::eCount);
}

void SetFailure(Slot& slot, Failure failure) noexcept
{
    slot.failure.store(static_cast<uint32_t>(failure),
        std::memory_order_release);
}

bool IsExecutableProtection(DWORD protection) noexcept
{
    protection &= 0xFFu;
    return protection == PAGE_EXECUTE
        || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE
        || protection == PAGE_EXECUTE_WRITECOPY;
}

bool QueryExecutableImage(void* address, size_t bytes,
    HMODULE owner) noexcept
{
    if (!address || bytes == 0 || !owner)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE
        || memory.AllocationBase != owner
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0
        || !IsExecutableProtection(memory.Protect))
        return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return begin >= region && begin <= UINTPTR_MAX - bytes
        && begin + bytes <= region + memory.RegionSize;
}

size_t ReplayBytes(Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::eNgxD3D12CreateFeature:
    case Kind::eNgxD3D12EvaluateFeature:
        return 2;
    case Kind::eNgxRuntimeD3D12CreateFeature:
    case Kind::eNgxRuntimeD3D12EvaluateFeature:
    case Kind::eSlInit:
    case Kind::eOptiScalerSlSetD3DDevice:
        return 5;
    case Kind::eDlssgSetOptions:
    case Kind::eDlssgGetState:
        return 4;
    default:
        return 0;
    }
}

bool EntryMatches(Kind kind, const uint8_t* entry) noexcept
{
    if (!entry)
        return false;
    switch (kind)
    {
    case Kind::eNgxD3D12CreateFeature:
        return std::memcmp(entry, kNgxCreateEntry.data(),
            kNgxCreateEntry.size()) == 0;
    case Kind::eNgxD3D12EvaluateFeature:
        return std::memcmp(entry, kNgxEvaluateEntry.data(),
            kNgxEvaluateEntry.size()) == 0;
    case Kind::eNgxRuntimeD3D12CreateFeature:
        return std::memcmp(entry, kNgxRuntimeCreateEntry.data(),
            kNgxRuntimeCreateEntry.size()) == 0;
    case Kind::eNgxRuntimeD3D12EvaluateFeature:
        return std::memcmp(entry, kNgxRuntimeEvaluateEntry.data(),
            kNgxRuntimeEvaluateEntry.size()) == 0;
    case Kind::eDlssgSetOptions:
    case Kind::eDlssgGetState:
        return std::memcmp(entry, kDlssgPublicEntry.data(),
            kDlssgPublicEntry.size()) == 0;
    case Kind::eSlInit:
        return std::memcmp(entry, kSlInitEntry.data(),
            kSlInitEntry.size()) == 0;
    case Kind::eOptiScalerSlSetD3DDevice:
        return std::memcmp(entry, kSlSetD3DDeviceEntry.data(),
            kSlSetD3DDeviceEntry.size()) == 0;
    default:
        // Generic Streamline internal entries deliberately use the relocation
        // path; no version-dependent byte pattern is guessed here.
        return false;
    }
}

bool IsChainedPublicEntry(Kind kind, const uint8_t* entry) noexcept
{
    return (kind == Kind::eDlssgSetOptions
            || kind == Kind::eDlssgGetState)
        && entry && entry[0] == 0xE9;
}

void* ResolveRelativeJumpTarget(const uint8_t* entry) noexcept
{
    int32_t displacement = 0;
    std::memcpy(&displacement, entry + 1, sizeof(displacement));
    const intptr_t branchEnd = reinterpret_cast<intptr_t>(entry + 5);
    if ((displacement > 0
            && branchEnd > std::numeric_limits<intptr_t>::max()
                - displacement)
        || (displacement < 0
            && branchEnd < std::numeric_limits<intptr_t>::min()
                - displacement))
        return nullptr;
    void* target = reinterpret_cast<void*>(
        branchEnd + static_cast<intptr_t>(displacement));
    MEMORY_BASIC_INFORMATION memory{};
    return target
        && VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory)
        && memory.State == MEM_COMMIT
        && (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
        && IsExecutableProtection(memory.Protect)
        ? target : nullptr;
}

bool HotpatchPaddingMatches(const uint8_t* entry) noexcept
{
    for (size_t index = 1; index <= kHotpatchPrefixBytes; ++index)
    {
        if (entry[-static_cast<ptrdiff_t>(index)] != 0xCC)
            return false;
    }
    return true;
}

void BuildAbsoluteJump(uint8_t* output, void* target) noexcept
{
    output[0] = 0xFF;
    output[1] = 0x25;
    output[2] = output[3] = output[4] = output[5] = 0;
    std::memcpy(output + 6, &target, sizeof(target));
}

void* AllocateExecutable(const void* bytes, size_t length) noexcept
{
    if (!bytes || length == 0)
        return nullptr;
    void* memory = VirtualAlloc(nullptr, length,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        return nullptr;
    std::memcpy(memory, bytes, length);
    FlushInstructionCache(GetCurrentProcess(), memory, length);
    DWORD prior = 0;
    if (!VirtualProtect(memory, length, PAGE_EXECUTE_READ, &prior))
    {
        VirtualFree(memory, 0, MEM_RELEASE);
        return nullptr;
    }
    return memory;
}

bool ExecutablePrivateCurrent(void* memory) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    return memory
        && VirtualQuery(memory, &info, sizeof(info)) == sizeof(info)
        && info.State == MEM_COMMIT && info.Type == MEM_PRIVATE
        && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
        && IsExecutableProtection(info.Protect);
}

bool RelayCurrent(void* relay, void* target) noexcept
{
    if (!ExecutablePrivateCurrent(relay) || !target)
        return false;
    std::array<uint8_t, kAbsoluteJumpBytes> expected{};
    BuildAbsoluteJump(expected.data(), target);
    return std::memcmp(relay, expected.data(), expected.size()) == 0;
}

void* AllocateNearRelay(const uint8_t* branchEnd, void* target) noexcept
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const uintptr_t origin = reinterpret_cast<uintptr_t>(branchEnd);
    const uintptr_t processMinimum = reinterpret_cast<uintptr_t>(
        system.lpMinimumApplicationAddress);
    const uintptr_t processMaximum = reinterpret_cast<uintptr_t>(
        system.lpMaximumApplicationAddress);
    const uintptr_t reach = static_cast<uintptr_t>(INT32_MAX);
    const uintptr_t low = origin > reach ? origin - reach : processMinimum;
    const uintptr_t high = origin <= processMaximum - reach
        ? origin + reach : processMaximum;
    const uintptr_t granularity = system.dwAllocationGranularity;
    uintptr_t cursor = (low / granularity) * granularity;
    if (cursor < low)
        cursor += granularity;

    while (cursor <= high)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory,
                sizeof(memory)) != sizeof(memory))
            break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
        const uintptr_t end = base <= UINTPTR_MAX - memory.RegionSize
            ? base + memory.RegionSize : UINTPTR_MAX;
        if (memory.State == MEM_FREE)
        {
            uintptr_t candidate = cursor > base ? cursor : base;
            candidate = ((candidate + granularity - 1) / granularity)
                * granularity;
            if (candidate <= high && candidate < end
                && end - candidate >= kAbsoluteJumpBytes)
            {
                void* relay = VirtualAlloc(reinterpret_cast<void*>(candidate),
                    kAbsoluteJumpBytes, MEM_RESERVE | MEM_COMMIT,
                    PAGE_READWRITE);
                if (relay)
                {
                    const intptr_t delta = static_cast<uint8_t*>(relay)
                        - branchEnd;
                    if (delta >= std::numeric_limits<int32_t>::min()
                        && delta <= std::numeric_limits<int32_t>::max())
                    {
                        std::array<uint8_t, kAbsoluteJumpBytes> code{};
                        BuildAbsoluteJump(code.data(), target);
                        std::memcpy(relay, code.data(), code.size());
                        FlushInstructionCache(GetCurrentProcess(), relay,
                            code.size());
                        DWORD prior = 0;
                        if (VirtualProtect(relay, code.size(),
                                PAGE_EXECUTE_READ, &prior)
                            && RelayCurrent(relay, target))
                            return relay;
                    }
                    VirtualFree(relay, 0, MEM_RELEASE);
                }
            }
        }
        if (end <= cursor || end == UINTPTR_MAX)
            break;
        cursor = ((end + granularity - 1) / granularity) * granularity;
    }
    return nullptr;
}

void* AllocateHotpatchTrampoline(const uint8_t* entry, size_t replay,
    void* chainTarget) noexcept
{
    std::array<uint8_t, 32> code{};
    size_t offset = 0;
    void* tail = chainTarget;
    if (!tail)
    {
        if (replay == 0 || replay + kAbsoluteJumpBytes > code.size())
            return nullptr;
        std::memcpy(code.data(), entry, replay);
        offset = replay;
        tail = const_cast<uint8_t*>(entry) + replay;
    }
    BuildAbsoluteJump(code.data() + offset, tail);
    return AllocateExecutable(code.data(), offset + kAbsoluteJumpBytes);
}

void* AllocateForwardRelay(Slot* slot) noexcept
{
    // mov r10, <slot>; mov r11, <dispatcher>; jmp r11
    std::array<uint8_t, kForwardRelayBytes> code{
        0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
        0x41, 0xFF, 0xE3, 0xCC, 0xCC, 0xCC};
    void* context = slot;
    void* dispatcher = reinterpret_cast<void*>(&MfgUnlockForwardDispatch);
    std::memcpy(code.data() + 2, &context, sizeof(context));
    std::memcpy(code.data() + 12, &dispatcher, sizeof(dispatcher));
    return AllocateExecutable(code.data(), code.size());
}

bool RestoreProtection(void* address, size_t bytes,
    DWORD originalProtection) noexcept
{
    DWORD ignored = 0;
    if (!VirtualProtect(address, bytes, originalProtection, &ignored))
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory)
        && (memory.Protect & 0xFFu) == (originalProtection & 0xFFu);
}

Handle HandleFor(size_t index, const Slot& slot) noexcept
{
    return {static_cast<uint32_t>(index),
        slot.serial.load(std::memory_order_acquire)};
}

bool KeyMatches(const Slot& slot, Kind kind, HMODULE owner, void* target,
    uint64_t generation) noexcept
{
    return slot.claimed.load(std::memory_order_acquire)
        && slot.kind.load(std::memory_order_acquire)
            == static_cast<uint32_t>(kind)
        && slot.owner.load(std::memory_order_acquire) == owner
        && slot.target.load(std::memory_order_acquire) == target
        && slot.generation.load(std::memory_order_acquire) == generation;
}

Slot* FindKeyLocked(Kind kind, HMODULE owner, void* target,
    uint64_t generation, size_t& index) noexcept
{
    for (size_t current = 0; current < gSlots.size(); ++current)
    {
        if (KeyMatches(gSlots[current], kind, owner, target, generation))
        {
            index = current;
            return &gSlots[current];
        }
    }
    return nullptr;
}

Slot* ReserveLocked(Kind kind, HMODULE owner, void* target,
    uint64_t generation, size_t& index) noexcept
{
    if (Slot* existing = FindKeyLocked(
            kind, owner, target, generation, index))
        return existing;
    for (size_t current = 0; current < gSlots.size(); ++current)
    {
        Slot& slot = gSlots[current];
        if (slot.claimed.load(std::memory_order_acquire))
            continue;
        uint32_t serial = gNextSerial.fetch_add(1, std::memory_order_relaxed);
        if (serial == 0)
            serial = gNextSerial.fetch_add(1, std::memory_order_relaxed);
        slot.serial.store(serial, std::memory_order_relaxed);
        slot.kind.store(static_cast<uint32_t>(kind),
            std::memory_order_relaxed);
        slot.owner.store(owner, std::memory_order_relaxed);
        slot.target.store(target, std::memory_order_relaxed);
        slot.generation.store(generation, std::memory_order_relaxed);
        slot.failure.store(static_cast<uint32_t>(Failure::eNone),
            std::memory_order_relaxed);
        slot.claimed.store(true, std::memory_order_release);
        index = current;
        return &slot;
    }
    return nullptr;
}

bool TargetAlreadyDetouredLocked(HMODULE owner, void* target,
    const Slot* requested) noexcept
{
    for (const Slot& slot : gSlots)
    {
        if (&slot == requested
            || !slot.installed.load(std::memory_order_acquire))
            continue;
        if (slot.owner.load(std::memory_order_acquire) == owner
            && slot.target.load(std::memory_order_acquire) == target)
            return true;
    }
    return false;
}

bool Current(const Slot& slot) noexcept
{
    if (!slot.installed.load(std::memory_order_acquire))
        return false;
    auto* entry = static_cast<uint8_t*>(
        slot.target.load(std::memory_order_acquire));
    const Method method = static_cast<Method>(
        slot.method.load(std::memory_order_acquire));
    if (method == Method::eHotpatch)
    {
        void* branchTarget = slot.branchRelay.load(std::memory_order_acquire);
        if (!branchTarget)
            branchTarget = slot.hook.load(std::memory_order_acquire);
        if (!entry || !branchTarget || entry[0] != 0xEB
            || entry[1] != 0xF9 || entry[-5] != 0xE9)
            return false;
        int32_t displacement = 0;
        std::memcpy(&displacement, entry - 4, sizeof(displacement));
        return entry + displacement == branchTarget
            && (!slot.branchRelay.load(std::memory_order_acquire)
                || RelayCurrent(
                    slot.branchRelay.load(std::memory_order_acquire),
                    slot.hook.load(std::memory_order_acquire)));
    }
    if (method == Method::eRelocated)
    {
        return entry && QueryExecutableImage(entry, kVerificationBytes,
                slot.owner.load(std::memory_order_acquire))
            && std::memcmp(entry, slot.relocatedBytes.data(),
                slot.relocatedBytes.size()) == 0;
    }
    return false;
}

// The bytes after the first five, before the entry is patched.
void RecordTail(Slot& slot, const uint8_t* entry) noexcept
{
    const bool readable = QueryExecutableImage(const_cast<uint8_t*>(entry), kVerificationBytes + kTailBytes,
        slot.owner.load(std::memory_order_acquire));
    if (readable)
        std::memcpy(slot.tailBytes.data(), entry + kVerificationBytes, slot.tailBytes.size());
    slot.tailRecorded.store(readable, std::memory_order_release);
}

// An entry whose patch another hook replaced by its own jump (rel32
// or short) within the first five bytes and nothing after them. That hook's
// trampoline still runs our jump, and our trampoline resumes at or past entry + 5
// (a relocated five-byte jump, or a hotpatch entry replaying the whole five-byte
// first instruction), so it forwards exactly as when the entry is current. Our
// own patch back in place after such a hook left also counts (its verification
// failure is sticky).
bool Overlaid(const Slot& slot) noexcept
{
    const Method method = static_cast<Method>(slot.method.load(std::memory_order_acquire));
    const bool resumesPastEntry = method == Method::eRelocated ? slot.relocatedBytes[0] == 0xE9
        : method == Method::eHotpatch
            && ReplayBytes(static_cast<Kind>(slot.kind.load(std::memory_order_acquire))) >= kVerificationBytes;
    if (!slot.installed.load(std::memory_order_acquire) || !resumesPastEntry
        || !slot.tailRecorded.load(std::memory_order_acquire)
        || !slot.original.load(std::memory_order_acquire))
        return false;
    const auto* entry = static_cast<const uint8_t*>(slot.target.load(std::memory_order_acquire));
    if (!entry || !QueryExecutableImage(const_cast<uint8_t*>(entry), kVerificationBytes + kTailBytes,
            slot.owner.load(std::memory_order_acquire)))
        return false;
    return (entry[0] == 0xE9 || entry[0] == 0xEB)
        && std::memcmp(entry + kVerificationBytes, slot.tailBytes.data(), slot.tailBytes.size()) == 0;
}

// Declare this before the install lock so its destructor runs after unlocking.
// FreeLibrary can run a final DLL detach under the loader lock; releasing an
// unsuccessful attempt's reference while holding gInstallMutex would invert
// the order used by synchronous entry installation from a DLL callback.
struct DeferredOwnerRelease
{
    HMODULE owner = nullptr;

    ~DeferredOwnerRelease() noexcept
    {
        if (owner)
            FreeLibrary(owner);
    }
};

bool PinOwner(Slot& slot, DeferredOwnerRelease& release) noexcept
{
    if (slot.retainedOwner.load(std::memory_order_acquire))
        return true;
    HMODULE retained = nullptr;
    void* target = slot.target.load(std::memory_order_acquire);
    HMODULE owner = slot.owner.load(std::memory_order_acquire);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(target), &retained)
        || retained != owner)
    {
        release.owner = retained;
        return false;
    }
    slot.retainedOwner.store(retained, std::memory_order_release);
    return true;
}

void ReleaseOwnerAfterFailure(Slot& slot,
    DeferredOwnerRelease& release) noexcept
{
    release.owner = slot.retainedOwner.exchange(
        nullptr, std::memory_order_acq_rel);
}

bool TryInstallHotpatchLocked(Slot& slot) noexcept
{
    auto* entry = static_cast<uint8_t*>(
        slot.target.load(std::memory_order_acquire));
    const Kind kind = static_cast<Kind>(
        slot.kind.load(std::memory_order_acquire));
    HMODULE owner = slot.owner.load(std::memory_order_acquire);
    if ((reinterpret_cast<uintptr_t>(entry) & 1u) != 0
        || !QueryExecutableImage(entry - kHotpatchPrefixBytes,
            kHotpatchPrefixBytes + kVerificationBytes, owner))
    {
        SetFailure(slot, Failure::eEntryNotHotpatchable);
        return false;
    }

    const bool chained = IsChainedPublicEntry(kind, entry);
    if (!EntryMatches(kind, entry) && !chained)
    {
        SetFailure(slot, Failure::eUnsupportedEntry);
        return false;
    }
    if (!HotpatchPaddingMatches(entry))
    {
        SetFailure(slot, Failure::eEntryNotHotpatchable);
        return false;
    }

    void* chainTarget = chained ? ResolveRelativeJumpTarget(entry) : nullptr;
    if (chained && (!chainTarget
            || chainTarget == slot.hook.load(std::memory_order_acquire)))
    {
        SetFailure(slot, Failure::eUnsupportedEntry);
        return false;
    }
    void* trampoline = AllocateHotpatchTrampoline(
        entry, ReplayBytes(kind), chainTarget);
    if (!trampoline)
    {
        SetFailure(slot, Failure::eRelayAllocationFailed);
        return false;
    }

    const uint8_t* branchEnd = entry;
    void* hook = slot.hook.load(std::memory_order_acquire);
    void* branchTarget = hook;
    const intptr_t direct = static_cast<uint8_t*>(hook) - branchEnd;
    if (direct < std::numeric_limits<int32_t>::min()
        || direct > std::numeric_limits<int32_t>::max())
    {
        branchTarget = AllocateNearRelay(branchEnd, hook);
        if (!branchTarget)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            SetFailure(slot, Failure::eRelayAllocationFailed);
            return false;
        }
        slot.branchRelay.store(branchTarget, std::memory_order_release);
    }
    const intptr_t wideDisplacement = static_cast<uint8_t*>(branchTarget)
        - branchEnd;
    const int32_t displacement = static_cast<int32_t>(wideDisplacement);
    std::array<uint8_t, kHotpatchPrefixBytes> longJump{0xE9};
    std::memcpy(longJump.data() + 1, &displacement, sizeof(displacement));

    auto* patch = entry - kHotpatchPrefixBytes;
    DWORD originalProtection = 0;
    if (!VirtualProtect(patch, kHotpatchPrefixBytes + 2,
            PAGE_EXECUTE_WRITECOPY, &originalProtection))
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        if (slot.branchRelay.exchange(nullptr, std::memory_order_acq_rel))
            VirtualFree(branchTarget, 0, MEM_RELEASE);
        SetFailure(slot, Failure::eProtectionChangeFailed);
        return false;
    }

    volatile uint8_t* materialize = patch;
    materialize[0] = materialize[0];
    std::memcpy(patch, longJump.data(), longJump.size());
    FlushInstructionCache(GetCurrentProcess(), patch, longJump.size());

    slot.original.store(trampoline, std::memory_order_release);
    slot.method.store(static_cast<uint32_t>(Method::eHotpatch),
        std::memory_order_release);
    slot.failure.store(static_cast<uint32_t>(Failure::eNone),
        std::memory_order_release);
    MemoryBarrier();
    const SHORT expected = static_cast<SHORT>(
        static_cast<uint16_t>(entry[0])
        | (static_cast<uint16_t>(entry[1]) << 8));
    const SHORT observed = _InterlockedCompareExchange16(
        reinterpret_cast<volatile SHORT*>(entry),
        static_cast<SHORT>(0xF9EBu), expected);
    FlushInstructionCache(GetCurrentProcess(), entry, 2);
    if (observed != expected)
    {
        std::array<uint8_t, kHotpatchPrefixBytes> padding{};
        padding.fill(0xCC);
        std::memcpy(patch, padding.data(), padding.size());
        FlushInstructionCache(GetCurrentProcess(), patch, padding.size());
        RestoreProtection(patch, kHotpatchPrefixBytes + 2,
            originalProtection);
        slot.original.store(nullptr, std::memory_order_release);
        slot.method.store(static_cast<uint32_t>(Method::eUnavailable),
            std::memory_order_release);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        if (slot.branchRelay.exchange(nullptr, std::memory_order_acq_rel))
            VirtualFree(branchTarget, 0, MEM_RELEASE);
        SetFailure(slot, Failure::eAtomicPublicationFailed);
        return false;
    }

    RecordTail(slot, entry);
    slot.installed.store(true, std::memory_order_release);
    if (!RestoreProtection(patch, kHotpatchPrefixBytes + 2,
            originalProtection))
    {
        SetFailure(slot, Failure::eProtectionRestoreFailed);
        return true;
    }
    if (!Current(slot))
    {
        SetFailure(slot, Failure::ePostWriteVerificationFailed);
        return true;
    }
    return true;
}

bool EnsureMinHookLocked() noexcept
{
    if (!gMinHookInitializationAttempted)
    {
        gMinHookInitializationAttempted = true;
        const MH_STATUS status = MH_Initialize();
        gMinHookReady = status == MH_OK
            || status == MH_ERROR_ALREADY_INITIALIZED;
    }
    return gMinHookReady;
}

bool TryInstallRelocatedLocked(Slot& slot) noexcept
{
    if (!EnsureMinHookLocked())
    {
        SetFailure(slot, Failure::eRelocatorInitializeFailed);
        return false;
    }
    void* target = slot.target.load(std::memory_order_acquire);
    void* hook = slot.hook.load(std::memory_order_acquire);
    void* trampoline = nullptr;
    const MH_STATUS created = MH_CreateHook(target, hook, &trampoline);
    if (created != MH_OK || !trampoline)
    {
        SetFailure(slot, Failure::eRelocatorCreateFailed);
        return false;
    }

    // Publish the per-entry trampoline before enabling the patch. A racing
    // caller can therefore always tail-forward safely.
    slot.original.store(trampoline, std::memory_order_release);
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK)
    {
        slot.original.store(nullptr, std::memory_order_release);
        MH_RemoveHook(target);
        SetFailure(slot, Failure::eRelocatorEnableFailed);
        return false;
    }
    std::memcpy(slot.relocatedBytes.data(), target,
        slot.relocatedBytes.size());
    RecordTail(slot, static_cast<const uint8_t*>(target));
    slot.method.store(static_cast<uint32_t>(Method::eRelocated),
        std::memory_order_release);
    slot.failure.store(static_cast<uint32_t>(Failure::eNone),
        std::memory_order_release);
    slot.installed.store(true, std::memory_order_release);
    if (!Current(slot))
    {
        SetFailure(slot, Failure::ePostWriteVerificationFailed);
        return true;
    }
    return true;
}

bool ValidateTarget(Slot& slot, DeferredOwnerRelease& release) noexcept
{
    HMODULE owner = slot.owner.load(std::memory_order_acquire);
    void* target = slot.target.load(std::memory_order_acquire);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(target, &memory, sizeof(memory)) != sizeof(memory)
        || memory.AllocationBase != owner
        || memory.Type != MEM_IMAGE
        || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0
        || !IsExecutableProtection(memory.Protect))
    {
        SetFailure(slot, Failure::eOwnerMismatch);
        return false;
    }
    if (!HookMitigationsSupported())
    {
        SetFailure(slot, Failure::eUnsupportedMitigation);
        return false;
    }
    if (!PinOwner(slot, release))
    {
        SetFailure(slot, Failure::eTargetPinFailed);
        return false;
    }
    return true;
}

bool InstallPreparedLocked(Slot& slot, const InstallOptions& options,
    void*& originalTrampoline, DeferredOwnerRelease& release) noexcept
{
    originalTrampoline = nullptr;
    if (slot.installed.load(std::memory_order_acquire))
    {
        const Failure failure = static_cast<Failure>(
            slot.failure.load(std::memory_order_acquire));
        if (failure != Failure::eNone || !Current(slot))
        {
            if (failure == Failure::eNone)
                SetFailure(slot, Failure::ePostWriteVerificationFailed);
            // Publication may already have exposed this trampoline to another
            // thread. Keep its owner and allocations alive, but never report
            // the entry as healthy; its hook remains transparent and the
            // coherent-route gate will fail closed.
            originalTrampoline = slot.original.load(std::memory_order_acquire);
            return false;
        }
        originalTrampoline = slot.original.load(std::memory_order_acquire);
        return true;
    }
    if (TargetAlreadyDetouredLocked(
            slot.owner.load(std::memory_order_acquire),
            slot.target.load(std::memory_order_acquire), &slot))
    {
        SetFailure(slot, Failure::eHookConflict);
        return false;
    }
    if (!ValidateTarget(slot, release))
        return false;

    const bool hotpatchInstalled = TryInstallHotpatchLocked(slot);
    if (hotpatchInstalled
        && static_cast<Failure>(slot.failure.load(std::memory_order_acquire))
            == Failure::eNone
        && Current(slot))
    {
        originalTrampoline = slot.original.load(std::memory_order_acquire);
        return true;
    }
    if (slot.installed.load(std::memory_order_acquire))
    {
        // Do not unpin or recycle an entry which may already be executing.
        // It is deliberately left as a transparent but unhealthy detour.
        if (static_cast<Failure>(slot.failure.load(std::memory_order_acquire))
            == Failure::eNone)
            SetFailure(slot, Failure::ePostWriteVerificationFailed);
        originalTrampoline = slot.original.load(std::memory_order_acquire);
        return false;
    }
    const Failure hotpatchFailure = static_cast<Failure>(
        slot.failure.load(std::memory_order_acquire));
    const bool mayRelocate = options.allowRelocated
        && (hotpatchFailure == Failure::eUnsupportedEntry
            || hotpatchFailure == Failure::eEntryNotHotpatchable);
    const bool relocatedInstalled = mayRelocate
        && TryInstallRelocatedLocked(slot);
    if (relocatedInstalled
        && static_cast<Failure>(slot.failure.load(std::memory_order_acquire))
            == Failure::eNone
        && Current(slot))
    {
        originalTrampoline = slot.original.load(std::memory_order_acquire);
        return true;
    }
    if (slot.installed.load(std::memory_order_acquire))
    {
        if (static_cast<Failure>(slot.failure.load(std::memory_order_acquire))
            == Failure::eNone)
            SetFailure(slot, Failure::ePostWriteVerificationFailed);
        originalTrampoline = slot.original.load(std::memory_order_acquire);
        return false;
    }
    ReleaseOwnerAfterFailure(slot, release);
    return false;
}

Snapshot SnapshotFor(size_t index, const Slot& slot) noexcept
{
    Snapshot snapshot{};
    if (!slot.claimed.load(std::memory_order_acquire))
        return snapshot;
    snapshot.installed = slot.installed.load(std::memory_order_acquire);
    snapshot.failure = static_cast<Failure>(
        slot.failure.load(std::memory_order_acquire));
    snapshot.method = static_cast<Method>(
        slot.method.load(std::memory_order_acquire));
    snapshot.kind = static_cast<Kind>(
        slot.kind.load(std::memory_order_acquire));
    snapshot.handle = HandleFor(index, slot);
    snapshot.owner = slot.owner.load(std::memory_order_acquire);
    snapshot.target = slot.target.load(std::memory_order_acquire);
    snapshot.original = slot.original.load(std::memory_order_acquire);
    snapshot.generation = slot.generation.load(std::memory_order_acquire);
    snapshot.current = snapshot.installed
        && snapshot.failure == Failure::eNone && Current(slot);
    snapshot.cachedPointersCovered = snapshot.current;
    snapshot.forwarding = snapshot.current && slot.forwardRelay.load(std::memory_order_acquire) != nullptr;
    // Only an install that completed: verification failing later is the other hook.
    snapshot.overlaid = !snapshot.current && snapshot.installed
        && (snapshot.failure == Failure::eNone || snapshot.failure == Failure::ePostWriteVerificationFailed)
        && Overlaid(slot);
    if (snapshot.owner && snapshot.target)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(snapshot.owner);
        const uintptr_t target = reinterpret_cast<uintptr_t>(snapshot.target);
        if (target >= base && target - base <= UINT32_MAX)
            snapshot.targetRva = static_cast<uint32_t>(target - base);
    }
    return snapshot;
}
}

bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline, const InstallOptions& options,
    Handle* installedHandle) noexcept
{
    originalTrampoline = nullptr;
    if (installedHandle)
        *installedHandle = {};
    if (!IsKindValid(kind) || !owner || !target || !hook)
        return false;

    DeferredOwnerRelease release;
    std::unique_lock lock(gInstallMutex, std::defer_lock);
    if (!options.nonBlocking)
        lock.lock();
    else if (!lock.try_lock())
        return false;
    InstallOptions effective = options;
    if (options.nonBlocking)
        effective.allowRelocated = false;
    size_t index = 0;
    Slot* slot = ReserveLocked(kind, owner, target, options.generation, index);
    if (!slot)
        return false;
    if (installedHandle)
        *installedHandle = HandleFor(index, *slot);

    void* existingHook = slot->hook.load(std::memory_order_acquire);
    if (existingHook && existingHook != hook)
    {
        SetFailure(*slot, Failure::eHookConflict);
        return false;
    }
    slot->hook.store(hook, std::memory_order_release);
    return InstallPreparedLocked(*slot, effective, originalTrampoline, release);
}

bool Install(Kind kind, HMODULE owner, void* target, void* hook,
    void*& originalTrampoline) noexcept
{
    return Install(kind, owner, target, hook, originalTrampoline,
        InstallOptions{}, nullptr);
}

bool InstallForwarding(Kind kind, HMODULE owner, void* target,
    ForwardPreCall preCall, void*& originalTrampoline,
    const InstallOptions& options, Handle* installedHandle,
    ForwardGate gate, void* rejectedTarget) noexcept
{
    originalTrampoline = nullptr;
    if (installedHandle)
        *installedHandle = {};
    if (!IsKindValid(kind) || !owner || !target || (!preCall && !gate)
        || (gate != nullptr) != (rejectedTarget != nullptr)
        || (gate && options.filterForwardArg2))
        return false;

    DeferredOwnerRelease release;
    std::lock_guard lock(gInstallMutex);
    size_t index = 0;
    Slot* slot = ReserveLocked(kind, owner, target, options.generation, index);
    if (!slot)
        return false;
    if (installedHandle)
        *installedHandle = HandleFor(index, *slot);

    ForwardPreCall existing = slot->preCall.load(std::memory_order_acquire);
    if (slot->installed.load(std::memory_order_acquire)
        && (slot->forwardGate.load(std::memory_order_acquire) != gate
            || slot->rejectedForwardTarget.load(std::memory_order_acquire) != rejectedTarget))
    {
        SetFailure(*slot, Failure::eHookConflict);
        return false;
    }
    if (existing && existing != preCall)
    {
        SetFailure(*slot, Failure::eHookConflict);
        return false;
    }
    if ((existing || slot->installed.load(std::memory_order_acquire))
        && (slot->filterForwardArg2.load(std::memory_order_acquire)
                != options.filterForwardArg2
            || slot->requiredForwardArg2.load(std::memory_order_acquire)
                != options.requiredForwardArg2))
    {
        SetFailure(*slot, Failure::eHookConflict);
        return false;
    }
    slot->preCall.store(preCall, std::memory_order_release);
    slot->rejectedForwardTarget.store(rejectedTarget, std::memory_order_release);
    slot->forwardGate.store(gate, std::memory_order_release);
    slot->requiredForwardArg2.store(options.requiredForwardArg2,
        std::memory_order_release);
    slot->filterForwardArg2.store(options.filterForwardArg2,
        std::memory_order_release);
    void* relay = slot->forwardRelay.load(std::memory_order_acquire);
    if (!relay)
    {
        relay = AllocateForwardRelay(slot);
        if (!relay)
        {
            SetFailure(*slot, Failure::eForwardRelayFailed);
            return false;
        }
        slot->forwardRelay.store(relay, std::memory_order_release);
    }
    void* existingHook = slot->hook.load(std::memory_order_acquire);
    if (existingHook && existingHook != relay)
    {
        SetFailure(*slot, Failure::eHookConflict);
        return false;
    }
    slot->hook.store(relay, std::memory_order_release);
    const bool installed = InstallPreparedLocked(
        *slot, options, originalTrampoline, release);
    if (!installed && !slot->installed.load(std::memory_order_acquire))
    {
        void* stale = slot->forwardRelay.exchange(
            nullptr, std::memory_order_acq_rel);
        slot->hook.store(nullptr, std::memory_order_release);
        if (stale)
            VirtualFree(stale, 0, MEM_RELEASE);
    }
    return installed;
}

Snapshot ReadSnapshot(Handle handle) noexcept
{
    if (!handle || handle.slot >= gSlots.size())
    {
        Snapshot invalid{};
        invalid.failure = Failure::eInvalidArgument;
        return invalid;
    }
    const Slot& slot = gSlots[handle.slot];
    if (slot.serial.load(std::memory_order_acquire) != handle.serial)
    {
        Snapshot invalid{};
        invalid.failure = Failure::eInvalidArgument;
        return invalid;
    }
    return SnapshotFor(handle.slot, slot);
}

Snapshot ReadSnapshot(Kind kind) noexcept
{
    Snapshot selected{};
    if (!IsKindValid(kind))
    {
        selected.failure = Failure::eInvalidArgument;
        return selected;
    }
    uint32_t matching = 0;
    uint32_t current = 0;
    for (size_t index = 0; index < gSlots.size(); ++index)
    {
        const Slot& slot = gSlots[index];
        if (!slot.claimed.load(std::memory_order_acquire)
            || slot.kind.load(std::memory_order_acquire)
                != static_cast<uint32_t>(kind))
            continue;
        ++matching;
        Snapshot candidate = SnapshotFor(index, slot);
        if (candidate.current)
            ++current;
        if (!selected.handle
            || (candidate.current && !selected.current)
            || (candidate.current == selected.current
                && candidate.handle.serial > selected.handle.serial))
            selected = candidate;
    }
    selected.kind = kind;
    selected.matchingEntries = matching;
    selected.currentEntries = current;
    return selected;
}

Snapshot ReadSnapshot(Kind kind, HMODULE owner) noexcept
{
    Snapshot selected{};
    if (!IsKindValid(kind) || !owner)
    {
        selected.failure = Failure::eInvalidArgument;
        return selected;
    }
    uint32_t matching = 0;
    uint32_t current = 0;
    for (size_t index = 0; index < gSlots.size(); ++index)
    {
        const Slot& slot = gSlots[index];
        if (!slot.claimed.load(std::memory_order_acquire)
            || slot.kind.load(std::memory_order_acquire)
                != static_cast<uint32_t>(kind)
            || slot.owner.load(std::memory_order_acquire) != owner)
            continue;
        ++matching;
        Snapshot candidate = SnapshotFor(index, slot);
        if (candidate.current)
            ++current;
        if (!selected.handle
            || (candidate.current && !selected.current)
            || (candidate.current == selected.current
                && candidate.handle.serial > selected.handle.serial))
            selected = candidate;
    }
    selected.kind = kind;
    selected.matchingEntries = matching;
    selected.currentEntries = current;
    return selected;
}

size_t RegistryCapacity() noexcept
{
    return gSlots.size();
}

const char* MethodName(Method method) noexcept
{
    switch (method)
    {
    case Method::eHotpatch: return "hotpatch";
    case Method::eRelocated: return "relocated";
    default: return "unavailable";
    }
}

const char* FailureName(Failure failure) noexcept
{
    switch (failure)
    {
    case Failure::eNone: return "none";
    case Failure::eInvalidArgument: return "invalid-argument";
    case Failure::eRegistryFull: return "registry-full";
    case Failure::eUnsupportedMitigation: return "unsupported-mitigation";
    case Failure::eOwnerMismatch: return "owner-mismatch";
    case Failure::eUnsupportedEntry: return "unsupported-entry";
    case Failure::eEntryNotHotpatchable: return "entry-not-hotpatchable";
    case Failure::eTargetPinFailed: return "target-pin-failed";
    case Failure::eRelayAllocationFailed: return "relay-allocation-failed";
    case Failure::eProtectionChangeFailed: return "protection-change-failed";
    case Failure::eAtomicPublicationFailed: return "atomic-publication-failed";
    case Failure::eProtectionRestoreFailed: return "protection-restore-failed";
    case Failure::ePostWriteVerificationFailed: return "post-write-verification-failed";
    case Failure::eHookConflict: return "hook-conflict";
    case Failure::eRelocatorInitializeFailed: return "relocator-initialize-failed";
    case Failure::eRelocatorCreateFailed: return "relocator-create-failed";
    case Failure::eRelocatorEnableFailed: return "relocator-enable-failed";
    case Failure::eForwardRelayFailed: return "forward-relay-failed";
    default: return "unknown";
    }
}

extern "C" void* WINAPI MfgUnlockDispatchForwarding(
    void* arg1, uintptr_t arg2, const void* arg3, void* arg4,
    uintptr_t arg5, uintptr_t arg6, Slot* slot,
    const void* originalCaller) noexcept
{
    if (!slot)
        return nullptr;
    void* const original = slot->original.load(std::memory_order_acquire);
    if (slot->filterForwardArg2.load(std::memory_order_acquire)
        && arg2 != slot->requiredForwardArg2.load(std::memory_order_acquire))
        return original;
    ForwardPreCall callback = slot->preCall.load(std::memory_order_acquire);
    const size_t index = static_cast<size_t>(slot - gSlots.data());
    if (ForwardGate gate = slot->forwardGate.load(std::memory_order_acquire))
    {
        if (!gate(arg1, arg2, arg3, arg4, arg5, arg6, HandleFor(index, *slot), originalCaller))
            return slot->rejectedForwardTarget.load(std::memory_order_acquire);
    }
    if (callback)
    {
        callback(arg1, arg2, arg3, arg4, arg5, arg6,
            HandleFor(index, *slot), originalCaller);
    }
    return original;
}
}
