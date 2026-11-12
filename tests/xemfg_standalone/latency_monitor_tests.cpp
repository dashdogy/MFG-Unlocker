// latmon.2: CPU checks of the live PC-latency estimator (source/native/latency_monitor.h) on synthetic runs that match
// tests/analyze_latency_probe.py's self-test: 60 fps base, 4 outputs per batch, 240 Hz, 10 MHz QPC.
#include "latency_monitor.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
using namespace latency_probe;
using latency_monitor::Estimator;
using latency_monitor::State;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr int64_t kFrequency = 10'000'000;
constexpr int64_t kVblank = kFrequency / 240;
constexpr uint32_t kGame = 10, kPresenter = 20;
constexpr uint64_t kChain = 0xABC;

enum class Route { eDlssg, eXefg };
struct Options
{
    Route route = Route::eDlssg;
    int frames = 120;
    bool markers = true;      // simulation-start markers
    bool statistics = true;   // GetFrameStatistics succeeds
    bool outputs = true;      // an output layer exists
    // latmon.3: DLSS-G hands the whole batch over within a millisecond and the driver meters the flips, one refresh
    // apart, a batch behind: the samples after each Present see about one flip per batch.
    bool burst = false;
    // latmon.4: frames quietFrom..quietTo send no simulation-start marker.
    int quietFrom = 0, quietTo = 0;
};

struct Timed { int64_t pushed; size_t order; Event event; };

Event Make(Kind kind, int64_t qpc, uint32_t thread, int sub, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    Event event{};
    event.kind = kind; event.qpc = qpc; event.thread = thread; event.sub = static_cast<uint16_t>(sub);
    event.a = a; event.b = b; event.c = c; event.d = d;
    return event;
}

// The events in the order the probe's queue would hold them (each pushed when its call returns).
std::vector<Event> Run(const Options& options)
{
    std::vector<Timed> timed;
    const auto push = [&](int64_t at, const Event& event) { timed.push_back({at, timed.size(), event}); };
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, int64_t>> flips;
    for (int frame = 1; frame <= options.frames; ++frame)
    {
        const int64_t start = frame * 4 * kVblank, handOver = start + 50'000;
        const bool quiet = frame >= options.quietFrom && frame <= options.quietTo;
        if (options.markers && !quiet)
            push(start, Make(Kind::eMarker, start, kGame, 0, uint64_t(frame), kSourcePcl, 0, 0));
        push(handOver - 10, Make(Kind::eMarker, handOver - 10, kGame, 4, uint64_t(frame), kSourcePcl, 0, 0));
        int64_t returned = handOver + 2'000;
        const int64_t firstVblank = (handOver + 40'000) / kVblank + (options.burst ? 3 : 1);
        for (int index = 0; options.outputs && index < 4; ++index)
        {
            const int64_t present = handOver + 40'000 + index * (options.burst ? 2'000 : kVblank);
            const int64_t flip = (firstVblank + index) * kVblank;
            const bool generated = index < 3;
            uint32_t thread = kPresenter;
            int role = kRoleStreamlineBase;
            if (options.route == Route::eXefg)
            {
                thread = generated ? kPresenter : kGame;
                role = kRoleXefgOutput;
                push(present - 5, Make(Kind::eXefgOutput, present - 5, thread, generated ? 1 : 0, uint64_t(frame), 0, 0, 0));
                if (!generated) returned = present + 1'000;
            }
            push(present + 500, Make(Kind::eOutputPresent, present, thread, role, uint64_t(present + 500), 0, kChain, 0));
            ++count;
            flips.push_back({count, flip});
            const int64_t now = present + 600;
            uint32_t shown = 0;
            int64_t shownFlip = 0;
            for (const auto& [c, f] : flips) if (f <= now) { shown = c; shownFlip = f; }
            if (options.statistics && shown)
            {
                const int64_t sync = now / kVblank;
                push(now, Make(Kind::eFrameStats, now, thread, kStatsCount | kStatsFrame,
                    (uint64_t(count) << 32) | shown, uint64_t(1000 + shownFlip / kVblank), uint64_t(1000 + sync),
                    uint64_t(sync * kVblank)));
            }
            else
                push(now, Make(Kind::eFrameStats, now, thread, options.statistics ? kStatsCount : (kStatsCount | kStatsDisjoint),
                    uint64_t(count) << 32, 0, 0, 0));
        }
        push(returned, Make(Kind::eGamePresent, handOver, kGame, options.route == Route::eXefg ? kRoleXemfgHost : 0,
            uint64_t(returned), 0, 77, 0));
    }
    std::stable_sort(timed.begin(), timed.end(), [](const Timed& l, const Timed& r) {
        return l.pushed != r.pushed ? l.pushed < r.pushed : l.order < r.order;
    });
    std::vector<Event> events;
    for (const auto& item : timed) events.push_back(item.event);
    return events;
}

