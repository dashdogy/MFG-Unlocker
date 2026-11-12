// Real D3D12 WARP swapchain/presentation harness for the bounded overlay:
// presentation, menu rendering, input, resize and teardown. run.py lists the
// modes the release regression runs. d3d12-with-d3d11-menu adds a D3D11 device and
// window to a D3D12 game (dx11menu.1; DirectX 11 games run in D3D11PresentHarness).
#include "common.h"
#include <d3d11_1.h>
#include "existing_overlay.h"
#include "unaligned_import_fixture.h"
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <atomic>
#include <dwmapi.h>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>
#include <sl.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
extern "C" __declspec(dllimport) HRESULT WINAPI EngineCreateFactory(unsigned,REFIID,void**);

namespace
{
using namespace harness;
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

constexpr uint32_t kStateActive = 2;
unsigned receivedKeys=0;
unsigned receivedMouseButtons=0;
unsigned receivedKeyUps[256]{}; // keyrelease.1
unsigned receivedRightUps=0;
LRESULT CALLBACK GameWindowProc(HWND window,UINT message,WPARAM key,LPARAM data) {
    if(message==WM_KEYDOWN&&key=='A')++receivedKeys;
    if(message==WM_KEYUP)++receivedKeyUps[key&0xFF];
    if(message==WM_RBUTTONUP)++receivedRightUps;
    if(message>=WM_LBUTTONDOWN&&message<=WM_XBUTTONDBLCLK&&message!=WM_MOUSEWHEEL)++receivedMouseButtons;
    return DefWindowProcW(window,message,key,data);
}
struct RestoreForeground { HWND window=GetForegroundWindow();~RestoreForeground(){if(IsWindow(window))SetForegroundWindow(window);} };

struct Chain
{
    ComPtr<IDXGISwapChain3> swapchain;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    DXGI_SWAP_CHAIN_DESC1 desc{};

    bool Create(WarpFixture& fixture, HWND window, UINT width, UINT height,
        UINT bufferCount = 3, DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_FLIP_DISCARD)
    {
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = effect;
        // Waitable latency: the harness must prove queued presentations have
        // retired before backbuffer references are dropped, or the D3D12
        // debug layer fail-fasts its final-release corruption report.
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> chain;
        if (FAILED(fixture.factory->CreateSwapChainForHwnd(fixture.queue.Get(),
                window, &desc, nullptr, nullptr, &chain))
            || FAILED(chain.As(&swapchain)))
            return false;
        // A waitable-latency chain requires an explicit maximum before its
        // first present and again after every ResizeBuffers.
        ComPtr<IDXGISwapChain2> latency;
        if (FAILED(swapchain.As(&latency))
            || FAILED(latency->SetMaximumFrameLatency(bufferCount)))
            return false;
        return Acquire();
    }

    bool Acquire()
    {
        buffers.clear();
        for (UINT index = 0; index < desc.BufferCount; ++index)
        {
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return false;
            buffers.push_back(std::move(buffer));
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.BufferCount;
        ComPtr<ID3D12Device> device;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))
            || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs))))
            return false;
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        for (auto& buffer : buffers)
        {
            device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
            handle.ptr += step;
        }
        return true;
    }

    // Releasing backbuffer references requires an idle application queue
    // and a drained presentation queue: with the D3D12 debug layer active,
    // releasing a backbuffer that in-flight GPU work still references is a
    // fatal corruption report.
    static void Drain(WarpFixture& fixture, IDXGISwapChain3* chain = nullptr)
    {
        // Idle the application queue, then retire queued presentations via
        // the waitable latency object and the compositor: the overlay's
        // teardown inside ResizeBuffers performs the final backbuffer
        // release, and the D3D12 debug layer fail-fasts if presentation work
        // still references the buffer at that moment.
        fixture.SignalAndWait();
        MSG message;
        for (int pass = 0; pass < 4; ++pass)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(20);
        }
        if (chain)
        {
            ComPtr<IDXGISwapChain2> latency;
            if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&latency))) && latency)
            {
                // The handle is owned by the swap chain and must not be closed.
                HANDLE waitable = latency->GetFrameLatencyWaitableObject();
                if (waitable && waitable != INVALID_HANDLE_VALUE)
                    for (unsigned attempt = 0; attempt < descBufferCount(chain) + 1; ++attempt)
                        WaitForSingleObject(waitable, 1000);
            }
        }
        DwmFlush();
        for (int pass = 0; pass < 3; ++pass)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(20);
        }
    }

    static UINT descBufferCount(IDXGISwapChain3* chain)
    {
        DXGI_SWAP_CHAIN_DESC1 current{};
        return SUCCEEDED(chain->GetDesc1(&current)) ? current.BufferCount + 1 : 3;
    }

    bool Resize(WarpFixture& fixture, UINT width, UINT height)
    {
        Drain(fixture, swapchain.Get());
        buffers.clear();
        rtvs.Reset();
        const HRESULT resized = swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(resized))
        {
            std::printf("dbg ResizeBuffers hr=0x%08lx\n", static_cast<unsigned long>(resized));
            std::fflush(stdout);
            return false;
        }
        ComPtr<IDXGISwapChain2> latency;
        if (FAILED(swapchain.As(&latency))
            || FAILED(latency->SetMaximumFrameLatency(desc.BufferCount)))
            return false;
        desc.Width = width;
        desc.Height = height;
        return Acquire();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const
    {
        ComPtr<ID3D12Device> device;
        swapchain->GetDevice(IID_PPV_ARGS(&device));
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * step;
        return handle;
    }
};

bool RenderAndPresent(WarpFixture& fixture, Chain& chain, float color[4], UINT sync = 0,
    UINT flags = 0)
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
            chain.rtvs.Get(), chain.Rtv(index), color))
        return false;
    ID3D12CommandList* list = fixture.commands.Get();
    fixture.queue->ExecuteCommandLists(1, &list);
    if (!fixture.SignalAndWait()) return false;
    const HRESULT result = chain.swapchain->Present(sync, flags);
    return SUCCEEDED(result) || result == DXGI_STATUS_OCCLUDED;
}

// The swapchain is R8G8B8A8_UNORM: the readback carries four unorm bytes in
// the float array. Compare against the clear color converted the same way
// ClearRenderTargetView does, with a small rounding tolerance.
bool PixelMatches(const float observed[4], const float expected[4])
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(observed);
    for (int channel = 0; channel < 4; ++channel)
    {
        int value;
        const float clamped = expected[channel] < 0.0f ? 0.0f
            : expected[channel] > 1.0f ? 1.0f : expected[channel];
        value = static_cast<int>(clamped * 255.0f + 0.5f);
        if (std::abs(static_cast<int>(bytes[channel]) - value) > 2) return false;
    }
    return true;
}

bool ReadCenter(WarpFixture& fixture, Chain& chain, float out[4],UINT x=UINT_MAX,UINT y=UINT_MAX)
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC stagingDesc{};
    stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    stagingDesc.Width = RequiredReadbackBytes(fixture, chain.buffers[index].Get());
    stagingDesc.Height = 1;
    stagingDesc.DepthOrArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    if (FAILED(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &stagingDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&staging))))
        return false;
    return ReadCenterPixel(fixture, chain.buffers[index].Get(), out, staging.Get(),x,y);
}

// Renders the clear, waits for completion, reads the app-owned current
// backbuffer, then presents it. The read is always taken before the buffer is
// handed back to DXGI.
bool RenderReadPresent(WarpFixture& fixture, Chain& chain, float color[4], float out[4])
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
            chain.rtvs.Get(), chain.Rtv(index), color))
        return false;
    ID3D12CommandList* list = fixture.commands.Get();
    fixture.queue->ExecuteCommandLists(1, &list);
    if (!fixture.SignalAndWait()) return false;
    if (!ReadCenter(fixture, chain, out)) return false;
    const HRESULT result = chain.swapchain->Present(0, 0);
    return SUCCEEDED(result) || result == DXGI_STATUS_OCCLUDED;
}


// Close the status file before the caller sleeps: an open reader (no
// FILE_SHARE_DELETE) makes every atomic replace fail with ACCESS_DENIED, so a
// polling loop that sleeps with it open never sees a newer status.
std::string ReadStatusText(const std::filesystem::path& status)
{
    std::ifstream input(status);
    return std::string{std::istreambuf_iterator<char>(input), {}};
}

