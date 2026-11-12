#include "motion_fill.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace xess_motion_fill
{
namespace
{
constexpr D3D12_RESOURCE_STATES kTextureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // the game's own motion tag state (192)
constexpr UINT kConstants = 24;

// Compiled at run time by the system compiler (no link dependency).
// Root: b0 constants, t0 depth (plane 0, R32 rows), t1 game motion (RGBA16F rows), u0 our motion.
constexpr char kShader[] = R"(
cbuffer Fill : register(b0)
{
    row_major float4x4 reprojection;
    uint width, height, motionPitch, depthPitch;
    uint depthWidth, depthHeight, mode, unused;
};
ByteAddressBuffer depthRows : register(t0);
ByteAddressBuffer motionRows : register(t1);
RWByteAddressBuffer filledRows : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const uint offset = id.y * motionPitch + id.x * 8;
    if (mode == 2) { filledRows.Store2(offset, uint2(0, 0)); return; }
    const uint2 raw = motionRows.Load2(offset);
    const float mx = f16tof32(raw.x), my = f16tof32(raw.x >> 16);
    if (abs(mx - 2.0) > 0.001 || abs(my - 2.0) > 0.001) { filledRows.Store2(offset, raw); return; }
    const uint dx = min(id.x * depthWidth / width, depthWidth - 1);
    const uint dy = min(id.y * depthHeight / height, depthHeight - 1);
    const float depth = asfloat(depthRows.Load(dy * depthPitch + dx * 4));
    const float2 ndc = float2((id.x + 0.5) / width * 2.0 - 1.0, 1.0 - (id.y + 0.5) / height * 2.0);
    const float4 previous = mul(float4(ndc, depth, 1.0), reprojection);
    float2 motion = float2(0.0, 0.0);
    if (previous.w > 1e-9)
    {
        const float2 p = previous.xy / previous.w;
        motion = float2(0.5 * (ndc.x - p.x), 0.5 * (p.y - ndc.y));
    }
    filledRows.Store2(offset, uint2(f32tof16(motion.x) | (f32tof16(motion.y) << 16), f32tof16(1.0) << 16));
}
)";

struct ShaderConstants
{
    float reprojection[16];
    uint32_t width, height, motionPitch, depthPitch;
    uint32_t depthWidth, depthHeight, mode, unused;
};
static_assert(sizeof(ShaderConstants) == kConstants*4);

void Multiply(const double (&a)[16], const double (&b)[16], double (&out)[16]) noexcept
{
    double r[16]{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i*4+j] += a[i*4+k]*b[k*4+j];
    std::memcpy(out, r, sizeof(r));
}

bool Buffer(ID3D12Device* device, uint64_t bytes, bool unordered, ComPtr<ID3D12Resource>& out) noexcept
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&out)));
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
}

const char* ModeName(Mode mode) noexcept
{
    switch (mode)
    {
    case Mode::eGame: return "game";
    case Mode::eFill: return "fill";
    case Mode::eZero: return "zero";
    }
    return "unknown";
}

bool Invert(const float (&m)[16], double (&out)[16]) noexcept
{
    double a[4][8]{};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j) a[i][j] = m[i*4+j];
        a[i][4+i] = 1.0;
    }
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c+1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (!(std::abs(a[pivot][c]) > 1e-12)) return false;
        if (pivot != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const double scale = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= scale;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f*a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i*4+j] = a[i][4+j];
    return true;
}

bool Reprojection(const float (&view)[16], const float (&projection)[16], const float (&prevView)[16],
    const float (&prevProjection)[16], float (&out)[16]) noexcept
{
    // Double precision: the world translations (~100 units in Witcher) cancel in inverse(V) * prevV.
    double inverseProjection[16], inverseView[16], previousView[16], previousProjection[16], m[16];
    if (!Invert(projection, inverseProjection) || !Invert(view, inverseView)) return false;
    for (int i = 0; i < 16; ++i) { previousView[i] = prevView[i]; previousProjection[i] = prevProjection[i]; }
    Multiply(inverseView, previousView, m);
    Multiply(m, previousProjection, m);
    Multiply(inverseProjection, m, m);
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(m[i])) return false;
        out[i] = float(m[i]);
    }
    return true;
}