int64_t EndOf(const Options& options) { return (options.frames + 1) * 4 * kVblank; }

std::vector<Event> Sorted(std::vector<Timed>& timed)
{
    std::stable_sort(timed.begin(), timed.end(), [](const Timed& l, const Timed& r) {
        return l.pushed != r.pushed ? l.pushed < r.pushed : l.order < r.order;
    });
    std::vector<Event> events;
    for (const auto& item : timed) events.push_back(item.event);
    return events;
}

// The statistics sample right after a Present: the latest flip by `now`, in vblanks of `period` from 0.
Event StatsAfter(int64_t now, uint32_t thread, uint32_t count, const std::vector<std::pair<uint32_t, int64_t>>& flips,
    int64_t period)
{
    uint32_t shown = 0;
    int64_t shownFlip = 0;
    for (const auto& [c, f] : flips) if (f <= now) { shown = c; shownFlip = f; }
    if (!shown) return Make(Kind::eFrameStats, now, thread, kStatsCount, uint64_t(count) << 32, 0, 0, 0);
    const int64_t sync = now / period;
    return Make(Kind::eFrameStats, now, thread, kStatsCount | kStatsFrame, (uint64_t(count) << 32) | shown,
        uint64_t(1000 + shownFlip / period), uint64_t(1000 + sync), uint64_t(sync * period));
}

