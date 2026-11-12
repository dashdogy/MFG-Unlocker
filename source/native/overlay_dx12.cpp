#include "overlay_dx12.h"
#include "adapter_discovery.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "overlay_native.h"
#include "overlay_color.h"
#include "overlay_device_identity.h"
#include "present_counter.h"
#include "ui_input_coherence.h"
#include "dlss5.h"
#include "optiscaler_host.h"
#include "xess/xess_mod.h"
#include "xess/d3d12_host.h"
#include <backends/imgui_impl_dx12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace single_overlay::dx12
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT kSrvCount = 256;
std::atomic<uint64_t> gNextTelemetryOwner{1};
enum class RenderOutcome
{
    eNoWrite,
    eSubmitted,
    eSkippedPartialUpdate,
    eSkippedPartialUpdateWithResidual
};

struct Frame
{
    ComPtr<ID3D12Resource> exposedBuffer; // Retain the resource returned by this swapchain layer.
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fenceValue = 0;
};

struct State;
thread_local const State* gRenderingState = nullptr;
struct RenderScope
{
    const State* previous;
    explicit RenderScope(const State* state) noexcept : previous(gRenderingState)
    {
        gRenderingState = state;
    }
    ~RenderScope() { gRenderingState = previous; }
};

struct State
{
    std::recursive_mutex mutex;
    uint64_t telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
    IDXGISwapChain3* swapchain = nullptr; // Alias owned by the enclosing Session.
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12DescriptorHeap> srvs;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::vector<Frame> frames;
    std::array<bool, kSrvCount> srvUsed{};
    UINT srvStep = 0;
    UINT64 nextFence = 1;
    PlatformState platform;
    HWND presentationWindow = nullptr;
    std::atomic<uint64_t> presentationArea{0};
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool colorSpaceObserved = false;
    bool initialized = false;
    bool disabled = false;
    bool rendererInitialized = false;
    bool overlayPixelsOutstanding = false;
    bool retired = false;

    void Disable()
    {
        disabled = true;
        platform.RetireInput();
    }

