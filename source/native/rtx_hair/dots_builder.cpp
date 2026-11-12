#include "dots_builder.h"
#include "lss_ir.h" // kTrianglesPerSegment

#include <Windows.h>

#include <algorithm>
#include <cstring>

namespace rtx_hair {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t kVerticesPerPrimitive = kTrianglesPerSegment * 3; // three ribbons, one winding (dots_converter.hlsl)
constexpr uint64_t kBytesPerPrimitive = kVerticesPerPrimitive * 12; // float3 vertices
constexpr uint64_t kPoolGranularity = 64 * 1024;
constexpr uint64_t kRetireMilliseconds = 10'000;  // outgrown buffers outlive any build that can still read them
// A destination BLAS not built for this long gives its vertex buffer back: the converted
// vertices are read only by the build itself (the BLAS keeps its own copy), so the next
// build of it simply takes a new buffer.
constexpr uint64_t kIdleMilliseconds = 5'000;
constexpr UINT kInactiveVertices = 3 * 1024; // BuildInactive: at most this many NaN vertices per geometry

bool PositionFormat(DXGI_FORMAT format, uint32_t& code, uint32_t& bytes, uint32_t& align)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32_FLOAT: code = 0; bytes = 12; align = 4; return true;
    case DXGI_FORMAT_R32G32_FLOAT: code = 1; bytes = 8; align = 4; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: code = 2; bytes = 6; align = 2; return true;
    case DXGI_FORMAT_R16G16_FLOAT: code = 3; bytes = 4; align = 2; return true;
    case DXGI_FORMAT_R16G16B16A16_SNORM: code = 4; bytes = 6; align = 2; return true;
    case DXGI_FORMAT_R16G16_SNORM: code = 5; bytes = 4; align = 2; return true;
    default: return false;
    }
}

// The last byte a reader touches, relative to an aligned base, must stay in 32 bits.
bool Addressable(uint64_t offset, uint64_t count, uint64_t stride, uint64_t bytes)
{
    return count == 0 || offset + (count - 1) * stride + bytes + 4 < (1ull << 32);
}

// NVAPI asks for sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX) as the array stride; 0 is read as that.
size_t Stride(const nvapi::BuildInputsEx& inputs)
{
    return inputs.geometryDescStrideInBytes ? inputs.geometryDescStrideInBytes : nvapi::kGeometryDescExBytes;
}

const nvapi::GeometryDescEx* Geometry(const nvapi::BuildInputsEx& inputs, uint32_t i)
{
    if (inputs.descsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY)
        return reinterpret_cast<const nvapi::GeometryDescEx*>(reinterpret_cast<const uint8_t*>(inputs.pGeometryDescs) + i * Stride(inputs));
    return inputs.ppGeometryDescs[i];
}
} // namespace

