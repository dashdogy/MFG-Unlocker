#include "overlay_platform.h"
#include "overlay_hook.h"
#include "optiscaler_host.h"
#include "optiscaler_menu_key.h"
#include "physical_key.h"
#include "dlss_sr.h"
#include "dlss5.h"
#include "overlay_scaling.h"
#include "standalone_ui.h"
#include "single_hotkey.h"
#include "ui_host.h"
#include "localization.h"
#include <backends/imgui_impl_win32.h>
#include <imgui_internal.h>
#include <Windowsx.h>
#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace single_overlay
{
namespace
{
struct InputMessage
{
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    ImGuiMouseSource mouseSource = ImGuiMouseSource_Mouse;
    bool fromHook = false; // lparam = mouse_route cursor (physical client x, y, width, height)
};
struct WindowInput
{
    WNDPROC original = nullptr;
    std::deque<InputMessage> messages;
    uint32_t clients = 0;
    uint32_t mouseButtons = 0;
    bool ownsCapture = false;
};
std::mutex gInputMutex;
std::unordered_map<HWND, WindowInput> gWindows;
std::atomic<HWND> gActiveWindow{nullptr};
MenuHotkey gMenuKey;
// keybinds.1: standalone_ui::KeybindCapturing() for the window procedure, set each frame under gUiMutex.
std::atomic<bool> gKeybindCapture{false};
// dlss5presets.1: standalone_ui::TextInputActive() for the window procedure, set each frame with gKeybindCapture.
std::atomic<bool> gTextInput{false};
bool gShortcutSaveFailed = false; // Render-thread state, protected by gUiMutex.
bool gFirstLaunchChecked = false; // Shared across swapchain/context recreation.
bool gFirstLaunchPending = false;
bool gFirstLaunchSavePending = false;
std::atomic<bool> gWantMouse{false};
std::atomic<bool> gWantKeyboard{false};
RECT gPreviousClip{};
bool gHavePreviousClip = false;
std::mutex gClipMutex;
void DetachWindow(HWND window, bool releaseClient = true);

// Beside OptiScaler's menu; defined with the input code below.
namespace shared_menu
{
bool Active() noexcept;
bool Ready() noexcept;
int Key() noexcept;
}

// "MFG Unlocker <version>###RTXMFG": the window ID ignores the visible text, so the
// saved position and size survive updates and the RTXMFG -> MFG Unlocker rename.
// Beside OptiScaler's menu the panel keeps a position of its own.
const char* PanelTitle()
{
    static const std::string text = []
    {
        wchar_t version[96]{};
        std::string text = "MFG Unlocker";
        if (optiscaler_host::ReadVersionString(single_module::Self(), L"ProductVersion", version, std::size(version))
            && version[0])
        {
            char utf8[192]{};
            if (WideCharToMultiByte(CP_UTF8, 0, version, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr) > 1)
                text.append(" ").append(utf8);
        }
        return text;
    }();
    static const std::string own = text + "###RTXMFG";
    static const std::string shared = text + "###RTXMFG-OptiScaler";
    return (shared_menu::Active() ? shared : own).c_str();
}
// Older builds saved the panel as "DLSS MFG".
constexpr const char* kLegacyPanelName = "DLSS MFG";

// Menu language: one process-wide choice in RTX40MFG-UI.ini [Overlay] Language
// (auto, en or a catalog code). Render-thread state, protected by gUiMutex.
bool gLanguageLoaded = false;
bool gLanguageSaveFailed = false;

void LoadLanguage()
{
    if (gLanguageLoaded) return;
    gLanguageLoaded = true;
    const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    wchar_t saved[64]{};
    if (!preferences.empty())
        GetPrivateProfileStringW(L"Overlay", L"Language", L"", saved, static_cast<DWORD>(std::size(saved)), preferences.c_str());
    char setting[64]{};
    WideCharToMultiByte(CP_UTF8, 0, saved, -1, setting, static_cast<int>(sizeof(setting)), nullptr, nullptr);
    l10n::Select(setting);
}

void SelectLanguage(const char* setting)
{
    l10n::Select(setting);
    const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    gLanguageSaveFailed = preferences.empty() || !WritePrivateProfileStringW(L"Overlay", L"Language",
        standalone_ui::Wide(l10n::Selection().c_str()).c_str(), preferences.c_str());
}

// True when the current font draws every character of a UTF-8 name.
bool Drawable(const char* text)
{
    ImFont* font = ImGui::GetFont();
    for (const char* at = text; *at;)
    {
        unsigned int codepoint = 0;
        const int length = ImTextCharFromUtf8(&codepoint, at, nullptr);
        if (length <= 0) return false;
        at += length;
        if (codepoint > 0x7F && (codepoint > 0xFFFF || !font->IsGlyphInFont(static_cast<ImWchar>(codepoint))))
            return false;
    }
    return true;
}

// A catalog's own name when the current font can draw it, else its English name.
const char* LanguageName(const l10n::Catalog* catalog)
{
    if (!catalog) return "English";
    return Drawable(catalog->nativeName) ? catalog->nativeName : catalog->englishName;
}

void DrawLanguagePicker()
{
    const auto& selection = l10n::Selection();
    char automatic[96]{};
    snprintf(automatic, sizeof(automatic), l10n::T("Auto (%s)"), LanguageName(l10n::AutoCatalog()));
    float width = std::max(ImGui::CalcTextSize(automatic).x, ImGui::CalcTextSize("English").x);
    for (size_t i = 0; i < l10n::CatalogCount(); ++i)
        width = std::max(width, ImGui::CalcTextSize(LanguageName(&l10n::CatalogAt(i))).x);
    ImGui::SetNextItemWidth(width + ImGui::GetFrameHeight() + 2.0f*ImGui::GetStyle().FramePadding.x);
    const bool open = ImGui::BeginCombo("##menulanguage",
        selection == l10n::kAuto ? automatic : LanguageName(l10n::Selected()));
    ImGui::SetItemTooltip("%s", l10n::T("Menu language"));
    if (!open) return;
    if (ImGui::Selectable(automatic, selection == l10n::kAuto)) SelectLanguage(l10n::kAuto);
    if (ImGui::Selectable("English", selection == l10n::kEnglish)) SelectLanguage(l10n::kEnglish);
    for (size_t i = 0; i < l10n::CatalogCount(); ++i)
    {
        const auto& catalog = l10n::CatalogAt(i);
        ImGui::PushID(catalog.code);
        if (ImGui::Selectable(LanguageName(&catalog), selection == catalog.code)) SelectLanguage(catalog.code);
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

bool Foreground(HWND window) noexcept
{
    const HWND foreground = GetForegroundWindow();
    return window && foreground && (foreground == window
        || GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT));
}

bool CaptureInput() noexcept
{
    return !gInsideOverlay && single_module::OverlayAllowed() && gVisible.load(std::memory_order_acquire)
        && shared_menu::Ready() && Foreground(gActiveWindow.load(std::memory_order_acquire));
}

void SetVisible(bool visible, HWND window)
{
    InternalScope internal;
    // Serialize admission with retirement. A queued window callback must not
    // reopen a renderer that has already relinquished its input client.
    std::unique_lock inputLock(gInputMutex, std::defer_lock);
    if (visible)
    {
        inputLock.lock();
        const auto found = gWindows.find(window);
        if (found == gWindows.end() || !found->second.clients) return;
    }
    const bool previous = gVisible.exchange(visible, std::memory_order_acq_rel);
    if (visible) gPinned.store(false, std::memory_order_release);
    if (!visible) gMenuKey.CancelBinding();
    gWantMouse.store(visible, std::memory_order_release);
    gWantKeyboard.store(visible, std::memory_order_release);
    if (visible && !previous)
    {
        gActiveWindow.store(window, std::memory_order_release);
        std::lock_guard clipLock(gClipMutex);
        gHavePreviousClip = GetClipCursor(&gPreviousClip) != FALSE;
        if (Foreground(window)) ClipCursor(nullptr);
    }
    else if (!visible && previous)
    {
        std::lock_guard clipLock(gClipMutex);
        if (gHavePreviousClip && Foreground(window)) ClipCursor(&gPreviousClip);
        gHavePreviousClip = false;
    }
}

bool MouseMessage(UINT message) noexcept
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        || message == WM_INPUT || message == WM_SETCURSOR;
}
bool KeyboardMessage(UINT message) noexcept
{
    return message == WM_KEYDOWN || message == WM_KEYUP
        || message == WM_CHAR || message == WM_UNICHAR;
}

int MouseButton(UINT message, WPARAM wparam, bool& down) noexcept
{
    down = false;
    switch (message)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_LBUTTONUP: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_RBUTTONUP: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_MBUTTONUP: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: down = true; [[fallthrough]];
    case WM_XBUTTONUP: return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
    }
    return -1;
}

void UpdateMouseCapture(HWND window, UINT message, WPARAM wparam)
{
    bool down = false;
    const int index = MouseButton(message, wparam, down);
    const uint32_t button = index >= 0 ? 1u << index : 0;
    const bool cancel = message == WM_CANCELMODE || message == WM_KILLFOCUS
        || message == WM_NCDESTROY || (message == WM_ACTIVATEAPP && !wparam);
    if (!button && !cancel && message != WM_CAPTURECHANGED) return;

    bool acquire = false;
    bool release = false;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found == gWindows.end()) return;
        auto& input = found->second;
        if (cancel || message == WM_CAPTURECHANGED)
        {
            constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
            for (unsigned index = 0; index < std::size(releases); ++index)
                if (input.mouseButtons & (1u << index))
                    input.messages.push_back({releases[index], index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0, 0});
        }
        if (message == WM_CAPTURECHANGED)
        {
            input.mouseButtons = 0;
            input.ownsCapture = false;
        }
        else if (cancel)
        {
            release = input.ownsCapture;
            input.mouseButtons = 0;
            input.ownsCapture = false;
        }
        else if (down && input.clients && gVisible.load(std::memory_order_acquire))
        {
            input.mouseButtons |= button;
            acquire = !input.ownsCapture;
        }
        else if (!down)
        {
            input.mouseButtons &= ~button;
            release = input.ownsCapture && !input.mouseButtons;
            if (release) input.ownsCapture = false;
        }
    }
    // SetCapture/ReleaseCapture belong to the HWND's thread. ImGui's queued
    // Win32 handler runs on the presentation thread, which may be different.
    // Never steal existing game capture, and never hold our mutex across an
    // API that can synchronously deliver WM_CAPTURECHANGED to this procedure.
    if (release && GetCapture() == window) ReleaseCapture();
    if (acquire && window == gActiveWindow.load(std::memory_order_acquire)
        && CaptureInput() && !GetCapture())
    {
        SetCapture(window);
        if (GetCapture() == window)
        {
            bool retained = false;
            {
                std::lock_guard lock(gInputMutex);
                const auto found = gWindows.find(window);
                if (found != gWindows.end() && found->second.clients)
                {
                    found->second.ownsCapture = true;
                    retained = true;
                }
            }
            if (!retained) ReleaseCapture();
        }
    }
}

