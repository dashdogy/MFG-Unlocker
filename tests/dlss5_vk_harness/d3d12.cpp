// vkdlss5.1 parity twin (local tool, not shipped): the Vulkan harness's scene, settings, jitter sequence and frame
// counts on D3D12, through the D3D12 DLSS 5 stage (dlss5.cpp) that the Vulkan stage ports. MFG Unlocker loads as this
// process's winmm.dll; the driver's NGX runtime runs DLSS Super Resolution (project-ID Init) and the output is read
// back with DLSS 5 off (A) and on (B). With "dump", output-a-off.rgba16f / output-b-on.rgba16f are written beside the
// exe in the Vulkan harness's layout, for compare_outputs.py.
//
// Usage: Dlss5D3d12Harness [frames=N] [stage=0|2] [passes=N] [scale=P] [model=N] [dump]
#include <Windows.h>
#include <mmsystem.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include "harness_common.h"

#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace harness;

namespace
{
FILE* gNgxFile = nullptr;
void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (gNgxFile) { fputs(message ? message : "", gNgxFile); fflush(gNgxFile); }
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t value = 0;
    bool Begin() { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); }
    bool Submit()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++value))) return false;
        if (fence->GetCompletedValue() < value)
        {
            fence->SetEventOnCompletion(value, event);
            if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return false;
        }
        return true;
    }
};

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav,
    D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    return texture;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{type};
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

// texels (tightly packed rows of `stride` bytes) into `texture` (COPY_DEST) through an upload buffer.
ComPtr<ID3D12Resource> Upload(Gpu& gpu, ID3D12Resource* texture, const void* texels, uint32_t width, uint32_t height, uint32_t stride)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    const auto desc = texture->GetDesc();
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto upload = Buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, static_cast<const uint8_t*>(texels) + size_t(y) * stride, stride);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.SubresourceIndex = 0; from.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    (void)width;
    return upload;
}
}

