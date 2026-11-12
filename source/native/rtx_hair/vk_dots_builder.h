#pragma once
// vkhair.3: Vulkan LSS acceleration-structure builds as DOTS triangle builds, the twin of dots_builder.h for
// VK_NV_ray_tracing_linear_swept_spheres.
//
// BuildSizes answers for the DOTS triangles that replace each LSS geometry (with ALLOW_DATA_ACCESS, which the
// rewritten shaders' position fetch needs), so the game sizes its BLAS and scratch for them. CmdBuild records,
// in the game's command buffer, the converter (vk_dots_converter.comp) into a vertex buffer of ours, then the
// triangle build into the game's own BLAS and scratch; top-level builds read a copy of their instances with
// culling off on every hair BLAS (vk_instance_patch.comp). Game buffers are read only through their device
// addresses. The converters take their parameters from a ring written by vkCmdUpdateBuffer and their slot from
// vkCmdDispatchBase's base workgroup z, so they bind no descriptor set and push no constant: the only state of
// the game's command buffer they change is the bound compute pipeline, which CmdBuild binds back.
//
// Every bottom-level size query and build of the device gets ALLOW_DATA_ACCESS (both see the same flags, so
// updates and compaction stay consistent): shaders that ask "is LSS" fetch whatever triangle was hit, and
// Vulkan defines position fetch only on BLASes built with it.
//
// Built against the Vulkan 1.4 headers (VK_NO_PROTOTYPES); calls only the functions it is given.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rtx_hair::vk {
// The device functions the builder calls: the driver's own (never a wrapped entry of this module).
struct DeviceFunctions
{
    PFN_vkCreateBuffer createBuffer{};
    PFN_vkDestroyBuffer destroyBuffer{};
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements{};
    PFN_vkAllocateMemory allocateMemory{};
    PFN_vkFreeMemory freeMemory{};
    PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkGetBufferDeviceAddress getBufferDeviceAddress{};
    PFN_vkCreateShaderModule createShaderModule{};
    PFN_vkDestroyShaderModule destroyShaderModule{};
    PFN_vkCreatePipelineLayout createPipelineLayout{};
    PFN_vkDestroyPipelineLayout destroyPipelineLayout{};
    PFN_vkCreateComputePipelines createComputePipelines{};
    PFN_vkDestroyPipeline destroyPipeline{};
    PFN_vkCmdBindPipeline cmdBindPipeline{};
    PFN_vkCmdDispatchBase cmdDispatchBase{};
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer{};
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier{};
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes{};
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureAddress{};
};
// Through `gdpa` (the device's own vkGetDeviceProcAddr); false if any is missing.
bool LoadDeviceFunctions(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, DeviceFunctions& out);

struct DotsStats
{
    uint64_t sizeQueries{}, builds{}, updates{}, rebuilds{}, geometries{}, primitives{}, invalidGeometries{}, indirectBuilds{}, failures{};
    // hairmem.1: every byte figure is what is held now.
    uint64_t topLevelPatched{}, topLevelCopyBytes{}, hairBlas{}, poolBuffers{}, poolBytes{}, evictions{};
    uint64_t instanceCopyBuffers{}, spareBuffers{}, spareBytes{}, retiredBuffers{}, retiredBytes{};
    uint64_t allocations{}, reuses{}, hairDestroyed{};
    uint64_t gameHairBlasBytes{}, gameBlasBytes{}, gameBlasCount{}; // the game's own bottom levels (vkCreateAccelerationStructureKHR sizes)
};

// hairmem.1: [RtxHair] switches of RTXMFG.RtxHair.ini that change the conversion (read once, before Initialize).
struct DotsOptions
{
    uint32_t ribbons = 3;    // Ribbons=2: two ribbons 90 degrees apart (4 triangles per segment) instead of three (6)
    bool fastBuild = false;  // FastBuild=1: hair BLASes prefer fast build over fast trace
};
// The ribbon half-width in LSS radii that makes the strand's width averaged over view angles its diameter:
// pi/3 for three ribbons (lss_ir.h kRibbon), pi/(2 sqrt 2) for two (between 0.785 and 1.111 of it at any angle).
inline float RibbonScale(uint32_t ribbons)
{
    return ribbons == 2 ? 1.11072073454f : 1.04719755120f;
}