    bool Wait(UINT64 value)
    {
        if (!fence || value == 0 || fence->GetCompletedValue() >= value) return true;
        if (FAILED(device->GetDeviceRemovedReason())) return true;
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) return false;
        return WaitForSingleObject(fenceEvent, 3000) == WAIT_OBJECT_0;
    }

    bool Drain()
    {
        for (const auto& frame : frames) if (!Wait(frame.fenceValue)) return false;
        return true;
    }

    bool ReleaseBuffers()
    {
        InternalScope internal;
        if (!Drain())
        {
            Disable();
            RecordFailure(L"D3D12 UI drain timed out; live resources retained");
            return false;
        }
        for (auto& frame : frames)
        {
            frame.buffer.Reset();
            frame.exposedBuffer.Reset();
            frame.fenceValue = 0;
        }
        return true;
    }

    bool AcquireBuffers()
    {
        if (frames.empty() || !swapchain) return false;
        if (frames[0].buffer) return true;
        // Acquire the whole generation before publishing its descriptors. A
        // failed GetBuffer must not leave a partly reacquired generation.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        ComPtr<IUnknown> expectedDevice;
        if (FAILED(swapchain->GetDesc1(&desc)) || desc.BufferCount != frames.size()
            || !DeviceIdentity(device.Get(), expectedDevice)) return false;
        std::vector<ComPtr<ID3D12Resource>> buffers(frames.size()), exposed(frames.size());
        for (UINT index = 0; index < buffers.size(); ++index)
        {
            ComPtr<ID3D12Device> resourceDevice;
            ComPtr<IUnknown> identity;
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&exposed[index])))
                || !native::Unwrap(exposed[index].Get(), buffers[index])
                || !native::PinInterface(buffers[index].Get(), 14)
                || FAILED(buffers[index]->GetDevice(IID_PPV_ARGS(&resourceDevice)))
                || !DeviceIdentity(resourceDevice.Get(), identity)
                || identity != expectedDevice) return false;
            const auto resource = buffers[index]->GetDesc();
            if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
                || resource.Width != desc.Width || resource.Height != desc.Height
                || resource.Format != desc.Format || resource.DepthOrArraySize != 1
                || resource.MipLevels != 1 || resource.SampleDesc.Count != 1
                || !(resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return false;
            for (UINT previous = 0; previous < index; ++previous)
                if (buffers[previous] == buffers[index]) return false;
        }
        for (UINT index = 0; index < buffers.size(); ++index)
        {
            frames[index].exposedBuffer = std::move(exposed[index]);
            frames[index].buffer = std::move(buffers[index]);
            device->CreateRenderTargetView(frames[index].buffer.Get(), nullptr, frames[index].rtv);
        }
        return true;
    }

    static void AllocateSrv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
    {
        auto& state = *static_cast<State*>(info->UserData);
        *cpu = {}; *gpu = {};
        for (UINT index = 0; index < kSrvCount; ++index)
        {
            if (state.srvUsed[index]) continue;
            state.srvUsed[index] = true;
            *cpu = state.srvs->GetCPUDescriptorHandleForHeapStart();
            *gpu = state.srvs->GetGPUDescriptorHandleForHeapStart();
            cpu->ptr += static_cast<SIZE_T>(index) * state.srvStep;
            gpu->ptr += static_cast<UINT64>(index) * state.srvStep;
            return;
        }
        state.Disable();
        RecordFailure(L"D3D12 UI descriptor pool exhausted");
    }

    static void FreeSrv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        auto& state = *static_cast<State*>(info->UserData);
        const auto base = state.srvs->GetCPUDescriptorHandleForHeapStart().ptr;
        if (cpu.ptr >= base && state.srvStep && (cpu.ptr-base) % state.srvStep == 0)
        {
            const auto index = (cpu.ptr-base) / state.srvStep;
            if (index < kSrvCount) state.srvUsed[index] = false;
        }
    }

    bool Initialize()
    {
        if (initialized) return true;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        HWND window = nullptr;
        if (!swapchain || FAILED(swapchain->GetDesc1(&desc))
            || FAILED(swapchain->GetHwnd(&window)) || !window
            || desc.BufferCount < 2 || desc.BufferCount > 16
            || desc.Stereo || desc.SampleDesc.Count != 1
            || (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD
                && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT
            || device->GetNodeCount() != 1) return false;
        if (!colorSpaceObserved && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        if (colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
            && colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            && colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return false;
        if (!platform.Initialize(window)) return false;
        ContextScope current(platform.context);
        InternalScope internal;
        frames.resize(desc.BufferCount);

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = desc.BufferCount;
        if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvs)))) return false;
        const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < desc.BufferCount; ++index)
        {
            auto& frame = frames[index];
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&frame.allocator)))) return false;
            frame.rtv = rtv;
            rtv.ptr += rtvStep;
        }
        if (!AcquireBuffers()) return false;
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvCount;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvs)))
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
            || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return false;
        if (FAILED(commands->Close())) return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) return false;
        srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ImGui_ImplDX12_InitInfo info{};
        info.Device = device.Get();
        info.CommandQueue = queue.Get();
        info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
        info.RTVFormat = desc.Format;
        info.SrvDescriptorHeap = srvs.Get();
        info.UserData = this;
        info.SrvDescriptorAllocFn = &AllocateSrv;
        info.SrvDescriptorFreeFn = &FreeSrv;
        rendererInitialized = ImGui_ImplDX12_Init(&info);
        if (!rendererInitialized) return false;
        initialized = true;
        return true;
    }

    bool Shutdown(bool destroyPlatform)
    {
        InternalScope releasingResources;
        if (!ReleaseBuffers()) return false;
        std::lock_guard uiLock(gUiMutex);
        if (platform.context)
        {
            ContextScope current(platform.context);
            InternalScope internal;
            if (rendererInitialized) ImGui_ImplDX12_Shutdown();
        }
        rendererInitialized = false;
        commands.Reset();
        frames.clear();
        rtvs.Reset();
        srvs.Reset();
        fence.Reset();
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        srvUsed.fill(false);
        nextFence = 1;
        initialized = false;
        overlayPixelsOutstanding = false;
        if (destroyPlatform) platform.Shutdown();
        return true;
    }

    RenderOutcome Render(bool partialUpdate = false)
    {
        std::lock_guard stateLock(mutex);
        RenderScope rendering(this);
        if (retired) return RenderOutcome::eNoWrite;
        if (partialUpdate)
        {
            // Dirty/scroll presentation reconstructs unspecified pixels from
            // prior frames. Without a retained clean background for every
            // backbuffer, drawing or widening damage would corrupt that
            // contract. Never submit overlay work for this call. A prior UI
            // write remains explicitly outstanding until lifecycle teardown;
            // it is not reclassified as reconstructed application content.
            return overlayPixelsOutstanding
                ? RenderOutcome::eSkippedPartialUpdateWithResidual
                : RenderOutcome::eSkippedPartialUpdate;
        }
        if (disabled) return RenderOutcome::eNoWrite;
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (!Initialize())
        {
            Disable();
            RecordFailure(L"D3D12 UI initialization rejected for this swapchain");
            Shutdown(true);
            return RenderOutcome::eNoWrite;
        }
        if (!platform.WantsFrame()) return RenderOutcome::eNoWrite;
        if (!AcquireBuffers())
        {
            Disable();
            RecordFailure(L"D3D12 UI backbuffer reacquisition failed");
            Shutdown(true);
            return RenderOutcome::eNoWrite;
        }
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        if (index >= frames.size()) { Disable(); return RenderOutcome::eNoWrite; }
        auto& frame = frames[index];
        if (!Wait(frame.fenceValue))
        {
            Disable();
            RecordFailure(L"D3D12 UI frame fence timed out");
            return RenderOutcome::eNoWrite;
        }
        ContextScope current(platform.context);
        ImGui_ImplDX12_SetOutputColorSpace(colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 1
            : colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0);
        ImGui_ImplDX12_NewFrame();
        if (!ImGui_ImplDX12_IsReady())
        {
            Disable();
            RecordFailure(L"D3D12 UI pipeline initialization failed");
            return RenderOutcome::eNoWrite;
        }
        const auto output = frame.buffer->GetDesc();
        if (!platform.NewFrame(static_cast<uint32_t>(output.Width), output.Height))
            return RenderOutcome::eNoWrite;
        platform.Draw();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (!drawData || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0)
            return RenderOutcome::eNoWrite;
        if (FAILED(frame.allocator->Reset())
            || FAILED(commands->Reset(frame.allocator.Get(), nullptr)))
        {
            Disable();
            RecordFailure(L"D3D12 UI command reset failed");
            return RenderOutcome::eNoWrite;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commands->ResourceBarrier(1, &barrier);
        commands->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heap = srvs.Get();
        commands->SetDescriptorHeaps(1, &heap);
        ImGui_ImplDX12_RenderDrawData(drawData, commands.Get());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        if (FAILED(commands->Close())) { Disable(); return RenderOutcome::eNoWrite; }
        ID3D12CommandList* list = commands.Get();
        queue->ExecuteCommandLists(1, &list);
        // ExecuteCommandLists has no result. From this point pixel writes may
        // have reached the queue even if the following fence signal fails.
        overlayPixelsOutstanding = true;
        const UINT64 value = nextFence++;
        // Once submitted, retain every referenced resource until this exact
        // queue's fence completes. An unsuccessful Signal poisons the state.
        frame.fenceValue = value;
        if (FAILED(queue->Signal(fence.Get(), value)))
        {
            Disable();
            RecordFailure(L"D3D12 UI fence signal failed");
            return RenderOutcome::eSubmitted;
        }
        gRenderedFrames.fetch_add(1, std::memory_order_relaxed);
        return RenderOutcome::eSubmitted;
    }
};


} // namespace

