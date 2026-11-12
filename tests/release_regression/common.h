#pragma once
// Shared helpers for the bounded-overlay regression harness. Every case runs
// in a fresh child process against an exact, hash-recorded candidate DLL that
// was copied to winhttp.dll next to the test executable.
#include "single_module_status.h"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "focus_standin.h"

using Microsoft::WRL::ComPtr;

namespace harness
{
inline int gFailures = 0;

inline bool Check(bool passed, const char* name)
{
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!passed) ++gFailures;
    return passed;
}

inline void Fail(const char* name) { Check(false, name); }

// ---------------------------------------------------------------------------
// Candidate status
// ---------------------------------------------------------------------------
inline HMODULE LoadCandidate(const wchar_t* path)
{
    return LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

using QueryFn = BOOL (WINAPI*)(MfgSingleModuleStatus*);

inline QueryFn CandidateQuery(HMODULE module)
{
    return reinterpret_cast<QueryFn>(GetProcAddress(module, "MfgUnlockSingleModuleQuery"));
}

inline MfgSingleModuleStatus Status(HMODULE module)
{
    MfgSingleModuleStatus status{};
    status.size = sizeof(status);
    if (QueryFn query = CandidateQuery(module)) {
        if(!query(&status)){status.size=kMfgSingleModuleStatusSizeV2;query(&status);}
    }
    return status;
}

// Version-1 (64 byte) ABI check: the extended layout must still answer the
// previous consumer contract.
inline BOOL StatusV1(HMODULE module, MfgSingleModuleStatus& status)
{
    status = {};
    status.size = kMfgSingleModuleStatusSizeV1;
    if (QueryFn query = CandidateQuery(module)) return query(&status);
    return FALSE;
}

inline bool WaitForTerminal(HMODULE module, uint32_t timeoutMs,
    const char* name = "coordinator reaches a terminal state")
{
    for (uint32_t waited = 0;; Sleep(25), waited += 25)
    {
        const MfgSingleModuleStatus status = Status(module);
        if (status.overlayInstallState >= 2)
        {
            if (waited == 0) Sleep(0);
            return Check(true, name);
        }
        if (waited >= timeoutMs) break;
    }
    return Check(false, name);
}

// ---------------------------------------------------------------------------
// Entry snapshots
// ---------------------------------------------------------------------------
struct EntryBytes
{
    void* entry = nullptr;
    unsigned char bytes[16]{};
    void Capture(void* target) { entry = target; memcpy(bytes, target, sizeof(bytes)); }
    bool Unchanged() const
    {
        unsigned char now[16]{};
        memcpy(now, entry, sizeof(now));
        return memcmp(now, bytes, sizeof(now)) == 0;
    }
    bool Changed() const { return !Unchanged(); }
};

inline HMODULE SystemModule(const wchar_t* name)
{
    wchar_t path[32768]{};
    GetSystemDirectoryW(path, 32768);
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

// ---------------------------------------------------------------------------
// WARP presentation fixtures
// ---------------------------------------------------------------------------
struct WarpFixture
{
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    bool debugLayer = false;

    bool Initialize(bool requestDebugLayer,IDXGIAdapter* selectedAdapter=nullptr,HMODULE deviceModule=nullptr)
    {
        if (requestDebugLayer)
        {
            HMODULE d3d12 = SystemModule(L"d3d12.dll");
            using DebugFn = HRESULT (WINAPI*)(REFIID, void**);
            if (auto getDebug = reinterpret_cast<DebugFn>(
                    GetProcAddress(d3d12, "D3D12GetDebugInterface")))
            {
                ComPtr<struct ID3D12Debug> debug;
                if (SUCCEEDED(getDebug(IID_PPV_ARGS(&debug))))
                {
                    debug->EnableDebugLayer();
                    debugLayer = true;
                }
            }
        }
        ComPtr<IDXGIFactory4> factory4;
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) return false;
        if(selectedAdapter)warp=selectedAdapter;
        else if(FAILED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))))return false;
        if (FAILED(factory4.As(&factory))) return false;
        using CreateDevice=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
        const auto create=selectedAdapter?reinterpret_cast<CreateDevice>(GetProcAddress(deviceModule?deviceModule:SystemModule(L"d3d12.dll"),"D3D12CreateDevice")):&D3D12CreateDevice;
        if (!create||FAILED(create(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;
        if (debugLayer)
        {
            // The debug layer terminates the process on its final-release
            // corruption report. Releasing backbuffer references before
            // ResizeBuffers while queued presentations reference them is the
            // documented flip-model resize pattern and is handled by the
            // runtime, so the harness collects those reports through the
            // info queue instead of dying on them.
            ComPtr<struct ID3D12InfoQueue> info;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info))) && info)
            {
                info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            }
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))
            || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocator)))
            || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))
            || FAILED(commands->Close())
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return fenceEvent != nullptr;
    }

    bool SubmitAndWait()
    {
        if (FAILED(allocator->Reset()) || FAILED(commands->Reset(allocator.Get(), nullptr)))
            return false;
        return true;
    }

    bool SignalAndWait()
    {
        const UINT64 value = ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), value))) return false;
        if (fence->GetCompletedValue() >= value) return true;
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) return false;
        return WaitForSingleObject(fenceEvent, 5000) == WAIT_OBJECT_0;
    }

    ~WarpFixture()
    {
        if (fenceEvent) CloseHandle(fenceEvent);
    }
};