void QueueWindowMessage(HWND window, const InputMessage& message)
{
    std::lock_guard lock(gInputMutex);
    const auto found = gWindows.find(window);
    if (found == gWindows.end() || !found->second.clients) return;
    auto& messages = found->second.messages;
    // menunotes.1: consecutive moves keep only the latest position, so a fast mouse cannot overflow the queue.
    if (message.message == WM_MOUSEMOVE && !messages.empty() && messages.back().message == WM_MOUSEMOVE
        && messages.back().fromHook == message.fromHook && messages.back().mouseSource == message.mouseSource)
    {
        messages.back() = message;
        return;
    }
    if (messages.size() >= 512)
    {
        messages.clear();
        messages.push_back({WM_KILLFOCUS, 0, 0});
        messages.push_back({WM_SETFOCUS, 0, 0});
    }
    messages.push_back(message);
}

bool RawMouseOmitsLegacyMessages(HWND window)
{
    // Query only on button/wheel transitions, not on high-rate motion packets.
    // Observing the registration leaves the game's input ownership unchanged.
    RAWINPUTDEVICE devices[64]{};
    UINT capacity = static_cast<UINT>(std::size(devices));
    const UINT count = GetRegisteredRawInputDevices(devices, &capacity, sizeof(RAWINPUTDEVICE));
    if (count == UINT(-1) || count > std::size(devices)) return false;
    for (UINT index = 0; index < count; ++index)
    {
        const auto& device = devices[index];
        if (device.usUsagePage == 0x01 && device.usUsage == 0x02
            && (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY
            && (!device.hwndTarget || device.hwndTarget == window))
            return true;
    }
    return false;
}

void QueueRawMouse(HWND window, const RAWMOUSE& mouse)
{
    if (!gVisible.load(std::memory_order_acquire)
        || window != gActiveWindow.load(std::memory_order_acquire)) return;
    // The desktop cursor already applies Windows pointer speed and absolute
    // device mapping. Keep its position ordered with this packet's buttons.
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor))
        QueueWindowMessage(window, {WM_MOUSEMOVE, 0, MAKELPARAM(cursor.x, cursor.y)});
    if (!mouse.usButtonFlags || !RawMouseOmitsLegacyMessages(window)) return;
    constexpr UINT downMessages[]{WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN};
    constexpr UINT upMessages[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (unsigned index = 0; index < std::size(downMessages); ++index)
    {
        const WPARAM parameter = index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0;
        for (unsigned transition = 0; transition < 2; ++transition)
        {
            if (!(mouse.usButtonFlags & (1u << (index * 2 + transition)))) continue;
            const UINT message = transition ? upMessages[index] : downMessages[index];
            // Preserve the existing window-thread capture and cancellation path.
            UpdateMouseCapture(window, message, parameter);
            QueueWindowMessage(window, {message, parameter, 0});
        }
    }
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
        QueueWindowMessage(window, {WM_MOUSEWHEEL, MAKEWPARAM(0, mouse.usButtonData), 0});
    if (mouse.usButtonFlags & RI_MOUSE_HWHEEL)
        QueueWindowMessage(window, {WM_MOUSEHWHEEL, MAKEWPARAM(0, mouse.usButtonData), 0});
}

void ReleaseRetiredRawMouse(HWND window, const RAWMOUSE& mouse)
{
    constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    for (unsigned index = 0; index < std::size(releases); ++index)
        if (mouse.usButtonFlags & (2u << (index * 2)))
            UpdateMouseCapture(window, releases[index],
                index >= 3 ? MAKEWPARAM(0, index == 3 ? XBUTTON1 : XBUTTON2) : 0);
}

// Render thread, open menu: the cursor where it is now. Window messages carry
// it only when the game pumps them, once per game frame, but the menu draws on
// every frame it presents: under frame generation every output frame, at 6x
// six cursor updates per game frame. Buttons and wheel stay on the messages,
// which ImGui orders before this position. Same thread as the backend's client
// rectangle, so both share one DPI context, as in its own cursor fallback.
void PollCursor(HWND window, ImGuiIO& io) noexcept
{
    POINT cursor{};
    if (!io.WantSetMousePos && GetCursorPos(&cursor) && ScreenToClient(window, &cursor))
        io.AddMousePosEvent(float(cursor.x), float(cursor.y));
}

// DirectInput mouse route. dinput8 reads an exclusive mouse as raw input on its
// own thread (hidden DIEmWin window, RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE), so
// the game window receives no button messages and the menu cannot be clicked
// (Dying Light: The Beast). While the menu holds input in such a process, a
// WH_MOUSE_LL hook on our own thread takes the mouse: the cursor follows each
// event and buttons and wheel are queued for the menu. Consuming the event does
// not stop raw input, and DirectInput reads its packets before dispatching
// them, so the game still receives the mouse while the menu is open. The hook
// exists only while the menu is open; other processes keep the window-message
// path unchanged.
//
// The same hook serves a menu window whose procedure gets no mouse messages at
// all (raw input aimed at another window, a pump that handles the mouse itself,
// a procedure set over ours that does not call it). Watch notices this while
// the menu is open; the window keeps the hook for the rest of the process.
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
namespace mouse_route
{
constexpr UINT kApply = WM_APP + 0x51;
std::atomic<HWND> gWindow{nullptr};     // menu window the hook serves; null = no hook wanted
std::atomic<HWND> gDeclined{nullptr};   // menu window checked without a DirectInput window
std::atomic<HWND> gStarved{nullptr};    // menu window found to get no mouse messages
std::atomic<bool> gNoMessages{false};   // the current request is for gStarved (else DirectInput)
std::atomic<uint32_t> gMessages{0};     // mouse messages the menu window's procedure received
std::atomic<uint32_t> gClicks{0};       // of those, button and wheel messages (legacy or raw)
std::atomic<uint32_t> gGeneration{0};   // each request reinstalls, so ours runs first
std::atomic<bool> gForeground{false};
std::atomic<uint64_t> gCursor{0};       // physical client x, y, width, height (16 bits each)
std::atomic<bool> gThreadStarted{false};
std::atomic<DWORD> gThread{0};
std::atomic<uint32_t> gMoves{0};
std::atomic<uint32_t> gButtons{0};
std::atomic<uint32_t> gWheel{0};
std::atomic<uint32_t> gLogLines{0};
// Watch's state for the open menu, render threads only. Two renderers on one
// window must not share it at once.
std::mutex gWatchMutex;
uint32_t gWatchMessages = 0;
uint32_t gWatchClicks = 0;
uint64_t gWatchSince = 0;               // first use of the mouse with no mouse message since; 0 = none
uint64_t gWatchPressed = 0;             // first press with no button message since; 0 = none
POINT gWatchCursor{LONG_MIN, LONG_MIN};
uint32_t gWatchButtons = 0;
uint32_t gWatchMoves = 0;
uint32_t gWatchPresses = 0;
// menunotes.1: a message posted to the menu window once it looks starved, and when the window's procedure got it.
std::atomic<UINT> gProbeMessage{0};     // registered on first use; 0 = none
std::atomic<uint32_t> gProbes{0};       // probes the menu window's procedure received
uint64_t gWatchProbe = 0;               // probe posted; 0 = none
uint64_t gWatchProbeSeen = 0;           // the procedure got it; 0 = not yet
uint32_t gWatchProbeCount = 0;

void Log(const wchar_t* line) noexcept
{
    if (gLogLines.fetch_add(1, std::memory_order_relaxed) < 32) single_module::Log(line);
}

std::optional<ImVec2> Map(uint64_t cursor, ImVec2 display) noexcept
{
    const float width = float(uint16_t(cursor >> 32));
    const float height = float(uint16_t(cursor >> 48));
    if (width <= 0.0f || height <= 0.0f || display.x <= 0.0f || display.y <= 0.0f) return std::nullopt;
    return ImVec2((float(uint16_t(cursor)) + 0.5f) * display.x / width,
        (float(uint16_t(cursor >> 16)) + 0.5f) * display.y / height);
}

LRESULT CALLBACK HookProc(int code, WPARAM message, LPARAM parameter)
{
    const HWND window = gWindow.load(std::memory_order_acquire);
    if (code != HC_ACTION || !window || window != gActiveWindow.load(std::memory_order_acquire)
        || !CaptureInput())
        return CallNextHookEx(nullptr, code, message, parameter);
    // This thread is per-monitor aware: event, cursor and client rectangle
    // are all physical pixels.
    const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(parameter);
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)
        || client.right <= 0 || client.bottom <= 0 || client.right > 0xFFFF || client.bottom > 0xFFFF)
        return CallNextHookEx(nullptr, code, message, parameter);
    const POINT target{std::clamp<LONG>(event.pt.x, origin.x, origin.x + client.right - 1),
        std::clamp<LONG>(event.pt.y, origin.y, origin.y + client.bottom - 1)};
    POINT cursor{};
    if (GetCursorPos(&cursor) && (cursor.x != target.x || cursor.y != target.y))
    {
        SetCursorPos(target.x, target.y);
        // A clip left by the game or DirectInput would hold the cursor.
        if (GetCursorPos(&cursor) && (cursor.x != target.x || cursor.y != target.y))
        {
            ClipCursor(nullptr);
            SetCursorPos(target.x, target.y);
        }
    }
    const uint64_t packed = uint64_t(uint16_t(target.x - origin.x))
        | uint64_t(uint16_t(target.y - origin.y)) << 16
        | uint64_t(uint16_t(client.right)) << 32 | uint64_t(uint16_t(client.bottom)) << 48;
    gCursor.store(packed, std::memory_order_release);
    WPARAM wparam = 0;
    switch (message)
    {
    case WM_MOUSEMOVE:
        gMoves.fetch_add(1, std::memory_order_relaxed);
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        gButtons.fetch_add(1, std::memory_order_relaxed);
        break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        gButtons.fetch_add(1, std::memory_order_relaxed);
        wparam = MAKEWPARAM(0, HIWORD(event.mouseData));
        break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        gWheel.fetch_add(1, std::memory_order_relaxed);
        wparam = MAKEWPARAM(0, HIWORD(event.mouseData));
        break;
    default:
        return 1;
    }
    QueueWindowMessage(window, {static_cast<UINT>(message), wparam,
        static_cast<LPARAM>(packed), ImGuiMouseSource_Mouse, true});
    return 1;
}