int main(int argc, char** argv)
{
    timeBeginPeriod(1);
    unsigned frames = 90;
    Nr nr{};
    bool dump = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strncmp(argv[i], "frames=", 7)) frames = static_cast<unsigned>(atoi(argv[i] + 7));
        else if (!strncmp(argv[i], "stage=", 6)) nr.stage = static_cast<unsigned>(atoi(argv[i] + 6));
        else if (!strncmp(argv[i], "passes=", 7)) nr.passes = static_cast<unsigned>(atoi(argv[i] + 7));
        else if (!strncmp(argv[i], "scale=", 6)) nr.scale = static_cast<float>(atof(argv[i] + 6));
        else if (!strncmp(argv[i], "model=", 6)) nr.model = static_cast<unsigned>(atoi(argv[i] + 6));
        else if (!strncmp(argv[i], "extra=", 6)) nr.extra = ExtraFields(argv[i] + 6);
        else if (!strcmp(argv[i], "dump")) dump = true;
    }
    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(L"winmm.dll"), modPath, MAX_PATH);
    printf("winmm.dll = %ls\nD3D12 frames %u stage %u passes %u scale %.0f model %u extra '%s'\n", modPath, frames, nr.stage, nr.passes, nr.scale,
        nr.model, nr.extra.c_str());
    if (!WriteControl(Nr{})) { printf("SETUP control write failed\n"); return 2; }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) { printf("adapter: %ls\n", desc.Description); break; }
        adapter.Reset();
    }
    if (!adapter) { printf("SETUP no NVIDIA adapter\n"); return 2; }
    Gpu gpu;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)))) { printf("SETUP device\n"); return 2; }
    D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    gpu.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue));
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list));
    gpu.list->Close();
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const std::wstring dataPath = Directory();
    gNgxFile = _wfopen((dataPath + L"\\ngx-callback.log").c_str(), L"wb");
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = &NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID("7f4b5c2e-91d3-4a60-b8e7-3c5d0a1f9e24", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
        dataPath.c_str(), gpu.device.Get(), &info);
    printf("NGX init 0x%08X\n", static_cast<unsigned>(result));
    if (NVSDK_NGX_FAILED(result)) return 2;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters);

    const unsigned outWidth = 2560, outHeight = 1440, renderWidth = 1707, renderHeight = 960;
    auto color = Texture(gpu.device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    auto depth = Texture(gpu.device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    auto motion = Texture(gpu.device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    auto output = Texture(gpu.device.Get(), outWidth, outHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback = Buffer(gpu.device.Get(), uint64_t(outWidth) * outHeight * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!color || !depth || !motion || !output || !readback) { printf("SETUP resources\n"); return 2; }
    const auto scene = Scene(renderWidth, renderHeight);
    const std::vector<float> depthTexels(size_t(renderWidth) * renderHeight, 0.5f);
    const std::vector<uint32_t> motionTexels(size_t(renderWidth) * renderHeight, 0u);
    if (!gpu.Begin()) return 2;
    auto u0 = Upload(gpu, color.Get(), scene.data(), renderWidth, renderHeight, renderWidth * 8);
    auto u1 = Upload(gpu, depth.Get(), depthTexels.data(), renderWidth, renderHeight, renderWidth * 4);
    auto u2 = Upload(gpu, motion.Get(), motionTexels.data(), renderWidth, renderHeight, renderWidth * 4);
    for (ID3D12Resource* input : {color.Get(), depth.Get(), motion.Get()})
        Barrier(gpu.list.Get(), input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!gpu.Submit()) { printf("SETUP upload\n"); return 2; }

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = renderWidth; create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = outWidth; create.Feature.InTargetHeight = outHeight;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    NVSDK_NGX_Handle* handle = nullptr;
    if (!gpu.Begin()) return 2;
    result = NGX_D3D12_CREATE_DLSS_EXT(gpu.list.Get(), 1, 1, &handle, parameters, &create);
    const bool createSubmitted = gpu.Submit();
    printf("SR CREATE 0x%08X handle=%p submitted=%d\n", static_cast<unsigned>(result), static_cast<void*>(handle), createSubmitted);
    if (NVSDK_NGX_FAILED(result) || !handle || !createSubmitted) return 2;

    NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = color.Get(); eval.Feature.pInOutput = output.Get();
    eval.pInDepth = depth.Get(); eval.pInMotionVectors = motion.Get();
    eval.InRenderSubrectDimensions = {renderWidth, renderHeight};
    unsigned evaluateFailures = 0, submitFailures = 0, frameIndex = 0;
    const auto run = [&](unsigned count)
    {
        for (unsigned i = 0; i != count; ++i, ++frameIndex)
        {
            eval.InJitterOffsetX = (frameIndex % 2) ? 0.25f : -0.25f;
            eval.InJitterOffsetY = (frameIndex % 3) ? 0.25f : -0.25f;
            if (!gpu.Begin()) { ++submitFailures; continue; }
            evaluateFailures += NVSDK_NGX_FAILED(NGX_D3D12_EVALUATE_DLSS_EXT(gpu.list.Get(), handle, parameters, &eval));
            submitFailures += !gpu.Submit();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    };
    const auto read = [&](const char* name)
    {
        std::vector<float> texels(size_t(outWidth) * outHeight * 4);
        if (!gpu.Begin()) return texels;
        Barrier(gpu.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        const auto desc = output->GetDesc();
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, from{output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.PlacedFootprint = footprint; from.SubresourceIndex = 0;
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(gpu.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!gpu.Submit()) { printf("readback submit failed\n"); return texels; }
        uint8_t* mapped = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        std::vector<uint16_t> halfs(texels.size());
        for (uint32_t y = 0; y < outHeight; ++y)
            std::memcpy(&halfs[size_t(y) * outWidth * 4], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(outWidth) * 8);
        readback->Unmap(0, nullptr);
        Decode(halfs.data(), texels, dump, name);
        return texels;
    };
    bool ok = true;
    const auto check = [&ok](bool condition, const char* what) { printf("%s %s\n", condition ? "PASS" : "FAIL", what); ok &= condition; };

    run(30);
    const auto a = read("a-off");
    check(PrintStatus("A off").state == 0, "A: DLSS 5 off");
    Nr on = nr; on.on = true;
    WriteControl(on);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(frames);
    const auto b = read("b-on");
    Status running{};
    for (int sample = 0; sample < 20; ++sample)
    {
        run(4);
        running = PrintStatus("B on");
        if (running.state == 2) break;
    }
    const Stats changed = Compare(a, b);
    printf("B vs A: meanAbs=%.5f maxAbs=%.4f changed=%.2f%% nonFinite=%zu meanLuma A=%.4f B=%.4f\n", changed.meanAbs, changed.maxAbs,
        changed.changed * 100.0, changed.nonFinite, Compare(a, a).meanLuma, changed.meanLuma);
    check(running.state == 2 && running.evaluations > 0 && running.failures == 0, "B: DLSS 5 running on D3D12");
    check(changed.nonFinite == 0 && changed.changed > 0.01, "B: the DLSS output changed, finite");
    WriteControl(Nr{});
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    run(20);
    result = NVSDK_NGX_D3D12_ReleaseFeature(handle);
    printf("SR RELEASE 0x%08X evaluateFailures=%u submitFailures=%u\n", static_cast<unsigned>(result), evaluateFailures, submitFailures);
    printf("DONE %s\n", ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (gNgxFile) fflush(gNgxFile);
    const int code = ok ? 0 : 1;
    TerminateProcess(GetCurrentProcess(), code);
    return code;
}
