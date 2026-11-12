#include "hudless_match.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace xess_hudless_match
{
namespace
{
constexpr D3D12_RESOURCE_STATES kOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // the game's own HUD-less tag state (192)
constexpr UINT kConstants = 8;
constexpr uint32_t kMinimumCount = 8;

// Compiled at run time by the system compiler, as motion_fill does. Our own command list, so a
// descriptor table (t0 HUD-less, t1 back buffer, u0 our texture) is fine; u1/u2 are root UAV buffers.
constexpr uint32_t kStride = 4; // histogram samples every 4th pixel in x and y (STRIDE in the shader)
static_assert(kCodes == 1024 && kBins == 256, "the median pass gives each of 256 threads 4 codes");
constexpr char kShader[] = R"(
#define STRIDE 4
#define OUTLIER (32.0 / 1023.0)
cbuffer Match : register(b0)
{
    uint width, height;
    float logMin, binsPerOctave;
    uint bins, codes, minimumCount, unused;
};
Texture2D<float4> hudless : register(t0);
Texture2D<float4> backBuffer : register(t1);
RWTexture2D<float4> output : register(u0);
RWByteAddressBuffer histogram : register(u1);
RWByteAddressBuffer lut : register(u2);

uint BinOf(float v)
{
    if (!(v > 0.0)) return 0;
    return (uint)clamp((log2(v) - logMin) * binsPerOctave, 0.0, (float)(bins - 1));
}

[numthreads(64, 1, 1)]
void clear(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 3 * bins * codes) histogram.Store(id.x * 4, 0);
}

[numthreads(8, 8, 1)]
void accumulate(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy * STRIDE;
    if (p.x >= width || p.y >= height) return;
    const float3 h = hudless.Load(int3(p, 0)).rgb;
    const float3 b = backBuffer.Load(int3(p, 0)).rgb;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const uint code = (uint)(saturate(b[c]) * (codes - 1) + 0.5);
        uint previous;
        histogram.InterlockedAdd(((c * bins + BinOf(h[c])) * codes + code) * 4, 1, previous);
    }
}

// One group per (bin, channel): 256 threads own 4 codes each, an inclusive scan finds the median code.
groupshared uint partial[256];
[numthreads(256, 1, 1)]
void median(uint3 group : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint bin = group.x, c = group.y;
    const uint base = (c * bins + bin) * codes + gi * 4;
    uint counts[4];
    uint sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k) { counts[k] = histogram.Load((base + k) * 4); sum += counts[k]; }
    partial[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint offset = 1; offset < 256; offset <<= 1)
    {
        uint v = partial[gi];
        if (gi >= offset) v += partial[gi - offset];
        GroupMemoryBarrierWithGroupSync();
        partial[gi] = v;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint total = partial[255];
    const uint before = gi ? partial[gi - 1] : 0;
    const uint slot = (c * bins + bin) * 4;
    if (total < minimumCount) { if (gi == 0) lut.Store(slot, asuint(-1.0)); return; }
    const uint middle = (total + 1) / 2;
    if (before < middle && partial[gi] >= middle)
    {
        uint seen = before, code = gi * 4 + 3;
        for (uint j = 0; j < 4; ++j) { seen += counts[j]; if (seen >= middle) { code = gi * 4 + j; break; } }
        lut.Store(slot, asuint((float)code / (float)(codes - 1)));
    }
}