int RunPresentation(const wchar_t* dll, bool menuMode, uint32_t expectedTargets, bool raceStartup = false, bool coexist = false,bool dynamicFactory=false,bool streamlineStartup=false,bool wrapped=false,bool layered=false,bool reshadeWrapped=false,bool firstLaunch=false,bool unaligned=false,bool executableInputPages=false,unsigned hdrStartup=0,bool engineFactory=false,bool unalignedGraphics=false,bool adapterParent=false,bool legacyFactory=false,bool alignedInput=false)
{
    HMODULE foreign=nullptr;
    using ForeignLookup=FARPROC(WINAPI*)(HMODULE,LPCSTR);
    using ForeignClip=BOOL(WINAPI*)(const RECT*);
    ForeignLookup foreignLookup=nullptr;
    ForeignClip foreignClip=nullptr;
    FARPROC originalFactory=nullptr;
    if(unaligned){
        foreign=LoadLibraryW(L"ScopedCallerFixture.dll");
        foreignLookup=foreign?reinterpret_cast<ForeignLookup>(GetProcAddress(foreign,"ForeignLookup")):nullptr;
        foreignClip=foreign?reinterpret_cast<ForeignClip>(GetProcAddress(foreign,"ForeignClip")):nullptr;
        originalFactory=GetProcAddress(SystemModule(L"dxgi.dll"),"CreateDXGIFactory1");
        if(!Check(foreignLookup&&foreignClip&&originalFactory,"foreign-caller fixture and original factory captured"))return 1;
        if(!Check(unaligned_fixture::Prepare(unalignedGraphics,alignedInput),alignedInput
                ?"aligned application input import table relocated to a data page"
                :"unaligned application import tables prepared"))return 1;
    }
    // ---------------------------------------------------------------- setup
    // Enable validation before any candidate/fixture device can be retained.
    // Turning it on after slInit's probe would invalidate an existing device.
    if(coexist||streamlineStartup){
        using Debug=HRESULT(WINAPI*)(REFIID,void**);
        const auto getDebug=reinterpret_cast<Debug>(GetProcAddress(SystemModule(L"d3d12.dll"),"D3D12GetDebugInterface"));
        ComPtr<ID3D12Debug> debug;if(getDebug&&SUCCEEDED(getDebug(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    }
    ExistingOverlay existing;
    if (coexist && !Check(existing.Load(), "pre-existing overlay installed on native exports and methods")) return 1;
    if(wrapped){
        const auto set=reinterpret_cast<void(WINAPI*)(unsigned)>(GetProcAddress(existing.module,"OverlayFixtureWrapper"));
        if(!Check(set!=nullptr,"outer COM fixture available"))return 1;set(hdrStartup==2?6:reshadeWrapped?5:layered?4:1);
    }
    auto overlayIntact=existing.intact;
    if(coexist&&unalignedGraphics){
        overlayIntact=reinterpret_cast<ExistingOverlay::Install>(GetProcAddress(existing.module,"OverlayFixtureMethodCodeIntact"));
        if(!Check(overlayIntact!=nullptr,"method-hook preservation probe available for chained public entries"))return 1;
    }
    if(unalignedGraphics&&executableInputPages)
        if(!Check(unaligned_fixture::MakeGraphicsPagesExecutable(),"MSFS-shaped executable graphics import page prepared"))return 1;
    ComPtr<IDXGIAdapter> selectedAdapter;
    std::array<void*,12> adapterTable{};
    using Parent=HRESULT(WINAPI*)(IDXGIObject*,REFIID,void**);
    Parent cachedParent=nullptr,foreignParent=nullptr;
    unsigned(WINAPI* parentOverlayCalls)()=nullptr;
    if(adapterParent){
        // MSFS selects its native adapter before slInit, then gets the factory
        // from that adapter after D3D12CreateDevice. Retain that exact ordering.
        ComPtr<IDXGIFactory4> earlyFactory;
        if(!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&earlyFactory)))
            &&SUCCEEDED(earlyFactory->EnumWarpAdapter(IID_PPV_ARGS(&selectedAdapter))),"native adapter selected before candidate startup"))return 1;
        memcpy(adapterTable.data(),*reinterpret_cast<void***>(selectedAdapter.Get()),sizeof(adapterTable));
        cachedParent=reinterpret_cast<Parent>(adapterTable[6]);
        if(!foreign)foreign=LoadLibraryW(L"ScopedCallerFixture.dll");
        foreignParent=reinterpret_cast<Parent>(GetProcAddress(foreign,"ForeignParent"));
        if(!Check(foreignParent!=nullptr,"foreign adapter-parent fixture available"))return 1;
        if(coexist){
            const auto install=reinterpret_cast<BOOL(WINAPI*)(IDXGIObject*)>(GetProcAddress(foreign,"InstallParentOverlay"));
            parentOverlayCalls=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(foreign,"ParentOverlayCalls"));
            if(!Check(install&&parentOverlayCalls&&install(selectedAdapter.Get()),"pre-existing adapter-parent overlay installed"))return 1;
        }
    }
    HMODULE module = raceStartup ? LoadCandidate(dll) : nullptr;
    HMODULE interposer=nullptr;
    HMODULE deviceModule=nullptr;
    ComPtr<IDXGIFactory2> engineCreated;
    if(engineFactory){
        if(!Check(SUCCEEDED(EngineCreateFactory(1,IID_PPV_ARGS(&engineCreated)))
            && Status(module).factoryWrappersCreated==1 && Status(module).overlayInstallState==2,
            "renderer DLL factory activates UI before any executable factory call"))return 1;
    }
    if (raceStartup && !Check(module != nullptr, "candidate loaded before the first factory/device")) return 1;
    if(unaligned&&!unalignedGraphics){
        const auto status=std::filesystem::current_path()/L"RTXMFG-Universal.status.json";
        bool ready=false;
        for(unsigned i=0;i<320&&!ready;++i){
            ready=ReadStatusText(status).find("\"mainResolverDiscoveryInstalled\":true")!=std::string::npos;
            if(!ready)Sleep(25);
        }
        if(!Check(ready,alignedInput?"main resolver becomes ready through its aligned IAT"
                :"unaligned main resolver becomes ready without rewriting its IAT"))return 1;
        if(executableInputPages&&!Check(unaligned_fixture::MakeInputPagesExecutable(),alignedInput
                ?"aligned input import page becomes executable after resolver startup as in MSFS Store"
                :"input import pages become executable after resolver startup as in GTA"))return 1;
        Check(unaligned_fixture::Unchanged(),alignedInput?"aligned input import bytes preserved before swapchain"
            :"unaligned resolver import bytes preserved");
        Check(foreignLookup(SystemModule(L"dxgi.dll"),"CreateDXGIFactory1")==originalFactory,"foreign DLL resolver caller remains transparent");
        std::atomic<bool> correct{true};
        auto kernel=GetModuleHandleW(L"kernel32.dll");
        const auto tick=foreignLookup(kernel,"GetTickCount64");
        std::vector<std::thread> workers;
        for(unsigned t=0;t<4;++t)workers.emplace_back([&]{for(unsigned n=0;n<1000;++n)if(GetProcAddress(kernel,"GetTickCount64")!=tick)correct.store(false);});
        for(auto& worker:workers)worker.join();
        Check(correct.load(),"concurrent unrelated resolver calls preserve native results");
    }
    if(streamlineStartup){
        wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
        const auto path=std::filesystem::path(exe).parent_path()/L"sl.interposer.dll";
        HMODULE stub=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!Check(stub!=nullptr,"local Streamline ABI fixture loaded"))return 1; if(!unalignedGraphics)interposer=stub;
        if(adapterParent)deviceModule=stub;
        using Init=sl::Result(*)(const sl::Preferences&,uint64_t);
        const auto init=reinterpret_cast<Init>(GetProcAddress(stub,"slInit"));
        Check(init&&Status(module).overlayInstallState==1,"slInit lookup does no graphics work");
        sl::Preferences prefs{};const auto flags=prefs.flags;
        const auto result=init?init(prefs,sl::kSDKVersion):sl::Result::eErrorNotInitialized;
        auto calls=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(stub,"FixtureInitCalls"));
        Check(result==sl::Result::eOk&&calls&&calls()==1&&prefs.flags==flags,"slInit result, single original call and caller preferences preserved");
        if(!Check(Status(module).factoryWrappersCreated==0&&Status(module).swapchainWrappersCreated==0&&(!coexist||existing.count(4)==0),"slInit creates no graphics objects or swapchain hooks"))return 1;
        if(coexist)Check(overlayIntact(),"slInit startup preserved pre-existing overlay method hooks");
    }
    if(unalignedGraphics){
        using ForeignFactory=HRESULT(WINAPI*)(unsigned,UINT,REFIID,void**);
        auto foreignFactory=reinterpret_cast<ForeignFactory>(foreignLookup(foreign,"ForeignFactory"));
        if(!Check(foreignFactory!=nullptr,"foreign middleware factory fixture available"))return 1;
        for(unsigned api=0;api<3;++api){
            const auto before=Status(module);
            ComPtr<IDXGIFactory> object;
            if(!Check(SUCCEEDED(foreignFactory(api,0,IID_PPV_ARGS(&object)))&&object
                &&Status(module).factoryGatewayCalls==before.factoryGatewayCalls,
                "foreign factory call bypasses the application-only gateway"))return 1;
            object.Reset();
            const auto prior=coexist?existing.count(api):0;
            const auto hr=api==0?CreateDXGIFactory(IID_PPV_ARGS(&object)):
                api==1?CreateDXGIFactory1(IID_PPV_ARGS(&object)):CreateDXGIFactory2(0,IID_PPV_ARGS(&object));
            if(!Check(SUCCEEDED(hr)&&object&&Status(module).factoryWrappersCreated==before.factoryWrappersCreated+1,
                "direct unaligned graphics import returns a proxy before startup continues"))return 1;
            if(coexist)Check(existing.count(api)==prior+1,"pre-existing public factory hook is forwarded exactly once");
        }
        Check(unaligned_fixture::Unchanged(),"direct graphics startup preserves unaligned IAT bytes and protection");
        if(executableInputPages)Check(unaligned_fixture::MakeInputPagesExecutable(),"input pages become executable before first swapchain");
    }
    if(dynamicFactory){
        using Factory=HRESULT(WINAPI*)(REFIID,void**);
        const auto factory=reinterpret_cast<Factory>(GetProcAddress(SystemModule(L"dxgi.dll"),"CreateDXGIFactory1"));
        Check(factory&&Status(module).overlayInstallState==1,"dynamic export resolution performs no installation");
        ComPtr<IDXGIFactory1> created;
        if(!Check(factory&&SUCCEEDED(factory(IID_PPV_ARGS(&created)))&&Status(module).overlayInstallState==2,"dynamic factory gateway completes startup before returning to game"))return 1;
    }
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true,selectedAdapter.Get(),deviceModule), "WARP device, queue and factory created"
            " (debug layer when available)"))
        return 1;
    if (!fixture.debugLayer)
        std::printf("NOTE D3D12 debug layer unavailable on this machine; validation reduced\n");
    if(deviceModule){
        const auto count=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(deviceModule,"FixtureDeviceCalls"));
        const auto adapter=reinterpret_cast<IUnknown*(WINAPI*)()>(GetProcAddress(deviceModule,"FixtureDeviceAdapter"));
        Check(count&&adapter&&count()==1&&adapter()==selectedAdapter.Get(),"interposer device creation receives original adapter exactly once");
    }

    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp", 640, 480,GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    if (firstLaunch)
    {
        const DWORD other = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD own = GetCurrentThreadId();
        const bool attached = own != other && AttachThreadInput(own, other, TRUE);
        SetForegroundWindow(window.window); SetFocus(window.window);
        if (attached) AttachThreadInput(own, other, FALSE);
        if (!Check(GetForegroundWindow() == window.window, "first-launch test owns foreground")) return 2;
    }

    if (!module) module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate winhttp.dll loads")) return 1;
    // Invoke the ordinary imported API; never synchronize through private status.
    ComPtr<IDXGIFactory1> applicationFactory;
    ComPtr<IDXGIFactory> legacyApplicationFactory;
    if (legacyFactory) {
        const auto before=Status(module).factoryWrappersCreated;
        if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&legacyApplicationFactory)))
            &&legacyApplicationFactory&&Status(module).factoryWrappersCreated==before+1,
            "Witcher-shaped IDXGIFactory creation returns an owned proxy")) return 1;
        // Reproduce the game's direct newer-vtable calls without a preceding
        // QueryInterface. The cast is intentional regression-fixture behavior.
        auto* newer=reinterpret_cast<IDXGIFactory1*>(legacyApplicationFactory.Get());
        ComPtr<IDXGIAdapter1> adapter;
        if (!Check(SUCCEEDED(newer->EnumAdapters1(0,&adapter))&&adapter,
            "EnumAdapters1 on the creation pointer acquires revision 1 on demand")) return 1;
        Check(newer->IsCurrent()!=FALSE,"IsCurrent reuses the acquired revision 1");
    } else if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory))), "real factory call returns after initialization")) return 1;
    // A candidate without the version-2 status ABI (the earlier diagnostic)
    // is exercised as a baseline control: forwarding and presentation still
    // run, but the bounded assertions are reported as unavailable.
    MfgSingleModuleStatus probe{};
    probe.size = sizeof(probe);
    const QueryFn query = CandidateQuery(module);
    const bool boundedAbi = query && query(&probe) && probe.version == 3;
    if (!boundedAbi) std::printf("NOTE version-2 status ABI absent; running as baseline control\n");
    if (boundedAbi && !raceStartup)
    {

        const MfgSingleModuleStatus installed = Status(module);
        std::printf("STARTUP state=%u reason=%u targets=%u\n", installed.overlayInstallState, installed.overlayInstallReason, installed.overlayActivatedTargets);
        if (installed.overlayInstallState != kStateActive) Sleep(1500); // Preserve failure diagnostics, never gates success.
        if (!Check(installed.overlayInstallState == kStateActive, "bounded hooks active"))
            return 1;
        Check(installed.overlayActivatedTargets <= 5,
            "only optional application input imports are published");
        Check(installed.overlaySuspendedThreads == 0, "UI installation never suspended process threads");
    }
    // Use a real post-load factory. The old harness accidentally retained a
    // pre-install factory; shared-table changes masked that coverage error.
    if(adapterParent){
        const auto before=Status(module).factoryWrappersCreated;
        ComPtr<IDXGIFactory2> foreignFactory;
        if(!Check(SUCCEEDED(foreignParent(selectedAdapter.Get(),IID_PPV_ARGS(&foreignFactory)))
            &&Status(module).factoryWrappersCreated==before,"foreign GetParent caller retains native behavior"))return 1;
        const GUID invalid={0x11223344,0x5566,0x7788,{1,2,3,4,5,6,7,8}};
        void* foreignOutput=nullptr;void* ownOutput=nullptr;
        const auto foreignResult=foreignParent(selectedAdapter.Get(),invalid,&foreignOutput);
        const auto ownResult=cachedParent(selectedAdapter.Get(),invalid,&ownOutput);
        Check(foreignResult==ownResult&&!foreignOutput&&!ownOutput&&Status(module).factoryWrappersCreated==before,"unsupported parent IID preserves native HRESULT and output");
        const auto prior=parentOverlayCalls?parentOverlayCalls():0;
        fixture.factory.Reset();
        if(!Check(SUCCEEDED(cachedParent(selectedAdapter.Get(),IID_PPV_ARGS(&fixture.factory)))
            &&Status(module).factoryWrappersCreated==before+1,"cached native adapter GetParent returns the overlay factory"))return 1;
        if(parentOverlayCalls)Check(parentOverlayCalls()==prior+1,"existing parent hook forwarded exactly once");
        Check(!memcmp(adapterTable.data(),*reinterpret_cast<void***>(selectedAdapter.Get()),sizeof(adapterTable)),"native adapter pointer and vtable entries preserved");
    }else if(interposer){
        using Factory=HRESULT(WINAPI*)(REFIID,void**);
        auto create=reinterpret_cast<Factory>(GetProcAddress(interposer,"CreateDXGIFactory1"));
        Check(create&&SUCCEEDED(create(IID_PPV_ARGS(&fixture.factory))),"Streamline exported factory gateway returns owned proxy");
    }else if(engineFactory){fixture.factory=engineCreated;engineCreated.Reset();}
    else if(legacyFactory)fixture.factory=reinterpret_cast<IDXGIFactory2*>(legacyApplicationFactory.Get());
    else Check(SUCCEEDED(applicationFactory.As(&fixture.factory)),"use intercepted application factory");
    Chain chain;
    if (coexist) {
        Check(overlayIntact(), unalignedGraphics?"candidate preserves method hooks while chaining public factory entries":"candidate preserved every existing overlay code patch");
        existing.reset();
    }
    if (!Check(chain.Create(fixture, window.window, 640, 480, menuMode ? 2 : 3,
            menuMode ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD),
            menuMode ? "flip-sequential swapchain created (menu content validation)"
                     : "flip-model swapchain created"))
        return 1;

    if(hdrStartup){
        // GTA calls the public revision-4 method on its creation-return pointer
        // before requesting revision 4 through the proxy's QueryInterface.
        // Exercise that exact ordering before any Present can initialize UI.
        auto* direct=static_cast<IDXGISwapChain4*>(chain.swapchain.Get());
        const HRESULT directResult=direct->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE,0,nullptr);
        ComPtr<IDXGISwapChain4> requested;
        const HRESULT queried=chain.swapchain.As(&requested);
        if(hdrStartup==2){
            Check(directResult==E_NOINTERFACE,"uncached unsupported HDR method fails closed without dereferencing null");
            Check(queried==E_NOINTERFACE&&!requested,"unsupported revision 4 remains unadvertised");
        }else{
            Check(directResult==S_OK,"uncached startup HDR clear forwards before the first Present");
            Check(queried==S_OK&&requested,"explicit revision 4 lookup remains supported after first-use acquisition");
            if(requested)Check(requested->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE,0,nullptr)==directResult,"cached HDR forwarding preserves the native result");
        }
    }

    if (raceStartup)
    {
        // Applications do not call the private coordinator status ABI before
        // creating their swapchains. Exercise that ordering explicitly.
        if (!Check(boundedAbi, "startup regression requires the real coordinator ABI")
            || !Check(Status(module).overlayInstallState == kStateActive, "initialization complete at factory return")) return 1;
        auto state = Status(module);
        std::printf("STARTUP state=%u reason=%u targets=%u\n", state.overlayInstallState,
            state.overlayInstallReason, state.overlayActivatedTargets);
    }

    // ------------------------------------------------- known content + read
    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    for (int frame = 0; frame < 5; ++frame)
        if (!Check(RenderAndPresent(fixture, chain, teal), "frame rendered and presented"))
            return 1;
    float observed[4]{};
    if (firstLaunch)
    {
        Check(Status(module).overlayVisible == 1, "clean first launch opens full menu without a hotkey");
        float pixel[4]{};
        Check(ReadCenter(fixture,chain,pixel)&&!PixelMatches(pixel,teal), "automatic menu changes presented center pixels");
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        const auto ini=std::filesystem::path(exe).parent_path()/L"RTX40MFG-UI.ini";
        Check(GetPrivateProfileIntW(L"Overlay",L"FirstLaunchMenuShown",0,ini.c_str())==1,"first-launch marker persisted after drawing menu");
        PostMessageW(window.window,WM_KEYDOWN,VK_BACK,0x000E0001);
        PostMessageW(window.window,WM_KEYUP,VK_BACK,0xC0E0001);
        window.Pump(4);
        for(int i=0;i<3;++i) RenderAndPresent(fixture,chain,teal);
        Check(Status(module).overlayVisible==0,"Backspace closes automatic menu");
    }
    else if(menuMode){float hint[4]{};Check(ReadCenter(fixture,chain,hint,24,24)&&!PixelMatches(hint,teal),"startup hint visibly composited into retained backbuffer");}
    if (!Check(RenderReadPresent(fixture, chain, teal, observed),
            "frame rendered, read back before presentation, and presented"))
        return 1;
    Check(PixelMatches(observed, teal), "backbuffer readback matches the rendered clear color");
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after presents");

    // ----------------------------------------------------- Present1 + ABI
    // Full-frame Present1.
    {
        std::printf("dbg present1 block enter\n"); std::fflush(stdout);
        const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
        std::printf("dbg index=%u\n", index); std::fflush(stdout);
        float violet[4] = {0.55f, 0.20f, 0.70f, 1.0f};
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = chain.Rtv(index);
        std::printf("dbg rtv resolved\n"); std::fflush(stdout);
        if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
                chain.rtvs.Get(), handle, violet))
            return 1;
        std::printf("dbg clear done\n"); std::fflush(stdout);
        ID3D12CommandList* list = fixture.commands.Get();
        fixture.queue->ExecuteCommandLists(1, &list);
        fixture.SignalAndWait();
        // Note: on this Windows build, native WARP Present1 with a null
        // DXGI_PRESENT_PARAMETERS pointer faults inside dxgi with no hooking
        // present (verified by the standalone ReadbackProbe control). The
        // overlay forwards the pointer unchanged; the harness therefore
        // presents a valid empty parameter structure.
        DXGI_PRESENT_PARAMETERS empty{};
        std::printf("dbg calling Present1\n"); std::fflush(stdout);
        const HRESULT full = chain.swapchain->Present1(0, 0, &empty);
        std::printf("dbg present1 returned 0x%08lx\n", static_cast<unsigned long>(full)); std::fflush(stdout);
        Check(SUCCEEDED(full) || full == DXGI_STATUS_OCCLUDED, "Present1 full-frame succeeds");
    }
    // Exact metadata forwarding on a controlled flip-sequential fixture:
    // legal combinations succeed and the documented invalid combination is
    // rejected by DXGI itself. A layer that dropped or rewrote the metadata
    // would change these outcomes.
    {
        TestWindow metadataWindow(L"BoundedMetadata", 320, 240);
        Chain metadataChain;
        if (!Check(metadataChain.Create(fixture, metadataWindow.window, 320, 240, 2,
                DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL), "metadata fixture chain created"))
            return 1;
        float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        for (int i = 0; i < 3; ++i) RenderAndPresent(fixture, metadataChain, gray);
        // Metadata forwarding on this Windows build: a Present1 carrying a
        // scroll rect plus offset succeeds through the activated detour,
        // proving both pointers reach DXGI exactly. Dirty-rect-only presents
        // and the scroll-without-offset rejection documented for earlier
        // Windows builds are not deterministic here (the standalone
        // ReadbackProbe control records INVALID_CALL, S_OK, and native
        // faults for those inputs with no DLL loaded), so those rejection
        // assertions are reported as unavailable rather than relied upon.
        RECT scroll{120, 120, 240, 240};
        POINT offset{8, 12};
        DXGI_PRESENT_PARAMETERS withScroll{};
        withScroll.pScrollRect = &scroll;
        withScroll.pScrollOffset = &offset;
        const HRESULT scrolled = metadataChain.swapchain->Present1(0, 0, &withScroll);
        if(coexist){FixturePresent seen{};existing.readPresent(&seen);Check(seen.kind==8&&seen.chain==FixtureNativeIdentity(metadataChain.swapchain.Get())&&seen.metadata==&withScroll&&seen.scroll==withScroll.pScrollRect&&seen.offset==withScroll.pScrollOffset&&seen.dirty==withScroll.pDirtyRects&&seen.dirtyCount==withScroll.DirtyRectsCount&&seen.sync==0&&seen.flags==0&&seen.result==scrolled,"Present1 preserves exact pointers, metadata, sync, flags and HRESULT through existing overlay");}
        Check(SUCCEEDED(scrolled) || scrolled == DXGI_STATUS_OCCLUDED,
            "Present1 forwards scroll-rect and scroll-offset metadata and succeeds");
        std::printf(
            "NOTE Present1 dirty-only and scroll-without-offset rejection checks are"
            " not deterministic on this Windows build and are not asserted\n");
        std::fflush(stdout);

        // One more full-frame metadata-free Present1 through the detour for
        // balanced fixture activity, then release.
        DXGI_PRESENT_PARAMETERS full{};
        metadataChain.swapchain->Present1(0, 0, &full);
        Chain::Drain(fixture, metadataChain.swapchain.Get());
    }
    // DXGI_PRESENT_TEST is forwarded as a test, never a real present.
    {
        const UINT before = 0;
        UINT count = before;
        chain.swapchain->GetLastPresentCount(&count);
        const HRESULT test = chain.swapchain->Present(0, DXGI_PRESENT_TEST);
        if(coexist){FixturePresent seen{};existing.readPresent(&seen);Check(seen.kind==7&&seen.chain==FixtureNativeIdentity(chain.swapchain.Get())&&seen.sync==0&&seen.flags==DXGI_PRESENT_TEST&&seen.result==test,"Present TEST preserves chain, flags, sync and HRESULT through existing overlay");}
        UINT after = 0;
        chain.swapchain->GetLastPresentCount(&after);
        Check(SUCCEEDED(test) && after == count, "DXGI_PRESENT_TEST forwarded without presenting");
    }
    if(coexist){
        auto busy=reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(existing.module,"OverlayFixtureStillDrawing"));
        if(!Check(busy!=nullptr,"deterministic nonblocking-present fixture available"))return 1;
        busy(TRUE);
        Check(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==DXGI_ERROR_WAS_STILL_DRAWING,"first rejected nonblocking Present preserves HRESULT");
        const auto beforeBusy=Status(module);
        Check(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==DXGI_ERROR_WAS_STILL_DRAWING,"repeated rejected nonblocking Present preserves HRESULT");
        const auto afterBusy=Status(module);
        Check(beforeBusy.renderedFrames==afterBusy.renderedFrames&&beforeBusy.dxgiFrames==afterBusy.dxgiFrames,"repeated rejected nonblocking Present submits no duplicate UI frame");
        busy(FALSE);
        Check(SUCCEEDED(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)),"nonblocking Present recovers after original accepts work");
    }
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after Present1");

    // -------------------------------------------------------------- resize
    const uint32_t before = Status(module).dxgiFrames;
    for (uint32_t round = 0; round < 3; ++round)
    {
        const UINT width = 320 + 64 * round;
        const UINT height = 240 + 48 * round;
        std::printf("dbg resize round %u enter\n", round); std::fflush(stdout);
        if (!Check(chain.Resize(fixture, width, height), "ResizeBuffers with backbuffer release"))
            return 1;
        float amber[4] = {0.80f, 0.55f, 0.15f, 1.0f};
        float pixel[4]{};
        if (!Check(RenderReadPresent(fixture, chain, amber, pixel), "frame after resize"))
            return 1;
        Check(PixelMatches(pixel, amber), "content after resize matches the new clear color");
    }

    // ------------------------------------------------ destroy and recreate
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    if (!Check(chain.Create(fixture, window.window, 512, 384), "swapchain recreated"))
        return 1;
    float green[4] = {0.15f, 0.70f, 0.25f, 1.0f};
    Check(RenderAndPresent(fixture, chain, green), "presented on the recreated chain");
    if (menuMode) Check(Status(module).overlayVisible==0,"closed menu stays closed across swapchain recreation");

    // ----------------------------------------------- two independent chains
    {
        TestWindow second(L"BoundedWarp2", 256, 256);
        Chain other;
        if (!Check(other.Create(fixture, second.window, 256, 256, 2),
                "second independent swapchain created"))
            return 1;
        std::mutex step;
        std::thread presenter([&] {
            std::lock_guard lock(step);
            float red[4] = {0.75f, 0.15f, 0.15f, 1.0f};
            for (int i = 0; i < 4; ++i)
                if (!RenderAndPresent(fixture, other, red)) return;
        });
        {
            std::lock_guard lock(step);
            float blue[4] = {0.15f, 0.25f, 0.80f, 1.0f};
            for (int i = 0; i < 4; ++i)
                if (!RenderAndPresent(fixture, chain, blue)) return 1;
        }
        presenter.join();
        float red[4]{}, blue[4]{};
        const float redColor[4] = {0.75f, 0.15f, 0.15f, 1.0f};
        const float blueColor[4] = {0.15f, 0.25f, 0.80f, 1.0f};
        Check(RenderReadPresent(fixture, other, const_cast<float*>(redColor), red)
                && PixelMatches(red, redColor),
            "independent chain A retained its own content");
        Check(RenderReadPresent(fixture, chain, const_cast<float*>(blueColor), blue)
                && PixelMatches(blue, blueColor),
            "independent chain B retained its own content");
    }
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after concurrency");

    // ------------------------------------------------------- tracked count
    const uint32_t after = Status(module).dxgiFrames;
    if (boundedAbi)
        Check(after > before, "principal-chain presentation telemetry counted");
    else
        std::printf("NOTE baseline control: skipping bounded telemetry assertion\n");

    // ----------------------------------------------------------- menu mode
    if (menuMode)
    {
        // Foreground ownership is a setup requirement for the real capture gate.
        const DWORD ourThread=GetCurrentThreadId();
        // Draw several frames so the overlay initializes its renderer on the
        // tracked principal chain, then toggle the menu through the window's
        // keyboard path (default hotkey VK_BACK) and present again.
        float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
        for (int i = 0; i < 8; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(2);
        }
        const uint64_t renderedBefore = Status(module).renderedFrames;
        Check(Status(module).dxgiFrames > 0, "presentation tracked on the principal chain");

        WPARAM key = 0;
        {
            MfgSingleModuleStatus status = Status(module);
            key = status.shortcutBindingActive ? status.shortcutVirtualKey : VK_BACK;
        }
        window.Pump(4);
        const DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        const bool attached=foregroundThread!=ourThread&&AttachThreadInput(ourThread,foregroundThread,TRUE);
        BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
        if(attached)AttachThreadInput(ourThread,foregroundThread,FALSE);
        window.Pump(2);
        Check(GetForegroundWindow()==window.window,"menu input test owns foreground window");
        PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
        PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
        for (int i = 0; i < 12; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(4);
        }
        MfgSingleModuleStatus menu = Status(module);
        Check(menu.overlayVisible == 1, "menu hotkey made the overlay visible");
        Check(menu.renderedFrames > renderedBefore,
            "menu rendering executed actual UI draw submissions");

        // Output content: FLIP_SEQUENTIAL preserves what each present
        // composited, so after the queue drains the retained backbuffers
        // carry base+UI. Read without rendering anything new.
        bool differs = false;
        for (int attempt = 0; attempt < 6 && !differs; ++attempt)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(3);
            Sleep(120);
            float pixel[4]{};
            if (ReadCenter(fixture, chain, pixel))
                differs = !PixelMatches(pixel, base);
        }
        Check(differs, "menu visibly composited into the presented backbuffer");

        // Input capture and release: the input exports stay functional while
        // the menu is open, and the cursor clip is released after closing.
        // The content readback above may span compositor waits. Re-establish
        // and verify foreground ownership at the actual message boundary;
        // focus can change independently of the renderer during that time.
        const DWORD inputForegroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        const bool inputAttached=inputForegroundThread!=ourThread&&AttachThreadInput(ourThread,inputForegroundThread,TRUE);
        SetForegroundWindow(window.window);SetFocus(window.window);
        if(inputAttached)AttachThreadInput(ourThread,inputForegroundThread,FALSE);
        if(!Check(GetForegroundWindow()==window.window,"keyboard capture check owns foreground at message boundary"))return 2;
        const SHORT async = GetAsyncKeyState('A');
        const SHORT state = GetKeyState('B');
        std::printf("NOTE input forwarding GetAsyncKeyState=%d GetKeyState=%d\n",
            static_cast<int>(async), static_cast<int>(state));
        Check(async==0&&state==0,"input exports suppress game keys while menu is open");
        if(unaligned){
            RECT testClip{30,30,230,230}, observed{};
            const BOOL foreignResult=foreignClip(&testClip);
            const BOOL observedResult=GetClipCursor(&observed);
            foreignClip(nullptr);
            Check(foreignResult&&observedResult&&EqualRect(&testClip,&observed),"foreign DLL cursor calls bypass game-only input suppression");
            Check(unaligned_fixture::Unchanged(),"unaligned input import bytes preserved during menu rendering");
        }
        unsigned keysBefore=receivedKeys;
        SendMessageW(window.window,WM_KEYDOWN,'A',0x001E0001);
        Check(receivedKeys==keysBefore,"open menu captures keyboard message before game WndProc");
        RECT probe{0, 0, 200, 200};
        Check(ClipCursor(&probe) && ClipCursor(nullptr), "ClipCursor forwards and releases");

        PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
        PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
        for (int i = 0; i < 6; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(3);
        }
        Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
        keysBefore=receivedKeys;
        SendMessageW(window.window,WM_KEYDOWN,'A',0x001E0001);
        Check(receivedKeys==keysBefore+1,"closed menu returns keyboard message to game WndProc");

        // keyrelease.1: a key and a mouse button held in the game as the menu opens are released in the game when
        // let go with the menu open. A key pressed and released inside the menu, or released with no press the game
        // saw, stays with the menu. Sent synchronously, so no focus change can come between press and release.
        {
            const DWORD heldForegroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
            const bool heldAttached=heldForegroundThread!=ourThread&&AttachThreadInput(ourThread,heldForegroundThread,TRUE);
            SetForegroundWindow(window.window);SetFocus(window.window);
            if(heldAttached)AttachThreadInput(ourThread,heldForegroundThread,FALSE);
            Check(GetForegroundWindow()==window.window,"held-input check owns foreground window");
            SendMessageW(window.window,WM_KEYDOWN,'C',0x002E0001);
            SendMessageW(window.window,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(20,20));
            SendMessageW(window.window,WM_KEYDOWN,key,0x000E0001);
            SendMessageW(window.window,WM_KEYUP,key,0xC00E0001);
            Check(Status(module).overlayVisible==1,"hotkey opened the menu over a held key and mouse button");
            const unsigned upsC=receivedKeyUps['C'],upsD=receivedKeyUps['D'],rightUps=receivedRightUps;
            SendMessageW(window.window,WM_KEYUP,'D',0xC0200001);
            Check(receivedKeyUps['D']==upsD,"open menu keeps the release of a key the game never saw pressed");
            SendMessageW(window.window,WM_KEYUP,'C',0xC02E0001);
            Check(receivedKeyUps['C']==upsC+1,"open menu passes the release of a key held as it opened");
            SendMessageW(window.window,WM_KEYDOWN,'C',0x002E0001);
            SendMessageW(window.window,WM_KEYUP,'C',0xC02E0001);
            Check(receivedKeyUps['C']==upsC+1,"open menu keeps a key pressed and released inside it");
            SendMessageW(window.window,WM_RBUTTONUP,0,MAKELPARAM(20,20));
            Check(receivedRightUps==rightUps+1,"open menu passes the release of a mouse button held as it opened");
            SendMessageW(window.window,WM_KEYDOWN,key,0x000E0001);
            SendMessageW(window.window,WM_KEYUP,key,0xC00E0001);
            Check(Status(module).overlayVisible==0,"hotkey closed the menu after the held-input check");
            for (int i = 0; i < 4; ++i)
            {
                RenderAndPresent(fixture, chain, base);
                window.Pump(3);
            }
        }

        // Resize while the renderer is initialized, then keep presenting.
        Check(chain.Resize(fixture, 400, 300), "resize after menu use");
        for (int i = 0; i < 4; ++i) RenderAndPresent(fixture, chain, base);
        Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after menu");
    }
    else
    {
        // Menu-enabled candidate with the menu never opened: the renderer
        // initializes on the tracked principal chain and stays healthy. UI
        // submissions may still occur (the software cursor is part of the
        // shipped renderer), so only visibility and failures are asserted.
        float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
        for (int i = 0; i < 8; ++i) RenderAndPresent(fixture, chain, base);
        MfgSingleModuleStatus status = Status(module);
        Check(status.overlayVisible == 0, "overlay stays invisible without the hotkey");
        Check(status.renderFailures == 0, "no UI render failures with the menu closed");
        std::printf("NOTE menu-enabled candidate submitted %llu UI frames with the menu closed (software cursor)\n",
            static_cast<unsigned long long>(status.renderedFrames));
        std::fflush(stdout);
    }

    // -------------------------------------------------------------- teardown
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    fixture.commands.Reset();
    fixture.allocator.Reset();
    fixture.queue.Reset();
    window.Pump(4);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy at teardown");
    if (fixture.debugLayer)
    {
        ComPtr<struct ID3D12InfoQueue> info;
        if (SUCCEEDED(fixture.device->QueryInterface(IID_PPV_ARGS(&info))) && info)
        {
            const UINT64 errors = info->GetNumStoredMessages();
            for (UINT64 index = 0; index < errors; ++index) {
                SIZE_T length = 0;
                info->GetMessage(index, nullptr, &length);
                std::vector<unsigned char> storage(length);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(index, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::printf("DEBUG_ERROR %s\n", message->pDescription);
                    Check(false, "D3D12 debug error/corruption");
                }
            }
            std::printf("NOTE debug layer stored messages: %llu\n",
                static_cast<unsigned long long>(errors));
        }
        else
            std::printf("NOTE info queue unavailable; error reporting reduced\n");
    }
    if (menuMode)
    {
        // Destroy the renderer-owned resources with the module still loaded.
        MfgSingleModuleStatus status = Status(module);
        Check(status.overlayInstallState == kStateActive, "coordinator still active at teardown");
    }
    std::printf("COMPLETE mode=%ls\n", menuMode ? L"menu" : L"present");
    if (coexist) {
        Check(overlayIntact(), unalignedGraphics?"existing overlay method hooks unchanged after menu, resize and teardown":"existing overlay code unchanged after menu, resize and teardown");
        Check(existing.restored(),"all native tables and protections unchanged");
        Check(existing.count(13)==0&&existing.count(14)==0&&existing.count(15)>0,"stateful overlay initializes before nested Release without a missing binding");
        Check(existing.count(4) > 0 && existing.count(7) > 0 && existing.count(8) > 0
            && existing.count(9) > 0 && existing.count(12) > 0,
            "existing overlay received real create, Present, Present1, resize and release calls");
    }
    Check(Status(module).liveSwapchainWrappers==0,"all swapchain proxies retired after teardown");
    if(layered){
        auto count=reinterpret_cast<uint64_t(WINAPI*)(unsigned)>(GetProcAddress(existing.module,"OverlayFixtureLayeredCount"));
        Check(count&&count(0)>=3,"layered fixture used separate application and presentation queues on the same device");
        Check(count&&count(1)>0&&count(4)>0,"application and native buffer counts and frame indices diverged");
        Check(count&&count(2)>20&&count(3)>0,"off-screen application buffers crossed a synchronized queue boundary before native presentation");
        auto live=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(existing.module,"OverlayFixtureLiveOuter"));
        Check(live&&live()==0,"layered swapchain generations released all fixture ownership");
    }
    return gFailures == 0 ? 0 : 10;
}