// latmon.4: the game sets its present-start markers on its main thread and presents on a render thread; its Present
// into frame generation returns after the next frame's simulation start and present-start marker.
std::vector<Event> RunCrossThread(int frames)
{
    constexpr uint32_t kRender = 30;
    std::vector<Timed> timed;
    const auto push = [&](int64_t at, const Event& event) { timed.push_back({at, timed.size(), event}); };
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, int64_t>> flips;
    for (int frame = 1; frame <= frames; ++frame)
    {
        const int64_t start = frame * 4 * kVblank, handOver = start + 50'000;
        push(start, Make(Kind::eMarker, start, kGame, 0, uint64_t(frame), kSourcePcl, 0, 0));
        push(handOver - 10, Make(Kind::eMarker, handOver - 10, kGame, 4, uint64_t(frame), kSourcePcl, 0, 0));
        const int64_t returned = handOver + 4 * kVblank + 10'000;
        const int64_t firstVblank = (handOver + 40'000) / kVblank + 1;
        for (int index = 0; index < 4; ++index)
        {
            const int64_t present = handOver + 40'000 + index * kVblank;
            push(present + 500, Make(Kind::eOutputPresent, present, kPresenter, kRoleStreamlineBase,
                uint64_t(present + 500), 0, kChain, 0));
            flips.push_back({++count, (firstVblank + index) * kVblank});
            push(present + 600, StatsAfter(present + 600, kPresenter, count, flips, kVblank));
        }
        push(returned, Make(Kind::eGamePresent, handOver, kRender, 0, uint64_t(returned), 0, 77, 0));
    }
    return Sorted(timed);
}

// latmon.4: no frame generation (a menu): one output per hand-over inside the game's Present, 145 fps on a 120 Hz
// display with tearing. Each flip lands 2 ms after its Present returns, mid-refresh; the statistics name the refresh
// it landed in, whose start can be before the Present began.
constexpr int64_t kTearingPeriod = kFrequency / 120, kTearingStep = kFrequency / 145;
std::vector<Event> RunTearing(int frames)
{
    std::vector<Timed> timed;
    const auto push = [&](int64_t at, const Event& event) { timed.push_back({at, timed.size(), event}); };
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, int64_t>> shown;
    for (int frame = 1; frame <= frames; ++frame)
    {
        const int64_t start = 1'000'000 + frame * kTearingStep, handOver = start + 30'000;
        push(start, Make(Kind::eMarker, start, kGame, 0, uint64_t(frame), kSourcePcl, 0, 0));
        push(handOver - 10, Make(Kind::eMarker, handOver - 10, kGame, 4, uint64_t(frame), kSourcePcl, 0, 0));
        const int64_t present = handOver + 200, returned = present + 300;
        push(returned, Make(Kind::eOutputPresent, present, kGame, kRoleStreamlineBase, uint64_t(returned), 0, kChain, 0));
        shown.push_back({++count, returned + 20'000});
        const int64_t now = returned + 100;
        uint32_t latest = 0;
        int64_t latestFlip = 0;
        for (const auto& [c, f] : shown) if (f <= now) { latest = c; latestFlip = f; }
        if (latest)
        {
            const int64_t sync = now / kTearingPeriod;
            push(now, Make(Kind::eFrameStats, now, kGame, kStatsCount | kStatsFrame, (uint64_t(count) << 32) | latest,
                uint64_t(1000 + latestFlip / kTearingPeriod), uint64_t(1000 + sync), uint64_t(sync * kTearingPeriod)));
        }
        else
            push(now, Make(Kind::eFrameStats, now, kGame, kStatsCount, uint64_t(count) << 32, 0, 0, 0));
        push(returned + 50, Make(Kind::eGamePresent, handOver, kGame, 0, uint64_t(returned + 50), 0, 77, 0));
    }
    return Sorted(timed);
}

// latmon.4: the burst (DLSS-G 4x, the batch handed over at once, flips one refresh apart) with every fourth game frame
// three frame times long: the presenter idles and the next exact anchor is a batch later.
struct Hitch { std::vector<Event> events; int64_t end = 0; int64_t truth = 0; };
Hitch RunHitch(int frames)
{
    std::vector<Timed> timed;
    const auto push = [&](int64_t at, const Event& event) { timed.push_back({at, timed.size(), event}); };
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, int64_t>> flips;
    int64_t t = 0, lastFlip = 0, truthSum = 0;
    for (int frame = 1; frame <= frames; ++frame)
    {
        t += 4 * kVblank * (frame % 4 == 0 ? 3 : 1);
        const int64_t start = t, handOver = start + 50'000;
        push(start, Make(Kind::eMarker, start, kGame, 0, uint64_t(frame), kSourcePcl, 0, 0));
        push(handOver - 10, Make(Kind::eMarker, handOver - 10, kGame, 4, uint64_t(frame), kSourcePcl, 0, 0));
        for (int index = 0; index < 4; ++index)
        {
            const int64_t present = handOver + 40'000 + index * 2'000;
            const int64_t flip = std::max(((present + 40'000) / kVblank + 1) * kVblank, lastFlip + kVblank);
            lastFlip = flip;
            if (index == 0) truthSum += flip - start;
            push(present + 500, Make(Kind::eOutputPresent, present, kPresenter, kRoleStreamlineBase,
                uint64_t(present + 500), 0, kChain, 0));
            flips.push_back({++count, flip});
            push(present + 600, StatsAfter(present + 600, kPresenter, count, flips, kVblank));
        }
        push(handOver + 2'000, Make(Kind::eGamePresent, handOver, kGame, 0, uint64_t(handOver + 2'000), 0, 77, 0));
    }
    Hitch hitch{};
    hitch.events = Sorted(timed);
    hitch.end = t + 4 * kVblank;
    hitch.truth = truthSum / frames;
    return hitch;
}

void Live(Route route)
{
    Options options{};
    options.route = route;
    auto estimator = std::make_unique<Estimator>();
    estimator->SetFrequency(kFrequency);
    for (const auto& event : Run(options)) estimator->Feed(event);
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    // Simulation start -> 9 ms to the first output's Present -> the next vblank: three vblanks after the start.
    const int64_t first = 3 * kVblank;
    CHECK(stats.state == State::eLive);
    CHECK(stats.frames >= 55 && stats.frames <= 61);   // one second at 60 fps
    CHECK(stats.meanFirst == first);
    CHECK(stats.p99First == first);
    CHECK(stats.meanReal == first + 3 * kVblank);
    CHECK(stats.meanGame == 50'000);
    CHECK(stats.meanFg == 40'000);
    CHECK(stats.meanDisplay == first - 90'000);
    CHECK(stats.exactPermille == 1000);
    const auto counters = estimator->ReadCounters();
    CHECK(counters.frames >= 110);
    CHECK(counters.noStart == 0);
    CHECK(counters.implausible == 0);
    CHECK(counters.inferred == 0);
    std::printf("%s: %u frames, first %.3f ms, real %.3f ms\n", route == Route::eDlssg ? "dlssg" : "xefg", stats.frames,
        double(stats.meanFirst) * 1000.0 / kFrequency, double(stats.meanReal) * 1000.0 / kFrequency);
}

void States()
{
    auto estimator = std::make_unique<Estimator>();
    estimator->SetFrequency(kFrequency);
    CHECK(estimator->Read(0, kFrequency).state == State::eMeasuring);

    // Four seconds of frames: past the two quiet seconds that decide why nothing is measured.
    Options noMarkers{};
    noMarkers.frames = 240;
    noMarkers.markers = false;
    for (const auto& event : Run(noMarkers)) estimator->Feed(event);
    auto stats = estimator->Read(EndOf(noMarkers), kFrequency);
    CHECK(stats.state == State::eNoMarkers);
    CHECK(stats.frames == 0);
    CHECK(estimator->ReadCounters().noStart > 100);

    estimator->Reset();
    CHECK(estimator->Read(EndOf(noMarkers), kFrequency).state == State::eMeasuring);
    Options noStatistics{};
    noStatistics.frames = 240;
    noStatistics.statistics = false;
    for (const auto& event : Run(noStatistics)) estimator->Feed(event);
    CHECK(estimator->Read(EndOf(noStatistics), kFrequency).state == State::eNoFlipTimes);

    estimator->Reset();
    Options noOutputs{};
    noOutputs.frames = 240;
    noOutputs.outputs = false;
    for (const auto& event : Run(noOutputs)) estimator->Feed(event);
    CHECK(estimator->Read(EndOf(noOutputs), kFrequency).state == State::eNoOutputs);

    // Less than two seconds in, with nothing measured yet: still measuring.
    estimator->Reset();
    Options shortRun{};
    shortRun.frames = 2;
    shortRun.markers = false;
    for (const auto& event : Run(shortRun)) estimator->Feed(event);
    CHECK(estimator->Read(EndOf(shortRun), kFrequency).state == State::eMeasuring);

    // Three seconds after the last flip the readout holds over the last five; past five seconds nothing is current.
    estimator->Reset();
    Options live{};
    for (const auto& event : Run(live)) estimator->Feed(event);
    CHECK(estimator->Read(EndOf(live), kFrequency).state == State::eLive);
    CHECK(!estimator->Read(EndOf(live), kFrequency).widened);
    const auto held = estimator->Read(EndOf(live) + 3 * kFrequency, kFrequency);
    CHECK(held.state == State::eLive);
    CHECK(held.widened);
    CHECK(held.meanFirst == 3 * kVblank);
    CHECK(estimator->Read(EndOf(live) + 6 * kFrequency, kFrequency).frames == 0);
}

// Inferred flips: every statistics sample one refresh late still gives the same latency through the refresh period.
void Inferred()
{
    Options options{};
    auto events = Run(options);
    // Make every third sample report one refresh after the flip (estimated through the period).
    int index = 0;
    for (auto& event : events)
    {
        if (event.kind != Kind::eFrameStats || !(event.sub & kStatsFrame)) continue;
        if (++index % 3 == 0 && event.b == event.c)
        {
            event.c += 1;
            event.d += uint64_t(kVblank);
        }
    }
    auto estimator = std::make_unique<Estimator>();
    estimator->SetFrequency(kFrequency);
    for (const auto& event : events) estimator->Feed(event);
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    CHECK(stats.state == State::eLive);
    CHECK(stats.meanFirst >= 3 * kVblank - 2 && stats.meanFirst <= 3 * kVblank + 2);
    CHECK(stats.exactPermille < 1000);
    CHECK(estimator->ReadCounters().inferred > 0);
}

// latmon.3: the batch handed over at once (DLSS-G at 4x). Exact samples name about one flip per batch; the presents
// between two of them are placed between them.
void Burst()
{
    Options options{};
    options.frames = 240;
    options.burst = true;
    auto estimator = std::make_unique<Estimator>();
    estimator->SetFrequency(kFrequency);
    for (const auto& event : Run(options)) estimator->Feed(event);
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    CHECK(stats.state == State::eLive);
    CHECK(stats.frames >= 55 && stats.frames <= 61);
    CHECK(stats.meanFirst == 5 * kVblank);       // 9 ms, the next vblank, then a batch queued ahead (2 more)
    CHECK(stats.p99First == 5 * kVblank);
    CHECK(stats.meanReal == 8 * kVblank);
    CHECK(stats.exactPermille < 1000);
    const auto counters = estimator->ReadCounters();
    CHECK(counters.frames >= 230);
    CHECK(counters.interpolated > 0);
    CHECK(counters.noFlip <= 2);
    CHECK(counters.noOutputs == 0);
    std::printf("burst: %u frames, first %.3f ms, real %.3f ms, %llu flips placed between samples\n", stats.frames,
        double(stats.meanFirst) * 1000.0 / kFrequency, double(stats.meanReal) * 1000.0 / kFrequency,
        static_cast<unsigned long long>(counters.interpolated));
}

std::unique_ptr<Estimator> Fed(const std::vector<Event>& events)
{
    auto estimator = std::make_unique<Estimator>();
    estimator->SetFrequency(kFrequency);
    for (const auto& event : events) estimator->Feed(event);
    return estimator;
}

// latmon.4: markers on one thread, Presents on another that return a frame late: no frame pairs with the next one's start.
void CrossThread()
{
    const auto estimator = Fed(RunCrossThread(240));
    const auto stats = estimator->Read((240 + 1) * 4 * kVblank, kFrequency);
    const auto counters = estimator->ReadCounters();
    CHECK(stats.state == State::eLive);
    CHECK(stats.meanFirst == 3 * kVblank);
    CHECK(counters.implausible == 0);
    CHECK(counters.frames >= 230);
}

// latmon.4: tearing flips in a refresh that began before their Present.
void Tearing()
{
    const auto estimator = Fed(RunTearing(600));
    const auto stats = estimator->Read(1'000'000 + 601 * kTearingStep, kFrequency);
    const auto counters = estimator->ReadCounters();
    CHECK(stats.state == State::eLive);
    CHECK(counters.implausible == 0);
    CHECK(counters.clamped > 0);
    CHECK(counters.frames >= 500);
    // The truth is 5.05 ms (30'000 + 200 + 300 + 20'000 ticks); a clamped flip is placed at its Present's return.
    CHECK(stats.meanFirst >= 30'500 && stats.meanFirst <= 50'500);
    std::printf("tearing: %u frames, first %.3f ms (truth 5.050), %llu flips placed at their Present\n", stats.frames,
        double(stats.meanFirst) * 1000.0 / kFrequency, static_cast<unsigned long long>(counters.clamped));
}

// latmon.4: a long game frame leaves the presenter idle; flips are never placed before their Present.
void HitchFrames()
{
    const auto hitch = RunHitch(240);
    const auto estimator = Fed(hitch.events);
    const auto stats = estimator->Read(hitch.end, kFrequency);
    const auto counters = estimator->ReadCounters();
    CHECK(stats.state == State::eLive);
    CHECK(counters.implausible == 0);
    CHECK(counters.frames >= 225);
    CHECK(stats.meanFirst >= hitch.truth - kVblank && stats.meanFirst <= hitch.truth + kVblank);
    std::printf("hitch: %u frames, first %.3f ms (truth %.3f)\n", stats.frames,
        double(stats.meanFirst) * 1000.0 / kFrequency, double(hitch.truth) * 1000.0 / kFrequency);
}

// latmon.4: a second and a half without measurable frames holds the last five seconds instead of Measuring.
void Gap()
{
    Options options{};
    options.frames = 240;
    options.quietFrom = 151;
    options.quietTo = 240;
    const auto estimator = Fed(Run(options));
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    CHECK(stats.state == State::eLive);
    CHECK(stats.widened);
    CHECK(stats.meanFirst == 3 * kVblank);
}

// latmon.4: a frame id the game uses again while the old one is still held is a new frame.
void ReusedIds()
{
    Options options{};
    options.frames = 480;
    auto events = Run(options);
    for (auto& event : events)
        if (event.kind == Kind::eMarker && event.a > 120) event.a -= 100;
    const auto estimator = Fed(events);
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    CHECK(stats.state == State::eLive);
    CHECK(stats.meanFirst == 3 * kVblank);
    CHECK(estimator->ReadCounters().implausible == 0);
}

// latmon.4: a gap in the events (the queue refused some) keeps the measured frames.
void Resync()
{
    Options options{};
    auto estimator = Fed(Run(options));
    estimator->Resync();
    const auto stats = estimator->Read(EndOf(options), kFrequency);
    CHECK(stats.state == State::eLive);
    CHECK(stats.meanFirst == 3 * kVblank);
}
}

int main()
{
    Live(Route::eDlssg);
    Live(Route::eXefg);
    States();
    Inferred();
    Burst();
    CrossThread();
    Tearing();
    HitchFrames();
    Gap();
    ReusedIds();
    Resync();
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