[numthreads(1, 1, 1)]
void fill(uint3 group : SV_GroupID)
{
    const uint c = group.x, base = c * bins;
    // A bin further than OUTLIER from the 5-bin running median of the filled bins is replaced by it: a
    // bin or two taken over by UI pixels (a HUD panel over a narrow range of scene values) is such an
    // outlier; a smooth monotonic curve (median within a bin step or two) is left unchanged.
    float raw[256], l[256];
    for (uint r = 0; r < 256; ++r) { raw[r] = asfloat(lut.Load((base + r) * 4)); l[r] = raw[r]; }
    for (uint k = 0; k < 256; ++k)
    {
        if (raw[k] < 0.0) continue;
        float w[5];
        uint n = 0;
        for (int d = -2; d <= 2; ++d)
        {
            const int j = (int)k + d;
            if (j >= 0 && j < 256 && raw[j] >= 0.0) { w[n] = raw[j]; ++n; }
        }
        for (uint a = 1; a < n; ++a)
        {
            const float x = w[a];
            int p = (int)a - 1;
            while (p >= 0 && w[p] > x) { w[p + 1] = w[p]; --p; }
            w[p + 1] = x;
        }
        if (abs(raw[k] - w[n / 2]) > OUTLIER) l[k] = w[n / 2];
    }
    int first = -1, last = -1;
    for (uint i = 0; i < 256; ++i) if (l[i] >= 0.0) { if (first < 0) first = (int)i; last = (int)i; }
    if (first < 0) { for (uint z = 0; z < 256; ++z) lut.Store((base + z) * 4, asuint(0.0)); return; }
    // Empty bins between filled ones: linear between their neighbours.
    int previous = first;
    for (int b = first + 1; b <= last; ++b)
    {
        if (l[b] < 0.0) continue;
        for (int g = previous + 1; g < b; ++g) l[g] = lerp(l[previous], l[b], (float)(g - previous) / (float)(b - previous));
        previous = b;
    }
    // Beyond the sampled range (rare highlights, deep shadows): the slope of the end's last 4 bins,
    // never decreasing, clamped to the code range.
    const int span = min(4, last - first);
    const float headSlope = span > 0 ? max((l[first + span] - l[first]) / (float)span, 0.0) : 0.0;
    const float tailSlope = span > 0 ? max((l[last] - l[last - span]) / (float)span, 0.0) : 0.0;
    for (int h = 0; h < first; ++h) l[h] = saturate(l[first] - headSlope * (float)(first - h));
    for (int t = last + 1; t < 256; ++t) l[t] = saturate(l[last] + tailSlope * (float)(t - last));
    for (uint o = 0; o < 256; ++o) lut.Store((base + o) * 4, asuint(l[o]));
}

[numthreads(8, 8, 1)]
void apply(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float3 h = hudless.Load(int3(id.xy, 0)).rgb;
    float3 o = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float v = h[c];
        float f = v > 0.0 ? (log2(v) - logMin) * binsPerOctave - 0.5 : 0.0;
        f = clamp(f, 0.0, (float)(bins - 1));
        const uint i = min((uint)f, bins - 2);
        const float t = saturate(f - (float)i);
        o[c] = lerp(asfloat(lut.Load((c * bins + i) * 4)), asfloat(lut.Load((c * bins + i + 1) * 4)), t);
    }
    output[id.xy] = float4(o, 1.0);
}
)";
constexpr const char* kEntries[5] = {"clear", "accumulate", "median", "fill", "apply"};

struct ShaderConstants
{
    uint32_t width, height;
    float logMin, binsPerOctave;
    uint32_t bins, codes, minimumCount, unused;
};
static_assert(sizeof(ShaderConstants) == kConstants*4);

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
D3D12_RESOURCE_BARRIER Uav(ID3D12Resource* resource) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

bool Buffer(ID3D12Device* device, uint64_t bytes, ComPtr<ID3D12Resource>& out) noexcept
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&out)));
}

// A typed view format for reading a resource's stored values (typeless families get their UNORM/FLOAT view;
// sRGB formats are refused: XeFG would decode the back buffer but not our UNORM copy).
DXGI_FORMAT ViewFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_UNKNOWN;
    default: return format;
    }
}
}

const char* ModeName(Mode mode) noexcept { return mode == Mode::eMatch ? "match" : "game"; }

Matcher::Matcher(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept : device_(device), queue_(queue) {}

Matcher::~Matcher()
{
    if (fence_ && fenceValue_ && fence_->GetCompletedValue() < fenceValue_ && event_
        && SUCCEEDED(fence_->SetEventOnCompletion(fenceValue_, event_)))
        WaitForSingleObject(event_, 2000);
    if (event_) CloseHandle(event_);
}

bool Matcher::Pipeline(const char*& reason) noexcept
{
    if (pipelines_[0]) return true;
    if (pipelineTried_) { reason = "pipeline_unavailable"; return false; }
    pipelineTried_ = true;
    try
    {
        HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!compiler) compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        if (!d3d12) d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        const auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
        if (!compile || !serialize) { reason = "compiler_unavailable"; return false; }
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2; ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, kConstants};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {2, ranges};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[2].Descriptor = {1, 0};
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[3].Descriptor = {2, 0};
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 4; desc.pParameters = parameters;
        ComPtr<ID3DBlob> blob, messages;
        if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages))
            || FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
        { reason = "root_signature"; return false; }
        std::array<ComPtr<ID3D12PipelineState>, 5> built;
        for (size_t i = 0; i < built.size(); ++i)
        {
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(compile(kShader, sizeof(kShader)-1, "rtxmfg_hudless_match", nullptr, nullptr, kEntries[i], "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) || !code) { reason = "shader_compile"; return false; }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root_.Get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&built[i])))) { reason = "pipeline_state"; return false; }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heapDesc.NumDescriptors = UINT(3*slots_.size());
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)))) { reason = "descriptor_heap"; return false; }
        descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!Buffer(device_.Get(), uint64_t(3)*kBins*kCodes*4, histogram_) || !Buffer(device_.Get(), uint64_t(3)*kBins*4, lut_))
        { reason = "buffers"; return false; }
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || !event_) { reason = "fence"; return false; }
        pipelines_ = std::move(built);
        return true;
    }
    catch (...) { reason = "pipeline_exception"; return false; }
}