// A genuinely legacy factory may reject IDXGIFactory1. Verify that newer
// vtable calls return safely through the exact DLL when that query fails.
int RunLegacyFactoryUnsupported(const wchar_t* dll)
{
    HMODULE module=LoadCandidate(dll);
    if (!Check(module!=nullptr,"candidate loads for unsupported factory revision")) return 1;
    const auto before=Status(module).liveFactoryWrappers;
    ComPtr<IDXGIFactory> factory;
    if (!Check(SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))&&factory
        &&Status(module).liveFactoryWrappers==before+1,"legacy DXGI factory returned through owned proxy")) return 1;
    auto* newer=reinterpret_cast<IDXGIFactory1*>(factory.Get());
    ComPtr<IDXGIAdapter1> adapter;
    Check(newer->EnumAdapters1(0,&adapter)==E_NOINTERFACE&&!adapter,
        "unavailable EnumAdapters1 returns E_NOINTERFACE without a null dereference");
    Check(newer->IsCurrent()==FALSE,"unavailable IsCurrent returns FALSE");
    factory.Reset();
    Check(Status(module).liveFactoryWrappers==before,"unsupported factory proxy retires cleanly");
    return gFailures==0?0:10;
}

// V-Sync Off override. The pre-existing overlay fixture hooks the native
// Present methods, so it records the interval that actually reaches DXGI below
// the menu's swapchain proxy. The saved mode is switched live through the
// config file, as the menu does: Game, then Off, then Game again.
int RunVsyncOverride(const wchar_t* dll)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto directory = std::filesystem::path(exe).parent_path();
    const auto writeConfig = [&](unsigned vsyncMode) {
        std::ofstream config(directory / L"RTXMFG-Universal.json", std::ios::binary | std::ios::trunc);
        config << "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,"
                  "\"dynamicTargetFrameRate\":0,\"dlssgPreset\":0,\"vsyncMode\":" << vsyncMode
               << ",\"reflexFrameLimitFps\":0,\"generatedOnlyDebug\":false,"
                  "\"intervalLogging\":false,\"version\":13}\n";
        return static_cast<bool>(config);
    };
    const auto status = directory / L"RTXMFG-Universal.status.json";
    const auto waitForStatus = [&](const char* expected) {
        for (unsigned attempt = 0; attempt < 320; ++attempt)
        {
            if (ReadStatusText(status).find(expected) != std::string::npos) return true;
            Sleep(25);
        }
        return false;
    };
    if (!Check(writeConfig(0), "saved V-Sync mode Game written before startup")) return 1;
    ExistingOverlay existing;
    if (!Check(existing.Load(), "pre-existing overlay records native Present arguments")) return 1;
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"VsyncOverride", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;

    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    const auto presentAndRead = [&](UINT flags, bool present1) {
        FixturePresent seen{};
        if (present1)
        {
            DXGI_PRESENT_PARAMETERS parameters{};
            chain.swapchain->Present1(1, flags, &parameters);
        }
        else if (flags)
            chain.swapchain->Present(1, flags);
        else
            RenderAndPresent(fixture, chain, teal, 1, 0);
        existing.readPresent(&seen);
        return seen;
    };
    // The worker applies a saved change within a few 100 ms polls. Present
    // until the new mode reaches DXGI; nothing else signals the switch.
    const auto converge = [&](UINT expected) {
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            if (presentAndRead(0, false).sync == expected) return true;
            Sleep(50);
        }
        return false;
    };
    const auto phase = [&](bool off) {
        const UINT expected = off ? 0u : 1u;
        FixturePresent seen = presentAndRead(0, false);
        Check(seen.kind == 7 && seen.sync == expected && seen.flags == 0,
            off ? "Off: Present(1) reaches DXGI with interval 0 and unchanged flags"
                : "Game: Present(1) reaches DXGI unchanged");
        seen = presentAndRead(0, true);
        Check(seen.kind == 8 && seen.sync == expected && seen.flags == 0,
            off ? "Off: Present1(1) reaches DXGI with interval 0"
                : "Game: Present1(1) reaches DXGI unchanged");
        seen = presentAndRead(DXGI_PRESENT_TEST, false);
        Check(seen.kind == 7 && seen.sync == 1 && seen.flags == DXGI_PRESENT_TEST,
            "TEST presents keep the application's interval");
        for (int frame = 0; frame < 20; ++frame)
        {
            seen = presentAndRead(0, false);
            if (seen.sync != expected) break;
        }
        Check(seen.sync == expected, off ? "Off holds across 20 presents" : "Game holds across 20 presents");
    };
    Check(waitForStatus("\"vsyncMode\":0,"), "saved Game mode loaded");
    phase(false);
    Check(writeConfig(1) && converge(0), "Off selected live reaches DXGI");
    phase(true);
    Check(writeConfig(0) && converge(1), "Game selected live again reaches DXGI");
    phase(false);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after presents");

    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    std::printf("COMPLETE mode=vsync-override\n");
    return gFailures == 0 ? 0 : 10;
}

