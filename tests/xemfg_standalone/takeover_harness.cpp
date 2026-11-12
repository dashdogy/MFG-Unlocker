// XeMFG standalone's D3D12 takeover through MFG Unlocker loaded as winmm.dll (the release-regression
// convention). The runner puts the profiled XeSS SDK 3.0.2 pair in RTXMFG-XeSS beside it.
//   hardware  XeFG on the first hardware adapter, fed depth and motion vectors by the stand-in libxess
//             (XessFixture.dll): the chain is XeFG's, frames generate at 4x, a released chain goes and a new one
//             is taken over again.
//   warp      XeFG refuses WARP: the chain is created natively and presents. The run ends after a status written
//             since the chain's creation (oldruntime.1), so the file left behind carries the takeover attempt.
//   off       switch off: native chain; switched on afterwards, the status asks for a restart. xemfglive.3: the test
//             window never answers the nudge, so the game is remembered: the status says resize_ignored_remembered
//             and [XeMFG] ProxyBackBuffers=1 is written to RTX40MFG-UI.ini beside the harness.
//   limit     hardware with the menu's FPS limit at 60: XeLL holds XeFG's presented frames to it (the
//             game's at ~15 at 4x; the status's frame rates and xemfgFpsLimited).
//   dynamic   hardware with Dynamic MFG up to 6X and a 120 FPS target: the test scene runs far above it at
//             6X, so the count steps down (the status's xemfgDynamicActive and xemfgFrames).
//   queueoff  sapacing.1: hardware with the control key xemfgFlipQueue = 2 (the CPU pacer, no flip queue); the runner
//             compares it with hardware (the queue on auto) from the log.
//   queueon   sapacing.1: hardware with xemfgFlipQueue = 1 (the queue as on auto, set by the key).
//   live      xemfglive.1: hardware, launched with the switch off, then switched on, off and on again while it runs.
//             The chain moves to XeFG (generating at 4x), back to a native one (status off, no frames) and to XeFG
//             again, with every present and resize succeeding. Its frame-latency waitable (maximum latency 2, waited on
//             before every frame as Unreal does) must keep signalling across every move. Run with [XeMFG]
//             ProxyBackBuffers=1, as a game that ignores window size changes: every move happens at a Present, and
//             while XeFG generates the game resizes twice (800x600 and back) through the proxy's buffers.
//   liveresize  live with ProxyBackBuffers=0: MFG Unlocker resizes the window and the loop answers every size change
//             with ResizeBuffers, as a game does; the moves happen in those resizes.
//   gamexell  xellbind.1: hardware, after "the game" loaded a libxell.dll of its own first (a copy of the pair's in
//             GameXeLL\, as Cyberpunk 2077 loads its 1.1.0.6 at start): XeFG still takes the host's XeLL context and
//             generates at 4x (the log's host_xell_binding names the bound lookups and the other libxell).
// Every mode sets the key first (absent: the default, no proxy back buffers for a game not known to need them).
#include "common.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace harness;
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace
{
std::filesystem::path Here()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool WaitForStatus(const char* needle, unsigned milliseconds)
{
    const auto status = Here() / L"RTXMFG-Universal.status.json";
    for (unsigned waited = 0; waited <= milliseconds; waited += 50) {
        if (ReadText(status).find(needle) != std::string::npos) return true;
        Sleep(50);
    }
    std::printf("NOTE status: %s\n", ReadText(status).substr(0, 4000).c_str());
    return false;
}

// oldruntime.1: wait for a status file written after `since`. The worker rewrites the status about once a second, so a
// short WARP run could end before any write that followed its takeover attempt. The file is replaced whole
// (status_transport), so its last write time is that status's.
bool WaitForStatusAfter(std::filesystem::file_time_type since, unsigned milliseconds)
{
    const auto status = Here() / L"RTXMFG-Universal.status.json";
    for (unsigned waited = 0; waited <= milliseconds; waited += 50) {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(status, error);
        if (!error && written > since) return true;
        Sleep(50);
    }
    return false;
}

std::string StatusText() { return ReadText(Here() / L"RTXMFG-Universal.status.json"); }

// The status's integer field `key` (0 when absent).
uint64_t StatusNumber(const char* key)
{
    const std::string text = ReadText(Here() / L"RTXMFG-Universal.status.json");
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = text.find(needle);
    return at == std::string::npos ? 0 : std::strtoull(text.c_str() + at + needle.size(), nullptr, 10);
}

bool WriteControl(bool standalone, uint32_t frames, uint32_t limitFps = 0, bool dynamic = false, uint32_t targetFps = 0,
    uint32_t flipQueue = 0)
{
    const auto path = Here() / L"RTXMFG-Universal.json";
    const auto temporary = Here() / L"RTXMFG-Universal.json.tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,\"dynamicTargetFrameRate\":0,\"dlssgPreset\":0,"
                "\"vsyncMode\":0,\"reflexFrameLimitFps\":" << limitFps << ",\"generatedOnlyDebug\":false,\"intervalLogging\":false,"
                "\"version\":13,\"xemfgStandalone\":" << (standalone ? 1 : 0) << ",\"xemfgFrames\":" << frames
             << ",\"xemfgDynamic\":" << (dynamic ? 1 : 0) << ",\"xemfgTargetFps\":" << targetFps
             << ",\"xemfgFlipQueue\":" << flipQueue << "}\n";
        if (!file) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// xemfglive.3: [XeMFG] ProxyBackBuffers in RTX40MFG-UI.ini beside the harness (null removes the key); its value.
std::filesystem::path UiSettings() { return Here() / L"RTX40MFG-UI.ini"; }
void SetBackBufferSetting(const wchar_t* value)
{
    WritePrivateProfileStringW(L"XeMFG", L"ProxyBackBuffers", value, UiSettings().c_str());
}
std::wstring BackBufferSetting()
{
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"XeMFG", L"ProxyBackBuffers", L"", value, 16, UiSettings().c_str());
    return value;
}

// live: the window's client area at w x h (a game's own resize).
bool SetClientSize(HWND window, int w, int h)
{
    RECT outer{0, 0, w, h};
    AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
    return SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

// libxess's xess_d3d12_execute_params_t (as the host mirrors it).
struct XessCoord { uint32_t x, y; };
struct XessExecuteParams
{
    ID3D12Resource* color;
    ID3D12Resource* velocity;
    ID3D12Resource* depth;
    ID3D12Resource* exposureScale;
    ID3D12Resource* responsiveMask;
    ID3D12Resource* output;
    float jitterOffsetX, jitterOffsetY;
    float exposureScaleValue;
    uint32_t resetHistory;
    uint32_t inputWidth, inputHeight;
    XessCoord inputColorBase, inputMotionVectorBase, inputDepthBase, inputResponsiveMaskBase, reserved0, outputColorBase;
    ID3D12DescriptorHeap* descriptorHeap;
    uint32_t descriptorHeapOffset;
};
using ExecuteFn = int32_t(*)(void*, ID3D12GraphicsCommandList*, const XessExecuteParams*);

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))))
        resource.Reset();
    return resource;
}