DWORD WINAPI Thread(void*)
{
    gThread.store(GetCurrentThreadId(), std::memory_order_seq_cst);
    // Every mouse event in the system waits for this thread while the hook is
    // installed; a busy game must not delay it.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    using SetContext = HANDLE (WINAPI*)(HANDLE);
    if (const auto set = reinterpret_cast<SetContext>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
        if (!set(reinterpret_cast<HANDLE>(-4))) set(reinterpret_cast<HANDLE>(-3)); // per-monitor v2, else v1
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // message queue for requests
    HHOOK hook = nullptr;
    uint32_t applied = 0;
    for (;;)
    {
        const bool wanted = gWindow.load(std::memory_order_seq_cst) != nullptr;
        const uint32_t generation = gGeneration.load(std::memory_order_seq_cst);
        if (wanted && (!hook || applied != generation))
        {
            if (hook) UnhookWindowsHookEx(hook);
            hook = SetWindowsHookExW(WH_MOUSE_LL, &HookProc, single_module::Self(), 0);
            const DWORD error = hook ? 0ul : GetLastError();
            applied = generation;
            wchar_t line[160]{};
            swprintf_s(line, L"MFG_PROXY_UI mouse-route=low-level-hook reason=%ls installed=%d generation=%u error=%lu",
                gNoMessages.load(std::memory_order_seq_cst) ? L"no-window-messages" : L"directinput",
                hook ? 1 : 0, generation, error);
            Log(line);
        }
        else if (!wanted && hook)
        {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
            wchar_t line[160]{};
            swprintf_s(line, L"MFG_PROXY_UI mouse-route closed moves=%u buttons=%u wheel=%u",
                gMoves.exchange(0), gButtons.exchange(0), gWheel.exchange(0));
            Log(line);
        }
        // Low-level hook callbacks are dispatched inside GetMessageW; a posted
        // kApply only wakes the loop to apply a new request.
        if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

void Request(HWND window) noexcept
{
    if (window)
    {
        gCursor.store(0, std::memory_order_release);
        gGeneration.fetch_add(1, std::memory_order_seq_cst);
    }
    gWindow.store(window, std::memory_order_seq_cst);
    if (!gThreadStarted.exchange(true))
    {
        // The thread runs this module's hook; keep the image for its lifetime.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&HookProc), &pinned);
        if (const HANDLE thread = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr)) CloseHandle(thread);
        else
        {
            gThreadStarted.store(false);
            Log(L"MFG_PROXY_UI mouse-route=window-messages hook thread unavailable");
        }
        return;
    }
    // The thread reads the request after publishing its id and creating its
    // queue, so a request it cannot be woken for is still applied.
    if (const DWORD thread = gThread.load(std::memory_order_seq_cst))
        PostThreadMessageW(thread, kApply, 0, 0);
}

bool DirectInputWindow() noexcept
{
    const DWORD self = GetCurrentProcessId();
    for (HWND candidate = FindWindowExW(nullptr, nullptr, L"DIEmWin", nullptr); candidate;
         candidate = FindWindowExW(nullptr, candidate, L"DIEmWin", nullptr))
    {
        DWORD process = 0;
        if (GetWindowThreadProcessId(candidate, &process) && process == self) return true;
    }
    return false;
}

// Once per menu opening without DirectInput: whether a procedure set later sits
// over ours, and where this process's raw mouse input goes.
void LogWindowMessages(HWND window) noexcept
{
    const bool ours = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WindowProc;
    const wchar_t* target = L"none";
    DWORD flags = 0;
    RAWINPUTDEVICE devices[64]{};
    UINT capacity = static_cast<UINT>(std::size(devices));
    const UINT count = GetRegisteredRawInputDevices(devices, &capacity, sizeof(RAWINPUTDEVICE));
    for (UINT index = 0; count <= std::size(devices) && index < count; ++index)
    {
        const auto& device = devices[index];
        if (device.usUsagePage != 0x01 || device.usUsage != 0x02) continue;
        flags = device.dwFlags;
        target = !device.hwndTarget ? L"focus" : device.hwndTarget == window ? L"menu-window" : L"other-window";
    }
    wchar_t line[192]{};
    swprintf_s(line, L"MFG_PROXY_UI mouse-route=window-messages directinput=0 wndproc=%ls raw-mouse=%ls flags=0x%lX",
        ours ? L"ours" : L"set-later", target, flags);
    Log(line);
}

void ResetWatch() noexcept
{
    gWatchSince = gWatchPressed = gWatchProbe = gWatchProbeSeen = 0;
    gWatchCursor = POINT{LONG_MIN, LONG_MIN};
    gWatchButtons = gWatchMoves = gWatchPresses = 0;
}

// Render thread, open menu in the foreground without the hook: true once the
// mouse was used inside the client area and 500 ms passed without a mouse (or,
// after a press, a button) message. A game pumps its messages every frame, so
// only a window that never gets the mouse stays this quiet. A long game frame
// stalls the pump just as long (menunotes.1), so a probe is posted: it decides
// once our procedure got the probe and 100 ms more passed without the mouse,
// or after 2 s when the probe never arrives (a pump that filters it).
bool Watch(HWND window) noexcept
{
    std::unique_lock lock(gWatchMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const uint32_t messages = gMessages.load(std::memory_order_acquire);
    const uint32_t clicks = gClicks.load(std::memory_order_acquire);
    POINT cursor{};
    RECT client{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor) || !GetClientRect(window, &client)) return false;
    const bool inside = cursor.x >= 0 && cursor.y >= 0 && cursor.x < client.right && cursor.y < client.bottom;
    constexpr int keys[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    uint32_t buttons = 0;
    for (unsigned index = 0; index < std::size(keys); ++index)
        if (PhysicalKeyDown(keys[index])) buttons |= 1u << index;
    const bool first = gWatchCursor.x == LONG_MIN;
    const bool moved = !first && inside && (cursor.x != gWatchCursor.x || cursor.y != gWatchCursor.y);
    const bool pressed = !first && inside && (buttons & ~gWatchButtons) != 0;
    gWatchCursor = cursor;
    gWatchButtons = buttons;
    if (messages != gWatchMessages)
    {
        gWatchMessages = messages;
        gWatchSince = 0;
        gWatchMoves = 0;
    }
    if (clicks != gWatchClicks)
    {
        gWatchClicks = clicks;
        gWatchPressed = 0;
        gWatchPresses = 0;
    }
    const uint64_t now = GetTickCount64();
    if ((moved || pressed) && !gWatchSince) gWatchSince = now;
    if (pressed && !gWatchPressed) gWatchPressed = now;
    gWatchMoves += moved ? 1 : 0;
    gWatchPresses += pressed ? 1 : 0;
    const bool silent = gWatchSince && now - gWatchSince >= 500;
    const bool unclicked = gWatchPressed && now - gWatchPressed >= 500;
    if (!silent && !unclicked)
    {
        gWatchProbe = gWatchProbeSeen = 0;
        return false;
    }
    const wchar_t* pump = L"not probed";
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WindowProc)
    {
        if (!gWatchProbe)
        {
            UINT probe = gProbeMessage.load(std::memory_order_acquire);
            if (!probe)
            {
                probe = RegisterWindowMessageW(L"MFG_PROXY_UI mouse-route probe");
                gProbeMessage.store(probe, std::memory_order_release);
            }
            gWatchProbeCount = gProbes.load(std::memory_order_acquire);
            if (probe && PostMessageW(window, probe, 0, 0))
            {
                gWatchProbe = now;
                return false;
            }
        }
        else if (gProbes.load(std::memory_order_acquire) == gWatchProbeCount)
        {
            if (now - gWatchProbe < 2000) return false; // the pump is late, or filters the probe
            pump = L"probe not received";
        }
        else
        {
            if (!gWatchProbeSeen) gWatchProbeSeen = now;
            if (now - gWatchProbeSeen < 100) return false;
            pump = L"pump ran";
        }
    }
    wchar_t line[224]{};
    swprintf_s(line, L"MFG_PROXY_UI mouse-route: no %ls message reached the menu window in %llu ms (moves=%u presses=%u, %ls)",
        silent ? L"mouse" : L"button", now - (silent ? gWatchSince : gWatchPressed), gWatchMoves, gWatchPresses, pump);
    Log(line);
    return true;
}

// Render thread, every frame: request the hook when the menu opens in a
// DirectInput process or on a window found to get no mouse messages, watch
// any other open menu's window, reinstall the hook when the game regains the
// foreground (a low-level hook installed meanwhile would otherwise run ahead
// of ours), and remove it when the menu closes. Beside OptiScaler's menu its
// input layer withholds every message, and PollMouse reads the mouse.
void Refresh(HWND window, bool foreground) noexcept
{
    const bool open = window && gVisible.load(std::memory_order_acquire)
        && window == gActiveWindow.load(std::memory_order_acquire);
    const HWND current = gWindow.load(std::memory_order_acquire);
    if (!open)
    {
        if (current && current == window) Request(nullptr);
        if (gDeclined.load(std::memory_order_acquire) == window) gDeclined.store(nullptr);
        if (!gVisible.load(std::memory_order_acquire))
        {
            std::unique_lock lock(gWatchMutex, std::try_to_lock);
            if (lock.owns_lock()) ResetWatch();
        }
        return;
    }
    if (current != window)
    {
        bool directInput = false;
        if (gStarved.load(std::memory_order_acquire) != window)
        {
            if (gDeclined.load(std::memory_order_acquire) != window)
            {
                directInput = DirectInputWindow();
                if (!directInput)
                {
                    gDeclined.store(window, std::memory_order_release);
                    LogWindowMessages(window);
                }
            }
            if (!directInput)
            {
                if (!foreground)
                {
                    // Back in the foreground the watch starts afresh: an activating click is no unexplained press.
                    std::unique_lock lock(gWatchMutex, std::try_to_lock);
                    if (lock.owns_lock()) ResetWatch();
                    return;
                }
                if (shared_menu::Active() || !Watch(window)) return;
                gStarved.store(window, std::memory_order_release);
            }
        }
        gNoMessages.store(!directInput, std::memory_order_seq_cst);
        gForeground.store(foreground, std::memory_order_release);
        Request(window);
        return;
    }
    if (!foreground) gForeground.store(false, std::memory_order_release);
    else if (!gForeground.exchange(true)) Request(window);
}

bool Serving(HWND window) noexcept
{
    return window && gWindow.load(std::memory_order_acquire) == window
        && gCursor.load(std::memory_order_acquire) != 0;
}

bool Starved(HWND window) noexcept
{
    return window && gStarved.load(std::memory_order_acquire) == window;
}
}

// Beside OptiScaler's menu (OptiScaler's shared UI mode) this menu offers the
// DLSS presets and DLSS 5. It opens and closes on OptiScaler's menu key, shows
// only while a feature it controls runs, and stays usable while OptiScaler's
// menu is open. OptiScaler's input layer then consumes every mouse and key
// message in PeekMessage/GetMessage and answers GetAsyncKeyState and
// GetCursorPos with blocked values for every caller, so this menu reads the
// key and the mouse below it: win32u's NtUserGetAsyncKeyState and GetCursorInfo.
namespace shared_menu
{
std::atomic<int> gKey{-1};              // OptiScaler's menu key: -1 not read, 0 none
std::atomic<uint64_t> gKeyRead{0};
std::atomic<bool> gReady{false};
std::atomic<uint64_t> gReadySince{0};   // the startup hint's start
std::atomic<uint32_t> gLogLines{0};
// Render thread only.
uint64_t gNextReadyCheck = 0;
bool gKeyHeld = false;
optiscaler_menu_key::Follower gFollower;
uint32_t gButtons = 0;
POINT gReal{LONG_MIN, LONG_MIN};
POINT gReported{LONG_MIN, LONG_MIN};
bool gOptiScalerCursor = false;

void Log(const wchar_t* line) noexcept
{
    if (gLogLines.fetch_add(1, std::memory_order_relaxed) < 32) single_module::Log(line);
}

bool Active() noexcept
{
    return optiscaler_host::UiDelegated() && !optiscaler_host::UiHidden();
}

bool Ready() noexcept
{
    return !Active() || gReady.load(std::memory_order_acquire);
}

bool PhysicalDown(int key) noexcept
{
    return PhysicalKeyDown(key);
}

// [Menu] ShortcutKey in OptiScaler.ini beside OptiScaler's module.
int ReadKey() noexcept
{
    wchar_t path[MAX_PATH]{};
    const HMODULE host = optiscaler_host::Module();
    const DWORD length = host ? GetModuleFileNameW(host, path, MAX_PATH) : 0;
    wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash || wcscpy_s(slash + 1, static_cast<size_t>(path + MAX_PATH - (slash + 1)), L"OptiScaler.ini"))
        return optiscaler_menu_key::kDefaultKey;
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"Menu", L"ShortcutKey", L"auto", value, static_cast<DWORD>(std::size(value)), path);
    return optiscaler_menu_key::ParseShortcutKey(value);
}

