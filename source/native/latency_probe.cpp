// latmon.1: the latency monitor and its optional file (latency_probe.h).
#include "latency_probe.h"
#include "latency_probe_ring.h"
#include "latency_monitor.h"
#include "diagnostic_paths.h"
#include "single_module.h"
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>

namespace latency_probe
{
namespace
{
// About 30 s of events at 1000/s; the drain empties it every 100 ms.
constexpr size_t kCapacity = size_t(1) << 15;
// A long session stops writing here (about 4-5 hours at 6x / 120 Hz); the log says so.
constexpr uint64_t kFileLimit = uint64_t(512) << 20;
// The log gets the readout this often.
constexpr uint64_t kLogEveryMs = 60000;
using Queue = Ring<kCapacity>;

struct Switches
{
    bool available = true; // [Overlay] LatencyMonitor
    bool file = false;     // [Diagnostics] LatencyProbe
};

// RTX40MFG-UI.ini beside the game, as [Overlay] MenuOnOutputFrames.
const Switches& ReadSwitches() noexcept
{
    static const Switches switches = []() noexcept {
        Switches read{};
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
        if (!slash || wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"RTX40MFG-UI.ini"))
            return read;
        read.available = GetPrivateProfileIntW(L"Overlay", L"LatencyMonitor", 1, path) != 0;
        read.file = read.available && GetPrivateProfileIntW(L"Diagnostics", L"LatencyProbe", 0, path) == 1;
        return read;
    }();
    return switches;
}

int64_t Frequency() noexcept
{
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? value.QuadPart : int64_t(0);
    }();
    return frequency;
}

std::atomic<Queue*> gQueue{nullptr};
std::atomic<bool> gRecording{false};
// The backend's loop only.
latency_monitor::Estimator* gEstimator = nullptr;
bool gAllocationFailed = false;
uint64_t gNextLog = 0;
uint64_t gResyncs = 0;
// The published readout.
std::mutex gLiveMutex;
Live gLive{};

void Push(Kind kind, int64_t qpc, uint16_t sub, uint64_t a, uint64_t b, uint64_t c, uint64_t d) noexcept
{
    if (!gRecording.load(std::memory_order_acquire)) return;
    Queue* const queue = gQueue.load(std::memory_order_acquire);
    if (!queue) return;
    Event event{};
    event.qpc = qpc;
    event.thread = GetCurrentThreadId();
    event.kind = kind;
    event.sub = sub;
    event.a = a; event.b = b; event.c = c; event.d = d;
    queue->Push(event);
}

uint64_t SyncAndFlags(UINT sync, UINT flags) noexcept
{
    return (uint64_t(sync) << 32) | flags;
}

// The file's state: the backend's loop is its only thread.
struct Writer
{
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t bytes = 0;
    bool opened = false, failed = false, stopped = false;
    char buffer[1 << 16]{};
    size_t used = 0;
};
Writer gWriter;

std::wstring FilePath()
{
    const auto& directory = diagnostic_paths::LogDirectory();
    if (directory.empty()) return {};
    wchar_t suffix[48]{};
    swprintf_s(suffix, L"-latency-%lu.csv", GetCurrentProcessId());
    const auto name = diagnostic_paths::FileName(diagnostic_paths::Executable(), suffix);
    return name.empty() ? std::wstring{} : directory + name;
}

bool WriteRaw(const char* data, size_t size) noexcept
{
    if (gWriter.file == INVALID_HANDLE_VALUE || gWriter.failed) return false;
    while (size)
    {
        DWORD written = 0;
        const DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
        if (!WriteFile(gWriter.file, data, chunk, &written, nullptr) || !written)
        {
            gWriter.failed = true;
            single_module::Log(L"Latency probe: writing its file failed; the probe stops recording");
            return false;
        }
        gWriter.bytes += written;
        data += written;
        size -= written;
    }
    return true;
}

void FlushBuffer() noexcept
{
    if (gWriter.used) WriteRaw(gWriter.buffer, gWriter.used);
    gWriter.used = 0;
}