bool DirectInputWindowPresent()
{
    const DWORD self = GetCurrentProcessId();
    for (HWND candidate = FindWindowExW(nullptr, nullptr, L"DIEmWin", nullptr); candidate;
         candidate = FindWindowExW(nullptr, candidate, L"DIEmWin", nullptr))
    {
        DWORD process = 0;
        if (GetWindowThreadProcessId(candidate, &process) && process == self) return true;
    }
    return false;
}

// Drains the device's buffered data and counts button transitions.
unsigned DirectInputButtons(IDirectInputDevice8W* mouse, bool& readable)
{
    unsigned buttons = 0;
    readable = true;
    for (;;)
    {
        DIDEVICEOBJECTDATA data[64]{};
        DWORD count = static_cast<DWORD>(std::size(data));
        if (FAILED(mouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0)))
        {
            readable = false;
            return buttons;
        }
        for (DWORD index = 0; index < count; ++index)
            if (data[index].dwOfs >= DIMOFS_BUTTON0 && data[index].dwOfs <= DIMOFS_BUTTON7) ++buttons;
        if (count < std::size(data)) return buttons;
    }
}

// Waits for buffered DirectInput button transitions to reach a count.
unsigned DirectInputButtonsWithin(IDirectInputDevice8W* mouse, unsigned expected, bool& readable)
{
    unsigned buttons = 0;
    for (const ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 600;)
    {
        buttons += DirectInputButtons(mouse, readable);
        if (!readable || buttons >= expected) break;
        Sleep(20);
    }
    return buttons;
}

// Absolute injected mouse event at a physical screen point. The process is
// DPI unaware, so the desktop bounds come from the monitors as this per-monitor
// aware thread sees them (GetSystemMetrics would report scaled sizes).
bool InjectMouse(POINT screen, DWORD flags)
{
    RECT desktop{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{sizeof(info)};
        auto& bounds = *reinterpret_cast<RECT*>(data);
        if (GetMonitorInfoW(monitor, &info))
        {
            bounds.left = std::min(bounds.left, info.rcMonitor.left);
            bounds.top = std::min(bounds.top, info.rcMonitor.top);
            bounds.right = std::max(bounds.right, info.rcMonitor.right);
            bounds.bottom = std::max(bounds.bottom, info.rcMonitor.bottom);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&desktop));
    const LONG width = desktop.right - desktop.left, height = desktop.bottom - desktop.top;
    if (width < 2 || height < 2) return false;
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dx = static_cast<LONG>(((static_cast<LONGLONG>(screen.x) - desktop.left) * 65536 + width / 2) / width);
    event.mi.dy = static_cast<LONG>(((static_cast<LONGLONG>(screen.y) - desktop.top) * 65536 + height / 2) / height);
    event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
    return SendInput(1, &event, sizeof(event)) == 1;
}

struct PanelLayout { int x = 0, y = 0, width = 0, height = 0; };

// The menu panel as Dear ImGui saved it to RTX40MFG-UI-layout.ini.
// The panel is "RTXMFG <version>###RTXMFG"; ImGui saves it under its ### ID.
std::optional<PanelLayout> ReadPanel(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::string line;
    bool inPanel = false;
    PanelLayout panel{};
    bool position = false, size = false;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '[') { inPanel = line == "[Window][###RTXMFG]"; continue; }
        if (!inPanel) continue;
        if (sscanf_s(line.c_str(), "Pos=%d,%d", &panel.x, &panel.y) == 2) position = true;
        if (sscanf_s(line.c_str(), "Size=%d,%d", &panel.width, &panel.height) == 2) size = true;
    }
    if (!position || !size || panel.width <= 0) return std::nullopt;
    return panel;
}

// Dying Light: The Beast reads the mouse through DirectInput. dinput8 runs an
// exclusive mouse through a low-level hook that consumes every event, so the
// game window receives no button messages and the menu could not be clicked.
// Pinned FPS readout: with [Overlay] PinnedFps=1 saved, the closed menu keeps
// a readout in the top-left corner composited after the 7-second
// startup hint is gone. The hint sits below the readout, so a point inside it
// is the control: drawn first, the plain clear colour after the hint expires.
int RunPinnedFps(const wchar_t* dll)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto ini = std::filesystem::path(exe).parent_path() / L"RTX40MFG-UI.ini";
    if (!Check(GetPrivateProfileIntW(L"Overlay", L"PinnedFps", 0, ini.c_str()) == 1,
            "pinned FPS saved before startup")) return 1;
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"PinnedFps", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;

    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    const ULONGLONG start = GetTickCount64();
    for (int frame = 0; frame < 6; ++frame) { RenderAndPresent(fixture, chain, teal); window.Pump(1); }
    float readout[4]{}, hint[4]{};
    Check(ReadCenter(fixture, chain, readout, 20, 18) && !PixelMatches(readout, teal),
        "pinned FPS readout composited in the top-left corner");
    Check(ReadCenter(fixture, chain, hint, 200, 50) && !PixelMatches(hint, teal),
        "startup hint composited below the readout");
    while (GetTickCount64() - start < 8500)
    {
        RenderAndPresent(fixture, chain, teal);
        window.Pump(1);
        Sleep(50);
    }
    for (int frame = 0; frame < 3; ++frame) RenderAndPresent(fixture, chain, teal);
    Check(Status(module).overlayVisible == 0, "menu stays closed");
    Check(ReadCenter(fixture, chain, hint, 200, 50) && PixelMatches(hint, teal),
        "startup hint gone after 7 seconds");
    Check(ReadCenter(fixture, chain, readout, 20, 18) && !PixelMatches(readout, teal),
        "pinned FPS readout still composited with the menu closed");
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after presents");
    return 0;
}

// keybinds.1: injected keyboard input, so the overlay's poll (below other mods'
// hooks) sees the key down. Page Up and the arrow block are extended keys.
bool InjectKey(WORD key, bool down)
{
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = key;
    event.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    event.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP)
        | (key == VK_PRIOR || key == VK_NEXT || key == VK_INSERT || key == VK_DELETE ? KEYEVENTF_EXTENDEDKEY : 0);
    return SendInput(1, &event, sizeof(event)) == 1;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// keybinds.1: the Keybinds tab's keys with the menu closed. [Keybinds] binds Ctrl+Page Up to Raise multiplier, F9 to