struct Session : State
{
    ComPtr<IDXGISwapChain3> renderingChain;
    // The creation-argument queue as the application holds it (a wrapper's
    // object, if any): the DLSS 5 Present stage submits there.
    ComPtr<ID3D12CommandQueue> applicationQueue;
    bool providerSession = false;
    // XeFG's native output chain under OptiScaler (every output
    // frame, generated ones included): the menu only.
    bool outputLayer = false;
    // menuout.1: DLSS-G's chain beneath Streamline (an output layer) and the
    // game's chain above Streamline. The base draws the menu on every output
    // frame; the layer above only while the base has not presented for 1 s.
    bool streamlineBase = false, aboveBase = false;
    std::atomic<int64_t> basePresentTicks{0};
    std::atomic<bool> providerPresented{false};
    bool resizing = false;
    bool wasStillDrawing = false;
    // Atomic so the output layer's overlap wait can WaitOnAddress on it with no lock held; written under mutex.
    std::atomic<uint32_t> presents{0};
    // The output layer's Presents, those that overlapped another in-flight Present, those that
    // then waited for it and got the menu, and those whose wait timed out. Written under mutex.
    std::atomic<DWORD> presentThread{0};
    uint64_t outputPresents = 0, overlaps = 0, overlapWaits = 0, overlapTimeouts = 0;
    int64_t lastPresentTicks = 0, intervalTicks = 0;
    bool menuDrawn = false, totalsLogged = false;
};
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
namespace {
std::mutex gSessionsMutex;
std::array<Session*, 64> gSessions{};
bool Principal(Session* session) noexcept
{
    const HWND foreground = GetForegroundWindow();
    auto score = [foreground](Session* s) {
        return s->presentationArea.load(std::memory_order_relaxed)
            + (s->presentationWindow == foreground ? (1ull << 60) : 0);
    };
    std::lock_guard lock(gSessionsMutex);
    for (auto* other : gSessions)
    {
        // XeSS's application layer owns FG input preparation and the paired
        // queue. Once used, it supersedes its underlying native window layer.
        if (other && other != session && !session->providerSession
            && other->presentationWindow == session->presentationWindow
            && other->providerSession && other->providerPresented.load(std::memory_order_acquire)) return false;
        // A frame generator's output chain yields to any other layer of its window.
        if (other && other != session && session->outputLayer && !session->streamlineBase && !other->outputLayer
            && other->presentationWindow == session->presentationWindow) return false;
        // Streamline's base and the layer above it share their window's turn.
        if (other && other != session && (session->streamlineBase || other->streamlineBase)
            && other->presentationWindow == session->presentationWindow) continue;
        if (other && other != session && score(other) > score(session)) return false;
    }
    return true;
}

// DLSS 5 Present stage: the application's current image, processed on its
// presentation queue before this Present reaches the next layer (Streamline
// DLSS-G reads it there). Before our UI, so the menu is never processed.
void PresentNeural(Session& session) noexcept
{
    if (!session.applicationQueue || !dlss5::PresentStageSelected()) return;
    InternalScope internal;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    ComPtr<ID3D12Resource> buffer;
    const UINT index = session.swapchain->GetCurrentBackBufferIndex();
    if (FAILED(session.swapchain->GetDesc1(&desc)) || index >= desc.BufferCount
        || FAILED(session.swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return;
    // An FP16 swapchain without SetColorSpace1 presents scRGB.
    const auto color = !session.colorSpaceObserved && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : session.colorSpace;
    dlss5::PresentTarget target{};
    target.queue = session.applicationQueue.Get();
    target.buffer = buffer.Get();
    target.colorSpace = static_cast<uint32_t>(color);
    target.owner = reinterpret_cast<uintptr_t>(&session);
    dlss5::Present(target);
}

}

// OptiScaler's frame-generation swapchain: its own wrapper or, as it answers
// Streamline's unwrap contract with what it wraps, the XeFG swapchain of the
// XeSS SDK it loaded. The vtable and Present slot must be in one of those two
// images, and OptiScaler's UI mode must let this menu show.
bool OptiScalerChain(IDXGISwapChain3* chain) noexcept
{
    if (!chain || !optiscaler_host::Detected() || optiscaler_host::UiHidden()) return false;
    const HMODULE fg = xess_mod::NativeFgModule();
    auto** table = *reinterpret_cast<void***>(chain);
    for (const HMODULE owner : {optiscaler_host::Module(), fg})
        if (owner && slots::ImageSlot(owner, table) && slots::ImageEntry(owner, table[8]))
            return native::PinInterface(chain, 39);
    return false;
}

// nvpresent.1: NVIDIA's driver present layer (NvPresent64.dll, Smooth Motion)
// answers no unwrapping IID. Like any base it only proves identity; only the
// driver store's own copy qualifies.
bool NvPresentChain(IDXGISwapChain3* chain) noexcept
{
    if (!chain) return false;
    auto** table = *reinterpret_cast<void***>(chain);
    HMODULE owner = nullptr;
    std::array<wchar_t, adapter_discovery::kPathCapacity> path{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(table), &owner)) return false;
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() && adapter_discovery::DriverFilePath(path.data(), L"NvPresent64.dll", false)
        && slots::ImageSlot(owner, table) && slots::ImageSlot(owner, table + 39) && native::PinInterface(chain, 39);
}

// The same rule for XeFG's native output chain of an OptiScaler context.
bool OutputSwapchainWanted() noexcept
{
    // XeMFG standalone's chains too (XeFG's proxy chain, the game's, is libxess_fg's own object).
    if (xess_d3d12::InsideTakeover()) return single_module::OverlayAllowed();
    return optiscaler_host::Detected() && !optiscaler_host::UiHidden() && single_module::OverlayAllowed();
}

// Refusal detail: the failing step and the module whose vtable the unwrapped chain uses.
void RefuseSession(const wchar_t* step, IDXGISwapChain3* nativeChain) noexcept
{
    wchar_t owner[MAX_PATH]{};
    HMODULE module = nullptr;
    if (nativeChain && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(*reinterpret_cast<void***>(nativeChain)), &module))
        GetModuleFileNameW(module, owner, MAX_PATH);
    wchar_t message[MAX_PATH + 200]{};
    swprintf_s(message, L"MFG_PROXY_UI renderer unavailable: native swapchain/queue/device ownership not established (step=%ls swapchain owner=%ls)",
        step, owner[0] ? owner : L"-");
    RecordFailure(message);
}

