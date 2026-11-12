#include "rtx_hair/vk_dots_builder.h"

#include "rtxhair_vk_shaders.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rtx_hair::vk {
namespace {
constexpr VkDeviceSize kPoolGranularity = 64 * 1024;
constexpr uint64_t kRetireMilliseconds = 10'000; // outgrown buffers outlive any build that can still read them
// HF-03: a destination not built for this long gives its buffer back (long enough that hair leaving view
// briefly is not allocated again on the game's recording thread).
constexpr uint64_t kIdleMilliseconds = 30'000;
// hairmem.1: a destroyed hair BLAS's vertex buffer is a spare for the next hair BLAS it fits (Vulkan completes
// every command using a BLAS before it is destroyed, and only that BLAS's builds read the buffer). It settles
// 200 ms first, for a game that destroys a little early (hairmem.2: hair BLASes can live only 0.3-0.5 s); a spare
// not taken within 4 s is freed, and settled spares never hold more than the live vertex buffers (or 64 MiB).
constexpr uint64_t kSpareSettleMilliseconds = 200;
constexpr uint64_t kSpareIdleMilliseconds = 4'000;
constexpr VkDeviceSize kSpareFloorBytes = 64ull * 1024 * 1024;
constexpr uint64_t kSummaryMilliseconds = 30'000; // one memory line per this while hair is built
constexpr int64_t kHitchMicroseconds = 80'000;    // a gap between scene builds this long counts as a hitch
constexpr uint32_t kPoolLogLines = 32;           // allocation, release and reuse lines per device, then the summary only
constexpr uint32_t kBuildKinds = 8;              // distinct hair build mode / flag pairs the summary lists
constexpr uint32_t kSlots = 4096, kSlotBytes = 256; // the parameter ring (vk_common.glsl kSlotBytes)
constexpr uint32_t kTableEntries = 8192;          // hair BLAS addresses one top-level patch can see
constexpr uint32_t kInstanceBytes = 64;           // VkAccelerationStructureInstanceKHR
constexpr uint32_t kMaxGroups = 65535;            // the guaranteed maxComputeWorkGroupCount
constexpr double kMiB = 1024.0 * 1024.0;

// An age that a clock read on another thread (a little earlier or later) never turns into a huge one.
uint64_t Age(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0;
}

// vk_dots_converter.comp's Params and vk_instance_patch.comp's Params (std430; uvec2 addresses as uint64).
struct ConverterParams
{
    uint64_t positions, radii, indices, target;
    uint32_t primitiveCount, flags, firstVertex, pad0;
    uint32_t positionOffset, positionStride, positionFormat, pad1;
    uint32_t radiusOffset, radiusStride, radiusFormat, pad2;
    uint32_t indexOffset, indexStride, indexFormat, pad3;
};
static_assert(sizeof(ConverterParams) == 96);
struct PatchParams
{
    uint64_t source, target, table;
    uint32_t count, tableCount, arrayOfPointers, pad0, pad1, pad2;
};
static_assert(sizeof(PatchParams) == 48);

const VkAccelerationStructureGeometryKHR& GeometryAt(const VkAccelerationStructureBuildGeometryInfoKHR& info, uint32_t i)
{
    return info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
}

// Linear swept spheres, or spheres (never reported, so a game should not build them; they become inactive).
bool SweptOrSphere(VkGeometryTypeKHR type)
{
    return type == VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV || type == VK_GEOMETRY_TYPE_SPHERES_NV;
}

const VkAccelerationStructureGeometryLinearSweptSpheresDataNV* LssData(const VkAccelerationStructureGeometryKHR& g)
{
    for (auto* s = static_cast<const VkBaseInStructure*>(g.pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_LINEAR_SWEPT_SPHERES_DATA_NV)
            return reinterpret_cast<const VkAccelerationStructureGeometryLinearSweptSpheresDataNV*>(s);
    return nullptr;
}

bool PositionFormat(VkFormat format, uint32_t& code, uint32_t& bytes, uint32_t& align)
{
    switch (format)
    {
    case VK_FORMAT_R32G32B32_SFLOAT: code = 0; bytes = 12; align = 4; return true;
    case VK_FORMAT_R32G32_SFLOAT: code = 1; bytes = 8; align = 4; return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT: code = 2; bytes = 6; align = 2; return true;
    case VK_FORMAT_R16G16_SFLOAT: code = 3; bytes = 4; align = 2; return true;
    case VK_FORMAT_R16G16B16A16_SNORM: code = 4; bytes = 6; align = 2; return true;
    case VK_FORMAT_R16G16_SNORM: code = 5; bytes = 4; align = 2; return true;
    default: return false;
    }
}

// One LSS geometry and its build range, reduced to the converter's parameters (target set by the caller).
struct Layout
{
    ConverterParams params{};
    uint32_t primitives{};
    bool valid{};
    const char* reason{};
};

// VkAccelerationStructureBuildRangeInfoKHR for LSS: indexed, primitiveOffset is into the index data and firstVertex
// is added to the index values; not indexed, the vertex and radius data start at primitiveOffset + stride *
// firstVertex. LIST: two indices (or vertices) per primitive; SUCCESSIVE: primitive i is (k, k + 1) for its one
// index k (or k = i).
Layout ReadLayout(const VkAccelerationStructureGeometryKHR& g, const VkAccelerationStructureBuildRangeInfoKHR& range)
{
    Layout l;
    l.primitives = range.primitiveCount;
    auto& p = l.params;
    p.primitiveCount = range.primitiveCount;
    auto reject = [&](const char* why) {
        l.valid = false;
        l.reason = why;
        p.flags = 2; // every primitive inactive
        p.positions = p.radii = p.indices = 0;
        return l;
    };
    if (g.geometryType == VK_GEOMETRY_TYPE_SPHERES_NV) return reject("sphere geometry (not converted)");
    const auto* d = LssData(g);
    if (!d) return reject("LSS geometry without its data structure");
    const bool successive = d->indexingMode == VK_RAY_TRACING_LSS_INDEXING_MODE_SUCCESSIVE_NV;
    if (!successive && d->indexingMode != VK_RAY_TRACING_LSS_INDEXING_MODE_LIST_NV) return reject("unknown indexing mode");
    uint32_t format{}, bytes{}, align{};
    if (!PositionFormat(d->vertexFormat, format, bytes, align)) return reject("unsupported position format");
    const VkDeviceAddress positions = d->vertexData.deviceAddress;
    if (!positions || d->vertexStride < bytes || positions % align || d->vertexStride % align || d->vertexStride > 0xFFFFFFFFull)
        return reject("position address or stride");
    const bool half = d->radiusFormat == VK_FORMAT_R16_SFLOAT;
    if (!half && d->radiusFormat != VK_FORMAT_R32_SFLOAT) return reject("unsupported radius format");
    const uint32_t radiusBytes = half ? 2 : 4;
    const VkDeviceAddress radii = d->radiusData.deviceAddress;
    if (!radii || radii % radiusBytes || d->radiusStride % radiusBytes || d->radiusStride > 0xFFFFFFFFull) return reject("radius address or stride");
    uint64_t base = 0;
    if (d->indexType != VK_INDEX_TYPE_NONE_KHR && d->indexData.deviceAddress)
    {
        uint32_t code{}, size{};
        switch (d->indexType)
        {
        case VK_INDEX_TYPE_UINT32: code = 1; size = 4; break;
        case VK_INDEX_TYPE_UINT16: code = 2; size = 2; break;
        case VK_INDEX_TYPE_UINT8_KHR: code = 3; size = 1; break;
        default: return reject("unsupported index type");
        }
        const uint64_t stride = d->indexStride ? d->indexStride : size;
        const VkDeviceAddress indices = d->indexData.deviceAddress;
        if (indices % size || stride % size || stride > 0xFFFFFFFFull || range.primitiveOffset % size) return reject("index address, stride or offset");
        const uint64_t offset = (indices & 3) + range.primitiveOffset;
        if (offset > 0xFFFFFFFFull) return reject("index offset beyond 4 GB");
        p.indices = indices & ~3ull;
        p.indexOffset = static_cast<uint32_t>(offset);
        p.indexStride = static_cast<uint32_t>(stride);
        p.indexFormat = code;
    }
    else
    {
        if (range.primitiveOffset % align) return reject("vertex offset alignment");
        base = range.primitiveOffset;
    }
    const uint64_t positionOffset = (positions & 3) + base, radiusOffset = (radii & 3) + base;
    if (positionOffset > 0xFFFFFFFFull || radiusOffset > 0xFFFFFFFFull) return reject("vertex offset beyond 4 GB");
    p.positions = positions & ~3ull;
    p.positionOffset = static_cast<uint32_t>(positionOffset);
    p.positionStride = static_cast<uint32_t>(d->vertexStride);
    p.positionFormat = format;
    p.radii = radii & ~3ull;
    p.radiusOffset = static_cast<uint32_t>(radiusOffset);
    p.radiusStride = static_cast<uint32_t>(d->radiusStride);
    p.radiusFormat = half ? 1 : 0;
    p.firstVertex = range.firstVertex;
    p.flags = successive ? 1 : 0;
    l.valid = true;
    return l;
}

// A bottom-level build or size query with every LSS / sphere geometry replaced by DOTS triangles at `vertices`
// (+ a 256-byte aligned offset each; 0 for a size query) and ALLOW_DATA_ACCESS added.
struct Translation
{
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<VkDeviceSize> offsets;
    VkDeviceSize bytes{};
    bool lss{};
};

// `trianglesPerSegment`: 6 (three ribbons) or 4 (two). `fastBuild`: a bottom level with LSS prefers fast build
// over fast trace (size queries and builds both pass here, so an update's flags still match its source's).
bool TranslateBottom(const VkAccelerationStructureBuildGeometryInfoKHR& info, const uint32_t* primitives, VkDeviceAddress vertices,
    uint32_t trianglesPerSegment, bool fastBuild, Translation& t, std::string& error)
{
    const uint64_t verticesPerPrimitive = uint64_t(trianglesPerSegment) * 3, bytesPerPrimitive = verticesPerPrimitive * 12;
    t.info = info;
    t.info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR;
    t.geometries.resize(info.geometryCount);
    t.offsets.assign(info.geometryCount, 0);
    t.bytes = 0;
    t.lss = false;
    for (uint32_t i = 0; i < info.geometryCount; ++i)
    {
        const auto& g = GeometryAt(info, i);
        t.geometries[i] = g;
        if (!SweptOrSphere(g.geometryType)) continue;
        t.lss = true;
        const uint64_t count = primitives ? primitives[i] : 0;
        if (count * verticesPerPrimitive > 0xFFFFFFFFull)
        {
            error = "too many LSS primitives in one geometry";
            return false;
        }
        VkAccelerationStructureGeometryKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        triangles.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        triangles.flags = g.flags;
        auto& d = triangles.geometry.triangles;
        d.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        d.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        d.vertexData.deviceAddress = vertices ? vertices + t.bytes : 0;
        d.vertexStride = 12;
        d.maxVertex = count ? static_cast<uint32_t>(count * verticesPerPrimitive - 1) : 0;
        d.indexType = VK_INDEX_TYPE_NONE_KHR;
        t.offsets[i] = t.bytes;
        t.bytes += (count * bytesPerPrimitive + 255) & ~255ull;
        t.geometries[i] = triangles;
    }
    if (t.lss && fastBuild)
    {
        t.info.flags &= ~VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        t.info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    t.info.pGeometries = t.geometries.data();
    t.info.ppGeometries = nullptr;
    return true;
}

// hairmem.1: an LSS build that cannot be converted (a geometry too large to translate, or no vertex buffer) is
// built as a full build from no triangles, a valid empty BLAS the game's scene can still point at, instead of
// going to the driver as LSS, which this device does not have. `ranges` null: an indirect build (every geometry
// gets no primitives, as the indirect path's direct builds do). `vertices`: any valid buffer address.
void EmptyBuild(const VkAccelerationStructureBuildGeometryInfoKHR& info, const VkAccelerationStructureBuildRangeInfoKHR* ranges, VkDeviceAddress vertices,
    uint32_t trianglesPerSegment, bool fastBuild, Translation& t)
{
    std::vector<uint32_t> none(info.geometryCount, 0);
    std::string ignored;
    TranslateBottom(info, none.data(), vertices, trianglesPerSegment, fastBuild, t, ignored); // no primitives: cannot fail
    t.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    t.info.srcAccelerationStructure = VK_NULL_HANDLE;
    if (ranges)
        t.ranges.assign(ranges, ranges + info.geometryCount);
    else
        t.ranges.assign(info.geometryCount, VkAccelerationStructureBuildRangeInfoKHR{});
    for (uint32_t g = 0; g < info.geometryCount; ++g)
        if (SweptOrSphere(GeometryAt(info, g).geometryType)) t.ranges[g] = {};
}

std::string KiB(VkDeviceSize bytes)
{
    return std::to_string(bytes / 1024);
}

template <class T>
T Load(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name, const char* alias = nullptr)
{
    PFN_vkVoidFunction f = gdpa(device, name);
    if (!f && alias) f = gdpa(device, alias);
    return reinterpret_cast<T>(f);
}
} // namespace

bool LoadDeviceFunctions(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, DeviceFunctions& out)
{
    if (!device || !gdpa) return false;
    out.createBuffer = Load<PFN_vkCreateBuffer>(gdpa, device, "vkCreateBuffer");
    out.destroyBuffer = Load<PFN_vkDestroyBuffer>(gdpa, device, "vkDestroyBuffer");
    out.getBufferMemoryRequirements = Load<PFN_vkGetBufferMemoryRequirements>(gdpa, device, "vkGetBufferMemoryRequirements");
    out.allocateMemory = Load<PFN_vkAllocateMemory>(gdpa, device, "vkAllocateMemory");
    out.freeMemory = Load<PFN_vkFreeMemory>(gdpa, device, "vkFreeMemory");
    out.bindBufferMemory = Load<PFN_vkBindBufferMemory>(gdpa, device, "vkBindBufferMemory");
    out.getBufferDeviceAddress = Load<PFN_vkGetBufferDeviceAddress>(gdpa, device, "vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR");
    out.createShaderModule = Load<PFN_vkCreateShaderModule>(gdpa, device, "vkCreateShaderModule");
    out.destroyShaderModule = Load<PFN_vkDestroyShaderModule>(gdpa, device, "vkDestroyShaderModule");
    out.createPipelineLayout = Load<PFN_vkCreatePipelineLayout>(gdpa, device, "vkCreatePipelineLayout");
    out.destroyPipelineLayout = Load<PFN_vkDestroyPipelineLayout>(gdpa, device, "vkDestroyPipelineLayout");
    out.createComputePipelines = Load<PFN_vkCreateComputePipelines>(gdpa, device, "vkCreateComputePipelines");
    out.destroyPipeline = Load<PFN_vkDestroyPipeline>(gdpa, device, "vkDestroyPipeline");
    out.cmdBindPipeline = Load<PFN_vkCmdBindPipeline>(gdpa, device, "vkCmdBindPipeline");
    out.cmdDispatchBase = Load<PFN_vkCmdDispatchBase>(gdpa, device, "vkCmdDispatchBase", "vkCmdDispatchBaseKHR");
    out.cmdUpdateBuffer = Load<PFN_vkCmdUpdateBuffer>(gdpa, device, "vkCmdUpdateBuffer");
    out.cmdPipelineBarrier = Load<PFN_vkCmdPipelineBarrier>(gdpa, device, "vkCmdPipelineBarrier");
    out.getBuildSizes = Load<PFN_vkGetAccelerationStructureBuildSizesKHR>(gdpa, device, "vkGetAccelerationStructureBuildSizesKHR");
    out.cmdBuild = Load<PFN_vkCmdBuildAccelerationStructuresKHR>(gdpa, device, "vkCmdBuildAccelerationStructuresKHR");
    out.getAccelerationStructureAddress =
        Load<PFN_vkGetAccelerationStructureDeviceAddressKHR>(gdpa, device, "vkGetAccelerationStructureDeviceAddressKHR");
    const void* all[] = {reinterpret_cast<const void*>(out.createBuffer), reinterpret_cast<const void*>(out.destroyBuffer),
        reinterpret_cast<const void*>(out.getBufferMemoryRequirements), reinterpret_cast<const void*>(out.allocateMemory),
        reinterpret_cast<const void*>(out.freeMemory), reinterpret_cast<const void*>(out.bindBufferMemory),
        reinterpret_cast<const void*>(out.getBufferDeviceAddress), reinterpret_cast<const void*>(out.createShaderModule),
        reinterpret_cast<const void*>(out.destroyShaderModule), reinterpret_cast<const void*>(out.createPipelineLayout),
        reinterpret_cast<const void*>(out.destroyPipelineLayout), reinterpret_cast<const void*>(out.createComputePipelines),
        reinterpret_cast<const void*>(out.destroyPipeline), reinterpret_cast<const void*>(out.cmdBindPipeline),
        reinterpret_cast<const void*>(out.cmdDispatchBase), reinterpret_cast<const void*>(out.cmdUpdateBuffer),
        reinterpret_cast<const void*>(out.cmdPipelineBarrier), reinterpret_cast<const void*>(out.getBuildSizes),
        reinterpret_cast<const void*>(out.cmdBuild), reinterpret_cast<const void*>(out.getAccelerationStructureAddress)};
    for (const void* f : all)
        if (!f) return false;
    return true;
}

uint64_t DotsBuilderVk::TickCount() noexcept
{
    return GetTickCount64();
}

DotsBuilderVk::~DotsBuilderVk()
{
    // The owner calls Shutdown with the device idle; nothing is freed here behind its back.
}

void DotsBuilderVk::SetOptions(const DotsOptions& options) noexcept
{
    options_ = options;
    options_.ribbons = options.ribbons == 2 ? 2 : 3;
    trianglesPerSegment_ = options_.ribbons * 2;
}

void DotsBuilderVk::SetBudgetQuery(PFN_vkGetPhysicalDeviceMemoryProperties2 query, VkPhysicalDevice physical) noexcept
{
    budgetQuery_ = query;
    physical_ = physical;
}

bool DotsBuilderVk::Allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, Allocation& out)
{
    out = {};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (f_.createBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    f_.getBufferMemoryRequirements(device_, out.buffer, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t pass = 0; pass < 2 && type == UINT32_MAX; ++pass)
        for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i))
                && (pass == 1 || (memory_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            {
                type = i;
                break;
            }
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX || f_.allocateMemory(device_, &allocate, nullptr, &out.memory) != VK_SUCCESS
        || f_.bindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS)
    {
        Free(out);
        return false;
    }
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = out.buffer;
    out.address = f_.getBufferDeviceAddress(device_, &address);
    out.bytes = bytes;
    return out.address != 0;
}

void DotsBuilderVk::Free(Allocation& allocation)
{
    if (allocation.buffer) f_.destroyBuffer(device_, allocation.buffer, nullptr);
    if (allocation.memory) f_.freeMemory(device_, allocation.memory, nullptr);
    allocation = {};
}

bool DotsBuilderVk::Initialize(VkDevice device, const DeviceFunctions& functions, const VkPhysicalDeviceMemoryProperties& memory, std::string& error)
{
    device_ = device;
    f_ = functions;
    memory_ = memory;
    window_.start = Now();
    // The ring is also the vertex address of the fail-closed empty builds (EmptyBuild), hence build-input usage.
    if (!Allocate(VkDeviceSize(kSlots) * kSlotBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, ring_)
        || !Allocate(VkDeviceSize(kTableEntries) * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, table_))
    {
        error = "DOTS builder: parameter buffers could not be allocated";
        Shutdown();
        return false;
    }
    // No descriptor set, no push constant: the game's bindings stay as they are.
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (f_.createPipelineLayout(device_, &layout, nullptr, &layout_) != VK_SUCCESS)
    {
        error = "DOTS builder: pipeline layout refused";
        Shutdown();
        return false;
    }
    const uint32_t ring[2] = {static_cast<uint32_t>(ring_.address), static_cast<uint32_t>(ring_.address >> 32)};
    const VkSpecializationMapEntry entries[2] = {{0, 0, 4}, {1, 4, 4}};
    VkSpecializationInfo specialization{2, entries, sizeof(ring), ring};
    struct Program
    {
        const uint32_t* words;
        size_t bytes;
        VkPipeline* pipeline;
        const char* name;
    } programs[2] = {options_.ribbons == 2
                         ? Program{vk_shaders::kConverterTwoRibbons, sizeof(vk_shaders::kConverterTwoRibbons), &converter_, "two-ribbon converter"}
                         : Program{vk_shaders::kConverter, sizeof(vk_shaders::kConverter), &converter_, "converter"},
        {vk_shaders::kInstancePatch, sizeof(vk_shaders::kInstancePatch), &patch_, "instance patch"}};
    for (const auto& program : programs)
    {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = program.bytes;
        moduleInfo.pCode = program.words;
        VkShaderModule module = VK_NULL_HANDLE;
        if (f_.createShaderModule(device_, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        {
            error = std::string("DOTS builder: ") + program.name + " shader refused";
            Shutdown();
            return false;
        }
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = &specialization;
        info.layout = layout_;
        const VkResult r = f_.createComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, program.pipeline);
        f_.destroyShaderModule(device_, module, nullptr);
        if (r != VK_SUCCESS)
        {
            error = std::string("DOTS builder: ") + program.name + " pipeline refused (" + std::to_string(r) + ")";
            Shutdown();
            return false;
        }
    }
    return true;
}

void DotsBuilderVk::Shutdown()
{
    if (!device_) return;
    std::lock_guard lock(lock_);
    if (converter_) f_.destroyPipeline(device_, converter_, nullptr);
    if (patch_) f_.destroyPipeline(device_, patch_, nullptr);
    if (layout_) f_.destroyPipelineLayout(device_, layout_, nullptr);
    converter_ = patch_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    Free(ring_);
    Free(table_);
    for (auto* pools : {&vertices_, &copies_})
    {
        for (auto& [structure, pool] : *pools) Free(pool.allocation);
        pools->clear();
    }
    for (auto* list : {&retired_, &spares_})
    {
        for (auto& r : *list) Free(r.allocation);
        list->clear();
    }
    hair_.clear();
    sizes_.clear();
    gameBlasBytes_ = 0;
}

bool DotsBuilderVk::HasLss(const VkAccelerationStructureBuildGeometryInfoKHR& info)
{
    if (info.type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) return false;
    for (uint32_t i = 0; i < info.geometryCount; ++i)
        if (SweptOrSphere(GeometryAt(info, i).geometryType)) return true;
    return false;
}

void DotsBuilderVk::BuildSizes(PFN_vkGetAccelerationStructureBuildSizesKHR original, VkAccelerationStructureBuildTypeKHR type,
    const VkAccelerationStructureBuildGeometryInfoKHR* info, const uint32_t* maxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* sizes)
{
    ++sizeQueries_;
    if (!info || info->type != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR || (info->geometryCount && !maxPrimitiveCounts))
    {
        original(device_, type, info, maxPrimitiveCounts, sizes);
        return;
    }
    Translation t;
    std::string error;
    if (!TranslateBottom(*info, maxPrimitiveCounts, 0, trianglesPerSegment_, options_.fastBuild, t, error))
    {
        ++failures_;
        Error(error);
        original(device_, type, info, maxPrimitiveCounts, sizes);
        return;
    }
    std::vector<uint32_t> counts(maxPrimitiveCounts, maxPrimitiveCounts + info->geometryCount);
    for (uint32_t i = 0; i < info->geometryCount; ++i)
        if (SweptOrSphere(GeometryAt(*info, i).geometryType)) counts[i] *= trianglesPerSegment_;
    original(device_, type, &t.info, counts.data(), sizes);
}

uint32_t DotsBuilderVk::NextSlot()
{
    return nextSlot_.fetch_add(1, std::memory_order_relaxed) % kSlots;
}

void DotsBuilderVk::Note(uint32_t& budget, const std::string& text)
{
    if (!log_ || budget >= kPoolLogLines) return;
    ++budget;
    log_(text);
}

bool DotsBuilderVk::Housekeep(uint64_t now)
{
    for (auto it = retired_.begin(); it != retired_.end();)
    {
        if (Age(now, it->tick) > kRetireMilliseconds)
        {
            Free(it->allocation);
            it = retired_.erase(it);
        }
        else
            ++it;
    }
    for (auto it = spares_.begin(); it != spares_.end();)
    {
        if (Age(now, it->tick) > kSpareIdleMilliseconds)
        {
            Note(releaseLogs_, "RTX_HAIR Vulkan spare vertex buffer released after " + std::to_string(kSpareIdleMilliseconds / 1000)
                    + " s unused: " + KiB(it->allocation.bytes) + " KiB");
            Free(it->allocation);
            it = spares_.erase(it);
        }
        else
            ++it;
    }
    VkDeviceSize live = 0, spare = 0;
    for (const auto& [structure, pool] : vertices_) live += pool.allocation.bytes;
    for (const auto& r : spares_) spare += r.allocation.bytes;
    while (spare > std::max(live, kSpareFloorBytes))
    {
        auto oldest = spares_.end();
        for (auto it = spares_.begin(); it != spares_.end(); ++it)
            if (Age(now, it->tick) >= kSpareSettleMilliseconds && (oldest == spares_.end() || it->tick < oldest->tick)) oldest = it;
        if (oldest == spares_.end()) break;
        spare -= oldest->allocation.bytes;
        Free(oldest->allocation);
        spares_.erase(oldest);
    }
    for (auto* pools : {&vertices_, &copies_})
        for (auto it = pools->begin(); it != pools->end();)
        {
            if (Age(now, it->second.lastUse) > kIdleMilliseconds)
            {
                Note(releaseLogs_, std::string("RTX_HAIR Vulkan ") + (pools == &vertices_ ? "vertex" : "instance copy") + " buffer released after "
                        + std::to_string(kIdleMilliseconds / 1000) + " s unused: " + KiB(it->second.allocation.bytes) + " KiB");
                // Not built for that long, so no recorded build still reads it: a vertex buffer can serve the next
                // hair BLAS (after the spare's settle time).
                if (pools == &vertices_)
                    spares_.push_back({it->second.allocation, now});
                else
                    retired_.push_back({it->second.allocation, now});
                it = pools->erase(it);
                ++evictions_;
            }
            else
                ++it;
        }
    if (Age(now, window_.start) < kSummaryMilliseconds) return false;
    if (log_ && builds_.load(std::memory_order_relaxed) != window_.builds) return true;
    // No hair built in this window: start the next one quietly.
    window_ = {};
    window_.start = now;
    window_.builds = builds_;
    window_.updates = updates_;
    window_.primitives = primitives_;
    window_.allocations = allocations_;
    window_.reuses = reuses_;
    return false;
}

bool DotsBuilderVk::TakeSpare(VkDeviceSize bytes, uint64_t now, Allocation& out)
{
    const VkDeviceSize most = 2 * std::max<VkDeviceSize>(bytes, kPoolGranularity);
    auto best = spares_.end();
    for (auto it = spares_.begin(); it != spares_.end(); ++it)
        if (it->allocation.bytes >= bytes && it->allocation.bytes <= most && Age(now, it->tick) >= kSpareSettleMilliseconds
            && (best == spares_.end() || it->allocation.bytes < best->allocation.bytes))
            best = it;
    if (best == spares_.end()) return false;
    out = best->allocation;
    spares_.erase(best);
    return true;
}

DotsBuilderVk::Pool* DotsBuilderVk::PoolFor(std::map<VkAccelerationStructureKHR, Pool>& pools, VkAccelerationStructureKHR structure, VkDeviceSize bytes,
    uint64_t now)
{
    const bool vertex = &pools == &vertices_;
    Pool& pool = pools[structure];
    if (!pool.builds) pool.firstUse = now;
    ++pool.builds;
    pool.lastUse = now;
    if (!pool.allocation.buffer || pool.allocation.bytes < bytes)
    {
        const VkDeviceSize previous = pool.allocation.bytes;
        if (pool.allocation.buffer) retired_.push_back({pool.allocation, now});
        pool.allocation = {};
        // HF-03: half as much again, so hair whose strand count changes settles after a growth or two.
        const VkDeviceSize want = previous ? bytes + bytes / 2 : bytes;
        const VkDeviceSize size = std::max<VkDeviceSize>((want + kPoolGranularity - 1) / kPoolGranularity * kPoolGranularity, kPoolGranularity);
        if (vertex && TakeSpare(want, now, pool.allocation))
        {
            ++reuses_;
            if (pool.allocation.bytes == size) ++window_.reusesSame;
            Note(reuseLogs_, "RTX_HAIR Vulkan vertex buffer of a destroyed hair BLAS reused: " + KiB(pool.allocation.bytes) + " KiB (build needs "
                    + KiB(bytes) + " KiB)");
            return &pool;
        }
        LARGE_INTEGER start{}, end{}, frequency{};
        QueryPerformanceCounter(&start);
        const bool allocated = Allocate(size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            pool.allocation);
        QueryPerformanceCounter(&end);
        if (allocated) ++allocations_;
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
        {
            const auto micros = static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1'000'000 / frequency.QuadPart);
            Note(allocationLogs_, std::string("RTX_HAIR Vulkan ") + (vertex ? "vertex" : "instance copy") + " buffer "
                    + (previous ? "grew " + KiB(previous) + " -> " : "allocated ") + KiB(size) + " KiB (build needs " + KiB(bytes) + " KiB) in "
                    + std::to_string(micros) + " us" + (allocated ? "" : ", FAILED"));
        }
        if (!allocated)
        {
            pools.erase(structure);
            return nullptr;
        }
    }
    return &pool;
}

void DotsBuilderVk::Record(VkCommandBuffer list, const std::vector<Dispatch>& dispatches, const std::vector<uint64_t>* table, VkPipeline compute)
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    // Earlier converters, patches and builds (in submission order) are done with the ring, the table and our
    // buffers before they are written again.
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    f_.cmdPipelineBarrier(list,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    for (const auto& d : dispatches) f_.cmdUpdateBuffer(list, ring_.buffer, VkDeviceSize(d.slot) * kSlotBytes, d.bytes, d.params);
    if (table && !table->empty()) f_.cmdUpdateBuffer(list, table_.buffer, 0, table->size() * sizeof(uint64_t), table->data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    f_.cmdPipelineBarrier(list, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const auto& d : dispatches)
    {
        const VkPipeline pipeline = d.patch ? patch_ : converter_;
        if (pipeline != bound)
        {
            f_.cmdBindPipeline(list, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            bound = pipeline;
        }
        for (uint32_t first = 0; first < d.groups; first += kMaxGroups)
            f_.cmdDispatchBase(list, first, 0, d.slot, std::min(kMaxGroups, d.groups - first), 1, 1);
    }
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    f_.cmdPipelineBarrier(list, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0,
        nullptr, 0, nullptr);
    // The one piece of the game's command buffer state the dispatches changed.
    if (compute) f_.cmdBindPipeline(list, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
}

void DotsBuilderVk::NoteScene()
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (!qpcFrequency_)
    {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
        qpcFrequency_ = frequency.QuadPart;
    }
    ++window_.sceneBuilds;
    if (lastScene_ && counter.QuadPart > lastScene_)
    {
        const int64_t gap = (counter.QuadPart - lastScene_) * 1'000'000 / qpcFrequency_;
        window_.longestGapUs = std::max<uint64_t>(window_.longestGapUs, static_cast<uint64_t>(gap));
        if (gap >= kHitchMicroseconds) ++window_.hitches;
    }
    lastScene_ = counter.QuadPart;
}

void DotsBuilderVk::NoteKind(const VkAccelerationStructureBuildGeometryInfoKHR& info)
{
    const uint32_t kind = (static_cast<uint32_t>(info.mode) << 16) | (static_cast<uint32_t>(info.flags) & 0xFFFFu);
    if (buildKinds_.size() < kBuildKinds && std::find(buildKinds_.begin(), buildKinds_.end(), kind) == buildKinds_.end())
        buildKinds_.push_back(kind);
}

void DotsBuilderVk::CmdBuild(PFN_vkCmdBuildAccelerationStructuresKHR original, VkCommandBuffer list, uint32_t count,
    const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkAccelerationStructureBuildRangeInfoKHR* const* ranges, VkPipeline compute)
{
    if (!count || !infos || !ranges)
    {
        original(list, count, infos, ranges);
        return;
    }
    std::vector<Translation> translations(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> outInfos(infos, infos + count);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> outRanges(ranges, ranges + count);
    std::vector<Dispatch> dispatches;
    std::vector<std::pair<size_t, PatchParams>> patches; // dispatch index, its parameters (table filled below)
    std::vector<uint64_t> table;
    std::string summary;
    {
        std::lock_guard lock(lock_);
        // Read under the lock: every age below compares times taken in lock order.
        const uint64_t now = Now();
        if (Housekeep(now)) summary = SummaryLine(now);
        // Gaps between scene (top-level) builds are the frame times behind the summary's hitch figures.
        for (uint32_t i = 0; i < count; ++i)
            if (infos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
            {
                NoteScene();
                break;
            }
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& info = infos[i];
            Translation& t = translations[i];
            std::string error;
            if (info.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR)
            {
                std::vector<uint32_t> primitives(info.geometryCount);
                for (uint32_t g = 0; g < info.geometryCount; ++g) primitives[g] = ranges[i][g].primitiveCount;
                if (!TranslateBottom(info, primitives.data(), 0, trianglesPerSegment_, options_.fastBuild, t, error))
                {
                    ++failures_;
                    Error(error);
                    if (HasLss(info))
                    {
                        EmptyBuild(info, ranges[i], ring_.address, trianglesPerSegment_, options_.fastBuild, t);
                        outInfos[i] = t.info;
                        outRanges[i] = t.ranges.data();
                    }
                    continue;
                }
                if (!t.lss)
                {
                    outInfos[i] = t.info; // ALLOW_DATA_ACCESS only; the game's geometries
                    continue;
                }
                NoteKind(info);
                Pool* pool = PoolFor(vertices_, info.dstAccelerationStructure, t.bytes, now);
                if (!pool)
                {
                    ++failures_;
                    Error("DOTS vertex buffer allocation failed");
                    EmptyBuild(info, ranges[i], ring_.address, trianglesPerSegment_, options_.fastBuild, t);
                    outInfos[i] = t.info;
                    outRanges[i] = t.ranges.data();
                    continue;
                }
                TranslateBottom(info, primitives.data(), pool->allocation.address, trianglesPerSegment_, options_.fastBuild, t, error);
                const bool update = info.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
                bool rebuild = false;
                if (!update)
                {
                    pool->refits = 0;
                    pool->fullScratch = info.scratchData.deviceAddress;
                }
                else if (++pool->refits >= rebuildRefits_ && pool->fullScratch && info.scratchData.deviceAddress == pool->fullScratch
                    && Age(now, lastRebuild_) >= rebuildSpacingMs_)
                {
                    uint64_t total = 0;
                    for (uint32_t g = 0; g < info.geometryCount; ++g) total += primitives[g];
                    if (total <= rebuildMaxPrimitives_)
                    {
                        rebuild = true;
                        pool->refits = 0;
                        lastRebuild_ = now;
                    }
                }
                if (rebuild)
                {
                    // A full build in place of the update: the scratch is the one this BLAS's last full build used.
                    t.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                    t.info.srcAccelerationStructure = VK_NULL_HANDLE;
                    ++rebuilds_;
                }
                t.ranges.assign(ranges[i], ranges[i] + info.geometryCount);
                for (uint32_t g = 0; g < info.geometryCount; ++g)
                {
                    const auto& geometry = GeometryAt(info, g);
                    if (!SweptOrSphere(geometry.geometryType)) continue;
                    Layout layout = ReadLayout(geometry, ranges[i][g]);
                    layout.params.target = pool->allocation.address + t.offsets[g];
                    ++geometries_;
                    primitives_ += layout.primitives;
                    if (!layout.valid)
                    {
                        ++invalid_;
                        Error(std::string("LSS geometry left without hair: ") + layout.reason);
                    }
                    if (layout.primitives)
                    {
                        Dispatch d;
                        d.slot = NextSlot();
                        d.groups = (layout.primitives + 63) / 64;
                        std::memcpy(d.params, &layout.params, sizeof(ConverterParams));
                        d.bytes = sizeof(ConverterParams);
                        dispatches.push_back(d);
                    }
                    t.ranges[g] = {layout.primitives * trianglesPerSegment_, 0, 0, 0};
                }
                outInfos[i] = t.info;
                outRanges[i] = t.ranges.data();
                if (!hair_.count(info.dstAccelerationStructure))
                {
                    VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                    address.accelerationStructure = info.dstAccelerationStructure;
                    hair_[info.dstAccelerationStructure] = f_.getAccelerationStructureAddress(device_, &address);
                }
                ++builds_;
                if (update && !rebuild) ++updates_;
            }
            else if (info.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR && !hair_.empty() && info.geometryCount == 1)
            {
                const auto& g = GeometryAt(info, 0);
                const uint32_t instances = ranges[i][0].primitiveCount;
                if (g.geometryType != VK_GEOMETRY_TYPE_INSTANCES_KHR || !instances || !g.geometry.instances.data.deviceAddress) continue;
                const VkDeviceSize bytes = VkDeviceSize(instances) * kInstanceBytes;
                Pool* pool = PoolFor(copies_, info.dstAccelerationStructure, bytes, now);
                if (!pool)
                {
                    ++failures_;
                    Error("instance copy allocation failed");
                    continue;
                }
                PatchParams p{};
                p.source = g.geometry.instances.data.deviceAddress + ranges[i][0].primitiveOffset;
                p.target = pool->allocation.address;
                p.count = instances;
                p.arrayOfPointers = g.geometry.instances.arrayOfPointers ? 1 : 0;
                t.info = info;
                t.geometries.assign(1, g);
                auto& copy = t.geometries[0].geometry.instances;
                copy.arrayOfPointers = VK_FALSE;
                copy.data.deviceAddress = pool->allocation.address;
                t.info.pGeometries = t.geometries.data();
                t.info.ppGeometries = nullptr;
                t.ranges.assign(ranges[i], ranges[i] + 1);
                t.ranges[0].primitiveOffset = 0;
                outInfos[i] = t.info;
                outRanges[i] = t.ranges.data();
                Dispatch d;
                d.patch = true;
                d.slot = NextSlot();
                d.groups = (instances + 63) / 64;
                d.bytes = sizeof(PatchParams);
                patches.push_back({dispatches.size(), p});
                dispatches.push_back(d);
                ++topLevelPatched_;
            }
        }
        if (!patches.empty())
        {
            for (const auto& [structure, address] : hair_)
                if (address && table.size() < kTableEntries) table.push_back(address);
            std::sort(table.begin(), table.end());
            for (auto& [index, p] : patches)
            {
                p.table = table_.address;
                p.tableCount = static_cast<uint32_t>(table.size());
                std::memcpy(dispatches[index].params, &p, sizeof(PatchParams));
            }
        }
    }
    if (!dispatches.empty()) Record(list, dispatches, patches.empty() ? nullptr : &table, compute);
    original(list, count, outInfos.data(), outRanges.data());
    if (!summary.empty()) Summary(std::move(summary));
}

void DotsBuilderVk::CmdBuildIndirect(PFN_vkCmdBuildAccelerationStructuresIndirectKHR original, VkCommandBuffer list, uint32_t count,
    const VkAccelerationStructureBuildGeometryInfoKHR* infos, const VkDeviceAddress* indirect, const uint32_t* strides,
    const uint32_t* const* maxPrimitiveCounts, VkPipeline compute)
{
    if (!count || !infos || !maxPrimitiveCounts)
    {
        original(list, count, infos, indirect, strides, maxPrimitiveCounts);
        return;
    }
    // Indirect: the game's own builds, ALLOW_DATA_ACCESS on bottom levels. Direct: LSS builds as inactive triangles.
    std::vector<Translation> translations(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> keptInfos, directInfos;
    std::vector<VkDeviceAddress> keptIndirect;
    std::vector<uint32_t> keptStrides;
    std::vector<const uint32_t*> keptCounts;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> directRanges;
    std::vector<Dispatch> dispatches;
    std::string summary;
    {
        std::lock_guard lock(lock_);
        const uint64_t now = Now();
        if (Housekeep(now)) summary = SummaryLine(now);
        for (uint32_t i = 0; i < count; ++i)
            if (infos[i].type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR)
            {
                NoteScene();
                break;
            }
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& info = infos[i];
            Translation& t = translations[i];
            std::string error;
            if (info.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && HasLss(info))
            {
                ++indirect_;
                NoteKind(info);
                Pool* pool = TranslateBottom(info, maxPrimitiveCounts[i], 0, trianglesPerSegment_, options_.fastBuild, t, error)
                    ? PoolFor(vertices_, info.dstAccelerationStructure, t.bytes, now)
                    : nullptr;
                if (!pool)
                {
                    // hairmem.1: never handed to the driver as LSS (this device has none).
                    ++failures_;
                    Error(error.empty() ? "DOTS vertex buffer allocation failed" : error);
                    EmptyBuild(info, nullptr, ring_.address, trianglesPerSegment_, options_.fastBuild, t);
                    directInfos.push_back(t.info);
                    directRanges.push_back(t.ranges.data());
                    continue;
                }
                TranslateBottom(info, maxPrimitiveCounts[i], pool->allocation.address, trianglesPerSegment_, options_.fastBuild, t, error);
                t.ranges.assign(info.geometryCount, VkAccelerationStructureBuildRangeInfoKHR{});
                for (uint32_t g = 0; g < info.geometryCount; ++g)
                {
                    if (!SweptOrSphere(GeometryAt(info, g).geometryType)) continue;
                    ConverterParams p{};
                    p.primitiveCount = maxPrimitiveCounts[i][g];
                    p.flags = 2; // inactive: the real counts are on the GPU
                    p.target = pool->allocation.address + t.offsets[g];
                    if (p.primitiveCount)
                    {
                        Dispatch d;
                        d.slot = NextSlot();
                        d.groups = (p.primitiveCount + 63) / 64;
                        std::memcpy(d.params, &p, sizeof(p));
                        d.bytes = sizeof(p);
                        dispatches.push_back(d);
                    }
                    t.ranges[g].primitiveCount = p.primitiveCount * trianglesPerSegment_;
                    ++geometries_;
                    ++invalid_;
                }
                t.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; // inactive triangles cannot refit the last build
                t.info.srcAccelerationStructure = VK_NULL_HANDLE;
                directInfos.push_back(t.info);
                directRanges.push_back(t.ranges.data());
                Error("indirect LSS build: built from inactive triangles (no hair in it)");
                continue;
            }
            if (info.type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && TranslateBottom(info, maxPrimitiveCounts[i], 0, trianglesPerSegment_,
                    options_.fastBuild, t, error))
                keptInfos.push_back(t.info);
            else
                keptInfos.push_back(info);
            keptIndirect.push_back(indirect[i]);
            keptStrides.push_back(strides[i]);
            keptCounts.push_back(maxPrimitiveCounts[i]);
        }
    }
    if (!dispatches.empty()) Record(list, dispatches, nullptr, compute);
    if (!directInfos.empty()) f_.cmdBuild(list, static_cast<uint32_t>(directInfos.size()), directInfos.data(), directRanges.data());
    if (!keptInfos.empty())
        original(list, static_cast<uint32_t>(keptInfos.size()), keptInfos.data(), keptIndirect.data(), keptStrides.data(), keptCounts.data());
    if (!summary.empty()) Summary(std::move(summary));
}

void DotsBuilderVk::Created(VkAccelerationStructureKHR structure, VkAccelerationStructureTypeKHR type, VkDeviceSize size)
{
    if (!structure || type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR) return;
    std::lock_guard lock(lock_);
    auto [it, inserted] = sizes_.try_emplace(structure, size);
    if (!inserted)
    {
        gameBlasBytes_ -= it->second;
        it->second = size;
    }
    gameBlasBytes_ += size;
}

void DotsBuilderVk::Destroyed(VkAccelerationStructureKHR structure)
{
    std::lock_guard lock(lock_);
    const uint64_t now = Now();
    const bool hair = hair_.erase(structure) != 0;
    if (const auto it = vertices_.find(structure); it != vertices_.end())
    {
        // A spare once kSpareSettleMilliseconds have passed (see there).
        if (it->second.allocation.buffer) spares_.push_back({it->second.allocation, now});
        ++window_.destroyedPools;
        window_.destroyedAgeMs += Age(now, it->second.firstUse);
        window_.destroyedBuilds += it->second.builds;
        vertices_.erase(it);
    }
    if (const auto it = copies_.find(structure); it != copies_.end())
    {
        retired_.push_back({it->second.allocation, now});
        copies_.erase(it);
    }
    if (hair)
    {
        ++hairDestroyed_;
        ++window_.destroyed;
    }
    if (const auto it = sizes_.find(structure); it != sizes_.end())
    {
        gameBlasBytes_ -= it->second;
        sizes_.erase(it);
    }
}

void DotsBuilderVk::Copied(VkAccelerationStructureKHR source, VkAccelerationStructureKHR destination)
{
    std::lock_guard lock(lock_);
    if (!hair_.count(source) || !destination || hair_.count(destination)) return;
    VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address.accelerationStructure = destination;
    hair_[destination] = f_.getAccelerationStructureAddress(device_, &address);
}

void DotsBuilderVk::SetRebuildPolicy(uint32_t refits, uint32_t maxPrimitives, uint64_t spacingMs)
{
    std::lock_guard lock(lock_);
    rebuildRefits_ = refits ? refits : 1;
    rebuildMaxPrimitives_ = maxPrimitives;
    rebuildSpacingMs_ = spacingMs;
}

std::string DotsBuilderVk::SummaryLine(uint64_t now)
{
    uint64_t vertexBytes = 0, copyBytes = 0, spareBytes = 0, retiredBytes = 0, hairBytes = 0;
    for (const auto& [structure, pool] : vertices_) vertexBytes += pool.allocation.bytes;
    for (const auto& [structure, pool] : copies_) copyBytes += pool.allocation.bytes;
    for (const auto& r : spares_) spareBytes += r.allocation.bytes;
    for (const auto& r : retired_) retiredBytes += r.allocation.bytes;
    for (const auto& [structure, address] : hair_)
        if (const auto it = sizes_.find(structure); it != sizes_.end()) hairBytes += it->second;
    const uint64_t builds = builds_, updates = updates_, primitives = primitives_, allocations = allocations_, reuses = reuses_;
    const double seconds = std::max<double>(1.0, Age(now, window_.start) / 1000.0);
    std::string kinds;
    for (const uint32_t kind : buildKinds_)
    {
        char k[32];
        snprintf(k, sizeof(k), "%s%s/0x%X", kinds.empty() ? "" : " ", (kind >> 16) == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR ? "update" : "build",
            kind & 0xFFFFu);
        kinds += k;
    }
    // Mean age and builds over the destroyed hair BLASes that still had a vertex buffer (a compacted copy or an
    // idle-evicted one has none).
    const uint64_t destroyed = window_.destroyed, pools = window_.destroyedPools;
    char text[1280];
    snprintf(text, sizeof(text),
        "RTX_HAIR Vulkan memory: hair BLASes %zu (the game's %.1f MiB), game bottom levels %zu (%.1f MiB); ours: vertex %.1f MiB in %zu, "
        "spare %.1f MiB in %zu, retired %.1f MiB in %zu, instance copies %.1f MiB in %zu | last %.0f s: scene builds %llu (longest gap %.1f ms, "
        "%llu over %lld ms), hair builds %llu (%llu updates), segments %.0f/s, allocations %llu, reuses %llu (%llu same size), hair BLASes "
        "destroyed %llu (mean age %.1f s, %.1f builds) | %u ribbons%s | hair build kinds %s",
        hair_.size(), hairBytes / kMiB, sizes_.size(), gameBlasBytes_ / kMiB, vertexBytes / kMiB, vertices_.size(), spareBytes / kMiB, spares_.size(),
        retiredBytes / kMiB, retired_.size(), copyBytes / kMiB, copies_.size(), seconds, static_cast<unsigned long long>(window_.sceneBuilds),
        window_.longestGapUs / 1000.0, static_cast<unsigned long long>(window_.hitches), static_cast<long long>(kHitchMicroseconds / 1000),
        static_cast<unsigned long long>(builds - window_.builds), static_cast<unsigned long long>(updates - window_.updates),
        (primitives - window_.primitives) / seconds, static_cast<unsigned long long>(allocations - window_.allocations),
        static_cast<unsigned long long>(reuses - window_.reuses), static_cast<unsigned long long>(window_.reusesSame),
        static_cast<unsigned long long>(destroyed), pools ? window_.destroyedAgeMs / 1000.0 / pools : 0.0,
        pools ? double(window_.destroyedBuilds) / pools : 0.0, options_.ribbons, options_.fastBuild ? ", fast build" : "",
        kinds.empty() ? "none" : kinds.c_str());
    window_ = {};
    window_.start = now;
    window_.builds = builds;
    window_.updates = updates;
    window_.primitives = primitives;
    window_.allocations = allocations;
    window_.reuses = reuses;
    return text;
}

void DotsBuilderVk::Summary(std::string line)
{
    if (!log_) return;
    if (budgetQuery_ && physical_)
    {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        budgetQuery_(physical_, &properties);
        uint64_t usage = 0, available = 0;
        const auto& memory = properties.memoryProperties;
        for (uint32_t h = 0; h < memory.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
            if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                usage += budget.heapUsage[h];
                available += budget.heapBudget[h];
            }
        char vram[96];
        snprintf(vram, sizeof(vram), " | VRAM %.2f of %.2f GiB (the process's use of its budget)", usage / (kMiB * 1024.0), available / (kMiB * 1024.0));
        line += vram;
    }
    log_(line);
}

DotsStats DotsBuilderVk::Stats() const
{
    DotsStats s;
    s.sizeQueries = sizeQueries_;
    s.builds = builds_;
    s.updates = updates_;
    s.rebuilds = rebuilds_;
    s.geometries = geometries_;
    s.primitives = primitives_;
    s.invalidGeometries = invalid_;
    s.indirectBuilds = indirect_;
    s.failures = failures_;
    s.topLevelPatched = topLevelPatched_;
    s.evictions = evictions_;
    s.allocations = allocations_;
    s.reuses = reuses_;
    s.hairDestroyed = hairDestroyed_;
    std::lock_guard lock(lock_);
    s.hairBlas = hair_.size();
    for (const auto& [structure, pool] : vertices_)
    {
        ++s.poolBuffers;
        s.poolBytes += pool.allocation.bytes;
    }
    for (const auto& [structure, pool] : copies_)
    {
        ++s.instanceCopyBuffers;
        s.topLevelCopyBytes += pool.allocation.bytes;
    }
    for (const auto& r : spares_)
    {
        ++s.spareBuffers;
        s.spareBytes += r.allocation.bytes;
    }
    for (const auto& r : retired_)
    {
        ++s.retiredBuffers;
        s.retiredBytes += r.allocation.bytes;
    }
    s.gameBlasCount = sizes_.size();
    s.gameBlasBytes = gameBlasBytes_;
    for (const auto& [structure, address] : hair_)
        if (const auto it = sizes_.find(structure); it != sizes_.end()) s.gameHairBlasBytes += it->second;
    return s;
}

VkBuffer DotsBuilderVk::InstanceCopy(VkAccelerationStructureKHR structure, VkDeviceSize& bytes) const
{
    std::lock_guard lock(lock_);
    const auto it = copies_.find(structure);
    bytes = it == copies_.end() ? 0 : it->second.allocation.bytes;
    return it == copies_.end() ? VK_NULL_HANDLE : it->second.allocation.buffer;
}

std::string DotsBuilderVk::LastError() const
{
    std::lock_guard lock(errorLock_);
    return lastError_;
}

void DotsBuilderVk::Error(std::string text)
{
    // Its own lock, never held while taking lock_: called with lock_ held (CmdBuild) and without (BuildSizes).
    std::lock_guard lock(errorLock_);
    lastError_ = std::move(text);
}
}
