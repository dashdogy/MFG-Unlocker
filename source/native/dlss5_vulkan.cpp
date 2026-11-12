#include "dlss5_vulkan.h"
#include "dlss5.h"
#include "dlss5_auto.h"
#include "dlss5_parameters.h"
#include "dlss5_stage_common.h"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include "dlss5_vk_spirv.h"
#include "dlss5_vk.packed.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace dlss5::vulkan {
namespace {
// Measurement and completion sets per feature: a recording uses one, and it is
// reused only once that recording's sequence number arrived.
constexpr uint32_t kSets = 8;
// Per set in the slot's host-visible buffer: the meter's float2 at 0 and the
// completion sequence (uint64) at kMarkerOffset.
constexpr VkDeviceSize kRegion = 256, kMarkerOffset = 64;
constexpr uint32_t kBindings = 7, kPushBytes = 112;

// The stage; taken before dlss5.cpp's lock (dlss5::shared), never after it.
std::mutex gMutex;
// Leaf lock: the live view dlss5::Read() takes under its own lock.
std::mutex gLiveMutex;
shared::Live gLive{};
std::atomic<uint64_t> gStageRefusedTick{0};
std::atomic<void*> gInitInstance{nullptr}, gInitPhysical{nullptr}, gInitDevice{nullptr};
VulkanDirectProvider gDirect;
HMODULE gDirectRuntime = nullptr;
// The provider-ownership snapshot once per process, as dlss5.cpp's (its own pair for Vulkan).
bool gProviderChecked = false, gProviderForeign = false;
uint64_t gNativeAttempts = 0;
std::atomic<bool> gForeignLogged{false};

// The stage's Vulkan entries, X(name) each: the device's (vkGetDeviceProcAddr),
// and the physical-device queries: the loader's exports, valid for any
// VkPhysicalDevice it handed out.
#define MFG_DLSS5_VK_DEVICE_ENTRIES(X) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdBindPipeline) X(vkCmdPushDescriptorSetKHR) \
    X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdUpdateBuffer) X(vkCmdWriteTimestamp) \
    X(vkCmdResetQueryPool)
#define MFG_DLSS5_VK_LOADER_ENTRIES(X) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFormatProperties)
struct Functions {
#define MFG_DLSS5_VK(name) PFN_##name name = nullptr;
    MFG_DLSS5_VK_DEVICE_ENTRIES(MFG_DLSS5_VK)
    MFG_DLSS5_VK_LOADER_ENTRIES(MFG_DLSS5_VK)
#undef MFG_DLSS5_VK
};

// The game's output formats the stage writes back (a storage format of its
// own SPIR-V variant) and the format of our copy of it.
struct OutputFormat { VkFormat game, copy; uint32_t decode, copyIn; };
const std::array<OutputFormat, 3> kOutputs{{
    {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, spirv::decode_rgba16f, spirv::copy_in_rgba16f},
    {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, spirv::decode_rgba32f, spirv::copy_in_rgba32f},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, spirv::decode_rgba8, spirv::copy_in_rgba8},
}};

struct Device {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    Functions vk{};
    VkPhysicalDeviceMemoryProperties memory{};
    float timestampPeriod = 0.0f;
    bool timestamps = false, ready = false;
    std::array<bool, 3> outputs{};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline encode = VK_NULL_HANDLE, meter = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> decode{}, copyIn{};
} gDevice;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    NVSDK_NGX_Resource_VK resource{};
    // Created, still UNDEFINED: the next recording moves it to GENERAL first.
    bool fresh = false;
};

// A provider feature and its textures at the processing extent, as the D3D12
// stage's: the slot's live one, or the replacement created beside it.
struct Feature : common::FeatureCore {
    // encoded: the network input; ping/pong: pass outputs.
    Image encoded, ping, pong;
    bool Holds() const noexcept { return passes || params || encoded.image || ping.image || pong.image; }
};
// A replaced feature, released once every recording up to sequence `after` executed.
struct Retired { Feature feature; uint64_t after = 0; };
// One per game DLSS SR/RR feature (handle on a route).
struct Slot : Feature, common::SlotState {
    VkDevice device = VK_NULL_HANDLE;
    Api api{};
    // copy: the stage's copy of the game's output (same size, our format), read by Encode/Decode.
    Image copy;
    // The replacement being created (ready once sequence nextReady
    // executed), the features it replaced, and whether this slot replaces its
    // feature in place instead.
    Feature next;
    uint64_t nextReady = 0;
    std::array<Retired, 3> retired{};
    bool replaceRefused = false;
    VkBuffer sets = VK_NULL_HANDLE;
    VkDeviceMemory setMemory = VK_NULL_HANDLE;
    volatile uint8_t* mapped = nullptr;
    VkQueryPool queries = VK_NULL_HANDLE;
    uint64_t sequence = 0;
    std::array<uint64_t, kSets> recorded{};
    std::array<bool, kSets> measured{};
    // The Resolution Scale of the feature each set measured (only the controller's level feeds Auto).
    std::array<float, kSets> measuredScale{};
};
std::array<Slot, 8> gSlots;

