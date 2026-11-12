#include "overlay_dx11.h"
#include "overlay_dx12.h"
#include "overlay_vulkan.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include "overlay_native.h"
#include "overlay_color.h"
#include "present_counter.h"
#include "ui_input_coherence.h"
#include <backends/imgui_impl_dx11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

// dx11menu.1: the D3D12 session's contract (overlay_dx12.cpp) on a DirectX 11 swapchain:
// - the menu draws in the game's Present, before the next layer, on the native device's immediate context, inside a
//   device context state of its own (SwapDeviceContextState), so the game's pipeline state is never read or changed;
// - on buffer 0 through a view made for that Present only, so no reference of ours outlasts a Present;
// - with the same HDR output encoding and per-present bookkeeping as D3D12 (an sRGB render target: mode 3).
namespace single_overlay::dx11
{
using Microsoft::WRL::ComPtr;
namespace
{
// Apart from the D3D12 sessions' owners: the present counter starts a new sample window for a new owner.
std::atomic<uint64_t> gNextTelemetryOwner{1ull << 48};

// native::PinInterface beyond its 64-entry bound (ID3D11DeviceContext1 has 134 methods). A table in an image (the
// debug layer's) is pinned like any interface. The plain runtime builds its context table in its own heap (read/write,
// not executable), accepted only when every entry 0..last is executable code of `runtime`, which is pinned.
bool PinTable(IUnknown* object, size_t last, HMODULE runtime) noexcept
{
    if (!object || !runtime) return false;
    auto** table = *reinterpret_cast<void***>(object);
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(table), &owner))
    {
        if (!native::PinInterface(object, 64) || !slots::ImageSlot(owner, table + last)) return false;
        for (size_t i = 65; i <= last; ++i)
        {
            HMODULE callable = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(table[i]), &callable) || !slots::ImageEntry(callable, table[i])) return false;
        }
        return true;
    }
    MEMORY_BASIC_INFORMATION first{}, end{};
    const auto readable = [](const MEMORY_BASIC_INFORMATION& m) {
        return m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && (m.Protect & (PAGE_READONLY | PAGE_READWRITE))
            && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS));
    };
    if (reinterpret_cast<uintptr_t>(table) % alignof(void*)
        || VirtualQuery(table, &first, sizeof(first)) != sizeof(first) || !readable(first)
        || VirtualQuery(table + last, &end, sizeof(end)) != sizeof(end) || !readable(end)) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(runtime), &pinned) || pinned != runtime) return false;
    for (size_t i = 0; i <= last; ++i)
    {
        HMODULE callable = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(table[i]), &callable) || callable != runtime
            || !slots::ImageEntry(callable, table[i])) return false;
    }
    return true;
}

// The image that holds `object`'s methods (its QueryInterface), or null.
HMODULE MethodImage(IUnknown* object) noexcept
{
    HMODULE owner = nullptr;
    if (!object || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>((*reinterpret_cast<void***>(object))[0]), &owner)) return nullptr;
    return owner;
}

// The native IUnknown identity behind any public wrapper (Streamline, ReShade, Special K contracts).
bool Identity(IUnknown* object, ComPtr<IUnknown>& identity) noexcept
{
    identity.Reset();
    ComPtr<ID3D11Device> unwrapped;
    return object && native::Unwrap(object, unwrapped) && SUCCEEDED(unwrapped.As(&identity)) && identity;
}

// A device made multithread-protected by the game serializes every immediate-context call on its lock; ours too.
struct Serialized
{
    ID3D11Multithread* lock = nullptr;
    explicit Serialized(ID3D11Multithread* candidate) noexcept
        : lock(candidate && candidate->GetMultithreadProtected() ? candidate : nullptr)
    {
        if (lock) lock->Enter();
    }
    ~Serialized() { if (lock) lock->Leave(); }
    Serialized(const Serialized&) = delete;
    Serialized& operator=(const Serialized&) = delete;
};