LssLayout ReadLssLayout(const nvapi::LssDesc& d)
{
    LssLayout l;
    l.primitives = d.primitiveCount;
    auto& c = l.constants;
    c[0] = d.primitiveCount;
    c[1] = d.vertexCount;
    c[2] = d.indexCount;
    auto reject = [&](const char* why) {
        l.valid = false;
        l.reason = why;
        c[3] = 2; // every primitive inactive
        l.positions = l.radii = l.indices = 0;
        return l;
    };
    uint32_t format{}, bytes{}, align{};
    if (d.primitiveFormat != nvapi::kLssList && d.primitiveFormat != nvapi::kLssSuccessiveImplicit) return reject("unknown primitive format");
    if (!PositionFormat(d.vertexPositionFormat, format, bytes, align)) return reject("unsupported position format");
    const auto& positions = d.vertexPositionBuffer;
    if (!positions.StartAddress || positions.StrideInBytes < bytes || positions.StartAddress % align || positions.StrideInBytes % align
        || positions.StrideInBytes > 0xFFFFFFFFull)
        return reject("position buffer address or stride");
    l.positions = positions.StartAddress & ~3ull;
    c[4] = static_cast<uint32_t>(positions.StartAddress & 3);
    c[5] = static_cast<uint32_t>(positions.StrideInBytes);
    c[6] = format;
    if (!Addressable(c[4], d.vertexCount, c[5], bytes)) return reject("position buffer beyond 4 GB");
    const auto& radii = d.vertexRadiusBuffer;
    const bool half = d.vertexRadiusFormat == DXGI_FORMAT_R16_FLOAT;
    if (!half && d.vertexRadiusFormat != DXGI_FORMAT_R32_FLOAT) return reject("unsupported radius format");
    const uint32_t radiusBytes = half ? 2 : 4;
    if (!radii.StartAddress || radii.StartAddress % radiusBytes || radii.StrideInBytes % radiusBytes || radii.StrideInBytes > 0xFFFFFFFFull)
        return reject("radius buffer address or stride");
    l.radii = radii.StartAddress & ~3ull;
    c[8] = static_cast<uint32_t>(radii.StartAddress & 3);
    c[9] = static_cast<uint32_t>(radii.StrideInBytes);
    c[10] = half ? 1 : 0;
    if (!Addressable(c[8], d.vertexCount, c[9], radiusBytes)) return reject("radius buffer beyond 4 GB");
    const auto& indices = d.indexBuffer;
    const bool successive = d.primitiveFormat == nvapi::kLssSuccessiveImplicit;
    if (indices.StartAddress)
    {
        uint32_t code{}, size{};
        switch (d.indexFormat)
        {
        case DXGI_FORMAT_R32_UINT: code = 1; size = 4; break;
        case DXGI_FORMAT_R16_UINT: code = 2; size = 2; break;
        case DXGI_FORMAT_R8_UINT: code = 3; size = 1; break;
        default: return reject("unsupported index format");
        }
        const uint64_t stride = indices.StrideInBytes ? indices.StrideInBytes : size;
        if (indices.StartAddress % size || stride % size || stride > 0xFFFFFFFFull) return reject("index buffer address or stride");
        l.indices = indices.StartAddress & ~3ull;
        c[12] = static_cast<uint32_t>(indices.StartAddress & 3);
        c[13] = static_cast<uint32_t>(stride);
        c[14] = code;
        if (!Addressable(c[12], d.indexCount, stride, size)) return reject("index buffer beyond 4 GB");
        const uint64_t needed = successive ? d.primitiveCount : 2ull * d.primitiveCount;
        if (needed > d.indexCount) return reject("fewer indices than primitives need");
    }
    else
    {
        if (successive) return reject("successive implicit primitives without an index buffer");
        if (2ull * d.primitiveCount > d.vertexCount) return reject("fewer vertices than list primitives need");
    }
    if (d.vertexCount == 0 && d.primitiveCount) return reject("no vertices");
    c[3] = successive ? 1 : 0;
    l.valid = true;
    return l;
}

bool DotsBuilder::Initialize(ID3D12Device5* device, nvapi::PrebuildFn prebuild, nvapi::BuildFn build, std::span<const std::byte> converter,
    std::string& error)
{
    if (!device || !prebuild || !build || converter.empty())
    {
        error = "DOTS builder: missing device, NVAPI build functions or converter";
        return false;
    }
    device_ = device;
    prebuild_ = prebuild;
    build_ = build;
    // Root: [0] 16 constants b0, [1-3] SRV t0-t2, [4] UAV u0. No descriptor tables, so the
    // game's descriptor heaps are left alone.
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 16;
    for (UINT i = 1; i <= 3; ++i)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = i - 1;
    }
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 5;
    desc.pParameters = params;
    ComPtr<ID3DBlob> serialized, messages;
    // Resolved from the loaded d3d12.dll: the module that embeds this builder must not
    // import D3D12 (it loads into games of every API).
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize || FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_))))
    {
        error = "DOTS builder: converter root signature";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {converter.data(), converter.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_))))
    {
        error = "DOTS builder: converter pipeline";
        return false;
    }
    // BuildInactive's vertices: NaN in an upload heap (always readable by builds).
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<uint64_t>(kInactiveVertices) * 12;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped{};
    if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&inactive_)))
        || FAILED(inactive_->Map(0, nullptr, &mapped)) || !mapped)
    {
        error = "DOTS builder: inactive vertex buffer";
        return false;
    }
    const uint32_t nan = 0x7FC00000u;
    for (UINT i = 0; i < kInactiveVertices * 3; ++i) std::memcpy(static_cast<std::byte*>(mapped) + i * 4, &nan, 4);
    inactive_->Unmap(0, nullptr);
    return true;
}

bool DotsBuilder::HasLss(const nvapi::BuildInputsEx& inputs)
{
    if (inputs.type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL || !inputs.pGeometryDescs) return false;
    for (uint32_t i = 0; i < inputs.numDescs; ++i)
        if (const auto* g = Geometry(inputs, i); g && g->type == nvapi::kGeometryLss) return true;
    return false;
}