Session* CreateSession(IDXGISwapChain* chain, IUnknown* presentedQueue, HMODULE provider, bool output,
    bool streamlineBase) noexcept
{
    if (!chain || !presentedQueue || native::InsideLoader()) return nullptr;
    InternalScope internal;
    try {
        // Streamline's chain over our proxy of its base: that proxy is the
        // base's identity (beneath it, unwrapping may reach a system chain).
        const bool aboveBase = !provider && !output && AboveStreamlineBase(chain);
        ComPtr<IDXGISwapChain3> nativeChain, renderingChain;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Device> device, swapDevice, nativeDevice;
        ComPtr<IUnknown> queueIdentity, swapIdentity;
        const wchar_t* step = nullptr;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&renderingChain))) || !native::PinInterface(renderingChain.Get(), 39))
            step = L"swapchain interface";
        else if (!(provider
                ? (slots::ImageSlot(provider, *reinterpret_cast<void***>(renderingChain.Get()))
                    && slots::ImageEntry(provider, (*reinterpret_cast<void***>(renderingChain.Get()))[8]))
                : (native::Unwrap(chain, nativeChain)
                    && (aboveBase || native::SystemDxgiObject(nativeChain.Get(), 39) || OptiScalerChain(nativeChain.Get())
                        || NvPresentChain(nativeChain.Get())))))
            step = L"swapchain owner";
        else if (!native::Unwrap(presentedQueue, queue) || !native::PinInterface(queue.Get(), 18)
            || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            step = L"presentation queue";
        else if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !native::Unwrap(device.Get(), nativeDevice)
            || !native::PinInterface(nativeDevice.Get(), 43))
            step = L"queue device";
        else if (FAILED(renderingChain->GetDevice(IID_PPV_ARGS(&swapDevice)))
            || !DeviceIdentity(nativeDevice.Get(), queueIdentity)
            || !DeviceIdentity(swapDevice.Get(), swapIdentity)
            || queueIdentity.Get() != swapIdentity.Get())
            step = L"swapchain device";
        if (step) {
            RefuseSession(step, nativeChain.Get());
            return nullptr;
        }
        const LUID a = nativeDevice->GetAdapterLuid(), b = swapDevice->GetAdapterLuid();
        if (a.HighPart != b.HighPart || a.LowPart != b.LowPart) return nullptr;
        auto session = std::make_unique<Session>();
        session->providerSession = provider != nullptr;
        session->outputLayer = output || streamlineBase;
        session->streamlineBase = streamlineBase;
        session->aboveBase = aboveBase;
        session->platform.afterFrameGeneration = output || streamlineBase;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(renderingChain->GetHwnd(&session->presentationWindow)) || !session->presentationWindow
            || FAILED(renderingChain->GetDesc1(&desc))) return nullptr;
        // Keep the returned swapchain's semantics paired with its creation
        // queue. Streamline may expose off-screen application buffers, a
        // different buffer count/index, and another native presentation queue.
        // The native chain above proves the base's identity; it is never used
        // to obtain rendering buffers or bypass this layer's frame indexing.
        session->renderingChain = renderingChain;
        session->swapchain = renderingChain.Get();
        session->queue = queue;
        if (FAILED(presentedQueue->QueryInterface(IID_PPV_ARGS(&session->applicationQueue))))
            session->applicationQueue.Reset();
        session->device = nativeDevice;
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
        {
            std::lock_guard lock(gSessionsMutex);
            auto slot = std::find(gSessions.begin(), gSessions.end(), nullptr);
            if (slot == gSessions.end()) return nullptr;
            *slot = session.get();
        }
        single_module::Log(provider ? L"MFG_PROXY_UI renderer registered layer=creation-return buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : streamlineBase ? L"MFG_PROXY_UI renderer registered layer=streamline-base buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : aboveBase ? L"MFG_PROXY_UI renderer registered layer=creation-return base=RTXMFG streamline-base buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : output ? L"MFG_PROXY_UI renderer registered layer=xefg-output base=OptiScaler XeFG buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : native::SystemDxgiObject(nativeChain.Get(), 39)
            ? L"MFG_PROXY_UI renderer registered layer=creation-return buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : NvPresentChain(nativeChain.Get())
            ? L"MFG_PROXY_UI renderer registered layer=creation-return base=NVIDIA NvPresent buffers=layer-local index=layer-local queue=creation-argument device=matched"
            : L"MFG_PROXY_UI renderer registered layer=creation-return base=OptiScaler swapchain buffers=layer-local index=layer-local queue=creation-argument device=matched");
        return session.release();
    } catch (...) {
        RecordFailure(L"MFG_PROXY_UI renderer allocation failed; original presentation retained");
        return nullptr;
    }
}