struct Chain
{
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    UINT rtvSize = 0;
    UINT flags = 0;              // the creation flags, which ResizeBuffers must repeat
    HANDLE waitable = nullptr;   // live: the frame-latency waitable the loop waits on (closed by the caller)
};
unsigned gWaitTimeouts = 0;      // live: waits on the latency waitable that ran 1 s without a signal

bool CreateChain(WarpFixture& fixture, HWND window, Chain& chain, bool waitable = false)
{
    DXGI_SWAP_CHAIN_DESC1 desc{640, 480, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                               DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED,
                               waitable ? UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) : 0u};
    ComPtr<IDXGISwapChain1> created;
    if (FAILED(fixture.factory->CreateSwapChainForHwnd(fixture.queue.Get(), window, &desc, nullptr, nullptr, &created)) || !created ||
        FAILED(created.As(&chain.swapchain)))
        return false;
    chain.flags = desc.Flags;
    if (waitable) {
        ComPtr<IDXGISwapChain2> chain2;
        if (FAILED(chain.swapchain.As(&chain2)) || FAILED(chain2->SetMaximumFrameLatency(2))) return false;
        chain.waitable = chain2->GetFrameLatencyWaitableObject();
        if (!chain.waitable) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
    if (FAILED(fixture.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&chain.heap)))) return false;
    chain.rtvSize = fixture.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; ++i) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(chain.swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) return false;
        auto handle = chain.heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(i) * chain.rtvSize;
        fixture.device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
        chain.buffers.push_back(buffer);
    }
    return true;
}

