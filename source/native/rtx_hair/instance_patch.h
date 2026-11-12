#pragma once
// rtxhair.1: TRIANGLE_CULL_DISABLE on every instance of a DOTS hair BLAS, applied on the GPU
// to a top-level build's instance descs (instance_patch.hlsl). DOTS ribbons are single-
// sided; an LSS hit is never culled. The caller (witcher_dots/gpu_runtime, or the probe)
// supplies the output buffer, keeps it alive until the build ran, and hands the list's
// compute bindings back afterwards.
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rtx_hair {
class InstancePatcher
{
public:
    // `shader`: instance_patch.hlsl compiled for cs_6_0 or later (signed DXIL).
    bool Initialize(ID3D12Device5* device, std::span<const std::byte> shader, std::string& error);
    // A DOTS hair BLAS (its destination address). Up to 65536 are kept.
    void Add(D3D12_GPU_VIRTUAL_ADDRESS blas);
    size_t Size() const;
    // Records on `list` the copy of `count` instance descs from `input` (readable as a
    // non-pixel shader resource) to `output` (UNORDERED_ACCESS, 64 * count bytes) with every
    // hair instance's culling disabled. False, with nothing recorded, when no hair BLAS is
    // known or the address table cannot be uploaded.
    bool Record(ID3D12GraphicsCommandList4* list, D3D12_GPU_VIRTUAL_ADDRESS input, uint32_t count, D3D12_GPU_VIRTUAL_ADDRESS output);
    uint64_t Records() const { return records_.load(std::memory_order_relaxed); }

private:
    struct Retired
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t tick{};
    };
    Microsoft::WRL::ComPtr<ID3D12Device5> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    mutable std::mutex lock_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> hair_; // sorted
    bool dirty_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> table_; // upload heap: hair_ as uint64
    uint32_t tableCount_{};
    std::vector<Retired> retired_; // earlier tables, kept until no recorded copy can still read them
    std::atomic<uint64_t> records_{};
};
}
