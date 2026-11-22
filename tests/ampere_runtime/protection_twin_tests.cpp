// cowtwin.1 (GitHub issues 170 and 172, Wine/Proton): a writable protection requested on an image page may read
// back as its copy-on-write twin (READWRITE as WRITECOPY, EXECUTE_READWRITE as EXECUTE_WRITECOPY, and the reverse).
// Wine always answers the copy-on-write form, which made every publication through protected_pointer fail there:
// the DLSS 5 provider's imports, the RTX 30 declaration byte and the resolver import hooks. The publication must
// succeed with either read-back, restore the original protection exactly, and still fail when nothing changed.
#include "protected_pointer.h"
#include <iostream>

unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

// Image pages of this executable that nothing writes before the test: a read-only data section with two pointer
// slots, and a read-execute section with patchable bytes.
#pragma section(".mfgro", read)
#pragma section(".mfgrx", read, execute)
// Each array is the only item of its section, so it starts on a page boundary.
__declspec(allocate(".mfgro")) const uintptr_t kSlots[2] = {0x1111, 0x2222};
__declspec(allocate(".mfgrx")) const unsigned char kCode[16] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

// Reports success and changes nothing: verification by read-back must not trust the return value.
BOOL WINAPI ProtectNothing(LPVOID, SIZE_T, DWORD, PDWORD previous)
{
    if (previous) *previous = 0;
    return TRUE;
}

// The address as a run-time value, so no read of the constant data above is folded at compile time.
uintptr_t RunTime(const void* address)
{
    volatile uintptr_t value = reinterpret_cast<uintptr_t>(address);
    return value;
}

DWORD ProtectionOf(uintptr_t address)
{
    DWORD protection = 0;
    return protected_pointer::QueryProtection(address, protection) ? protection : 0;
}

