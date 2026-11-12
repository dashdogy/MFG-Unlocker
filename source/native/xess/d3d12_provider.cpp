// ngxprov.1: XeMFG as a D3D12 DLSS-G provider (d3d12_provider.h).
#include "d3d12_provider.h"
#include "ngx_provider.h"
#include "ngx_provider_policy.h"
#include "standalone_policy.h"
#include "xemfg_host_common.h"
#include "xess_mod.h"
#include "cuda/provider_list.h"
#include "vulkan/vk_host.h"
#include "vulkan/xefg_internal.h"
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace xess_d3d12_provider
{
namespace
{
using Microsoft::WRL::ComPtr;
using xemfg_host::Escape, xemfg_host::Log;

constexpr uint32_t kMaxGenerated = ngx_provider::kMaxGenerated;
// Streamline's states for the evaluation's resources (d3d12_provider.h).
constexpr D3D12_RESOURCE_STATES kBackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_COPY_DEST;            // depth, motion vectors, HUD-less
constexpr D3D12_RESOURCE_STATES kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;    // OutputInterpolated, OutputReal
// XeFG's: inputs between Executes, outputs inside one.
constexpr D3D12_RESOURCE_STATES kSlotState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kGenerateState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

int32_t Ngx(NVSDK_NGX_Result result) { return static_cast<int32_t>(result); }

struct Device
{
    std::mutex mutex;
    bool ready = false;   // XeFG's internal entry points on the route's pair (unlocked, ported model)
    std::string reason = "not_initialized";
    ComPtr<ID3D12Device> device;
    xefg_internal::Api api;
    uint32_t liveFeatures = 0;
    // pluginprov.2: the timestamp frequency of a direct queue on `device`, read once (0: no GPU timing).
    uint64_t timestampFrequency = 0;
    bool frequencyRead = false;
};
Device gDevice;

// pluginprov.2: one evaluation's three timestamps in the feature's query heap (ngx_provider::GpuTiming).
struct TimingRecord
{
    bool pending = false;       // resolved into the readback buffer, not yet read
    bool batchStart = false;    // the evaluation that copied the batch's inputs
    uint64_t consumedEnd = 0;   // the end stamp last read from this record: the same value again is a stale read
};

struct Slot   // one real frame's inputs, kept for the next batch as its previous frame
{
    ComPtr<ID3D12Resource> backbuffer, depth, motion, hudless;
    bool hasHudless = false;
    float projection[16]{};
    float jitterX = 0, jitterY = 0, motionScaleX = 0, motionScaleY = 0;
    ngx_provider::InputRect depthRect, motionRect;   // the areas of this frame's depth and motion XeFG reads
};

// What the GPU may still read when a feature is released or re-created.
struct Retired
{
    xefg_internal::Api api;
    xefg_internal::Handle context = nullptr;
    uint64_t pipelineToken{};
    std::unique_ptr<xefg_cuda::ProviderList> proxy;
    std::array<Slot, 2> slots;
    std::array<ComPtr<ID3D12Resource>, kMaxGenerated> outputs;
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> readback;
};

// Released 2 s after the last recording that used it. A thread that can't start leaks it rather than free it under
// the GPU.
void Retire(std::unique_ptr<Retired> retired) noexcept
{
    Retired* raw = retired.release();
    if (!raw) return;
    try {
        std::thread([raw] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (raw->context) raw->api.Destroy(raw->context);
            xefg_cuda::ForgetPipelines(raw->pipelineToken);
            delete raw;
        }).detach();
    } catch (...) {}
}

struct Feature
{
    xefg_internal::Handle context = nullptr;
    uint64_t pipelineToken{};
    std::unique_ptr<xefg_cuda::ProviderList> proxy;
    uint32_t width = 0, height = 0;             // back buffer
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;   // back buffer copies and outputs (ngx_provider::ColorFormat)
    D3D12_RESOURCE_DESC depthDesc{}, motionDesc{};
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t hudlessWidth = 0, hudlessHeight = 0;
    bool inverted = false;
    std::array<Slot, 2> slots;
    uint32_t current = 0;
    bool havePrevious = false;
    std::array<ComPtr<ID3D12Resource>, kMaxGenerated> outputs;
    std::array<D3D12_RESOURCE_STATES, kMaxGenerated> outputStates{};
    uint32_t lastIndex = 0;                     // the previous evaluation's DLSSG.MultiFrameIndex
    uint32_t batchCount = 0;
    bool batchReady = false;
    bool cameraLogged = false, realSkippedLogged = false;
    ngx_provider::DepthMotionRects loggedRects;  // d3d12_provider_rects: the last one logged
    uint32_t rectLogs = 0;
    bool broken = false;                        // creation failed for these inputs: no retry until they change
    uint64_t batches = 0, failures = 0, evaluations = 0, realCopies = 0, realInPlace = 0;
    std::string fault, lastLoggedFault, brokenFault;
    // pluginprov.2: the batch's XeFG call, kept for its later evaluations, which each record their own output
    // (ngx_provider::ExecuteRange). XeFG takes the same output array for every call of a pair.
    xefg_internal::ExecuteParams exec{};
    std::array<ID3D12Resource*, kMaxGenerated> execOutputs{};
    std::array<xefg_internal::OutputBase, kMaxGenerated> execBases{};
    uint32_t generatedThrough = 0;              // the batch's outputs recorded so far
    uint64_t paused = 0;                        // evaluations with DLSSG.EnableInterp 0
    // pluginprov.2: GPU timing.
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> readback;
    const uint64_t* stamps = nullptr;           // the readback buffer, mapped while the feature lives
    std::array<TimingRecord, ngx_provider::kTimingRecords> records{};
    uint32_t nextRecord = 0;
    int32_t record = -1;                        // this evaluation's, while it records
    bool batchStartRecord = false;              // this evaluation copied the batch's inputs
    bool inputsStamped = false;                 // this evaluation's stamp 1 is recorded
    bool timingUnavailable = false;
    ngx_provider::GpuTiming timing;
};

template <class T> T Number(NVSDK_NGX_Parameter* parameters, const char* name, T fallback)
{
    T value{};
    return NVSDK_NGX_SUCCEED(parameters->Get(name, &value)) ? value : fallback;
}

// A 2D texture, one sample. OptiScaler sets some (its depth copy) as void*, Streamline as ID3D12Resource*.
ID3D12Resource* Texture(NVSDK_NGX_Parameter* parameters, const char* name, D3D12_RESOURCE_DESC& desc)
{
    void* value = nullptr;
    ID3D12Resource* resource = nullptr;
    if (NVSDK_NGX_SUCCEED(parameters->Get(name, &value)) && value) resource = static_cast<ID3D12Resource*>(value);
    else if (NVSDK_NGX_FAILED(parameters->Get(name, &resource))) resource = nullptr;
    if (!resource) return nullptr;
    desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height)
        return nullptr;
    return resource;
}

