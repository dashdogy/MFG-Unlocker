#include "optiscaler_menu_tab.h"
#include "optiscaler_host.h"
#include "optiscaler_plugin_fork2.h"
#include "standalone_ui.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace optiscaler_menu_tab
{
namespace
{
constexpr char kLabel[] = "MFG Unlocker";
std::atomic<const OptiPluginApi*> gApi{nullptr};
std::atomic<OptiPlugin*> gPlugin{nullptr};
// Registering is tried once: from AcceptApi when the mode is hidden then (the default once the API is offered,
// tabdefault.1), else from the worker when it becomes hidden.
std::atomic<bool> gClaimed{false};
std::atomic<bool> gRegistered{false};
std::atomic<bool> gRefused{false};
std::atomic<bool> gReported{false};
// pluginabi.1: a table in our fork's version 2 layout is copied here and handed to the tab as gAdapted, upstream's
// layout. Both are written before gApi publishes gAdapted.
OptiPluginApiFork2 gFork2{};
OptiPluginApi gAdapted{};
std::atomic<bool> gFork2Adapted{false};

void Draw(void*)
{
    if (const OptiPluginApi* api = gApi.load(std::memory_order_acquire))
        standalone_ui::DrawOptiScalerTab(*api);
}

bool InHost(const void* address) noexcept
{
    return address && optiscaler_host::IsHostAddress(address);
}

template<class Fn> bool InHostFn(Fn entry) noexcept
{
    return entry && InHost(reinterpret_cast<const void*>(entry));
}

// The fork's Combo greys out items with a bit mask (ComboEx); upstream's takes an array.
bool Fork2Combo(const char* label, int32_t* current, const char* const* items, int32_t itemCount,
    const bool* itemDisabled)
{
    uint32_t disabled = 0;
    for (int32_t i = 0; itemDisabled && i < itemCount && i < 32; ++i)
        if (itemDisabled[i]) disabled |= 1u << i;
    return gFork2.ComboEx(label, current, items, itemCount, disabled);
}

// Every entry the tab calls lies in the identified OptiScaler image; Table is either layout.
template<class Table> bool TabEntriesInHost(const Table& api) noexcept
{
    return InHostFn(api.RegisterTab) && InHostFn(api.Section) && InHostFn(api.Separator) && InHostFn(api.SameLine)
        && InHostFn(api.Disabled) && InHostFn(api.ItemTooltip) && InHostFn(api.Text) && InHostFn(api.TextDisabled)
        && InHostFn(api.TextColored) && InHostFn(api.Button) && InHostFn(api.Checkbox) && InHostFn(api.SliderFloat)
        && InHostFn(api.SliderInt);
}

enum class Layout
{
    eRefused,
    eUpstream,
    eFork2
};

// The table lies in the identified OptiScaler image. Upstream's layout has every field up to Log; later fields are
// optional (OPTI_API_HAS). A version 2 table of the fork's size that fails the fork's checks is refused, never read as
// upstream's.
Layout Identify(const OptiPluginApi* api) noexcept
{
    if (!InHost(api)) return Layout::eRefused;
    if (api->version == OPTI_PLUGIN_FORK2_API_VERSION && api->size == sizeof(OptiPluginApiFork2))
    {
        OptiPluginApiFork2 fork{};
        std::memcpy(&fork, api, sizeof(fork));
        return TabEntriesInHost(fork) && InHostFn(fork.Combo) && InHostFn(fork.ComboEx)
                && InHostFn(fork.ItemDeactivatedAfterEdit)
            ? Layout::eFork2 : Layout::eRefused;
    }
    if (api->version < 1 || api->size < offsetof(OptiPluginApi, Log) + sizeof(api->Log)) return Layout::eRefused;
    const bool released = !api->ItemDeactivatedAfterEdit || InHostFn(api->ItemDeactivatedAfterEdit);
    return TabEntriesInHost(*api) && InHostFn(api->Combo) && released ? Layout::eUpstream : Layout::eRefused;
}

// The fork's entries at upstream's places; Combo goes through Fork2Combo, and GetMenuScale has no place.
const OptiPluginApi* Adapt(const OptiPluginApi* api) noexcept
{
    std::memcpy(&gFork2, api, sizeof(gFork2));
    const OptiPluginApiFork2& f = gFork2;
    OptiPluginApi& a = gAdapted;
    a.size = sizeof(OptiPluginApi);
    a.version = OPTI_PLUGIN_API_VERSION;
    a.RegisterTab = f.RegisterTab;
    a.Section = f.Section;
    a.Separator = f.Separator;
    a.SameLine = f.SameLine;
    a.Spacing = f.Spacing;
    a.Disabled = f.Disabled;
    a.ItemTooltip = f.ItemTooltip;
    a.ItemDeactivatedAfterEdit = f.ItemDeactivatedAfterEdit;
    a.Text = f.Text;
    a.TextDisabled = f.TextDisabled;
    a.TextColored = f.TextColored;
    a.Button = f.Button;
    a.Checkbox = f.Checkbox;
    a.SliderFloat = f.SliderFloat;
    a.SliderInt = f.SliderInt;
    a.Combo = &Fork2Combo;
    a.InputText = f.InputText;
    a.ColorEdit = f.ColorEdit;
    a.IsMenuOpen = f.IsMenuOpen;
    a.GetBool = f.GetBool;
    a.GetInt = f.GetInt;
    a.GetFloat = f.GetFloat;
    a.GetString = f.GetString;
    a.SetBool = f.SetBool;
    a.SetInt = f.SetInt;
    a.SetFloat = f.SetFloat;
    a.SetString = f.SetString;
    a.ResetValue = f.ResetValue;
    a.SaveSettings = f.SaveSettings;
    a.Log = f.Log;
    return &gAdapted;
}

void Register() noexcept
{
    const OptiPluginApi* api = gApi.load(std::memory_order_acquire);
    OptiPlugin* plugin = gPlugin.load(std::memory_order_acquire);
    if (!api || !plugin || gClaimed.exchange(true, std::memory_order_acq_rel)) return;
    if (api->RegisterTab(plugin, kLabel, nullptr, &Draw, nullptr))
        gRegistered.store(true, std::memory_order_release);
    else
        gRefused.store(true, std::memory_order_release);
}
}

bool AcceptApi(const OptiPluginApi* api, OptiPlugin* plugin) noexcept
{
    if (!plugin || gApi.load(std::memory_order_acquire)) return false;
    const Layout layout = Identify(api);
    if (layout == Layout::eRefused) return false;
    if (layout == Layout::eFork2)
    {
        api = Adapt(api);
        gFork2Adapted.store(true, std::memory_order_release);
    }
    gPlugin.store(plugin, std::memory_order_release);
    gApi.store(api, std::memory_order_release);
    // tabdefault.1: without "optiScalerUi" the mode is hidden from here on, so the tab registers now.
    optiscaler_host::NoteMenuTabOffered();
    if (optiscaler_host::UiHidden()) Register();
    return true;
}

bool RegisterProvider(const char* name, OptiProviderKind kind, OptiGetProcFn getProc, void* userData) noexcept
{
    const OptiPluginApi* api = gApi.load(std::memory_order_acquire);
    OptiPlugin* plugin = gPlugin.load(std::memory_order_acquire);
    if (!plugin || !OPTI_API_HAS(api, RegisterProvider) || !InHostFn(api->RegisterProvider)) return false;
    return api->RegisterProvider(plugin, name, kind, getProc, userData);
}

bool Fork2Layout() noexcept
{
    return gFork2Adapted.load(std::memory_order_acquire);
}

bool Offered() noexcept
{
    return gApi.load(std::memory_order_acquire) != nullptr;
}

const OptiPluginApi* Api() noexcept
{
    return gApi.load(std::memory_order_acquire);
}

Change Update() noexcept
{
    if (Offered() && optiscaler_host::UiHidden()) Register();
    // Reported once, whichever thread registered (AcceptApi runs inside OptiScaler's DllMain).
    if (gRegistered.load(std::memory_order_acquire) && !gReported.exchange(true, std::memory_order_acq_rel))
        return Change::eRegistered;
    if (gRefused.load(std::memory_order_acquire) && !gReported.exchange(true, std::memory_order_acq_rel))
        return Change::eRefused;
    return Change::eNone;
}

bool Registered() noexcept
{
    return gRegistered.load(std::memory_order_acquire);
}
}
