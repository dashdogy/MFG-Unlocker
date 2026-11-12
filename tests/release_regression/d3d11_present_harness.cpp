// dx11menu.1: real D3D11 WARP presentation for the menu in DirectX 11 games. Its own executable: like a DirectX 11
// game's, it links no D3D12 creation (a D3D12-linked executable is a D3D12 game to the menu). run.py lists the modes.
#include "common.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <thread>

namespace
{
using namespace harness;

constexpr uint32_t kStateActive = 2;
// The menu's private factory proxy IID (overlay_dxgi_proxy.cpp factoryId).
constexpr GUID kFactoryProxy = {0x95f71528, 0xa89b, 0x4d8a, {0xa3, 0x15, 0x60, 0xca, 0x09, 0x12, 0x10, 0x01}};
unsigned receivedKeys = 0;
LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM key, LPARAM data)
{
    if (message == WM_KEYDOWN && key == 'A') ++receivedKeys;
    return DefWindowProcW(window, message, key, data);
}
struct RestoreForeground
{
    HWND window = GetForegroundWindow();
    ~RestoreForeground() { if (IsWindow(window)) SetForegroundWindow(window); }
};

// A DirectX 11 game on WARP: its device, immediate context and the pipeline state it leaves bound when it presents,
// in every stage the menu's draw could disturb.
struct D3D11Game
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Buffer> constants, upperConstants, streamOut, computeBuffer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RenderTargetView> offscreen;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ComputeShader> compute;
    ComPtr<ID3D11UnorderedAccessView> computeOutput;
    ComPtr<ID3D11ShaderResourceView> vertexResource;
    ComPtr<ID3D11Predicate> predicate;
    bool debugLayer = false;

    // d3d11.dll's entries through the executable's own GetProcAddress, as a game resolving them at run time; the
    // menu's gateways answer once the candidate is loaded.
    template<class Fn> static Fn Entry(const char* name)
    {
        return reinterpret_cast<Fn>(GetProcAddress(SystemModule(L"d3d11.dll"), name));
    }

    // The device flags to try in order: the debug layer first when the SDK layers are installed (Graphics Tools),
    // else the plain runtime. MFG_HARNESS_D3D11_DEBUG=0 forces the plain runtime every game runs on: its immediate
    // context's method table is built in the runtime's heap, unlike the debug layer's.
    static std::array<UINT, 2> DeviceFlags()
    {
        wchar_t value[8]{};
        const DWORD n = GetEnvironmentVariableW(L"MFG_HARNESS_D3D11_DEBUG", value, 8);
        const bool plain = n && n < 8 && !wcscmp(value, L"0");
        return plain ? std::array<UINT, 2>{0u, 0u} : std::array<UINT, 2>{UINT(D3D11_CREATE_DEVICE_DEBUG), 0u};
    }

    // D3D11CreateDevice on `adapter`, or on WARP without one.
    bool CreateDevice(IDXGIAdapter* adapter)
    {
        const auto create = Entry<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
        if (!create) return false;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        for (const UINT flags : DeviceFlags())
        {
            if (SUCCEEDED(create(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr, flags,
                    &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context)))
            {
                debugLayer = flags != 0;
                break;
            }
        }
        return device && context && CreateStates();
    }

    // D3D11CreateDeviceAndSwapChain on WARP: a game making its device and swapchain in one call.
    bool CreateWithChain(const DXGI_SWAP_CHAIN_DESC& desc, ComPtr<IDXGISwapChain>& chain)
    {
        const auto create = Entry<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>("D3D11CreateDeviceAndSwapChain");
        if (!create) return false;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        for (const UINT flags : DeviceFlags())
        {
            if (SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &level, 1, D3D11_SDK_VERSION, &desc,
                    &chain, &device, nullptr, &context)))
            {
                debugLayer = flags != 0;
                break;
            }
        }
        return chain && device && context && CreateStates();
    }

    // The device of a swapchain whose creation did not hand it out, as the swapchain reports it.
    bool Adopt(IDXGISwapChain* chain)
    {
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        device->GetImmediateContext(&context);
        ComPtr<ID3D11InfoQueue> info;
        debugLayer = SUCCEEDED(device.As(&info)) && info;
        return context && CreateStates();
    }

    bool CreateStates()
    {
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
        rasterizerDesc.CullMode = D3D11_CULL_FRONT;
        rasterizerDesc.ScissorEnable = TRUE;
        D3D11_BUFFER_DESC constantsDesc{};
        constantsDesc.ByteWidth = 256;
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BUFFER_DESC streamDesc{};
        streamDesc.ByteWidth = 256;
        streamDesc.Usage = D3D11_USAGE_DEFAULT;
        streamDesc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        D3D11_BUFFER_DESC computeDesc{};
        computeDesc.ByteWidth = 64;
        computeDesc.Usage = D3D11_USAGE_DEFAULT;
        computeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 16;
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_TEXTURE2D_DESC colorDesc{};
        colorDesc.Width = colorDesc.Height = 64;
        colorDesc.MipLevels = colorDesc.ArraySize = 1;
        colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorDesc.SampleDesc.Count = 1;
        colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_TEXTURE2D_DESC resourceDesc = colorDesc;
        resourceDesc.Width = resourceDesc.Height = 4;
        resourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> color, depthTexture, resource;
        const D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        if (FAILED(device->CreateBlendState(&blendDesc, &blend))
            || FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizer))
            || FAILED(device->CreateBuffer(&constantsDesc, nullptr, &constants))
            || FAILED(device->CreateBuffer(&constantsDesc, nullptr, &upperConstants))
            || FAILED(device->CreateBuffer(&streamDesc, nullptr, &streamOut))
            || FAILED(device->CreateBuffer(&computeDesc, nullptr, &computeBuffer))
            || FAILED(device->CreateUnorderedAccessView(computeBuffer.Get(), &uavDesc, &computeOutput))
            || FAILED(device->CreateSamplerState(&samplerDesc, &sampler))
            || FAILED(device->CreateTexture2D(&colorDesc, nullptr, &color))
            || FAILED(device->CreateRenderTargetView(color.Get(), nullptr, &offscreen))
            || FAILED(device->CreateTexture2D(&depthDesc, nullptr, &depthTexture))
            || FAILED(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depth))
            || FAILED(device->CreateTexture2D(&resourceDesc, nullptr, &resource))
            || FAILED(device->CreateShaderResourceView(resource.Get(), nullptr, &vertexResource))
            || FAILED(device->CreatePredicate(&predicateDesc, &predicate)))
            return false;
        // An issued, empty occlusion predicate: no samples, so with predicate value TRUE nothing is ever skipped.
        context->Begin(predicate.Get());
        context->End(predicate.Get());
        BOOL passed = TRUE;
        for (int wait = 0; wait < 200 && context->GetData(predicate.Get(), &passed, sizeof passed, 0) == S_FALSE; ++wait)
            Sleep(1);
        // A compute shader the menu's renderer would unbind (it never restores the compute stage).
        using Compile = pD3DCompile;
        const auto compile = reinterpret_cast<Compile>(GetProcAddress(SystemModule(L"d3dcompiler_47.dll"), "D3DCompile"));
        static const char source[] =
            "RWBuffer<uint> output : register(u0);\n"
            "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { output[id.x] = id.x; }\n";
        ComPtr<ID3DBlob> code;
        return compile && SUCCEEDED(compile(source, sizeof source - 1, "d3d11-harness", nullptr, nullptr, "main", "cs_5_0",
                0, 0, &code, nullptr))
            && SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compute));
    }

    // What a game leaves bound when it presents: values the menu's own drawing would replace or clear. The render
    // target is an off-screen one: a flip-model Present unbinds views of the back buffer itself.
    void Bind()
    {
        const D3D11_VIEWPORT viewport{3.0f, 5.0f, 111.0f, 77.0f, 0.25f, 0.75f};
        const D3D11_RECT scissor{1, 2, 30, 40};
        const float factor[4]{0.1f, 0.2f, 0.3f, 0.4f};
        ID3D11Buffer* buffer = constants.Get();
        ID3D11Buffer* upper = upperConstants.Get();
        ID3D11Buffer* stream = streamOut.Get();
        const UINT streamOffset = 0;
        ID3D11SamplerState* state = sampler.Get();
        ID3D11RenderTargetView* target = offscreen.Get();
        ID3D11UnorderedAccessView* output = computeOutput.Get();
        ID3D11ShaderResourceView* resource = vertexResource.Get();
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &scissor);
        context->RSSetState(rasterizer.Get());
        context->OMSetRenderTargets(1, &target, depth.Get());
        context->OMSetBlendState(blend.Get(), factor, 0x0F0F0F0Fu);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        context->VSSetConstantBuffers(0, 1, &buffer);
        context->VSSetShaderResources(0, 1, &resource);
        context->PSSetConstantBuffers(0, 1, &buffer);
        context->PSSetConstantBuffers(2, 1, &upper);
        context->PSSetSamplers(0, 1, &state);
        context->CSSetShader(compute.Get(), nullptr, 0);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->SOSetTargets(1, &stream, &streamOffset);
        context->SetPredication(predicate.Get(), TRUE);
    }

    void ReportDebugErrors()
    {
        if (!debugLayer) return;
        ComPtr<ID3D11InfoQueue> info;
        if (FAILED(device.As(&info)) || !info)
        {
            std::printf("NOTE D3D11 info queue unavailable; error reporting reduced\n");
            return;
        }
        const UINT64 messages = info->GetNumStoredMessages();
        for (UINT64 index = 0; index < messages; ++index)
        {
            SIZE_T length = 0;
            info->GetMessage(index, nullptr, &length);
            std::vector<unsigned char> storage(length);
            auto message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (length && SUCCEEDED(info->GetMessage(index, message, &length))
                && message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            {
                std::printf("DEBUG_ERROR %s\n", message->pDescription);
                Check(false, "D3D11 debug error/corruption");
            }
        }
        std::printf("NOTE D3D11 debug layer stored messages: %llu\n", static_cast<unsigned long long>(messages));
    }
};

