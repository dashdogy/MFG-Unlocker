#pragma once
#include "protected_pointer.h"
#include "overlay_build.h"
#include "caller_scoped_import.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace single_overlay::slots
{
inline bool Fault(const char* point) noexcept
{
#if MFG_UNLOCK_OVERLAY_TEST_FAULTS
    char value[64]{};
    const auto n = GetEnvironmentVariableA("RTXMFG_SLOT_FAULT", value, sizeof(value));
    return n && n < sizeof(value) && !strcmp(value, point);
#else
    return false;
#endif
}

// Aligned application IAT publication. Four-byte-aligned system input/factory imports,
// and aligned ones whose declared data page became executable, can use
// caller-scoped public-entry detours; their IAT and native COM tables remain
// untouched. All other unsuitable slots continue to fail closed.
inline bool ImageSlot(HMODULE image, void** slot) noexcept
{
    MEMORY_BASIC_INFORMATION m{};
    const auto address = reinterpret_cast<uintptr_t>(slot);
    return image && slot && !(address % alignof(void*))
        && VirtualQuery(slot, &m, sizeof(m)) == sizeof(m)
        && m.State == MEM_COMMIT && m.Type == MEM_IMAGE && m.AllocationBase == image
        && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_EXECUTE
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        && address >= reinterpret_cast<uintptr_t>(m.BaseAddress)
        && m.RegionSize >= sizeof(void*)
        && address - reinterpret_cast<uintptr_t>(m.BaseAddress) <= m.RegionSize - sizeof(void*);
}

inline bool ImageEntry(HMODULE image, void* entry) noexcept
{
    MEMORY_BASIC_INFORMATION m{};
    return image && entry && VirtualQuery(entry, &m, sizeof(m)) == sizeof(m)
        && m.Type == MEM_IMAGE && m.State == MEM_COMMIT && m.AllocationBase == image
        && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

inline bool ReShadeFactoryExports(HMODULE image) noexcept
{
    for (const char* name : {"CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
                            "ReShadeRegisterAddon", "ReShadeUnregisterAddon"})
        if (!ImageEntry(image, reinterpret_cast<void*>(GetProcAddress(image, name)))) return false;
    return true;
}

// Windows may report WRITECOPY for an image page requested as READWRITE (and
// READWRITE for its already-private copy). Both are writable data protections;
// restoring the precise original protection is still mandatory.
inline bool Exchange(void** slot, void* expected, void* replacement, DWORD restore) noexcept
{
    auto writable = [&]() {
        DWORD ignored = 0, observed = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &ignored)
            || !protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(slot), observed, sizeof(void*))) return false;
        return observed == PAGE_READWRITE || observed == PAGE_WRITECOPY;
    };
    auto restored = [&]() {
        return protected_pointer::RestoreProtectionWithRetry(slot, sizeof(void*), restore, &VirtualProtect);
    };
    if (!writable()) { restored(); return false; }
    void* observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), replacement, expected);
    if (observed != expected) { restored(); return false; }
    if (restored() && protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot)) == replacement) return true;
    // Only reverse our own publication, and only after reestablishing writable
    // protection. Forwarders have process lifetime even on ambiguous recovery.
    if (writable()) InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), expected, replacement);
    restored();
    return false;
}

struct Batch
{
    using PublishOriginal = bool (*)(const char*, void*, void*) noexcept;
    struct Item { void** slot; void* expected; void* replacement; DWORD protection; HMODULE owner; const char* symbol; bool scoped; PublishOriginal publishOriginal; };
    std::array<Item, 64> items{};
    size_t count = 0;
    size_t published = 0;
    bool rollbackVerified = true;
    size_t failureIndex = 0;
    unsigned disposition = 0;

