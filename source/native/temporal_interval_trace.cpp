#include "build_variant.h"
#include "gpu_dispatch.h"
#include "temporal_interval_trace.h"
#include "diagnostic_paths.h"

#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <share.h>
#include <vector>

namespace temporal_interval_trace
{
namespace
{
constexpr size_t kEventPoolSize = 4096;

struct Event
{
    uint64_t sequence = 0;
    int64_t qpc = 0;
    uint32_t threadId = 0;
    uintptr_t handle = 0;
    uintptr_t output = 0;
    uint64_t frameId = 0;
    int32_t count = 0;
    int32_t index = 0;
    int32_t maximum = 0;
    uint32_t countResult = 0;
    uint32_t indexResult = 0;
    uint32_t maximumResult = 0;
    uint32_t frameIdResult = 0;
    uint32_t outputResult = 0;
    bool descriptorReady = false;
    bool readException = false;
};

struct alignas(MEMORY_ALLOCATION_ALIGNMENT) EventNode
{
    SLIST_ENTRY link{};
    Event event{};
};

static_assert(alignof(EventNode) >= MEMORY_ALLOCATION_ALIGNMENT);

std::array<EventNode, kEventPoolSize> gEventPool{};
SLIST_HEADER gFreeEvents{};
SLIST_HEADER gPendingEvents{};
std::atomic<uint32_t> gInitializationState{0};
std::atomic<bool> gEnabled{false};
// GitHub issue 122: the FPS readout needs only the counters below; the CSV (one row per real frame, unbounded
// within a session) is an opt-in diagnostic, read once at Initialize.
std::atomic<bool> gFileEnabled{false};
std::atomic<bool> gLogReady{false};
std::atomic<uint64_t> gSequence{0};
std::atomic<uint64_t> gValidSamples{0};
std::atomic<uint64_t> gInvalidSamples{0};
std::atomic<uint64_t> gDroppedSamples{0};
std::atomic<uint32_t> gSeenCountMask{0};
std::atomic<uint32_t> gSeenIndexMask{0};
std::atomic<int32_t> gLastCount{0};
std::atomic<int32_t> gLastIndex{0};
std::atomic<uint32_t> gLastPositionNumerator{0};
std::atomic<uint32_t> gLastPositionDenominator{0};
FirstSampleTable<kFirstSampleHandleCapacity> gFirstSamples;
LARGE_INTEGER gQpcFrequency{};
int64_t gLastWrittenQpc = 0;
FILE* gTrace = nullptr;
wchar_t gFileName[160]{};
wchar_t gFilePath[32768]{};

bool FullyInitialized() noexcept
{
    return gInitializationState.load(std::memory_order_acquire) == 2;
}

void ReadParameters(const NVSDK_NGX_Parameter* parameters,
    Event& event) noexcept
{
    if (!parameters)
        return;
    __try
    {
        event.countResult = static_cast<uint32_t>(
            parameters->Get("DLSSG.MultiFrameCount", &event.count));
        event.indexResult = static_cast<uint32_t>(
            parameters->Get("DLSSG.MultiFrameIndex", &event.index));
        event.maximumResult = static_cast<uint32_t>(
            parameters->Get("DLSSG.MultiFrameCountMax", &event.maximum));
        if (gpu_dispatch::UsesTranslatedProgram())
        {
            void* frame = nullptr;
            void* output = nullptr;
            event.frameIdResult = static_cast<uint32_t>(
                parameters->Get("DLSSG.BackbufferFrameID", &frame));
            event.frameId = reinterpret_cast<uintptr_t>(frame);
            event.outputResult = static_cast<uint32_t>(
                parameters->Get("DLSSG.OutputInterpolated", &output));
            event.output = reinterpret_cast<uintptr_t>(output);
        }
        else
        {
            event.frameIdResult = static_cast<uint32_t>(
                parameters->Get("DLSSG.BackbufferFrameID", &event.frameId));
            ID3D12Resource* output = nullptr;
            event.outputResult = static_cast<uint32_t>(
                parameters->Get("DLSSG.OutputInterpolated", &output));
            event.output = reinterpret_cast<uintptr_t>(output);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        event.readException = true;
    }
}

bool ValidTemporalSample(const Event& event) noexcept
{
    return !event.readException
        && event.countResult == NVSDK_NGX_Result_Success
        && event.indexResult == NVSDK_NGX_Result_Success
        && event.count >= 1 && event.count <= 5
        && event.index >= 1 && event.index <= event.count;
}

void RecordFirstSample(uintptr_t handle) noexcept
{
    gFirstSamples.Record(handle);
}

bool OpenTrace() noexcept
{
    if (gTrace)
        return true;
    if (!FullyInitialized() || !gFilePath[0])
        return false;
    gTrace = _wfsopen(gFilePath, L"w", _SH_DENYWR);
    if (!gTrace)
        return false;
    setvbuf(gTrace, nullptr, _IOFBF, 64 * 1024);
    std::fputs(
        "# NGX temporal-request trace. requestedPosition=index/(count+1). "
        "normalizedInterval=1/(count+1). submitDeltaUs is CPU time between "
        "Evaluate entries, not final-display timing or proof of unique images.\n",
        gTrace);
    std::fputs(
        "sequence,qpc,submitDeltaUs,threadId,handle,frameIdResult,frameId,"
        "countResult,count,indexResult,index,maximumResult,maximum,"
        "requestedPosition,normalizedInterval,outputResult,output,"
        "descriptorReady,readException\n",
        gTrace);
    std::fflush(gTrace);
    gLogReady.store(true, std::memory_order_release);
    return true;
}

void ReturnNode(EventNode* node) noexcept
{
    if (node)
        InterlockedPushEntrySList(&gFreeEvents, &node->link);
}

bool RecordImpl(const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, bool descriptorReady,
    bool requireValidTemporalSample) noexcept
{
    if (!Enabled() || !FullyInitialized())
        return false;

    Event event{};
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    event.qpc = qpc.QuadPart;
    event.threadId = GetCurrentThreadId();
    event.handle = reinterpret_cast<uintptr_t>(handle);
    event.descriptorReady = descriptorReady;
    ReadParameters(parameters, event);

    const bool valid = ValidTemporalSample(event);
    if (requireValidTemporalSample && !valid)
        return false;

    event.sequence = gSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (valid)
    {
        gValidSamples.fetch_add(1, std::memory_order_relaxed);
        if (event.index == 1)
            RecordFirstSample(event.handle);
        gSeenCountMask.fetch_or(1u << static_cast<uint32_t>(event.count),
            std::memory_order_relaxed);
        gSeenIndexMask.fetch_or(1u << static_cast<uint32_t>(event.index - 1),
            std::memory_order_relaxed);
        gLastCount.store(event.count, std::memory_order_relaxed);
        gLastIndex.store(event.index, std::memory_order_relaxed);
        gLastPositionNumerator.store(
            static_cast<uint32_t>(event.index), std::memory_order_relaxed);
        gLastPositionDenominator.store(
            static_cast<uint32_t>(event.count + 1), std::memory_order_release);
    }
    else
    {
        gInvalidSamples.fetch_add(1, std::memory_order_relaxed);
    }

    if (!gFileEnabled.load(std::memory_order_acquire))
        return valid;
    PSLIST_ENTRY entry = InterlockedPopEntrySList(&gFreeEvents);
    if (!entry)
    {
        gDroppedSamples.fetch_add(1, std::memory_order_relaxed);
        return valid;
    }
    auto* node = CONTAINING_RECORD(entry, EventNode, link);
    node->event = event;
    InterlockedPushEntrySList(&gPendingEvents, &node->link);
    return valid;
}
}

void Initialize(const wchar_t* tempDirectory, const wchar_t* executablePath) noexcept
{
    uint32_t expected = 0;
    if (!gInitializationState.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel))
        return;

