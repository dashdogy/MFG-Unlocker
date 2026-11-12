#pragma once
#include "single_module.h"
#include <cstdint>

namespace single_overlay::install
{
enum class State : uint32_t
{
    Uninitialized = 0,
    Preparing = 1,
    Active = 2,
    Unavailable = 3
};

enum class Reason : uint32_t
{
    None = 0,
    DisabledByBuild,       // Diagnostic build without integrated interception.
    NotBackendOwner,       // Duplicate or legacy-suppressed copy.
    WorkerStartFailed,     // Reserved legacy ABI value; no UI worker is used.
    SystemDxgiUnavailable, // The pinned System32 dxgi.dll could not be loaded.
    MissingExport,         // A required factory-creation export is absent.
    TargetUnowned,         // A discovered code entry is not owned by the pinned image.
    PreexistingDetour,     // Reserved legacy ABI value; native code is preserved.
    ProbeFailed,           // Reserved legacy ABI value; no graphics probe is used.
    PreparationFailed,     // Reserved legacy ABI value.
    ActivationFailed,      // Optional input publication failed; menu drawing stays off.
    FaultInjected          // Test-fault build: an injected failure point.
};

// The coordinator's terminal state after RequestInstall. Active means the
// first real factory returned and received an owned proxy. Preparing means armed.
void FactoryReady() noexcept;
void VulkanReady() noexcept;
// dx11menu.1: a D3D11CreateDeviceAndSwapChain swapchain received the menu's proxy (no factory proxy on that route).
void D3D11Ready() noexcept;
bool PrepareInput() noexcept;
bool InputReady() noexcept;
State Current() noexcept;
Reason UnavailableReason() noexcept;

// Number of optional application input-IAT data slots published. Native tables are never changed. The UI suspends no
// process threads; SuspendedThreadCount remains zero for ABI compatibility.
uint32_t ActivatedTargetCount() noexcept;
uint32_t SuspendedThreadCount() noexcept;

// Arms the main image's factory imports without graphics work or waiting.
// Factory callbacks wrap returned objects outside loader callbacks. No graphics
// creation, native table publication, installer waits or discovery worker occur.
void RequestInstall() noexcept;
}