// The menu's own context state for one draw. The game's state always comes back, cleared of ours first, however the
// scope ends. When the swap reported no state to return to, it did not happen: nothing is cleared or swapped back.
struct StateSwap
{
    ID3D11DeviceContext1* context;
    ComPtr<ID3DDeviceContextState> previous;
    StateSwap(ID3D11DeviceContext1* target, ID3DDeviceContextState* ours) noexcept : context(target)
    {
        context->SwapDeviceContextState(ours, &previous);
    }
    ~StateSwap()
    {
        if (!previous) return;
        context->ClearState();
        context->SwapDeviceContextState(previous.Get(), nullptr);
    }
    StateSwap(const StateSwap&) = delete;
    StateSwap& operator=(const StateSwap&) = delete;
};

enum class RenderOutcome { eNoWrite, eSubmitted };
}

struct Session
{
    std::recursive_mutex mutex;
    uint64_t telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
    // The creation-return layer the proxy wraps: its buffers and its own frame indexing, never the native chain's.
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D11Device1> device;           // the native device beneath any public wrapper
    ComPtr<ID3D11DeviceContext1> context;   // its immediate context
    ComPtr<ID3DDeviceContextState> state;  // the menu's own pipeline state
    ComPtr<ID3D11Multithread> multithread;
    PlatformState platform;
    HWND presentationWindow = nullptr;
    std::atomic<uint64_t> presentationArea{0};
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool colorSpaceObserved = false;
    bool initialized = false;
    bool rendererInitialized = false;
    bool disabled = false;
    bool retired = false;
    bool resizing = false;
    bool wasStillDrawing = false;
    bool formatLogged = false;
    uint32_t presents = 0;
    DWORD presentThread = 0; // the thread of the outer Present while presents > 0

    void Disable()
    {
        disabled = true;
        platform.RetireInput();
    }

    bool Initialize()
    {
        if (initialized) return true;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        HWND window = nullptr;
        // Every swap effect: DirectX 11 draws on buffer 0 in each. Stereo buffers are refused as on D3D12.
        if (!swapchain || FAILED(swapchain->GetDesc1(&desc))
            || FAILED(swapchain->GetHwnd(&window)) || !window
            || desc.BufferCount < 1 || desc.BufferCount > 16 || desc.Stereo
            || !(desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)) return false;
        if (!platform.Initialize(window)) return false;
        ContextScope current(platform.context);
        InternalScope internal;
        rendererInitialized = ImGui_ImplDX11_Init(device.Get(), context.Get());
        if (!rendererInitialized) return false;
        initialized = true;
        return true;
    }

    // Nothing of ours is bound or referenced between Presents, so no GPU wait: Direct3D 11 keeps released objects
    // alive until the work that used them completes.
    void Shutdown()
    {
        InternalScope releasingResources;
        std::lock_guard uiLock(gUiMutex);
        if (platform.context)
        {
            ContextScope current(platform.context);
            InternalScope internal;
            if (rendererInitialized) ImGui_ImplDX11_Shutdown();
        }
        rendererInitialized = false;
        initialized = false;
        platform.Shutdown();
    }