bool DotsBuilder::Translate(const nvapi::BuildInputsEx& inputs, D3D12_GPU_VIRTUAL_ADDRESS vertices, std::vector<nvapi::GeometryDescEx>& geometries,
    nvapi::BuildInputsEx& translated, std::vector<LssLayout>* layouts, uint64_t& vertexBytes)
{
    vertexBytes = 0;
    geometries.assign(inputs.numDescs, nvapi::GeometryDescEx{});
    const size_t copy = inputs.descsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
        ? std::min<size_t>(Stride(inputs), nvapi::kGeometryDescExBytes)
        : nvapi::kGeometryDescExBytes;
    if (inputs.descsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY && copy < 8 + sizeof(nvapi::LssDesc))
    {
        Error("geometry descriptor stride too small");
        return false;
    }
    for (uint32_t i = 0; i < inputs.numDescs; ++i)
    {
        const auto* source = Geometry(inputs, i);
        if (!source)
        {
            Error("null geometry descriptor");
            return false;
        }
        std::memcpy(&geometries[i], source, copy);
        auto& g = geometries[i];
        if (g.type != nvapi::kGeometryLss)
        {
            // NVAPI does not allow LSS mixed with other geometry types in one BLAS.
            Error("LSS mixed with other geometry types");
            return false;
        }
        const nvapi::LssDesc lss = g.lss;
        const LssLayout layout = ReadLssLayout(lss);
        if (layouts) layouts->push_back(layout);
        const uint64_t bytes = static_cast<uint64_t>(lss.primitiveCount) * kBytesPerPrimitive;
        if (static_cast<uint64_t>(lss.primitiveCount) * kVerticesPerPrimitive > 0xFFFFFFFFull)
        {
            Error("too many LSS primitives");
            return false;
        }
        std::memset(g.bytes, 0, sizeof(g.bytes));
        g.type = nvapi::kGeometryTriangles;
        g.triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.triangles.VertexCount = lss.primitiveCount * kVerticesPerPrimitive;
        g.triangles.VertexBuffer.StartAddress = vertices ? vertices + vertexBytes : 0;
        g.triangles.VertexBuffer.StrideInBytes = 12;
        g.triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        vertexBytes += (bytes + 255) & ~255ull;
    }
    translated = inputs;
    translated.flags |= nvapi::kBuildAllowDataAccess;
    translated.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    translated.geometryDescStrideInBytes = sizeof(nvapi::GeometryDescEx);
    translated.pGeometryDescs = geometries.data();
    return true;
}

nvapi::Status DotsBuilder::Prebuild(const nvapi::BuildInputsEx& inputs, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info)
{
    ++prebuilds_;
    std::vector<nvapi::GeometryDescEx> geometries;
    nvapi::BuildInputsEx translated{};
    uint64_t bytes{};
    if (!Translate(inputs, 0, geometries, translated, nullptr, bytes))
    {
        ++failures_;
        return nvapi::kInvalidArgument;
    }
    nvapi::PrebuildParams params{nvapi::kPrebuildParamsVersion, &translated, &info};
    const nvapi::Status status = prebuild_(device_.Get(), &params);
    if (status != nvapi::kOk)
    {
        ++failures_;
        Error("NVAPI prebuild of the DOTS triangles failed (" + std::to_string(status) + ")");
    }
    return status;
}