class DotsBuilderVk
{
public:
    ~DotsBuilderVk();
    // `memory`: the physical device's memory properties.
    bool Initialize(VkDevice device, const DeviceFunctions& functions, const VkPhysicalDeviceMemoryProperties& memory, std::string& error);
    // Frees everything; the device must be idle (vkDestroyDevice).
    void Shutdown();

    static bool HasLss(const VkAccelerationStructureBuildGeometryInfoKHR& info);

    // vkGetAccelerationStructureBuildSizesKHR through `original`, with LSS geometries replaced by DOTS triangles and
    // ALLOW_DATA_ACCESS on every bottom-level query.
    void BuildSizes(PFN_vkGetAccelerationStructureBuildSizesKHR original, VkAccelerationStructureBuildTypeKHR type,
        const VkAccelerationStructureBuildGeometryInfoKHR* info, const uint32_t* maxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* sizes);
    // vkCmdBuildAccelerationStructuresKHR through `original`. `compute`: the compute pipeline bound on `list` before
    // this call (VK_NULL_HANDLE when none), bound again after the converter dispatches.
    void CmdBuild(PFN_vkCmdBuildAccelerationStructuresKHR original, VkCommandBuffer list, uint32_t count,
        const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, VkPipeline compute);
    // vkCmdBuildAccelerationStructuresIndirectKHR through `original`: the primitive counts are on the GPU, so an LSS
    // geometry is built (directly) from `maxPrimitiveCounts` inactive DOTS triangles, valid and never hit; every
    // other build stays indirect.
    void CmdBuildIndirect(PFN_vkCmdBuildAccelerationStructuresIndirectKHR original, VkCommandBuffer list, uint32_t count,
        const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkDeviceAddress* indirect, const uint32_t* strides,
        const uint32_t* const* maxPrimitiveCounts, VkPipeline compute);
    // vkCreateAccelerationStructureKHR (succeeded): the game's size for it, so the summary can say what its hair
    // BLASes and all its bottom levels hold.
    void Created(VkAccelerationStructureKHR structure, VkAccelerationStructureTypeKHR type, VkDeviceSize size);
    // vkDestroyAccelerationStructureKHR: it stops being a hair BLAS. Its vertex buffer becomes a spare for the next
    // hair BLAS it fits; its instance copy buffer is retired.
    void Destroyed(VkAccelerationStructureKHR structure);
    // vkCmdCopyAccelerationStructureKHR (compaction, clones): a copy of a hair BLAS is a hair BLAS.
    void Copied(VkAccelerationStructureKHR source, VkAccelerationStructureKHR destination);