// DLSSG.<prefix>{BaseX,BaseY,Width,Height}; zero when absent.
ngx_provider::InputRect Subrect(NVSDK_NGX_Parameter* parameters, const std::string& prefix)
{
    return {Number<unsigned int>(parameters, (prefix + "BaseX").c_str(), 0u), Number<unsigned int>(parameters, (prefix + "BaseY").c_str(), 0u),
            Number<unsigned int>(parameters, (prefix + "Width").c_str(), 0u), Number<unsigned int>(parameters, (prefix + "Height").c_str(), 0u)};
}

std::string RectText(const ngx_provider::InputRect& rect)
{
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," + std::to_string(rect.width) + "x" + std::to_string(rect.height);
}

class Barriers
{
  public:
    void Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (!resource || before == after || count_ == items_.size()) return;
        D3D12_RESOURCE_BARRIER& barrier = items_[count_++];
        barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
    }
    void Record(ID3D12GraphicsCommandList* list)
    {
        if (count_) list->ResourceBarrier(count_, items_.data());
        count_ = 0;
    }

  private:
    std::array<D3D12_RESOURCE_BARRIER, 16> items_{};
    UINT count_ = 0;
};

// Subresource 0, the top-left width x height. Color formats of one group.
void CopyRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* target, ID3D12Resource* source, uint32_t width, uint32_t height)
{
    D3D12_TEXTURE_COPY_LOCATION to{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    to.SubresourceIndex = from.SubresourceIndex = 0;
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
}

HRESULT CreateTexture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                      ComPtr<ID3D12Resource>& out)
{
    out.Reset();
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
}

D3D12_RESOURCE_DESC Texture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    return {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
}

// A copy of the input with its own format, size and planes (CopyResource), shader-readable. Depth-stencil formats keep
// ALLOW_DEPTH_STENCIL, without which they can't be created.
D3D12_RESOURCE_DESC SameAs(const D3D12_RESOURCE_DESC& source)
{
    D3D12_RESOURCE_DESC desc = source;
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = source.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return desc;
}

// pluginprov.2: depth history as a plain color texture where the format allows (ngx_provider::ReadableDepthFormat), as
// the Vulkan provider keeps it; stencil formats keep their own.
D3D12_RESOURCE_DESC DepthCopyDesc(const D3D12_RESOURCE_DESC& source)
{
    D3D12_RESOURCE_DESC desc = SameAs(source);
    const DXGI_FORMAT readable = ngx_provider::ReadableDepthFormat(source.Format);
    if (readable != DXGI_FORMAT_UNKNOWN) {
        desc.Format = readable;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    }
    return desc;
}

bool SameDesc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.MipLevels == b.MipLevels &&
           a.DepthOrArraySize == b.DepthOrArraySize;
}

bool PrepareDevice(Device& d)
{
    std::string why;
    // The pair the SDK route drives (OptiScaler's), else the player's from the runtime search, as on Vulkan.
    const HMODULE fg = xess_vulkan::ProviderRuntime(why);
    if (!fg) { d.reason = "runtime_" + why; return false; }
    if (!xess_mod::PrepareProvider(d.device.Get(), why)) { d.reason = "route_" + why; return false; }
    if (!xefg_internal::Resolve(fg, d.api, why)) { d.reason = "xefg_" + why; return false; }
    xess_mod::InstallProviderTensorCores(fg, d.device.Get());
    d.ready = true;
    d.reason.clear();
    Log("d3d12_provider_ready", std::string("\"xefg\":\"") + d.api.family + "\",\"generated\":" + std::to_string(kMaxGenerated) +
        ",\"model\":\"" + xess_mod::ProviderModelName() + "\",\"outputs_per_evaluation\":" +
        (ngx_provider::WholeBatch() ? "\"all\"" : "1"));
    return true;
}