struct TestWindow
{
    HWND window = nullptr;
    ATOM registered = 0;
    HINSTANCE instance = nullptr;
    std::wstring className;
    WNDPROC proc = DefWindowProcW;

    TestWindow(const wchar_t* name, LONG width, LONG height, WNDPROC custom = DefWindowProcW)
        : instance(GetModuleHandleW(nullptr)), className(name), proc(custom)
    {
        WNDCLASSW description{};
        description.lpfnWndProc = custom;
        description.lpszClassName = name;
        description.hInstance = instance;
        registered = RegisterClassW(&description);
        if (registered) window = CreateWindowExW(0, name, name, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, instance, nullptr);
    }
    ~TestWindow()
    {
        if (window) DestroyWindow(window);
        if (registered) UnregisterClassW(className.c_str(), instance);
    }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
    void Show()
    {
        // bgfocus.1: a background case stays behind the user's windows, and neither a click on it nor the closing of
        // the window in front of it may make Windows activate it.
        if (focus::Virtual()) SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        if (focus::Virtual()) SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        UpdateWindow(window);
    }
    void Pump(unsigned iterations = 8)
    {
        MSG message;
        for (unsigned i = 0; i < iterations; ++i)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
    }
};

// Renders a full-screen clear of the given color into the current backbuffer
// and returns its index. The caller owns command-list reset/close discipline.
inline bool ClearBackbuffer(WarpFixture& fixture, IDXGISwapChain3* chain,
    ID3D12Resource* buffer, ID3D12DescriptorHeap* rtvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, float color[4])
{
    if (!fixture.SubmitAndWait()) return false;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    fixture.commands->ResourceBarrier(1, &barrier);
    fixture.commands->ClearRenderTargetView(rtv, color, 0, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    fixture.commands->ResourceBarrier(1, &barrier);
    return SUCCEEDED(fixture.commands->Close());
}

// Reads back the center pixel of a PRESENT-state backbuffer via a copy queue
// compatible transition on the direct queue itself.
inline UINT64 RequiredReadbackBytes(WarpFixture& fixture, ID3D12Resource* buffer)
{
    const auto desc = buffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    fixture.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    return total;
}

inline bool ReadCenterPixel(WarpFixture& fixture, ID3D12Resource* buffer,
    float out[4], ID3D12Resource* staging, UINT pixelX=UINT_MAX, UINT pixelY=UINT_MAX)
{
    if (!fixture.SubmitAndWait()) return false;
    const auto desc = buffer->GetDesc();
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = buffer;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    fixture.commands->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = staging;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 rowBytes = 0;
    fixture.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, &rowBytes, nullptr);
    destination.PlacedFootprint = footprint;
    const UINT x = pixelX==UINT_MAX ? static_cast<UINT>(desc.Width) / 2 : pixelX;
    const UINT y = pixelY==UINT_MAX ? desc.Height / 2 : pixelY;
    D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
    // Buffer destinations take a byte offset in DstX; the box selects the
    // single center source pixel and it lands at offset 0 of the staging
    // buffer's first row.
    fixture.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    D3D12_RESOURCE_BARRIER back{};
    back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back.Transition.pResource = buffer;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    fixture.commands->ResourceBarrier(1, &back);
    if (FAILED(fixture.commands->Close())) return false;
    ID3D12CommandList* executed = fixture.commands.Get();
    fixture.queue->ExecuteCommandLists(1, &executed);
    if (!fixture.SignalAndWait()) return false;
    void* mapped = nullptr;
    D3D12_RANGE range{0, 4};
    if (FAILED(staging->Map(0, &range, &mapped))) return false;
    memcpy(out, mapped, sizeof(float) * 4);
    staging->Unmap(0, nullptr);
    return true;
}
}