void Log(const wchar_t* message) noexcept { shared::Log(message); }
void SlotFailure(Slot& slot, State state, uint32_t result) noexcept {
    slot.failed = true; slot.failureRevision = slot.revision; slot.failureState = state; slot.failureResult = result;
    shared::Failure(state, result);
}
Slot* Find(size_t route, uintptr_t game) noexcept {
    if (!game) return nullptr;
    for (auto& slot : gSlots) if (slot.occupied && !slot.retiring && slot.route == route && slot.game == game) return &slot;
    return nullptr;
}
// Set n's last recording executed (or the set was never used).
bool Done(const Slot& slot, uint32_t n) noexcept {
    if (!slot.recorded[n]) return true;
    if (!slot.mapped) return false;
    const auto marker = *reinterpret_cast<volatile const uint64_t*>(slot.mapped + n * kRegion + kMarkerOffset);
    return marker == slot.recorded[n];
}
// Every recording of this slot executed.
bool Completed(const Slot& slot) noexcept {
    for (uint32_t n = 0; n < kSets; ++n) if (!Done(slot, n)) return false;
    return true;
}
// Every recording up to sequence `value` executed: a set is reused only once
// its last recording executed, so each set's latest is the one to check.
bool Executed(const Slot& slot, uint64_t value) noexcept {
    for (uint32_t n = 0; n < kSets; ++n) if (slot.recorded[n] && slot.recorded[n] <= value && !Done(slot, n)) return false;
    return true;
}
// A set whose last recording executed and was measured (kSets: none).
uint32_t FreeSet(const Slot& slot) noexcept {
    for (uint32_t n = 0; n < kSets; ++n) if (Done(slot, n) && !slot.measured[n]) return n;
    return kSets;
}
uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) noexcept {
    for (uint32_t i = 0; i < gDevice.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (gDevice.memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return UINT32_MAX;
}
void DestroyImage(Image& image) noexcept {
    auto& vk = gDevice.vk;
    if (image.view) vk.vkDestroyImageView(gDevice.device, image.view, nullptr);
    if (image.image) vk.vkDestroyImage(gDevice.device, image.image, nullptr);
    if (image.memory) vk.vkFreeMemory(gDevice.device, image.memory, nullptr);
    image = Image{};
}
bool MakeImage(uint32_t width, uint32_t height, VkFormat format, Image& out) noexcept {
    auto& vk = gDevice.vk;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Image image{};
    if (vk.vkCreateImage(gDevice.device, &info, nullptr, &image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(gDevice.device, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format; view.subresourceRange = range;
    if (allocate.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(gDevice.device, &allocate, nullptr, &image.memory) != VK_SUCCESS
        || vk.vkBindImageMemory(gDevice.device, image.image, image.memory, 0) != VK_SUCCESS
        || vk.vkCreateImageView(gDevice.device, &view, nullptr, &image.view) != VK_SUCCESS) {
        DestroyImage(image); return false;
    }
    image.format = format; image.width = width; image.height = height; image.fresh = true;
    image.resource = NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, format, width, height, true);
    out = image;
    return true;
}
// The sets' host-visible buffer (meters and completion sequences) and the
// stage's timestamps. Without timestamps Auto keeps its starting scale.
bool MakeSets(Slot& slot) noexcept {
    if (slot.sets) return true;
    auto& vk = gDevice.vk;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kRegion * kSets;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.vkCreateBuffer(gDevice.device, &info, nullptr, &slot.sets) != VK_SUCCESS) { slot.sets = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(gDevice.device, slot.sets, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    if (allocate.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(gDevice.device, &allocate, nullptr, &slot.setMemory) != VK_SUCCESS
        || vk.vkBindBufferMemory(gDevice.device, slot.sets, slot.setMemory, 0) != VK_SUCCESS
        || vk.vkMapMemory(gDevice.device, slot.setMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) {
        if (slot.setMemory) vk.vkFreeMemory(gDevice.device, slot.setMemory, nullptr);
        vk.vkDestroyBuffer(gDevice.device, slot.sets, nullptr);
        slot.sets = VK_NULL_HANDLE; slot.setMemory = VK_NULL_HANDLE; return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(kRegion * kSets));
    slot.mapped = static_cast<volatile uint8_t*>(mapped);
    if (gDevice.timestamps) {
        VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP; queries.queryCount = kSets * 2;
        if (vk.vkCreateQueryPool(gDevice.device, &queries, nullptr, &slot.queries) != VK_SUCCESS) slot.queries = VK_NULL_HANDLE;
    }
    if (!slot.queries) Log(L"DLSS 5 Auto measurements unavailable (Vulkan): Resolution Scale and exposure keep their starting values");
    return true;
}
void DestroySets(Slot& slot) noexcept {
    auto& vk = gDevice.vk;
    if (slot.queries) vk.vkDestroyQueryPool(gDevice.device, slot.queries, nullptr);
    if (slot.mapped) vk.vkUnmapMemory(gDevice.device, slot.setMemory);
    if (slot.sets) vk.vkDestroyBuffer(gDevice.device, slot.sets, nullptr);
    if (slot.setMemory) vk.vkFreeMemory(gDevice.device, slot.setMemory, nullptr);
    slot.queries = VK_NULL_HANDLE; slot.mapped = nullptr; slot.sets = VK_NULL_HANDLE; slot.setMemory = VK_NULL_HANDLE;
}
// Releases a feature's passes, parameters and textures. A refused release
// fails the slot, which keeps what is left.
bool ReleaseFeature(Slot& slot, Feature& feature) noexcept {
    while (feature.passes) {
        auto*& handle = feature.handles[feature.passes - 1];
        const auto result = slot.direct ? gDirect.Release(handle) : slot.api.release(handle);
        if (result != NVSDK_NGX_Result_Success) {
            slot.cleanupFailed = true; SlotFailure(slot, State::eEvaluateFailed, static_cast<uint32_t>(result)); return false;
        }
        handle = nullptr; --feature.passes;
    }
    if (feature.params) {
        if (!slot.api.destroy || slot.api.destroy(feature.params) != NVSDK_NGX_Result_Success) {
            slot.cleanupFailed = true; SlotFailure(slot, State::eEvaluateFailed, NVSDK_NGX_Result_FAIL_InvalidParameter); return false;
        }
        feature.params = nullptr;
    }
    DestroyImage(feature.encoded); DestroyImage(feature.ping); DestroyImage(feature.pong);
    feature = Feature{}; return true;
}
// Any feature of the slot: live, being created, or replaced and not yet released.
bool Outstanding(const Slot& slot) noexcept {
    if (slot.Holds() || slot.next.Holds()) return true;
    for (const auto& entry : slot.retired) if (entry.feature.Holds()) return true;
    return false;
}
// Hands `feature` over to be released once everything recorded so far executed.
// False while every retired entry is still taken.
bool Retire(Slot& slot, Feature& feature) noexcept {
    if (!feature.Holds()) { feature = Feature{}; return true; }
    for (auto& entry : slot.retired) if (!entry.feature.Holds()) {
        entry.feature = std::move(feature); entry.after = slot.sequence; feature = Feature{}; return true;
    }
    return false;
}
// Releases the retired features whose recordings executed.
void Collect(Slot& slot) noexcept {
    for (auto& entry : slot.retired)
        if (!slot.cleanupFailed && entry.feature.Holds() && Executed(slot, entry.after) && !ReleaseFeature(slot, entry.feature)) return;
}
// Releases every feature, parameters and textures once every recording executed.
void Destroy(Slot& slot) noexcept {
    if (!Completed(slot) || slot.cleanupFailed) return;
    for (auto& entry : slot.retired) if (!ReleaseFeature(slot, entry.feature)) return;
    if (!ReleaseFeature(slot, slot.next)) return;
    slot.nextReady = 0;
    if (!ReleaseFeature(slot, slot)) return;
    DestroyImage(slot.copy);
    slot.measured = {};
}
void Pump() noexcept {
    for (auto& slot : gSlots) if (slot.occupied && slot.retiring && !slot.game && Completed(slot)) {
        Destroy(slot);
        if (!Outstanding(slot) && !slot.cleanupFailed) { DestroySets(slot); slot = Slot{}; }
    }
    // Hook Method Off releases the features once their work executed (as on D3D12).
    uint64_t revision = 0;
    if (!shared::Wanted(revision).enabled) for (auto& slot : gSlots) if (slot.occupied && !slot.retiring && Outstanding(slot)) {
        Collect(slot);
        if (!Completed(slot)) continue;
        Destroy(slot);
        if (!Outstanding(slot)) { slot.gpuMs = -1.0f; Log(L"DLSS 5 off: its feature was released (Vulkan)"); }
    }
}

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags written, VkPipelineStageFlags to, VkAccessFlags used) noexcept {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = written; barrier.dstAccessMask = used;
    gDevice.vk.vkCmdPipelineBarrier(cmd, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
// Fresh textures of ours go from UNDEFINED to GENERAL, where they stay.
void PrepareImages(VkCommandBuffer cmd, std::initializer_list<Image*> images) noexcept {
    std::array<VkImageMemoryBarrier, 4> barriers{};
    uint32_t count = 0;
    for (Image* image : images) {
        if (!image->image || !image->fresh || count == barriers.size()) continue;
        auto& barrier = barriers[count++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image->image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        image->fresh = false;
    }
    if (count) gDevice.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr, 0, nullptr, count, barriers.data());
}
// The recording's end on set `set`: after everything recorded before it, the
// GPU writes the next sequence number into host-visible memory of ours.
void Mark(Slot& slot, VkCommandBuffer cmd, uint32_t set) noexcept {
    Barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const uint64_t value = ++slot.sequence;
    gDevice.vk.vkCmdUpdateBuffer(cmd, slot.sets, set * kRegion + kMarkerOffset, sizeof(value), &value);
    Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    slot.recorded[set] = value;
}
struct Bindings {
    std::array<VkImageView, 4> sampled{};
    VkImageView storage = VK_NULL_HANDLE, game = VK_NULL_HANDLE;
    VkBuffer meter = VK_NULL_HANDLE;
    VkDeviceSize meterOffset = 0;
};
// Binds `pipeline` with every binding of the layout and runs it over x by y
// (8x8 tiles), or as one group.
void Run(VkCommandBuffer cmd, VkPipeline pipeline, const Bindings& b, const void* constants, uint32_t bytes,
    uint32_t x, uint32_t y, bool group = false) noexcept {
    auto& vk = gDevice.vk;
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    std::array<VkDescriptorImageInfo, 6> images{};
    for (uint32_t i = 0; i < 4; ++i) images[i] = {VK_NULL_HANDLE, b.sampled[i], VK_IMAGE_LAYOUT_GENERAL};
    images[4] = {VK_NULL_HANDLE, b.storage, VK_IMAGE_LAYOUT_GENERAL};
    images[5] = {VK_NULL_HANDLE, b.game, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffer{b.meter, b.meterOffset, 16};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (uint32_t i = 0; i < kBindings; ++i) {
        auto& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstBinding = i; write.descriptorCount = 1;
        write.descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (i == 5) write.pBufferInfo = &buffer;
        else write.pImageInfo = &images[i < 4 ? i : i == 4 ? 4 : 5];
    }
    vk.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gDevice.layout, 0, kBindings, writes.data());
    vk.vkCmdPushConstants(cmd, gDevice.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, constants);
    vk.vkCmdDispatch(cmd, group ? 1 : (x + 7) / 8, group ? 1 : (y + 7) / 8, 1);
}

// A pass's SPIR-V (a payload of the packed dlss5_vk group, word-aligned) as a compute pipeline.
bool MakePipeline(uint32_t pass, const char* entry, VkPipeline& out) noexcept {
    const auto code = embedded_payload::Find(embedded_payload::Owner(), embedded_payload::packed::dlss5_vk,
        embedded_payload::packed::dlss5_vk_items, pass);
    if (code.empty() || code.size() % 4) return false;
    auto& vk = gDevice.vk;
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = code.size(); module.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vk.vkCreateShaderModule(gDevice.device, &module, nullptr, &shader) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = shader; info.stage.pName = entry;
    info.layout = gDevice.layout;
    const bool made = vk.vkCreateComputePipelines(gDevice.device, VK_NULL_HANDLE, 1, &info, nullptr, &out) == VK_SUCCESS;
    vk.vkDestroyShaderModule(gDevice.device, shader, nullptr);
    if (!made) out = VK_NULL_HANDLE;
    return made;
}
bool Storable(VkFormat format, bool sampled) noexcept {
    VkFormatProperties properties{};
    gDevice.vk.vkGetPhysicalDeviceFormatProperties(gDevice.physical, format, &properties);
    const VkFormatFeatureFlags wanted = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | (sampled ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0);
    return (properties.optimalTilingFeatures & wanted) == wanted;
}
// The game's VkDevice: entry points, NVIDIA adapter, formats, layout and the
// pipelines, once. Null when ready, else the refused step.
const wchar_t* SetupDevice(VkDevice device) noexcept {
    if (gDevice.ready) return gDevice.device == device ? nullptr : L"a second VkDevice (the stage serves the first)";
    if (gDevice.device && gDevice.device != device) return L"a second VkDevice (the stage serves the first)";
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    if (!loader) return L"vulkan-1.dll is not loaded";
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr"));
    auto* physical = static_cast<VkPhysicalDevice>(gInitPhysical.load(std::memory_order_acquire));
    if (!gipa || !gdpa) return L"the Vulkan loader exports no vkGetInstanceProcAddr/vkGetDeviceProcAddr";
    if (!physical) return L"the game's VkPhysicalDevice was not observed (NGX Vulkan Init)";
    gDevice.device = device; gDevice.physical = physical; gDevice.gipa = gipa; gDevice.gdpa = gdpa;
    auto& vk = gDevice.vk;
#define MFG_DLSS5_PHYSICAL(name) vk.name = reinterpret_cast<PFN_##name>(GetProcAddress(loader, #name)); if (!vk.name) return L"loader export " L#name;
    MFG_DLSS5_VK_LOADER_ENTRIES(MFG_DLSS5_PHYSICAL)
#undef MFG_DLSS5_PHYSICAL
#define MFG_DLSS5_DEVICE(name) vk.name = reinterpret_cast<PFN_##name>(gdpa(device, #name)); if (!vk.name) return L"device entry " L#name;
    MFG_DLSS5_VK_DEVICE_ENTRIES(MFG_DLSS5_DEVICE)
#undef MFG_DLSS5_DEVICE
#undef MFG_DLSS5_VK_DEVICE_ENTRIES
#undef MFG_DLSS5_VK_LOADER_ENTRIES
    VkPhysicalDeviceProperties properties{};
    vk.vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.vendorID != 0x10DE) return L"adapter is not NVIDIA";
    vk.vkGetPhysicalDeviceMemoryProperties(physical, &gDevice.memory);
    gDevice.timestampPeriod = properties.limits.timestampPeriod;
    gDevice.timestamps = properties.limits.timestampComputeAndGraphics && properties.limits.timestampPeriod > 0.0f;
    if (!Storable(VK_FORMAT_R16G16B16A16_SFLOAT, true)) return L"no sampled storage RGBA16F textures";
    for (size_t n = 0; n < kOutputs.size(); ++n)
        gDevice.outputs[n] = Storable(kOutputs[n].game, false) && Storable(kOutputs[n].copy, true);
    std::array<VkDescriptorSetLayoutBinding, kBindings> bindings{};
    for (uint32_t i = 0; i < kBindings; ++i)
        bindings[i] = {i, i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set.bindingCount = kBindings; set.pBindings = bindings.data();
    if (vk.vkCreateDescriptorSetLayout(device, &set, nullptr, &gDevice.setLayout) != VK_SUCCESS) return L"descriptor set layout";
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &gDevice.setLayout; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layout, nullptr, &gDevice.layout) != VK_SUCCESS) return L"pipeline layout";
    if (!MakePipeline(spirv::encode, "Encode", gDevice.encode)
        || !MakePipeline(spirv::meter, "Meter", gDevice.meter)) return L"compute pipelines";
    for (size_t n = 0; n < kOutputs.size(); ++n)
        if (gDevice.outputs[n] && (!MakePipeline(kOutputs[n].decode, "Decode", gDevice.decode[n])
            || !MakePipeline(kOutputs[n].copyIn, "CopyIn", gDevice.copyIn[n]))) return L"compute pipelines";
    gDevice.ready = true;
    wchar_t message[300]{};
    swprintf_s(message, L"DLSS 5 Vulkan stage ready: device=%p physical=%p timestamps=%d outputs rgba16f=%d rgba32f=%d rgba8=%d",
        static_cast<void*>(device), static_cast<void*>(physical), gDevice.timestamps ? 1 : 0, gDevice.outputs[0] ? 1 : 0,
        gDevice.outputs[1] ? 1 : 0, gDevice.outputs[2] ? 1 : 0);
    Log(message);
    return nullptr;
}
// Setup for this slot: the device and its pipelines, the NGX route's entries
// (the same for the slot's lifetime) and the slot's sets.
bool Setup(Slot& slot, const Api& api) noexcept {
    if (slot.device && slot.api.Complete() && gDevice.ready && gDevice.device == slot.device) {
        if (slot.api.runtime != api.runtime || slot.api.evaluate != api.evaluate || slot.api.release != api.release) {
            SlotFailure(slot, State::eUnsupported, NVSDK_NGX_Result_FAIL_InvalidParameter); return false;
        }
        return MakeSets(slot) || (SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_OutOfGPUMemory), false);
    }
    if (!slot.device) slot.device = static_cast<VkDevice>(gInitDevice.load(std::memory_order_acquire));
    const wchar_t* step = !api.Complete() ? L"NGX Vulkan entry points incomplete"
        : !slot.device ? L"the game's VkDevice is unknown (CreateFeature without a device and no base NGX Init)"
        : SetupDevice(slot.device);
    if (step) {
        static const wchar_t* last = nullptr;
        if (last != step) {
            last = step;
            wchar_t message[400]{};
            swprintf_s(message, L"DLSS 5 Vulkan setup refused: step=%ls", step);
            Log(message);
        }
        SlotFailure(slot, State::eUnsupported, NVSDK_NGX_Result_FAIL_FeatureNotSupported); return false;
    }
    slot.api = api;
    return MakeSets(slot) || (SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_OutOfGPUMemory), false);
}
// A failed slot retries only after a settings change, once its work and cleanup completed.
bool Recovered(Slot& slot) noexcept {
    if (!slot.failed) return true;
    shared::SetState(slot.failureState, slot.failureResult);
    if (slot.revision == slot.failureRevision || slot.cleanupFailed || (slot.direct && gDirect.Faulted()) || !Completed(slot)) return false;
    Destroy(slot); if (Outstanding(slot)) return false;
    slot.failed = false; return true;
}
// autorect.1: Resolution Scale Auto's feature is its capacity, whatever its level.
Settings Expected(const Slot&, Settings wanted) noexcept {
    wanted.stage = static_cast<uint32_t>(Stage::eUpscaled);
    if (wanted.scaleAuto) wanted.scale = auto_mode::CapacityScale(wanted.scale);
    return wanted;
}
// Executed sets' timestamps and meters feed Auto (as the D3D12 stage's Harvest).
void Harvest(Slot& slot, const Settings& wanted) noexcept {
    double ms = 0.0; uint32_t timed = 0; float sum = 0.0f, samples = 0.0f;
    bool otherLevel = false;
    for (uint32_t n = 0; n < kSets; ++n) {
        if (!slot.measured[n] || !Done(slot, n)) continue;
        slot.measured[n] = false;
        uint64_t stamps[2]{};
        if (slot.queries && gDevice.vk.vkGetQueryPoolResults(gDevice.device, slot.queries, n * 2, 2, sizeof(stamps), stamps,
                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] > stamps[0]) {
            ms += double(stamps[1] - stamps[0]) * gDevice.timestampPeriod / 1.0e6; ++timed;
            otherLevel |= slot.measuredScale[n] != slot.scaling.Scale();
        }
        float meter[2]{};
        std::memcpy(meter, const_cast<const uint8_t*>(slot.mapped + n * kRegion), sizeof(meter));
        if (std::isfinite(meter[0]) && std::isfinite(meter[1]) && meter[1] > 0.0f) { sum += meter[0]; samples += meter[1]; }
    }
    if (!timed && samples <= 0.0f) return;
    const double seconds = slot.frameSeconds; slot.frameSeconds = 0.0;
    if (timed) {
        slot.gpuMs = static_cast<float>(ms / timed);
        shared::Cost(slot.cost, slot.applied.stage, common::ShownWidth(slot), common::ShownHeight(slot), slot.gpuMs, slot.clock.averageMs);
        common::StepScale(slot, wanted, seconds, otherLevel, Log,
            L"DLSS 5 Auto resolution scale (Vulkan): %.0f%% -> %.0f%% (stage %.2f ms, budget %.2f ms), within the feature",
            L"DLSS 5 Auto resolution scale (Vulkan) holds its %.0f%% quality floor (stage %.2f ms, budget %.2f ms)");
    }
    if (samples > 0.0f && wanted.adaptiveExposure) slot.exposure.Update(sum / samples, seconds, wanted.adaptiveTarget);
}
// A set whose last recording executed, or none (this frame stays unprocessed).
bool Reserve(Slot& slot, const Settings& wanted, uint32_t& set) noexcept {
    Harvest(slot, wanted);
    for (uint32_t n = 0; n < kSets; ++n) if (Done(slot, n) && !slot.measured[n]) { set = n; return true; }
    slot.resetHistory = true;
    return false;
}

// The game's DLSS output and inputs, as NGX resources of image views.
struct Output {
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    size_t format = 0;
};
bool ImageResource(uintptr_t value, const Rect& rect, NVSDK_NGX_Resource_VK& out) noexcept {
    __try {
        out = *reinterpret_cast<const NVSDK_NGX_Resource_VK*>(value);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    const auto& info = out.Resource.ImageViewInfo;
    return out.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW && info.ImageView && info.Image
        && info.SubresourceRange.levelCount >= 1 && (info.SubresourceRange.layerCount == 1 || info.SubresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS)
        && RectFits(rect, info.Width, info.Height);
}
// Logs a refused input once per change of what was refused.
bool Refused(const wchar_t* what, const NVSDK_NGX_Resource_VK& resource, const Rect& rect) noexcept {
    wchar_t reason[400]{};
    const auto& info = resource.Resource.ImageViewInfo;
    swprintf_s(reason, L"the Vulkan %ls is refused: type=%u format=%u size=%ux%u readWrite=%u levels=%u layers=%u rect=%d,%d %dx%d",
        what, static_cast<unsigned>(resource.Type), static_cast<unsigned>(info.Format), info.Width, info.Height,
        resource.ReadWrite ? 1u : 0u, info.SubresourceRange.levelCount, info.SubresourceRange.layerCount, rect.x, rect.y, rect.width, rect.height);
    shared::InputsUnavailable(reason);
    return false;
}
bool Inputs(const Frame& frame, Output& output) noexcept {
    NVSDK_NGX_Resource_VK color{}, depth{}, motion{};
    // The output is written as a storage image: one mip level, ReadWrite (STORAGE usage) as NGX requires.
    if (!ImageResource(frame.color, frame.colorRect, color) || !color.ReadWrite || color.Resource.ImageViewInfo.SubresourceRange.levelCount != 1)
        return Refused(L"DLSS output", color, frame.colorRect);
    if (!ImageResource(frame.depth, frame.depthRect, depth)) return Refused(L"depth", depth, frame.depthRect);
    if (!ImageResource(frame.motion, frame.motionRect, motion)) return Refused(L"motion vectors", motion, frame.motionRect);
    const auto& info = color.Resource.ImageViewInfo;
    for (size_t n = 0; n < kOutputs.size(); ++n)
        if (info.Format == kOutputs[n].game && gDevice.outputs[n]) {
            output.view = info.ImageView; output.width = info.Width; output.height = info.Height; output.format = n;
            return true;
        }
    return Refused(L"DLSS output format (written back: RGBA16F, RGBA32F, RGBA8)", color, frame.colorRect);
}
// Our copy of the game's output: its size, the copy format of its class.
bool CopyFor(Slot& slot, const Output& output) noexcept {
    const VkFormat format = kOutputs[output.format].copy;
    if (slot.copy.image && slot.copy.width == output.width && slot.copy.height == output.height && slot.copy.format == format) return true;
    if (slot.copy.image) { if (!Completed(slot)) return false; DestroyImage(slot.copy); }
    if (MakeImage(output.width, output.height, format, slot.copy)) return true;
    SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_OutOfGPUMemory);
    return false;
}

// Creates the feature's passes and textures on `cmd`, its recording ending on
// set `set`. The first evaluation waits until this recording executed. The
// log gives the time the provider's create calls took on the calling thread.
void CreateFeature(Slot& slot, VkCommandBuffer cmd, const Frame& frame, const Settings& run, uint32_t extentWidth, uint32_t extentHeight,
    uint32_t set, const wchar_t* waiting = L"waiting for its recording to execute") noexcept {
    if (slot.params) {
        if (slot.api.destroy(slot.params) != NVSDK_NGX_Result_Success) {
            slot.cleanupFailed = true; SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_InvalidParameter); return;
        }
        slot.params = nullptr;
    }
    const std::wstring path = shared::ProviderPath();
    if (!slot.nativeAttempted) {
        if (!gProviderChecked) {
            gProviderChecked = true;
            const HMODULE loaded = GetModuleHandleW(L"nvngx_dlssnr.dll");
            gProviderForeign = loaded && !gDirect.Module();
            if (gProviderForeign) common::LogProviderLoaded(Log, L" Vulkan", loaded, path, gNativeAttempts);
        }
        slot.directOwnershipDenied = gProviderForeign;
    }
    if (slot.nativeAttempted && !slot.nativeSucceeded && slot.directOwnershipDenied) {
        if (!slot.ownershipLogged) {
            slot.ownershipLogged = true;
            wchar_t message[200]{};
            swprintf_s(message, L"DLSS 5 Vulkan direct provider refused: nvngx_dlssnr.dll is held by another loader; native NGX create returned 0x%08X",
                slot.nativeResult);
            Log(message);
        }
        shared::SetState(State::eOtherMod); return;
    }
    if (slot.api.allocate(&slot.params) != NVSDK_NGX_Result_Success || !slot.params
        || !WriteCreate(slot.params, extentWidth, extentHeight, run)) {
        SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_InvalidParameter); return;
    }
    NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const wchar_t* directStep = L"";
    bool route = true;
    if (!slot.nativeAttempted || slot.nativeSucceeded) { slot.nativeAttempted = true; slot.direct = false; ++gNativeAttempts; }
    else {
        slot.direct = true; route = false;
        void* instance = gInitInstance.load(std::memory_order_acquire);
        if (path.empty()) {}
        else if (!instance) { result = NVSDK_NGX_Result_FAIL_PlatformError; directStep = L"initialize (the game's VkInstance was not observed)"; }
        else if ((!gDirectRuntime || gDirectRuntime == slot.api.runtime) && gDirect.Open(path.c_str(), slot.api.runtime)) {
            gDirectRuntime = slot.api.runtime;
            common::LogDirectIdentity(Log, L" Vulkan", gDirect.Module(), path);
            result = gDirect.Initialize(instance, gDevice.physical, slot.device, reinterpret_cast<void*>(gDevice.gipa),
                reinterpret_cast<void*>(gDevice.gdpa), slot.params);
            directStep = L"initialize";
            if (result == NVSDK_NGX_Result_Success) { route = true; directStep = L"create"; }
        } else { result = NVSDK_NGX_Result_FAIL_PlatformError; directStep = L"open"; }
    }
    LARGE_INTEGER frequency{}, start{}, stop{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    for (uint32_t pass = 0; route && pass < run.passes; ++pass) {
        NVSDK_NGX_Handle* handle = nullptr;
        const auto feature = static_cast<NVSDK_NGX_Feature>(kFeature);
        // Each pass is created with its own Model and strengths (PassSettings).
        if (!WritePassControls(slot.params, run, pass)) { result = NVSDK_NGX_Result_FAIL_InvalidParameter; break; }
        result = slot.direct ? gDirect.Create(slot.device, cmd, slot.params, &handle)
            : slot.api.create1 ? slot.api.create1(slot.device, cmd, feature, slot.params, &handle)
            : slot.api.create(cmd, feature, slot.params, &handle);
        if (handle) slot.handles[slot.passes++] = handle;
        if (result != NVSDK_NGX_Result_Success || !handle) break;
    }
    QueryPerformanceCounter(&stop);
    const double createMs = frequency.QuadPart ? double(stop.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart) : 0.0;
    // Whatever the create recorded completes before anything is released.
    Mark(slot, cmd, set);
    const uint32_t code = static_cast<uint32_t>(result);
    if (!slot.direct) slot.nativeResult = code;
    if (result != NVSDK_NGX_Result_Success || slot.passes != run.passes) {
        wchar_t message[2048]{};
        if (slot.direct) swprintf_s(message, L"DLSS 5 Vulkan direct provider %ls failed: result=0x%08X passes=%u/%u createMs=%.1f provider=%.1700ls",
            directStep, code, slot.passes, run.passes, createMs, path.c_str());
        else swprintf_s(message, L"DLSS 5 Vulkan native NGX create returned 0x%08X%ls%ls (createMs=%.1f)", code,
            path.empty() ? L"; no provider beside the game for the direct route" : L"; the next evaluation uses the direct provider ",
            path.substr(0, 1700).c_str(), createMs);
        Log(message);
        if (slot.passes) { SlotFailure(slot, State::eCreateFailed, result == NVSDK_NGX_Result_Success ? NVSDK_NGX_Result_FAIL_OutOfGPUMemory : code); return; }
        if (slot.direct || path.empty()) SlotFailure(slot, CreateFailureState(code), code);
        else shared::SetState(State::eWaitingForDlss, code);
        return;
    }
    if (!slot.direct) slot.nativeSucceeded = true;
    slot.applied = run; slot.width = frame.width; slot.height = frame.height;
    slot.extentWidth = extentWidth; slot.extentHeight = extentHeight; slot.createdReady = false;
    shared::Created(slot.passes);
    if (!MakeImage(extentWidth, extentHeight, VK_FORMAT_R16G16B16A16_SFLOAT, slot.encoded)
        || !MakeImage(extentWidth, extentHeight, VK_FORMAT_R16G16B16A16_SFLOAT, slot.ping)
        || (slot.passes > 1 && !MakeImage(extentWidth, extentHeight, VK_FORMAT_R16G16B16A16_SFLOAT, slot.pong))) {
        SlotFailure(slot, State::eCreateFailed, NVSDK_NGX_Result_FAIL_OutOfGPUMemory); return;
    }
    shared::SetState(State::eWaitingForDlss, code);
    wchar_t message[2048]{};
    swprintf_s(message, L"DLSS 5 feature created (Vulkan): route=%ls stage=%u size=%ux%u extent=%ux%u passes=%u preset=%u model=%u later=%ls%u create=%.1f ms provider=%.1400ls; %.200ls",
        slot.direct ? L"direct" : L"native-ngx", run.stage, slot.width, slot.height, extentWidth, extentHeight, slot.passes, run.preset,
        run.model, run.laterCustom ? L"custom/" : L"same/", LaterModel(run), createMs, path.empty() ? L"NGX-managed" : path.c_str(), waiting);
    Log(message);
}

// The live feature for `run` at this extent, on `cmd`: kept, created or
// replaced, as the D3D12 stage's Settle. True when a feature
// evaluates on `cmd` now; `run` is then what it evaluates with. A replacement
// is created beside a ready live feature at the same stage size and takes
// over once its create's recording executed; otherwise the feature is
// replaced in place once every recording executed.
bool Settle(Slot& slot, VkCommandBuffer cmd, const Frame& frame, Settings& run, uint32_t extentWidth, uint32_t extentHeight) noexcept {
    Collect(slot);
    const auto differs = [&](const Feature& feature) {
        return feature.width != frame.width || feature.height != frame.height || feature.extentWidth != extentWidth
            || feature.extentHeight != extentHeight || !SameFeature(feature.applied, run);
    };
    if (slot.HasFeature() && !differs(slot)) {
        // The replacement of a change that was undone goes.
        if (slot.next.Holds() && Retire(slot, slot.next)) slot.nextReady = 0;
        return true;
    }
    const bool beside = slot.HasFeature() && !slot.replaceRefused && slot.createdReady && slot.applied.stage == run.stage
        && slot.width == frame.width && slot.height == frame.height;
    if (!beside) {
        if (Outstanding(slot)) {
            if (!Completed(slot)) return false;
            Destroy(slot); if (Outstanding(slot)) return false;
        }
        if (!Completed(slot)) return false;
        // Every set executed; a new feature starts its measurements afresh.
        slot.measured = {};
        CreateFeature(slot, cmd, frame, run, extentWidth, extentHeight, 0);
        return false;
    }
    const Settings live = Keeping(run, slot.applied);
    // A replacement for settings that changed again since goes first.
    if (slot.next.Holds() && differs(slot.next)) {
        if (!Retire(slot, slot.next)) { run = live; return true; }
        slot.nextReady = 0;
    }
    if (!slot.next.Holds()) {
        // The create's recording ends on a free set; without one, next time.
        const uint32_t set = FreeSet(slot);
        if (set == kSets) { run = live; return true; }
        std::swap(static_cast<Feature&>(slot), slot.next);
        CreateFeature(slot, cmd, frame, run, extentWidth, extentHeight, set, L"the live feature evaluates until this recording executed");
        const bool made = !slot.failed && slot.passes == run.passes && slot.encoded.image;
        std::swap(static_cast<Feature&>(slot), slot.next);
        slot.nextReady = slot.recorded[set];
        if (!made) {
            // The live feature stays; Destroy takes what was made with it.
            slot.failed = false; slot.replaceRefused = true;
            Log(L"DLSS 5 could not create a replacement beside the live feature (Vulkan); its changes replace it in place from now on");
        }
        run = live; return true;
    }
    // The live feature moves to the retired ones (a free entry first).
    if (!Executed(slot, slot.nextReady) || !Retire(slot, slot)) { run = live; return true; }
    static_cast<Feature&>(slot) = std::move(slot.next); slot.next = Feature{}; slot.nextReady = 0;
    slot.createdReady = true; slot.resetHistory = true;
    wchar_t message[200]{};
    swprintf_s(message, L"DLSS 5 feature replaced without a gap (Vulkan): extent=%ux%u passes=%u model=%u; the previous one is released once its recordings executed",
        slot.extentWidth, slot.extentHeight, slot.passes, slot.applied.model);
    Log(message);
    return true;
}

// Copy in, encode, the network passes and decode back into the game's output,
// on `cmd` after the game's evaluation. The output stays in GENERAL (DLSS's
// storage state); our textures rest in GENERAL.
bool Record(Slot& slot, VkCommandBuffer cmd, Frame frame, const Settings& run, const Output& output, uint32_t set) noexcept {
    auto& vk = gDevice.vk;
    if (slot.resetHistory) frame.reset = 1;
    const bool measuring = slot.queries != VK_NULL_HANDLE;
    PrepareImages(cmd, {&slot.encoded, &slot.ping, &slot.pong, &slot.copy});
    // The game's DLSS (and anything before) before our reads and writes.
    Barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (measuring) {
        vk.vkCmdResetQueryPool(cmd, slot.queries, set * 2, 2);
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, slot.queries, set * 2);
    }
    const VkImageView copy = slot.copy.view;
    Bindings bindings{};
    bindings.meter = slot.sets; bindings.meterOffset = set * kRegion;
    // The output's rect into our copy at the same texels.
    const uint32_t region[4]{static_cast<uint32_t>(frame.colorRect.x), static_cast<uint32_t>(frame.colorRect.y), frame.width, frame.height};
    bindings.sampled = {copy, copy, copy, copy}; bindings.storage = copy; bindings.game = output.view;
    Run(cmd, gDevice.copyIn[output.format], bindings, region, sizeof(region), frame.width, frame.height);
    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    // Encode (and the meter) read the copy: no exposure texture on Vulkan, its layout is unknown here.
    frame.exposure = 0;
    // autorect.1: the network's rect within the processing extent (Activate) is the shaders' extent.
    const uint32_t activeWidth = common::ShownWidth(slot), activeHeight = common::ShownHeight(slot);
    const auto encode = shared::Constants(frame, run, activeWidth, activeHeight, slot.automatic, common::Unbounded(slot, run),
        common::AdaptiveGain(slot, run), false, false);
    bindings.storage = slot.encoded.view; bindings.game = slot.encoded.view;
    Run(cmd, gDevice.encode, bindings, &encode, sizeof(encode), activeWidth, activeHeight);
    if (measuring) Run(cmd, gDevice.meter, bindings, &encode, sizeof(encode), 1, 1, true);
    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    Image* outputs[2]{&slot.ping, &slot.pong};
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
    for (uint32_t pass = 0; pass < slot.passes; ++pass) {
        if (pass) Barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        Frame neural = frame;
        neural.color = reinterpret_cast<uintptr_t>(pass == 0 ? &slot.encoded.resource : &outputs[(pass - 1) % 2]->resource);
        neural.output = reinterpret_cast<uintptr_t>(&outputs[pass % 2]->resource);
        neural.colorRect = neural.outputRect = {0, 0, static_cast<int32_t>(activeWidth), static_cast<int32_t>(activeHeight)};
        WriteEvaluate(slot.params, neural, PassSettings(run, pass));
        result = slot.direct ? gDirect.Evaluate(cmd, slot.handles[pass], slot.params) : slot.api.evaluate(cmd, slot.handles[pass], slot.params, nullptr);
        if (result != NVSDK_NGX_Result_Success) break;
    }
    if (result == NVSDK_NGX_Result_Success) {
        Barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const auto decode = shared::Constants(frame, run, activeWidth, activeHeight, slot.automatic, common::Unbounded(slot, run),
            common::AdaptiveGain(slot, run), true, false);
        bindings.sampled = {slot.encoded.view, outputs[(slot.passes - 1) % 2]->view, copy, copy};
        bindings.storage = output.view; bindings.game = slot.encoded.view;
        Run(cmd, gDevice.decode[output.format], bindings, &decode, sizeof(decode), frame.width, frame.height);
        // The game reads its output after this.
        Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        if (measuring) {
            vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, slot.queries, set * 2 + 1);
            slot.measured[set] = true; slot.measuredScale[set] = common::ShownScale(slot);
        }
        slot.resetHistory = false;
        shared::Evaluated();
    } else SlotFailure(slot, State::eEvaluateFailed, static_cast<uint32_t>(result));
    Mark(slot, cmd, set);
    return result == NVSDK_NGX_Result_Success;
}

