#pragma once
#include <Windows.h>
#include <atomic>
#include <string>
#include "ui_host.h"

// GitHub issue 122: the runtime log can be turned off from the menu's Diagnostics section, saved as
// [Diagnostics] Logging=0 in RTX40MFG-UI.ini beside the game executable. It is on by default: a missing file or key,
// 1, true, on, and any value that is not 0, false or off keep it on. The patch worker reads it once, before it opens
// the log, so a change applies at the next game start. Off, the worker neither rotates nor opens the log; the
// session diagnostics' crash dumps, written beside the log, stop with it. The status file is not a log and stays.
namespace runtime_log_setting
{
constexpr wchar_t kSection[] = L"Diagnostics";
constexpr wchar_t kKey[] = L"Logging";

enum class Setting { eOff, eOn, eInvalid };

inline std::wstring Path() { return standalone_ui::SettingsPath("RTX40MFG-UI.ini"); }

inline Setting ReadSetting(const wchar_t* iniPath) noexcept
{
    return standalone_ui::ReadOnOff(kSection, kKey, iniPath, Setting::eOn);
}

// Only an explicit off turns the log off.
inline bool SavedOn() noexcept
{
    try { return ReadSetting(Path().c_str()) != Setting::eOff; }
    catch (...) { return true; }
}

inline bool Save(bool on) noexcept
{
    try
    {
        const std::wstring path = Path();
        return !path.empty() && WritePrivateProfileStringW(kSection, kKey, on ? L"1" : L"0", path.c_str())
            && ReadSetting(path.c_str()) == (on ? Setting::eOn : Setting::eOff);
    }
    catch (...) { return false; }
}

// This session's choice, set once by the patch worker before it opens the log: -1 not yet decided, 0 off, 1 on.
inline std::atomic<int> gSession{-1};
inline void SetSession(bool on) noexcept { gSession.store(on ? 1 : 0, std::memory_order_release); }
inline int Session() noexcept { return gSession.load(std::memory_order_acquire); }
}