namespace {
// The output layer's overlap totals, once per session.
void LogOverlapTotals(Session& session) noexcept
{
    if (!session.outputLayer || session.totalsLogged) return;
    session.totalsLogged = true;
    wchar_t message[256]{};
    swprintf_s(message, L"MFG_PROXY_UI output layer retired: presents=%llu overlapped=%llu waited=%llu timed_out=%llu",
        static_cast<unsigned long long>(session.outputPresents), static_cast<unsigned long long>(session.overlaps),
        static_cast<unsigned long long>(session.overlapWaits), static_cast<unsigned long long>(session.overlapTimeouts));
    single_module::Log(message);
}
}

// Quiesces a provider session while the XeSS context is being destroyed.
// Fails (and keeps the session) while a present or resize is in flight.
bool RetireSession(Session* session) noexcept
{
    if (!session) return true;
    std::lock_guard lock(session->mutex);
    if (session->presents || session->resizing) return false;
    session->platform.RetireInput();
    if (!session->Shutdown(true)) return false;
    session->retired = true;
    LogOverlapTotals(*session);
    return true;
}

void DestroySession(Session* session) noexcept
{
    if (!session) return;
    {
        std::lock_guard lock(gSessionsMutex);
        for (auto& slot : gSessions) if (slot == session) slot = nullptr;
    }
    {
        std::lock_guard lock(session->mutex);
        LogOverlapTotals(*session);
    }
    session->retired = true;
    session->platform.RetireInput();
    dlss5::DrainPresent(reinterpret_cast<uintptr_t>(session));
    // A wrapper holds a method lease across every forwarded callback. Its
    // final release proves no other method can still access this session.
    // A failed GPU drain retains the entire generation, including the native
    // chain, queue and allocator, for process lifetime without new allocation.
    if (!session->Shutdown(true)) {
        RecordFailure(L"MFG_PROXY_UI retiring generation retained after GPU drain failure");
        return;
    }
    delete session;
}