    InitializeSListHead(&gFreeEvents);
    InitializeSListHead(&gPendingEvents);
    for (auto& node : gEventPool)
        InterlockedPushEntrySList(&gFreeEvents, &node.link);
    QueryPerformanceFrequency(&gQpcFrequency);

    const auto name=diagnostic_paths::FileName(executablePath?executablePath:L"",L"-intervals.csv");
    wcsncpy_s(gFileName,name.c_str(),_TRUNCATE);
    if (tempDirectory && *tempDirectory)
    {
        wcsncpy_s(gFilePath, tempDirectory, _TRUNCATE);
        const size_t length = std::wcslen(gFilePath);
        if (length && gFilePath[length - 1] != L'\\'
            && gFilePath[length - 1] != L'/')
        {
            wcscat_s(gFilePath, L"\\");
        }
        wcscat_s(gFilePath, gFileName);
    }
    wchar_t file[2]{};
    gFileEnabled.store(GetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_INTERVAL_TRACE", file, 2) == 1 && file[0] == L'1',
        std::memory_order_release);
    gInitializationState.store(2, std::memory_order_release);
}

void SetEnabled(bool enabled) noexcept
{
    gEnabled.store(enabled, std::memory_order_release);
}

bool Enabled() noexcept
{
    return gEnabled.load(std::memory_order_acquire);
}

bool FileEnabled() noexcept
{
    return gFileEnabled.load(std::memory_order_acquire);
}

void Record(const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, bool descriptorReady) noexcept
{
    (void)RecordImpl(handle, parameters, descriptorReady, false);
}

bool RecordIfValidTemporalSample(const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, bool descriptorReady) noexcept
{
    return RecordImpl(handle, parameters, descriptorReady, true);
}

void Flush() noexcept
{
    if (!FullyInitialized())
        return;
    if (Enabled() && FileEnabled())
        OpenTrace();

    PSLIST_ENTRY list = InterlockedFlushSList(&gPendingEvents);
    if (!list)
    {
        if (gTrace)
            std::fflush(gTrace);
        return;
    }

    std::vector<EventNode*> nodes;
    nodes.reserve(256);
    while (list)
    {
        PSLIST_ENTRY next = list->Next;
        nodes.push_back(CONTAINING_RECORD(list, EventNode, link));
        list = next;
    }
    std::sort(nodes.begin(), nodes.end(), [](const EventNode* left,
        const EventNode* right) {
        return left->event.sequence < right->event.sequence;
    });

    if (!OpenTrace())
    {
        gDroppedSamples.fetch_add(nodes.size(), std::memory_order_relaxed);
        for (EventNode* node : nodes)
            ReturnNode(node);
        return;
    }

    for (EventNode* node : nodes)
    {
        const Event& event = node->event;
        const bool valid = ValidTemporalSample(event);
        const double position = valid
            ? static_cast<double>(event.index)
                / static_cast<double>(event.count + 1)
            : -1.0;
        const double interval = valid
            ? 1.0 / static_cast<double>(event.count + 1)
            : -1.0;
        const double deltaMicroseconds = gLastWrittenQpc > 0
                && event.qpc >= gLastWrittenQpc && gQpcFrequency.QuadPart > 0
            ? static_cast<double>(event.qpc - gLastWrittenQpc) * 1000000.0
                / static_cast<double>(gQpcFrequency.QuadPart)
            : -1.0;
        std::fprintf(gTrace,
            "%llu,%lld,%.3f,%u,0x%llX,0x%08X,%llu,0x%08X,%d,"
            "0x%08X,%d,0x%08X,%d,%.9f,%.9f,0x%08X,0x%llX,%d,%d\n",
            static_cast<unsigned long long>(event.sequence),
            static_cast<long long>(event.qpc), deltaMicroseconds,
            event.threadId,
            static_cast<unsigned long long>(event.handle),
            event.frameIdResult,
            static_cast<unsigned long long>(event.frameId),
            event.countResult, event.count,
            event.indexResult, event.index,
            event.maximumResult, event.maximum,
            position, interval, event.outputResult,
            static_cast<unsigned long long>(event.output),
            event.descriptorReady ? 1 : 0,
            event.readException ? 1 : 0);
        gLastWrittenQpc = event.qpc;
        ReturnNode(node);
    }
    std::fflush(gTrace);
}

Snapshot ReadSnapshot() noexcept
{
    Snapshot snapshot{};
    snapshot.initialized = FullyInitialized();
    snapshot.enabled = Enabled();
    snapshot.logReady = gLogReady.load(std::memory_order_acquire);
    snapshot.validSamples = gValidSamples.load(std::memory_order_relaxed);
    snapshot.invalidSamples = gInvalidSamples.load(std::memory_order_relaxed);
    snapshot.droppedSamples = gDroppedSamples.load(std::memory_order_relaxed);
    snapshot.seenCountMask = gSeenCountMask.load(std::memory_order_relaxed);
    snapshot.seenIndexMask = gSeenIndexMask.load(std::memory_order_relaxed);
    snapshot.lastCount = gLastCount.load(std::memory_order_relaxed);
    snapshot.lastIndex = gLastIndex.load(std::memory_order_relaxed);
    snapshot.lastPositionNumerator =
        gLastPositionNumerator.load(std::memory_order_relaxed);
    snapshot.lastPositionDenominator =
        gLastPositionDenominator.load(std::memory_order_acquire);
    for (size_t index = 0; index < kFirstSampleHandleCapacity; ++index)
    {
        snapshot.firstSampleCounters[index].handle = gFirstSamples.Handle(index);
        snapshot.firstSampleCounters[index].samples = gFirstSamples.Samples(index);
    }
    return snapshot;
}

const wchar_t* FileName() noexcept
{
    return gFileName;
}

const wchar_t* FilePath() noexcept
{
    return gFilePath;
}
}
