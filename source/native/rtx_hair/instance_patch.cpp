#include "instance_patch.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>

namespace rtx_hair {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t kMaxHair = 65536;
constexpr uint64_t kRetireMilliseconds = 10'000; // a replaced table outlives any recorded copy that reads it
}

bool InstancePatcher::Initialize(ID3D12Device5* device, std::span<const std::byte> shader, std::string& error)
{
    if (!device || shader.empty())
    {
        error = "instance patch: missing device or shader";
        return false;
    }
    device_ = device;
    // Root: [0] 4 constants b0, [1] SRV t0 (game instances), [2] SRV t1 (hair table), [3] UAV u0.
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters = params;
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    ComPtr<ID3DBlob> serialized, messages;
    if (!serialize || FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root_))))
    {
        error = "instance patch: root signature";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {shader.data(), shader.size()};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_))))
    {
        error = "instance patch: pipeline";
        return false;
    }
    return true;
}

void InstancePatcher::Add(D3D12_GPU_VIRTUAL_ADDRESS blas)
{
    if (!blas) return;
    std::lock_guard lock(lock_);
    const auto at = std::lower_bound(hair_.begin(), hair_.end(), blas);
    if (at != hair_.end() && *at == blas) return;
    if (hair_.size() >= kMaxHair) return;
    hair_.insert(at, blas);
    dirty_ = true;
}

size_t InstancePatcher::Size() const
{
    std::lock_guard lock(lock_);
    return hair_.size();
}

bool InstancePatcher::Record(ID3D12GraphicsCommandList4* list, D3D12_GPU_VIRTUAL_ADDRESS input, uint32_t count, D3D12_GPU_VIRTUAL_ADDRESS output)
{
    if (!list || !input || !count || !output || !pipeline_) return false;
    D3D12_GPU_VIRTUAL_ADDRESS table{};
    uint32_t tableCount{};
    {
        std::lock_guard lock(lock_);
        if (hair_.empty()) return false;
        const uint64_t now = GetTickCount64();
        std::erase_if(retired_, [&](const Retired& r) { return now - r.tick > kRetireMilliseconds; });
        if (dirty_ || !table_)
        {
            // A new table for each change: copies already recorded keep reading the old one.
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resource.Width = std::max<uint64_t>(hair_.size() * sizeof(uint64_t), 256);
            resource.Height = 1;
            resource.DepthOrArraySize = 1;
            resource.MipLevels = 1;
            resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            void* mapped{};
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&buffer)))
                || FAILED(buffer->Map(0, nullptr, &mapped)) || !mapped)
                return false;
            std::memcpy(mapped, hair_.data(), hair_.size() * sizeof(uint64_t));
            buffer->Unmap(0, nullptr);
            if (table_) retired_.push_back({table_, now});
            table_ = buffer;
            tableCount_ = static_cast<uint32_t>(hair_.size());
            dirty_ = false;
        }
        table = table_->GetGPUVirtualAddress();
        tableCount = tableCount_;
    }
    const uint32_t constants[4] = {count, tableCount, static_cast<uint32_t>(input & 3), 0};
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pipeline_.Get());
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    list->SetComputeRootShaderResourceView(1, input & ~D3D12_GPU_VIRTUAL_ADDRESS(3));
    list->SetComputeRootShaderResourceView(2, table);
    list->SetComputeRootUnorderedAccessView(3, output);
    list->Dispatch((count + 63) / 64, 1, 1);
    records_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
} // namespace rtx_hair
