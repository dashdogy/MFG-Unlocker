#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

// keybinds.1: the standalone menu's Keybinds tab. The bindable actions, their saved form in RTX40MFG-UI.ini
// [Keybinds], and the mode a frame generation key selects from the modes the menu's Mode list offers. Selection only:
// the menu's own write applies it (ApplySettings, NativeFilePersistXemfg), so the backend's checks stay the gate.
namespace keybinds
{
enum class Action : uint8_t
{
    eRaise, eLower, eDynamic, eFollowGame, eOff, e2x, e3x, e4x, e5x, e6x, eFpsReadout, ePinPanel,
    // keybinds.3: the preset of the DLSS the game runs (Super Resolution's, or Ray Reconstruction's while it runs).
    ePresetNext, ePresetPrevious, ePresetDefault, eCount
};
inline constexpr size_t kActionCount = static_cast<size_t>(Action::eCount);

// Each action's [Keybinds] name, in the enum's order.
inline constexpr const wchar_t* kIniNames[kActionCount]{L"RaiseMultiplier", L"LowerMultiplier", L"Dynamic",
    L"FollowGame", L"Off", L"Mode2x", L"Mode3x", L"Mode4x", L"Mode5x", L"Mode6x", L"FpsReadout", L"PinPanel",
    L"DlssPresetNext", L"DlssPresetPrevious", L"DlssPresetDefault"};

inline constexpr bool ModeAction(Action action) noexcept { return action <= Action::e6x; }
inline constexpr bool PresetAction(Action action) noexcept
{
    return action >= Action::ePresetNext && action <= Action::ePresetDefault;
}
// menunotes.1: beside OptiScaler's menu (shared) its frame generation and FPS overlay are OptiScaler's, so only the
// DLSS preset keys and Pin panel apply.
inline constexpr bool SharedAction(Action action) noexcept
{
    return PresetAction(action) || action == Action::ePinPanel;
}
// dx11menu.1: in a DirectX 11 game only Pin panel acts; frame generation, its FPS readout and the DLSS presets act
// through DirectX 12 or Vulkan.
inline constexpr bool D3D11Action(Action action) noexcept
{
    return action == Action::ePinPanel;
}
// The fixed mode of 2x-6x, else 0.
inline constexpr int FixedMultiplier(Action action) noexcept
{
    return action >= Action::e2x && action <= Action::e6x ? static_cast<int>(action) - static_cast<int>(Action::e2x) + 2 : 0;
}

enum Modifier : uint8_t { kCtrl = 1, kShift = 2, kAlt = 4 };

struct Binding
{
    uint8_t key = 0;       // virtual-key code, 0 = not set
    uint8_t modifiers = 0; // Modifier bits, held exactly
    bool operator==(const Binding&) const = default;
};

// Keys an action takes: keyboard keys and mouse 4/5. Not the left, right or middle button, Esc (it cancels a
// binding), the modifiers themselves, the Windows keys or VK_PACKET.
inline constexpr bool Bindable(int key) noexcept
{
    return (key == VK_XBUTTON1 || key == VK_XBUTTON2 || (key >= VK_BACK && key < 0xFF))
        && key != VK_ESCAPE && key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU
        && !(key >= VK_LSHIFT && key <= VK_RMENU) && key != VK_LWIN && key != VK_RWIN && key != VK_PACKET;
}

// "Ctrl+Shift+0x21", "Alt+33", "0x75"; empty, "0" or anything unreadable binds nothing. Modifier names ignore case.
inline Binding Parse(const wchar_t* text) noexcept
{
    Binding binding{};
    if (!text) return {};
    uint8_t modifiers = 0;
    const wchar_t* part = text;
    for (;;)
    {
        while (*part == L' ') ++part;
        const wchar_t* plus = std::wcschr(part, L'+');
        if (!plus) break;
        size_t length = static_cast<size_t>(plus - part);
        while (length && part[length - 1] == L' ') --length;
        auto named = [&](const wchar_t* name) {
            return std::wcslen(name) == length && _wcsnicmp(part, name, length) == 0;
        };
        if (named(L"Ctrl") || named(L"Control")) modifiers |= kCtrl;
        else if (named(L"Shift")) modifiers |= kShift;
        else if (named(L"Alt")) modifiers |= kAlt;
        else return {};
        part = plus + 1;
    }
    wchar_t* end = nullptr;
    const unsigned long key = std::wcstoul(part, &end, 0);
    while (end && *end == L' ') ++end;
    if (!end || end == part || *end || key > 0xFF || !Bindable(static_cast<int>(key))) return {};
    binding.key = static_cast<uint8_t>(key);
    binding.modifiers = modifiers;
    return binding;
}

inline std::wstring Format(Binding binding)
{
    if (!binding.key) return {};
    std::wstring text;
    if (binding.modifiers & kCtrl) text += L"Ctrl+";
    if (binding.modifiers & kShift) text += L"Shift+";
    if (binding.modifiers & kAlt) text += L"Alt+";
    wchar_t code[8]{};
    swprintf_s(code, L"0x%02X", binding.key);
    return text + code;
}

// The modes a frame generation key may select: the Mode list's enabled entries. lowest is 1 where the list has Off
// (RTX 30/20 DLSS-G), else 2; highest is the safe maximum. any = false while the list is unavailable.
struct Offered
{
    bool any = false, followGame = false, dynamic = false;
    int lowest = 2, highest = 2;
};
// The saved selection: Follow game, Dynamic (multiplier = the most frames for XeMFG), or a fixed multiplier (1 = Off).
struct Mode
{
    bool followGame = false, dynamic = false;
    int multiplier = 2;
    bool operator==(const Mode&) const = default;
};
enum class Outcome { eChanged, eUnchanged, eHighest, eLowest, eUnavailable };
struct Result
{
    Outcome outcome = Outcome::eUnavailable;
    Mode mode{};
};

// Raise and Lower step one fixed mode from the saved multiplier, between 2x and the highest offered (Lower never
// turns frame generation Off); from Follow game or Dynamic they leave it for that step. Dynamic toggles Dynamic and
// the saved fixed mode; Follow game, Off and 2x-6x select that mode.
inline Result Select(Action action, Mode current, const Offered& offered) noexcept
{
    if (!ModeAction(action) || !offered.any) return {Outcome::eUnavailable, current};
    const int highest = offered.highest < 2 ? 2 : offered.highest > 6 ? 6 : offered.highest;
    const bool fixed = !current.followGame && !current.dynamic;
    const int saved = current.multiplier < 1 ? 1 : current.multiplier > 6 ? 6 : current.multiplier;
    auto fixedAt = [](int multiplier) { return Mode{false, false, multiplier}; };
    switch (action)
    {
    case Action::eRaise:
    case Action::eLower:
    {
        const bool raise = action == Action::eRaise;
        const int target = saved + (raise ? 1 : -1);
        if (fixed && raise && target > highest) return {Outcome::eHighest, current};
        if (fixed && !raise && target < 2) return {Outcome::eLowest, current};
        return {Outcome::eChanged, fixedAt(target < 2 ? 2 : target > highest ? highest : target)};
    }
    case Action::eDynamic:
        if (current.dynamic && !current.followGame)
            return {Outcome::eChanged, fixedAt(saved < 2 ? 2 : saved > highest ? highest : saved)};
        if (!offered.dynamic) return {Outcome::eUnavailable, current};
        return {Outcome::eChanged, Mode{false, true, saved < 2 ? 2 : saved}};
    case Action::eFollowGame:
        if (!offered.followGame) return {Outcome::eUnavailable, current};
        if (current.followGame) return {Outcome::eUnchanged, current};
        return {Outcome::eChanged, Mode{true, false, saved}};
    case Action::eOff:
        if (offered.lowest > 1) return {Outcome::eUnavailable, current};
        if (fixed && saved == 1) return {Outcome::eUnchanged, current};
        return {Outcome::eChanged, fixedAt(1)};
    default:
    {
        const int multiplier = FixedMultiplier(action);
        if (multiplier > highest) return {Outcome::eHighest, current};
        if (fixed && saved == multiplier) return {Outcome::eUnchanged, current};
        return {Outcome::eChanged, fixedAt(multiplier)};
    }
    }
}

// keybinds.3: the preset a DLSS preset key selects from the menu's list for the feature (values[0] is Game / driver
// default): Next and Previous step one entry and wrap around; Default selects values[0]. A current value the list
// does not hold counts as values[0]. Returns current for any other action or an empty list.
inline uint32_t StepPreset(Action action, const uint32_t* values, size_t count, uint32_t current) noexcept
{
    if (!PresetAction(action) || !values || !count) return current;
    if (action == Action::ePresetDefault) return values[0];
    size_t index = 0;
    for (size_t i = 0; i != count; ++i)
        if (values[i] == current) index = i;
    return values[action == Action::ePresetNext ? (index + 1) % count : (index + count - 1) % count];
}
}