void StoreKey(int key) noexcept
{
    gKeyRead.store(GetTickCount64(), std::memory_order_release);
    if (gKey.exchange(key, std::memory_order_acq_rel) == key) return;
    wchar_t line[160]{};
    swprintf_s(line, L"MFG_PROXY_UI shared menu: OptiScaler menu key 0x%02X%ls", key,
        key ? L"" : L" (none; this menu keeps its own key)");
    Log(line);
}

// OptiScaler's menu key while this menu shares it; 0 = this menu's own key
// (outside shared mode, or OptiScaler has no menu key).
int Key() noexcept
{
    if (!Active()) return 0;
    int key = gKey.load(std::memory_order_acquire);
    if (key < 0)
    {
        key = ReadKey();
        StoreKey(key);
    }
    return key;
}

// Render thread: OptiScaler writes a rebound key to its INI when it saves.
void RefreshKey() noexcept
{
    if (!Active() || Key() < 0
        || GetTickCount64() - gKeyRead.load(std::memory_order_acquire) < 10000) return;
    StoreKey(ReadKey());
}

// Render thread: whether DLSS Super Resolution, DLAA, Ray Reconstruction or
// DLSS 5 runs, checked four times a second.
bool RefreshReady() noexcept
{
    if (!Active()) return true;
    const uint64_t now = GetTickCount64();
    if (now < gNextReadyCheck) return gReady.load(std::memory_order_acquire);
    gNextReadyCheck = now + 250;
    const auto dlss = dlss_sr::Read();
    const bool neural = dlss5::Running();
    const bool ready = dlss.running || dlss.rayReconstructionRunning || neural;
    if (ready != gReady.exchange(ready, std::memory_order_acq_rel))
    {
        if (ready) gReadySince.store(now, std::memory_order_release);
        wchar_t line[160]{};
        swprintf_s(line, L"MFG_PROXY_UI shared menu %ls (DLSS SR=%d RR=%d DLSS 5=%d)",
            ready ? L"available" : L"hidden while no DLSS feature runs",
            dlss.running ? 1 : 0, dlss.rayReconstructionRunning ? 1 : 0, neural ? 1 : 0);
        Log(line);
    }
    return ready;
}

// Render thread, game in the foreground. OptiScaler toggles its menu on the
// key's release; this menu follows it at the press.
void PollKey(HWND window, int key) noexcept
{
    const bool down = PhysicalDown(key);
    if (down && !gKeyHeld)
    {
        const bool reported = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool visible = gVisible.load(std::memory_order_acquire);
        const bool next = gFollower.AfterPress(reported, visible);
        if (next != visible) SetVisible(next, window);
        wchar_t line[160]{};
        swprintf_s(line, L"MFG_PROXY_UI shared menu key: OptiScaler menu %ls, this menu %ls",
            !reported ? L"closes" : gFollower.BlockingSeen() ? L"opens" : L"toggles",
            gVisible.load(std::memory_order_acquire) ? L"open" : L"closed");
        Log(line);
    }
    gKeyHeld = down;
}

// Render thread, this menu open with the game in the foreground: the cursor
// and buttons, read below OptiScaler's input blocking. Window messages that
// OptiScaler lets through give the same events, which ImGui drops as
// duplicates, and the wheel. Also used on a window that gets no mouse
// messages (mouse_route::Starved) until the hook serves it.
void PollMouse(HWND window, ImGuiIO& io) noexcept
{
    using SetContext = HANDLE (WINAPI*)(HANDLE);
    static const auto set = reinterpret_cast<SetContext>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    // Per-monitor aware for these calls: cursor and client rectangle are both physical pixels.
    HANDLE previous = set ? set(reinterpret_cast<HANDLE>(-4)) : nullptr;
    if (set && !previous) previous = set(reinterpret_cast<HANDLE>(-3));
    CURSORINFO cursor{sizeof(cursor)};
    RECT client{};
    POINT point{};
    bool placed = GetCursorInfo(&cursor) && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0;
    if (placed)
    {
        point = cursor.ptScreenPos;
        placed = ScreenToClient(window, &point) != FALSE;
    }
    if (previous) set(previous);
    if (placed)
        io.AddMousePosEvent((float(point.x) + 0.5f) * io.DisplaySize.x / float(client.right),
            (float(point.y) + 0.5f) * io.DisplaySize.y / float(client.bottom));
    // These are the physical buttons; follow the swap setting.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int keys[3]{swapped ? VK_RBUTTON : VK_LBUTTON, swapped ? VK_LBUTTON : VK_RBUTTON, VK_MBUTTON};
    for (int button = 0; button < 3; ++button)
    {
        const bool down = PhysicalDown(keys[button]);
        if (down == (((gButtons >> button) & 1u) != 0)) continue;
        gButtons ^= 1u << button;
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMouseButtonEvent(button, down);
    }
}

// Render thread: whether OptiScaler draws the cursor. While its menu is open
// its hooks answer GetCursorPos with the position where the menu opened, so
// the real cursor moves and the reported one does not. Each source is only
// compared with itself, which keeps this independent of DPI virtualization.
bool OptiScalerCursor() noexcept
{
    CURSORINFO cursor{sizeof(cursor)};
    POINT reported{};
    if (!GetCursorInfo(&cursor) || !GetCursorPos(&reported)) return gOptiScalerCursor;
    const bool realMoved = cursor.ptScreenPos.x != gReal.x || cursor.ptScreenPos.y != gReal.y;
    const bool reportedMoved = reported.x != gReported.x || reported.y != gReported.y;
    if (realMoved && gReal.x != LONG_MIN) gOptiScalerCursor = !reportedMoved;
    gReal = cursor.ptScreenPos;
    gReported = reported;
    return gOptiScalerCursor;
}
}

bool ProcessKey(HWND window, int key, bool down)
{
    if (!single_module::OverlayAllowed())
    {
        // OptiScaler detected after the menu was attached: close it once and
        // leave every key to the game and OptiScaler.
        if (gVisible.load(std::memory_order_acquire)) SetVisible(false, window);
        return false;
    }
    // Beside OptiScaler's menu, its key is polled each frame and reaches OptiScaler.
    if (shared_menu::Key()) return false;
    // keybinds.1: while the Keybinds tab binds a key, the menu key neither opens nor closes the menu.
    if (gKeybindCapture.load(std::memory_order_acquire)) return key == gMenuKey.Key();
    const bool wasBinding = gMenuKey.Binding();
    const auto action = gMenuKey.Process(key, down);
    // dlss5presets.1: while a menu text field has focus, the menu key (Backspace by default) edits it: no toggle.
    if (!wasBinding && key == gMenuKey.Key() && gVisible.load(std::memory_order_acquire)
        && gTextInput.load(std::memory_order_acquire)) return false;
    if (action == KeyAction::Toggle)
        SetVisible(!gVisible.load(std::memory_order_acquire), window);
    return wasBinding || action != KeyAction::None || key == gMenuKey.Key();
}

// keybinds.1: the Pin panel key pins the open menu (or, closed, the panel) and unpins a pinned panel.
void TogglePinnedPanel(HWND window)
{
    if (gVisible.load(std::memory_order_acquire))
    {
        SetVisible(false, window);
        gPinned.store(true, std::memory_order_release);
    }
    else gPinned.store(!gPinned.load(std::memory_order_acquire), std::memory_order_release);
}

// keybinds.1: once per frame from WantsFrame, under gUiMutex. A key being bound in the Keybinds tab ends when the menu
// closes. The tab's keys are read like the menu key and applied the next frame. Beside OptiScaler's menu only the
// preset keys and Pin panel apply; in a DirectX 11 game only Pin panel does.
void PollKeybindKeys(HWND window, bool foreground, bool shared, int sharedKey, bool d3d11)
{
    if (!gVisible.load(std::memory_order_acquire) && standalone_ui::KeybindCapturing())
        standalone_ui::CancelKeybindCapture();
    if (standalone_ui::KeybindCapturing()) gMenuKey.CancelBinding();
    gKeybindCapture.store(standalone_ui::KeybindCapturing(), std::memory_order_release);
    gTextInput.store(standalone_ui::TextInputActive(), std::memory_order_release);
    if (!foreground || (!shared && gMenuKey.Binding())) return;
    try
    {
        const auto action = standalone_ui::PollKeybinds(shared ? sharedKey : gMenuKey.Key());
        if (shared && !keybinds::SharedAction(action)) return;
        if (d3d11 && !keybinds::D3D11Action(action)) return;
        if (action == keybinds::Action::ePinPanel) TogglePinnedPanel(window);
        else if (action != keybinds::Action::eCount) standalone_ui::QueueKeybind(action);
    }
    catch (...) {}
}

// A compact one-line pill in the menu's own style: its panel colour and
// rounding, a small inset, and a thin accent in the title-bar blue. The minimum
// size override keeps it one text line tall. Returns its bottom edge.
template<class Content>
float Pill(const char* id, ImVec2 position, ImVec2 pivot, float uiScale, Content&& content)
{
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f*uiScale, 2.0f*uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    float bottom = position.y;
    if (ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
    {
        const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float edge = 3.0f*uiScale, width = 2.0f*uiScale;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x+edge, at.y+edge),
            ImVec2(at.x+edge+width, at.y+size.y-edge), ImGui::GetColorU32(ImGuiCol_TitleBgActive), width);
        content();
        bottom = at.y + size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    return bottom;
}

// DLSS 5 Compare as its last evaluation applied it, within the last half second;
// swapped: the original is on the right (Swap sides).
bool CompareLabels(float& split, bool& held, bool* swapped = nullptr) noexcept
{
    const auto view = dlss5::LastCompare();
    if (!view.tick || view.split <= 0.0f || GetTickCount64() - view.tick > 500) return false;
    split = view.split;
    held = view.held;
    if (swapped) *swapped = view.swapped;
    return true;
}