void Process(Slot& slot, const Api& api, VkCommandBuffer cmd, const NVSDK_NGX_Parameter* params, const Settings& wanted, uint64_t revision) noexcept {
    slot.revision = revision;
    common::Tick(slot);
    if (shared::Foreign()) {
        // The D3D12 route's 'DLSS 5 stopped' line names the caller.
        if (!gForeignLogged.exchange(true)) Log(L"DLSS 5 Vulkan stopped: another caller created NGX feature 18 (see 'DLSS 5 stopped')");
        shared::SetState(State::eOtherMod); return;
    }
    if (!Recovered(slot)) return;
    Frame frame{}; InputsWhy why{}; Source source{};
    if (!shared::ReadFrame(params, slot.keys, Stage::eUpscaled, frame, why, source)) {
        shared::InputsUnavailable(why, source, slot.keys); slot.resetHistory = true; shared::SetState(State::eMissingInputs); return;
    }
    if (!Setup(slot, api)) return;
    Output output{};
    if (!Inputs(frame, output)) { slot.resetHistory = true; shared::SetState(State::eMissingInputs); return; }
    Settings run = common::Effective(slot, wanted, Stage::eUpscaled, frame.width, frame.height);
    uint32_t extentWidth = 0, extentHeight = 0;
    if (!ProcessingExtent(frame.width, frame.height, run.scale, extentWidth, extentHeight)) {
        wchar_t reason[160]{};
        swprintf_s(reason, L"the stage extent %ux%u is beyond the %u texture limit", frame.width, frame.height, kMaxExtent);
        shared::InputsUnavailable(reason); shared::SetState(State::eMissingInputs); return;
    }
    if (!Settle(slot, cmd, frame, run, extentWidth, extentHeight)) return;
    if (!slot.createdReady) { if (!Completed(slot)) return; slot.createdReady = true; slot.resetHistory = true; }
    if (!CopyFor(slot, output)) return;
    uint32_t set = 0;
    if (!Reserve(slot, wanted, set)) return;
    // autorect.1: after Harvest, so an Auto step applies to this evaluation.
    common::Activate(slot, slot, wanted, run);
    if (Record(slot, cmd, frame, run, output, set)) shared::InputsAvailable();
}

