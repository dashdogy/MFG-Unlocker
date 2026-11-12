// pacerfix.2: the flip queue's per-output trace (source/native/xess/pacing_trace.h). A synthetic run is recorded the way
// sdk_pacing.cpp QueueThread records it, written to a file and read back, and replayed through a fresh JitterBufferClock,
// which must reproduce every release and interval; a changed record must show as a mismatch. The ring keeps the last
// kCapacity records, and Read refuses another format.
// With a file argument (a user's RTXMFG-<game>-<id>.pacing.trace, written beside the game's log when the meter stops with
// RTX_MFG_PACING_TRACE=1 set) it replays that file instead and prints what it found: `XemfgTraceReplayTests <file>`
// (exit 1 on any mismatch in a trace that starts at the queue's first output).
#include "xess/pacing_trace.h"
#include "xess/sdk_pacing_meter.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace
{
using namespace sdk_pacing::cadence;
using namespace pacing_trace;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr int64_t kMs = 1000000;

struct ReplayResult
{
    size_t records{}, planned{}, mismatches{}, firstMismatch{SIZE_MAX};
    uint64_t compensated{}, lateFirsts{}, hitches{};
    int64_t lastInterval{};
};

// What QueueThread does with each output, from its record alone, in QueueThread's order: an accepted transition's
// reset, the capacity, the V-Sync floor, metering, then (ready) the pass latency, Plan with the committed slot, and a
// metered batch's ungated outputs; a copy that never became ready resets the clock.
ReplayResult Replay(const std::vector<Record>& records)
{
    JitterBufferClock clock;
    clock.SetBufferFraction(0.5);
    clock.SetFirstAtReadiness(true);
    ReplayResult result{};
    for (size_t i = 0; i < records.size(); ++i)
    {
        const Record& r = records[i];
        ++result.records;
        if (r.flags & kReset) clock.Reset();
        clock.SetCapacity(r.capacity);
        clock.SetFloor(r.floorNs);
        clock.SetUnmetered(!(r.flags & kMetered));
        if (!(r.flags & kReady)) { clock.Reset(); continue; }
        clock.SetPassLatency(int64_t(r.passNs));
        const auto release = clock.Plan(r.readyNs, r.submitNs, (r.flags & kFirst) != 0, r.outputs, r.gateOpenNs,
            (r.flags & kCoupled) != 0, (r.flags & kFull) != 0, r.gameWaitNs, r.slotNs, int64_t(r.sleptNs),
            (r.flags & kHostQuiet) ? 0 : -1);
        if ((r.flags & kMeteredBatch) && r.outputs > 1) clock.AdvanceUngated(r.outputs-1);
        ++result.planned;
        if (release.ns != r.releaseNs || release.interval != r.intervalNs)
        {
            if (!result.mismatches) result.firstMismatch = i;
            ++result.mismatches;
        }
        result.lastInterval = release.interval;
    }
    result.compensated = clock.Compensated();
    result.lateFirsts = clock.LateFirsts();
    result.hitches = clock.Hitches();
    return result;
}

// A run recorded as QueueThread records it: 4X then 3X (a live count change), jittered hand-overs and copies, late
// outputs, a V-Sync stretch with its floor, game-wait evidence on every third batch (with a low-latency sleep on
// every second of those), NVIDIA-metered batches gated at their first output only, coupled / full evidence, an accepted
// transition (reset), a resize to 3 buffers and a copy that never became ready.
std::vector<Record> Synthetic()
{
    JitterBufferClock clock;
    clock.SetBufferFraction(0.5);
    clock.SetFirstAtReadiness(true);
    std::vector<Record> records;
    uint32_t random = 11u;
    const auto next = [&random](int64_t low, int64_t high) {
        random = random*1664525u+1013904223u;
        return low+int64_t(uint64_t(random >> 8)%uint64_t(high-low+1));
    };
    int64_t submit = 1000 * kMs, gateOpen = 0;
    uint64_t value = 0;
    uint32_t capacity = 8;
    for (int batch = 0; batch < 400; ++batch)
    {
        const uint32_t outputs = batch < 200 ? 4 : 3;
        const bool vsync = batch >= 100 && batch < 150;
        const bool metered = batch >= 250 && batch < 300;
        const int64_t floor = vsync ? 8333333 : 0;
        if (batch == 340) capacity = 3;
        submit += 18*kMs+next(-4*kMs, 4*kMs);
        const int64_t wait = batch%3 == 0 ? next(0, 3*kMs) : -1;
        const uint32_t slept = batch%6 == 0 ? uint32_t(next(0, kMs)) : 0u;
        int64_t ready = submit+3*kMs+next(-kMs, 4*kMs);
        for (uint32_t i = 0; i < outputs; ++i)
        {
            if (metered && i) break; // the driver flips the rest ungated
            const bool first = i == 0;
            const bool reset = first && batch == 320;
            if (reset) clock.Reset();
            clock.SetCapacity(capacity);
            clock.SetFloor(floor);
            clock.SetUnmetered(!metered);
            if (!first) ready = gateOpen+next(100000, 7*kMs);
            const int64_t slot = clock.Slot(first);
            const int64_t committed = slot <= 0 ? 0
                : ready < slot ? (std::min)(slot, ready+JitterBufferClock::kMaximumHold) : slot;
            Record r{};
            r.value = ++value; r.outputs = outputs; r.index = i; r.capacity = capacity; r.floorNs = floor;
            r.submitNs = first ? submit : 0; r.slotNs = committed; r.gateOpenNs = gateOpen;
            r.gameWaitNs = first ? wait : -1;
            r.sleptNs = first ? slept : 0u;
            r.signalNs = committed > 0 ? committed-500000 : ready;
            r.flags = (first ? kFirst : 0u) | (vsync ? kVsync : 0u) | (metered ? kMetered : 0u)
                | (metered && first ? kMeteredBatch : 0u) | (reset ? kReset : 0u);
            if (batch == 360 && i == 1)
            {
                // Never ready: released at once, the clock starts over.
                clock.Reset();
                r.releaseNs = r.signalNs;
                r.passNs = uint32_t(clock.PassLatency());
                records.push_back(r);
                gateOpen = r.signalNs;
                continue;
            }
            const bool coupled = first && batch%7 == 0, full = first && batch%11 == 0;
            clock.SetPassLatency(!metered && slot <= 0 ? 400000 : 0);
            const uint64_t compensated = clock.Compensated();
            const auto release = clock.Plan(ready, r.submitNs, first, outputs, gateOpen, coupled, full, r.gameWaitNs,
                committed, int64_t(r.sleptNs));
            if (metered && first) clock.AdvanceUngated(outputs-1);
            r.readyNs = ready; r.releaseNs = release.ns; r.intervalNs = release.interval;
            r.passNs = uint32_t(clock.PassLatency());
            r.flags |= kReady | (coupled ? kCoupled : 0u) | (full ? kFull : 0u) | (release.held ? kHeld : 0u)
                | (release.late ? kLate : 0u) | (release.hitch ? kHitch : 0u)
                | (committed > 0 && ready > r.signalNs ? kAhead : 0u)
                | (clock.Compensated() != compensated ? kCompensated : 0u);
            records.push_back(r);
            gateOpen = (std::max)(r.signalNs, ready);
        }
    }
    return records;
}

std::filesystem::path TempTrace()
{
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path(error);
    if (error) directory = std::filesystem::current_path();
    return directory / L"xemfg_trace_replay_test.pacing.trace";
}

void RoundTrip()
{
    const auto records = Synthetic();
    size_t ready = 0, firsts = 0, metered = 0, evidence = 0;
    for (const auto& r : records)
    {
        ready += (r.flags & kReady) != 0;
        firsts += (r.flags & kFirst) != 0;
        metered += (r.flags & kMeteredBatch) != 0;
        evidence += (r.flags & kCompensated) != 0;
    }
    // 200 4X batches, 50 metered 3X batches of one gated output, 150 other 3X batches.
    CHECK(records.size() == 200*4+50*1+150*3);
    CHECK(firsts == 400 && metered == 50 && ready+1 == records.size());
    CHECK(evidence > 0);
    auto ring = std::make_unique<Ring>();
    for (const auto& r : records) ring->Push(r);
    CHECK(ring->Count() == records.size() && ring->Dropped() == 0);
    const auto path = TempTrace();
    std::FILE* file{};
    CHECK(!_wfopen_s(&file, path.c_str(), L"wb") && file);
    if (!file) return;
    CHECK(Write(file, *ring, 42));
    std::fclose(file);
    file = nullptr;
    Header header{};
    std::vector<Record> read;
    CHECK(!_wfopen_s(&file, path.c_str(), L"rb") && file);
    if (!file) return;
    CHECK(Read(file, header, read));
    std::fclose(file);
    std::error_code removed;
    std::filesystem::remove(path, removed);
    CHECK(header.generation == 42 && header.count == records.size() && header.dropped == 0);
    CHECK(read.size() == records.size());
    CHECK(read.size() == records.size() && std::memcmp(read.data(), records.data(), records.size()*sizeof(Record)) == 0);
    // The replay reproduces every decision.
    const auto replay = Replay(read);
    CHECK(replay.records == records.size() && replay.planned == ready);
    CHECK(replay.mismatches == 0);
    CHECK(replay.compensated == evidence);
    // A changed record shows where it changed.
    auto changed = read;
    const size_t at = 600;
    changed[at].releaseNs += 1;
    const auto broken = Replay(changed);
    CHECK(broken.mismatches >= 1 && broken.firstMismatch == at);
    std::printf("trace: %zu records, %zu planned, %llu compensated, %llu late firsts, interval %lld ns\n", replay.records,
        replay.planned, static_cast<unsigned long long>(replay.compensated),
        static_cast<unsigned long long>(replay.lateFirsts), static_cast<long long>(replay.lastInterval));
}

void RingAndFormat()
{
    auto ring = std::make_unique<Ring>();
    for (uint64_t i = 1; i <= Ring::kCapacity+10; ++i)
    {
        Record r{};
        r.value = i;
        ring->Push(r);
    }
    CHECK(ring->Count() == Ring::kCapacity && ring->Dropped() == 10);
    CHECK(ring->At(0).value == 11 && ring->At(Ring::kCapacity-1).value == Ring::kCapacity+10);
    ring->Clear();
    CHECK(ring->Count() == 0 && ring->Dropped() == 0);
    // Another magic, version or record size, or too many records: refused.
    const auto path = TempTrace();
    const auto refused = [&path](const Header& header) {
        std::FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"wb") || !file) return false;
        std::fwrite(&header, sizeof(header), 1, file);
        std::fclose(file);
        file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return false;
        Header read{};
        std::vector<Record> records;
        const bool ok = Read(file, read, records);
        std::fclose(file);
        return !ok;
    };
    Header magic{}; magic.magic[7] = '2';
    Header version{}; version.version = 2;
    Header size{}; size.recordSize = 96;
    Header count{}; count.count = Ring::kCapacity+1;
    Header shortFile{}; shortFile.count = 3; // no records follow
    CHECK(refused(magic));
    CHECK(refused(version));
    CHECK(refused(size));
    CHECK(refused(count));
    CHECK(refused(shortFile));
    Header empty{};
    CHECK(!refused(empty));
    std::error_code removed;
    std::filesystem::remove(path, removed);
}