    RenderOutcome Render(bool partialUpdate)
    {
        std::lock_guard stateLock(mutex);
        // Dirty/scroll presentation keeps the pixels outside its rectangles from earlier frames, which a UI
        // drawn into them would corrupt: as on D3D12, no UI for that Present.
        if (retired || disabled || partialUpdate) return RenderOutcome::eNoWrite;
        std::lock_guard uiLock(gUiMutex);
        InternalScope internal;
        if (!Initialize())
        {
            Disable();
            RecordFailure(L"D3D11 UI initialization rejected for this swapchain");
            Shutdown();
            return RenderOutcome::eNoWrite;
        }
        if (!platform.WantsFrame()) return RenderOutcome::eNoWrite;
        if (FAILED(device->GetDeviceRemovedReason()))
        {
            Disable();
            RecordFailure(L"D3D11 UI device removed; UI disabled for this swapchain");
            return RenderOutcome::eNoWrite;
        }
        // The current back buffer of the layer the proxy wraps, on this session's device.
        DXGI_SWAP_CHAIN_DESC1 chain{};
        ComPtr<ID3D11Texture2D> exposed, buffer;
        ComPtr<ID3D11Device> bufferDevice;
        ComPtr<IUnknown> bufferIdentity, deviceIdentity;
        if (FAILED(swapchain->GetDesc1(&chain)) || FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&exposed)))
            || !native::Unwrap(exposed.Get(), buffer) || !native::PinInterface(buffer.Get(), 10))
        {
            Disable();
            RecordFailure(L"D3D11 UI back buffer unavailable");
            return RenderOutcome::eNoWrite;
        }
        buffer->GetDevice(&bufferDevice);
        if (!bufferDevice || FAILED(bufferDevice.As(&bufferIdentity)) || FAILED(device.As(&deviceIdentity))
            || bufferIdentity.Get() != deviceIdentity.Get())
        {
            Disable();
            RecordFailure(L"D3D11 UI back buffer belongs to another device");
            return RenderOutcome::eNoWrite;
        }
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        if (desc.Width != chain.Width || desc.Height != chain.Height || desc.ArraySize != 1 || desc.MipLevels != 1
            || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET))
        {
            Disable();
            RecordFailure(L"D3D11 UI back buffer shape unsupported");
            return RenderOutcome::eNoWrite;
        }
        // As on D3D12: an FP16 swapchain without SetColorSpace1 presents scRGB. An sRGB format encodes the linear
        // colour itself (mode 3). Another colour space gets no UI until it changes.
        const auto color = !colorSpaceObserved && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
            ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : colorSpace;
        const bool srgbTarget = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        int mode = 0;
        if (color == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) mode = 1;
        else if (color == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) mode = 2;
        else if (color != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) mode = -1;
        else if (srgbTarget) mode = 3;
        // The output pairing, once per chain. HDR10 switched on through NVAPI or AGS leaves a 10-bit chain at G22
        // without SetColorSpace1: the menu draws as SDR (too bright in a PQ signal) and the log says why.
        if (!formatLogged)
        {
            formatLogged = true;
            const bool tenBitSdr = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && !colorSpaceObserved
                && color == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            const wchar_t* note = tenBitSdr
                ? L" (10-bit at G22 without SetColorSpace1: drawn as SDR, too bright if HDR10 was enabled outside DXGI)"
                : mode < 0 ? L" (colour space unsupported: no UI until it changes)" : L"";
            wchar_t line[300]{};
            swprintf_s(line, L"MFG_PROXY_UI D3D11 UI output format=%u colorSpace=%u setByGame=%u mode=%d%ls",
                static_cast<unsigned>(desc.Format), static_cast<unsigned>(color), colorSpaceObserved ? 1u : 0u,
                mode, note);
            single_module::Log(line);
        }
        if (mode < 0) return RenderOutcome::eNoWrite;
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target)) || !target)
        {
            Disable();
            RecordFailure(L"D3D11 UI render target view failed");
            return RenderOutcome::eNoWrite;
        }
        ContextScope current(platform.context);
        ImGui_ImplDX11_SetOutputColorSpace(mode);
        ImGui_ImplDX11_NewFrame();
        if (!ImGui_ImplDX11_IsReady())
        {
            Disable();
            RecordFailure(L"D3D11 UI pipeline initialization failed");
            return RenderOutcome::eNoWrite;
        }
        if (!platform.NewFrame(desc.Width, desc.Height)) return RenderOutcome::eNoWrite;
        platform.Draw();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (!drawData || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0)
            return RenderOutcome::eNoWrite;
        {
            // Destroyed in reverse: the game's state returns (ours cleared first), then the lock is left.
            Serialized serialized(multithread.Get());
            StateSwap swap(context.Get(), state.Get());
            if (!swap.previous)
            {
                // No state was reported: the swap did not happen, so the game's state is left as it is.
                Disable();
                RecordFailure(L"D3D11 UI: the context state swap reported no state to return to; UI disabled");
                return RenderOutcome::eNoWrite;
            }
            ID3D11RenderTargetView* view = target.Get();
            context->OMSetRenderTargets(1, &view, nullptr);
            ImGui_ImplDX11_RenderDrawData(drawData);
        }
        gRenderedFrames.fetch_add(1, std::memory_order_relaxed);
        return RenderOutcome::eSubmitted;
    }
};