// The pipeline state Bind sets, as the context reports it: what ImGui's own backup restores and what only the menu's
// context state isolation keeps (render target and depth view, compute shader and UAV, stream output, predication,
// a vertex shader resource, a constant buffer above slot 0).
struct D3D11State
{
    UINT viewports = 1, scissors = 1;
    D3D11_VIEWPORT viewport{};
    D3D11_RECT scissor{};
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float factor[4]{};
    UINT mask = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11Buffer> vertexConstants, pixelConstants, upperConstants, streamOut;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ComputeShader> compute;
    ComPtr<ID3D11UnorderedAccessView> computeOutput;
    ComPtr<ID3D11ShaderResourceView> vertexResource;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;

    void Read(ID3D11DeviceContext* context)
    {
        viewports = scissors = 1;
        context->RSGetViewports(&viewports, &viewport);
        context->RSGetScissorRects(&scissors, &scissor);
        context->RSGetState(rasterizer.ReleaseAndGetAddressOf());
        context->OMGetBlendState(blend.ReleaseAndGetAddressOf(), factor, &mask);
        context->OMGetRenderTargets(1, target.ReleaseAndGetAddressOf(), depth.ReleaseAndGetAddressOf());
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetConstantBuffers(0, 1, vertexConstants.ReleaseAndGetAddressOf());
        context->VSGetShaderResources(0, 1, vertexResource.ReleaseAndGetAddressOf());
        context->PSGetConstantBuffers(0, 1, pixelConstants.ReleaseAndGetAddressOf());
        context->PSGetConstantBuffers(2, 1, upperConstants.ReleaseAndGetAddressOf());
        context->PSGetSamplers(0, 1, sampler.ReleaseAndGetAddressOf());
        context->CSGetShader(compute.ReleaseAndGetAddressOf(), nullptr, nullptr);
        context->CSGetUnorderedAccessViews(0, 1, computeOutput.ReleaseAndGetAddressOf());
        context->SOGetTargets(1, streamOut.ReleaseAndGetAddressOf());
        context->GetPredication(predicate.ReleaseAndGetAddressOf(), &predicateValue);
    }
    bool Same(const D3D11State& other) const
    {
        return viewports == other.viewports && scissors == other.scissors
            && !memcmp(&viewport, &other.viewport, sizeof viewport) && !memcmp(&scissor, &other.scissor, sizeof scissor)
            && rasterizer.Get() == other.rasterizer.Get() && blend.Get() == other.blend.Get()
            && !memcmp(factor, other.factor, sizeof factor) && mask == other.mask && topology == other.topology
            && vertexConstants.Get() == other.vertexConstants.Get() && pixelConstants.Get() == other.pixelConstants.Get()
            && upperConstants.Get() == other.upperConstants.Get() && streamOut.Get() == other.streamOut.Get()
            && sampler.Get() == other.sampler.Get() && target.Get() == other.target.Get() && depth.Get() == other.depth.Get()
            && compute.Get() == other.compute.Get() && computeOutput.Get() == other.computeOutput.Get()
            && vertexResource.Get() == other.vertexResource.Get() && predicate.Get() == other.predicate.Get()
            && predicateValue == other.predicateValue
            && rasterizer && blend && vertexConstants && upperConstants && streamOut && sampler && target && depth
            && compute && computeOutput && vertexResource && predicate;
    }
};