    bool Add(HMODULE owner, void** slot, void* expected, void* replacement, const char* symbol = nullptr, PublishOriginal publishOriginal = nullptr) noexcept
    {
        const bool aligned = ImageSlot(owner, slot);
        const bool scoped = !aligned && publishOriginal && caller_scoped_import::Eligible(owner, slot, expected, symbol);
        if (!expected || !replacement || (!aligned && !scoped) || count == items.size()
            || protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot)) != expected) return false;
        for (size_t i = 0; i < count; ++i)
            if (items[i].slot == slot) return items[i].expected == expected && items[i].replacement == replacement;
        DWORD protection = 0;
        if (!protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(slot), protection, sizeof(void*))) return false;
        items[count++] = {slot, expected, replacement, protection, owner, symbol, scoped, publishOriginal};
        return true;
    }

    bool Rollback(size_t attempted) noexcept
    {
        bool ok = true;
        while (attempted)
        {
            const auto& item = items[--attempted];
            if (item.scoped) {
                ok = caller_scoped_import::Deactivate(item.expected,item.replacement) && ok;
                ok = protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(item.slot)) == item.expected && ok;
                continue;
            }
            auto observed = protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(item.slot));
            if (observed == item.replacement)
                Exchange(item.slot, item.replacement, item.expected, item.protection);
            // Never overwrite a third party's newer publication.
            ok = protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(item.slot)) == item.expected
                && protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(item.slot), item.protection, sizeof(void*)) && ok;
        }
        rollbackVerified = ok;
        return ok;
    }

    bool Publish() noexcept
    {
        for (size_t i = 0; i < count; ++i)
        {
            failureIndex = i;
            const auto& item = items[i];
            char fault[32]{};
            sprintf_s(fault, "publish-%zu", i);
            if (Fault(fault)) { Rollback(i); return false; }
            void* trampoline = nullptr;
            const bool result = item.scoped
                ? caller_scoped_import::Prepare(item.owner,item.slot,item.expected,item.replacement,item.symbol,trampoline)
                    && item.publishOriginal(item.symbol,item.expected,trampoline)
                    && caller_scoped_import::Activate(item.expected,item.replacement)
                : Exchange(item.slot, item.expected, item.replacement, item.protection);
            disposition = result ? 1 : 0;
            if (!result)
            { Rollback(i + 1); return false; }
            ++published;
        }
        return true;
    }
};

// Bounded inspection of the PE import directory. No delay imports, private
// wrapper layouts, foreign code writes, or module loading inside this scan.
template<class Visitor>
bool VisitImports(HMODULE image, Visitor visit) noexcept
{
    if (!image) return false;
    const auto base = reinterpret_cast<uint8_t*>(image);
    const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const size_t size = nt->OptionalHeader.SizeOfImage;
    auto valid = [&](size_t rva, size_t bytes) { return rva < size && bytes <= size - rva; };
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;
    if (!valid(dir.VirtualAddress, dir.Size) || dir.Size > 1024 * sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    const auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (size_t i = 0; i < dir.Size / sizeof(*imports); ++i)
    {
        const auto& imp = imports[i];
        if (!imp.Name) return true;
        if (!imp.OriginalFirstThunk || !imp.FirstThunk) continue;
        for (size_t j = 0; j < 4096; ++j)
        {
            const size_t nameRva = size_t(imp.OriginalFirstThunk) + j * sizeof(IMAGE_THUNK_DATA64);
            const size_t slotRva = size_t(imp.FirstThunk) + j * sizeof(void*);
            if (!valid(nameRva, sizeof(IMAGE_THUNK_DATA64)) || !valid(slotRva, sizeof(void*))) return false;
            const auto value = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + nameRva)->u1.AddressOfData;
            if (!value) break;
            if (IMAGE_SNAP_BY_ORDINAL64(value)) continue;
            if (!valid(static_cast<size_t>(value), 3)) return false;
            const auto name = reinterpret_cast<const char*>(base + value + 2);
            // C++ decorated imports (for example Cyberpunk's PhysX queries)
            // legally exceed 127 bytes. Keep the scan bounded without rejecting
            // those unrelated imports before reaching the graphics/input slots.
            if (!memchr(name, 0, (std::min)(size_t(4096), size - static_cast<size_t>(value) - 2))) return false;
            if (!visit(name, reinterpret_cast<void**>(base + slotRva))) return false;
        }
    }
    return false;
}
}