void Append(const Event& event) noexcept
{
    if (gWriter.failed || gWriter.stopped || gWriter.file == INVALID_HANDLE_VALUE) return;
    int length = Format(event, gWriter.buffer + gWriter.used, sizeof(gWriter.buffer) - gWriter.used);
    if (!length)
    {
        FlushBuffer();
        length = Format(event, gWriter.buffer, sizeof(gWriter.buffer));
    }
    gWriter.used += static_cast<size_t>(length);
}

void Open() noexcept
{
    gWriter.opened = true;
    std::wstring path;
    try { path = FilePath(); } catch (...) { path.clear(); }
    if (path.empty())
    {
        gWriter.failed = true;
        single_module::Log(L"Latency probe: no log directory; the probe stops recording");
        return;
    }
    gWriter.file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gWriter.file == INVALID_HANDLE_VALUE)
    {
        gWriter.failed = true;
        wchar_t line[MAX_PATH + 96]{};
        swprintf_s(line, L"Latency probe: cannot create %s (error %lu); the probe stops recording", path.c_str(),
            GetLastError());
        single_module::Log(line);
        return;
    }
    // The clock pair lets the analyzer place PresentMon's rows (or wall-clock notes) on this file's QPC timeline.
    LARGE_INTEGER counter{};
    FILETIME now{};
    QueryPerformanceCounter(&counter);
    GetSystemTimePreciseAsFileTime(&now);
    char header[512]{};
    const int length = std::snprintf(header, sizeof(header),
        "# latmon.1 latency probe (source/native/latency_probe_ring.h has the columns; tests/analyze_latency_probe.py reads it)\n"
        "# qpc_frequency=%lld pid=%lu open_qpc=%lld open_filetime=%llu\n%s",
        static_cast<long long>(Frequency()), GetCurrentProcessId(), static_cast<long long>(counter.QuadPart),
        (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime, kHeader);
    if (length > 0 && size_t(length) < sizeof(header)) WriteRaw(header, size_t(length));
    wchar_t line[MAX_PATH + 64]{};
    swprintf_s(line, L"Latency probe: recording to %s", path.c_str());
    single_module::Log(line);
}

uint32_t Microseconds(int64_t ticks) noexcept
{
    const int64_t frequency = Frequency();
    if (ticks <= 0 || frequency <= 0) return 0;
    const int64_t us = ticks * 1'000'000 / frequency;
    return us > int64_t(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(us);
}

void Publish(const Live& live) noexcept
{
    std::lock_guard lock(gLiveMutex);
    gLive = live;
}

// latmon.3: always, while the monitor is available, so the readout is live the moment it is shown.
bool Wanted(const Switches& switches) noexcept
{
    return switches.available;
}

void LogReadout(const latency_monitor::Stats& stats) noexcept
{
    const uint64_t tick = GetTickCount64();
    if (tick < gNextLog) return;
    gNextLog = tick + kLogEveryMs;
    const auto counters = gEstimator->ReadCounters();
    // latmon.4: implausible frames by reason (latency_monitor.h Counters), flips clamped to their Present, whether the
    // means are over the last 5 s (widened=1: under 5 frames in the last second), resyncs after queue overflows.
    wchar_t line[640]{};
    swprintf_s(line, L"Latency monitor: state=%u frames=%u widened=%u mean=%.1f ms 1%%high=%.1f ms real=%.1f ms "
        L"game=%.1f fg=%.1f display=%.1f ms exact=%u/1000 measured=%llu no_start=%llu no_flip=%llu no_outputs=%llu "
        L"implausible=%llu (before_start=%llu before_present=%llu too_long=%llu) clamped=%llu between=%llu "
        L"inferred=%llu unassigned=%llu overwritten=%llu resyncs=%llu",
        static_cast<uint32_t>(stats.state), stats.frames, stats.widened ? 1u : 0u, Microseconds(stats.meanFirst) / 1000.0,
        Microseconds(stats.p99First) / 1000.0, Microseconds(stats.meanReal) / 1000.0,
        Microseconds(stats.meanGame) / 1000.0, Microseconds(stats.meanFg) / 1000.0,
        Microseconds(stats.meanDisplay) / 1000.0, stats.exactPermille,
        static_cast<unsigned long long>(counters.frames), static_cast<unsigned long long>(counters.noStart),
        static_cast<unsigned long long>(counters.noFlip), static_cast<unsigned long long>(counters.noOutputs),
        static_cast<unsigned long long>(counters.implausible), static_cast<unsigned long long>(counters.beforeStart),
        static_cast<unsigned long long>(counters.beforePresent), static_cast<unsigned long long>(counters.tooLong),
        static_cast<unsigned long long>(counters.clamped), static_cast<unsigned long long>(counters.interpolated),
        static_cast<unsigned long long>(counters.inferred), static_cast<unsigned long long>(counters.unassigned),
        static_cast<unsigned long long>(counters.overwritten), static_cast<unsigned long long>(gResyncs));
    single_module::Log(line);
}
}