// Clears buffer 0 (the current back buffer in every DirectX 11 swap effect), binds the game's state and presents.
// before/after: that state read just before and just after the Present. Predication is off around the harness's own
// clear and read-back copies.
bool PresentD3D11(D3D11Game& game, IDXGISwapChain* chain, ComPtr<ID3D11RenderTargetView>& target, const float color[4],
    D3D11State* before = nullptr, D3D11State* after = nullptr, UINT flags = 0)
{
    game.context->SetPredication(nullptr, FALSE);
    if (!target)
    {
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))
            || FAILED(game.device->CreateRenderTargetView(buffer.Get(), nullptr, &target))) return false;
    }
    game.context->ClearRenderTargetView(target.Get(), color);
    game.Bind();
    if (before) before->Read(game.context.Get());
    const HRESULT result = chain->Present(0, flags);
    if (after) after->Read(game.context.Get());
    game.context->SetPredication(nullptr, FALSE);
    return SUCCEEDED(result) || result == DXGI_STATUS_OCCLUDED;
}

// One pixel of buffer 0, as stored (four 8-bit channels).
bool ReadD3D11(D3D11Game& game, IDXGISwapChain* chain, UINT x, UINT y, std::array<uint8_t, 4>& out)
{
    ComPtr<ID3D11Texture2D> buffer, staging;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    if (x >= desc.Width || y >= desc.Height || desc.SampleDesc.Count != 1) return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(game.device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    game.context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(game.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    memcpy(out.data(), static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4, 4);
    game.context->Unmap(staging.Get(), 0);
    return true;
}

bool NearPixel(const std::array<uint8_t, 4>& a, const std::array<uint8_t, 4>& b)
{
    for (size_t channel = 0; channel < 4; ++channel)
        if (std::abs(int(a[channel]) - int(b[channel])) > 2) return false;
    return true;
}

void OwnForeground(TestWindow& window)
{
    const DWORD ourThread = GetCurrentThreadId();
    window.Pump(4);
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread != ourThread && AttachThreadInput(ourThread, foregroundThread, TRUE);
    BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
    if (attached) AttachThreadInput(ourThread, foregroundThread, FALSE);
    window.Pump(2);
}

// With the game's multithread protection on, another of its threads uses the immediate context under that lock while
// the menu draws. Every time it holds the lock it must find the game's own viewport bound, never the menu's.
struct ContextWatcher
{
    ID3D11Multithread* lock;
    ID3D11DeviceContext* context;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> observations{0}, foreign{0};
    std::thread worker;
    ContextWatcher(ID3D11Multithread* protectedLock, ID3D11DeviceContext* immediate) : lock(protectedLock), context(immediate)
    {
        if (!lock) return;
        worker = std::thread([this] {
            const D3D11_VIEWPORT expected{3.0f, 5.0f, 111.0f, 77.0f, 0.25f, 0.75f};
            while (!stop.load(std::memory_order_acquire))
            {
                lock->Enter();
                UINT count = 1;
                D3D11_VIEWPORT seen{};
                context->RSGetViewports(&count, &seen);
                if (count == 1)
                {
                    observations.fetch_add(1, std::memory_order_relaxed);
                    if (memcmp(&seen, &expected, sizeof seen)) foreign.fetch_add(1, std::memory_order_relaxed);
                }
                lock->Leave();
                SwitchToThread();
            }
        });
    }
    void Stop()
    {
        stop.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }
    ~ContextWatcher() { Stop(); }
};

// How a DirectX 11 game obtains the swapchain the menu attaches to.
// Factory: CreateDXGIFactory1, the adapter it enumerates, D3D11CreateDevice and the factory's own creations.
// Adapter: D3D11CreateDevice on WARP, then the factory from IDXGIDevice -> GetAdapter -> GetParent.
// Chain: D3D11CreateDeviceAndSwapChain, its re-creation from the chain's parent factory, and a call without a device.
enum class D3D11Route { eFactory, eAdapter, eChain };

// The menu on `chain`, opened with its key: pixels, the game's pipeline state unchanged across a Present with the
// menu drawn, no UI on a TEST Present, keyboard capture, ResizeBuffers with the menu open (no reference of the menu's
// may outlive a Present) when `resize`, and closing. `lock`: the game's multithread lock, watched meanwhile.
void ExerciseD3D11Menu(D3D11Game& game, IDXGISwapChain* chain, ComPtr<ID3D11RenderTargetView>& target,
    TestWindow& window, HMODULE module, bool resize, ID3D11Multithread* lock = nullptr)
{
    const float teal[4]{0.10f, 0.60f, 0.60f, 1.0f};
    for (int frame = 0; frame < 4; ++frame) PresentD3D11(game, chain, target, teal);
    std::array<uint8_t, 4> baseline{}, pixel{};
    Check(ReadD3D11(game, chain, 320, 240, baseline), "presented DirectX 11 back buffer read back");
    const uint64_t renderedBefore = Status(module).renderedFrames;
    const WPARAM key = Status(module).shortcutVirtualKey ? Status(module).shortcutVirtualKey : VK_BACK;
    OwnForeground(window);
    Check(GetForegroundWindow() == window.window, "menu input test owns foreground window");
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    {
        ContextWatcher watcher(lock, game.context.Get());
        for (int i = 0; i < 12; ++i)
        {
            PresentD3D11(game, chain, target, teal);
            window.Pump(4);
        }
        Check(Status(module).overlayVisible == 1, "menu key opened the menu on the DirectX 11 chain");
        Check(Status(module).renderedFrames > renderedBefore, "menu drawn on the DirectX 11 chain");
        D3D11State before, after;
        const uint64_t renderedAtState = Status(module).renderedFrames;
        Check(PresentD3D11(game, chain, target, teal, &before, &after)
            && Status(module).renderedFrames > renderedAtState && before.Same(after),
            "the game's whole pipeline state is unchanged across a Present with the menu drawn");
        for (int i = 0; lock && i < 20; ++i) PresentD3D11(game, chain, target, teal);
        watcher.Stop();
        if (lock)
        {
            std::printf("NOTE multithread watcher observations=%u foreign=%u\n", watcher.observations.load(),
                watcher.foreign.load());
            Check(watcher.observations.load() > 0 && watcher.foreign.load() == 0,
                "another thread holding the game's lock never sees the menu's context state");
        }
    }
    const uint64_t renderedAtTest = Status(module).renderedFrames;
    Check(PresentD3D11(game, chain, target, teal, nullptr, nullptr, DXGI_PRESENT_TEST)
        && Status(module).renderedFrames == renderedAtTest, "a TEST Present draws no UI");
    bool differs = false;
    for (int attempt = 0; attempt < 6 && !differs; ++attempt)
    {
        PresentD3D11(game, chain, target, teal);
        window.Pump(3);
        Sleep(60);
        differs = ReadD3D11(game, chain, 320, 240, pixel) && !NearPixel(pixel, baseline);
    }
    Check(differs, "menu visibly composited into the presented DirectX 11 back buffer");
    OwnForeground(window);
    unsigned keysBefore = receivedKeys;
    SendMessageW(window.window, WM_KEYDOWN, 'A', 0x001E0001);
    Check(receivedKeys == keysBefore, "open menu captures keyboard message before game WndProc");
    if (resize)
    {
        // A game releases its own views of the back buffer before ResizeBuffers; the menu holds none.
        game.context->ClearState();
        game.context->Flush();
        target.Reset();
        const HRESULT resized = chain->ResizeBuffers(0, 400, 300, DXGI_FORMAT_UNKNOWN, 0);
        std::printf("NOTE ResizeBuffers with the menu open hr=0x%08lx\n", static_cast<unsigned long>(resized));
        Check(SUCCEEDED(resized), "ResizeBuffers succeeds with the menu open");
        differs = false;
        for (int attempt = 0; attempt < 6 && !differs; ++attempt)
        {
            PresentD3D11(game, chain, target, teal);
            window.Pump(3);
            differs = ReadD3D11(game, chain, 200, 150, pixel) && !NearPixel(pixel, baseline);
        }
        Check(differs, "menu composited after the resize");
    }
    OwnForeground(window);
    PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
    PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
    for (int i = 0; i < 6; ++i)
    {
        PresentD3D11(game, chain, target, teal);
        window.Pump(3);
    }
    Check(Status(module).overlayVisible == 0, "second menu key press closed the menu");
    keysBefore = receivedKeys;
    SendMessageW(window.window, WM_KEYDOWN, 'A', 0x001E0001);
    Check(receivedKeys == keysBefore + 1, "closed menu returns keyboard message to game WndProc");
    Check(SUCCEEDED(game.device->GetDeviceRemovedReason()), "device healthy after the menu");
}

// The application lets go of a chain: its views first, then the chain, whose proxy must retire with it.
template<class SwapChainPtr> void ReleaseD3D11Chain(D3D11Game& game, ComPtr<ID3D11RenderTargetView>& target,
    SwapChainPtr& chain, TestWindow& window, HMODULE module)
{
    game.context->ClearState();
    game.context->Flush();
    target.Reset();
    chain.Reset();
    window.Pump(2);
    Check(Status(module).liveSwapchainWrappers == 0, "released DirectX 11 chain retired its proxy");
}

DXGI_SWAP_CHAIN_DESC LegacyD3D11Desc(HWND window)
{
    DXGI_SWAP_CHAIN_DESC legacy{};
    legacy.BufferDesc.Width = 640;
    legacy.BufferDesc.Height = 480;
    legacy.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    legacy.SampleDesc.Count = 1;
    legacy.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    legacy.BufferCount = 1;
    legacy.OutputWindow = window;
    legacy.Windowed = TRUE;
    legacy.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    return legacy;
}

bool FactoryProxy(IUnknown* factory)
{
    ComPtr<IUnknown> own;
    return factory && factory->QueryInterface(kFactoryProxy, reinterpret_cast<void**>(own.GetAddressOf())) == S_OK && own;
}

// The menu on DirectX 11, through each route a DirectX 11 game takes to its swapchain. d3d11-menu (factory route) saves
// [Overlay] PinnedFps=1: a DirectX 11 game has no frame generation rates, so no readout may appear once the startup hint
// is gone; a Present without UI is the control for the state check. It also runs a legacy blt-model sRGB chain.
// d3d11-mt-menu adds the game's ID3D11Multithread protection; d3d11-adapter-menu takes the factory from the device's
// adapter (GetParent); d3d11-chain-menu uses D3D11CreateDeviceAndSwapChain, its parent factory and a call without a
// device. Every proxy retires with the application's references; D3D11 debug-layer errors fail.
int RunD3D11Menu(const wchar_t* dll, D3D11Route route, bool multithread)
{
    const char* const mode = multithread ? "d3d11-mt-menu" : route == D3D11Route::eFactory ? "d3d11-menu"
        : route == D3D11Route::eAdapter ? "d3d11-adapter-menu" : "d3d11-chain-menu";
    const bool full = route == D3D11Route::eFactory && !multithread;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto ini = std::filesystem::path(exe).parent_path() / L"RTX40MFG-UI.ini";
    if (full && !Check(GetPrivateProfileIntW(L"Overlay", L"PinnedFps", 0, ini.c_str()) == 1,
            "pinned FPS readout saved before startup")) return 1;
    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp11", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    const float teal[4]{0.10f, 0.60f, 0.60f, 1.0f};
    D3D11Game game;
    ComPtr<ID3D11RenderTargetView> target;
    std::array<uint8_t, 4> pixel{}, center{};

    if (route == D3D11Route::eChain)
    {
        // ------------------------------------------ D3D11CreateDeviceAndSwapChain
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 640;
        desc.BufferDesc.Height = 480;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window.window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain> chain;
        if (!Check(game.CreateWithChain(desc, chain) && Status(module).swapchainWrappersCreated == 1
                && Status(module).liveSwapchainWrappers == 1,
                "D3D11CreateDeviceAndSwapChain's swapchain wrapped by the menu")) return 1;
        Check(Status(module).overlayInstallState == kStateActive, "the DirectX 11 swapchain return activated the menu");
        if (!game.debugLayer) std::printf("NOTE D3D11 debug layer unavailable on this machine; validation reduced\n");
        ComPtr<ID3D11Device> reported;
        Check(SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&reported))) && reported == game.device,
            "the wrapped chain reports the device the call returned");
        reported.Reset();
        ComPtr<IDXGIFactory> parent;
        Check(SUCCEEDED(chain->GetParent(IID_PPV_ARGS(&parent))) && FactoryProxy(parent.Get())
            && Status(module).factoryWrappersCreated == 1, "the chain's parent factory is the menu's proxy");
        for (int frame = 0; frame < 4; ++frame) PresentD3D11(game, chain.Get(), target, teal);
        Check(ReadD3D11(game, chain.Get(), 320, 240, center) && ReadD3D11(game, chain.Get(), 24, 24, pixel)
            && !NearPixel(pixel, center), "startup hint composited into the retained DirectX 11 back buffer");
        ExerciseD3D11Menu(game, chain.Get(), target, window, module, true);
        ReleaseD3D11Chain(game, target, chain, window, module);

        // The game creates its chain again from that parent (an MSAA or fullscreen change): it keeps the menu.
        if (parent)
        {
            DXGI_SWAP_CHAIN_DESC again = LegacyD3D11Desc(window.window);
            ComPtr<IDXGISwapChain> recreated;
            if (!Check(SUCCEEDED(parent->CreateSwapChain(game.device.Get(), &again, &recreated)) && recreated
                    && Status(module).swapchainWrappersCreated == 2 && Status(module).liveSwapchainWrappers == 1,
                    "a chain created again from the parent factory keeps the menu")) return 1;
            ExerciseD3D11Menu(game, recreated.Get(), target, window, module, false);
            ReleaseD3D11Chain(game, target, recreated, window, module);
            parent.Reset();
        }
        game.ReportDebugErrors();

        // A call that asks for no device: the swapchain reports the one the menu draws with.
        D3D11Game second;
        const auto create = D3D11Game::Entry<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>("D3D11CreateDeviceAndSwapChain");
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const DXGI_SWAP_CHAIN_DESC legacy = LegacyD3D11Desc(window.window);
        const auto wrappers = Status(module).swapchainWrappersCreated;
        ComPtr<IDXGISwapChain> legacyChain;
        if (!Check(create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                &legacy, &legacyChain, nullptr, nullptr, nullptr)) && legacyChain
                && Status(module).swapchainWrappersCreated == wrappers + 1 && Status(module).liveSwapchainWrappers == 1
                && second.Adopt(legacyChain.Get()),
                "D3D11CreateDeviceAndSwapChain without a device out (blt model, sRGB) wrapped by the menu")) return 1;
        ExerciseD3D11Menu(second, legacyChain.Get(), target, window, module, false);
        ReleaseD3D11Chain(second, target, legacyChain, window, module);
        second.ReportDebugErrors();
    }
    else
    {
        ComPtr<IDXGIFactory2> factory;
        if (route == D3D11Route::eFactory)
        {
            // ------------------------------------------------ factory route
            ComPtr<IDXGIFactory4> created;
            ComPtr<IDXGIAdapter> adapter;
            if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&created))) && Status(module).factoryWrappersCreated >= 1
                    && SUCCEEDED(created.As(&factory)), "application factory created through the menu's gateway")) return 1;
            if (!Check(SUCCEEDED(created->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter enumerated through the proxy"))
                return 1;
            if (!Check(game.CreateDevice(adapter.Get()), "D3D11 WARP device and the game's state objects created")) return 1;
        }
        else
        {
            // ------------------------------------------ device -> adapter -> GetParent
            if (!Check(game.CreateDevice(nullptr), "D3D11 WARP device (no adapter) and the game's state objects created"))
                return 1;
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            const auto factories = Status(module).factoryWrappersCreated;
            if (!Check(SUCCEEDED(game.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))
                    && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && FactoryProxy(factory.Get())
                    && Status(module).factoryWrappersCreated == factories + 1,
                    "the factory the device's adapter gives the executable is the menu's proxy")) return 1;
            Check(Status(module).overlayInstallState == kStateActive, "the adapter's factory activated the menu");
        }
        if (!game.debugLayer) std::printf("NOTE D3D11 debug layer unavailable on this machine; validation reduced\n");
        ComPtr<ID3D11Multithread> lock;
        if (multithread)
        {
            if (!Check(SUCCEEDED(game.context.As(&lock)) && lock, "the game's immediate context offers its lock")) return 1;
            lock->SetMultithreadProtected(TRUE);
            Check(lock->GetMultithreadProtected() == TRUE, "the game's multithread protection is on");
        }

        // ------------------------------------------------- flip-sequential chain
        const auto wrappers = Status(module).swapchainWrappersCreated;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 640;
        desc.Height = 480;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> chain;
        if (!Check(SUCCEEDED(factory->CreateSwapChainForHwnd(game.device.Get(), window.window, &desc, nullptr, nullptr, &chain))
                && Status(module).swapchainWrappersCreated == wrappers + 1 && Status(module).liveSwapchainWrappers == 1,
                "DirectX 11 swapchain wrapped by the menu")) return 1;
        const ULONGLONG start = GetTickCount64();
        const uint64_t framesBefore = Status(module).dxgiFrames;
        for (int frame = 0; frame < 6; ++frame)
        {
            if (!Check(PresentD3D11(game, chain.Get(), target, teal), "DirectX 11 frame rendered and presented")) return 1;
            window.Pump(1);
        }
        Check(Status(module).dxgiFrames >= framesBefore + 6, "DirectX 11 Presents counted on the principal chain");
        Check(ReadD3D11(game, chain.Get(), 320, 240, center) && ReadD3D11(game, chain.Get(), 24, 24, pixel)
            && !NearPixel(pixel, center), "startup hint composited into the retained DirectX 11 back buffer");
        if (full)
        {
            while (GetTickCount64() - start < 8500)
            {
                PresentD3D11(game, chain.Get(), target, teal);
                window.Pump(1);
                Sleep(50);
            }
            for (int frame = 0; frame < 3; ++frame) PresentD3D11(game, chain.Get(), target, teal);
            Check(Status(module).overlayVisible == 0, "menu stays closed without the key");
            Check(ReadD3D11(game, chain.Get(), 24, 24, pixel) && NearPixel(pixel, center), "startup hint gone after 7 seconds");
            Check(ReadD3D11(game, chain.Get(), 20, 18, pixel) && NearPixel(pixel, center),
                "no pinned FPS readout in a DirectX 11 game");
            D3D11State before, after;
            const uint64_t rendered = Status(module).renderedFrames;
            Check(PresentD3D11(game, chain.Get(), target, teal, &before, &after)
                && Status(module).renderedFrames == rendered && before.Same(after),
                "control: a Present without UI keeps the game's whole pipeline state");
        }
        ExerciseD3D11Menu(game, chain.Get(), target, window, module, true, lock.Get());
        ReleaseD3D11Chain(game, target, chain, window, module);

        if (full)
        {
            // ------------------------------------------- legacy blt-model sRGB chain
            DXGI_SWAP_CHAIN_DESC legacy = LegacyD3D11Desc(window.window);
            ComPtr<IDXGISwapChain> legacyChain;
            if (!Check(SUCCEEDED(factory->CreateSwapChain(game.device.Get(), &legacy, &legacyChain))
                    && Status(module).swapchainWrappersCreated == wrappers + 2 && Status(module).liveSwapchainWrappers == 1,
                    "legacy CreateSwapChain (blt model, sRGB) wrapped by the menu")) return 1;
            ExerciseD3D11Menu(game, legacyChain.Get(), target, window, module, false);
            ReleaseD3D11Chain(game, target, legacyChain, window, module);
        }
        game.ReportDebugErrors();
    }

    // -------------------------------------------------------------- teardown
    window.Pump(4);
    Check(Status(module).liveSwapchainWrappers == 0, "all swapchain proxies retired after teardown");
    Check(Status(module).renderFailures == 0, "no UI render failures on DirectX 11");
    Check(SUCCEEDED(game.device->GetDeviceRemovedReason()), "device healthy at teardown");
    std::printf("COMPLETE mode=%s\n", mode);
    return gFailures == 0 ? 0 : 10;
}