bool IsPrincipal(Session* session) noexcept
{
    return session && Principal(session);
}

bool AnySession() noexcept
{
    std::lock_guard lock(gSessionsMutex);
    for (auto* session : gSessions)
        if (session) return true;
    return false;
}

bool WindowSession(HWND window) noexcept
{
    if (!window) return false;
    std::lock_guard lock(gSessionsMutex);
    for (auto* session : gSessions)
        if (session && session->presentationWindow == window) return true;
    return false;
}

namespace {
using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressFn = VOID(WINAPI*)(PVOID);
struct AddressWait
{
    WaitOnAddressFn wait = nullptr;
    WakeByAddressFn wake = nullptr;
    AddressWait() noexcept
    {
        // KernelBase exports them on Windows 8+; resolved here so no new import is linked.
        if (const HMODULE base = GetModuleHandleW(L"kernelbase.dll")) {
            wait = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(base, "WaitOnAddress"));
            wake = reinterpret_cast<WakeByAddressFn>(GetProcAddress(base, "WakeByAddressAll"));
        }
        if (!wait || !wake) wait = nullptr, wake = nullptr;
    }
};
const AddressWait& Address() noexcept { static const AddressWait functions; return functions; }
int64_t TickFrequency() noexcept
{
    static const int64_t frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return frequency;
}