nvapi::Status DotsBuilder::Build(ID3D12GraphicsCommandList4* list, const nvapi::BuildParams& params,
    const std::function<void(ID3D12GraphicsCommandList4*)>& restore)
{
    if (!list || !params.pDesc || (params.version & 0xFFFF) < sizeof(nvapi::BuildParams))
    {
        ++failures_;
        Error("invalid build parameters");
        return nvapi::kInvalidArgument;
    }
    const nvapi::BuildDescEx& desc = *params.pDesc;
    const bool update = (desc.inputs.flags & nvapi::kBuildPerformUpdate) != 0;
    // Size the converted vertices first, then take this BLAS's vertex buffer.
    std::vector<nvapi::GeometryDescEx> geometries;
    nvapi::BuildInputsEx translated{};
    uint64_t bytes{};
    if (!Translate(desc.inputs, 0, geometries, translated, nullptr, bytes))
    {
        ++failures_;
        return nvapi::kInvalidArgument;
    }
    ComPtr<ID3D12Resource> buffer;
    bool rebuild = false;
    {
        std::lock_guard lock(lock_);
        const uint64_t now = GetTickCount64();
        if (update && inactiveBlas_.contains(desc.destAccelerationStructureData))
        {
            // Built by BuildInactive with fewer vertices: an update would not match it.
            ++skippedUpdates_;
            return nvapi::kOk;
        }
        if (!update) inactiveBlas_.erase(desc.destAccelerationStructureData);
        std::erase_if(retired_, [&](const Retired& r) { return now - r.tick > kRetireMilliseconds; });
        for (auto it = pools_.begin(); it != pools_.end();)
        {
            if (it->first != desc.destAccelerationStructureData && now - it->second.lastUse > kIdleMilliseconds)
            {
                retired_.push_back({it->second.buffer, now});
                it = pools_.erase(it);
                ++evictions_;
            }
            else ++it;
        }
        Pool& pool = pools_[desc.destAccelerationStructureData];
        pool.lastUse = now;
        if (!update)
        {
            pool.refits = 0;
            pool.fullScratch = desc.scratchAccelerationStructureData;
        }
        else if (++pool.refits >= rebuildRefits_ && pool.fullScratch && desc.scratchAccelerationStructureData == pool.fullScratch
            && now - lastRebuild_ >= rebuildSpacingMs_)
        {
            uint64_t primitives = 0;
            for (uint32_t i = 0; i < desc.inputs.numDescs; ++i)
                if (const auto* g = Geometry(desc.inputs, i)) primitives += g->lss.primitiveCount;
            if (primitives <= rebuildMaxPrimitives_)
            {
                rebuild = true;
                pool.refits = 0;
                lastRebuild_ = now;
            }
        }
        if (!pool.buffer || pool.bytes < bytes)
        {
            if (pool.buffer) retired_.push_back({pool.buffer, now});
            pool.bytes = std::max<uint64_t>((bytes + kPoolGranularity - 1) / kPoolGranularity * kPoolGranularity, kPoolGranularity);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resource.Width = pool.bytes;
            resource.Height = 1;
            resource.DepthOrArraySize = 1;
            resource.MipLevels = 1;
            resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            pool.buffer.Reset();
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(&pool.buffer))))
            {
                pools_.erase(desc.destAccelerationStructureData);
                ++failures_;
                lastError_ = "DOTS vertex buffer allocation failed";
                return nvapi::kError;
            }
        }
        buffer = pool.buffer;
    }
    std::vector<LssLayout> layouts;
    if (!Translate(desc.inputs, buffer->GetGPUVirtualAddress(), geometries, translated, &layouts, bytes))
    {
        ++failures_;
        return nvapi::kInvalidArgument;
    }
    // Convert: COMMON -> UAV, one dispatch per LSS geometry, UAV barrier, -> read by the build.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pipeline_.Get());
    const D3D12_GPU_VIRTUAL_ADDRESS base = buffer->GetGPUVirtualAddress();
    uint64_t offset = 0;
    for (auto& layout : layouts)
    {
        layout.constants[15] = static_cast<uint32_t>(offset);
        const D3D12_GPU_VIRTUAL_ADDRESS fallback = base; // unused or rejected inputs: any valid address
        list->SetComputeRoot32BitConstants(0, 16, layout.constants, 0);
        list->SetComputeRootShaderResourceView(1, layout.positions ? layout.positions : fallback);
        list->SetComputeRootShaderResourceView(2, layout.radii ? layout.radii : fallback);
        list->SetComputeRootShaderResourceView(3, layout.indices ? layout.indices : fallback);
        list->SetComputeRootUnorderedAccessView(4, base);
        if (layout.primitives) list->Dispatch((layout.primitives + 63) / 64, 1, 1);
        ++geometries_;
        primitives_ += layout.primitives;
        if (!layout.valid)
        {
            ++invalid_;
            Error(std::string("LSS geometry left without hair: ") + layout.reason);
        }
        offset += (static_cast<uint64_t>(layout.primitives) * kBytesPerPrimitive + 255) & ~255ull;
    }
    D3D12_RESOURCE_BARRIER after[2]{};
    after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    after[0].UAV.pResource = buffer.Get();
    after[1] = barrier;
    after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(2, after);
    if (restore) restore(list);
    // The game's build, now of triangles, into the game's BLAS and scratch.
    nvapi::BuildDescEx triangles = desc;
    triangles.inputs = translated;
    if (rebuild)
    {
        // A full build in place of the update: the scratch is the one this BLAS's last full
        // build used, so the game sized it for one (SetRebuildPolicy).
        triangles.inputs.flags &= ~nvapi::kBuildPerformUpdate;
        triangles.sourceAccelerationStructureData = 0;
    }
    nvapi::BuildParams forwarded = params;
    forwarded.version = nvapi::kBuildParamsVersion;
    forwarded.pDesc = &triangles;
    const nvapi::Status status = build_(list, &forwarded);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &barrier);
    if (status != nvapi::kOk)
    {
        ++failures_;
        Error("NVAPI build of the DOTS triangles failed (" + std::to_string(status) + ")");
        return status;
    }
    ++builds_;
    if (desc.inputs.flags & nvapi::kBuildPerformUpdate) ++updates_;
    if (rebuild) ++rebuilds_;
    return status;
}