// FPS readout and Backspace (the menu key) to 6x. The control file switches XeMFG standalone on with 2x, which applies
// only after a restart, so the status echoes saved values. Ctrl+Page Up raises the saved frames to 3x and draws its note
// at the top centre, F9 pins the FPS readout, and Backspace opens the menu instead of selecting 6x.
int RunKeybindsMenu(const wchar_t* dll)
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto folder = std::filesystem::path(exe).parent_path();
    const auto ini = folder / L"RTX40MFG-UI.ini";
    const auto control = folder / L"RTXMFG-Universal.json";
    const auto status = folder / L"RTXMFG-Universal.status.json";
    wchar_t saved[64]{};
    GetPrivateProfileStringW(L"Keybinds", L"RaiseMultiplier", L"", saved, 64, ini.c_str());
    if (!Check(std::wstring(saved) == L"Ctrl+0x21", "keybinds saved before startup")) return 1;
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"Keybinds", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;

    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame)
        {
            RenderAndPresent(fixture, chain, teal);
            window.Pump(1);
            Sleep(15);
        }
    };
    // Presents until done() or the time runs out.
    const auto until = [&](ULONGLONG milliseconds, auto&& done) {
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < milliseconds)
        {
            if (done()) return true;
            frames(2);
        }
        return done();
    };
    const auto topCentre = [&](bool clear) {
        float pixel[4]{};
        frames(2);
        return ReadCenter(fixture, chain, pixel, 320, 16) && PixelMatches(pixel, teal) == clear;
    };
    const auto foreground = [&] {
        const DWORD ourThread = GetCurrentThreadId();
        const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
        BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
        if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
        window.Pump(2);
        return GetForegroundWindow() == window.window;
    };
    const auto press = [&](std::initializer_list<WORD> keys) {
        bool sent = true;
        for (const WORD key : keys) sent &= InjectKey(key, true);
        frames(8);
        for (auto key = std::rbegin(keys); key != std::rend(keys); ++key) sent &= InjectKey(*key, false);
        frames(4);
        return sent;
    };

    // The startup hint is gone after 7 seconds; then the top centre is the clear colour.
    until(8500, [] { return false; });
    Check(topCentre(true), "top centre shows the clear colour before any key");
    Check(Status(module).overlayVisible == 0, "menu closed before the keys");

    std::ofstream(control, std::ios::binary | std::ios::trunc)
        << "{\"followGame\":true,\"mode\":\"follow\",\"multiplier\":2,\"dynamicTargetFrameRate\":0,\"dlssgPreset\":2,"
           "\"vsyncMode\":0,\"reflexFrameLimitFps\":0,\"generatedOnlyDebug\":false,\"intervalLogging\":true,"
           "\"version\":13,\"xemfgStandalone\":1,\"xemfgFrames\":2}\n";
    Check(until(6000, [&] {
            const auto text = ReadText(status);
            return text.find("\"xemfgStandalone\":true") != std::string::npos
                && text.find("\"xemfgSavedFrames\":2") != std::string::npos;
        }), "status shows XeMFG standalone switched on with 2x saved");

    if (!Check(foreground(), "keybinds test owns the foreground window")) return 2;
    Check(press({VK_CONTROL, VK_PRIOR}), "Ctrl+Page Up injected");
    Check(until(3000, [&] { return ReadText(control).find("\"xemfgFrames\":3") != std::string::npos; }),
        "Ctrl+Page Up raised XeMFG's saved frames from 2x to 3x with the menu closed");
    Check(topCentre(false), "the key's note composited at the top centre");
    Check(Status(module).overlayVisible == 0, "the key left the menu closed");
    until(2000, [] { return false; });
    Check(topCentre(true), "the note is gone after 1.5 seconds");

    if (!Check(foreground(), "keybinds test owns the foreground window for F9")) return 2;
    Check(press({VK_F9}), "F9 injected");
    Check(until(2000, [&] { return GetPrivateProfileIntW(L"Overlay", L"PinnedFps", 0, ini.c_str()) == 1; }),
        "F9 turned the FPS readout on and saved it");
    float readout[4]{};
    frames(3);
    Check(ReadCenter(fixture, chain, readout, 20, 18) && !PixelMatches(readout, teal),
        "FPS readout composited in the top-left corner with the menu closed");

    // keybinds.3: the next DLSS preset key fails closed while the game runs no DLSS (XeMFG's panel here, which shows
    // the SR/RR preset row only while DLSS upscaling runs): its note shows and nothing is written.
    until(2000, [] { return false; });
    Check(topCentre(true), "the FPS readout key's note is gone");
    const std::string beforePreset = ReadText(control);
    if (!Check(foreground(), "keybinds test owns the foreground window for F7")) return 2;
    Check(press({VK_F7}), "F7 injected");
    Check(topCentre(false), "the DLSS preset key's note composited at the top centre");
    Check(ReadText(control) == beforePreset, "the DLSS preset key wrote nothing while the game runs no DLSS");

    if (!Check(foreground(), "keybinds test owns the foreground window for Backspace")) return 2;
    Check(press({VK_BACK}), "Backspace injected");
    Check(until(2000, [&] { return Status(module).overlayVisible == 1; }), "Backspace opened the menu");
    Check(ReadText(control).find("\"xemfgFrames\":3") != std::string::npos,
        "Backspace stayed the menu key: the 6x binding on it did not run");
    if (foreground()) press({VK_BACK});
    Check(until(2000, [&] { return Status(module).overlayVisible == 0; }), "Backspace closed the menu");
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after presents");

    // As vsync-override: drain the queue before the swapchain proxy's release (the debug layer fails fast on a
    // release with frames in flight), then again after it.
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    std::printf("COMPLETE mode=keybinds-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// This case acquires a real exclusive DirectInput mouse, then drags the menu
// panel by its title bar with injected input: the cursor and the panel must
// follow, and DirectInput must still read the mouse once the menu closes. The
// cursor is parked inside the test window first, so an injected click that
// escapes the menu lands there.
int RunDirectInputMenu(const wchar_t* dll)
{
    // Physical pixels throughout: window, menu layout, monitors and injected
    // input. Monitor and system metrics follow the process awareness.
    if (!Check(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "process is per-monitor DPI aware")) return 1;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto layout = std::filesystem::path(exe).parent_path() / L"RTX40MFG-UI-layout.ini";
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"DirectInputMenu", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;
    float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(2);
        }
    };
    frames(8);

    const DWORD ourThread = GetCurrentThreadId();
    harness::focus::PrepareClaim();
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
    BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
    if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
    window.Pump(2);
    if (!Check(GetForegroundWindow() == window.window, "DirectInput test owns the foreground window")) return 2;
    RECT client{};
    GetClientRect(window.window, &client);
    POINT center{client.right / 2, client.bottom / 2};
    ClientToScreen(window.window, &center);
    SetCursorPos(center.x, center.y);

    ComPtr<IDirectInput8W> input;
    ComPtr<IDirectInputDevice8W> mouse;
    DIPROPDWORD buffer{};
    buffer.diph.dwSize = sizeof(buffer);
    buffer.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    buffer.diph.dwHow = DIPH_DEVICE;
    buffer.dwData = 256;
    const bool acquired = SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
            IID_IDirectInput8W, reinterpret_cast<void**>(input.GetAddressOf()), nullptr))
        && SUCCEEDED(input->CreateDevice(GUID_SysMouse, mouse.GetAddressOf(), nullptr))
        && SUCCEEDED(mouse->SetDataFormat(&c_dfDIMouse2))
        && SUCCEEDED(mouse->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph))
        && SUCCEEDED(mouse->SetCooperativeLevel(window.window, DISCL_EXCLUSIVE | DISCL_FOREGROUND))
        && SUCCEEDED(mouse->Acquire());
    if (!Check(acquired, "exclusive foreground DirectInput mouse acquired")) return 2;
    window.Pump(4);
    if (!Check(DirectInputWindowPresent(), "DirectInput hook window (DIEmWin) present in the process")) return 2;
    {
        RAWINPUTDEVICE devices[16]{};
        UINT capacity = static_cast<UINT>(std::size(devices));
        const UINT count = GetRegisteredRawInputDevices(devices, &capacity, sizeof(RAWINPUTDEVICE));
        std::printf("NOTE raw input registrations=%d\n", static_cast<int>(count));
        for (UINT index = 0; count != UINT(-1) && index < count; ++index)
        {
            wchar_t name[64]{};
            if (devices[index].hwndTarget) GetClassNameW(devices[index].hwndTarget, name, 64);
            std::printf("NOTE raw input page=0x%X usage=0x%X flags=0x%lX target=%ls\n", devices[index].usUsagePage,
                devices[index].usUsage, devices[index].dwFlags, name);
        }
    }

    // Menu closed: the DLTB condition. The window never sees the click.
    bool readable = false;
    DirectInputButtons(mouse.Get(), readable);
    unsigned windowButtons = receivedMouseButtons;
    Check(InjectMouse(center, MOUSEEVENTF_LEFTDOWN) && InjectMouse(center, MOUSEEVENTF_LEFTUP),
        "closed menu: click injected");
    Sleep(100);
    window.Pump(4);
    unsigned directInputButtons = DirectInputButtonsWithin(mouse.Get(), 2, readable);
    std::printf("NOTE closed menu: DirectInput buttons=%u readable=%d\n", directInputButtons, readable ? 1 : 0);
    Check(receivedMouseButtons == windowButtons,
        "closed menu: exclusive DirectInput withholds the click from the window (DLTB condition)");

    WPARAM key = 0;
    {
        const MfgSingleModuleStatus current = Status(module);
        key = current.shortcutBindingActive ? current.shortcutVirtualKey : VK_BACK;
    }
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    frames(12);
    if (!Check(Status(module).overlayVisible == 1, "menu hotkey made the overlay visible")) return 2;
    std::optional<PanelLayout> before;
    for (int attempt = 0; attempt < 40 && !(before = ReadPanel(layout)); ++attempt) frames(1);
    if (!Check(before.has_value(), "menu layout saved with the panel position")) return 2;

    // Title-bar centre, clear of the collapse and close buttons.
    POINT grab{before->x + before->width / 2, before->y + 6};
    ClientToScreen(window.window, &grab);
    const POINT shift{60, 40};
    const POINT drop{grab.x + shift.x, grab.y + shift.y};
    DirectInputButtons(mouse.Get(), readable);
    windowButtons = receivedMouseButtons;
    const auto cursorNote = [](const char* label) {
        POINT cursor{};
        GetCursorPos(&cursor);
        std::printf("NOTE %s cursor=%ld,%ld\n", label, cursor.x, cursor.y);
        return cursor;
    };
    cursorNote("before grab");
    bool injected = InjectMouse(grab, 0);
    Sleep(30); frames(3);
    const POINT grabbed = cursorNote("after move to grab");
    Check(std::abs(grabbed.x - grab.x) <= 2 && std::abs(grabbed.y - grab.y) <= 2,
        "open menu: the cursor follows the injected mouse");
    injected &= InjectMouse(grab, MOUSEEVENTF_LEFTDOWN);
    Sleep(30); frames(3);
    for (LONG step = 1; step <= 6; ++step)
    {
        injected &= InjectMouse(POINT{grab.x + shift.x * step / 6, grab.y + shift.y * step / 6}, 0);
        Sleep(20); frames(2);
    }
    cursorNote("after drag");
    injected &= InjectMouse(drop, MOUSEEVENTF_LEFTUP);
    Sleep(30); frames(3);
    Check(injected, "open menu: title-bar drag injected");
    // DirectInput reads raw input before the low-level hook's decision applies,
    // so the game still sees the menu's clicks (known limitation, recorded).
    directInputButtons = DirectInputButtonsWithin(mouse.Get(), 2, readable);
    std::printf("NOTE open menu: DirectInput buttons=%u readable=%d\n", directInputButtons, readable ? 1 : 0);
    Check(receivedMouseButtons == windowButtons, "open menu: the game window receives no button messages");
    // Dear ImGui saves a moved window after io.IniSavingRate (5 s).
    std::optional<PanelLayout> after;
    for (const ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 9000;)
    {
        frames(2);
        Sleep(40);
        after = ReadPanel(layout);
        if (after && (after->x != before->x || after->y != before->y)) break;
    }
    std::printf("NOTE panel before=%d,%d %dx%d after=%d,%d grab=%ld,%ld\n", before->x, before->y,
        before->width, before->height, after ? after->x : -1, after ? after->y : -1, grab.x, grab.y);
    Check(after && std::abs(after->x - (before->x + shift.x)) <= 3 && std::abs(after->y - (before->y + shift.y)) <= 3,
        "open menu: the panel follows a title-bar drag made with the DirectInput mouse");

    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    frames(8);
    Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
    Sleep(100);
    window.Pump(4);
    DirectInputButtons(mouse.Get(), readable);
    windowButtons = receivedMouseButtons;
    Check(InjectMouse(drop, MOUSEEVENTF_LEFTDOWN) && InjectMouse(drop, MOUSEEVENTF_LEFTUP),
        "closed menu: second click injected");
    Sleep(100);
    window.Pump(4);
    directInputButtons = DirectInputButtonsWithin(mouse.Get(), 2, readable);
    Check(readable && directInputButtons == 2, "closed menu: the mouse returns to DirectInput");
    Check(receivedMouseButtons == windowButtons, "closed menu: the window again sees no DirectInput click");

    mouse->Unacquire();
    mouse.Reset();
    input.Reset();
    frames(4);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after DirectInput menu use");
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    std::printf("COMPLETE mode=directinput-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// Pixels of Dear ImGui's arrow cursor (border, fill or shadow) whose tip is at
// a client point, against the clear colour: all of them when the menu draws
// its cursor there, none when it does not.
unsigned CursorPixels(WarpFixture& fixture, Chain& chain, POINT tip, const float base[4])
{
    constexpr POINT offsets[]{{1, 3}, {1, 6}, {2, 5}, {2, 8}, {3, 7}, {3, 10}, {4, 9}, {5, 10}};
    unsigned drawn = 0;
    for (const auto& offset : offsets)
    {
        float pixel[4]{};
        if (ReadCenter(fixture, chain, pixel, UINT(tip.x + offset.x), UINT(tip.y + offset.y))
            && !PixelMatches(pixel, base))
            ++drawn;
    }
    return drawn;
}

// Under frame generation the menu draws on every output frame, while the game
// pumps its window messages once per game frame (Subnautica 2, XeMFG 6x: ~116
// menu frames and ~20 pumps a second), so the mouse messages wait for the
// next pump. On frames drawn between two pumps the open menu must put its
// cursor where the mouse is. The menu draws its own cursor, so the frame shows
// where the menu thinks the mouse is. A title-bar drag with a pump on every
// other frame must still move the panel by exactly the drag.
int RunSlowPumpMenu(const wchar_t* dll)
{
    // Physical pixels throughout: window, menu layout, monitors and injected input.
    if (!Check(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "process is per-monitor DPI aware")) return 1;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto layout = std::filesystem::path(exe).parent_path() / L"RTX40MFG-UI-layout.ini";
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    // Wide enough for clear background beside the default panel (570x610 at 24,24).
    TestWindow window(L"SlowPumpMenu", 1280, 900, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    // Backbuffer = client area: frame pixels and client coordinates match.
    RECT client{};
    if (!Check(GetClientRect(window.window, &client) && client.right > 200 && client.bottom > 200,
            "test window client area")) return 1;
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, UINT(client.right), UINT(client.bottom), 2,
                DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;
    float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
    // Frames without a pump are the output frames between two game frames.
    const auto frames = [&](int count, bool pump) {
        for (int frame = 0; frame < count; ++frame)
        {
            RenderAndPresent(fixture, chain, base);
            if (pump) window.Pump(2);
        }
    };
    frames(8, true);

    const DWORD ourThread = GetCurrentThreadId();
    harness::focus::PrepareClaim();
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
    BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
    if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
    window.Pump(2);
    if (!Check(GetForegroundWindow() == window.window, "slow-pump menu test owns the foreground window")) return 2;

    WPARAM key = 0;
    {
        const MfgSingleModuleStatus current = Status(module);
        key = current.shortcutBindingActive ? current.shortcutVirtualKey : VK_BACK;
    }
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    frames(12, true);
    if (!Check(Status(module).overlayVisible == 1, "menu hotkey made the overlay visible")) return 2;
    std::optional<PanelLayout> before;
    for (int attempt = 0; attempt < 40 && !(before = ReadPanel(layout)); ++attempt) frames(1, true);
    if (!Check(before.has_value(), "menu layout saved with the panel position")) return 2;

    // The cursor starts over the panel; the target is clear background beside
    // or below it, with room for the arrow.
    const POINT park{before->x + before->width / 2, before->y + before->height / 2};
    POINT target{before->x + before->width + 20, before->y + 40};
    if (target.x > client.right - 24) target = POINT{before->x + 40, before->y + before->height + 12};
    if (!Check(target.x + 24 < client.right && target.y + 24 < client.bottom,
            "clear background beside or below the panel")) return 2;
    const auto screen = [&](POINT point) { ClientToScreen(window.window, &point); return point; };
    Check(InjectMouse(screen(park), 0), "cursor parked over the panel");
    Sleep(30); frames(4, true);
    const unsigned parked = CursorPixels(fixture, chain, target, base);
    std::printf("NOTE panel=%d,%d %dx%d park=%ld,%ld target=%ld,%ld cursor pixels at target before the move=%u\n",
        before->x, before->y, before->width, before->height, park.x, park.y, target.x, target.y, parked);
    Check(parked == 0, "no cursor at the target while the mouse is over the panel");

    // The mouse moves between two pumps: its messages wait in the queue.
    Check(InjectMouse(screen(target), 0), "cursor moved to the target");
    Sleep(30);
    frames(3, false);
    const unsigned unpumped = CursorPixels(fixture, chain, target, base);
    std::printf("NOTE cursor pixels at target before the pump=%u\n", unpumped);
    Check(unpumped >= 6, "open menu: the cursor follows the mouse on frames drawn between two pumps");
    frames(4, true);
    const unsigned pumped = CursorPixels(fixture, chain, target, base);
    std::printf("NOTE cursor pixels at target after the pump=%u\n", pumped);
    Check(pumped >= 6, "open menu: the pumped messages leave the cursor at the mouse");

    // Title-bar drag at a pump every other frame.
    POINT grab{before->x + before->width / 2, before->y + 6};
    const POINT shift{60, 40};
    bool injected = InjectMouse(screen(grab), 0);
    Sleep(30); frames(3, true);
    injected &= InjectMouse(screen(grab), MOUSEEVENTF_LEFTDOWN);
    Sleep(30); frames(3, true);
    for (LONG step = 1; step <= 6; ++step)
    {
        injected &= InjectMouse(screen(POINT{grab.x + shift.x * step / 6, grab.y + shift.y * step / 6}), 0);
        Sleep(20); frames(2, step % 2 == 0);
    }
    frames(2, true);
    injected &= InjectMouse(screen(POINT{grab.x + shift.x, grab.y + shift.y}), MOUSEEVENTF_LEFTUP);
    Sleep(30); frames(3, true);
    Check(injected, "open menu: title-bar drag injected");
    // Dear ImGui saves a moved window after io.IniSavingRate (5 s).
    std::optional<PanelLayout> after;
    for (const ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 9000;)
    {
        frames(2, true);
        Sleep(40);
        after = ReadPanel(layout);
        if (after && (after->x != before->x || after->y != before->y)) break;
    }
    std::printf("NOTE panel before=%d,%d after=%d,%d\n", before->x, before->y, after ? after->x : -1, after ? after->y : -1);
    Check(after && std::abs(after->x - (before->x + shift.x)) <= 3 && std::abs(after->y - (before->y + shift.y)) <= 3,
        "open menu: the panel follows a title-bar drag pumped every other frame");

    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    frames(8, true);
    Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after slow-pump menu use");
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    std::printf("COMPLETE mode=slowpump-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// A game's own raw mouse thread: a hidden window on its own thread takes the mouse as raw input with RIDEV_NOLEGACY |
// RIDEV_CAPTUREMOUSE, as dinput8 does, so the game window gets no mouse messages. It counts the button transitions.
struct RawMouseThread
{
    std::atomic<unsigned> buttons{0};
    std::atomic<bool> registered{false};
    std::atomic<DWORD> id{0};
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_INPUT)
        {
            RAWINPUT input{};
            UINT bytes = sizeof(input);
            const UINT copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                &input, &bytes, sizeof(RAWINPUTHEADER));
            auto* self = reinterpret_cast<RawMouseThread*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (self && copied != UINT(-1) && copied >= sizeof(RAWINPUTHEADER) && input.header.dwType == RIM_TYPEMOUSE)
                for (unsigned bit = 0; bit < 10; ++bit)
                    if (input.data.mouse.usButtonFlags & (1u << bit)) self->buttons.fetch_add(1);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    bool Start()
    {
        thread = std::thread([this] {
            id.store(GetCurrentThreadId());
            WNDCLASSW description{};
            description.lpfnWndProc = &Proc;
            description.hInstance = GetModuleHandleW(nullptr);
            description.lpszClassName = L"GameRawMouse";
            const ATOM atom = RegisterClassW(&description);
            const HWND sink = atom ? CreateWindowExW(0, L"GameRawMouse", L"", WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, description.hInstance, nullptr) : nullptr;
            if (sink)
            {
                SetWindowLongPtrW(sink, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                const RAWINPUTDEVICE device{0x01, 0x02, RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE, sink};
                registered.store(RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE);
            }
            SetEvent(ready);
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
            if (registered.load())
            {
                const RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
                RegisterRawInputDevices(&device, 1, sizeof(device));
            }
            if (sink) DestroyWindow(sink);
            if (atom) UnregisterClassW(L"GameRawMouse", description.hInstance);
        });
        return WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && registered.load();
    }
    ~RawMouseThread()
    {
        if (thread.joinable())
        {
            PostThreadMessageW(id.load(), WM_QUIT, 0, 0);
            thread.join();
        }
        CloseHandle(ready);
    }
};

// AION 2 (issues 78 and 121): the menu opens on its key, but the game takes every click. The menu window's procedure
// gets no mouse messages: with rawSink the game reads the mouse on a raw input thread of its own, otherwise its pump
// handles every mouse message itself. After the mouse moves to the panel the menu must take it through its low-level
// hook: a title-bar drag moves the panel and no click reaches the game while the menu is open. Closed, the game gets
// the clicks again; reopened, the menu takes the mouse at once.
int RunUnroutedMenu(const wchar_t* dll, bool rawSink)
{
    // Physical pixels throughout: window, menu layout, monitors and injected input.
    if (!Check(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
            "process is per-monitor DPI aware")) return 1;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto layout = std::filesystem::path(exe).parent_path() / L"RTX40MFG-UI-layout.ini";
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(rawSink ? L"RawSinkMenu" : L"OwnPumpMenu", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory)))
            && SUCCEEDED(applicationFactory.As(&fixture.factory)),
            "application factory created through the menu's gateway"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == 1,
            "application swapchain wrapped by the menu"))
        return 1;
    // The game's own pump: the window's mouse messages are handled here and never dispatched to a window procedure.
    unsigned pumpButtons = 0;
    const auto pump = [&](unsigned iterations) {
        if (rawSink) { window.Pump(iterations); return; }
        MSG message;
        for (unsigned i = 0; i < iterations; ++i)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.hwnd == window.window
                    && message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST)
                {
                    if (message.message >= WM_LBUTTONDOWN && message.message <= WM_XBUTTONDBLCLK
                        && message.message != WM_MOUSEWHEEL) ++pumpButtons;
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
    };
    float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame)
        {
            RenderAndPresent(fixture, chain, base);
            pump(2);
        }
    };
    frames(8);

    const DWORD ourThread = GetCurrentThreadId();
    harness::focus::PrepareClaim();
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
    BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
    if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
    pump(2);
    if (!Check(GetForegroundWindow() == window.window, "unrouted menu test owns the foreground window")) return 2;
    RECT client{};
    GetClientRect(window.window, &client);
    POINT center{client.right / 2, client.bottom / 2};
    ClientToScreen(window.window, &center);
    SetCursorPos(center.x, center.y);
    RawMouseThread sink;
    if (rawSink)
    {
        if (!Check(sink.Start(), "game raw mouse thread registered (NOLEGACY | CAPTUREMOUSE, a class of its own)")) return 2;
        Check(!DirectInputWindowPresent(), "no DirectInput window in the process");
    }
    pump(4);
    // The game's view of the clicks: its pump, or its raw mouse thread.
    const auto gameButtons = [&]() -> unsigned { return rawSink ? sink.buttons.load() : pumpButtons; };
    const auto gameButtonsSince = [&](unsigned from) {
        for (const ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 600 && gameButtons() - from < 2;)
            pump(2);
        return gameButtons() - from;
    };

    // Menu closed: the AION 2 condition.
    unsigned game = gameButtons();
    unsigned procedureButtons = receivedMouseButtons;
    Check(InjectMouse(center, MOUSEEVENTF_LEFTDOWN) && InjectMouse(center, MOUSEEVENTF_LEFTUP),
        "closed menu: click injected");
    Sleep(100);
    unsigned seen = gameButtonsSince(game);
    std::printf("NOTE closed menu: game buttons=%u\n", seen);
    Check(seen == 2 && receivedMouseButtons == procedureButtons,
        "closed menu: the game reads the click and the window procedure sees none (AION 2 condition)");

    WPARAM key = 0;
    {
        const MfgSingleModuleStatus current = Status(module);
        key = current.shortcutBindingActive ? current.shortcutVirtualKey : VK_BACK;
    }
    const auto toggle = [&] {
        PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
        PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    };
    toggle();
    frames(12);
    if (!Check(Status(module).overlayVisible == 1, "menu hotkey made the overlay visible")) return 2;
    std::optional<PanelLayout> before;
    for (int attempt = 0; attempt < 40 && !(before = ReadPanel(layout)); ++attempt) frames(1);
    if (!Check(before.has_value(), "menu layout saved with the panel position")) return 2;

    // A title-bar drag by shift: true when injected and the cursor reached the grab.
    const auto drag = [&](POINT grab, POINT shift) {
        bool injected = InjectMouse(grab, 0);
        Sleep(30); frames(3);
        POINT cursor{};
        GetCursorPos(&cursor);
        std::printf("NOTE grab=%ld,%ld cursor=%ld,%ld\n", grab.x, grab.y, cursor.x, cursor.y);
        const bool reached = std::abs(cursor.x - grab.x) <= 2 && std::abs(cursor.y - grab.y) <= 2;
        injected &= InjectMouse(grab, MOUSEEVENTF_LEFTDOWN);
        Sleep(30); frames(3);
        for (LONG step = 1; step <= 6; ++step)
        {
            injected &= InjectMouse(POINT{grab.x + shift.x * step / 6, grab.y + shift.y * step / 6}, 0);
            Sleep(20); frames(2);
        }
        injected &= InjectMouse(POINT{grab.x + shift.x, grab.y + shift.y}, MOUSEEVENTF_LEFTUP);
        Sleep(30); frames(3);
        return injected && reached;
    };
    // The panel as saved once it moved by shift (Dear ImGui saves after io.IniSavingRate, 5 s).
    const auto movedBy = [&](const PanelLayout& from, POINT shift) -> std::optional<PanelLayout> {
        std::optional<PanelLayout> after;
        for (const ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 9000;)
        {
            frames(2);
            Sleep(40);
            after = ReadPanel(layout);
            if (after && (after->x != from.x || after->y != from.y)) break;
        }
        std::printf("NOTE panel before=%d,%d after=%d,%d\n", from.x, from.y, after ? after->x : -1, after ? after->y : -1);
        if (after && std::abs(after->x - (from.x + shift.x)) <= 3 && std::abs(after->y - (from.y + shift.y)) <= 3)
            return after;
        return std::nullopt;
    };

    // The player moves the mouse to the panel; no mouse message reaches the menu window, so the menu uses its hook.
    POINT grab{before->x + before->width / 2, before->y + 6};
    ClientToScreen(window.window, &grab);
    // 16 steps of at least 60 ms: the menu decides after 500 ms.
    bool approached = true;
    for (LONG step = 1; step <= 16; ++step)
    {
        approached &= InjectMouse(POINT{center.x + (grab.x - center.x) * step / 16,
            center.y + (grab.y - center.y) * step / 16}, 0);
        Sleep(40); frames(1);
    }
    Check(approached, "open menu: mouse moved to the panel");
    const POINT shift{60, 40};
    game = gameButtons();
    procedureButtons = receivedMouseButtons;
    Check(drag(grab, shift), "open menu: title-bar drag injected, the cursor follows the mouse");
    if (rawSink)
        std::printf("NOTE open menu: raw mouse thread buttons=%u (raw input still reaches the game, as with DirectInput)\n",
            gameButtons() - game);
    else
        Check(gameButtons() == game, "open menu: the game's pump gets no click");
    Check(receivedMouseButtons == procedureButtons, "open menu: the window procedure gets no click");
    const auto dragged = movedBy(*before, shift);
    Check(dragged.has_value(), "open menu: the panel follows a title-bar drag on a window that gets no mouse messages");

    toggle();
    frames(8);
    Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
    Sleep(100);
    pump(4);
    const POINT drop{grab.x + shift.x, grab.y + shift.y};
    game = gameButtons();
    procedureButtons = receivedMouseButtons;
    Check(InjectMouse(drop, MOUSEEVENTF_LEFTDOWN) && InjectMouse(drop, MOUSEEVENTF_LEFTUP),
        "closed menu: second click injected");
    Sleep(100);
    seen = gameButtonsSince(game);
    Check(seen == 2 && receivedMouseButtons == procedureButtons, "closed menu: the mouse returns to the game");

    // Reopened: this window keeps the hook, so a drag made at once still moves the panel.
    toggle();
    frames(12);
    if (!Check(Status(module).overlayVisible == 1, "third hotkey press reopened the menu")) return 2;
    if (dragged)
    {
        POINT regrab{dragged->x + dragged->width / 2, dragged->y + 6};
        ClientToScreen(window.window, &regrab);
        const POINT back{-shift.x, -shift.y};
        Check(drag(regrab, back), "reopened menu: title-bar drag injected at once");
        Check(movedBy(*dragged, back).has_value(), "reopened menu: the menu takes the mouse at once");
    }
    toggle();
    frames(8);
    Check(Status(module).overlayVisible == 0, "fourth hotkey press closed the menu");

    frames(4);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after unrouted menu use");
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    std::printf("COMPLETE mode=%s\n", rawSink ? "rawsink-menu" : "ownpump-menu");
    return gFailures == 0 ? 0 : 10;
}