std::string KeyLabel()
{
    const int shared = shared_menu::Key();
    return KeyName(shared ? shared : gMenuKey.Key());
}

// keyrelease.1: keys and mouse buttons whose press reached the game, per route (window messages or raw input). While
// the menu holds input, presses stay with the menu but these releases still reach the game, so no key stays down in
// the game; a release the game never saw pressed stays with the menu.
namespace held_input
{
enum Route : int { kMessages, kRaw, kRoutes };
constexpr int kButtons = 5; // left, right, middle, X1, X2: MouseButton's order and raw input's buttons 1-5
std::atomic<bool> gKeys[kRoutes][256]{};
std::atomic<bool> gButtons[kRoutes][kButtons]{};

struct Event
{
    int route = -1;       // Route; -1 = no key or mouse button
    int key = -1;         // virtual key, -1 = none
    bool keyDown = false;
    uint32_t pressed = 0; // bit per button
    uint32_t released = 0;
};

Event FromMessage(UINT message, WPARAM wparam) noexcept
{
    Event event;
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP)
    {
        event.route = kMessages;
        event.key = static_cast<int>(wparam & 0xFF);
        event.keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        return event;
    }
    bool down = false;
    if (const int button = MouseButton(message, wparam, down); button >= 0)
    {
        event.route = kMessages;
        (down ? event.pressed : event.released) = 1u << button;
    }
    return event;
}

Event FromRawKeyboard(const RAWKEYBOARD& keyboard) noexcept
{
    Event event;
    // 0xFF is the fake key of an escaped sequence, not a physical key.
    if (!keyboard.VKey || keyboard.VKey >= 0xFF) return event;
    event.route = kRaw;
    event.key = keyboard.VKey;
    event.keyDown = !(keyboard.Flags & RI_KEY_BREAK);
    return event;
}

Event FromRawMouse(const RAWMOUSE& mouse) noexcept
{
    Event event;
    // RI_MOUSE_BUTTON_n_DOWN is bit 2(n-1), RI_MOUSE_BUTTON_n_UP the bit above it.
    for (int button = 0; button < kButtons; ++button)
    {
        if (mouse.usButtonFlags & (1u << (2 * button))) event.pressed |= 1u << button;
        if (mouse.usButtonFlags & (2u << (2 * button))) event.released |= 1u << button;
    }
    if (event.pressed || event.released) event.route = kRaw;
    return event;
}

bool ReleasesHeld(const Event& event) noexcept
{
    if (event.route < 0) return false;
    if (event.key >= 0 && !event.keyDown && gKeys[event.route][event.key].load(std::memory_order_relaxed))
        return true;
    for (int button = 0; button < kButtons; ++button)
        if ((event.released >> button & 1u) && gButtons[event.route][button].load(std::memory_order_relaxed))
            return true;
    return false;
}

// The event reached the game's window procedure.
void Record(const Event& event) noexcept
{
    if (event.route < 0) return;
    if (event.key >= 0) gKeys[event.route][event.key].store(event.keyDown, std::memory_order_relaxed);
    // A raw packet can carry a press and a release of one button: the release is the later of the two.
    for (int button = 0; button < kButtons; ++button)
    {
        if (event.pressed >> button & 1u) gButtons[event.route][button].store(true, std::memory_order_relaxed);
        if (event.released >> button & 1u) gButtons[event.route][button].store(false, std::memory_order_relaxed);
    }
}

// The process lost the foreground: releases go to the next window, and games drop held input themselves.
void Clear() noexcept
{
    for (auto& route : gKeys) for (auto& key : route) key.store(false, std::memory_order_relaxed);
    for (auto& route : gButtons) for (auto& button : route) button.store(false, std::memory_order_relaxed);
}
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    const LPARAM extra = GetMessageExtraInfo();
    const auto mouseSource = (extra & 0xFFFFFF80) == 0xFF515700 ? ImGuiMouseSource_Pen
        : (extra & 0xFFFFFF80) == 0xFF515780 ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse;
    WNDPROC original = nullptr;
    bool hasClient = false;
    bool drainRawCapture = false;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found != gWindows.end())
        {
            original = found->second.original;
            hasClient = found->second.clients != 0;
            drainRawCapture = !hasClient && found->second.ownsCapture;
        }
    }
    if (!original) return DefWindowProcW(window, message, wparam, lparam);
    // mouse_route::Watch's probe: the pump ran. Ours, so the game never sees it.
    if (const UINT probe = mouse_route::gProbeMessage.load(std::memory_order_acquire); probe && message == probe)
    {
        mouse_route::gProbes.fetch_add(1, std::memory_order_release);
        return 0;
    }
    UpdateMouseCapture(window, message, wparam);
    held_input::Event held = held_input::FromMessage(message, wparam); // keyrelease.1; WM_INPUT's below
    // mouse_route::Watch: this procedure gets the menu window's mouse.
    const bool menuWindow = hasClient && window == gActiveWindow.load(std::memory_order_acquire);
    if (menuWindow && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
    {
        if (message != WM_MOUSEMOVE) mouse_route::gClicks.fetch_add(1, std::memory_order_release);
        mouse_route::gMessages.fetch_add(1, std::memory_order_release);
    }
    // OptiScaler's menu key reaches OptiScaler (and the game) even while this menu is open.
    const int sharedKey = hasClient ? shared_menu::Key() : 0;
    bool passKey = sharedKey && (message == WM_KEYDOWN || message == WM_KEYUP) && static_cast<int>(wparam) == sharedKey;
    if (hasClient && (message == WM_KEYDOWN || message == WM_KEYUP))
    {
        if (ProcessKey(window, static_cast<int>(wparam), message == WM_KEYDOWN)) return 0;
    }
    if (message == WM_INPUT && ((hasClient && Foreground(window)) || drainRawCapture))
    {
        // RIDEV_NOLEGACY suppresses ordinary keyboard and mouse messages.
        // Copy the packet now: its handle cannot outlive WM_INPUT cleanup.
        RAWINPUT input{};
        UINT bytes = sizeof(input);
        UINT copied = UINT(-1);
        {
            InternalScope internal;
            copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                &input, &bytes, sizeof(RAWINPUTHEADER));
        }
        if (copied <= sizeof(input) && copied >= sizeof(RAWINPUTHEADER)
            && input.header.dwSize <= copied)
        {
            if (hasClient && input.header.dwType == RIM_TYPEKEYBOARD
                && input.header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
            {
                if (sharedKey && input.data.keyboard.VKey == sharedKey) passKey = true;
                else if (ProcessKey(window, input.data.keyboard.VKey, !(input.data.keyboard.Flags & RI_KEY_BREAK)))
                    return DefWindowProcW(window, message, wparam, lparam);
                held = held_input::FromRawKeyboard(input.data.keyboard);
            }
            if (input.header.dwType == RIM_TYPEMOUSE
                && input.header.dwSize >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
            {
                if (drainRawCapture) ReleaseRetiredRawMouse(window, input.data.mouse);
                else
                {
                    if (menuWindow && input.data.mouse.usButtonFlags)
                        mouse_route::gClicks.fetch_add(1, std::memory_order_release);
                    if (menuWindow) mouse_route::gMessages.fetch_add(1, std::memory_order_release);
                    QueueRawMouse(window, input.data.mouse);
                    held = held_input::FromRawMouse(input.data.mouse);
                }
            }
        }
    }
    if (!hasClient) DetachWindow(window, false);
    if (hasClient)
    {
        // WM_INPUT itself is not queued: QueueRawMouse queued what the menu needs from it.
        if ((gVisible.load(std::memory_order_acquire) && message != WM_INPUT
                && (MouseMessage(message) || KeyboardMessage(message)
                    || message == WM_NCMOUSEMOVE
                    || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP))
            // These notifications are outside WM_MOUSEFIRST..WM_MOUSELAST.
            // The Win32 backend needs them even if the menu just closed,
            // to retire hover state and rearm tracking on the next entry.
            || message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE
            || message == WM_SETFOCUS || message == WM_KILLFOCUS)
            QueueWindowMessage(window, {message, wparam, lparam, mouseSource});
    }
    if (hasClient && CaptureInput())
    {
        if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT)
        {
            SetCursor(nullptr); // ImGui draws its own cursor at the game position.
            return TRUE;
        }
        // keyrelease.1: the release of a key or button the game holds still reaches it (a raw packet with its motion).
        if (!passKey && !held_input::ReleasesHeld(held)
            && ((MouseMessage(message) && gWantMouse.load(std::memory_order_acquire))
                || (KeyboardMessage(message) && gWantKeyboard.load(std::memory_order_acquire))))
        {
            // WM_INPUT still requires the OS cleanup performed by DefWindowProc.
            return message == WM_INPUT
                ? DefWindowProcW(window, message, wparam, lparam) : 0;
        }
    }
    if (message == WM_ACTIVATEAPP && !wparam)
    {
        InternalScope internal;
        gMenuKey.ResetPressed();
        held_input::Clear();
        ClipCursor(nullptr);
    }
    if (message == WM_NCDESTROY)
    {
        if (gActiveWindow.load(std::memory_order_acquire) == window)
        {
            SetVisible(false, window);
            gActiveWindow.store(nullptr, std::memory_order_release);
        }
        std::lock_guard lock(gInputMutex);
        gWindows.erase(window);
    }
    held_input::Record(held);
    return CallWindowProcW(original, window, message, wparam, lparam);
}

bool AttachWindow(HWND window)
{
    DWORD process = 0;
    if (!IsWindow(window) || !GetWindowThreadProcessId(window, &process)
        || process != GetCurrentProcessId()) return false;
    std::lock_guard lock(gInputMutex);
    auto found = gWindows.find(window);
    if (found != gWindows.end())
    {
        ++found->second.clients;
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (!original) return false;
    gWindows.emplace(window, WindowInput{original, {}, 1});
    return true;
}

void DetachWindow(HWND window, bool releaseClient)
{
    std::lock_guard lock(gInputMutex);
    const auto found = gWindows.find(window);
    if (found == gWindows.end()) return;
    if (releaseClient && found->second.clients) --found->second.clients;
    if (found->second.clients) return;
    // Input retirement must not depend on a GPU fence or ImGui destruction.
    // Keep another renderer's client on this HWND (or active HWND) intact.
    if (gActiveWindow.load(std::memory_order_acquire) == window)
    {
        SetVisible(false, window);
        gActiveWindow.store(nullptr, std::memory_order_release);
    }
    // A render-thread retirement cannot release window-thread capture. Keep
    // forwarding until button-up, cancellation or focus loss retires it there.
    if (found->second.ownsCapture) return;
    // Preserve a later subclass belonging to the game, ReShade or another mod.
    // Our pinned forwarding thunk remains safe in that chain without clients.
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WindowProc)
    {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(found->second.original));
        gWindows.erase(found);
    }
}