// ---------------------------------------------------------------- pluginprov.2: GPU timing (ngx_provider::GpuTiming)
uint64_t TimestampFrequency(Device& d)
{
    if (!d.frequencyRead) {
        d.frequencyRead = true;
        d.timestampFrequency = 0;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        UINT64 frequency = 0;
        if (d.device && SUCCEEDED(d.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
            SUCCEEDED(queue->GetTimestampFrequency(&frequency)))
            d.timestampFrequency = frequency;
        Log("d3d12_provider_timing", "\"frequency\":" + std::to_string(d.timestampFrequency));
    }
    return d.timestampFrequency;
}

// The query heap and its mapped readback buffer, once per feature. Without them the provider runs untimed.
bool EnsureTiming(Device& d, Feature& f)
{
    if (f.queryHeap && f.stamps) return true;
    if (f.timingUnavailable || !TimestampFrequency(d)) return false;
    constexpr UINT kQueries = ngx_provider::kTimingRecords * 3;
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kQueries;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, kQueries * sizeof(uint64_t), 1, 1, 1,
                                     DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    void* mapped = nullptr;
    if (FAILED(d.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&f.queryHeap))) ||
        FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&f.readback))) ||
        FAILED(f.readback->Map(0, nullptr, &mapped)) || !mapped) {
        f.queryHeap.Reset();
        f.readback.Reset();
        f.timingUnavailable = true;
        Log("d3d12_provider_timing", "\"available\":false");
        return false;
    }
    f.stamps = static_cast<const uint64_t*>(mapped);
    f.records = {};
    return true;
}

// The record's stamps once the GPU has written them (a value not read before), into f.timing.
void ConsumeRecord(Device& d, Feature& f, uint32_t record)
{
    TimingRecord& slot = f.records[record];
    if (!slot.pending || !f.stamps) return;
    const uint64_t* stamps = f.stamps + record * 3;
    const ngx_provider::GpuStamps values{stamps[0], stamps[1], stamps[2]};
    if (values.end == slot.consumedEnd) return;   // not written yet: read again when the record comes round
    slot.pending = false;
    slot.consumedEnd = values.end;
    f.timing.Add(values, slot.batchStart, d.timestampFrequency);
}

// Stamp 0 opens the evaluation's record (reading the record's previous evaluation first), 1 marks the inputs copied
// (once; stamp 2 writes it when nothing did), 2 closes the record and resolves its three stamps into the readback buffer.
void Stamp(Device& d, Feature& f, ID3D12GraphicsCommandList* list, uint32_t which)
{
    if (which == 0) {
        f.record = -1;
        f.batchStartRecord = false;
        f.inputsStamped = false;
        if (!EnsureTiming(d, f)) return;
        const uint32_t record = f.nextRecord++ % ngx_provider::kTimingRecords;
        ConsumeRecord(d, f, record);
        f.records[record].pending = false;
        f.record = int32_t(record);
    }
    if (f.record < 0 || !f.queryHeap || !f.readback) return;
    const UINT base = UINT(f.record) * 3;
    if (which == 1) {
        if (f.inputsStamped) return;
        f.inputsStamped = true;
    }
    if (which == 2 && !f.inputsStamped) list->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
    list->EndQuery(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + which);
    if (which == 2) {
        list->ResolveQueryData(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, 3, f.readback.Get(), base * sizeof(uint64_t));
        TimingRecord& record = f.records[size_t(f.record)];
        record.pending = true;
        record.batchStart = f.batchStartRecord;
        f.record = -1;
    }
}

// The averages since the last log line, for the batch line; empty before any timed batch.
std::string TimingFields(const Device& d, Feature& f)
{
    if (!f.timing.Batches()) return {};
    char text[160]{};
    std::snprintf(text, sizeof(text), ",\"gpu_ms\":{\"inputs\":%.3f,\"total\":%.3f,\"batches\":%llu}",
                  f.timing.InputsMs(d.timestampFrequency), f.timing.TotalMs(d.timestampFrequency),
                  static_cast<unsigned long long>(f.timing.Batches()));
    f.timing.Reset();
    return text;
}

// XeFG's internal entry points for `device`. Under gDevice.mutex. A refusal is final and, the game having one device,
// noted for the process (the shim's Refusal and MaxGeneratedFrames). Another device is taken once no feature lives
// on the first; the unlock and the ported model are image-wide.
bool EnsureDevice(Device& d, ID3D12Device* device)
{
    if (!device) return false;
    if (d.ready) {
        if (d.device.Get() == device) return true;
        if (d.liveFeatures) { d.reason = "second_device"; return false; }
        d.device = device;
        d.frequencyRead = false;
        Log("d3d12_provider_device", "\"changed\":true");
        return true;
    }
    if (d.reason != "not_initialized" && d.reason != "pending") return false;
    d.device = device;
    if (PrepareDevice(d)) return true;
    ngx_provider::NoteRefusal("d3d12_" + d.reason);
    return false;
}