bool Matcher::Prepare(const D3D12_RESOURCE_DESC& backBuffer, const char*& reason) noexcept
{
    const DXGI_FORMAT format = ViewFormat(backBuffer.Format);
    if (prepared_ && outputDesc_.Width == backBuffer.Width && outputDesc_.Height == backBuffer.Height && outputDesc_.Format == format)
        return true;
    if (format == DXGI_FORMAT_UNKNOWN) { reason = "srgb_back_buffer"; return false; }
    // Code values 0..1 only: a float (scRGB) back buffer would clip in the histogram.
    if (format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM)
    { reason = "back_buffer_format_not_unorm"; return false; }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
        || !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) { reason = "back_buffer_format_not_storable"; return false; }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = backBuffer.Width; desc.Height = backBuffer.Height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    std::array<Slot, 4> fresh;
    for (auto& slot : fresh)
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kOutputState, nullptr, IID_PPV_ARGS(&slot.output)))
            || FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
            || FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list)))
            || FAILED(slot.list->Close()))
        { reason = "allocation"; return false; }
    if (prepared_) retired_.emplace_back(fenceValue_, std::move(slots_));
    slots_ = std::move(fresh);
    outputDesc_ = desc;
    prepared_ = true; next_ = 0;
    return true;
}

bool Matcher::WaitSlot(const Slot& slot, DWORD milliseconds) noexcept
{
    if (!slot.fence || fence_->GetCompletedValue() >= slot.fence) return true;
    if (FAILED(fence_->SetEventOnCompletion(slot.fence, event_))) return false;
    return WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0 || fence_->GetCompletedValue() >= slot.fence;
}

bool Matcher::OnHudless(uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& hudless, Mode mode, const char*& reason) noexcept
{
    reason = nullptr;
    try
    {
        if (mode == Mode::eGame) return false;
        if (broken_) { reason = "disabled"; return false; }
        if (pending_ && frame != frame_)
        {
            // The previous frame's HUD-less input was consumed but its Present never came through
            // PreparePresent: the game's own tags are the only safe route then. (A repeated tag of the
            // same frame replaces the pending one.)
            broken_ = true; pending_ = false; hudless_.Reset(); reason = "present_hook_missing"; return false;
        }
        if (!hudless.pResource) { reason = "no_resource"; return false; }
        if (hudless.validity != XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT) { reason = "hudless_only_now"; return false; }
        const auto desc = hudless.pResource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
            || ViewFormat(desc.Format) == DXGI_FORMAT_UNKNOWN || hudless.resourceBase.x || hudless.resourceBase.y
            || hudless.resourceSize.x != desc.Width || hudless.resourceSize.y != desc.Height) { reason = "hudless_shape"; return false; }
        if (!Pipeline(reason)) { broken_ = true; return false; }
        hudless_ = hudless.pResource;
        hudlessState_ = hudless.incomingState;
        base_ = hudless.resourceBase; size_ = hudless.resourceSize; frame_ = frame;
        pending_ = true;
        return true;
    }
    catch (...) { reason = "exception"; return false; }
}