// Reads an export from the image's export table, as a protected executable
// that resolves APIs itself does; GetProcAddress hooks never see it.
FARPROC ExportAddress(HMODULE module, const char* name)
{
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) return nullptr;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index)
        if (!strcmp(reinterpret_cast<const char*>(base + names[index]), name))
            return reinterpret_cast<FARPROC>(const_cast<unsigned char*>(base + functions[ordinals[index]]));
    return nullptr;
}

bool ExerciseMenu(WarpFixture& fixture, Chain& chain, TestWindow& window, HMODULE module);

// Streamline manual hooking (Wuthering Waves): the factory, device and queue
// exist before RTXMFG loads, the interposer's exports are read from its export
// table, and the application upgrades its own factory through
// slUpgradeInterface. The menu must attach to swapchains the upgraded factory
// creates; an upgraded swapchain is left untouched.
// immediate: like Wuthering Waves, slInit, slSetD3DDevice and the upgrade run
// right after LoadLibrary returns, before module discovery can see the
// interposer. That fixture has the real interposer's hotpatchable slInit.
int RunManualUpgrade(const wchar_t* dll, bool upgradeSwapchain, bool immediate = false)
{
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and native factory created before RTXMFG loads"))
        return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"ManualUpgrade", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto path = std::filesystem::path(exe).parent_path()
        / (immediate ? L"hotpatch\\sl.interposer.dll" : L"sl.interposer.dll");
    HMODULE interposer = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!Check(interposer != nullptr, "local Streamline ABI fixture loaded")) return 1;
    using Init = sl::Result(*)(const sl::Preferences&, uint64_t);
    using Upgrade = sl::Result(*)(void**);
    using SetDevice = sl::Result(*)(void*);
    const auto init = reinterpret_cast<Init>(ExportAddress(interposer, "slInit"));
    const auto upgrade = reinterpret_cast<Upgrade>(ExportAddress(interposer, "slUpgradeInterface"));
    const auto setDevice = reinterpret_cast<SetDevice>(ExportAddress(interposer, "slSetD3DDevice"));
    const auto upgrades = reinterpret_cast<unsigned(WINAPI*)()>(ExportAddress(interposer, "FixtureUpgradeCalls"));
    const auto deviceSets = reinterpret_cast<unsigned(WINAPI*)()>(ExportAddress(interposer, "FixtureDeviceSets"));
    if (!Check(init && upgrade && upgrades && setDevice && deviceSets,
            "interposer exports read from its export table"))
        return 1;

    const auto status = std::filesystem::current_path() / L"RTXMFG-Universal.status.json";
    const auto waitForStatus = [&](const char* expected) {
        for (unsigned attempt = 0; attempt < 320; ++attempt)
        {
            if (ReadStatusText(status).find(expected) != std::string::npos) return true;
            Sleep(25);
        }
        return false;
    };
    // Otherwise the background module scan discovers the interposer first.
    if (!immediate)
        Check(waitForStatus("\"slUpgradeInterfaceEntryDetourCurrent\":true"),
            "slUpgradeInterface entry detour installed after the interposer loads");
    sl::Preferences prefs{};
    prefs.flags |= sl::PreferenceFlags::eUseManualHooking;
    Check(init(prefs, sl::kSDKVersion) == sl::Result::eOk, "manual-hooking slInit succeeds");
    Check(setDevice(fixture.device.Get()) == sl::Result::eOk && deviceSets() == 1,
        "slSetD3DDevice forwarded exactly once");
    const MfgSingleModuleStatus start = Status(module);
    Check(start.factoryWrappersCreated == 0 && start.swapchainWrappersCreated == 0,
        "no factory or swapchain creation reached RTXMFG");

    // Non-DXGI objects and invalid arguments pass through untouched.
    {
        void* object = fixture.device.Get();
        const sl::Result result = upgrade(&object);
        const MfgSingleModuleStatus after = Status(module);
        Check(result == sl::Result::eOk && object == fixture.device.Get()
            && after.factoryWrappersCreated == 0 && after.swapchainWrappersCreated == 0,
            "upgrading a D3D12 device leaves it untouched");
        Check(upgrade(nullptr) == sl::Result::eErrorInvalidParameter,
            "invalid upgrade forwards the interposer's result");
    }

    Chain chain;
    if (!upgradeSwapchain)
    {
        IDXGIFactory2* factory = fixture.factory.Detach();
        IDXGIFactory2* native = factory;
        const unsigned before = upgrades();
        const sl::Result result = upgrade(reinterpret_cast<void**>(&factory));
        fixture.factory.Attach(factory);
        Check(result == sl::Result::eOk && upgrades() == before + 1, "factory upgrade forwarded once with its result");
        if (!Check(factory != native && Status(module).factoryWrappersCreated == 1,
                "upgraded factory replaced by the menu's factory proxy"))
            return 1;
        factory = fixture.factory.Detach();
        IDXGIFactory2* proxy = factory;
        upgrade(reinterpret_cast<void**>(&factory));
        fixture.factory.Attach(factory);
        Check(factory == proxy && Status(module).factoryWrappersCreated == 1,
            "upgrading the menu's proxy again does not wrap it twice");
    }
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL),
            "flip-sequential swapchain created"))
        return 1;
    if (upgradeSwapchain)
    {
        // A D3D12 swapchain never reports its presentation queue, which the
        // renderer needs: an upgraded swapchain must be left untouched.
        Check(Status(module).swapchainWrappersCreated == 0, "native swapchain creation is not seen by RTXMFG");
        IDXGISwapChain3* swapchain = chain.swapchain.Detach();
        IDXGISwapChain3* native = swapchain;
        const sl::Result result = upgrade(reinterpret_cast<void**>(&swapchain));
        chain.swapchain.Attach(swapchain);
        Check(result == sl::Result::eOk && swapchain == native
            && Status(module).swapchainWrappersCreated == 0 && Status(module).liveSwapchainWrappers == 0,
            "upgraded swapchain without a known queue is left untouched");
        float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
        float observed[4]{};
        for (int frame = 0; frame < 3; ++frame) RenderAndPresent(fixture, chain, teal);
        Check(RenderReadPresent(fixture, chain, teal, observed) && PixelMatches(observed, teal)
            && SUCCEEDED(fixture.device->GetDeviceRemovedReason()),
            "untouched swapchain keeps presenting the application's content");
        Chain::Drain(fixture, chain.swapchain.Get());
        chain.buffers.clear();
        chain.rtvs.Reset();
        chain.swapchain.Reset();
        Chain::Drain(fixture);
        std::printf("COMPLETE mode=manual-upgrade-swapchain\n");
        return gFailures == 0 ? 0 : 10;
    }
    if (!Check(Status(module).swapchainWrappersCreated == 1,
            "swapchain created from the upgraded factory is wrapped"))
        return 1;
    if (immediate)
    {
        Check(waitForStatus("\"slInitEntryInstalledAtLoad\":true"),
            "slInit entry hotpatched while the interposer was loading");
        Check(waitForStatus("\"slInitCalls\":1,"), "slInit hook ran for the call right after loading");
        Check(waitForStatus("\"slSetD3DDeviceCalls\":1,"),
            "slSetD3DDevice hook ran for the call right after slInit");
    }
    if (!ExerciseMenu(fixture, chain, window, module)) return 1;
    std::printf("COMPLETE mode=manual-upgrade-%ls\n", upgradeSwapchain ? L"swapchain" : L"factory");
    return gFailures == 0 ? 0 : 10;
}