void Publish() noexcept {
    shared::Live view{};
    uint64_t revision = 0;
    const Settings requested = shared::Wanted(revision);
    for (const auto& slot : gSlots) {
        if (!slot.occupied || !slot.HasFeature()) continue;
        ++view.live; view.route = slot.direct ? 2 : 1; view.stage = slot.applied.stage; view.passes = slot.passes;
        view.width = slot.width; view.height = slot.height;
        view.extentWidth = common::ShownWidth(slot); view.extentHeight = common::ShownHeight(slot);
        view.pending |= !SameFeature(slot.applied, Expected(slot, requested)) || !slot.createdReady;
        view.scale = common::ShownScale(slot); view.gpuMs = slot.gpuMs; view.budgetMs = common::AutoBudgetMs(slot);
        view.scaleFloorHeld |= requested.scaleAuto && slot.scalingSeeded && slot.scaling.floorHeld;
        view.frameMs = static_cast<float>(slot.clock.averageMs); view.exposureGain = common::AdaptiveGain(slot, requested);
    }
    std::lock_guard lock(gLiveMutex);
    gLive = view;
}
bool ReadLive(shared::Live& out) noexcept {
    {
        std::lock_guard lock(gLiveMutex);
        out = gLive;
    }
    const uint64_t refused = gStageRefusedTick.load(std::memory_order_acquire);
    out.stageUnavailable = refused && GetTickCount64() - refused < 1000;
    return true;
}
// Before upscaling and Present have no Vulkan stage in this build.
void StageUnavailable(Stage stage) noexcept {
    static Stage logged = Stage::eAuto;
    gStageRefusedTick.store(GetTickCount64(), std::memory_order_release);
    shared::SetState(State::eStageUnavailable);
    if (logged == stage) return;
    logged = stage;
    Log(stage == Stage::eRender
        ? L"DLSS 5 on Vulkan: Hook Method Before upscaling is not available in this build (After upscaling and Auto run); the frame is left as is"
        : L"DLSS 5 on Vulkan: Hook Method Present is not available in this build (After upscaling and Auto run); the frame is left as is");
}
}