// The layer above Streamline leaves the menu to the base beneath it while
// that base presented within the last second (menuout.1).
bool BaseDraws(const Session& session) noexcept
{
    if (!session.aboveBase) return false;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    std::lock_guard lock(gSessionsMutex);
    for (auto* other : gSessions)
    {
        if (!other || !other->streamlineBase || other->presentationWindow != session.presentationWindow) continue;
        const int64_t last = other->basePresentTicks.load(std::memory_order_acquire);
        if (last && now.QuadPart - last < TickFrequency()) return true;
    }
    return false;
}

// XeFG presents its outputs from two threads, so a Present can arrive while the other is in flight and would forward
// without the menu. While the menu draws, the output layer waits for it with no lock held (DXGI holds this call behind
// that Present anyway), at most two output intervals and never more than 20 ms; on timeout it forwards without the menu.
void WaitForOtherPresent(Session& session, UINT flags) noexcept
{
    if (flags & DXGI_PRESENT_TEST) return;
    const int64_t frequency = TickFrequency();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    int64_t bound = frequency / 50; // 20 ms
    DWORD owner = 0;
    {
        std::lock_guard lock(session.mutex);
        ++session.outputPresents;
        // Mean output interval: Present starts per unit of time, across both threads; pauses are not intervals.
        const int64_t delta = session.lastPresentTicks ? now.QuadPart - session.lastPresentTicks : 0;
        if (delta > 0 && delta < frequency / 10)
            session.intervalTicks = session.intervalTicks ? session.intervalTicks + (delta - session.intervalTicks) / 16 : delta;
        session.lastPresentTicks = now.QuadPart;
        if (!session.presents.load(std::memory_order_acquire) || session.resizing || session.retired) return;
        owner = session.presentThread.load(std::memory_order_relaxed);
        if (owner == GetCurrentThreadId()) return; // re-entered from inside its own Present: never wait on itself
        ++session.overlaps;
        if (!session.menuDrawn) return;
        if (session.intervalTicks) bound = std::clamp<int64_t>(2 * session.intervalTicks, frequency / 1000, bound);
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        wchar_t message[200]{};
        swprintf_s(message, L"MFG_PROXY_UI output layer: concurrent Present on thread %lu while thread %lu presents; waiting up to %.1f ms for it",
            GetCurrentThreadId(), owner, 1000.0 * double(bound) / double(frequency));
        single_module::Log(message);
    }
    const auto& address = Address();
    const int64_t deadline = now.QuadPart + bound;
    bool resolved = false;
    for (;;)
    {
        uint32_t observed = session.presents.load(std::memory_order_acquire);
        if (!observed) { resolved = true; break; }
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= deadline) break;
        const DWORD remaining = static_cast<DWORD>(std::max<int64_t>(1, (deadline - now.QuadPart) * 1000 / frequency));
        if (address.wait) address.wait(&session.presents, &observed, sizeof observed, remaining);
        else SwitchToThread();
    }
    std::lock_guard lock(session.mutex);
    ++(resolved ? session.overlapWaits : session.overlapTimeouts);
}
}

bool BeginPresent(Session* session, UINT flags, bool partialUpdate) noexcept
{
    // XeFG's real frame reaches its output chain on the game's thread, inside our Vulkan overlay's
    // forwarding of the game's present; only that scope may be open for the output layer to draw on it.
    if (!session || (gInsideOverlay && !(session->outputLayer && gForwardingVulkanPresent))) return false;
    if (session->outputLayer) WaitForOtherPresent(*session, flags);
    std::lock_guard lock(session->mutex);
    if (session->resizing || session->retired) return false;
    ++session->presents;
    if (session->presents != 1) return true; // Concurrent calls forward; only one UI submission can own a frame.
    session->presentThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if ((flags & DXGI_PRESENT_TEST) || ((flags & DXGI_PRESENT_DO_NOT_WAIT) && session->wasStillDrawing)) return true;
    session->menuDrawn = false;
    if (session->providerSession) session->providerPresented.store(true, std::memory_order_release);
    if (session->streamlineBase)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        session->basePresentTicks.store(now.QuadPart, std::memory_order_release);
    }
    if (!Principal(session)) return true;
    // Streamline's base presents the layer above's frames too.
    if (!session->streamlineBase) gDxgiFrames.fetch_add(1, std::memory_order_relaxed);