// d3d11-after-d3d12: an application that created a D3D12 device first, through the executable's own D3D12 entry (the
// menu's D3D12 gateway answers it), is a D3D12 application. Its later D3D11 device arms no adapter-parent route and
// its D3D11 swapchain gets no DirectX 11 menu: no session, no Presents counted or drawn on.
int RunD3D11AfterD3D12(const wchar_t* dll)
{
    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp11", 640, 480, GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    HMODULE module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate loads")) return 1;
    using CreateDevice12 = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create12 = reinterpret_cast<CreateDevice12>(GetProcAddress(SystemModule(L"d3d12.dll"), "D3D12CreateDevice"));
    ComPtr<ID3D12Device> device12;
    HRESULT created12 = create12 ? create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)) : E_FAIL;
    bool warp12 = false;
    if (FAILED(created12) && create12)
    {
        // No hardware D3D12 adapter: WARP. A D3D12 device on an adapter arms the D3D12 adapter-parent route itself.
        ComPtr<IDXGIFactory4> factory4;
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))) && SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            created12 = create12(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
        warp12 = SUCCEEDED(created12);
        std::printf("NOTE no hardware D3D12 adapter: the D3D12 device is on WARP\n");
    }
    if (!Check(SUCCEEDED(created12) && device12, "the application's D3D12 device created first")) return 1;
    D3D11Game game;
    if (!Check(game.CreateDevice(nullptr), "a D3D11 WARP device created after it")) return 1;
    if (!warp12)
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> native;
        const auto factories = Status(module).factoryWrappersCreated;
        Check(SUCCEEDED(game.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))
            && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&native))) && !FactoryProxy(native.Get())
            && Status(module).factoryWrappersCreated == factories,
            "the D3D11 device's adapter gives the native factory: no adapter-parent arming from it");
    }
    ComPtr<IDXGIFactory2> factory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && FactoryProxy(factory.Get()),
            "application factory created through the menu's gateway")) return 1;
    const auto wrappers = Status(module).swapchainWrappersCreated;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 640;
    desc.Height = 480;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain1> chain;
    if (!Check(SUCCEEDED(factory->CreateSwapChainForHwnd(game.device.Get(), window.window, &desc, nullptr, nullptr, &chain))
            && Status(module).swapchainWrappersCreated == wrappers,
            "a D3D11 swapchain of a D3D12 application gets no DirectX 11 menu")) return 1;
    ComPtr<ID3D11RenderTargetView> target;
    const float teal[4]{0.10f, 0.60f, 0.60f, 1.0f};
    const auto frames = Status(module).dxgiFrames;
    const auto rendered = Status(module).renderedFrames;
    for (int frame = 0; frame < 6; ++frame)
    {
        PresentD3D11(game, chain.Get(), target, teal);
        window.Pump(1);
    }
    Check(Status(module).dxgiFrames == frames && Status(module).renderedFrames == rendered,
        "its Presents are neither counted nor drawn on");
    game.context->ClearState();
    game.context->Flush();
    target.Reset();
    chain.Reset();
    window.Pump(2);
    Check(Status(module).liveSwapchainWrappers == 0, "no swapchain proxy left");
    std::printf("COMPLETE mode=d3d11-after-d3d12\n");
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
    if (mode == L"d3d11-menu") return RunD3D11Menu(dll, D3D11Route::eFactory, false);
    if (mode == L"d3d11-mt-menu") return RunD3D11Menu(dll, D3D11Route::eFactory, true);
    if (mode == L"d3d11-adapter-menu") return RunD3D11Menu(dll, D3D11Route::eAdapter, false);
    if (mode == L"d3d11-chain-menu") return RunD3D11Menu(dll, D3D11Route::eChain, false);
    if (mode == L"d3d11-after-d3d12") return RunD3D11AfterD3D12(dll);
    std::printf("unknown mode\n");
    return 2;
}