void ObserveInit(void* instance, void* physical, void* device) noexcept {
    if (instance) gInitInstance.store(instance, std::memory_order_release);
    if (physical) gInitPhysical.store(physical, std::memory_order_release);
    if (device) gInitDevice.store(device, std::memory_order_release);
}
void GameCreated(size_t route, const NVSDK_NGX_Handle* handle, const dlss_sr::CreateKeys& keys, void* device) noexcept {
    if (!handle) return;
    shared::SetVulkanLive(&ReadLive);
    std::lock_guard lock(gMutex);
    Pump();
    if (Find(route, reinterpret_cast<uintptr_t>(handle))) return;
    for (auto& slot : gSlots) if (!slot.occupied) {
        slot.occupied = true; slot.route = route; slot.game = reinterpret_cast<uintptr_t>(handle); slot.keys = keys;
        slot.device = static_cast<VkDevice>(device);
        return;
    }
    shared::InputsUnavailable(L"all 8 Vulkan DLSS feature slots are in use, so this DLSS feature is not tracked");
    shared::Failure(State::eMissingInputs, NVSDK_NGX_Result_FAIL_OutOfGPUMemory);
}
void AfterEvaluate(size_t route, const Api& api, void* command, const NVSDK_NGX_Handle* game,
    const NVSDK_NGX_Parameter* params, NVSDK_NGX_Result result) noexcept {
    if (result != NVSDK_NGX_Result_Success || !command || !game || !params) return;
    try {
        std::lock_guard lock(gMutex);
        Pump();
        Slot* slot = Find(route, reinterpret_cast<uintptr_t>(game));
        if (!slot) return;
        uint64_t revision = 0;
        const Settings wanted = shared::Wanted(revision);
        if (!wanted.enabled) slot->resetHistory = true;
        else if (wanted.stage == static_cast<uint32_t>(Stage::eRender) || wanted.stage == static_cast<uint32_t>(Stage::ePresent))
            StageUnavailable(static_cast<Stage>(wanted.stage));
        else Process(*slot, api, static_cast<VkCommandBuffer>(command), params, wanted, revision);
        Publish();
    } catch (...) { shared::Failure(State::eEvaluateFailed, kProviderFault); }
}
void BeforeRelease(size_t route, const NVSDK_NGX_Handle* game) noexcept {
    std::lock_guard lock(gMutex);
    if (auto* slot = Find(route, reinterpret_cast<uintptr_t>(game))) {
        slot->retiring = true;
        if (Completed(*slot)) Destroy(*slot);
    }
}
void AfterRelease(size_t route, const NVSDK_NGX_Handle* game, NVSDK_NGX_Result result) noexcept {
    std::lock_guard lock(gMutex);
    for (auto& slot : gSlots) if (slot.occupied && slot.retiring && slot.route == route && slot.game == reinterpret_cast<uintptr_t>(game)) {
        if (result == NVSDK_NGX_Result_Success) slot.game = 0; else slot.retiring = false;
    }
    Pump();
    Publish();
}
}