struct Inputs
{
    ID3D12Resource* backbuffer = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* hudless = nullptr;
    D3D12_RESOURCE_DESC backbufferDesc{}, depthDesc{}, motionDesc{}, hudlessDesc{};
    ngx_provider::InputRect depthSubrect, motionSubrect;   // as DLSS-G gave them
    ngx_provider::DepthMotionRects rects;                  // what XeFG reads (ngx_provider::ChooseDepthMotionRects)
};

std::unique_ptr<Retired> TakeResources(Device& d, Feature& f)
{
    auto retired = std::make_unique<Retired>();
    retired->api = d.api;
    retired->context = f.context;
    retired->pipelineToken = std::exchange(f.pipelineToken, 0);
    retired->proxy = std::move(f.proxy);
    retired->slots = std::move(f.slots);
    retired->outputs = std::move(f.outputs);
    retired->queryHeap = std::move(f.queryHeap);
    retired->readback = std::move(f.readback);
    f.context = nullptr;
    f.slots = {};
    f.outputs = {};
    f.exec = {};
    f.execOutputs = {};
    f.generatedThrough = 0;
    f.stamps = nullptr;
    f.records = {};
    f.record = -1;
    return retired;
}

// The textures of one feature and XeFG's context. Re-created (the old ones retired) when any input's size or format
// changes.
bool EnsureFeature(Device& d, Feature& f, const Inputs& in, bool inverted)
{
    const auto& bb = in.backbufferDesc;
    const DXGI_FORMAT format = ngx_provider::ColorFormat(bb.Format);
    if (format == DXGI_FORMAT_UNKNOWN) { f.fault = "backbuffer_format_" + std::to_string(int(bb.Format)); return false; }
    // XeFG reads depth and motion vectors over areas of one size. The Vulkan provider resamples them; this one takes the
    // game's rectangles, or padding when it gave none.
    if (!in.rects.depth.width) {
        f.fault = "depth_motion_size_" + std::to_string(in.depthDesc.Width) + "x" + std::to_string(in.depthDesc.Height) + "_" +
                  std::to_string(in.motionDesc.Width) + "x" + std::to_string(in.motionDesc.Height) + "_subrects_" +
                  RectText(in.depthSubrect) + "_" + RectText(in.motionSubrect);
        return false;
    }
    const DXGI_FORMAT hudlessFormat = in.hudless ? ngx_provider::ColorFormat(in.hudlessDesc.Format) : DXGI_FORMAT_UNKNOWN;
    const uint32_t width = uint32_t(bb.Width), height = bb.Height;
    const uint32_t hudlessWidth = hudlessFormat ? uint32_t(in.hudlessDesc.Width) : 0;
    const uint32_t hudlessHeight = hudlessFormat ? in.hudlessDesc.Height : 0;
    const bool sameInputs = f.width == width && f.height == height && f.format == format &&
                            SameDesc(f.depthDesc, in.depthDesc) && SameDesc(f.motionDesc, in.motionDesc) &&
                            f.hudlessFormat == hudlessFormat && f.hudlessWidth == hudlessWidth &&
                            f.hudlessHeight == hudlessHeight && f.inverted == inverted;
    if (sameInputs && f.context) return true;
    if (sameInputs && f.broken) { f.fault = f.brokenFault; return false; }
    if (f.context || f.outputs[0] || f.slots[0].backbuffer) Retire(TakeResources(d, f));
    const auto fail = [&f](std::string why) {
        f.fault = why;
        f.brokenFault = std::move(why);
        f.broken = true;
        return false;
    };
    f.broken = false;
    f.havePrevious = false;
    f.width = width;
    f.height = height;
    f.format = format;
    f.depthDesc = in.depthDesc;
    f.motionDesc = in.motionDesc;
    f.hudlessFormat = hudlessFormat;
    f.hudlessWidth = hudlessWidth;
    f.hudlessHeight = hudlessHeight;
    f.inverted = inverted;
    ID3D12Device* device = d.device.Get();
    for (Slot& slot : f.slots) {
        slot = {};
        if (FAILED(CreateTexture(device, Texture2D(width, height, format, D3D12_RESOURCE_FLAG_NONE), kSlotState, slot.backbuffer)) ||
            FAILED(CreateTexture(device, DepthCopyDesc(in.depthDesc), kSlotState, slot.depth)) ||
            FAILED(CreateTexture(device, SameAs(in.motionDesc), kSlotState, slot.motion)) ||
            (hudlessFormat && FAILED(CreateTexture(device, Texture2D(hudlessWidth, hudlessHeight, hudlessFormat, D3D12_RESOURCE_FLAG_NONE),
                                                   kSlotState, slot.hudless))))
            return fail("input_textures");
    }
    for (uint32_t i = 0; i < kMaxGenerated; ++i) {
        if (FAILED(CreateTexture(device, Texture2D(width, height, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), kGenerateState,
                                 f.outputs[i])))
            return fail("output_textures");
        f.outputStates[i] = kGenerateState;
    }
    xefg_internal::Handle context = nullptr;
    f.pipelineToken = xefg_cuda::NewPipelineToken();
    xefg_cuda::PipelineScope pipelineScope(f.pipelineToken);
    int32_t result = d.api.CreateContext(device, &context);
    if (result != 0 || !context) {
        xefg_cuda::ForgetPipelines(std::exchange(f.pipelineToken, 0));
        return fail("create_context_" + std::to_string(result));
    }
    xefg_internal::InitParams init{};
    init.width = width;
    init.height = height;
    init.format = format;
    init.flags = inverted ? 1u : 0u;   // XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH; motion vectors in pixels (scale below)
    init.maxInterpolatedFrames = kMaxGenerated;
    init.creationNodeMask = init.visibleNodeMask = 1;
    init.uiMode = 0;   // AUTO: from the inputs given (HUD-less color)
    result = d.api.Init(context, init);
    if (result != 0) {
        // Nothing was recorded with it yet.
        d.api.Destroy(context);
        xefg_cuda::ForgetPipelines(std::exchange(f.pipelineToken, 0));
        return fail("init_" + std::to_string(result));
    }
    f.context = context;
    if (xefg_cuda::Snapshot().state == xefg_cuda::State::Active) f.proxy = std::make_unique<xefg_cuda::ProviderList>();
    Log("d3d12_provider_feature", "\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
        ",\"format\":" + std::to_string(int(format)) + ",\"backbuffer_format\":" + std::to_string(int(bb.Format)) +
        ",\"depth\":[" + std::to_string(in.depthDesc.Width) + "," + std::to_string(in.depthDesc.Height) + "," +
        std::to_string(int(in.depthDesc.Format)) + "],\"motion\":[" + std::to_string(in.motionDesc.Width) + "," +
        std::to_string(in.motionDesc.Height) + "],\"extent\":[" + std::to_string(in.rects.depth.width) + "," + std::to_string(in.rects.depth.height) +
        "],\"motion_format\":" + std::to_string(int(in.motionDesc.Format)) +
        ",\"hudless\":" + (hudlessFormat ? "true" : "false") + ",\"inverted\":" + (inverted ? "true" : "false"));
    return true;
}

