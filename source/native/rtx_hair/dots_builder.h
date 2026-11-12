#pragma once
// rtxhair.1: NVAPI LSS acceleration-structure builds as DOTS triangle builds.
//
// Prebuild answers for the DOTS triangles that replace the LSS geometries (with
// ALLOW_DATA_ACCESS_EX, which the rewritten shaders' position fetch needs), so the game
// sizes its BLAS and scratch for them. Build records, on the game's command list, the
// converter (dots_converter.hlsl) into a vertex buffer of ours, then the triangle build
// into the game's own BLAS and scratch. Game buffers are read only through their GPU
// addresses; nothing else of the game is touched except the compute bindings, which the
// caller restores through `restore`.
#include "nvapi_lss.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace rtx_hair {
// One LSS geometry checked and reduced to the converter's constants (dots_converter.hlsl).
struct LssLayout
{
    uint32_t constants[16]{};
    D3D12_GPU_VIRTUAL_ADDRESS positions{}, radii{}, indices{}; // 4-byte aligned root SRV addresses (0: unused)
    uint32_t primitives{};
    bool valid{};          // false: every primitive becomes an inactive (NaN) triangle set
    const char* reason{};  // why it is not valid
};
LssLayout ReadLssLayout(const nvapi::LssDesc& desc);

struct BuilderStats
{
    uint64_t prebuilds{}, builds{}, updates{}, geometries{}, primitives{}, invalidGeometries{}, failures{};
    uint64_t inactiveBuilds{}, skippedUpdates{}, evictions{}, rebuilds{};
    uint64_t poolBuffers{}, poolBytes{};
};

class DotsBuilder
{
public:
    // `converter`: dots_converter.hlsl compiled for cs_6_0 or later (signed DXIL).
    bool Initialize(ID3D12Device5* device, nvapi::PrebuildFn prebuild, nvapi::BuildFn build, std::span<const std::byte> converter,
        std::string& error);
    static bool HasLss(const nvapi::BuildInputsEx& inputs);
    nvapi::Status Prebuild(const nvapi::BuildInputsEx& inputs, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info);
    // `restore` re-binds the caller's compute root signature, root arguments and pipeline
    // on `list`; it runs once after the converter dispatches, before the triangle build.
    nvapi::Status Build(ID3D12GraphicsCommandList4* list, const nvapi::BuildParams& params,
        const std::function<void(ID3D12GraphicsCommandList4*)>& restore);
    // Without the converter, for a list whose compute bindings cannot be restored: a new
    // BLAS is built from inactive (NaN) triangles, valid and never hit; an update leaves
    // the BLAS as it is. Updates of a BLAS built this way are skipped until its next full
    // build (their vertex count would not match). No compute state is touched.
    nvapi::Status BuildInactive(ID3D12GraphicsCommandList4* list, const nvapi::BuildParams& params);
    // Refits keep the first build's tree; strands that travel far (wind, riding) swell its
    // boxes and slow every ray through the hair. An update of a BLAS refitted `refits`
    // times, of at most `maxPrimitives` LSS primitives, that uses the scratch of its last
    // full build (so the game sized it for one) becomes a full build in place, at most one
    // per `spacingMs` (witcher_dots's policy: 300, 200k segments, 250 ms).
    void SetRebuildPolicy(uint32_t refits, uint32_t maxPrimitives, uint64_t spacingMs);
    BuilderStats Stats() const;
    std::string LastError() const;

private:
    struct Pool
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t bytes{}, lastUse{};
        uint32_t refits{};                              // updates since its last full build
        D3D12_GPU_VIRTUAL_ADDRESS fullScratch{};        // the scratch its last full build used
    };
    struct Retired
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t tick{};
    };
    // The inputs with every LSS geometry replaced by DOTS triangles at `vertices` (+ its
    // offset); `layouts` receives the LSS geometries in order.
    bool Translate(const nvapi::BuildInputsEx& inputs, D3D12_GPU_VIRTUAL_ADDRESS vertices, std::vector<nvapi::GeometryDescEx>& geometries,
        nvapi::BuildInputsEx& translated, std::vector<LssLayout>* layouts, uint64_t& vertexBytes);
    void Error(std::string text);

    Microsoft::WRL::ComPtr<ID3D12Device5> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> inactive_; // upload heap of NaN vertices (BuildInactive)
    nvapi::PrebuildFn prebuild_{};
    nvapi::BuildFn build_{};
    mutable std::mutex lock_;
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, Pool> pools_; // converted vertices per destination BLAS
    std::vector<Retired> retired_;                    // outgrown or idle buffers, released once no build can still read them
    std::set<D3D12_GPU_VIRTUAL_ADDRESS> inactiveBlas_; // destinations whose last full build was BuildInactive
    std::string lastError_;
    std::atomic<uint64_t> prebuilds_{}, builds_{}, updates_{}, geometries_{}, primitives_{}, invalid_{}, failures_{};
    std::atomic<uint64_t> inactiveBuilds_{}, skippedUpdates_{}, evictions_{}, rebuilds_{};
    uint32_t rebuildRefits_{300}, rebuildMaxPrimitives_{200000};
    uint64_t rebuildSpacingMs_{250}, lastRebuild_{};
};
}