std::string IniFilename()
{
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return {};
    std::wstring path(executable);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash+1);
    path += L"RTX40MFG-UI.ini";
    const UINT key = GetPrivateProfileIntW(L"Overlay", L"ToggleKey", kDefaultMenuKey, path.c_str());
    gMenuKey.Load(static_cast<int>(key));
    // Dear ImGui rewrites its entire INI. Keep layout separate from the
    // panel's preferences so saving window positions cannot discard them.
    path.resize(slash+1);
    path += L"RTX40MFG-UI-layout.ini";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

bool SystemKey(int key) noexcept
{
    return key == VK_LWIN || key == VK_RWIN || key == VK_MENU
        || key == VK_LMENU || key == VK_RMENU || key == VK_TAB;
}
struct AsyncKeyTag
{
    static SHORT Call(SHORT (WINAPI* original)(int), int key)
    { return CaptureInput() && !SystemKey(key) ? 0 : original(key); }
};
struct KeyTag
{
    static SHORT Call(SHORT (WINAPI* original)(int), int key)
    { return CaptureInput() && !SystemKey(key) ? 0 : original(key); }
};
struct KeyboardTag
{
    static BOOL Call(BOOL (WINAPI* original)(PBYTE), PBYTE keys)
    {
        const BOOL result = original(keys);
        if (result && keys && CaptureInput())
            for (int key = 0; key < 256; ++key) if (!SystemKey(key)) keys[key] = 0;
        return result;
    }
};
struct CursorPositionTag
{
    static BOOL Call(BOOL (WINAPI* original)(int, int), int x, int y)
    { return CaptureInput() ? TRUE : original(x, y); }
};
struct ClipTag
{
    static BOOL Call(BOOL (WINAPI* original)(const RECT*), const RECT* clip)
    {
        if (!CaptureInput()) return original(clip);
        std::lock_guard clipLock(gClipMutex);
        if (clip) { gPreviousClip = *clip; gHavePreviousClip = true; }
        else gHavePreviousClip = false;
        return original(nullptr);
    }
};
}

bool PlatformState::Initialize(HWND hwnd)
{
    if (context)
    {
        if (window != hwnd || !inputAttached) return false;
        ContextScope current(context);
        InternalScope internal;
        // The pinned docking renderer's Shutdown destroys platform viewport
        // data too, even with extra viewports disabled. Rebind Win32 after a
        // renderer recreation while retaining this context and its UI state.
        if (!ImGui::GetMainViewport()->PlatformUserData)
        {
            ImGui_ImplWin32_Shutdown();
            return ImGui_ImplWin32_Init(hwnd);
        }
        return true;
    }
    if (!AttachWindow(hwnd)) return false;
    inputAttached = true;
    window = hwnd;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(previous);
    ContextScope current(context);
    InternalScope internal;
    ImGui::StyleColorsDark();
    {
        // Stock dark colours; slightly roomier spacing and rounded corners.
        auto& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(12.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.CellPadding = ImVec2(6.0f, 3.0f);
        style.WindowRounding = 6.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 6.0f;
        style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
    }
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    iniPath = IniFilename();
    io.IniFilename = iniPath.empty() ? nullptr : iniPath.c_str();
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;
    ImFontConfig fontConfig;
    fontConfig.Flags |= ImFontFlags_NoLoadError;
    wchar_t windowsPath[32768]{};
    const UINT windowsLength = GetWindowsDirectoryW(windowsPath, static_cast<UINT>(std::size(windowsPath)));
    if (windowsLength && windowsLength < std::size(windowsPath))
    {
        // Use the installed Windows UI font. No font file is redistributed.
        const auto fontPath = (std::filesystem::path(windowsPath)/L"Fonts"/L"segoeui.ttf").u8string();
        io.FontDefault = io.Fonts->AddFontFromFileTTF(
            reinterpret_cast<const char*>(fontPath.c_str()), 16.0f, &fontConfig);
    }
    if (!io.FontDefault)
    {
        fontConfig.SizePixels = 16.0f;
        io.FontDefault = io.Fonts->AddFontDefault(&fontConfig);
    }
    baseFont = io.FontDefault;
    catalogFonts.clear();
    catalogFontStates.clear();
    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ImGui::DestroyContext(context);
        context = nullptr;
        RetireInput();
        window = nullptr;
        return false;
    }
    baseStyle = ImGui::GetStyle();
    uiScale = std::clamp(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd), 0.75f, 3.0f);
    layoutScale = uiScale; // Migrate an older DPI-only saved window once.
    standalone_ui::Initialize(single_module::Self());
    wchar_t savedScale[32]{};
    const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    if (!gFirstLaunchChecked)
    {
        gFirstLaunchChecked = true;
        gFirstLaunchPending = !preferences.empty() && !GetPrivateProfileIntW(
            L"Overlay", L"FirstLaunchMenuShown", 0, preferences.c_str());
    }
    GetPrivateProfileStringW(L"Overlay", L"LayoutScale", L"", savedScale,
        static_cast<DWORD>(std::size(savedScale)), preferences.c_str());
    wchar_t* scaleEnd = nullptr;
    const float parsedScale = wcstof(savedScale, &scaleEnd);
    if (scaleEnd != savedScale && *scaleEnd == L'\0' && std::isfinite(parsedScale)
        && parsedScale >= 0.125f && parsedScale <= 32.0f)
        layoutScale = parsedScale;
    layoutPrepared = false;
    ImGui::GetStyle() = baseStyle;
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    ImGui::GetStyle().FontScaleDpi = uiScale;
    ImGui::GetStyle().MouseCursorScale = 1.0f;
    firstFrame = GetTickCount64();
    if (!gVisible.load(std::memory_order_acquire))
        gActiveWindow.store(hwnd, std::memory_order_release);
    return true;
}

void PlatformState::RetireInput()
{
    if (!inputAttached) return;
    inputAttached = false;
    // DetachWindow retains only the forwarding thunk while window-thread
    // button-up/cancellation is still needed to release owned mouse capture.
    DetachWindow(window);
}

void PlatformState::Shutdown()
{
    RetireInput();
    if (!context) return;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (previous == context) previous = nullptr;
    ImGui::SetCurrentContext(context);
    InternalScope internal;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    context = nullptr;
    baseFont = nullptr;
    catalogFonts.clear();
    catalogFontStates.clear();
    ImGui::SetCurrentContext(previous);
    window = nullptr;
}

// A catalog's font: the Latin UI font with the first installed face the catalog
// lists merged over it. Each catalog gets its own, so languages that share
// characters (Chinese, Japanese) keep their own glyph forms. Built outside a
// frame on first display; used only if it draws every glyph the catalog has.
ImFont* PlatformState::CatalogFont(size_t index)
{
    const size_t count = l10n::CatalogCount();
    if (catalogFontStates.size() != count)
    {
        catalogFonts.assign(count, nullptr);
        catalogFontStates.assign(count, 0);
    }
    if (index >= count) return nullptr;
    if (catalogFontStates[index]) return catalogFonts[index];
    const auto& catalog = l10n::CatalogAt(index);
    auto& io = ImGui::GetIO();
    ImFont* font = baseFont;
    std::wstring used = L"UI font";
    if (catalog.fontCount)
    {
        font = nullptr;
        used.clear();
        wchar_t windowsPath[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windowsPath, MAX_PATH);
        const auto fonts = length && length < MAX_PATH
            ? std::filesystem::path(windowsPath)/L"Fonts" : std::filesystem::path();
        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        if (!fonts.empty())
        {
            const auto latin = (fonts/L"segoeui.ttf").u8string();
            font = io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(latin.c_str()), 16.0f, &config);
        }
        if (!font)
        {
            config.SizePixels = 16.0f;
            font = io.Fonts->AddFontDefault(&config);
        }
        for (size_t i = 0; font && !fonts.empty() && i < catalog.fontCount; ++i)
        {
            const auto path = fonts/catalog.fonts[i].file;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) continue;
            ImFontConfig merge;
            merge.Flags |= ImFontFlags_NoLoadError;
            merge.MergeMode = true;
            merge.DstFont = font;
            merge.FontNo = catalog.fonts[i].face;
            const auto file = path.u8string();
            if (io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(file.c_str()), 16.0f, &merge))
            {
                used = catalog.fonts[i].file;
                break;
            }
        }
    }
    bool covers = font != nullptr;
    for (size_t i = 0; covers && i < catalog.glyphCount; ++i)
        covers = font->IsGlyphInFont(static_cast<ImWchar>(catalog.glyphs[i]));
    if (!covers && font && font != baseFont) io.Fonts->RemoveFont(font);
    catalogFonts[index] = covers ? font : nullptr;
    catalogFontStates[index] = covers ? 1 : 2;
    wchar_t line[256]{};
    swprintf_s(line, L"UI language %S: %s (%s)", catalog.code,
        covers ? L"font ready" : L"no installed font draws every character; the menu stays English",
        used.empty() ? L"no listed font found" : used.c_str());
    single_module::Log(line);
    return catalogFonts[index];
}

// Before ImGui::NewFrame: T() and the default font follow the selected catalog
// once its font is ready, else English.
void PlatformState::PrepareLanguage()
{
    LoadLanguage();
    const l10n::Catalog* selected = l10n::Selected();
    ImFont* font = nullptr;
    for (size_t i = 0; selected && i < l10n::CatalogCount(); ++i)
        if (&l10n::CatalogAt(i) == selected) font = CatalogFont(i);
    l10n::SetDisplayed(font ? selected : nullptr);
    ImGui::GetIO().FontDefault = font ? font : baseFont;
}

bool PlatformState::WantsFrame() const noexcept
{
    if (!single_module::OverlayAllowed())
    {
        if (gVisible.load(std::memory_order_acquire)) SetVisible(false, window);
        mouse_route::Refresh(window, false); // the closed menu releases the mouse route
        return false;
    }
    // The edge state is shared with window/raw messages, so polling cannot
    // toggle twice for a single press. It also covers custom input pumps.
    const bool foreground = context && inputAttached && Foreground(window);
    // Beside OptiScaler's menu: its menu key, and only while a DLSS feature runs.
    const bool shared = shared_menu::Active();
    if (shared) shared_menu::RefreshKey();
    const bool ready = shared_menu::RefreshReady();
    const int sharedKey = shared_menu::Key();
    if (foreground)
    {
        InternalScope internal;
        // OptiScaler's menu opens on its key, so this one does not open by itself.
        if (gFirstLaunchPending && !shared)
        {
            SetVisible(true, window);
            if (gVisible.load(std::memory_order_acquire))
            {
                gFirstLaunchPending = false;
                gFirstLaunchSavePending = true;
            }
        }
        if (sharedKey) shared_menu::PollKey(window, sharedKey);
        else if (gMenuKey.Binding())
        {
            for (int key = VK_BACK; key < 255 && gMenuKey.Binding(); ++key)
                if (key == VK_ESCAPE || MenuHotkey::Bindable(key))
                    ProcessKey(window, key, (GetAsyncKeyState(key) & 0x8000) != 0);
        }
        else
        {
            const int key = gMenuKey.Key();
            ProcessKey(window, key, (GetAsyncKeyState(key) & 0x8000) != 0);
        }
    }
    PollKeybindKeys(window, foreground, shared || sharedKey != 0, sharedKey, d3d11); // keybinds.1
    if (context && inputAttached) mouse_route::Refresh(window, foreground);
    if (!ready) return false;
    // Beside OptiScaler's menu the startup hint follows the first DLSS feature.
    const uint64_t hintStart = shared ? shared_menu::gReadySince.load(std::memory_order_acquire) : firstFrame;
    float split = 0.0f;
    bool held = false;
    return context && inputAttached && IsWindow(window) && (gVisible.load(std::memory_order_acquire)
        || gPinned.load(std::memory_order_acquire) || (standalone_ui::PinnedFps() && !shared && !d3d11)
        || (standalone_ui::PinnedLatency() && !shared && !d3d11) // latmon.2
        || standalone_ui::KeybindFrameWanted()
        || GetTickCount64() - hintStart < 7000 || CompareLabels(split, held));
}

