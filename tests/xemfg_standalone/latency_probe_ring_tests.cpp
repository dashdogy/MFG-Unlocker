// latmon.1: CPU checks of the latency probe's queue and CSV row (source/native/latency_probe_ring.h).
#include "latency_probe_ring.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using namespace latency_probe;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

Event Make(uint32_t producer, uint64_t sequence) noexcept
{
    Event event{};
    event.qpc = int64_t(sequence);
    event.thread = producer;
    event.kind = Kind::eMarker;
    event.a = sequence;
    return event;
}

// In order, refused when full (and counted), then reusable after a drain, across many wraps.
void SingleThread()
{
    Ring<8> ring;
    Event out{};
    CHECK(!ring.Pop(out));
    for (uint64_t i = 0; i < 8; ++i) CHECK(ring.Push(Make(1, i)));
    CHECK(!ring.Push(Make(1, 8)));
    CHECK(!ring.Push(Make(1, 9)));
    CHECK(ring.TakeDropped() == 2);
    CHECK(ring.TakeDropped() == 0);
    for (uint64_t i = 0; i < 8; ++i) CHECK(ring.Pop(out) && out.a == i);
    CHECK(!ring.Pop(out));
    uint64_t next = 100, expected = 100;
    bool ordered = true;
    for (int round = 0; round < 1000; ++round)
    {
        const int pushes = 1 + round % 8;
        for (int i = 0; i < pushes; ++i) ring.Push(Make(1, next++));
        while (ring.Pop(out)) ordered &= out.a == expected++;
    }
    CHECK(ordered);
    CHECK(expected == next);
    CHECK(ring.TakeDropped() == 0);
}

// Four producers and one draining thread: every event arrives once or is counted as refused, and each producer's
// events arrive in the order it pushed them.
void Concurrent()
{
    constexpr uint32_t kProducers = 4;
    constexpr uint64_t kPerProducer = 200000;
    auto ring = std::make_unique<Ring<1024>>();
    std::atomic<uint32_t> running{kProducers};
    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; ++p)
        producers.emplace_back([&ring, &running, p] {
            for (uint64_t i = 0; i < kPerProducer; ++i) ring->Push(Make(p, i));
            running.fetch_sub(1, std::memory_order_release);
        });
    std::vector<uint64_t> next(kProducers, 0);
    uint64_t received = 0;
    bool known = true, ordered = true;
    Event out{};
    for (;;)
    {
        const bool done = running.load(std::memory_order_acquire) == 0;
        while (ring->Pop(out))
        {
            ++received;
            if (out.thread >= kProducers) { known = false; continue; }
            // Refused events leave gaps; what does arrive must keep rising.
            ordered &= out.a >= next[out.thread];
            next[out.thread] = out.a + 1;
        }
        if (done) break;
        std::this_thread::yield();
    }
    for (auto& producer : producers) producer.join();
    while (ring->Pop(out)) ++received;
    const uint64_t dropped = ring->TakeDropped();
    CHECK(known);
    CHECK(ordered);
    CHECK(received + dropped == kProducers * kPerProducer);
    CHECK(received > 0);
    std::printf("concurrent: received %llu, refused %llu\n", static_cast<unsigned long long>(received),
        static_cast<unsigned long long>(dropped));
}

void Rows()
{
    Event event{};
    event.qpc = 123456789012345;
    event.thread = 4242;
    event.kind = Kind::eFrameStats;
    event.sub = kStatsCount | kStatsFrame;
    event.a = (uint64_t(77) << 32) | 76;
    event.b = 9001;
    event.c = 9002;
    event.d = 123456789000000;
    char row[160]{};
    const int length = Format(event, row, sizeof(row));
    const char expected[] = "frame_stats,123456789012345,4242,3,330712481868,9001,9002,123456789000000\n";
    CHECK(length == int(sizeof(expected) - 1));
    CHECK(std::strcmp(row, expected) == 0);
    char small[16]{};
    CHECK(Format(event, small, sizeof(small)) == 0);
    CHECK(Format(event, nullptr, 0) == 0);
    event.kind = Kind::eGamePresent;
    event.qpc = -1;
    CHECK(Format(event, row, sizeof(row)) > 0 && std::strncmp(row, "game_present,-1,", 16) == 0);
    CHECK(std::strcmp(KindName(static_cast<Kind>(200)), "unknown") == 0);
    CHECK(std::strcmp(kHeader, "kind,qpc,thread,sub,a,b,c,d\n") == 0);
}
}

int main()
{
    SingleThread();
    Concurrent();
    Rows();
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