nvapi::Status DotsBuilder::BuildInactive(ID3D12GraphicsCommandList4* list, const nvapi::BuildParams& params)
{
    if (!list || !params.pDesc || (params.version & 0xFFFF) < sizeof(nvapi::BuildParams) || !inactive_)
    {
        ++failures_;
        Error("invalid build parameters");
        return nvapi::kInvalidArgument;
    }
    const nvapi::BuildDescEx& desc = *params.pDesc;
    if (desc.inputs.flags & nvapi::kBuildPerformUpdate)
    {
        ++skippedUpdates_;
        return nvapi::kOk; // the BLAS keeps its last build
    }
    std::vector<nvapi::GeometryDescEx> geometries;
    nvapi::BuildInputsEx translated{};
    uint64_t bytes{};
    const D3D12_GPU_VIRTUAL_ADDRESS nan = inactive_->GetGPUVirtualAddress();
    if (!Translate(desc.inputs, nan, geometries, translated, nullptr, bytes))
    {
        ++failures_;
        return nvapi::kInvalidArgument;
    }
    for (auto& g : geometries)
    {
        g.triangles.VertexBuffer.StartAddress = nan;
        g.triangles.VertexCount = std::min<UINT>(g.triangles.VertexCount, kInactiveVertices);
    }
    {
        std::lock_guard lock(lock_);
        inactiveBlas_.insert(desc.destAccelerationStructureData);
    }
    nvapi::BuildDescEx triangles = desc;
    triangles.inputs = translated;
    nvapi::BuildParams forwarded = params;
    forwarded.version = nvapi::kBuildParamsVersion;
    forwarded.pDesc = &triangles;
    const nvapi::Status status = build_(list, &forwarded);
    if (status != nvapi::kOk)
    {
        ++failures_;
        Error("NVAPI build of inactive DOTS triangles failed (" + std::to_string(status) + ")");
        return status;
    }
    ++inactiveBuilds_;
    return status;
}

void DotsBuilder::SetRebuildPolicy(uint32_t refits, uint32_t maxPrimitives, uint64_t spacingMs)
{
    std::lock_guard lock(lock_);
    rebuildRefits_ = refits ? refits : 1;
    rebuildMaxPrimitives_ = maxPrimitives;
    rebuildSpacingMs_ = spacingMs;
}

BuilderStats DotsBuilder::Stats() const
{
    BuilderStats s;
    s.prebuilds = prebuilds_;
    s.builds = builds_;
    s.updates = updates_;
    s.geometries = geometries_;
    s.primitives = primitives_;
    s.invalidGeometries = invalid_;
    s.failures = failures_;
    s.inactiveBuilds = inactiveBuilds_;
    s.skippedUpdates = skippedUpdates_;
    s.evictions = evictions_;
    s.rebuilds = rebuilds_;
    std::lock_guard lock(lock_);
    s.poolBuffers = pools_.size();
    for (const auto& [address, pool] : pools_) s.poolBytes += pool.bytes;
    return s;
}

std::string DotsBuilder::LastError() const
{
    std::lock_guard lock(lock_);
    return lastError_;
}

void DotsBuilder::Error(std::string text)
{
    std::lock_guard lock(lock_);
    lastError_ = std::move(text);
}
} // namespace rtx_hair
