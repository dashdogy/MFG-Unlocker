#pragma once

#include "optiscaler_plugin.h"

// menutab.2: RTXMFG's tab in OptiScaler's own menu, through OptiScaler's plugin API (optiscaler_plugin.h). In the
// hidden UI mode RTXMFG draws no menu and wraps no factory, swap chain or input; the tab offers the DLSS presets and
// DLSS 5 instead, drawn by OptiScaler's ImGui (standalone_ui::DrawOptiScalerTab). OptiScaler hands the API only to an
// .asi it loads from its plugins folder, so a sibling proxy gets no tab.
namespace optiscaler_menu_tab
{
// OptiPluginInit. OptiScaler calls it right after InitializeASI, inside its DllMain (the loader lock is held).
// Accepted only from the identified OptiScaler: the table and every entry RTXMFG calls lie in its image. Registers the
// tab at once when the UI mode is hidden. Returns false when refused (OptiScaler then keeps nothing).
// pluginabi.1: upstream's layout (optiscaler_plugin.h) is used as given; the version 2 table is adapted to it.
bool AcceptApi(const OptiPluginApi* api, OptiPlugin* plugin) noexcept;

// The accepted table was our fork's version 2 layout, adapted.
bool Fork2Layout() noexcept;

// The API was accepted: the saved DLSS presets and DLSS 5 apply in the hidden mode too (EffectiveControl).
bool Offered() noexcept;

// pluginprov.1: registers a provider where the accepted API has RegisterProvider and its entry lies in the identified
// OptiScaler image. False otherwise (older OptiScaler, our fork's table), and the shim file is used as before.
bool RegisterProvider(const char* name, OptiProviderKind kind, OptiGetProcFn getProc, void* userData) noexcept;

// The accepted API, for the tab's draw callback; null before AcceptApi.
const OptiPluginApi* Api() noexcept;

enum class Change
{
    eNone,
    eRegistered,
    eRefused
};

// Control worker: registers the tab once the UI mode becomes hidden (a live switch from "shared" or "rtxmfg"). The API
// has no way to remove a tab, so once registered it stays and keeps working in every mode.
Change Update() noexcept;
bool Registered() noexcept;
}