#if !MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
    // Dirty/scroll presentation reuses pixels outside its rectangles, which
    // whole-image processing would change. A frame generator's output chain
    // also presents generated frames: DLSS 5 stays on the game's frames.
    if (!partialUpdate && !session->retired && !session->outputLayer) PresentNeural(*session);
#endif
#if MFG_UNLOCK_OVERLAY_MENU_DRAW && !MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
    if (install::InputReady() && single_module::OverlayAllowed() // A concurrent creator never waits on input publication.
        && !BaseDraws(*session))
    {
        try { session->menuDrawn = session->Render(partialUpdate) == RenderOutcome::eSubmitted; }
        catch (...) { session->Disable(); RecordFailure(L"MFG_PROXY_UI render exception; UI disabled for this generation"); }
    }
#endif
    return true;
}
void EndPresent(Session* session, HRESULT result, UINT flags) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    // Streamline's base counts the same presents the layer above reads through Streamline.
    if (result==S_OK && !(flags & DXGI_PRESENT_TEST) && !session->streamlineBase && Principal(session)) {
        InternalScope internal;
        UINT count=0;
        const bool available=SUCCEEDED(session->swapchain->GetLastPresentCount(&count));
        frame_telemetry::SamplePresentCounter(session->telemetryOwner,count,available);
    }
    if (!(flags & DXGI_PRESENT_TEST)) session->wasStillDrawing = result == DXGI_ERROR_WAS_STILL_DRAWING;
    if (session->presents && !--session->presents) session->presentThread.store(0, std::memory_order_relaxed);
    if (session->outputLayer && Address().wake) Address().wake(&session->presents);
}
bool BeginResize(Session* session) noexcept
{
    if (!session) return true;
    // XeFG resizes its output chain on the game's thread; its presenter thread
    // may still be inside a Present there (bounded wait, never under the lock).
    for (int waited = 0; session->outputLayer && waited < 250; ++waited)
    {
        {
            std::lock_guard lock(session->mutex);
            if (!session->presents && !session->resizing) break;
        }
        Sleep(1);
    }
    std::lock_guard lock(session->mutex);
    if (session->resizing || session->presents) return false;
    session->resizing = true;
    ui_input_coherence::InvalidateResources();
    if (!dlss5::DrainPresent(reinterpret_cast<uintptr_t>(session))
        || !session->Shutdown(false)) { session->resizing = false; return false; }
    return true;
}
void EndResize(Session* session, HRESULT result) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(result) && SUCCEEDED(session->swapchain->GetDesc1(&desc)))
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
    session->telemetryOwner=gNextTelemetryOwner.fetch_add(1,std::memory_order_relaxed);
    session->resizing = false;
    session->wasStillDrawing = false;
}
void ObserveColorSpace(Session* session, DXGI_COLOR_SPACE_TYPE color) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    session->colorSpace = color;
    session->colorSpaceObserved = true;
}
void VerifyResizeQueues(Session* session, UINT count, IUnknown* const* queues) noexcept
{
    if (!session || !queues) return;
    std::lock_guard lock(session->mutex);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(session->swapchain->GetDesc1(&desc))) { session->Disable(); return; }
    if (!count) count = desc.BufferCount;
    if (count == 0 || count > 16 || count != desc.BufferCount) { session->Disable(); return; }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IUnknown> a, b;
        if (!native::Unwrap(queues[i], queue) || FAILED(queue.As(&a))
            || FAILED(session->queue.As(&b)) || a.Get() != b.Get()) {
            session->Disable();
            RecordFailure(L"MFG_PROXY_UI ResizeBuffers1 changed presentation queue; rendering disabled until recreation");
            return;
        }
    }
}
} // namespace single_overlay::dx12
