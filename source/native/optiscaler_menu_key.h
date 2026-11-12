#pragma once
#include <Windows.h>
#include <cerrno>
#include <cwchar>
#include <string_view>

// OptiScaler shared menu: while RTXMFG's menu offers the DLSS presets
// and DLSS 5 beside OptiScaler's menu, it opens and closes on OptiScaler's menu
// key, so one press shows or hides both menus.
namespace optiscaler_menu_key
{
// OptiScaler's default when [Menu] ShortcutKey is absent, "auto" or unreadable.
inline constexpr int kDefaultKey = VK_INSERT;

// OptiScaler's Config::readInt: the whole value as "0x" hex or decimal. A value
// outside 1-255 gives OptiScaler no menu key (0 here); anything unreadable is
// its default.
inline int ParseShortcutKey(std::wstring_view text) noexcept
{
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.remove_suffix(1);
    if (text.empty() || text.size() >= 32) return kDefaultKey;
    wchar_t value[32]{};
    text.copy(value, text.size());
    if (_wcsicmp(value, L"auto") == 0) return kDefaultKey;
    const bool hex = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
    wchar_t* end = nullptr;
    errno = 0;
    const long key = std::wcstol(value, &end, hex ? 16 : 10);
    if (errno == ERANGE || end != value + text.size()) return kDefaultKey;
    return key > 0 && key < 256 ? static_cast<int>(key) : 0;
}

// Follows OptiScaler's menu across presses of its key. OptiScaler toggles its
// menu on the key's release, and while its menu is open its input layer
// answers GetAsyncKeyState for the held key as up to every caller. A build
// that never does is told apart by never doing so; its menu is then assumed
// to toggle with this one.
class Follower
{
    bool blockingSeen_ = false;
public:
    // reportedDown: GetAsyncKeyState through OptiScaler's hooks while the key
    // is physically down. Returns this menu's state after OptiScaler's toggle.
    bool AfterPress(bool reportedDown, bool visible) noexcept
    {
        if (!reportedDown)
        {
            blockingSeen_ = true;
            return false; // OptiScaler's menu was open and closes.
        }
        return blockingSeen_ ? true : !visible;
    }
    bool BlockingSeen() const noexcept { return blockingSeen_; }
};
}