namespace
{
std::mutex gSessionsMutex;
std::array<Session*, 64> gSessions{};

// The game's main output among the DirectX 11 sessions: the largest presentation area, the foreground window first.
// Never while a D3D12 or Vulkan menu session is live.
bool Principal(Session* session) noexcept
{
    if (dx12::AnySession() || vulkan::AnySession()) return false;
    const HWND foreground = GetForegroundWindow();
    auto score = [foreground](Session* s) {
        return s->presentationArea.load(std::memory_order_relaxed)
            + (s->presentationWindow == foreground ? (1ull << 60) : 0);
    };
    std::lock_guard lock(gSessionsMutex);
    for (auto* other : gSessions)
        if (other && other != session && score(other) > score(session)) return false;
    return true;
}

// Refusal detail: the failing step and the module whose table the unwrapped chain uses.
void RefuseSession(const wchar_t* step, IUnknown* nativeChain) noexcept
{
    wchar_t owner[MAX_PATH]{};
    HMODULE module = nullptr;
    if (nativeChain && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(*reinterpret_cast<void***>(nativeChain)), &module))
        GetModuleFileNameW(module, owner, MAX_PATH);
    wchar_t message[MAX_PATH + 200]{};
    swprintf_s(message, L"MFG_PROXY_UI DirectX 11 renderer unavailable: native swapchain/device ownership not established (step=%ls swapchain owner=%ls)",
        step, owner[0] ? owner : L"-");
    RecordFailure(message);
}
}

Session* CreateSession(IDXGISwapChain* chain, IUnknown* presentedDevice) noexcept
{
    if (!chain || !presentedDevice || native::InsideLoader()) return nullptr;
    InternalScope internal;
    try {
        ComPtr<IDXGISwapChain3> renderingChain, nativeChain;
        ComPtr<ID3D11Device> device, swapDevice;
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> immediate;
        ComPtr<ID3D11DeviceContext1> context1;
        ComPtr<ID3DDeviceContextState> state;
        ComPtr<ID3D11Multithread> multithread;
        ComPtr<IUnknown> deviceIdentity, swapIdentity;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1, chosen = D3D_FEATURE_LEVEL_9_1;
        bool nvPresent = false;
        const wchar_t* step = nullptr;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&renderingChain))) || !native::PinInterface(renderingChain.Get(), 39))
            step = L"swapchain interface";
        // The native chain only proves the base's identity (a system DXGI chain, or NVIDIA's driver present
        // layer); the menu draws on the returned layer's buffers.
        else if (!native::Unwrap(chain, nativeChain)
            || !(native::SystemDxgiObject(nativeChain.Get(), 39) || (nvPresent = dx12::NvPresentChain(nativeChain.Get()))))
            step = L"swapchain owner";
        else if (!native::Unwrap(presentedDevice, device) || !native::PinInterface(device.Get(), 42))
            step = L"device";
        else if (FAILED(renderingChain->GetDevice(IID_PPV_ARGS(&swapDevice)))
            || !Identity(device.Get(), deviceIdentity) || !Identity(swapDevice.Get(), swapIdentity)
            || deviceIdentity.Get() != swapIdentity.Get())
            step = L"swapchain device";
        else if (FAILED(device.As(&device1)) || !native::PinInterface(device1.Get(), 49))
            step = L"Direct3D 11.1 device";
        // The menu's shaders are vs_4_0/ps_4_0.
        else if ((level = device->GetFeatureLevel()) < D3D_FEATURE_LEVEL_10_0)
            step = L"feature level";
        // ImGui_ImplDX11_Init takes the device's factory through its adapter and needs all three.
        else if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetParent(IID_PPV_ARGS(&adapter)))
            || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
            step = L"device adapter";
        else
        {
            device->GetImmediateContext(&immediate);
            const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
            if (!immediate || FAILED(immediate.As(&context1)) || !PinTable(context1.Get(), 133, MethodImage(device.Get())))
                step = L"immediate context";
            // The game's lock, where it has one, must be ours too.
            else if (SUCCEEDED(immediate.As(&multithread)) && !native::PinInterface(multithread.Get(), 6))
                step = L"immediate context lock";
            else if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                    __uuidof(ID3D11Device), &chosen, &state)) || !state || chosen != level)
                step = L"device context state";
        }
        if (step) {
            RefuseSession(step, nativeChain.Get());
            return nullptr;
        }
        auto session = std::make_unique<Session>();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(renderingChain->GetHwnd(&session->presentationWindow)) || !session->presentationWindow
            || FAILED(renderingChain->GetDesc1(&desc))) return nullptr;
        session->swapchain = renderingChain;
        session->device = device1;
        session->context = context1;
        session->state = state;
        session->multithread = multithread;
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
        // The menu offers only what acts in a DirectX 11 game (standalone_ui::Draw).
        session->platform.d3d11 = true;
        {
            std::lock_guard lock(gSessionsMutex);
            auto slot = std::find(gSessions.begin(), gSessions.end(), nullptr);
            if (slot == gSessions.end()) return nullptr;
            *slot = session.get();
        }
        single_module::Log(nvPresent
            ? L"MFG_PROXY_UI renderer registered api=d3d11 layer=creation-return base=NVIDIA NvPresent buffers=layer-local device=matched state=own"
            : L"MFG_PROXY_UI renderer registered api=d3d11 layer=creation-return buffers=layer-local device=matched state=own");
        return session.release();
    } catch (...) {
        RecordFailure(L"MFG_PROXY_UI DirectX 11 renderer allocation failed; original presentation retained");
        return nullptr;
    }
}

