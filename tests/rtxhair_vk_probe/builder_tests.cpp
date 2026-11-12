// hairmem.1 (CPU, CTest rtxhair-vkbuilder): rtx_hair/vk_dots_builder through fake device functions and an injected
// clock. Games swap hair BLASes many times a second, so held memory must stay bounded. Covered: the held figures,
// spare reuse (200 ms settle, size fit, trim, cap), clocks read on two threads, LSS never reaching the driver when
// it cannot be converted, Ribbons=2, FastBuild=1, the game's BLAS sizes and the 30 s summary line.
#include "rtx_hair/vk_dots_builder.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
using rtx_hair::vk::DeviceFunctions;
using rtx_hair::vk::DotsBuilderVk;
using rtx_hair::vk::DotsOptions;

unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& text)
{
    ++checks;
    failures += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
}

// The fake device.
uint64_t gNow = 1'000'000;
uint64_t Clock() noexcept { return gNow; }
uint64_t gNextHandle = 0x1000;
std::map<uint64_t, VkDeviceSize> gBuffers; // live buffers and their sizes
uint64_t gBufferCreates = 0, gBufferDestroys = 0, gMemoryAllocations = 0, gMemoryFrees = 0;
bool gFailAllocations = false;
std::vector<std::string> gLog;

struct RecordedBuild
{
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
};
std::vector<RecordedBuild> gBuilds;
std::vector<uint32_t> gSizeCounts;
VkBuildAccelerationStructureFlagsKHR gSizeFlags{};

template <class T> T Handle() { return reinterpret_cast<T>(static_cast<uintptr_t>(gNextHandle++)); }

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer)
{
    *buffer = Handle<VkBuffer>();
    gBuffers[reinterpret_cast<uintptr_t>(*buffer)] = info->size;
    ++gBufferCreates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*)
{
    gBuffers.erase(reinterpret_cast<uintptr_t>(buffer));
    ++gBufferDestroys;
}
VKAPI_ATTR void VKAPI_CALL Requirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* r)
{
    r->size = gBuffers[reinterpret_cast<uintptr_t>(buffer)];
    r->alignment = 256;
    r->memoryTypeBits = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory)
{
    if (gFailAllocations) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *memory = Handle<VkDeviceMemory>();
    ++gMemoryAllocations;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*)
{
    if (memory) ++gMemoryFrees;
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkDeviceAddress VKAPI_CALL BufferAddress(VkDevice, const VkBufferDeviceAddressInfo* info)
{
    return static_cast<VkDeviceAddress>(reinterpret_cast<uintptr_t>(info->buffer)) << 24;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* m)
{
    *m = Handle<VkShaderModule>();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* l)
{
    *l = Handle<VkPipelineLayout>();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo*,
    const VkAllocationCallbacks*, VkPipeline* pipelines)
{
    for (uint32_t i = 0; i < count; ++i) pipelines[i] = Handle<VkPipeline>();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL BindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL DispatchBase(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL UpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
    uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
{
}
VKAPI_ATTR void VKAPI_CALL BuildSizes(VkDevice, VkAccelerationStructureBuildTypeKHR, const VkAccelerationStructureBuildGeometryInfoKHR* info,
    const uint32_t* counts, VkAccelerationStructureBuildSizesInfoKHR* sizes)
{
    gSizeCounts.assign(counts, counts + info->geometryCount);
    gSizeFlags = info->flags;
    sizes->accelerationStructureSize = 1;
}
VKAPI_ATTR void VKAPI_CALL CmdBuild(VkCommandBuffer, uint32_t count, const VkAccelerationStructureBuildGeometryInfoKHR* infos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ranges)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        RecordedBuild b;
        b.info = infos[i];
        for (uint32_t g = 0; g < infos[i].geometryCount; ++g)
        {
            b.geometries.push_back(infos[i].pGeometries ? infos[i].pGeometries[g] : *infos[i].ppGeometries[g]);
            b.ranges.push_back(ranges[i][g]);
        }
        gBuilds.push_back(b);
    }
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL StructureAddress(VkDevice, const VkAccelerationStructureDeviceAddressInfoKHR* info)
{
    return static_cast<VkDeviceAddress>(reinterpret_cast<uintptr_t>(info->accelerationStructure)) << 8;
}

DeviceFunctions Functions()
{
    DeviceFunctions f;
    f.createBuffer = CreateBuffer;
    f.destroyBuffer = DestroyBuffer;
    f.getBufferMemoryRequirements = Requirements;
    f.allocateMemory = AllocateMemory;
    f.freeMemory = FreeMemory;
    f.bindBufferMemory = BindBufferMemory;
    f.getBufferDeviceAddress = BufferAddress;
    f.createShaderModule = CreateShaderModule;
    f.destroyShaderModule = DestroyShaderModule;
    f.createPipelineLayout = CreatePipelineLayout;
    f.destroyPipelineLayout = DestroyPipelineLayout;
    f.createComputePipelines = CreateComputePipelines;
    f.destroyPipeline = DestroyPipeline;
    f.cmdBindPipeline = BindPipeline;
    f.cmdDispatchBase = DispatchBase;
    f.cmdUpdateBuffer = UpdateBuffer;
    f.cmdPipelineBarrier = Barrier;
    f.getBuildSizes = BuildSizes;
    f.cmdBuild = CmdBuild;
    f.getAccelerationStructureAddress = StructureAddress;
    return f;
}

void Log(const std::string& line) noexcept
{
    gLog.push_back(line);
}

// One builder on a fresh fake device.
struct Rig
{
    DotsBuilderVk builder;
    VkDevice device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0xD0));
    VkCommandBuffer list = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0xC0));
    explicit Rig(DotsOptions options = {})
    {
        gBuffers.clear();
        gBuilds.clear();
        gLog.clear();
        gFailAllocations = false;
        builder.SetClock(&Clock);
        builder.SetLog(&Log);
        builder.SetOptions(options);
        VkPhysicalDeviceMemoryProperties memory{};
        memory.memoryTypeCount = 1;
        memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        memory.memoryHeapCount = 1;
        memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        std::string error;
        if (!builder.Initialize(device, Functions(), memory, error)) std::printf("FAIL Initialize: %s\n", error.c_str());
    }
    ~Rig() { builder.Shutdown(); }
};