    // As dots_builder.h's SetRebuildPolicy.
    void SetRebuildPolicy(uint32_t refits, uint32_t maxPrimitives, uint64_t spacingMs);
    DotsStats Stats() const;
    std::string LastError() const;
    // The first 32 buffer allocations, growths, reuses and idle releases, then one summary line every 30 s while
    // hair is built. Null keeps it silent.
    using LogSink = void (*)(const std::string&) noexcept;
    void SetLog(LogSink sink) noexcept { log_ = sink; }
    // Before Initialize.
    void SetOptions(const DotsOptions& options) noexcept;
    const DotsOptions& Options() const noexcept { return options_; }
    // The summary line's VRAM figures (VK_EXT_memory_budget); without them it leaves VRAM out.
    void SetBudgetQuery(PFN_vkGetPhysicalDeviceMemoryProperties2 query, VkPhysicalDevice physical) noexcept;
    // Tests: the millisecond clock (GetTickCount64 by default).
    using Clock = uint64_t (*)() noexcept;
    void SetClock(Clock clock) noexcept { clock_ = clock; }
    // Tests: the instance copy the latest top-level build of `structure` read (VK_NULL_HANDLE if none).
    VkBuffer InstanceCopy(VkAccelerationStructureKHR structure, VkDeviceSize& bytes) const;

private:
    struct Allocation
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceAddress address{};
        VkDeviceSize bytes{};
    };
    struct Pool
    {
        Allocation allocation;
        uint64_t lastUse{};
        uint64_t firstUse{};
        uint64_t builds{};
        uint32_t refits{};
        VkDeviceAddress fullScratch{};
    };
    struct Retired
    {
        Allocation allocation;
        uint64_t tick{};
    };
    // The summary's window: counters at its start, and what was destroyed in it.
    struct Window
    {
        uint64_t start{}, builds{}, updates{}, primitives{}, allocations{}, reuses{};
        uint64_t destroyed{}, destroyedPools{}, destroyedAgeMs{}, destroyedBuilds{}, reusesSame{};
        uint64_t sceneBuilds{}, longestGapUs{}, hitches{}; // top-level builds and the gaps between them
    };
    struct Dispatch
    {
        bool patch{};      // instance patch, else converter
        uint32_t slot{};
        uint32_t groups{};
        uint8_t params[96]{};
        uint32_t bytes{};
    };

    bool Allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, Allocation& out);
    void Free(Allocation& allocation);
    // The pool of `structure` (vertex or instance copy buffer) with at least `bytes`; null on failure. Locked.
    Pool* PoolFor(std::map<VkAccelerationStructureKHR, Pool>& pools, VkAccelerationStructureKHR structure, VkDeviceSize bytes, uint64_t now);
    // The smallest spare of at least `bytes` and at most twice that, destroyed at least kSpareSettleMilliseconds ago;
    // taken off the list. Locked.
    bool TakeSpare(VkDeviceSize bytes, uint64_t now, Allocation& out);
    // Locked. True when a summary line is due (written by Summary, after the lock is released).
    bool Housekeep(uint64_t now);
    std::string SummaryLine(uint64_t now); // locked
    void Summary(std::string line);        // not locked: adds VRAM and logs
    uint64_t Now() const noexcept { return clock_(); }
    void Note(uint32_t& budget, const std::string& text);                // a pool event line, within its budget (locked)
    void NoteKind(const VkAccelerationStructureBuildGeometryInfoKHR& info); // a hair build's mode and flags (locked)
    void NoteScene();                                                      // a top-level build (locked)
    uint32_t NextSlot();
    // Records the parameter writes, the dispatches and their barriers, then binds `compute` back.
    void Record(VkCommandBuffer list, const std::vector<Dispatch>& dispatches, const std::vector<uint64_t>* table, VkPipeline compute);
    void Error(std::string text);

    VkDevice device_ = VK_NULL_HANDLE;
    DeviceFunctions f_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline converter_ = VK_NULL_HANDLE, patch_ = VK_NULL_HANDLE;
    Allocation ring_{}, table_{};
    std::atomic<uint32_t> nextSlot_{};
    mutable std::mutex lock_;
    std::map<VkAccelerationStructureKHR, Pool> vertices_; // converted vertices per destination BLAS
    std::map<VkAccelerationStructureKHR, Pool> copies_;   // patched instances per destination TLAS
    std::vector<Retired> retired_;                        // outgrown or idle buffers, freed once no build can read them
    std::vector<Retired> spares_;                         // vertex buffers of destroyed hair BLASes, for the next one
    std::map<VkAccelerationStructureKHR, VkDeviceAddress> hair_; // DOTS hair BLASes and their device addresses
    std::map<VkAccelerationStructureKHR, VkDeviceSize> sizes_;   // the game's bottom levels and their sizes
    mutable std::mutex errorLock_;
    std::string lastError_;
    std::atomic<uint64_t> sizeQueries_{}, builds_{}, updates_{}, rebuilds_{}, geometries_{}, primitives_{}, invalid_{}, indirect_{}, failures_{};
    std::atomic<uint64_t> topLevelPatched_{}, evictions_{}, allocations_{}, reuses_{}, hairDestroyed_{};
    uint32_t rebuildRefits_{300}, rebuildMaxPrimitives_{200000};
    DotsOptions options_{};
    uint32_t trianglesPerSegment_{6};
    LogSink log_{};
    Clock clock_{&TickCount};
    PFN_vkGetPhysicalDeviceMemoryProperties2 budgetQuery_{};
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t allocationLogs_{}, releaseLogs_{}, reuseLogs_{}; // under lock_
    uint64_t gameBlasBytes_{};                                // under lock_: sum of sizes_
    std::vector<uint32_t> buildKinds_;                        // under lock_: (mode << 16) | flags of hair builds seen (first 8)
    Window window_{};                                         // under lock_
    int64_t lastScene_{}, qpcFrequency_{};                    // under lock_: QPC of the latest top-level build
    uint64_t rebuildSpacingMs_{250}, lastRebuild_{};
    static uint64_t TickCount() noexcept;
};
}