bool UsableMatrices(const float (&view)[16], const float (&projection)[16]) noexcept
{
    const auto zero = [](float value) { return std::abs(value) < 1e-4f; };
    const bool rigid = std::abs(view[15]-1.0f) < 1e-4f && zero(view[3]) && zero(view[7]) && zero(view[11]);
    const bool perspective = zero(projection[15]) && std::abs(std::abs(projection[11])-1.0f) < 1e-3f;
    for (float value : view) if (!std::isfinite(value)) return false;
    for (float value : projection) if (!std::isfinite(value)) return false;
    return rigid && perspective;
}

Filler::Filler(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept : device_(device), queue_(queue) {}

Filler::~Filler()
{
    if (fence_ && fenceValue_ && fence_->GetCompletedValue() < fenceValue_ && event_
        && SUCCEEDED(fence_->SetEventOnCompletion(fenceValue_, event_)))
        WaitForSingleObject(event_, 2000);
    if (event_) CloseHandle(event_);
}

bool Filler::Pipeline(const char*& reason) noexcept
{
    if (pipeline_) return true;
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
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(compile(kShader, sizeof(kShader)-1, "rtxmfg_motion_fill", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) || !code) { reason = "shader_compile"; return false; }
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 0, kConstants};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[1].Descriptor = {0, 0};
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[2].Descriptor = {1, 0};
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[3].Descriptor = {0, 0};
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 4; desc.pParameters = parameters;
        ComPtr<ID3DBlob> blob, messages;
        if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages))
            || FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_))))
        { reason = "root_signature"; return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root_.Get();
        pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_)))) { reason = "pipeline_state"; return false; }
        if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || !event_)
        { pipeline_.Reset(); reason = "fence"; return false; }
        return true;
    }
    catch (...) { pipeline_.Reset(); reason = "pipeline_exception"; return false; }
}

bool Filler::Prepare(const D3D12_RESOURCE_DESC& motion, const D3D12_RESOURCE_DESC& depth, const char*& reason) noexcept
{
    const auto same = [](const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
        return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format; };
    if (prepared_ && same(motion, motionDesc_) && same(depth, depthDesc_)) return true;
    if (motion.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || motion.Format != DXGI_FORMAT_R16G16B16A16_FLOAT
        || motion.SampleDesc.Count != 1 || !motion.Width || !motion.Height || motion.Width > 8192 || motion.Height > 8192)
    { reason = "motion_shape"; return false; }
    if (depth.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || depth.SampleDesc.Count != 1 || !depth.Width || !depth.Height
        || (depth.Format != DXGI_FORMAT_R32G8X24_TYPELESS && depth.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT
            && depth.Format != DXGI_FORMAT_R32_TYPELESS && depth.Format != DXGI_FORMAT_D32_FLOAT
            && depth.Format != DXGI_FORMAT_R32_FLOAT))
    { reason = "depth_shape"; return false; }
    if (!Pipeline(reason)) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint{}, depthFootprint{};
    UINT64 motionBytes{}, depthBytes{};
    device_->GetCopyableFootprints(&motion, 0, 1, 0, &motionFootprint, nullptr, nullptr, &motionBytes);
    device_->GetCopyableFootprints(&depth, 0, 1, 0, &depthFootprint, nullptr, nullptr, &depthBytes);
    // Plane 0 of every accepted depth format is 32-bit rows.
    if (!motionBytes || !depthBytes || motionBytes > 512ull*1024*1024 || depthBytes > 512ull*1024*1024
        || motionFootprint.Footprint.RowPitch < motion.Width*8 || depthFootprint.Footprint.RowPitch < depth.Width*4)
    { reason = "footprint"; return false; }
    std::array<Slot, 4> fresh;
    D3D12_RESOURCE_DESC texture = motion;
    texture.Alignment = 0; texture.MipLevels = 1; texture.DepthOrArraySize = 1;
    texture.SampleDesc = {1, 0}; texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; texture.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (auto& slot : fresh)
    {
        if (!Buffer(device_.Get(), depthBytes, false, slot.depth) || !Buffer(device_.Get(), motionBytes, false, slot.motionIn)
            || !Buffer(device_.Get(), motionBytes, true, slot.motionOut)
            || FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, kTextureState, nullptr,
                IID_PPV_ARGS(&slot.texture)))
            || FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
            || FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                IID_PPV_ARGS(&slot.list)))
            || FAILED(slot.list->Close()))
        { reason = "allocation"; return false; }
    }
    if (prepared_) retired_.emplace_back(fenceValue_, std::move(slots_));
    slots_ = std::move(fresh);
    motionDesc_ = motion; depthDesc_ = depth;
    motionFootprint_ = motionFootprint; depthFootprint_ = depthFootprint;
    prepared_ = true; current_ = nullptr; pending_ = false; next_ = 0;
    return true;
}