bool PlatformState::NewFrame(uint32_t outputWidth, uint32_t outputHeight)
{
    InternalScope internal;
    const bool visible = gVisible.load(std::memory_order_acquire);
    if (visible && !inputWasVisible)
    {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        shared_menu::gButtons = 0;
    }
    inputWasVisible = visible;
    std::deque<InputMessage> messages;
    {
        std::lock_guard lock(gInputMutex);
        const auto found = gWindows.find(window);
        if (found != gWindows.end()) messages.swap(found->second.messages);
    }
    for (const auto& input : messages)
    {
        bool down = false;
        const int button = MouseButton(input.message, input.wparam, down);
        if (input.fromHook)
        {
            // Position first, so the button or wheel applies where it happened.
            auto& io = ImGui::GetIO();
            if (const auto position = mouse_route::Map(static_cast<uint64_t>(input.lparam), io.DisplaySize))
                io.AddMousePosEvent(position->x, position->y);
            if (button >= 0)
            {
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMouseButtonEvent(button, down);
            }
            else if (input.message == WM_MOUSEWHEEL)
                io.AddMouseWheelEvent(0.0f, float(GET_WHEEL_DELTA_WPARAM(input.wparam)) / float(WHEEL_DELTA));
            else if (input.message == WM_MOUSEHWHEEL)
                io.AddMouseWheelEvent(-float(GET_WHEEL_DELTA_WPARAM(input.wparam)) / float(WHEEL_DELTA), 0.0f);
            continue;
        }
        if (button >= 0)
        {
            // Native capture was handled synchronously by WindowProc. Calling
            // the Win32 backend's button handler here would attempt capture on
            // the wrong thread, or release capture that belongs to the game.
            ImGui::GetIO().AddMouseSourceEvent(input.mouseSource);
            ImGui::GetIO().AddMouseButtonEvent(button, down);
        }
        else ImGui_ImplWin32_WndProcHandler(window, input.message, input.wparam, input.lparam);
    }
    auto& io = ImGui::GetIO();
    // A closed menu (startup hint, pinned views) stays on screen during play.
    // The backend still reads the cursor while the game is focused, so take no
    // mouse, draw no cursor and leave the game's cursor shape alone.
    // Beside an open OptiScaler menu, OptiScaler draws the cursor and owns its shape.
    const bool shared = visible && shared_menu::Active() && Foreground(window);
    // On XeFG's output this menu covers OptiScaler's cursor, so over this menu the cursor is ours.
    const bool optiScalerCursor =
        shared && shared_menu::OptiScalerCursor() && !(afterFrameGeneration && io.WantCaptureMouse);
    io.MouseDrawCursor = visible && !optiScalerCursor;
    if (visible) io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    if (optiScalerCursor) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_NewFrame();
    // The backend reads the cursor only while it tracks no window area, so the
    // open menu reads it here: the hook's last cursor when the hook drives the
    // mouse, else the cursor itself. Beside OptiScaler, PollMouse reads it and
    // the buttons; so it does on a window that gets no mouse messages while the
    // hook does not serve it (not installed yet, or refused).
    const bool polled = shared || (visible && Foreground(window)
        && mouse_route::Starved(window) && !mouse_route::Serving(window));
    if (visible && Foreground(window))
    {
        if (mouse_route::Serving(window))
        {
            if (const auto position = mouse_route::Map(mouse_route::gCursor.load(std::memory_order_acquire), io.DisplaySize))
                io.AddMousePosEvent(position->x, position->y);
        }
        else if (!polled) PollCursor(window, io);
    }
    if (polled) shared_menu::PollMouse(window, io);
    const auto scaling = CalculateOverlayScaling(outputWidth, outputHeight,
        io.DisplaySize.x, io.DisplaySize.y, ImGui_ImplWin32_GetDpiScaleForHwnd(window));
    if (!scaling.valid) return false;
    io.DisplayFramebufferScale = ImVec2(scaling.framebufferX, scaling.framebufferY);
    if (scaling.logical != uiScale)
    {
        // Start from unscaled metrics so repeated resolution changes cannot
        // accumulate rounding damage to padding, borders or control sizes.
        uiScale = scaling.logical;
        ImGui::GetStyle() = baseStyle;
        ImGui::GetStyle().ScaleAllSizes(uiScale);
        ImGui::GetStyle().FontScaleDpi = uiScale;
    }
    // Cursor size is independent of the panel's resolution scaling. Undo
    // framebuffer magnification as well, so virtualized client sizes do not
    // enlarge the software cursor at the final output resolution.
    ImGui::GetStyle().MouseCursorScale = 1.0f
        / std::max(scaling.framebufferX, scaling.framebufferY);
    PrepareLanguage();
    ImGui::NewFrame();
    return true;
}