xefg_internal::Resource Describe(ID3D12Resource* resource, uint32_t width, uint32_t height)
{
    return {resource, 0, 0, width, height};
}

xefg_internal::Resource Describe(ID3D12Resource* resource, const ngx_provider::InputRect& area)
{
    return {resource, area.x, area.y, area.width, area.height};
}

void DescribeFrame(const Feature& f, const Slot& slot, xefg_internal::Frame& frame)
{
    frame = {};
    frame.backbuffer = Describe(slot.backbuffer.Get(), f.width, f.height);
    frame.depth = Describe(slot.depth.Get(), slot.depthRect);
    frame.motionVectors = Describe(slot.motion.Get(), slot.motionRect);
    if (slot.hasHudless) frame.hudless = Describe(slot.hudless.Get(), f.hudlessWidth, f.hudlessHeight);
    for (int i = 0; i < 16; ++i) frame.viewMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity: no camera matrices given
    std::memcpy(frame.projectionMatrix, slot.projection, sizeof(frame.projectionMatrix));
    frame.outputWidth = float(f.width);
    frame.outputHeight = float(f.height);
    frame.jitterX = slot.jitterX;
    frame.jitterY = slot.jitterY;
    frame.motionVectorScaleX = slot.motionScaleX;
    frame.motionVectorScaleY = slot.motionScaleY;
}

