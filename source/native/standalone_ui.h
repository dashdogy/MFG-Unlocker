#pragma once
#include <Windows.h>
#include "keybinds_policy.h"

struct OptiPluginApi;

namespace standalone_ui
{
void Initialize(HMODULE self);
// d3d11: the menu draws on a DirectX 11 swapchain (dx11menu.1) and offers only what acts there.
void Draw(bool d3d11);
// Right-aligned status for the panel footer (Live / Paused); call after the footer controls.
void DrawFooterStatus();
// Pinned FPS readout: whether it is on (the toggle beside the menu's FPS row), and
// its real/DLSS frame rate text for the top-left corner while the menu is closed.
bool PinnedFps();
void DrawPinnedFps();
// latmon.2: the pinned latency readout (the toggle beside the menu's Latency row) and its text.
bool PinnedLatency();
void DrawPinnedLatency();
// menutab.2: RTXMFG's tab in OptiScaler's own menu (optiscaler_menu_tab.h). OptiScaler calls it on its menu thread
// and draws it through its plugin API's widget functions, never this module's ImGui.
void DrawOptiScalerTab(const OptiPluginApi& ui) noexcept;

// keybinds.1: the Keybinds tab's keys, under gUiMutex. PollKeybinds reads the bound keys once per frame while the
// game is in front and returns the action whose key went down (eCount: none); a binding equal to the menu key (no
// modifiers) stays the menu's. QueueKeybind hands an action to the next Draw, where RunQueuedKeybind applies it;
// KeybindFrameWanted keeps frames coming for it and its note, which KeybindNote returns (nullptr: none).
keybinds::Action PollKeybinds(int menuKey);
void QueueKeybind(keybinds::Action action);
bool KeybindFrameWanted();
void RunQueuedKeybind();
const char* KeybindNote();
// A key being bound in the Keybinds tab: the overlay leaves the menu key alone meanwhile. Cancelled when the menu
// closes or the footer's Change key starts.
bool KeybindCapturing();
void CancelKeybindCapture();
// dlss5presets.1: a text field of the menu had focus in the last frame drawn (ImGui's WantTextInput). The menu key,
// Backspace by default, then edits the field instead of closing the menu.
bool TextInputActive();
}
