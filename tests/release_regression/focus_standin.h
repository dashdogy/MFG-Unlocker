#pragma once
// bgfocus.1: the menu cases without the desktop's focus.
//
// Windows gives a new window the foreground only under conditions that a run with nobody at the desk does not meet, and
// a case that really takes the foreground takes it from whoever is working. Most menu cases only need Windows to say
// that their window is in front: their input arrives through window messages. MFG_HARNESS_FOCUS, set per process by
// run.py, selects how a case gets its focus:
//   (unset), real   the real foreground.
//   claim           the real foreground, after a harmless key so that this process sent the last input.
//   standin         the calls below are answered here, inside this process only; nothing reaches the desktop.
//   elsewhere       as standin, but another process always holds the foreground: the negative control, which must fail
//                   as a genuinely unfocused run does.
//
// The candidate DLL is the exact shipped file. Patching the exports, rather than offering it a switch, means it and every
// other module in the process (OptiScaler, Streamline) run their ordinary calls and see the same answer.
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace harness::focus
{
enum class Mode { eReal, eClaim, eStandIn, eElsewhere };

inline Mode CurrentMode() noexcept
{
    static const Mode mode = [] {
        wchar_t value[16]{};
        const DWORD length = GetEnvironmentVariableW(L"MFG_HARNESS_FOCUS", value, 16);
        if (length && length < 16)
        {
            if (!wcscmp(value, L"claim")) return Mode::eClaim;
            if (!wcscmp(value, L"standin")) return Mode::eStandIn;
            if (!wcscmp(value, L"elsewhere")) return Mode::eElsewhere;
        }
        return Mode::eReal;
    }();
    return mode;
}

// Whether this process answers the calls itself.
inline bool Virtual() noexcept
{
    const Mode mode = CurrentMode();
    return mode == Mode::eStandIn || mode == Mode::eElsewhere;
}

namespace detail
{
// A cursor position no window is at: the real cursor is usually outside the test window too, and the user's mouse must
// not change what a case draws.
inline constexpr LONG kNoCursor = -20000;

inline std::atomic<HWND> gForeground{nullptr};
inline std::atomic<bool> gKeyDown[256]{};
inline std::atomic<LONG> gCursorX{kNoCursor};
inline std::atomic<LONG> gCursorY{kNoCursor};
inline SRWLOCK gClipLock = SRWLOCK_INIT;
inline bool gClipped = false;
inline RECT gClip{};

using ForegroundFn = HWND (WINAPI*)();
inline ForegroundFn gRealForeground = nullptr;   // win32u's system call, left unpatched
inline std::atomic<bool> gTheftReported{false};

// The promise of a background case is that it leaves the desktop alone. If a window of this process really is in front
// the promise was broken (or the user clicked the window), and the case says so. Worded so that tests.ps1's "owns the
// foreground" retry does not take it for the ordinary unfocused failure.
inline void ReportTheft() noexcept
{
    if (!gRealForeground) return;
    const HWND real = gRealForeground();
    DWORD process = 0;
    if (!real || !GetWindowThreadProcessId(real, &process) || process != GetCurrentProcessId()) return;
    if (gTheftReported.exchange(true, std::memory_order_relaxed)) return;
    wchar_t name[64]{};
    GetClassNameW(real, name, 64);
    std::printf("NOTE focus stand-in: the real foreground is window class %ls\n", name);
    std::printf("FAIL focus stand-in: a window of this process took the real desktop focus\n");
    std::fflush(stdout);
}

inline HWND WINAPI GetForegroundWindowStandIn()
{
    ReportTheft();
    if (CurrentMode() == Mode::eElsewhere) return GetShellWindow();
    const HWND window = gForeground.load(std::memory_order_acquire);
    return window ? window : GetShellWindow();   // before a case asks for it, as on a real desktop: someone else's window
}

inline BOOL WINAPI SetForegroundWindowStandIn(HWND window)
{
    if (CurrentMode() == Mode::eElsewhere) return FALSE;
    gForeground.store(window, std::memory_order_release);
    return TRUE;
}

inline BOOL WINAPI BringWindowToTopStandIn(HWND window) { return window != nullptr; }

// The real SetFocus activates the window's top-level parent, and Windows makes that the real foreground whenever this
// process is allowed to take it. This delivers the messages it would send.
inline std::atomic<HWND> gFocus{nullptr};

inline HWND WINAPI SetFocusStandIn(HWND window)
{
    const HWND previous = gFocus.exchange(window, std::memory_order_acq_rel);
    if (previous == window) return previous;
    if (previous && IsWindow(previous)) SendMessageW(previous, WM_KILLFOCUS, reinterpret_cast<WPARAM>(window), 0);
    if (window) SendMessageW(window, WM_SETFOCUS, reinterpret_cast<WPARAM>(previous), 0);
    return previous;
}

inline HWND WINAPI GetFocusStandIn() { return gFocus.load(std::memory_order_acquire); }

// Merging this queue with the real foreground thread's is how a background window could disturb the desktop.
inline BOOL WINAPI AttachThreadInputStandIn(DWORD, DWORD, BOOL) { return FALSE; }

inline SHORT WINAPI GetAsyncKeyStateStandIn(int key)
{
    return key >= 0 && key < 256 && gKeyDown[key].load(std::memory_order_acquire) ? static_cast<SHORT>(0x8000) : SHORT{0};
}

inline void SetKey(WORD key, bool down) noexcept
{
    gKeyDown[key & 0xFF].store(down, std::memory_order_release);
    // A generic modifier also reports its left key, as the real key state does.
    const int side = key == VK_CONTROL ? VK_LCONTROL : key == VK_SHIFT ? VK_LSHIFT : key == VK_MENU ? VK_LMENU : 0;
    if (side) gKeyDown[side].store(down, std::memory_order_release);
}

// Keyboard events reach the key state and, as Windows would route them, the window in front (this harness never injects
// Alt or F10, so WM_SYSKEY* is not needed). A stand-in case moves no mouse: asking to fails the call.
inline UINT WINAPI SendInputStandIn(UINT count, LPINPUT inputs, int size)
{
    if (size != static_cast<int>(sizeof(INPUT)) || !inputs) return 0;
    for (UINT index = 0; index < count; ++index)
    {
        if (inputs[index].type != INPUT_KEYBOARD) return index;
        const KEYBDINPUT& key = inputs[index].ki;
        const bool down = !(key.dwFlags & KEYEVENTF_KEYUP);
        SetKey(key.wVk, down);
        const HWND window = gForeground.load(std::memory_order_acquire);
        if (!window) continue;
        const LPARAM parameter = static_cast<LPARAM>(static_cast<ULONG>(1u | (static_cast<ULONG>(key.wScan & 0xFF) << 16)
            | (key.dwFlags & KEYEVENTF_EXTENDEDKEY ? 1ul << 24 : 0ul) | (down ? 0ul : 3ul << 30)));
        PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, key.wVk, parameter);
    }
    return count;
}

inline BOOL WINAPI GetCursorPosStandIn(LPPOINT point)
{
    if (!point) return FALSE;
    point->x = gCursorX.load(std::memory_order_acquire);
    point->y = gCursorY.load(std::memory_order_acquire);
    return TRUE;
}

inline BOOL WINAPI SetCursorPosStandIn(int x, int y)
{
    gCursorX.store(x, std::memory_order_release);
    gCursorY.store(y, std::memory_order_release);
    return TRUE;
}

inline BOOL WINAPI GetCursorInfoStandIn(PCURSORINFO info)
{
    if (!info || info->cbSize != sizeof(CURSORINFO)) return FALSE;
    info->flags = CURSOR_SHOWING;
    info->hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW, wide whatever UNICODE is
    return GetCursorPosStandIn(&info->ptScreenPos);
}

inline BOOL WINAPI ClipCursorStandIn(const RECT* rect)
{
    AcquireSRWLockExclusive(&gClipLock);
    gClipped = rect != nullptr;
    if (rect) gClip = *rect;
    ReleaseSRWLockExclusive(&gClipLock);
    return TRUE;
}

inline BOOL WINAPI GetClipCursorStandIn(LPRECT rect)
{
    if (!rect) return FALSE;
    AcquireSRWLockShared(&gClipLock);
    const bool clipped = gClipped;
    const RECT saved = gClip;
    ReleaseSRWLockShared(&gClipLock);
    if (clipped) { *rect = saved; return TRUE; }
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *rect = RECT{left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return TRUE;
}

// mov rax, imm64 / jmp rax: twelve bytes, position independent, and RAX is free at a function's entry. Exports are 16-byte
// aligned, so these bytes stay inside the entry's own slot even when the function is a six-byte jump; a target that is not
// aligned is refused rather than risking its neighbour.
inline bool Patch(const wchar_t* moduleName, const char* name, const void* replacement) noexcept
{
    const HMODULE module = GetModuleHandleW(moduleName);
    const auto target = module ? reinterpret_cast<unsigned char*>(GetProcAddress(module, name)) : nullptr;
    if (!target || (reinterpret_cast<uintptr_t>(target) & 15)) return false;
    unsigned char code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(code + 2, &replacement, sizeof(replacement));
    DWORD protection = 0;
    if (!VirtualProtect(target, sizeof(code), PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(target, code, sizeof(code));
    DWORD ignored = 0;
    const bool restored = VirtualProtect(target, sizeof(code), protection, &ignored) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(code));
    return restored && !std::memcmp(target, code, sizeof(code));
}
}

// First thing in wmain, before any candidate loads. A case must never run half virtual, so a patch that cannot be applied
// ends the process.
inline void Install() noexcept
{
    if (!Virtual()) return;
    using namespace detail;
    const struct { const wchar_t* module; const char* name; const void* replacement; } patches[] = {
        {L"user32.dll", "GetForegroundWindow", reinterpret_cast<const void*>(&GetForegroundWindowStandIn)},
        {L"user32.dll", "SetForegroundWindow", reinterpret_cast<const void*>(&SetForegroundWindowStandIn)},
        {L"user32.dll", "BringWindowToTop", reinterpret_cast<const void*>(&BringWindowToTopStandIn)},
        {L"user32.dll", "SetFocus", reinterpret_cast<const void*>(&SetFocusStandIn)},
        {L"user32.dll", "GetFocus", reinterpret_cast<const void*>(&GetFocusStandIn)},
        {L"user32.dll", "AttachThreadInput", reinterpret_cast<const void*>(&AttachThreadInputStandIn)},
        {L"user32.dll", "SendInput", reinterpret_cast<const void*>(&SendInputStandIn)},
        // user32's own GetAsyncKeyState answers from a per-thread cache that only real input refreshes, so both are needed.
        {L"user32.dll", "GetAsyncKeyState", reinterpret_cast<const void*>(&GetAsyncKeyStateStandIn)},
        {L"win32u.dll", "NtUserGetAsyncKeyState", reinterpret_cast<const void*>(&GetAsyncKeyStateStandIn)},
        {L"user32.dll", "GetCursorPos", reinterpret_cast<const void*>(&GetCursorPosStandIn)},
        {L"user32.dll", "SetCursorPos", reinterpret_cast<const void*>(&SetCursorPosStandIn)},
        {L"user32.dll", "GetCursorInfo", reinterpret_cast<const void*>(&GetCursorInfoStandIn)},
        {L"user32.dll", "ClipCursor", reinterpret_cast<const void*>(&ClipCursorStandIn)},
        {L"user32.dll", "GetClipCursor", reinterpret_cast<const void*>(&GetClipCursorStandIn)},
    };
    const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
    gRealForeground = win32u ? reinterpret_cast<ForegroundFn>(GetProcAddress(win32u, "NtUserGetForegroundWindow")) : nullptr;
    if (!gRealForeground)
    {
        std::printf("FAIL focus stand-in found win32u NtUserGetForegroundWindow\n");
        std::fflush(stdout);
        ExitProcess(3);
    }
    for (const auto& patch : patches)
    {
        if (Patch(patch.module, patch.name, patch.replacement)) continue;
        std::printf("FAIL focus stand-in patched %s\n", patch.name);
        std::fflush(stdout);
        ExitProcess(3);
    }
    std::printf("NOTE focus=%s patched=%zu\n", CurrentMode() == Mode::eElsewhere ? "elsewhere" : "standin", std::size(patches));
    std::fflush(stdout);
}

// claim only, just before a case asks for the real foreground: F24 is bound to nothing, and sending it makes this process
// the source of the last input.
inline void PrepareClaim() noexcept
{
    if (CurrentMode() != Mode::eClaim) return;
    INPUT inputs[2]{};
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = VK_F24;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}
}
