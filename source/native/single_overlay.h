#pragma once
#include "single_module.h"

namespace single_overlay
{
void ArmFactoryGateway() noexcept;
// The existing backend boundary rechecks application imports, and scopes the
// interposer factory relays to a verified renderer DLL that called slInit.
void BeforeStreamlineInit(const void* caller) noexcept;
void InstallKnownModules() noexcept;
FARPROC ResolveProc(HMODULE module, LPCSTR name, FARPROC original) noexcept;
// Streamline manual hooking (slUpgradeInterface). OwnsInterface: the object
// passed in is already this layer's proxy (checked before the upgrade).
// WrapUpgradedInterface: wraps the upgraded DXGI factory in *output so its
// swapchains get the menu; true when *output was replaced.
bool OwnsInterface(void* object) noexcept;
bool WrapUpgradedInterface(void** output) noexcept;
// Logging only: what an slUpgradeInterface argument is and which
// module implements it.
void DescribeInterface(void* object, wchar_t* text, size_t count) noexcept;
void ReadStatus(MfgSingleModuleStatus& status) noexcept;
}