// Where Winds Meet: a packed executable reaches the interposer's factory
// exports without any import slot or GetProcAddress call RTXMFG observes,
// after a non-manual slInit. Executable calls must return exactly one menu
// factory proxy; a foreign module calling the same entry stays native.
int RunInterposerEntry(const wchar_t* dll)
{
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device and queue created before RTXMFG loads"))
        return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"InterposerEntry", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    HMODULE foreign = LoadLibraryW(L"ScopedCallerFixture.dll");
    using ForeignEntry = HRESULT(WINAPI*)(void*, unsigned, UINT, REFIID, void**);
    const auto foreignEntry = foreign
        ? reinterpret_cast<ForeignEntry>(GetProcAddress(foreign, "ForeignFactoryEntry")) : nullptr;
    if (!Check(foreignEntry != nullptr, "foreign-caller fixture loaded")) return 1;
    // The game's slInit entry is hotpatched while the interposer loads, as
    // with this fixture's copy of the real interposer's hotpatchable slInit.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto path = std::filesystem::path(exe).parent_path() / L"hotpatch\\sl.interposer.dll";
    HMODULE interposer = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!Check(interposer != nullptr, "local Streamline ABI fixture loaded")) return 1;
    using Init = sl::Result(*)(const sl::Preferences&, uint64_t);
    using Factory = HRESULT(WINAPI*)(REFIID, void**);
    using Factory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto init = reinterpret_cast<Init>(ExportAddress(interposer, "slInit"));
    const auto initCalls = reinterpret_cast<unsigned(WINAPI*)()>(ExportAddress(interposer, "FixtureInitCalls"));
    void* entries[3] = {reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory")),
        reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory1")),
        reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory2"))};
    if (!Check(init && initCalls && entries[0] && entries[1] && entries[2],
            "interposer exports read from its export table"))
        return 1;
    const auto create = [&](unsigned api, REFIID iid, void** output) {
        return api == 0 ? reinterpret_cast<Factory>(entries[0])(iid, output)
            : api == 1 ? reinterpret_cast<Factory>(entries[1])(iid, output)
            : reinterpret_cast<Factory2>(entries[2])(0, iid, output);
    };

    const auto status = std::filesystem::current_path() / L"RTXMFG-Universal.status.json";
    const auto waitForStatus = [&](const char* expected) {
        for (unsigned attempt = 0; attempt < 320; ++attempt)
        {
            if (ReadStatusText(status).find(expected) != std::string::npos) return true;
            Sleep(25);
        }
        return false;
    };
    Check(waitForStatus("\"slInitEntryDetourCurrent\":true"),
        "slInit entry detour current after the interposer loads");
    sl::Preferences prefs{};
    Check(init(prefs, sl::kSDKVersion) == sl::Result::eOk && initCalls() == 1,
        "non-manual slInit forwarded exactly once");
    {
        const MfgSingleModuleStatus after = Status(module);
        if (!Check(after.factoryWrappersCreated == 0 && after.swapchainWrappersCreated == 0,
                "slInit creates no graphics objects"))
            return 1;
    }
    for (unsigned api = 0; api < 3; ++api)
    {
        const MfgSingleModuleStatus prior = Status(module);
        ComPtr<IDXGIFactory> object;
        const bool created = SUCCEEDED(foreignEntry(entries[api], api, 0, IID_PPV_ARGS(&object))) && object;
        const MfgSingleModuleStatus after = Status(module);
        Check(created && after.factoryGatewayCalls == prior.factoryGatewayCalls
            && after.factoryWrappersCreated == prior.factoryWrappersCreated,
            "foreign caller of the exported interposer entry stays native");
    }
    for (unsigned api = 0; api < 3; ++api)
    {
        const uint64_t prior = Status(module).factoryWrappersCreated;
        ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(create(api, IID_PPV_ARGS(&object))) && object
            && Status(module).factoryWrappersCreated == prior + 1,
            "executable call through the export table returns exactly one menu proxy");
        object.Reset();
        void* native = nullptr;
        void* routed = nullptr;
        const HRESULT expected = foreignEntry(entries[api], api, 0, IID_NULL, &native);
        Check(create(api, IID_NULL, &routed) == expected && !routed && !native
            && Status(module).factoryWrappersCreated == prior + 1,
            "failed IID HRESULT and output preserved");
    }
    // Engines call slInit again after device loss; the relay is reused.
    Check(init(prefs, sl::kSDKVersion) == sl::Result::eOk && initCalls() == 2, "second slInit forwarded");
    {
        const uint64_t prior = Status(module).factoryWrappersCreated;
        ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(create(2, IID_PPV_ARGS(&object))) && object
            && Status(module).factoryWrappersCreated == prior + 1,
            "re-armed entry still returns exactly one menu proxy");
    }
    fixture.factory.Reset();
    if (!Check(SUCCEEDED(create(2, IID_PPV_ARGS(&fixture.factory))) && fixture.factory,
            "application factory created through the interposer export"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL),
            "flip-sequential swapchain created"))
        return 1;
    if (!Check(Status(module).swapchainWrappersCreated == 1,
            "swapchain created from the interposer factory is wrapped"))
        return 1;
    if (!ExerciseMenu(fixture, chain, window, module)) return 1;
    std::printf("COMPLETE mode=interposer-entry-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// Dying Light 2 / The Beast: rd3d12_x64_rwdi.dll has no graphics imports. It
// resolves the interposer's factory exports with GetProcAddress before its
// statically imported slInit, then calls them from its own image. That slInit
// host joins the executable's caller scope; a DLL that resolves slInit itself
// is not a host, and a foreign module calling the same entry stays native.
int RunInterposerHost(const wchar_t* dll, bool staticHost)
{
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device and queue created before RTXMFG loads"))
        return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"InterposerHost", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    HMODULE foreign = LoadLibraryW(L"ScopedCallerFixture.dll");
    using ForeignEntry = HRESULT(WINAPI*)(void*, unsigned, UINT, REFIID, void**);
    const auto foreignEntry = foreign
        ? reinterpret_cast<ForeignEntry>(GetProcAddress(foreign, "ForeignFactoryEntry")) : nullptr;
    if (!Check(foreignEntry != nullptr, "foreign-caller fixture loaded")) return 1;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto path = std::filesystem::path(exe).parent_path() / L"hotpatch\\sl.interposer.dll";
    HMODULE interposer = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!Check(interposer != nullptr, "local Streamline ABI fixture loaded")) return 1;
    const auto status = std::filesystem::current_path() / L"RTXMFG-Universal.status.json";
    bool detourCurrent = false;
    for (unsigned attempt = 0; attempt < 320 && !detourCurrent; ++attempt)
    {
        detourCurrent = ReadStatusText(status).find("\"slInitEntryDetourCurrent\":true") != std::string::npos;
        if (!detourCurrent) Sleep(25);
    }
    Check(detourCurrent, "slInit entry detour current after the interposer loads");
    // Loaded after the interposer: its static slInit import binds to that image.
    HMODULE renderer = LoadLibraryW(staticHost ? L"RendererHostFixture.dll" : L"RendererDynamicFixture.dll");
    const auto resolve = renderer ? reinterpret_cast<BOOL(WINAPI*)()>(GetProcAddress(renderer, "HostResolveFactories")) : nullptr;
    const auto hostEntry = renderer ? reinterpret_cast<void*(WINAPI*)(unsigned)>(GetProcAddress(renderer, "HostFactoryEntry")) : nullptr;
    const auto hostInit = renderer ? reinterpret_cast<sl::Result(WINAPI*)()>(GetProcAddress(renderer, "HostInit")) : nullptr;
    const auto hostCreate = renderer
        ? reinterpret_cast<HRESULT(WINAPI*)(unsigned, UINT, REFIID, void**)>(GetProcAddress(renderer, "HostCreateFactory")) : nullptr;
    if (!Check(resolve && hostEntry && hostInit && hostCreate, "renderer fixture loaded from the application directory"))
        return 1;
    using Factory = HRESULT(WINAPI*)(REFIID, void**);
    using Factory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto initCalls = reinterpret_cast<unsigned(WINAPI*)()>(ExportAddress(interposer, "FixtureInitCalls"));
    void* entries[3] = {reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory")),
        reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory1")),
        reinterpret_cast<void*>(ExportAddress(interposer, "CreateDXGIFactory2"))};
    if (!Check(initCalls && entries[0] && entries[1] && entries[2], "interposer exports read from its export table"))
        return 1;
    const auto create = [&](unsigned api, REFIID iid, void** output) {
        return api == 0 ? reinterpret_cast<Factory>(entries[0])(iid, output)
            : api == 1 ? reinterpret_cast<Factory>(entries[1])(iid, output)
            : reinterpret_cast<Factory2>(entries[2])(0, iid, output);
    };
    Check(resolve() && hostEntry(0) == entries[0] && hostEntry(1) == entries[1] && hostEntry(2) == entries[2],
        "renderer resolved the exact interposer factory entries before slInit");
    Check(hostInit() == sl::Result::eOk && initCalls() == 1, "renderer slInit forwarded exactly once");
    {
        const MfgSingleModuleStatus after = Status(module);
        if (!Check(after.factoryWrappersCreated == 0 && after.swapchainWrappersCreated == 0,
                "slInit creates no graphics objects"))
            return 1;
    }
    for (unsigned api = 0; api < 3; ++api)
    {
        const MfgSingleModuleStatus prior = Status(module);
        ComPtr<IDXGIFactory> object;
        const bool created = SUCCEEDED(foreignEntry(entries[api], api, 0, IID_PPV_ARGS(&object))) && object;
        const MfgSingleModuleStatus after = Status(module);
        Check(created && after.factoryGatewayCalls == prior.factoryGatewayCalls
            && after.factoryWrappersCreated == prior.factoryWrappersCreated,
            "foreign caller of the exported interposer entry stays native");
    }
    for (unsigned api = 0; api < 3; ++api)
    {
        const MfgSingleModuleStatus prior = Status(module);
        ComPtr<IDXGIFactory> object;
        const bool created = SUCCEEDED(hostCreate(api, 0, IID_PPV_ARGS(&object))) && object;
        const MfgSingleModuleStatus after = Status(module);
        if (staticHost)
        {
            Check(created && after.factoryWrappersCreated == prior.factoryWrappersCreated + 1,
                "renderer host call through its resolved entry returns exactly one menu proxy");
            object.Reset();
            void* native = nullptr;
            void* routed = nullptr;
            const HRESULT expected = foreignEntry(entries[api], api, 0, IID_NULL, &native);
            Check(hostCreate(api, 0, IID_NULL, &routed) == expected && !routed && !native
                && Status(module).factoryWrappersCreated == prior.factoryWrappersCreated + 1,
                "failed IID HRESULT and output preserved");
        }
        else
        {
            Check(created && after.factoryGatewayCalls == prior.factoryGatewayCalls
                && after.factoryWrappersCreated == prior.factoryWrappersCreated,
                "renderer that resolves slInit itself is not a host and stays native");
        }
    }
    {
        const uint64_t prior = Status(module).factoryWrappersCreated;
        ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(create(2, IID_PPV_ARGS(&object))) && object
            && Status(module).factoryWrappersCreated == prior + 1,
            "executable call through the export table keeps its menu proxy");
    }
    if (!staticHost)
    {
        std::printf("COMPLETE mode=interposer-host-dynamic\n");
        return gFailures == 0 ? 0 : 10;
    }
    // Engines call slInit again after device loss; the host scope is retained.
    Check(hostInit() == sl::Result::eOk && initCalls() == 2, "second renderer slInit forwarded");
    {
        const uint64_t prior = Status(module).factoryWrappersCreated;
        ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(hostCreate(2, 0, IID_PPV_ARGS(&object))) && object
            && Status(module).factoryWrappersCreated == prior + 1,
            "host scope retained after a repeated slInit");
    }
    fixture.factory.Reset();
    if (!Check(SUCCEEDED(hostCreate(2, 0, IID_PPV_ARGS(&fixture.factory))) && fixture.factory,
            "application factory created by the renderer host"))
        return 1;
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL),
            "flip-sequential swapchain created"))
        return 1;
    if (!Check(Status(module).swapchainWrappersCreated == 1,
            "swapchain created from the renderer host's factory is wrapped"))
        return 1;
    if (!ExerciseMenu(fixture, chain, window, module)) return 1;
    std::printf("COMPLETE mode=interposer-host-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// Present, open and close the menu through the window's keyboard path, resize,
// then retire every proxy and report D3D12 debug errors.
bool ExerciseMenu(WarpFixture& fixture, Chain& chain, TestWindow& window, HMODULE module)
{
    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    for (int frame = 0; frame < 5; ++frame)
        if (!Check(RenderAndPresent(fixture, chain, teal), "frame rendered and presented"))
            return false;
    float hint[4]{};
    Check(ReadCenter(fixture, chain, hint, 24, 24) && !PixelMatches(hint, teal),
        "startup hint visibly composited into retained backbuffer");

    // Menu hotkey through the window's keyboard path, as in the menu suite.
    float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
    for (int i = 0; i < 8; ++i)
    {
        RenderAndPresent(fixture, chain, base);
        window.Pump(2);
    }
    const uint64_t renderedBefore = Status(module).renderedFrames;
    WPARAM key = 0;
    {
        const MfgSingleModuleStatus current = Status(module);
        key = current.shortcutBindingActive ? current.shortcutVirtualKey : VK_BACK;
    }
    const DWORD ourThread = GetCurrentThreadId();
    window.Pump(4);
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
    BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
    if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
    window.Pump(2);
    Check(GetForegroundWindow() == window.window, "menu input test owns foreground window");
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    for (int i = 0; i < 12; ++i)
    {
        RenderAndPresent(fixture, chain, base);
        window.Pump(4);
    }
    const MfgSingleModuleStatus menu = Status(module);
    Check(menu.overlayVisible == 1, "menu hotkey made the overlay visible");
    Check(menu.renderedFrames > renderedBefore, "menu rendering executed actual UI draw submissions");
    bool differs = false;
    for (int attempt = 0; attempt < 6 && !differs; ++attempt)
    {
        RenderAndPresent(fixture, chain, base);
        window.Pump(3);
        Sleep(120);
        float pixel[4]{};
        if (ReadCenter(fixture, chain, pixel)) differs = !PixelMatches(pixel, base);
    }
    Check(differs, "menu visibly composited into the presented backbuffer");
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    for (int i = 0; i < 6; ++i)
    {
        RenderAndPresent(fixture, chain, base);
        window.Pump(3);
    }
    Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
    Check(chain.Resize(fixture, 400, 300), "resize through the upgraded chain");
    for (int i = 0; i < 4; ++i) RenderAndPresent(fixture, chain, base);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after menu");

    // -------------------------------------------------------------- teardown
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    fixture.factory.Reset();
    Check(Status(module).liveFactoryWrappers == 0, "factory proxy retired with the application's reference");
    fixture.commands.Reset();
    fixture.allocator.Reset();
    fixture.queue.Reset();
    window.Pump(4);
    if (fixture.debugLayer)
    {
        ComPtr<struct ID3D12InfoQueue> info;
        if (SUCCEEDED(fixture.device->QueryInterface(IID_PPV_ARGS(&info))) && info)
        {
            const UINT64 messages = info->GetNumStoredMessages();
            for (UINT64 index = 0; index < messages; ++index)
            {
                SIZE_T length = 0;
                info->GetMessage(index, nullptr, &length);
                std::vector<unsigned char> storage(length);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(index, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                {
                    std::printf("DEBUG_ERROR %s\n", message->pDescription);
                    Check(false, "D3D12 debug error/corruption");
                }
            }
        }
    }
    return true;
}

// dx11menu.1: d3d12-with-d3d11-menu. A D3D12 game (this executable links D3D12CreateDevice and no D3D11 creation)
// that also creates a D3D11 device and a D3D11 window, as a launcher, video or middleware would. Its D3D12 menu is
// unchanged; the D3D11 side gets no DirectX 11 menu (no session, no Presents counted) and arms no adapter-parent
// route: the menu's D3D11 gateways are never bound in a D3D12-linked executable.
int RunD3D12WithD3D11(const wchar_t* dll)
{
    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true) && Status(module).factoryWrappersCreated >= 1,
            "WARP D3D12 device, queue and the application's factory proxy created")) return 1;

    // The D3D11 side: a device through d3d11.dll's entry, its adapter's factory, and a swapchain on a second window
    // from the application's factory.
    const auto create11 = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        GetProcAddress(SystemModule(L"d3d11.dll"), "D3D11CreateDevice"));
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    if (!Check(create11 && SUCCEEDED(create11(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
            &device11, nullptr, &context11)), "D3D11 WARP device created beside the D3D12 game")) return 1;
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> parent;
        const auto factories = Status(module).factoryWrappersCreated;
        Check(SUCCEEDED(device11.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))
            && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&parent))) && Status(module).factoryWrappersCreated == factories,
            "the D3D11 device's adapter gives the native factory: no adapter-parent arming from it");
    }
    TestWindow side(L"BoundedWarp11Side", 320, 240);
    if (!Check(side.window != nullptr, "D3D11 side window created")) return 1;
    side.Show();
    DXGI_SWAP_CHAIN_DESC1 desc11{};
    desc11.Width = 320;
    desc11.Height = 240;
    desc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc11.SampleDesc.Count = 1;
    desc11.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc11.BufferCount = 2;
    desc11.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    const auto wrappers = Status(module).swapchainWrappersCreated;
    ComPtr<IDXGISwapChain1> chain11;
    if (!Check(SUCCEEDED(fixture.factory->CreateSwapChainForHwnd(device11.Get(), side.window, &desc11, nullptr, nullptr,
            &chain11)) && Status(module).swapchainWrappersCreated == wrappers,
            "the D3D11 swapchain of a D3D12 game gets no DirectX 11 menu")) return 1;
    {
        ComPtr<ID3D11Texture2D> buffer;
        ComPtr<ID3D11RenderTargetView> target;
        if (!Check(SUCCEEDED(chain11->GetBuffer(0, IID_PPV_ARGS(&buffer)))
                && SUCCEEDED(device11->CreateRenderTargetView(buffer.Get(), nullptr, &target)), "D3D11 back buffer view"))
            return 1;
        const float red[4]{0.75f, 0.15f, 0.15f, 1.0f};
        const auto frames = Status(module).dxgiFrames;
        const auto rendered = Status(module).renderedFrames;
        for (int frame = 0; frame < 5; ++frame)
        {
            context11->ClearRenderTargetView(target.Get(), red);
            chain11->Present(0, 0);
            side.Pump(1);
        }
        Check(Status(module).dxgiFrames == frames && Status(module).renderedFrames == rendered,
            "the D3D11 chain's Presents are neither counted nor drawn on");
        context11->ClearState();
        context11->Flush();
    }
    chain11.Reset();
    side.Pump(2);

    // The D3D12 game's own chain and menu, exactly as in the menu suite.
    Chain chain;
    if (!Check(chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            && Status(module).swapchainWrappersCreated == wrappers + 1, "D3D12 swapchain wrapped by the menu")) return 1;
    if (!ExerciseMenu(fixture, chain, window, module)) return 1;
    std::printf("COMPLETE mode=d3d12-with-d3d11-menu\n");
    return gFailures == 0 ? 0 : 10;
}