// pluginprov.2: XeFG records the outputs `range` names (from output 0 also the pair's shared work) with the batch's call;
// they then wait as copy sources for their evaluations.
bool RecordOutputs(Device& d, Feature& f, ID3D12GraphicsCommandList* list, const ngx_provider::OutputRange& range)
{
    f.exec.firstOutput = range.first;
    f.exec.outputCount = range.count;
    xefg_cuda::PipelineScope pipelineScope(f.pipelineToken);
    const bool proxied = f.proxy && f.proxy->Bind(list);
    const int32_t result = d.api.Execute(f.context, proxied ? f.proxy->Interface() : list, f.exec);
    if (proxied) {
        f.proxy->Unbind();
        if (result != 0) f.proxy->Disable(result);
    }
    Barriers barriers;
    for (uint32_t i = range.first; i < range.through && i < kMaxGenerated; ++i) {
        barriers.Add(f.outputs[i].Get(), f.outputStates[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.outputStates[i] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    barriers.Record(list);
    if (result != 0) { f.fault = "execute_" + std::to_string(result); return false; }
    f.generatedThrough = range.through;
    return true;
}

// The batch's first evaluation: the frame's inputs into the current slot, XeFG's call for the batch, then the outputs
// `range` names (ngx_provider::ExecuteRange).
bool GenerateBatch(Device& d, Feature& f, ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, uint32_t count,
                   const ngx_provider::OutputRange& range)
{
    Inputs in;
    in.backbuffer = Texture(parameters, "DLSSG.Backbuffer", in.backbufferDesc);
    in.depth = Texture(parameters, "DLSSG.Depth", in.depthDesc);
    in.motion = Texture(parameters, "DLSSG.MVecs", in.motionDesc);
    in.hudless = Texture(parameters, "DLSSG.HUDLess", in.hudlessDesc);
    if (!in.backbuffer || !in.depth || !in.motion) { f.fault = "inputs_missing"; return false; }
    in.depthSubrect = Subrect(parameters, "DLSSG.DepthSubrect");
    in.motionSubrect = Subrect(parameters, "DLSSG.MVecsSubrect");
    in.rects = ngx_provider::ChooseDepthMotionRects(in.depthSubrect, in.motionSubrect, uint32_t(in.depthDesc.Width), in.depthDesc.Height,
                                                    uint32_t(in.motionDesc.Width), in.motionDesc.Height);
    const bool inverted = Number<int>(parameters, "DLSSG.DepthInverted", 0) != 0;
    if (!EnsureFeature(d, f, in, inverted)) return false;
    const bool reset = Number<int>(parameters, "DLSSG.Reset", 0) != 0;
    Slot& slot = f.slots[f.current];
    slot.hasHudless = in.hudless && slot.hudless;
    slot.depthRect = in.rects.depth;
    slot.motionRect = in.rects.motion;
    if (in.rects != f.loggedRects && f.rectLogs < 16) {   // the first choice and the first changes: what the game passes
        ++f.rectLogs;
        f.loggedRects = in.rects;
        Log("d3d12_provider_rects", std::string("\"source\":\"") + (in.rects.fromSubrects ? "subrects" : "padding") +
            "\",\"depth_rect\":\"" + RectText(in.rects.depth) + "\",\"motion_rect\":\"" + RectText(in.rects.motion) +
            "\",\"depth_subrect\":\"" + RectText(in.depthSubrect) + "\",\"motion_subrect\":\"" + RectText(in.motionSubrect) +
            "\",\"depth_texture\":[" + std::to_string(in.depthDesc.Width) + "," + std::to_string(in.depthDesc.Height) +
            "],\"motion_texture\":[" + std::to_string(in.motionDesc.Width) + "," + std::to_string(in.motionDesc.Height) + "]");
    }
    // The frame's inputs.
    Barriers barriers;
    barriers.Add(in.backbuffer, kBackbufferState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(in.depth, kInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(in.motion, kInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (slot.hasHudless) barriers.Add(in.hudless, kInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(slot.backbuffer.Get(), kSlotState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Add(slot.depth.Get(), kSlotState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Add(slot.motion.Get(), kSlotState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (slot.hasHudless) barriers.Add(slot.hudless.Get(), kSlotState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Record(list);
    CopyRegion(list, slot.backbuffer.Get(), in.backbuffer, f.width, f.height);
    list->CopyResource(slot.depth.Get(), in.depth);
    list->CopyResource(slot.motion.Get(), in.motion);
    if (slot.hasHudless) CopyRegion(list, slot.hudless.Get(), in.hudless, f.hudlessWidth, f.hudlessHeight);
    barriers.Add(in.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, kBackbufferState);
    barriers.Add(in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, kInputState);
    barriers.Add(in.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, kInputState);
    if (slot.hasHudless) barriers.Add(in.hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, kInputState);
    barriers.Add(slot.backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSlotState);
    barriers.Add(slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSlotState);
    barriers.Add(slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSlotState);
    if (slot.hasHudless) barriers.Add(slot.hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSlotState);
    for (uint32_t i = 0; i < count; ++i) {
        barriers.Add(f.outputs[i].Get(), f.outputStates[i], kGenerateState);
        f.outputStates[i] = kGenerateState;
    }
    barriers.Record(list);
    f.batchStartRecord = true;
    Stamp(d, f, list, 1);
    // The frame's constants, as the Vulkan provider takes them (vk_provider.cpp).
    const float mvecX = Number<float>(parameters, "DLSSG.MvecScaleX", 1.0f);
    const float mvecY = Number<float>(parameters, "DLSSG.MvecScaleY", 1.0f);
    const uint32_t motionWidth = uint32_t(in.motionDesc.Width), motionHeight = in.motionDesc.Height;
    const auto [scaleX, scaleY] = xemfg_standalone::NgxDlssgMotionScale(mvecX, mvecY, motionWidth, motionHeight, motionWidth,
                                                                        motionHeight);
    slot.motionScaleX = scaleX;
    slot.motionScaleY = scaleY;
    slot.jitterX = Number<float>(parameters, "DLSSG.JitterOffsetX", 0.0f);
    slot.jitterY = Number<float>(parameters, "DLSSG.JitterOffsetY", 0.0f);
    const float aspect = Number<float>(parameters, "DLSSG.CameraAspectRatio", float(f.width) / float(f.height));
    const float cameraNear = Number<float>(parameters, "DLSSG.CameraNear", 0.0f);
    const float cameraFar = Number<float>(parameters, "DLSSG.CameraFar", 0.0f);
    const float fov = Number<float>(parameters, "DLSSG.CameraFOV", 0.0f);
    xemfg_standalone::Projection(aspect, cameraNear, cameraFar, fov, inverted, slot.projection);
    if (!f.cameraLogged) {
        f.cameraLogged = true;
        D3D12_RESOURCE_DESC real{};
        Log("d3d12_provider_camera", "\"aspect\":" + std::to_string(aspect) + ",\"near\":" + std::to_string(cameraNear) +
            ",\"far\":" + std::to_string(cameraFar) + ",\"fov\":" + std::to_string(fov) + ",\"inverted\":" +
            (inverted ? "true" : "false") + ",\"mvec_param\":[" + std::to_string(mvecX) + "," + std::to_string(mvecY) + "]" +
            ",\"mvecs\":[" + std::to_string(motionWidth) + "," + std::to_string(motionHeight) + "]" +
            ",\"mvec_scale\":[" + std::to_string(scaleX) + "," + std::to_string(scaleY) + "]" +
            ",\"output_real\":" + (Texture(parameters, "DLSSG.OutputReal", real) ? "true" : "false"));
    }
    // XeFG skips a pair whose areas differ, as across a dynamic-resolution step: that batch starts from this frame alone.
    const Slot& prior = f.slots[f.current ^ 1];
    const bool sameAreas = prior.depthRect == slot.depthRect && prior.motionRect == slot.motionRect;
    const Slot& previous = f.havePrevious && !reset && sameAreas ? prior : slot;
    // The batch's call, kept for its later evaluations (the slots it names stay until the next batch).
    f.exec = {};
    DescribeFrame(f, previous, f.exec.previous);
    DescribeFrame(f, slot, f.exec.current);
    f.execOutputs = {};
    f.execBases = {};
    for (uint32_t i = 0; i < count; ++i) f.execOutputs[i] = f.outputs[i].Get();
    f.exec.outputs = f.execOutputs.data();
    f.exec.outputBases = f.execBases.data();
    f.exec.interpolatedFrames = uint8_t(count);
    f.exec.uiComposition = slot.hasHudless ? 1u : 0u;
    f.current ^= 1;
    f.havePrevious = true;
    return RecordOutputs(d, f, list, range);
}

// `source` (back buffer, NON_PIXEL_SHADER_RESOURCE) into `target` (UNORDERED_ACCESS), the top-left region both cover.
void CopyFromBackbuffer(ID3D12GraphicsCommandList* list, ID3D12Resource* target, const D3D12_RESOURCE_DESC& targetDesc,
                        ID3D12Resource* source, const D3D12_RESOURCE_DESC& sourceDesc)
{
    Barriers barriers;
    barriers.Add(source, kBackbufferState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(target, kOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Record(list);
    CopyRegion(list, target, source, uint32_t(std::min(sourceDesc.Width, targetDesc.Width)), std::min(sourceDesc.Height, targetDesc.Height));
    barriers.Add(source, D3D12_RESOURCE_STATE_COPY_SOURCE, kBackbufferState);
    barriers.Add(target, D3D12_RESOURCE_STATE_COPY_DEST, kOutputState);
    barriers.Record(list);
}
}

// ---------------------------------------------------------------- the API's D3D12 entries (xess/ngx_provider.cpp)
int32_t Init(ID3D12Device* device)
{
    if (!device) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    try {
        std::lock_guard lock(gDevice.mutex);
        if (gDevice.reason == "not_initialized") gDevice.reason = "pending";
        const bool ready = EnsureDevice(gDevice, device);
        Log("d3d12_provider_init", std::string("\"ready\":") + (ready ? "true" : "false") +
            (ready ? std::string() : ",\"reason\":\"" + Escape(gDevice.reason) + "\""));
        return Ngx(ready ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t CreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter*, void** feature)
{
    if (!feature) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    *feature = nullptr;
    try {
        ComPtr<ID3D12Device> device;
        if (list) list->GetDevice(IID_PPV_ARGS(&device));
        std::lock_guard lock(gDevice.mutex);
        if (gDevice.reason == "not_initialized") gDevice.reason = "pending";
        if (!EnsureDevice(gDevice, device ? device.Get() : gDevice.device.Get())) {
            Log("d3d12_provider_refused", "\"reason\":\"" + Escape(gDevice.reason) + "\"");
            return Ngx(NVSDK_NGX_Result_FAIL_FeatureNotSupported);
        }
        *feature = new Feature();   // XeFG's context comes with the first evaluation, once the inputs are known
        ++gDevice.liveFeatures;
        return Ngx(NVSDK_NGX_Result_Success);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t Evaluate(ID3D12GraphicsCommandList* list, void* handle, NVSDK_NGX_Parameter* parameters)
{
    if (!list || !handle || !parameters) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    Feature& f = *static_cast<Feature*>(handle);
    try {
        std::lock_guard lock(gDevice.mutex);
        Device& d = gDevice;
        if (!d.ready) return Ngx(NVSDK_NGX_Result_FAIL_NotInitialized);
        const uint32_t count = std::clamp<uint32_t>(Number<unsigned int>(parameters, "DLSSG.MultiFrameCount", 1u), 1u, kMaxGenerated);
        const uint32_t index = std::clamp<uint32_t>(Number<unsigned int>(parameters, "DLSSG.MultiFrameIndex", 1u), 1u, count);
        D3D12_RESOURCE_DESC outputDesc{}, realDesc{}, backbufferDesc{};
        ID3D12Resource* output = Texture(parameters, "DLSSG.OutputInterpolated", outputDesc);
        if (!output) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
        ID3D12Resource* real = Texture(parameters, "DLSSG.OutputReal", realDesc);
        ID3D12Resource* backbuffer = Texture(parameters, "DLSSG.Backbuffer", backbufferDesc);
        ++f.evaluations;
        Stamp(d, f, list, 0);
        // pluginprov.2: DLSSG.EnableInterp 0 pauses generation for this frame (a menu, a loading screen): no XeFG work, the
        // real frame fills the output below, and the next batch starts without a previous frame.
        const bool interpolate = Number<int>(parameters, "DLSSG.EnableInterp", 1) != 0;
        if (!interpolate) {
            f.batchReady = false;
            f.havePrevious = false;
            f.generatedThrough = 0;
            ++f.paused;
        } else if (ngx_provider::NewBatch(index, f.lastIndex, count, f.batchCount, f.batches)) {
            f.batchCount = count;
            f.fault.clear();
            f.generatedThrough = 0;
            f.batchReady = GenerateBatch(d, f, list, parameters, count,
                                         ngx_provider::ExecuteRange(true, index, count, ngx_provider::WholeBatch()));
            ++f.batches;
            xess_mod::NoteProviderBatch(f.batchReady);
            // A failure is logged when its reason changes, and the first 16 times.
            if (!f.batchReady && (++f.failures <= 16 || f.fault != f.lastLoggedFault)) {
                f.lastLoggedFault = f.fault;
                Log("d3d12_provider_batch_failed", "\"reason\":\"" + Escape(f.fault) + "\",\"index\":" + std::to_string(index) +
                    ",\"count\":" + std::to_string(count) + ",\"failures\":" + std::to_string(f.failures));
            } else if (f.batches == 1 || f.batches % 600 == 0)   // also while failing, so a log can't go quiet
                Log("d3d12_provider_batch", "\"batches\":" + std::to_string(f.batches) + ",\"count\":" + std::to_string(count) +
                    ",\"evaluations\":" + std::to_string(f.evaluations) + ",\"real\":" + std::to_string(f.realCopies) +
                    ",\"failures\":" + std::to_string(f.failures) + ",\"real_in_place\":" + std::to_string(f.realInPlace) +
                    ",\"paused\":" + std::to_string(f.paused) +
                    ",\"cuda_launches\":" + std::to_string(xefg_cuda::Snapshot().launches) + ",\"cuda_fallbacks\":" + std::to_string(xefg_cuda::Snapshot().fallbacks) +
                    TimingFields(d, f) +
                    (f.batchReady ? std::string(",\"ready\":true") : ",\"ready\":false,\"fault\":\"" + Escape(f.fault) + "\""));
        } else if (f.batchReady && index > f.generatedThrough) {
            // pluginprov.2: a later evaluation of the batch records its own output.
            const auto range = ngx_provider::ExecuteRange(false, index, f.batchCount, ngx_provider::WholeBatch());
            if (range.record && f.context) {
                Stamp(d, f, list, 1);
                if (!RecordOutputs(d, f, list, range) && (++f.failures <= 16 || f.fault != f.lastLoggedFault)) {
                    f.lastLoggedFault = f.fault;
                    Log("d3d12_provider_output_failed", "\"reason\":\"" + Escape(f.fault) + "\",\"index\":" + std::to_string(index) +
                        ",\"count\":" + std::to_string(f.batchCount) + ",\"failures\":" + std::to_string(f.failures));
                }
            }
        }
        f.lastIndex = index;
        const bool copied = f.batchReady && index <= f.generatedThrough && index <= f.batchCount && f.outputs[index - 1] &&
                            ngx_provider::SameColorGroup(outputDesc.Format, f.format);
        if (copied) {
            Barriers barriers;
            barriers.Add(output, kOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
            barriers.Record(list);
            CopyRegion(list, output, f.outputs[index - 1].Get(), std::min(uint32_t(outputDesc.Width), f.width),
                       std::min(outputDesc.Height, f.height));
            barriers.Add(output, D3D12_RESOURCE_STATE_COPY_DEST, kOutputState);
            barriers.Record(list);
        } else if (backbuffer && backbuffer != output && ngx_provider::SameColorGroup(outputDesc.Format, backbufferDesc.Format)) {
            // No generated frame for this index: the real frame takes its place, never the output's stale content.
            CopyFromBackbuffer(list, output, outputDesc, backbuffer, backbufferDesc);
            ++f.realInPlace;
        }
        // Streamline presents DLSSG.OutputReal as the real frame after the generated ones (vkprov.3).
        if (real && backbuffer && real != backbuffer) {
            if (ngx_provider::SameColorGroup(realDesc.Format, backbufferDesc.Format)) {
                CopyFromBackbuffer(list, real, realDesc, backbuffer, backbufferDesc);
                ++f.realCopies;
            } else if (!f.realSkippedLogged) {
                f.realSkippedLogged = true;
                Log("d3d12_provider_real_skipped", "\"backbuffer_format\":" + std::to_string(int(backbufferDesc.Format)) +
                    ",\"real_format\":" + std::to_string(int(realDesc.Format)));
            }
        }
        Stamp(d, f, list, 2);
        return Ngx(copied || !interpolate ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t ReleaseFeature(void* handle)
{
    if (!handle) return Ngx(NVSDK_NGX_Result_FAIL_InvalidParameter);
    auto* f = static_cast<Feature*>(handle);
    try {
        std::lock_guard lock(gDevice.mutex);
        Retire(TakeResources(gDevice, *f));
        delete f;
        if (gDevice.liveFeatures) --gDevice.liveFeatures;
        return Ngx(NVSDK_NGX_Result_Success);
    } catch (...) {
        return Ngx(NVSDK_NGX_Result_Fail);
    }
}

int32_t Shutdown()
{
    return Ngx(NVSDK_NGX_Result_Success);
}
}