void PlatformState::Draw()
{
    if (const int key = gMenuKey.TakePendingSave())
    {
        const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        const auto value = std::to_wstring(key);
        gShortcutSaveFailed = path.empty() || !WritePrivateProfileStringW(
            L"Overlay", L"ToggleKey", value.c_str(), path.c_str());
    }
    // keybinds.1: the key WantsFrame read, applied through the menu's own write before the menu draws it.
    standalone_ui::RunQueuedKeybind();
    const bool visible = gVisible.load(std::memory_order_acquire);
    bool open = visible;
    const bool pinned = !open && gPinned.load(std::memory_order_acquire);
    const float margin = 16.0f*uiScale;
    float cornerBottom = margin; // the startup hint goes below the FPS readout
    const float inset = 8.0f*uiScale;
    float pillTop = inset; // the Compare pills go just below the FPS readout
    if (shared_menu::Active())
    {
        // Beside OptiScaler its frame generation and FPS overlay (top left) are
        // OptiScaler's, so the DLSS FG readout stays off; the Compare pills keep
        // the place they would have below it.
        pillTop = inset + ImGui::GetTextLineHeight() + 4.0f*uiScale + ImGui::GetStyle().ItemSpacing.y;
    }
    // The FPS readout reports frame generation's rates, which a DirectX 11 game has none of.
    else if (!open && !pinned && (standalone_ui::PinnedFps() || standalone_ui::PinnedLatency()) && !d3d11)
    {
        // The FPS readout first; latmon.2's latency readout below it, or in its place.
        float top = inset;
        if (standalone_ui::PinnedFps())
        {
            const float bottom = Pill("##RTX40MFG pinned fps", ImVec2(inset, top), ImVec2(0.0f, 0.0f), uiScale,
                [] { standalone_ui::DrawPinnedFps(); });
            top = bottom + ImGui::GetStyle().ItemSpacing.y;
        }
        if (standalone_ui::PinnedLatency())
        {
            const float bottom = Pill("##RTX40MFG pinned latency", ImVec2(inset, top), ImVec2(0.0f, 0.0f), uiScale,
                [] { standalone_ui::DrawPinnedLatency(); });
            top = bottom + ImGui::GetStyle().ItemSpacing.y;
        }
        cornerBottom = std::max(margin, top);
        pillTop = top;
    }
    // DLSS 5 Compare: the divider over the finished image, for every Hook Method,
    // and which side is which in the same pills at the screen's left and right
    // edges, just below the FPS readout ("DLSS 5 On" on the left with Swap
    // sides); "DLSS 5 Off" on both while the compare key is held.
    float split = 0.0f;
    bool held = false, swapped = false;
    if (CompareLabels(split, held, &swapped))
    {
        const auto display = ImGui::GetIO().DisplaySize;
        if (!held)
        {
            // Visible but quiet: a white line (2 px at 1080p, 3 at 4K) in soft dark edges.
            const float x = std::round(split*display.x);
            const float core = std::max(1.0f, std::round(display.x/1280.0f));
            auto* draw = ImGui::GetBackgroundDrawList();
            draw->AddRectFilled(ImVec2(x - 0.5f*core - 1.0f, 0.0f), ImVec2(x + 0.5f*core + 1.0f, display.y), IM_COL32(0, 0, 0, 140));
            draw->AddRectFilled(ImVec2(x - 0.5f*core, 0.0f), ImVec2(x + 0.5f*core, display.y), IM_COL32(255, 255, 255, 230));
            dlss5::NoteOverlayDivider();
        }
        const char* off = l10n::T("DLSS 5 Off");
        const char* on = l10n::T("DLSS 5 On");
        const char* left = !held && swapped ? on : off;
        const char* right = held ? off : swapped ? off : on;
        const float bottom = Pill("##RTX40MFG compare left", ImVec2(inset, pillTop), ImVec2(0.0f, 0.0f), uiScale,
            [left] { ImGui::TextUnformatted(left); });
        Pill("##RTX40MFG compare right", ImVec2(display.x - inset, pillTop), ImVec2(1.0f, 0.0f), uiScale,
            [right] { ImGui::TextUnformatted(right); });
        cornerBottom = std::max(cornerBottom, bottom + ImGui::GetStyle().ItemSpacing.y);
    }
    // keybinds.1: what a key just did, for 1.5 s at the top centre.
    if (const char* note = standalone_ui::KeybindNote())
        Pill("##RTX40MFG keybind note", ImVec2(0.5f*ImGui::GetIO().DisplaySize.x, inset), ImVec2(0.5f, 0.0f),
            uiScale, [note] { ImGui::TextUnformatted(note); });
    if (open || pinned)
    {
        const bool rescaleLayout = !layoutPrepared || std::abs(layoutScale-uiScale) > 0.0001f;
        const auto display = ImGui::GetIO().DisplaySize;
        const ImVec2 maximum(std::max(32.0f, display.x-2.0f*margin), std::max(32.0f, display.y-2.0f*margin));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(360.0f*uiScale, maximum.x), std::min(180.0f*uiScale, maximum.y)), maximum);
        if (rescaleLayout)
        {
            ImVec2 size(570.0f*uiScale, 610.0f*uiScale), position(24.0f*uiScale, 24.0f*uiScale);
            // OptiScaler's menu opens at the top left; beside it this panel starts at the top right.
            const bool shared = shared_menu::Active();
            if (shared) position.x = display.x - size.x - 24.0f*uiScale;
            const float ratio = uiScale/layoutScale;
            const ImGuiWindowSettings* saved = ImGui::FindWindowSettingsByID(ImHashStr(PanelTitle()));
            if (!saved && !shared) saved = ImGui::FindWindowSettingsByID(ImHashStr(kLegacyPanelName));
            if (const ImGuiWindow* panel = ImGui::FindWindowByName(PanelTitle()))
            {
                size = ImVec2(panel->SizeFull.x*ratio, panel->SizeFull.y*ratio);
                position = ImVec2(panel->Pos.x*ratio, panel->Pos.y*ratio);
            }
            else if (saved)
            {
                size = ImVec2(float(saved->Size.x)*ratio, float(saved->Size.y)*ratio);
                position = ImVec2(float(saved->Pos.x)*ratio, float(saved->Pos.y)*ratio);
            }
            size.x = std::clamp(size.x, std::min(360.0f*uiScale, maximum.x), maximum.x);
            size.y = std::clamp(size.y, std::min(180.0f*uiScale, maximum.y), maximum.y);
            position.x = std::clamp(position.x, 0.0f, std::max(0.0f, display.x-size.x));
            position.y = std::clamp(position.y, 0.0f, std::max(0.0f, display.y-size.y));
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        }
        if (pinned) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
        const auto flags = pinned ? ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse : ImGuiWindowFlags_None;
        if (ImGui::Begin(PanelTitle(), pinned ? nullptr : &open, flags))
        {
            // Keep the pin control in the title bar so scrolled settings can
            // be pinned without returning to the top of the content.
            if (!pinned)
            {
                auto* panel = ImGui::GetCurrentWindow();
                const auto cursor = ImGui::GetCursorScreenPos();
                const char* pin = l10n::Label("Pin");
                const float width = ImGui::CalcTextSize(pin, nullptr, true).x + 2*ImGui::GetStyle().FramePadding.x;
                const auto title = panel->TitleBarRect();
                ImGui::PushClipRect(title.Min, title.Max, false);
                // SmallButton has no vertical padding: centre its text line in the title bar.
                ImGui::SetCursorScreenPos(ImVec2(title.Max.x-width-title.GetHeight()-8*uiScale,
                    title.Min.y + std::max(0.0f, 0.5f*(title.GetHeight()-ImGui::GetTextLineHeight()))));
                if (ImGui::SmallButton(pin))
                {
                    gPinned.store(true, std::memory_order_release);
                    open = false;
                }
                ImGui::SetItemTooltip(l10n::T("Keeps this panel on screen, see-through and click-through.\n"
                    "%s brings the menu back."), KeyLabel().c_str());
                ImGui::SetCursorScreenPos(cursor);
                ImGui::PopClipRect();
            }
            // The settings scroll; the footer keeps the menu key and status in view.
            const auto& style = ImGui::GetStyle();
            float footer = 2.0f*style.ItemSpacing.y + 1.0f + ImGui::GetFrameHeight();
            if (gShortcutSaveFailed) footer += 2.0f*ImGui::GetTextLineHeightWithSpacing();
            if (gLanguageSaveFailed) footer += 2.0f*ImGui::GetTextLineHeightWithSpacing();
            if (ImGui::BeginChild("##settings", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened))
                standalone_ui::Draw(d3d11);
            ImGui::EndChild();
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            if (pinned)
                ImGui::Text(l10n::T("Pinned  |  %s brings the menu back"), KeyLabel().c_str());
            else
            {
                ImGui::Text(l10n::T("Menu key: %s"), KeyLabel().c_str());
                ImGui::SameLine();
                if (shared_menu::Key())
                    ImGui::TextDisabled("%s", l10n::T("Shared with OptiScaler's menu"));
                else if (gMenuKey.Binding())
                {
                    ImGui::TextUnformatted(l10n::T("Press a key... Esc cancels"));
                    ImGui::SameLine();
                    if (ImGui::Button(l10n::Label("Cancel"))) gMenuKey.CancelBinding();
                }
                else if (ImGui::Button(l10n::Label("Change key")))
                {
                    standalone_ui::CancelKeybindCapture(); // keybinds.1: one key binds at a time
                    gMenuKey.BeginBinding();
                }
                ImGui::SameLine();
                DrawLanguagePicker();
            }
            standalone_ui::DrawFooterStatus();
            if (gShortcutSaveFailed)
                ImGui::TextWrapped("%s", l10n::T("The new shortcut works for this session, but could not be saved."));
            if (gLanguageSaveFailed)
                ImGui::TextWrapped("%s", l10n::T("The language works for this session, but could not be saved."));
        }
        ImGui::End();
        if (pinned) ImGui::PopStyleVar();
        if (gFirstLaunchSavePending)
        {
            // Persist only after the full menu has reached Draw, never merely
            // on DLL load or a background/splash swapchain attachment. A failed
            // write leaves the next launch eligible, without per-frame I/O.
            gFirstLaunchSavePending = false;
            const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
            if (!preferences.empty()) WritePrivateProfileStringW(
                L"Overlay", L"FirstLaunchMenuShown", L"1", preferences.c_str());
        }
        if (rescaleLayout)
        {
            layoutScale = uiScale;
            layoutPrepared = true;
            // Save the dimensions with their coordinate scale, so relaunching
            // at a different resolution rescales a saved panel exactly once.
            if (ImGui::GetIO().IniFilename)
            {
                ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
                const auto preferences = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
                const auto value = std::to_wstring(layoutScale);
                WritePrivateProfileStringW(L"Overlay", L"LayoutScale", value.c_str(), preferences.c_str());
            }
        }
        if (visible && !open) SetVisible(false, window);
    }
    else if (GetTickCount64() - (shared_menu::Active()
        ? shared_menu::gReadySince.load(std::memory_order_acquire) : firstFrame) < 7000)
    {
        ImGui::SetNextWindowPos(ImVec2(margin, cornerBottom), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("##RTX40MFG startup", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::Text(l10n::T("MFG Unlocker  |  %s opens the menu"), KeyLabel().c_str());
        }
        ImGui::End();
    }
    ImGui::Render();
    gWantMouse.store(open, std::memory_order_release);
    gWantKeyboard.store(open, std::memory_order_release);
}

FARPROC ResolveBoundedInput(HMODULE module, LPCSTR name, FARPROC original) noexcept
{
    if (!name || !original || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu) return original;
    // Only wrap the public entry from the actual pinned System32 user32 image.
    wchar_t path[MAX_PATH]{};
    wchar_t system[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH) || !GetSystemDirectoryW(system, MAX_PATH)) return original;
    wcscat_s(system, L"\\user32.dll");
    if (_wcsicmp(path, system) || !slots::ImageEntry(module, reinterpret_cast<void*>(original))) return original;
    void* thunk = nullptr;
    if (!strcmp(name, "GetAsyncKeyState")) thunk = HookFamily<AsyncKeyTag, SHORT, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "GetKeyState")) thunk = HookFamily<KeyTag, SHORT, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "GetKeyboardState")) thunk = HookFamily<KeyboardTag, BOOL, PBYTE>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "SetCursorPos")) thunk = HookFamily<CursorPositionTag, BOOL, int, int>::Forwarder(reinterpret_cast<void*>(original));
    else if (!strcmp(name, "ClipCursor")) thunk = HookFamily<ClipTag, BOOL, const RECT*>::Forwarder(reinterpret_cast<void*>(original));
    return thunk ? reinterpret_cast<FARPROC>(thunk) : original;
}

bool PublishBoundedInputOriginal(const char* name, void* target, void* trampoline) noexcept
{
    if (!strcmp(name,"GetAsyncKeyState")) return HookFamily<AsyncKeyTag,SHORT,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"GetKeyState")) return HookFamily<KeyTag,SHORT,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"GetKeyboardState")) return HookFamily<KeyboardTag,BOOL,PBYTE>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"SetCursorPos")) return HookFamily<CursorPositionTag,BOOL,int,int>::PublishForwarderOriginal(target,trampoline);
    if (!strcmp(name,"ClipCursor")) return HookFamily<ClipTag,BOOL,const RECT*>::PublishForwarderOriginal(target,trampoline);
    return false;
}

bool PrepareBoundedInput(HMODULE user32, slots::Batch& batch) noexcept
{
    HMODULE executable = GetModuleHandleW(nullptr);
    bool refused = false;
    const bool walked = slots::VisitImports(executable, [&](const char* name, void** address) {
        FARPROC original = GetProcAddress(user32, name);
        FARPROC replacement = ResolveBoundedInput(user32, name, original);
        if (replacement == original) return true;
        if (*address != reinterpret_cast<void*>(original))
        {
            single_module::Log(L"D3D12 UI: foreign input IAT slot retained; window messages remain available");
            return true;
        }
        const bool added=batch.Add(executable, address, reinterpret_cast<void*>(original), reinterpret_cast<void*>(replacement), name, &PublishBoundedInputOriginal);
        if (!added) {
            MEMORY_BASIC_INFORMATION memory{};VirtualQuery(address,&memory,sizeof(memory));
            wchar_t line[256]{};
            swprintf_s(line,L"MFG_PROXY_UI input preparation failed symbol=%hs slotRva=0x%llX protection=0x%lX",name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(executable)),memory.Protect);
            single_module::Log(line);
            refused = true;
        }
        return added;
    });
    if (walked || refused) return walked;
    // An unreadable import directory (seen once some games run) publishes no input slot: the menu takes its input
    // from window messages, as with a packed executable that imports no input functions.
    batch.count = 0;
    single_module::Log(L"MFG_PROXY_UI executable import directory unreadable; menu input from window messages only");
    return true;
}

void OnBoundedInputActivated() noexcept
{
    HookFamily<AsyncKeyTag, SHORT, int>::MarkAllPreparedActivated();
    HookFamily<KeyTag, SHORT, int>::MarkAllPreparedActivated();
    HookFamily<KeyboardTag, BOOL, PBYTE>::MarkAllPreparedActivated();
    HookFamily<CursorPositionTag, BOOL, int, int>::MarkAllPreparedActivated();
    HookFamily<ClipTag, BOOL, const RECT*>::MarkAllPreparedActivated();
}

void RecordFailure(const wchar_t* reason) noexcept
{
    if (gRenderFailures.fetch_add(1, std::memory_order_relaxed) < 12)
        single_module::Log(reason);
}

void ReadStatus(MfgSingleModuleStatus& status) noexcept
{
    status.dxgiHooked = gDxgiHooked.load();
    status.vulkanHooked = gVulkanHooked.load();
    status.overlayVisible = gVisible.load();
    status.renderFailures = gRenderFailures.load();
    const int sharedKey = shared_menu::Active() ? shared_menu::gKey.load(std::memory_order_acquire) : 0;
    status.shortcutVirtualKey = static_cast<uint32_t>(sharedKey > 0 ? sharedKey : gMenuKey.Key());
    status.shortcutBindingActive = gMenuKey.Binding();
    status.dxgiFrames = gDxgiFrames.load();
    status.vulkanFrames = gVulkanFrames.load();
    status.renderedFrames = gRenderedFrames.load();
}
}