bool Filler::WaitSlot(const Slot& slot, DWORD milliseconds) noexcept
{
    if (!slot.fence || fence_->GetCompletedValue() >= slot.fence) return true;
    if (FAILED(fence_->SetEventOnCompletion(slot.fence, event_))) return false;
    return WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0 || fence_->GetCompletedValue() >= slot.fence;
}

void Filler::Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES state,
    ID3D12Resource* buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) noexcept
{
    // Only the game resource's own declared state is touched, and put back.
    const bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto barrier = Transition(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (transition) list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = buffer; destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &from, nullptr);
    if (transition)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }
}

void Filler::OnConstants(uint32_t frame, const float (&view)[16], const float (&projection)[16]) noexcept
{
    auto& slot = constants_[frame%2];
    slot.frame = frame;
    std::memcpy(slot.view, view, sizeof(slot.view));
    std::memcpy(slot.projection, projection, sizeof(slot.projection));
    slot.valid = UsableMatrices(view, projection);
}

void Filler::OnDepth(ID3D12GraphicsCommandList* list, uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& depth,
    Mode mode) noexcept
{
    try
    {
        if (broken_ || mode != Mode::eFill || !list || !depth.pResource) return;
        const auto desc = depth.pResource->GetDesc();
        seenDepth_ = desc; // the motion tag (re)prepares with it
        if (!prepared_ || desc.Width != depthDesc_.Width || desc.Height != depthDesc_.Height || desc.Format != depthDesc_.Format)
            return;
        // A motion tag of the previous frame that never reached Submit keeps its slot.
        auto& slot = slots_[next_%slots_.size()];
        Copy(list, depth.pResource, depth.incomingState, slot.depth.Get(), depthFootprint_);
        slot.frame = frame; slot.depthCopied = true;
        current_ = &slot;
    }
    catch (...) {}
}