// live: as a game does after WM_SIZE, the chain's buffers go and come back at the window's client size. True when the
// size changed (`result` is ResizeBuffers').
bool Resize(WarpFixture& fixture, HWND window, Chain& chain, UINT& width, UINT& height, HRESULT& result)
{
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const UINT w = UINT(client.right), h = UINT(client.bottom);
    if (!w || !h || (w == width && h == height)) return false;
    width = w;
    height = h;
    if (!fixture.SignalAndWait()) { result = E_FAIL; return true; }
    chain.buffers.clear();
    result = chain.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, chain.flags);
    if (FAILED(result)) return true;
    for (UINT i = 0; i < 3; ++i) {
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(result = chain.swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) return true;
        auto handle = chain.heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(i) * chain.rtvSize;
        fixture.device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
        chain.buffers.push_back(buffer);
    }
    std::printf("NOTE resized to %ux%u\n", w, h);
    return true;
}

// One base frame: the stand-in upscaler on the frame's list (the host tags its inputs there), a clear, the present.
bool Frame(WarpFixture& fixture, Chain& chain, ExecuteFn execute, XessExecuteParams* params, unsigned index, HRESULT& presented)
{
    // As Unreal: the frame starts once the latency waitable lets it (a hang here would be a game's hang).
    if (chain.waitable && WaitForSingleObjectEx(chain.waitable, 1000, TRUE) == WAIT_TIMEOUT) ++gWaitTimeouts;
    if (!fixture.SubmitAndWait()) return false;
    if (execute) execute(reinterpret_cast<void*>(uintptr_t(0x1000)), fixture.commands.Get(), params);
    const UINT back = chain.swapchain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = chain.buffers[back].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    fixture.commands->ResourceBarrier(1, &barrier);
    auto rtv = chain.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(back) * chain.rtvSize;
    const float shade = float(index % 60) / 60.0f;
    const float color[4] = {shade, 0.25f, 1.0f - shade, 1.0f};
    fixture.commands->ClearRenderTargetView(rtv, color, 0, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    fixture.commands->ResourceBarrier(1, &barrier);
    if (FAILED(fixture.commands->Close())) return false;
    ID3D12CommandList* lists[] = {fixture.commands.Get()};
    fixture.queue->ExecuteCommandLists(1, lists);
    presented = chain.swapchain->Present(0, 0);
    return fixture.SignalAndWait();
}

// live: frames for `milliseconds` (the control file is polled every 100 ms, and the resize follows a present), the
// window's messages after each, and a resize whenever its client size changed.
struct LiveCounts { unsigned resizes = 0, failedPresents = 0, failedResizes = 0, frames = 0; };
bool LiveFrames(WarpFixture& fixture, TestWindow& window, Chain& chain, ExecuteFn execute, XessExecuteParams* params,
    unsigned milliseconds, UINT& width, UINT& height, LiveCounts& counts)
{
    const ULONGLONG until = GetTickCount64() + milliseconds;
    for (unsigned i = 0; GetTickCount64() < until; ++i, ++counts.frames) {
        HRESULT presented = S_OK;
        if (!Frame(fixture, chain, execute, params, i, presented)) return false;
        if (FAILED(presented)) ++counts.failedPresents;
        window.Pump(1);
        HRESULT resized = S_OK;
        if (Resize(fixture, window.window, chain, width, height, resized)) {
            ++counts.resizes;
            if (FAILED(resized)) { ++counts.failedResizes; std::printf("NOTE ResizeBuffers 0x%08lX\n", static_cast<unsigned long>(resized)); }
        }
    }
    return true;
}

// live: XeFG's chain generating at about 4x the game's frame rate. A move to XeFG holds the game's ResizeBuffers while
// XeFG and its CUDA layers start (about 1.5 s on an RTX 4080), so present in 250 ms steps until the status shows both
// (rates at most 2 s old), up to 12 s.
bool LiveGenerating(WarpFixture& fixture, TestWindow& window, Chain& chain, ExecuteFn execute, XessExecuteParams* params,
    UINT& width, UINT& height, LiveCounts& counts, const char* when)
{
    std::printf("NOTE %s\n", when);
    bool generating = false, rates = false;
    uint64_t base = 0, output = 0, waited = 0;
    for (; waited < 12000; waited += 250) {
        if (!LiveFrames(fixture, window, chain, execute, params, 250, width, height, counts)) return false;
        const std::string text = StatusText();
        generating = text.find("\"xemfgFrames\":4") != std::string::npos
            && text.find("\"xemfgGenerating\":true") != std::string::npos;
        base = StatusNumber("xemfgBaseFpsMilli");
        output = StatusNumber("xemfgOutputFpsMilli");
        rates = base > 0 && StatusNumber("xemfgFpsAgeMs") < 2000;
        if (generating && rates && output >= base * 3 && output <= base * 5) break;
    }
    std::printf("NOTE after %llu ms of frames: base %.1f output %.1f\n", static_cast<unsigned long long>(waited),
        base / 1000.0, output / 1000.0);
    Check(generating, "the chain is XeFG's at 4x and generates");
    Check(rates, "frame rates published");
    Check(rates && output >= base * 3 && output <= base * 5, "output about 4x the game's frame rate");
    return true;
}

// buffers: the proxy's own back buffers, every move at a Present with no resize; else the window nudge and moves in the
// resize it brings.
int RunLive(WarpFixture& fixture, TestWindow& window, ExecuteFn execute, XessExecuteParams* params, HMODULE module, bool buffers)
{
    const auto wrappersBefore = Status(module).swapchainWrappersCreated;
    Chain chain;
    if (!Check(CreateChain(fixture, window.window, chain, true), "waitable chain created (maximum latency 2)")) return 1;
    Check(Status(module).swapchainWrappersCreated >= wrappersBefore + 1, "a proxy wraps the chain");
    UINT width = 640, height = 480;
    LiveCounts counts;
    if (!LiveFrames(fixture, window, chain, execute, params, 500, width, height, counts)) return Check(false, "frame submitted"), 1;
    Check(WaitForStatus("\"xemfgState\":\"off\"", 5000), "native chain while switched off");
    // On: the running chain moves to XeFG (at a Present, or in the resize MFG Unlocker asks for).
    Check(WriteControl(true, 4), "switched on while the chain runs");
    if (!LiveFrames(fixture, window, chain, execute, params, 3000, width, height, counts)) return Check(false, "frame submitted"), 1;
    if (buffers) Check(counts.resizes == 0, "no resize: the chain moved at a Present (switch on)");
    else Check(counts.resizes >= 1, "MFG Unlocker resized the window (switch on)");
    if (!LiveGenerating(fixture, window, chain, execute, params, width, height, counts, "after switching on"))
        return Check(false, "frame submitted"), 1;
    if (buffers) {
        // The game's own resizes while XeFG generates: the proxy's buffers follow the chain's size, there and back.
        const unsigned before = counts.resizes, failedBefore = counts.failedResizes;
        Check(SetClientSize(window.window, 800, 600), "the game resizes its window to 800x600");
        if (!LiveFrames(fixture, window, chain, execute, params, 1000, width, height, counts)) return Check(false, "frame submitted"), 1;
        Check(width == 800 && height == 600 && counts.resizes == before + 1, "the chain resized to 800x600");
        Check(SetClientSize(window.window, 640, 480), "the game resizes its window back to 640x480");
        if (!LiveFrames(fixture, window, chain, execute, params, 1000, width, height, counts)) return Check(false, "frame submitted"), 1;
        Check(width == 640 && height == 480 && counts.resizes == before + 2, "the chain resized back to 640x480");
        Check(counts.failedResizes == failedBefore, "both resizes through the proxy's buffers succeeded");
        if (!LiveGenerating(fixture, window, chain, execute, params, width, height, counts, "after the game's resizes"))
            return Check(false, "frame submitted"), 1;
    }
    // Off: back to a native chain (at a Present, or in the next resize).
    const unsigned resizesOn = counts.resizes;
    Check(WriteControl(false, 4), "switched off while XeFG runs");
    if (!LiveFrames(fixture, window, chain, execute, params, 2000, width, height, counts)) return Check(false, "frame submitted"), 1;
    if (buffers) Check(counts.resizes == resizesOn, "no resize: the chain moved back at a Present (switch off)");
    else Check(counts.resizes > resizesOn, "MFG Unlocker resized the window (switch off)");
    Check(WaitForStatus("\"xemfgState\":\"off\"", 5000), "the chain is native again");
    Check(WaitForStatus("\"xemfgFrames\":0", 5000), "no XeFG frames while off");
    const unsigned resizesOff = counts.resizes;
    Check(WriteControl(true, 4), "switched on again");
    if (!LiveFrames(fixture, window, chain, execute, params, 3000, width, height, counts)) return Check(false, "frame submitted"), 1;
    if (buffers) Check(counts.resizes == resizesOff, "no resize: the chain moved at a Present (switch on again)");
    else Check(counts.resizes > resizesOff, "MFG Unlocker resized the window (switch on again)");
    if (!LiveGenerating(fixture, window, chain, execute, params, width, height, counts, "after switching on again"))
        return Check(false, "frame submitted"), 1;
    Check(counts.failedPresents == 0, "every present succeeded");
    Check(counts.failedResizes == 0, "every resize succeeded");
    std::printf("NOTE resizes %u frames %u waitable timeouts %u\n", counts.resizes, counts.frames, gWaitTimeouts);
    Check(gWaitTimeouts == 0, "the latency waitable kept signalling across every move");
    CloseHandle(chain.waitable);
    chain = Chain{};
    Check(fixture.SignalAndWait(), "queue idle after release");
    Sleep(300);
    return 0;
}

int Run(const std::wstring& mode, const wchar_t* dll)
{
    const bool limit = mode == L"limit", dynamic = mode == L"dynamic";
    const bool queueOff = mode == L"queueoff", queueOn = mode == L"queueon";
    const bool liveBuffers = mode == L"live", liveResize = mode == L"liveresize", live = liveBuffers || liveResize;
    const bool gameXell = mode == L"gamexell";
    const bool hardware = mode == L"hardware" || limit || dynamic || queueOff || queueOn || live || gameXell, off = mode == L"off";
    const uint32_t flipQueue = queueOn ? 1 : queueOff ? 2 : 0;
    // xemfglive.3: read once at MFG Unlocker's first chain, so set before it loads.
    SetBackBufferSetting(liveBuffers ? L"1" : liveResize ? L"0" : nullptr);
    if (!Check(BackBufferSetting() == (liveBuffers ? L"1" : liveResize ? L"0" : L""), "[XeMFG] ProxyBackBuffers set for the mode"))
        return 1;
    if (gameXell) {
        // xellbind.1: the game's own libxell.dll, loaded before MFG Unlocker from a folder the pair search never reads.
        const auto folder = Here() / L"GameXeLL";
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const bool copied = CopyFileW((Here() / L"RTXMFG-XeSS" / L"libxell.dll").c_str(), (folder / L"libxell.dll").c_str(), FALSE) != FALSE;
        const HMODULE game = copied ? LoadLibraryExW((folder / L"libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (!Check(game != nullptr, "the game's own libxell.dll loaded first")) return 1;
        HMODULE byName = nullptr;
        Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, L"libxell.dll", &byName) && byName == game,
            "a lookup of libxell.dll by name finds the game's");
    }
    if (!Check(dynamic ? WriteControl(true, 6, 0, true, 120) : WriteControl(!off && !live, 4, limit ? 60 : 0, false, 0, flipQueue),
            "control file written"))
        return 1;
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loaded")) return 1;
    if (!Check(WaitForStatus(off || live ? "\"xemfgState\":\"off\"" : "\"xemfgState\":\"waiting\"", 20000),
            "XeMFG standalone state published"))
        return 1;
    // The stand-in upscaler loads after MFG Unlocker, as a game's upscaler DLL does: the load notification has the
    // worker hook it.
    ExecuteFn execute = nullptr;
    HMODULE fixtureModule = nullptr;
    if (hardware) {
        fixtureModule = LoadLibraryW((Here() / L"XessFixture.dll").c_str());
        execute = fixtureModule ? reinterpret_cast<ExecuteFn>(GetProcAddress(fixtureModule, "xessD3D12Execute")) : nullptr;
        if (!Check(execute != nullptr, "stand-in libxess loaded")) return 1;
        Sleep(1500);
    }
    ComPtr<IDXGIAdapter> adapter;
    if (hardware) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return Check(false, "adapter factory"), 1;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::printf("NOTE adapter %ls\n", desc.Description);
                break;
            }
        }
        if (!Check(adapter != nullptr, "hardware adapter found")) return 1;
    }
    WarpFixture fixture;
    if (!Check(fixture.Initialize(false, adapter.Get()), hardware ? "hardware device, queue and factory created"
                                                                   : "WARP device, queue and factory created"))
        return 1;
    // live: a client area of exactly 640x480, so the chain's size only changes when MFG Unlocker resizes the window.
    RECT outer{0, 0, 640, 480};
    if (live) AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
    TestWindow window(L"XemfgStandalone", outer.right - outer.left, outer.bottom - outer.top);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    window.Pump(4);

    ComPtr<ID3D12Resource> depth = Texture(fixture.device.Get(), DXGI_FORMAT_R32_FLOAT, 640, 480, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12Resource> motion = Texture(fixture.device.Get(), DXGI_FORMAT_R16G16_FLOAT, 640, 480, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!Check(depth && motion, "upscaler inputs created")) return 1;
    XessExecuteParams params{};
    params.depth = depth.Get();
    params.velocity = motion.Get();
    params.inputWidth = 640;
    params.inputHeight = 480;

    if (live) {
        const int result = RunLive(fixture, window, execute, &params, module, liveBuffers);
        std::printf("COMPLETE mode=%ls failures=%d\n", mode.c_str(), gFailures);
        return result ? result : gFailures ? 10 : 0;
    }
    const unsigned rounds = hardware && !limit && !dynamic && !queueOff && !queueOn && !gameXell ? 2 : 1;
    for (unsigned round = 0; round < rounds; ++round) {
        const auto wrappersBefore = Status(module).swapchainWrappersCreated;
        Chain chain;
        if (!Check(CreateChain(fixture, window.window, chain), round ? "chain created again after release" : "chain created")) return 1;
        // The takeover attempt (runtime load included) ran inside the creation call.
        const auto created = std::filesystem::file_time_type::clock::now();
        // The game's chain gets a proxy (a menu-less one under XeMFG, which puts the menu on XeFG's output chain).
        Check(Status(module).swapchainWrappersCreated >= wrappersBefore + 1, "a proxy wraps the chain");
        if (off) {
            // xemfglive.1: MFG Unlocker asks for a resize the test window never answers; the status then says why.
            Check(WriteControl(true, 4), "switched on after the chain");
            // Presents until the status names why (the control file is polled, and the ask follows a present).
            for (unsigned i = 0; i < 360; ++i) {
                HRESULT presented = S_OK;
                if (!Frame(fixture, chain, execute, &params, i, presented)) return Check(false, "frame submitted"), 1;
                window.Pump(1);
                Sleep(16);
                if ((i % 30) == 29 && StatusText().find("\"xemfgReason\":\"resize_ignored") != std::string::npos) break;
            }
            Check(WaitForStatus("\"xemfgState\":\"restart\"", 10000), "status asks for a restart");
            Check(WaitForStatus("\"xemfgReason\":\"resize_ignored_remembered\"", 3000), "the game did not resize, and is remembered");
            Check(BackBufferSetting() == L"1", "[XeMFG] ProxyBackBuffers=1 written for the next launch");
        }
        const unsigned frames = limit ? 150 : dynamic ? 2400 : hardware ? (round ? 120 : 700) : 120;
        unsigned failedPresents = 0;
        for (unsigned i = 0; i < frames; ++i) {
            HRESULT presented = S_OK;
            if (!Frame(fixture, chain, execute, &params, i, presented)) return Check(false, "frame submitted"), 1;
            if (FAILED(presented)) ++failedPresents;
            if ((i & 15) == 0) window.Pump(1);
        }
        Check(failedPresents == 0, "every present succeeded");
        if (dynamic) {
            Check(WaitForStatus("\"xemfgDynamicActive\":true", 5000), "Dynamic MFG runs");
            Check(WaitForStatus("\"xemfgDynamicTargetMilli\":120000", 3000), "its target is the one set");
            const uint64_t shown = StatusNumber("xemfgFrames");
            const uint64_t base = StatusNumber("xemfgBaseFpsMilli"), output = StatusNumber("xemfgOutputFpsMilli");
            std::printf("NOTE dynamic: %llux, frame rates base %.1f output %.1f\n", static_cast<unsigned long long>(shown),
                base / 1000.0, output / 1000.0);
            Check(shown >= 1 && shown < 6, "the count stepped down from 6X");
        } else if (hardware) {
            Check(WaitForStatus("\"xemfgFrames\":4", 5000), "the chain is XeFG's at 4x");
            Check(WaitForStatus("\"xemfgSource\":\"xess\"", 5000), "inputs from the stand-in XeSS upscaler");
            Check(WaitForStatus("\"xemfgGenerating\":true", 5000), "XeFG generates");
            // The frame rates for the menu's FPS row (XeFG's presents per game frame = 4).
            Check(WaitForStatus("\"xemfgBaseFpsMilli\":", 3000), "frame rates published");
            const uint64_t base = StatusNumber("xemfgBaseFpsMilli"), output = StatusNumber("xemfgOutputFpsMilli");
            std::printf("NOTE frame rates: base %.1f output %.1f\n", base / 1000.0, output / 1000.0);
            Check(base > 0 && output >= base * 3 && output <= base * 5, "output about 4x the game's frame rate");
            if (limit) {
                Check(WaitForStatus("\"xemfgFpsLimited\":true", 3000), "XeLL holds the FPS limit");
                Check(output >= 50000 && output <= 66000, "XeMFG presents about the 60 FPS limit");
            }
        } else if (!off) {
            Check(WaitForStatus("\"xemfgFrames\":0", 5000), "WARP keeps the game's own chain");
            Check(WaitForStatusAfter(created, 3000), "status written after the takeover attempt");
        }
        chain = Chain{};
        Check(fixture.SignalAndWait(), "queue idle after release");
        Sleep(300);
    }
    std::printf("COMPLETE mode=%ls failures=%d\n", mode.c_str(), gFailures);
    return gFailures ? 10 : 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::printf("usage: XemfgTakeoverHarness hardware|warp|off|limit|dynamic|queueoff|queueon|live|liveresize|gamexell <candidate dll>\n"); return 2; }
    std::printf("mode=%ls dll=%ls\n", argv[1], argv[2]);
    return Run(argv[1], argv[2]);
}
