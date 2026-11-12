// winemenu.1 (GitHub #131): InsideLoader() under Wine/Proton, where ntdll has no
// RtlIsThreadWithinLoaderCallout. The callout answers when it exists; otherwise the
// loader lock's owner does, and with neither the check fails closed.
#include "overlay_native.h"
#include <iostream>
#include <thread>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }

BOOLEAN NTAPI CalloutTrue() { return TRUE; }
BOOLEAN NTAPI CalloutFalse() { return FALSE; }

int wmain()
{
    using single_overlay::native::InsideLoader;
    using single_overlay::native::LoaderLock;
    CRITICAL_SECTION lock{};
    InitializeCriticalSection(&lock);

    Check(InsideLoader(CalloutTrue, &lock) && !InsideLoader(CalloutFalse, &lock),
        "the callout answers when ntdll exports it");
    EnterCriticalSection(&lock);
    Check(!InsideLoader(CalloutFalse, &lock), "with the callout, a held lock is not consulted");
    Check(InsideLoader(nullptr, &lock), "without the callout, a lock this thread holds means inside the loader");
    bool other = true;
    std::thread([&] { other = InsideLoader(nullptr, &lock); }).join();
    Check(!other, "a lock another thread holds does not");
    LeaveCriticalSection(&lock);
    Check(!InsideLoader(nullptr, &lock), "an unheld lock means outside the loader");
    Check(InsideLoader(nullptr, nullptr), "without the callout and the lock, the check fails closed");
    DeleteCriticalSection(&lock);

    // The real process: PEB+0x110 is the loader's critical section on Windows (and Wine).
    const RTL_CRITICAL_SECTION* loader = LoaderLock();
    Check(loader != nullptr, "the PEB names a loader lock");
    Check(!InsideLoader() && !InsideLoader(nullptr, loader), "wmain runs outside the loader either way");
    if (loader) {
        auto* held = const_cast<RTL_CRITICAL_SECTION*>(loader);
        EnterCriticalSection(held);
        const bool inside = InsideLoader(nullptr, loader);
        LeaveCriticalSection(held);
        Check(inside, "holding the real loader lock reads as inside the loader without the callout");
    }

    std::cout << (failures ? "LOADER_LOCK_TESTS_FAIL " : "LOADER_LOCK_TESTS_PASS ") << checks - failures << '/' << checks << '\n';
    return failures ? 1 : 0;
}