bool Filler::OnMotion(ID3D12GraphicsCommandList* list, uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& motion,
    Mode mode, const char*& reason) noexcept
{
    reason = nullptr;
    try
    {
        if (mode == Mode::eGame) return false;
        if (broken_) { reason = "disabled"; return false; }
        if (!list || !motion.pResource) { reason = "no_command_list"; return false; }
        if (pending_)
        {
            // The previous frame's motion was consumed but its Present never came through
            // PreparePresent: without that hook the game's own tags are the only safe route.
            broken_ = true; reason = "present_hook_missing"; return false;
        }
        const auto desc = motion.pResource->GetDesc();
        D3D12_RESOURCE_DESC depth = seenDepth_;
        if (!depth.Width)
        {
            if (mode == Mode::eFill) { reason = "no_depth_tag"; return false; }
            depth = desc; depth.Format = DXGI_FORMAT_R32_FLOAT; // zero motion reads no depth
        }
        if (!Prepare(desc, depth, reason))
        {
            if (reason && std::strcmp(reason, "motion_shape") && std::strcmp(reason, "depth_shape")) broken_ = true;
            return false;
        }
        Slot* slot = current_;
        if (mode == Mode::eFill)
        {
            if (!slot || slot->frame != frame || !slot->depthCopied) { reason = "no_depth_for_frame"; return false; }
            const auto& now = constants_[frame%2];
            const auto& before = constants_[(frame-1)%2];
            if (!now.valid || now.frame != frame) { reason = "no_usable_constants"; return false; }
            if (!before.valid || before.frame != frame-1) { reason = "no_previous_constants"; return false; }
            if (!Reprojection(now.view, now.projection, before.view, before.projection, slot->reprojection))
            { reason = "reprojection_singular"; return false; }
        }
        else
        {
            slot = &slots_[next_%slots_.size()];
            slot->depthCopied = false;
        }
        if (motion.resourceBase.x || motion.resourceBase.y || motion.resourceSize.x != desc.Width || motion.resourceSize.y != desc.Height)
        { reason = "motion_subregion"; return false; }
        Copy(list, motion.pResource, motion.incomingState, slot->motionIn.Get(), motionFootprint_);
        slot->frame = frame; slot->mode = mode; slot->base = motion.resourceBase; slot->size = motion.resourceSize;
        current_ = slot; pending_ = true; ++next_;
        return true;
    }
    catch (...) { reason = "exception"; return false; }
}

bool Filler::Submit(uint32_t& frame, xefg_swapchain_d3d12_resource_data_t& tag, const char*& reason) noexcept
{
    reason = nullptr;
    if (!pending_ || !current_) { reason = "nothing_pending"; return false; }
    pending_ = false;
    try
    {
        Slot& slot = *current_;
        current_ = nullptr;
        frame = slot.frame;
        if (!WaitSlot(slot, 250)) { broken_ = true; reason = "slot_busy"; return false; }
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), pipeline_.Get())))
        { broken_ = true; reason = "list_reset"; return false; }
        auto* list = slot.list.Get();
        D3D12_RESOURCE_BARRIER begin[3]{
            Transition(slot.depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(slot.motionIn.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(slot.motionOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(3, begin);
        ShaderConstants constants{};
        std::memcpy(constants.reprojection, slot.reprojection, sizeof(constants.reprojection));
        constants.width = UINT(motionDesc_.Width); constants.height = motionDesc_.Height;
        constants.motionPitch = motionFootprint_.Footprint.RowPitch; constants.depthPitch = depthFootprint_.Footprint.RowPitch;
        constants.depthWidth = UINT(depthDesc_.Width); constants.depthHeight = depthDesc_.Height;
        constants.mode = uint32_t(slot.mode);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRoot32BitConstants(0, kConstants, &constants, 0);
        list->SetComputeRootShaderResourceView(1, slot.depth->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2, slot.motionIn->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3, slot.motionOut->GetGPUVirtualAddress());
        list->Dispatch((constants.width+7)/8, (constants.height+7)/8, 1);
        D3D12_RESOURCE_BARRIER middle[2]{
            Transition(slot.motionOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(slot.texture.Get(), kTextureState, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, middle);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = slot.texture.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = slot.motionOut.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = motionFootprint_;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        auto end = Transition(slot.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kTextureState);
        list->ResourceBarrier(1, &end);
        tag = {};
        tag.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        tag.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        tag.resourceBase = slot.base; tag.resourceSize = slot.size;
        tag.pResource = slot.texture.Get();
        tag.incomingState = kTextureState;
        if (FAILED(list->Close())) { broken_ = true; reason = "list_close"; return false; }
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(fence_.Get(), ++fenceValue_))) { broken_ = true; reason = "signal"; return false; }
        slot.fence = fenceValue_;
        slot.depthCopied = false;
        ++submitted_;
        // Release resource sets replaced by a size change once the GPU is past them.
        const uint64_t done = fence_->GetCompletedValue();
        // XeFG reads our textures after the Present that follows; 8 more submits is well past that.
        std::erase_if(retired_, [&](const auto& entry) { return entry.first+8 <= done; });
        return true;
    }
    catch (...) { broken_ = true; reason = "exception"; return false; }
}
}
