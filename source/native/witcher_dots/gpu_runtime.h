#pragma once
#include "geometry.h"
#include "shaders.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
struct IDXGIAdapter3;
namespace witcher_dots {
// ABI declarations for the one verified game/NVAPI profile. This translation
// layer implements only array-layout, one opaque LSS geometry, implicit +1
// segment endpoints and the common DXR build/update flags.
struct AddressStride { uint64_t address{},stride{}; };
struct LssGeometry {
    uint32_t type{},flags{},vertexCount{},indexCount{},primitiveCount{},pad0{};
    AddressStride positions;
    uint32_t positionFormat{},pad1{};
    AddressStride radii;
    uint32_t radiusFormat{},pad2{};
    AddressStride indices;
    uint32_t indexFormat{},endcaps{},primitiveFormat{};
};
struct ExtendedInputs {
    uint32_t type{},flags{},count{},layout{},stride{},pad{};
    const void* geometry{};
};
struct PrebuildParams { uint32_t version{},pad{};const ExtendedInputs* inputs{};D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info{}; };
struct ExtendedBuild { uint64_t destination{};ExtendedInputs inputs;uint64_t source{},scratch{}; };
struct BuildParams { uint32_t version{},pad{};const ExtendedBuild* desc{};uint32_t postCount{},pad2{};const void* post{}; };
static_assert(sizeof(ExtendedInputs)==32&&sizeof(PrebuildParams)==24&&sizeof(ExtendedBuild)==56&&sizeof(BuildParams)==32);
static_assert(offsetof(LssGeometry,positions)==24&&offsetof(LssGeometry,radii)==48&&offsetof(LssGeometry,indices)==72);
struct HairInput {
    void* owner{};
    Microsoft::WRL::ComPtr<ID3D12Resource> positions,indices,blas,scratch;
    LssGeometry geometry{};
    Plan plan{};
    uint32_t segmentsPerStrand{};
};
struct RuntimeStats {
    uint64_t prebuilds{}, builds{}, updates{}, rejected{}, shaderLibraries{}, instanceCopies{}, geometryBytes{}, leaseReuses{};
    uint64_t lastBuildTick{}, lastHairTick{}; // GetTickCount64 of the latest conversion / admitted hair instance
    uint64_t evictions{};
    uint64_t poolAllocations{}, poolReleases{}, fullRebuilds{}, reclaims{};
    uint64_t poolGrowths{}; // converted-vertex buffers allocated past the pool budget (loads)
    uint64_t hairBlasBytes{}, hairScratchBytes{}; // the game's AS/scratch buffers of live converted hair
    uint64_t buildLockWaitTicks{}, buildLockHeldTicks{}; // QPC ticks: waiting for / holding the runtime lock in hair builds
    uint64_t releasedLists{}, releasedRoots{}; // destroyed by the game, then released from tracking
    uint32_t trackedLists{}, trackedRoots{};
    uint64_t tableChanges{}, tableFailures{}, tableChangeTicks{}; // list table changes at Reset, failed ones, QPC ticks spent
    uint32_t liveOwners{}, hairInstances{}; // live associations; hair instances in the latest admitting copy
    // Cost diagnostics. Instrumented list calls (per-thread counts reach these
    // every 1024 calls) and timed samples (one in 32): DOTS's own and the
    // runtime's QPC ticks, each capped at 20 us (listOutliers: samples above it,
    // usually a preempted thread). ExecuteCommandLists on tracked queues: calls,
    // lists, submissions carrying converted hair, DOTS's own ticks.
    uint64_t listCalls{}, listSamples{}, listOverheadTicks{}, listOriginalTicks{}, listOutliers{};
    uint64_t executeCalls{}, executeLists{}, executeHair{}, executeOverheadTicks{};
    double qpcCostTicks{}; // one QueryPerformanceCounter call; each list sample's overhead includes two
    bool lost{};
};
// Opt-in GPU crash diagnostics: also instrument Dispatch so a device-removal
// report names the hung dispatch (pipeline, thread groups, issuing code). Only
// before InitializeGpu; returns false afterwards.
bool EnableDispatchDiagnostics() noexcept;
// The game's Path Traced Hair setting changed (for the device-removal report).
void NoteSettingChange(bool on) noexcept;
bool InitializeGpu(ID3D12Device5* device,ShaderCache* shaders,std::string& error);
// Only for failed early preparation, before any hair gate has been accepted.
// Forwarding bindings and retained interfaces remain alive for racing callers.
bool AbortGpuPreparation() noexcept;
void StopConversions() noexcept;
// Offsets of the build's hair owner fields (scratch, BLAS, LSS positions,
// LSS indices); set once, before any hair hook is active.
void SetHairOwnerLayout(uint32_t scratch,uint32_t blas,uint32_t positions,uint32_t indices) noexcept;
// The prepared device's adapter: its OS video-memory budget decides whether the
// converted-vertex pool may grow past kGeometryBudget. Without it the budget is
// a hard limit.
void SetMemoryAdapter(IDXGIAdapter3* adapter) noexcept;
// Harness only: smaller pool limits, so pool pressure can be exercised.
void SetGeometryPoolLimits(uint64_t budget,uint64_t ceiling,uint64_t reserve) noexcept;
bool ReadHairInput(void* owner,const ExtendedInputs& inputs,HairInput& out,std::string& error);
bool PrebuildTriangles(const HairInput& hair,uint32_t flags,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info);
bool BuildTriangles(HairInput hair,ID3D12GraphicsCommandList4* list,const ExtendedBuild& desc,std::string& error);
// Modify a private copy of this game's CPU instance data. Suppress unowned
// hair entries, never mutate an unclassified AS or a non-hair instance. With
// hairTraced false (the game's Path Traced Hair is off) all hair is suppressed.
bool PrepareInstances(std::span<D3D12_RAYTRACING_INSTANCE_DESC> instances,bool hairTraced=true);
// After the game's own copy of `count` TLAS instances, rewrite only the hair
// entries (mask 0x80) in the destination, prepared as PrepareInstances does.
// The scan holds no lock; hair is prepared in small batches. `hair` returns the
// number of hair entries; ok is false if either array faulted during the scan.
struct InstancePatch { size_t hair{};bool ok{true}; };
InstancePatch PatchInstanceCopy(D3D12_RAYTRACING_INSTANCE_DESC* destination,const D3D12_RAYTRACING_INSTANCE_DESC* source,
    size_t count,bool hairTraced) noexcept;
// Releases destroyed hair at most every 250 ms, independent of builds and of
// hair appearing in any instance copy.
void SweepReleased() noexcept;
// The native D3D12 device behind any forwarding wrapper: public COM unwrapping
// (Streamline, ReShade 6.7+), else a natively created child's GetDevice
// (wrappers without a public unwrap, such as ReShade before 6.7).
bool ResolveNativeDevice(IUnknown* object,Microsoft::WRL::ComPtr<ID3D12Device5>& out) noexcept;
// For DLSS 5 (dlss5::SetQueueForward): the runtime method behind this queue's
// ExecuteCommandLists slot while that slot holds our hook; null otherwise.
void* QueueExecuteForward(ID3D12CommandQueue* queue) noexcept;
RuntimeStats ReadRuntimeStats();
// rtxhair.1 universal hair (rtx_hair/universal_hair.cpp): this runtime's device, queue,
// command-list and root-signature tracking without the Witcher hair profile, never in
// the same process as InitializeGpu. `rewrite` translates one DXIL library or compute
// shader (false: the game's is kept); it runs inside CreateStateObject,
// AddToStateObject and CreateComputePipelineState of the tracked device.
using ShaderRewrite=bool(*)(const void* code,size_t size,std::vector<std::byte>& output) noexcept;
bool InitializeTracking(ID3D12Device5* device,ShaderRewrite rewrite,std::string& error);
// Records `inject` on the native list behind `list` (the game's, possibly wrapped) with
// this runtime's own calls left out of tracking; `restore` re-binds the compute root
// signature, root arguments and pipeline the game had bound. False with `error`, and
// nothing recorded, when those bindings are not established (an untracked list, an
// unknown root signature).
using ComputeInjection=std::function<void(ID3D12GraphicsCommandList4* native,const std::function<void()>& restore)>;
bool InjectCompute(ID3D12GraphicsCommandList4* list,const ComputeInjection& inject,std::string& error);
// The tracked device, or a proven forwarding wrapper of it.
bool TrackedDevice(ID3D12Device* device) noexcept;
// The driver refused a state object built with rewritten libraries: true when the rewrite
// now produces something else (Witcher 3's look fixes switched off), so it is tried once
// more before the game's own libraries.
using RewriteRefused=bool(*)() noexcept;
void SetRewriteRefused(RewriteRefused refused) noexcept;
// Top-level builds (universal tracking only): `patch` records, on the native list, the copy
// of `count` instance descs from `input` to `output` (a buffer of this runtime, in
// UNORDERED_ACCESS) with its changes, or returns false having recorded nothing. Every
// top-level build through a tracked list's BuildRaytracingAccelerationStructure then reads
// the copy, which stays alive until that submission completes (the queue fences of the
// hair leases); the list's compute bindings are handed back. Null turns it off.
using TopLevelPatch=bool(*)(ID3D12GraphicsCommandList4* native,D3D12_GPU_VIRTUAL_ADDRESS input,uint32_t count,D3D12_GPU_VIRTUAL_ADDRESS output) noexcept;
void SetInstancePatch(TopLevelPatch patch) noexcept;
// The same for a top-level build the caller records itself (NVAPI BuildEx) on `list` (the
// game's, possibly wrapped): the address to build from, or 0 with `error` to build from
// `input` unchanged.
D3D12_GPU_VIRTUAL_ADDRESS PatchTopLevel(ID3D12GraphicsCommandList4* list,D3D12_GPU_VIRTUAL_ADDRESS input,uint32_t count,std::string& error) noexcept;
struct TopLevelStats { uint64_t patched{},unpatched{},leaseBytes{};uint32_t leases{}; };
TopLevelStats ReadTopLevelStats() noexcept;
// Device-removal report (DRED breadcrumbs/page fault plus DOTS state), armed
// only for the prepared DOTS device when the opt-in report is enabled.
bool ArmRemovalReport(ID3D12Device5* device,const std::wstring& path) noexcept;
}
