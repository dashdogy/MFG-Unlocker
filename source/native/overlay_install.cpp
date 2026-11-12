#include "overlay_install.h"
#include "overlay_build.h"
#include "overlay_platform.h"
#include "overlay_native.h"
#include "single_overlay.h"
#include <atomic>
namespace single_overlay::install {
namespace {
std::atomic<State> state{State::Uninitialized};
std::atomic<Reason> reason{Reason::None};
std::atomic<uint32_t> inputs{0};
std::atomic<unsigned> inputState{0};
}
State Current() noexcept { return state.load(std::memory_order_acquire); }
Reason UnavailableReason() noexcept { return reason.load(std::memory_order_acquire); }
uint32_t ActivatedTargetCount() noexcept { return inputs.load(std::memory_order_acquire); }
uint32_t SuspendedThreadCount() noexcept { return 0; }
void RequestInstall() noexcept {
    State expected=State::Uninitialized;
    if (!state.compare_exchange_strong(expected,State::Preparing)) return;
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    reason.store(Reason::DisabledByBuild); state.store(State::Unavailable,std::memory_order_release);
#else
    ArmFactoryGateway();
#if MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
    single_module::Log(L"MFG_PROXY_UI comparison=skip-gpu-work proxies=retained input=retained uiResources=disabled uiDraw=disabled");
#endif
#endif
}
void FactoryReady() noexcept {
    const auto previous=state.exchange(State::Active,std::memory_order_acq_rel);
    if (previous!=State::Active)
        single_module::Log(L"MFG_PROXY_UI state=active boundary=real-factory-return nativeTableWrites=0");
}
void VulkanReady() noexcept {
    const auto previous=state.exchange(State::Active,std::memory_order_acq_rel);
    if (previous!=State::Active)
        single_module::Log(L"MFG_PROXY_UI state=active boundary=vulkan-swapchain-return nativeTableWrites=0 executablePatches=0");
}
void D3D11Ready() noexcept {
    const auto previous=state.exchange(State::Active,std::memory_order_acq_rel);
    if (previous!=State::Active)
        single_module::Log(L"MFG_PROXY_UI state=active boundary=d3d11-swapchain-return nativeTableWrites=0");
}
bool PrepareInput() noexcept {
#if !MFG_UNLOCK_OVERLAY_MENU_DRAW
    return true;
#else
    // OptiScaler mode: no RTXMFG menu, so no input imports are published.
    if (!single_module::OverlayAllowed()) return false;
    if (native::InsideLoader()) return false;
    unsigned expected=0;
    if (!inputState.compare_exchange_strong(expected,1,std::memory_order_acq_rel))
        return expected==2 || expected==1; // Window messages remain usable; never wait on a creator.
    const HMODULE user32=single_module::LoadSystemModule(L"user32.dll");
    slots::Batch batch;
    if (!user32||slots::Fault("input")||!PrepareBoundedInput(user32,batch)||!batch.Publish()) {
        reason.store(Reason::ActivationFailed);
        inputState.store(3,std::memory_order_release);
        single_module::Log(L"MFG_PROXY_UI input imports unavailable; original swapchain retained");
        return false;
    }
    inputs.store(static_cast<uint32_t>(batch.count),std::memory_order_release);
    OnBoundedInputActivated();
    inputState.store(2,std::memory_order_release);
    wchar_t line[128]{};swprintf_s(line,L"MFG_PROXY_UI input ready imports=%zu",batch.count);single_module::Log(line);
    return true;
#endif
}
bool InputReady() noexcept {
#if MFG_UNLOCK_OVERLAY_MENU_DRAW
    return inputState.load(std::memory_order_acquire)==2;
#else
    return true;
#endif
}
}