void DestroySession(Session* session) noexcept
{
    if (!session) return;
    {
        std::lock_guard lock(gSessionsMutex);
        for (auto& slot : gSessions) if (slot == session) slot = nullptr;
    }
    {
        // The proxy's method lease ends before its final release: no Present or resize can still be inside.
        std::lock_guard lock(session->mutex);
        session->retired = true;
        session->platform.RetireInput();
        session->Shutdown();
    }
    delete session;
}

bool BeginPresent(Session* session, UINT flags, bool partialUpdate) noexcept
{
    if (!session || gInsideOverlay) return false;
    std::lock_guard lock(session->mutex);
    if (session->resizing || session->retired) return false;
    ++session->presents;
    if (session->presents != 1) return true; // A nested or concurrent call forwards; one UI draw per frame.
    session->presentThread = GetCurrentThreadId();
    // A rejected DO_NOT_WAIT Present is presented again: the menu is already in that image.
    if ((flags & DXGI_PRESENT_TEST) || ((flags & DXGI_PRESENT_DO_NOT_WAIT) && session->wasStillDrawing)) return true;
    if (!Principal(session)) return true;
    gDxgiFrames.fetch_add(1, std::memory_order_relaxed);
#if MFG_UNLOCK_OVERLAY_MENU_DRAW && !MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
    if (install::InputReady() && single_module::OverlayAllowed())
    {
        try { session->Render(partialUpdate); }
        catch (...) { session->Disable(); RecordFailure(L"MFG_PROXY_UI D3D11 render exception; UI disabled for this generation"); }
    }
#else
    (void)partialUpdate;
#endif
    return true;
}

void EndPresent(Session* session, HRESULT result, UINT flags) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    if (result == S_OK && !(flags & DXGI_PRESENT_TEST) && Principal(session))
    {
        InternalScope internal;
        UINT count = 0;
        const bool available = SUCCEEDED(session->swapchain->GetLastPresentCount(&count));
        frame_telemetry::SamplePresentCounter(session->telemetryOwner, count, available);
    }
    if (!(flags & DXGI_PRESENT_TEST)) session->wasStillDrawing = result == DXGI_ERROR_WAS_STILL_DRAWING;
    if (session->presents && !--session->presents) session->presentThread = 0;
}

bool BeginResize(Session* session) noexcept
{
    if (!session) return true;
    std::lock_guard lock(session->mutex);
    // The session holds nothing once its draw returned, so a resize nested in the presenting thread's own Present
    // proceeds; another thread's is refused while that Present runs, as on D3D12.
    if (session->resizing || (session->presents && session->presentThread != GetCurrentThreadId())) return false;
    session->resizing = true;
    ui_input_coherence::InvalidateResources();
    return true;
}

void EndResize(Session* session, HRESULT result) noexcept
{
    if (!session) return;
    std::lock_guard lock(session->mutex);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(result) && SUCCEEDED(session->swapchain->GetDesc1(&desc)))
        session->presentationArea.store(uint64_t(desc.Width) * desc.Height);
    session->telemetryOwner = gNextTelemetryOwner.fetch_add(1, std::memory_order_relaxed);
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
}
