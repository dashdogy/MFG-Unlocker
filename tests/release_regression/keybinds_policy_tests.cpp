// keybinds.1: the Keybinds tab's saved form and the mode each frame generation key selects (keybinds_policy.h).
#include "keybinds_policy.h"
#include <cstdio>
using namespace keybinds;

int main()
{
    unsigned checks = 0, failures = 0;
    auto check = [&](bool passed, const char* reason) {
        ++checks;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", reason);
        if (!passed) ++failures;
    };
    auto same = [](Binding binding, int key, int modifiers) {
        return binding.key == key && binding.modifiers == modifiers;
    };

    // Saved form: RTX40MFG-UI.ini [Keybinds].
    check(same(Parse(L"Ctrl+Shift+0x21"), VK_PRIOR, kCtrl | kShift), "Ctrl+Shift+0x21 reads as Ctrl+Shift+Page Up");
    check(same(Parse(L"alt+117"), VK_F6, kAlt), "modifier names ignore case and the code may be decimal");
    check(same(Parse(L"0x05"), VK_XBUTTON1, 0), "mouse 4 binds without modifiers");
    check(same(Parse(L" Control + 0x70 "), VK_F1, kCtrl), "Control and spaces around the parts are accepted");
    check(!Parse(L"").key && !Parse(L"0").key && !Parse(nullptr).key, "empty, 0 and absent bind nothing");
    check(!Parse(L"Ctrl+").key && !Parse(L"Win+0x21").key && !Parse(L"0x21x").key && !Parse(L"0x1FF").key,
        "a missing key, an unknown modifier, trailing text or a code above 0xFF bind nothing");
    check(!Parse(L"0x1B").key && !Parse(L"0x10").key && !Parse(L"0xA2").key && !Parse(L"0x5B").key
        && !Parse(L"0x01").key && !Parse(L"0x02").key && !Parse(L"0x04").key,
        "Esc, the modifiers, the Windows keys and the left, right and middle buttons are not bindable");
    check(Format({VK_PRIOR, kCtrl | kAlt}) == L"Ctrl+Alt+0x21" && Format({}) == L"",
        "a binding writes back as modifiers and its code; unbound writes nothing");
    bool roundTrip = true;
    for (int key = 0; key < 256; ++key)
        for (int modifiers = 0; modifiers < 8; ++modifiers)
            if (Bindable(key))
            {
                const Binding binding{static_cast<uint8_t>(key), static_cast<uint8_t>(modifiers)};
                roundTrip &= Parse(Format(binding).c_str()) == binding;
            }
    check(roundTrip, "every bindable key and modifier set reads back as written");
    check(std::wstring(kIniNames[static_cast<size_t>(Action::ePinPanel)]) == L"PinPanel"
        && std::wstring(kIniNames[static_cast<size_t>(Action::e6x)]) == L"Mode6x",
        "the [Keybinds] names follow the actions");

    // RTX 40/50 DLSS-G: Follow game, 2x-4x (safe maximum 4), Dynamic supported, no Off.
    const Offered ada{true, true, true, 2, 4};
    const Mode follow{true, false, 3}, dynamic{false, true, 3}, fixed2{false, false, 2}, fixed4{false, false, 4};
    auto selects = [](Result result, Outcome outcome, Mode mode) {
        return result.outcome == outcome && result.mode == mode;
    };
    check(selects(Select(Action::eRaise, fixed2, ada), Outcome::eChanged, {false, false, 3}), "Raise steps 2x to 3x");
    check(selects(Select(Action::eRaise, fixed4, ada), Outcome::eHighest, fixed4), "Raise stops at the safe maximum");
    check(selects(Select(Action::eLower, fixed2, ada), Outcome::eLowest, fixed2), "Lower stops at 2x and never turns FG Off");
    check(selects(Select(Action::eLower, fixed4, ada), Outcome::eChanged, {false, false, 3}), "Lower steps 4x to 3x");
    check(selects(Select(Action::eRaise, follow, ada), Outcome::eChanged, fixed4),
        "Raise from Follow game leaves it one step above the saved multiplier");
    check(selects(Select(Action::eLower, dynamic, ada), Outcome::eChanged, fixed2),
        "Lower from Dynamic leaves it one step below the saved multiplier");
    check(selects(Select(Action::eRaise, {false, false, 6}, ada), Outcome::eHighest, {false, false, 6}),
        "a saved mode above the maximum does not go higher");
    check(selects(Select(Action::eLower, {false, false, 6}, ada), Outcome::eChanged, fixed4),
        "Lower from above the maximum lands on the maximum");
    check(selects(Select(Action::eDynamic, fixed2, ada), Outcome::eChanged, {false, true, 2}), "Dynamic switches on");
    check(selects(Select(Action::eDynamic, dynamic, ada), Outcome::eChanged, {false, false, 3}),
        "Dynamic switches back to the saved fixed mode");
    check(selects(Select(Action::eDynamic, {false, true, 6}, ada), Outcome::eChanged, fixed4),
        "Dynamic off clamps the saved mode to the maximum");
    check(selects(Select(Action::eFollowGame, fixed2, ada), Outcome::eChanged, {true, false, 2}), "Follow game selects");
    check(selects(Select(Action::eFollowGame, follow, ada), Outcome::eUnchanged, follow), "Follow game again changes nothing");
    check(Select(Action::eOff, fixed2, ada).outcome == Outcome::eUnavailable, "no Off without the RTX 30/20 Off entry");
    check(selects(Select(Action::e4x, fixed2, ada), Outcome::eChanged, fixed4), "4x selects 4x");
    check(selects(Select(Action::e4x, fixed4, ada), Outcome::eUnchanged, fixed4), "4x again changes nothing");
    check(selects(Select(Action::e5x, fixed2, ada), Outcome::eHighest, fixed2), "5x above the maximum is refused");
    check(selects(Select(Action::e2x, follow, ada), Outcome::eChanged, fixed2), "2x leaves Follow game");
    Offered noDynamic = ada;
    noDynamic.dynamic = false;
    check(selects(Select(Action::eDynamic, fixed2, noDynamic), Outcome::eUnavailable, fixed2),
        "Dynamic is refused while unsupported");
    check(selects(Select(Action::eDynamic, dynamic, noDynamic), Outcome::eChanged, {false, false, 3}),
        "a saved Dynamic can still be switched off while unsupported");
    const Offered closed{};
    bool none = true;
    for (size_t i = 0; i <= static_cast<size_t>(Action::e6x); ++i)
        none &= Select(static_cast<Action>(i), fixed2, closed).outcome == Outcome::eUnavailable;
    check(none, "every frame generation key is refused while the Mode list is unavailable");
    check(Select(Action::eFpsReadout, fixed2, ada).outcome == Outcome::eUnavailable
        && Select(Action::ePinPanel, fixed2, ada).outcome == Outcome::eUnavailable,
        "display keys select no frame generation mode");

    // RTX 30/20 DLSS-G: Off at the bottom of the list.
    const Offered ampere{true, true, true, 1, 6};
    const Mode off{false, false, 1};
    check(selects(Select(Action::eOff, fixed2, ampere), Outcome::eChanged, off), "Off selects Off on RTX 30/20");
    check(selects(Select(Action::eOff, off, ampere), Outcome::eUnchanged, off), "Off again changes nothing");
    check(selects(Select(Action::eRaise, off, ampere), Outcome::eChanged, fixed2), "Raise from Off starts at 2x");
    check(selects(Select(Action::eLower, off, ampere), Outcome::eLowest, off), "Lower from Off stays Off");
    check(selects(Select(Action::eDynamic, off, ampere), Outcome::eChanged, {false, true, 2}),
        "Dynamic from Off keeps at least 2x, as the menu does");
    check(selects(Select(Action::e6x, off, ampere), Outcome::eChanged, {false, false, 6}), "6x selects 6x at a 6x maximum");

    // XeMFG standalone: 2x-6x and Dynamic (up to the frames set), no Follow game or Off.
    const Offered xemfg{true, false, true, 2, 6};
    check(Select(Action::eFollowGame, fixed2, xemfg).outcome == Outcome::eUnavailable
        && Select(Action::eOff, fixed2, xemfg).outcome == Outcome::eUnavailable, "XeMFG has no Follow game or Off");
    check(selects(Select(Action::eRaise, {false, true, 4}, xemfg), Outcome::eChanged, {false, false, 5}),
        "Raise from XeMFG Dynamic up to 4x selects 5x");
    check(selects(Select(Action::eDynamic, {false, false, 5}, xemfg), Outcome::eChanged, {false, true, 5}),
        "XeMFG Dynamic keeps the frames as its most");

    // keybinds.3: the DLSS preset keys over the menu's lists, Game / driver default first (SR J-M, RR D-F).
    const uint32_t sr[]{0, 10, 11, 12, 13}, rr[]{0, 4, 5, 6};
    check(StepPreset(Action::ePresetNext, sr, 5, 0) == 10 && StepPreset(Action::ePresetNext, sr, 5, 13) == 0,
        "Next steps from Game / driver to J and wraps after M");
    check(StepPreset(Action::ePresetPrevious, sr, 5, 0) == 13 && StepPreset(Action::ePresetPrevious, sr, 5, 11) == 10,
        "Previous wraps from Game / driver to M and steps K to J");
    check(StepPreset(Action::ePresetNext, rr, 4, 6) == 0 && StepPreset(Action::ePresetPrevious, rr, 4, 4) == 0
        && StepPreset(Action::ePresetNext, rr, 4, 4) == 5, "Ray Reconstruction's list steps and wraps over D, E, F");
    check(StepPreset(Action::ePresetDefault, rr, 4, 5) == 0 && StepPreset(Action::ePresetDefault, sr, 5, 0) == 0,
        "Game / driver default selects the list's first entry");
    check(StepPreset(Action::ePresetNext, sr, 5, 7) == 10, "a preset the list does not hold counts as Game / driver");
    check(StepPreset(Action::eRaise, sr, 5, 11) == 11 && StepPreset(Action::ePresetNext, nullptr, 0, 11) == 11,
        "other actions and an empty list change nothing");
    check(PresetAction(Action::ePresetNext) && PresetAction(Action::ePresetDefault) && !PresetAction(Action::ePinPanel)
        && !ModeAction(Action::ePresetNext) && Select(Action::ePresetNext, fixed2, ada).outcome == Outcome::eUnavailable,
        "preset keys select no frame generation mode");
    {
        bool shared = true;
        for (size_t i = 0; i < kActionCount; ++i)
        {
            const auto action = static_cast<Action>(i);
            shared = shared && SharedAction(action) == (PresetAction(action) || action == Action::ePinPanel);
        }
        check(shared && !SharedAction(Action::eRaise) && !SharedAction(Action::e6x) && !SharedAction(Action::eFpsReadout),
            "beside OptiScaler's menu only the preset keys and Pin panel apply");
    }
    {
        // dx11menu.1: a DirectX 11 game has no frame generation, FPS readout or DLSS presets to act on.
        unsigned acting = 0;
        for (size_t i = 0; i < kActionCount; ++i) acting += D3D11Action(static_cast<Action>(i)) ? 1u : 0u;
        check(acting == 1 && D3D11Action(Action::ePinPanel) && !D3D11Action(Action::eFpsReadout)
            && !D3D11Action(Action::ePresetNext) && !D3D11Action(Action::eRaise) && !D3D11Action(Action::eOff),
            "in a DirectX 11 game only Pin panel applies");
    }
    check(kActionCount == 15 && std::wstring(kIniNames[static_cast<size_t>(Action::ePresetNext)]) == L"DlssPresetNext"
        && std::wstring(kIniNames[static_cast<size_t>(Action::ePresetDefault)]) == L"DlssPresetDefault"
        && Parse(L"Ctrl+0x76") == Binding{VK_F7, kCtrl}, "the preset keys' [Keybinds] names follow the actions");

    std::printf("COMPLETE Keybinds policy checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