bool Matcher::Submit(ID3D12Resource* backBuffer, uint32_t& frame, xefg_swapchain_d3d12_resource_data_t& tag,
    const char*& reason) noexcept
{
    reason = nullptr;
    if (!pending_) { reason = "nothing_pending"; return false; }
    pending_ = false;
    ComPtr<ID3D12Resource> hudless = std::move(hudless_);
    frame = frame_;
    try
    {
        if (!backBuffer || !hudless) { reason = "no_back_buffer"; return false; }
        const auto backDesc = backBuffer->GetDesc();
        const auto hudDesc = hudless->GetDesc();
        if (backDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || backDesc.SampleDesc.Count != 1
            || backDesc.Width != hudDesc.Width || backDesc.Height != hudDesc.Height) { reason = "back_buffer_shape"; return false; }
        if (backDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) { broken_ = true; reason = "back_buffer_not_readable"; return false; }
        if (!Prepare(backDesc, reason)) { broken_ = true; return false; }
        const uint32_t index = next_++ % uint32_t(slots_.size());
        Slot& slot = slots_[index];
        if (!WaitSlot(slot, 250)) { broken_ = true; reason = "slot_busy"; return false; }
        // Descriptors of this slot: t0 HUD-less, t1 back buffer, u0 our texture.
        auto cpu = heap_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(index)*3*descriptorSize_;
        auto gpu = heap_->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(index)*3*descriptorSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = ViewFormat(hudDesc.Format);
        device_->CreateShaderResourceView(hudless.Get(), &srv, cpu);
        srv.Format = ViewFormat(backDesc.Format);
        D3D12_CPU_DESCRIPTOR_HANDLE second{cpu.ptr + descriptorSize_}, third{cpu.ptr + 2*SIZE_T(descriptorSize_)};
        device_->CreateShaderResourceView(backBuffer, &srv, second);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = outputDesc_.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(slot.output.Get(), nullptr, &uav, third);

        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        { broken_ = true; reason = "list_reset"; return false; }
        auto* list = slot.list.Get();
        const bool hudNeedsBarrier = !(hudlessState_ & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        std::vector<D3D12_RESOURCE_BARRIER> begin{
            Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(slot.output.Get(), kOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(histogram_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(lut_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        if (hudNeedsBarrier) begin.push_back(Transition(hudless.Get(), hudlessState_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        list->ResourceBarrier(UINT(begin.size()), begin.data());
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        const ShaderConstants constants{UINT(backDesc.Width), backDesc.Height, kLogMin, kBinsPerOctave, kBins, kCodes, kMinimumCount, 0};
        list->SetComputeRoot32BitConstants(0, kConstants, &constants, 0);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRootUnorderedAccessView(2, histogram_->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, lut_->GetGPUVirtualAddress());
        D3D12_RESOURCE_BARRIER histogramUav = Uav(histogram_.Get()), lutUav = Uav(lut_.Get());
        list->SetPipelineState(pipelines_[0].Get());
        list->Dispatch((3*kBins*kCodes+63)/64, 1, 1);
        list->ResourceBarrier(1, &histogramUav);
        list->SetPipelineState(pipelines_[1].Get());
        const UINT samplesX = (UINT(backDesc.Width)+kStride-1)/kStride, samplesY = (backDesc.Height+kStride-1)/kStride;
        list->Dispatch((samplesX+7)/8, (samplesY+7)/8, 1);
        list->ResourceBarrier(1, &histogramUav);
        list->SetPipelineState(pipelines_[2].Get());
        list->Dispatch(kBins, 3, 1);
        list->ResourceBarrier(1, &lutUav);
        list->SetPipelineState(pipelines_[3].Get());
        list->Dispatch(3, 1, 1);
        list->ResourceBarrier(1, &lutUav);
        list->SetPipelineState(pipelines_[4].Get());
        list->Dispatch((UINT(backDesc.Width)+7)/8, (backDesc.Height+7)/8, 1);
        std::vector<D3D12_RESOURCE_BARRIER> end{
            Transition(backBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT),
            Transition(slot.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOutputState),
            Transition(histogram_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
            Transition(lut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)};
        if (hudNeedsBarrier) end.push_back(Transition(hudless.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState_));
        list->ResourceBarrier(UINT(end.size()), end.data());
        tag = {};
        tag.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        tag.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        tag.resourceBase = base_; tag.resourceSize = size_;
        tag.pResource = slot.output.Get();
        tag.incomingState = kOutputState;
        if (FAILED(list->Close())) { broken_ = true; reason = "list_close"; return false; }
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(fence_.Get(), ++fenceValue_))) { broken_ = true; reason = "signal"; return false; }
        slot.fence = fenceValue_;
        ++submitted_;
        const uint64_t done = fence_->GetCompletedValue();
        std::erase_if(retired_, [&](const auto& entry) { return entry.first+8 <= done; });
        return true;
    }
    catch (...) { broken_ = true; reason = "exception"; return false; }
}
}