int wmain()
{
    using namespace protected_pointer;

    Check(CopyOnWriteTwin(PAGE_READWRITE) == PAGE_WRITECOPY && CopyOnWriteTwin(PAGE_WRITECOPY) == PAGE_READWRITE
        && CopyOnWriteTwin(PAGE_EXECUTE_READWRITE) == PAGE_EXECUTE_WRITECOPY
        && CopyOnWriteTwin(PAGE_EXECUTE_WRITECOPY) == PAGE_EXECUTE_READWRITE,
        "each writable protection has its copy-on-write twin");
    Check(CopyOnWriteTwin(PAGE_READONLY) == 0 && CopyOnWriteTwin(PAGE_EXECUTE_READ) == 0
        && CopyOnWriteTwin(PAGE_EXECUTE) == 0 && CopyOnWriteTwin(PAGE_NOACCESS) == 0,
        "protections that are not writable have no twin, so a restore is verified exactly");
    Check(CopyOnWriteTwin(PAGE_READWRITE | PAGE_GUARD) == PAGE_WRITECOPY, "modifier bits do not hide the twin");

    // A pointer slot in a never-written read-only image page: the import-table case.
    const uintptr_t slot = RunTime(&kSlots[0]);
    const uintptr_t other = RunTime(&kSlots[1]);
    Check(ImageMemory(slot) && ProtectionOf(slot) == PAGE_READONLY, "the slot is read-only image memory");
    {
        DWORD previous = 0;
        const bool changed = VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &previous) != FALSE;
        const DWORD observed = ProtectionOf(slot);
        std::cout << "NOTE READWRITE requested on a shared image page reads back as 0x" << std::hex << observed
                  << std::dec << (observed == PAGE_WRITECOPY ? " (the copy-on-write twin)" : "") << '\n';
        Check(changed && (observed == PAGE_READWRITE || observed == PAGE_WRITECOPY),
            "a writable request on the image page reads back as itself or its twin");
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), previous, &ignored);
        Check(ProtectionOf(slot) == PAGE_READONLY, "the probe restored the page");
    }
    {
        const PublishResult refused = ReplaceProtectedPointer(slot, 0x1111, 0x3333, &ProtectNothing);
        Check(refused.disposition == PublishDisposition::eNotPublishedRestored && !refused.replacementWasPublished
            && ReadPointer(slot) == reinterpret_cast<void*>(uintptr_t{0x1111}) && ProtectionOf(slot) == PAGE_READONLY,
            "a protection call that changes nothing publishes nothing");
    }
    {
        const PublishResult published = ReplaceProtectedPointer(slot, 0x1111, 0x3333);
        Check(published.disposition == PublishDisposition::ePublishedRestored && published.replacementWasPublished,
            "the pointer is published whichever writable form the page reads back as");
        Check(ReadPointer(slot) == reinterpret_cast<void*>(uintptr_t{0x3333})
            && ReadPointer(other) == reinterpret_cast<void*>(uintptr_t{0x2222}),
            "only the requested slot changed");
        Check(ProtectionMatches(slot, PAGE_READONLY, sizeof(void*)), "the slot's page is read-only again, exactly");
    }
    {
        // The same call with the copy-on-write request some callers pick for a still-shared page.
        const PublishResult published = ReplaceProtectedPointer(other, 0x2222, 0x4444, &VirtualProtect, PAGE_WRITECOPY);
        Check(published.disposition == PublishDisposition::ePublishedRestored
            && ReadPointer(other) == reinterpret_cast<void*>(uintptr_t{0x4444})
            && ProtectionMatches(other, PAGE_READONLY, sizeof(void*)),
            "a WRITECOPY request on a page that is already private is published too");
    }

    // Bytes in a never-written read-execute image page: the provider patch case.
    const uintptr_t code = RunTime(&kCode[0]);
    const unsigned char expected[2] = {0x90, 0x90};
    const unsigned char replacement[2] = {0xCC, 0xCC};
    Check(ImageMemory(code) && ProtectionOf(code) == PAGE_EXECUTE_READ, "the code bytes are read-execute image memory");
    {
        const PublishResult refused = ReplaceProtectedBytes(code, expected, replacement, sizeof(expected),
            PAGE_EXECUTE_READ, &ProtectNothing);
        Check(refused.disposition == PublishDisposition::eNotPublishedRestored && !refused.replacementWasPublished
            && BytesEqual(code, expected, sizeof(expected)),
            "a protection call that changes nothing patches nothing");
    }
    {
        const PublishResult published = ReplaceProtectedBytes(code, expected, replacement, sizeof(expected));
        Check(published.disposition == PublishDisposition::ePublishedRestored && published.replacementWasPublished
            && BytesEqual(code, replacement, sizeof(replacement)),
            "the bytes are patched whichever writable form the page reads back as");
        Check(ProtectionMatches(code, PAGE_EXECUTE_READ, sizeof(expected)), "the code page is read-execute again, exactly");
    }

    // Private memory has no copy-on-write state: there the read-back stays exact.
    auto* heap = static_cast<uintptr_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (heap)
    {
        heap[0] = 0x5555;
        DWORD ignored = 0;
        VirtualProtect(heap, 4096, PAGE_READONLY, &ignored);
        const uintptr_t location = RunTime(heap);
        Check(!ImageMemory(location), "allocated memory is not image memory");
        const PublishResult refused = ReplaceProtectedPointer(location, 0x5555, 0x6666, &ProtectNothing);
        Check(refused.disposition == PublishDisposition::eNotPublishedRestored
            && ReadPointer(location) == reinterpret_cast<void*>(uintptr_t{0x5555}),
            "private memory: a protection call that changes nothing publishes nothing");
        const PublishResult published = ReplaceProtectedPointer(location, 0x5555, 0x6666);
        Check(published.disposition == PublishDisposition::ePublishedRestored
            && ReadPointer(location) == reinterpret_cast<void*>(uintptr_t{0x6666})
            && ProtectionMatches(location, PAGE_READONLY, sizeof(void*)),
            "private memory: an exact read-back still publishes");
        VirtualFree(heap, 0, MEM_RELEASE);
    }
    else Check(false, "test page allocated");

    std::cout << "COMPLETE protection twin checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