void Start() noexcept
{
    const Switches& switches = ReadSwitches();
    if (!switches.available)
        single_module::Log(L"Latency monitor off ([Overlay] LatencyMonitor=0): no marker wrap, no Latency row");
    else if (switches.file)
        single_module::Log(L"Latency probe on ([Diagnostics] LatencyProbe=1): markers, hand-overs, output flips");
    Live live{};
    live.available = switches.available;
    Publish(live);
}

bool Available() noexcept
{
    return ReadSwitches().available;
}

bool Recording() noexcept
{
    return gRecording.load(std::memory_order_acquire);
}

int64_t Now() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

void Marker(uint32_t marker, uint64_t frame, uint32_t source) noexcept
{
    if (!Recording()) return;
    Push(Kind::eMarker, Now(), static_cast<uint16_t>(marker > 0xFFFF ? 0xFFFF : marker), frame, source, 0, 0);
}

void GamePresent(const void* chain, int64_t entry, int64_t returned, HRESULT result, UINT sync, UINT flags,
    uint16_t role) noexcept
{
    Push(Kind::eGamePresent, entry, role, uint64_t(returned), uint32_t(result), uint64_t(uintptr_t(chain)),
        SyncAndFlags(sync, flags));
}

void OutputPresent(const void* chain, IDXGISwapChain* native, int64_t entry, int64_t returned, HRESULT result,
    UINT sync, UINT flags, uint16_t role) noexcept
{
    if (!Recording()) return;
    Push(Kind::eOutputPresent, entry, role, uint64_t(returned), uint32_t(result), uint64_t(uintptr_t(chain)),
        SyncAndFlags(sync, flags));
    if (!native) return;
    uint16_t status = 0;
    UINT count = 0;
    if (SUCCEEDED(native->GetLastPresentCount(&count))) status |= kStatsCount;
    DXGI_FRAME_STATISTICS stats{};
    const HRESULT statsResult = native->GetFrameStatistics(&stats);
    if (SUCCEEDED(statsResult)) status |= kStatsFrame;
    else if (statsResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) status |= kStatsDisjoint;
    const bool frame = (status & kStatsFrame) != 0;
    Push(Kind::eFrameStats, Now(), status, (uint64_t(count) << 32) | (frame ? stats.PresentCount : 0u),
        frame ? stats.PresentRefreshCount : 0u, frame ? stats.SyncRefreshCount : 0u,
        frame ? uint64_t(stats.SyncQPCTime.QuadPart) : 0u);
}

void XefgOutput(uint64_t frame, bool generated) noexcept
{
    if (!Recording()) return;
    Push(Kind::eXefgOutput, Now(), generated ? 1 : 0, frame, 0, 0, 0);
}

void DlssgState(uint32_t presented, uint32_t status) noexcept
{
    if (!Recording()) return;
    Push(Kind::eDlssgState, Now(), 0, presented, status, 0, 0);
}