// GitHub issues 150/157 (Modern Warfare 4): the game's import directory could not be read once it ran, so input
// preparation failed and the menu kept the original swapchain. Here the harness drops the terminator from its own import
// directory after the candidate loaded. The swapchain must still be wrapped, with no input slot published, and the menu
// must open, draw and close through window messages.
int RunUnreadableImportsMenu(const wchar_t* dll)
{
    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true) && Status(module).factoryWrappersCreated >= 1,
            "WARP D3D12 device, queue and the application's factory proxy created")) return 1;

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    DWORD real = 0;
    while (descriptors[real].Name) ++real;
    const DWORD originalSize = directory.Size;
    DWORD protection = 0;
    if (!Check(directory.VirtualAddress && real && VirtualProtect(&directory, sizeof(directory), PAGE_READWRITE, &protection),
            "harness import directory made writable")) return 1;
    directory.Size = real * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    DWORD ignored = 0;
    VirtualProtect(&directory, sizeof(directory), protection, &ignored);

    Chain chain;
    const auto wrappers = Status(module).swapchainWrappersCreated;
    const bool created = chain.Create(fixture, window.window, 640, 480, 2, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    Check(created && Status(module).swapchainWrappersCreated == wrappers + 1,
        "swapchain wrapped by the menu although the import directory cannot be read");
    Check(Status(module).overlayActivatedTargets == 0, "no input slot published from an unreadable import directory");
    VirtualProtect(&directory, sizeof(directory), PAGE_READWRITE, &protection);
    directory.Size = originalSize;
    VirtualProtect(&directory, sizeof(directory), protection, &ignored);
    if (!created || gFailures) return 1;
    if (!ExerciseMenu(fixture, chain, window, module)) return 1;
    std::printf("COMPLETE mode=unreadable-imports-menu\n");
    return gFailures == 0 ? 0 : 10;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    harness::focus::Install();
    if (argc != 3) return 2;
    const std::wstring mode = argv[1];
    const wchar_t* dll = argv[2];
    std::printf("mode=%ls dll=%ls\n", argv[1], dll);
    if (mode == L"factory-revision-menu") return RunPresentation(dll,true,18,false,false,false,false,false,false,false,false,false,false,0,false,false,false,true);
    if (mode == L"factory-revision-present") return RunPresentation(dll,false,18,false,false,false,false,false,false,false,false,false,false,0,false,false,false,true);
    if (mode == L"factory-revision-unsupported") return RunLegacyFactoryUnsupported(dll);
    if (mode == L"adapter-parent-menu") return RunPresentation(dll,true,18,true,false,false,false,false,false,false,false,false,false,0,false,false,true);
    if (mode == L"adapter-parent-coexist-menu") return RunPresentation(dll,true,18,true,true,false,false,false,false,false,false,false,false,0,false,false,true);
    if (mode == L"adapter-parent-unaligned-menu") return RunPresentation(dll,true,18,true,false,false,true,false,false,false,false,true,false,0,false,true,true);
    if (mode == L"unaligned-graphics-menu") return RunPresentation(dll,true,18,true,false,false,true,false,false,false,false,true,false,0,false,true);
    if (mode == L"rwx-unaligned-graphics-menu") return RunPresentation(dll,true,18,true,false,false,true,false,false,false,false,true,true,0,false,true);
    if (mode == L"unaligned-graphics-coexist-menu") return RunPresentation(dll,true,18,true,true,false,true,false,false,false,false,true,false,0,false,true);
    if (mode == L"unaligned-menu") return RunPresentation(dll,true,18,true,false,true,false,false,false,false,false,true);
    if (mode == L"rwx-unaligned-menu") return RunPresentation(dll,true,18,true,false,true,false,false,false,false,false,true,true);
    if (mode == L"rwx-aligned-menu") return RunPresentation(dll,true,18,true,false,true,false,false,false,false,false,true,true,0,false,false,false,false,true);
    if (mode == L"hdr-startup-menu") return RunPresentation(dll,true,18,true,false,false,false,false,false,false,false,false,false,1);
    if (mode == L"hdr-unsupported-menu") return RunPresentation(dll,true,18,true,true,false,false,true,false,false,false,false,false,2);
    if (mode == L"startup-menu") return RunPresentation(dll, true, 18, true);
    if (mode == L"engine-menu") return RunPresentation(dll,true,18,true,false,false,false,false,false,false,false,false,false,0,true);
    if (mode == L"first-launch-menu") return RunPresentation(dll,true,18,true,false,false,false,false,false,false,true);
    if (mode == L"layered-slinit-menu") return RunPresentation(dll,true,18,true,true,false,true,true,true);
    if (mode == L"reshade-wrapped-menu") return RunPresentation(dll,true,18,true,true,false,true,true,false,true);
    if (mode == L"manual-upgrade-factory-menu") return RunManualUpgrade(dll,false);
    if (mode == L"manual-upgrade-swapchain-passthrough") return RunManualUpgrade(dll,true);
    if (mode == L"manual-upgrade-immediate-menu") return RunManualUpgrade(dll,false,true);
    if (mode == L"interposer-entry-menu") return RunInterposerEntry(dll);
    if (mode == L"interposer-host-menu") return RunInterposerHost(dll, true);
    if (mode == L"interposer-host-dynamic") return RunInterposerHost(dll, false);
    if (mode == L"vsync-override-menu") return RunVsyncOverride(dll);
    if (mode == L"pinned-fps-menu") return RunPinnedFps(dll);
    if (mode == L"keybinds-menu") return RunKeybindsMenu(dll);
    if (mode == L"directinput-menu") return RunDirectInputMenu(dll);
    if (mode == L"slowpump-menu") return RunSlowPumpMenu(dll);
    if (mode == L"ownpump-menu") return RunUnroutedMenu(dll, false);
    if (mode == L"rawsink-menu") return RunUnroutedMenu(dll, true);
    if (mode == L"d3d12-with-d3d11-menu") return RunD3D12WithD3D11(dll);
    if (mode == L"unreadable-imports-menu") return RunUnreadableImportsMenu(dll);
    std::printf("unknown mode\n");
    return 2;
}