int ReplayFile(const char* name)
{
    std::FILE* file{};
    if (fopen_s(&file, name, "rb") || !file) { std::printf("cannot open %s\n", name); return 2; }
    Header header{};
    std::vector<Record> records;
    const bool read = Read(file, header, records);
    std::fclose(file);
    if (!read)
    {
        std::printf("%s: not a pacing trace (XMFGTRC1, version 1, %zu-byte records)\n", name, sizeof(Record));
        return 2;
    }
    size_t firsts = 0, metered = 0, ahead = 0, late = 0, notReady = 0, resets = 0;
    for (const auto& r : records)
    {
        firsts += (r.flags & kFirst) != 0;
        metered += (r.flags & kMeteredBatch) != 0;
        ahead += (r.flags & kAhead) != 0;
        late += (r.flags & kLate) != 0;
        notReady += !(r.flags & kReady);
        resets += (r.flags & kReset) != 0;
    }
    const auto replay = Replay(records);
    std::printf("%s: generation %llu, %zu records (%llu dropped before them), %zu first outputs, %zu metered batches, "
        "%zu signalled ahead, %zu late firsts, %zu not ready, %zu resets\n", name,
        static_cast<unsigned long long>(header.generation), records.size(),
        static_cast<unsigned long long>(header.dropped), firsts, metered, ahead, late, notReady, resets);
    std::printf("replay: %zu planned, %zu mismatches", replay.planned, replay.mismatches);
    if (replay.mismatches) std::printf(" (first at record %zu, gate %llu)", replay.firstMismatch,
        static_cast<unsigned long long>(records[replay.firstMismatch].value));
    std::printf(", %llu compensated, %llu hitches, last interval %lld ns\n",
        static_cast<unsigned long long>(replay.compensated), static_cast<unsigned long long>(replay.hitches),
        static_cast<long long>(replay.lastInterval));
    // A trace that starts mid-run (the ring dropped older records) replays from a fresh clock, so it may differ.
    return replay.mismatches && !header.dropped ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "roundtrip") && std::strcmp(argv[1], "ring")) return ReplayFile(argv[1]);
    const char* only = argc > 1 ? argv[1] : nullptr;
    const auto run = [&](const char* name, void (*test)()) { if (!only || std::strcmp(only, name) == 0) test(); };
    run("roundtrip", RoundTrip);
    run("ring", RingAndFormat);
    std::printf("trace replay: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