VkAccelerationStructureGeometryLinearSweptSpheresDataNV gLss{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_LINEAR_SWEPT_SPHERES_DATA_NV};

VkAccelerationStructureGeometryKHR LssGeometry()
{
    gLss.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    gLss.vertexData.deviceAddress = 0x10000000;
    gLss.vertexStride = 12;
    gLss.radiusFormat = VK_FORMAT_R32_SFLOAT;
    gLss.radiusData.deviceAddress = 0x20000000;
    gLss.radiusStride = 4;
    gLss.indexType = VK_INDEX_TYPE_NONE_KHR;
    gLss.indexingMode = VK_RAY_TRACING_LSS_INDEXING_MODE_LIST_NV;
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV;
    g.pNext = &gLss;
    return g;
}

VkAccelerationStructureKHR Structure(uint64_t value)
{
    return reinterpret_cast<VkAccelerationStructureKHR>(static_cast<uintptr_t>(value));
}

// A hair BLAS build of `segments` LSS segments into `dst`.
void HairBuild(Rig& rig, VkAccelerationStructureKHR dst, uint32_t segments,
    VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR)
{
    const VkAccelerationStructureGeometryKHR g = LssGeometry();
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = flags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.dstAccelerationStructure = dst;
    info.geometryCount = 1;
    info.pGeometries = &g;
    info.scratchData.deviceAddress = 0x30000000;
    const VkAccelerationStructureBuildRangeInfoKHR range{segments, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
    rig.builder.CmdBuild(CmdBuild, rig.list, 1, &info, ranges, VK_NULL_HANDLE);
}

// A scene build of `instances` instances into `dst`.
void SceneBuild(Rig& rig, VkAccelerationStructureKHR dst, uint32_t instances)
{
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = 0x40000000;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.dstAccelerationStructure = dst;
    info.geometryCount = 1;
    info.pGeometries = &g;
    const VkAccelerationStructureBuildRangeInfoKHR range{instances, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
    rig.builder.CmdBuild(CmdBuild, rig.list, 1, &info, ranges, VK_NULL_HANDLE);
}

bool NoLssReachedTheDriver()
{
    for (const auto& b : gBuilds)
        for (const auto& g : b.geometries)
            if (g.geometryType == VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV || g.geometryType == VK_GEOMETRY_TYPE_SPHERES_NV) return false;
    return true;
}

bool Logged(const char* text)
{
    for (const auto& line : gLog)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

constexpr VkDeviceSize kMiB = 1024 * 1024;
} // namespace

int main()
{
    {
        // The menu's counter: 600 scene builds hold one 64 KiB instance copy, not 600 of them.
        Rig rig;
        HairBuild(rig, Structure(0xA1), 1000);
        for (int frame = 0; frame < 600; ++frame)
        {
            gNow += 16;
            SceneBuild(rig, Structure(0x71), 500);
        }
        const auto s = rig.builder.Stats();
        Check(s.topLevelCopyBytes == 64 * 1024 && s.instanceCopyBuffers == 1, "600 scene builds: instance copies read the 64 KiB held, not a running total ("
            + std::to_string(s.topLevelCopyBytes) + ")");
        Check(s.topLevelPatched == 600, "every scene build was patched");
        Check(s.poolBuffers == 1 && s.poolBytes >= 1000ull * 216, "the vertex pool counts the hair BLAS's vertex buffer only");
    }
    {
        // Three ribbons: 6 triangles a segment, vertices in our buffer, no LSS at the driver.
        Rig rig;
        HairBuild(rig, Structure(0xA1), 1000);
        const auto& b = gBuilds.back();
        Check(b.geometries.size() == 1 && b.geometries[0].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR && b.ranges[0].primitiveCount == 6000,
            "1000 LSS segments become 6000 DOTS triangles");
        Check((b.info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR) != 0, "the hair BLAS allows data access");
        Check((b.info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) != 0, "without FastBuild the game's fast-trace preference stays");
    }
    {
        // A destroyed hair BLAS's buffer serves the next one, after it settles; same size again on the next swap.
        Rig rig;
        HairBuild(rig, Structure(0xA1), 100000);
        rig.builder.Destroyed(Structure(0xA1));
        auto s = rig.builder.Stats();
        Check(s.spareBuffers == 1 && s.poolBuffers == 0 && s.hairDestroyed == 1, "a destroyed hair BLAS's vertex buffer becomes a spare");
        gNow += 100;
        HairBuild(rig, Structure(0xA2), 100000);
        s = rig.builder.Stats();
        Check(s.allocations == 2 && s.reuses == 0, "a spare younger than 200 ms is not handed out");
        rig.builder.Destroyed(Structure(0xA2));
        gNow += 250; // the game's hair BLASes live 0.3-0.5 s: a spare must be ready inside that
        const uint64_t creates = gBufferCreates;
        HairBuild(rig, Structure(0xA3), 100000);
        s = rig.builder.Stats();
        Check(s.reuses == 1 && gBufferCreates == creates, "a spare 350 ms old of the right size is reused, nothing allocated");
        Check(s.spareBuffers == 1, "the other spare is still held (4 s not up)");
        rig.builder.Destroyed(Structure(0xA3));
        gNow += 1200;
        HairBuild(rig, Structure(0xA4), 40000); // smaller: a 100k-segment spare is more than twice it
        s = rig.builder.Stats();
        Check(s.reuses == 1, "a spare over twice the need is not used");
        gNow += 5000;
        HairBuild(rig, Structure(0xA4), 40000);
        s = rig.builder.Stats();
        Check(s.spareBuffers == 0 && s.spareBytes == 0, "spares not taken within 4 s are freed");
    }
    {
        // Spares never hold more than the live vertex buffers or 64 MiB.
        Rig rig;
        HairBuild(rig, Structure(0xB1), 400000); // ~82 MiB each
        HairBuild(rig, Structure(0xB2), 400000);
        HairBuild(rig, Structure(0xB3), 400000);
        rig.builder.Destroyed(Structure(0xB1));
        rig.builder.Destroyed(Structure(0xB2));
        gNow += 1100;
        SceneBuild(rig, Structure(0x71), 10); // any build housekeeps
        const auto s = rig.builder.Stats();
        Check(s.spareBuffers == 1 && s.spareBytes <= s.poolBytes, "two 82 MiB spares next to one live buffer: one is freed (cap = live bytes)");
    }
    {
        // A clock read on another thread a little earlier never ages a buffer by 2^64 ms.
        Rig rig;
        HairBuild(rig, Structure(0xC1), 1000);
        rig.builder.Destroyed(Structure(0xC1));
        HairBuild(rig, Structure(0xC2), 1000);
        gNow -= 50; // the next build's time is older than the latest use
        HairBuild(rig, Structure(0xC3), 1000);
        const auto s = rig.builder.Stats();
        Check(s.evictions == 0 && s.poolBuffers == 2, "an earlier clock evicts nothing just used");
        Check(s.spareBuffers == 1, "and frees no spare");
        gNow += 50;
    }
    {
        // No vertex buffer: an empty triangle build, never LSS at the driver.
        Rig rig;
        gFailAllocations = true;
        HairBuild(rig, Structure(0xD1), 1000);
        gFailAllocations = false;
        const auto& b = gBuilds.back();
        Check(NoLssReachedTheDriver(), "a hair build without a vertex buffer sends no LSS geometry to the driver");
        Check(b.geometries.size() == 1 && b.geometries[0].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR && b.ranges[0].primitiveCount == 0
                && b.info.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR && b.geometries[0].geometry.triangles.vertexData.deviceAddress != 0,
            "it is a full build of no triangles at a valid address");
        Check(rig.builder.Stats().failures == 1, "and counts as a failure");
    }
    {
        // Ribbons=2 and FastBuild=1.
        DotsOptions options;
        options.ribbons = 2;
        options.fastBuild = true;
        Rig rig(options);
        HairBuild(rig, Structure(0xE1), 1000);
        const auto& b = gBuilds.back();
        Check(b.ranges[0].primitiveCount == 4000, "Ribbons=2: 1000 segments become 4000 triangles");
        Check(rig.builder.Stats().poolBytes >= 1000ull * 144 && rig.builder.Stats().poolBytes < 1000ull * 216, "Ribbons=2: 144 bytes of vertices a segment");
        Check((b.info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR) && !(b.info.flags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR),
            "FastBuild=1: the hair BLAS prefers fast build");
        const VkAccelerationStructureGeometryKHR g = LssGeometry();
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.geometryCount = 1;
        info.pGeometries = &g;
        const uint32_t counts[] = {1000};
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rig.builder.BuildSizes(BuildSizes, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, counts, &sizes);
        Check(gSizeCounts.size() == 1 && gSizeCounts[0] == 4000 && (gSizeFlags & VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR),
            "its size query asks for the same 4000 triangles and flags as the build");
    }
    {
        // The game's own sizes, and the summary line.
        Rig rig;
        rig.builder.Created(Structure(0xF1), VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 3 * kMiB);
        rig.builder.Created(Structure(0xF2), VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 5 * kMiB);
        rig.builder.Created(Structure(0x72), VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 7 * kMiB);
        HairBuild(rig, Structure(0xF1), 1000);
        auto s = rig.builder.Stats();
        Check(s.gameBlasCount == 2 && s.gameBlasBytes == 8 * kMiB && s.gameHairBlasBytes == 3 * kMiB, "the game's bottom levels and its hair BLAS's size");
        Check(!Logged("RTX_HAIR Vulkan memory"), "no summary inside the first 30 s");
        for (int frame = 0; frame < 40; ++frame)
        {
            gNow += 1000;
            SceneBuild(rig, Structure(0x72), 10);
            HairBuild(rig, Structure(0xF1), 1000);
        }
        Check(Logged("RTX_HAIR Vulkan memory: hair BLASes 1 (the game's 3.0 MiB), game bottom levels 2 (8.0 MiB)"), "the 30 s summary line");
        Check(Logged("scene builds") && Logged("hair build kinds build/0x"), "with scene builds and the hair build kinds");
        rig.builder.Destroyed(Structure(0xF1));
        s = rig.builder.Stats();
        Check(s.gameBlasBytes == 5 * kMiB && s.gameHairBlasBytes == 0 && s.hairBlas == 0, "a destroyed hair BLAS leaves both totals");
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
