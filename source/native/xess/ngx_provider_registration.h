#pragma once
// pluginprov.1: XeMFG's DLSS-G provider registered through OptiScaler's plugin API (RegisterProvider), in place of the
// shim file in custom\<name>\dlssg. No Vulkan or NGX types, so the module's own units can include it.
namespace ngx_provider_registration
{
// The name OptiScaler lists it under; FGNvngxReplacement names it in OptiScaler.ini, as it does the shim's folder.
inline constexpr char kName[] = "MFGUnlocker";
inline constexpr wchar_t kNameW[] = L"MFGUnlocker";

// OptiGetProcFn: the entry points the shim exports, by the same names. Null for all of them while the provider can't
// run in this process, the gate the shim keeps in its DllMain, so OptiScaler falls back the same way.
void* GetProc(void* userData, const char* exportName) noexcept;

// OptiScaler accepted the registration.
void NoteRegistered() noexcept;
bool Registered() noexcept;
}
