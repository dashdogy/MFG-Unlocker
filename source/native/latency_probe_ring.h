#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// latmon.1: the latency probe's event record, its bounded queue and its CSV row, without Windows calls (tested in
// tests/xemfg_standalone). latency_probe.cpp owns one Ring and drains it to a file; tests/analyze_latency_probe.py
// reads that file. Every timestamp is a QueryPerformanceCounter value.
namespace latency_probe
{
enum class Kind : uint8_t
{
    // The game's latency marker, stamped before the call is forwarded. sub: the marker (sl::PCLMarker: 0 simulation
    // start, 4 present start, 13 controller input sample, ...); a: the frame token; b: Source.
    eMarker = 1,
    // The game's Present on its own swap chain: frame generation's hand-over of a real frame. qpc: entry; a: return;
    // b: the HRESULT; c: the chain (our proxy's address); d: sync interval << 32 | flags; sub: Role bits.
    eGamePresent = 2,
    // One output Present on the output layer (DLSS-G's chain beneath Streamline, XeFG's native output chain); fields as
    // eGamePresent. An eFrameStats from the same thread follows it.
    eOutputPresent = 3,
    // The output chain's native swap chain right after that Present. sub: StatsFlags; a: GetLastPresentCount << 32 |
    // DXGI_FRAME_STATISTICS::PresentCount; b: PresentRefreshCount; c: SyncRefreshCount; d: SyncQPCTime.
    eFrameStats = 4,
    // XeFG's label for its next native Present on this thread. a: the frame id; sub: 1 generated, 0 the real frame.
    eXefgOutput = 5,
    // A DLSS-G state query's result. a: numFramesActuallyPresented; b: the status.
    eDlssgState = 6,
    // Written by the drain: a: the events a full queue refused since the previous one.
    eDropped = 7,
};

enum Source : uint32_t
{
    kSourcePcl = 1,           // slPCLSetMarker
    kSourceReflexLegacy = 2,  // slReflexSetMarker (Streamline 2.0-2.4)
};

enum Role : uint16_t
{
    kRoleStreamlineBase = 1u << 0, // DLSS-G's chain beneath Streamline (menuout.1)
    kRoleXefgOutput = 1u << 1,     // XeFG's native output chain (OptiScaler or XeMFG standalone)
    kRoleXemfgHost = 1u << 2,      // the game's chain over XeMFG standalone's XeFG proxy
    kRoleProvider = 1u << 3,       // the game's chain over an XeSS provider chain
    kRoleD3D11 = 1u << 4,
    kRoleNested = 1u << 5,         // a Present inside another Present of the same chain on this thread
};

enum StatsFlags : uint16_t
{
    kStatsCount = 1u << 0,    // GetLastPresentCount succeeded
    kStatsFrame = 1u << 1,    // GetFrameStatistics succeeded
    kStatsDisjoint = 1u << 2, // GetFrameStatistics returned DXGI_ERROR_FRAME_STATISTICS_DISJOINT
};

struct Event
{
    int64_t qpc;
    uint32_t thread;
    Kind kind;
    uint8_t reserved;
    uint16_t sub;
    uint64_t a, b, c, d;
};
static_assert(sizeof(Event) == 48);

// A bounded multi-producer, single-consumer queue (sequence-numbered cells). Push never blocks or allocates; a full
// queue refuses the event and counts it. Pop is for one draining thread. A cell is one 64-byte line; the producers'
// and the consumer's indices are padded apart (no alignas, so no padding warning at /W4).
template <size_t Capacity>
class Ring
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    struct Cell
    {
        std::atomic<uint64_t> sequence;
        Event event;
        uint64_t padding;
    };
    static_assert(sizeof(Cell) == 64);

public:
    Ring() noexcept
    {
        for (size_t i = 0; i < Capacity; ++i) cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    bool Push(const Event& event) noexcept
    {
        uint64_t position = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = cells_[position & (Capacity - 1)];
            const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const int64_t difference = int64_t(sequence) - int64_t(position);
            if (difference == 0)
            {
                if (tail_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                {
                    cell.event = event;
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (difference < 0)
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            else
                position = tail_.load(std::memory_order_relaxed);
        }
    }

    bool Pop(Event& event) noexcept
    {
        Cell& cell = cells_[head_ & (Capacity - 1)];
        const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
        if (int64_t(sequence) - int64_t(head_ + 1) < 0) return false;
        event = cell.event;
        cell.sequence.store(head_ + Capacity, std::memory_order_release);
        ++head_;
        return true;
    }

    uint64_t TakeDropped() noexcept { return dropped_.exchange(0, std::memory_order_relaxed); }

private:
    std::array<Cell, Capacity> cells_;
    std::atomic<uint64_t> tail_{0};
    std::atomic<uint64_t> dropped_{0};
    uint64_t tailPadding_[6]{};
    uint64_t head_{0};
};

inline const char* KindName(Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::eMarker: return "marker";
    case Kind::eGamePresent: return "game_present";
    case Kind::eOutputPresent: return "output_present";
    case Kind::eFrameStats: return "frame_stats";
    case Kind::eXefgOutput: return "xefg_output";
    case Kind::eDlssgState: return "dlssg_state";
    case Kind::eDropped: return "dropped";
    }
    return "unknown";
}

inline constexpr const char* kHeader = "kind,qpc,thread,sub,a,b,c,d\n";

// One CSV row (kHeader's columns); the length written, or 0 when `size` is too small.
inline int Format(const Event& event, char* out, size_t size) noexcept
{
    if (!out || !size) return 0;
    const int length = std::snprintf(out, size, "%s,%lld,%u,%u,%llu,%llu,%llu,%llu\n", KindName(event.kind),
        static_cast<long long>(event.qpc), event.thread, static_cast<unsigned>(event.sub),
        static_cast<unsigned long long>(event.a), static_cast<unsigned long long>(event.b),
        static_cast<unsigned long long>(event.c), static_cast<unsigned long long>(event.d));
    return length > 0 && size_t(length) < size ? length : 0;
}
}