void Flush() noexcept
{
    const Switches& switches = ReadSwitches();
    if (!switches.available || gAllocationFailed) return;
    const bool wanted = Wanted(switches);
    Queue* queue = gQueue.load(std::memory_order_acquire);
    if (wanted && !queue)
    {
        // First use: the queue (2 MB) and the estimator, here rather than on a presenting thread.
        queue = new (std::nothrow) Queue();
        gEstimator = new (std::nothrow) latency_monitor::Estimator();
        if (!queue || !gEstimator)
        {
            delete queue;
            delete gEstimator;
            queue = nullptr;
            gEstimator = nullptr;
            gAllocationFailed = true;
            single_module::Log(L"Latency monitor: out of memory for its queue; it stays off");
            return;
        }
        gEstimator->SetFrequency(Frequency());
        gQueue.store(queue, std::memory_order_release);
    }
    if (!queue) return;
    if (wanted != gRecording.load(std::memory_order_relaxed))
    {
        // A new measurement starts from nothing: frames from before a pause are not mixed in.
        if (wanted) { gEstimator->Reset(); gNextLog = GetTickCount64() + 5000; }
        gRecording.store(wanted, std::memory_order_release);
    }
    if (switches.file && !gWriter.opened) Open();
    if (const uint64_t dropped = queue->TakeDropped())
    {
        Event note{};
        note.qpc = Now();
        note.thread = GetCurrentThreadId();
        note.kind = Kind::eDropped;
        note.a = dropped;
        if (switches.file) Append(note);
        // A gap in the events can pair frames wrongly: the frames still being paired go, the measured ones stay.
        if (wanted) { gEstimator->Resync(); ++gResyncs; }
    }
    Event event{};
    while (queue->Pop(event))
    {
        if (wanted) gEstimator->Feed(event);
        if (switches.file) Append(event);
    }
    if (switches.file)
    {
        FlushBuffer();
        if (!gWriter.stopped && !gWriter.failed && gWriter.bytes >= kFileLimit)
        {
            gWriter.stopped = true;
            static constexpr char kStopped[] = "# stopped: the file reached its size limit\n";
            WriteRaw(kStopped, sizeof(kStopped) - 1);
            single_module::Log(L"Latency probe: its file reached 512 MB; recording stopped");
        }
    }
    Live live{};
    live.available = true;
    if (wanted)
    {
        const int64_t now = Now();
        const auto stats = gEstimator->Read(now, Frequency());
        live.state = static_cast<uint32_t>(stats.state);
        live.frames = stats.frames;
        live.exactPermille = stats.exactPermille;
        live.meanUs = Microseconds(stats.meanFirst);
        live.p99Us = Microseconds(stats.p99First);
        live.realUs = Microseconds(stats.meanReal);
        live.gameUs = Microseconds(stats.meanGame);
        live.fgUs = Microseconds(stats.meanFg);
        live.displayUs = Microseconds(stats.meanDisplay);
        if (stats.newest && Frequency() > 0)
            live.ageMs = static_cast<uint64_t>(std::max<int64_t>(0, now - stats.newest) * 1000 / Frequency());
        LogReadout(stats);
    }
    Publish(live);
}

Live ReadLive() noexcept
{
    std::lock_guard lock(gLiveMutex);
    return gLive;
}

std::string StatusFragment()
{
    const Live live = ReadLive();
    char text[512]{};
    std::snprintf(text, sizeof(text),
        ",\"latencyAvailable\":%s,\"latencyState\":%u,\"latencyFrames\":%u,\"latencyExactPermille\":%u,"
        "\"latencyMeanUs\":%u,\"latencyP99Us\":%u,\"latencyRealUs\":%u,\"latencyGameUs\":%u,\"latencyFgUs\":%u,"
        "\"latencyDisplayUs\":%u,\"latencyAgeMs\":%llu",
        live.available ? "true" : "false", live.state, live.frames, live.exactPermille, live.meanUs, live.p99Us,
        live.realUs, live.gameUs, live.fgUs, live.displayUs,
        static_cast<unsigned long long>(std::min<uint64_t>(live.ageMs, UINT32_MAX)));
    return text;
}
}
