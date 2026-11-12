// pacerfix.1: the flip queue's CPU pieces (source/native/xess/sdk_pacing_meter.h). "mirror" numbers below come from an
// offline Python model of the same clock; the bounds leave room around them.
#include "xess/sdk_pacing_meter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace
{
using namespace sdk_pacing::cadence;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr int64_t kMs = 1000000;

struct Lcg
{
    uint32_t state;
    int64_t Next(int64_t low, int64_t high)
    {
        state = state*1664525u+1013904223u;
        return low+int64_t(uint64_t(state >> 8)%uint64_t(high-low+1));
    }
};

// The slot the meter commits before an output's copy is ready (gate-ahead), as QueueThread does.
int64_t Committed(const JitterBufferClock& clock, bool first, int64_t ready)
{
    const int64_t slot = clock.Slot(first);
    return slot <= 0 ? 0 : ready < slot ? (std::min)(slot, ready+JitterBufferClock::kMaximumHold) : slot;
}

// The chain's capacity and the small-chain rule. XeFG's own chain: 3 buffers, latency 3. NativeBuffersScope raises it to
// 8 (OptiScaler without the menu fix, XeMFG standalone) or 16 buffers.
void SmallChainRule()
{
    CHECK(ChainCapacity(3, 3) == 3);
    CHECK(ChainCapacity(4, 3) == 3);
    CHECK(ChainCapacity(5, 3) == 4);
    CHECK(ChainCapacity(8, 3) == 7);
    CHECK(ChainCapacity(16, 3) == 15);
    // A 3-buffer chain without driver metering: 5X and 6X (4 and 5 generated) are refused, 4X stays gated.
    CHECK(SmallChain(false, 3, 5));
    CHECK(SmallChain(false, 3, 4));
    CHECK(!SmallChain(false, 3, 3));
    // NVIDIA's metered route keeps every batch.
    CHECK(!SmallChain(true, 3, 5));
    // Unknown capacity refuses nothing.
    CHECK(!SmallChain(false, 0, 5));
    // The rule reads the chain's capacity, not the latency 2X left (3): on 8 buffers 6X and 7X stay gated, 8X does not fit.
    const uint32_t capacity = ChainCapacity(8, 3);
    CHECK(SmallChain(false, 3, 5));          // the latency in effect after 2X
    CHECK(!SmallChain(false, capacity, 5));  // 6X
    CHECK(!SmallChain(false, capacity, 6));  // 7X
    CHECK(SmallChain(false, capacity, 8));   // 9 outputs
    // After a resize gives the chain the game's 3 buffers back, its capacity is 3 again and 6X is refused.
    CHECK(SmallChain(false, ChainCapacity(3, 3), 5));
}

// 2X, a batch every 20 ms, the first output ready 5 ms after the hand-over and the real frame 1 ms after its release.
// The first output flips at readiness, `pass` after its signal; the real frame is held to its slot and flips at its
// release. Told the pass latency, the clock puts the real frame's flip one interval after the first's.
struct Displayed { int64_t gap{}, interval{}; bool firstHeld{}, realHeld{}; };
Displayed TwoX(int64_t pass, int64_t told)
{
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(8);
    clock.SetUnmetered(true);
    clock.SetPassLatency(told);
    Displayed result{};
    int64_t submit = 1000 * kMs;
    for (int batch = 0; batch < 12; ++batch, submit += 20 * kMs)
    {
        const auto first = clock.Plan(submit+5*kMs, submit, true, 2);
        const auto real = clock.Plan(first.ns+kMs, 0, false, 2);
        const int64_t firstFlip = first.ns+(first.held ? 0 : pass);
        const int64_t realFlip = real.ns+(real.held ? 0 : pass);
        result = {realFlip-firstFlip, real.interval, first.held, real.held};
    }
    return result;
}

void PassLatencySpacing()
{
    // No pass latency: nothing changes.
    const auto none = TwoX(0, 0);
    CHECK(none.interval == 10 * kMs);
    CHECK(!none.firstHeld && none.realHeld);
    CHECK(none.gap == 10 * kMs);
    // 0.6 ms pass latency (a CPU-scheduled fence wait), not told to the clock: the gap after the first output was short.
    const auto before = TwoX(600000, 0);
    CHECK(before.gap == 10 * kMs - 600000);
    // Told: the real frame's slot follows the first output's flip.
    const auto after = TwoX(600000, 600000);
    CHECK(!after.firstHeld && after.realHeld);
    CHECK(after.gap == 10 * kMs);
    JitterBufferClock clock;
    clock.SetPassLatency(9 * kMs);
    CHECK(clock.PassLatency() == JitterBufferClock::kMaximumPass);
    clock.SetPassLatency(-5);
    CHECK(clock.PassLatency() == 0);
    clock.SetPassLatency(300000);
    clock.Reset();
    CHECK(clock.PassLatency() == 300000);
}

// The gate lead and the pass latency the clock gets.
void GateLead()
{
    GatePassLead lead;
    for (uint64_t i = 0; i + 1 < GatePassLead::kMinimumSamples; ++i) lead.Add(22000);
    CHECK(lead.Lead(500000) == 500000);  // the fallback until enough samples
    CHECK(lead.Pass() == 0);
    lead.Add(22000);
    // A GPU-scheduled wait passes in ~22 us: the lead's lower bound is 50 us.
    CHECK(lead.Lead(500000) == GatePassLead::kMinimumLead);
    CHECK(GatePassLead::kMinimumLead == 50000);
    CHECK(lead.Pass() == 22000);
    GatePassLead slow;
    for (int i = 0; i < 64; ++i) slow.Add(650000);
    CHECK(slow.Lead(500000) == 650000);
    CHECK(slow.Pass() == 650000);
    GatePassLead stuck;
    for (int i = 0; i < 64; ++i) stuck.Add(3000000);
    CHECK(stuck.Lead(500000) == GatePassLead::kMaximumLead);
    CHECK(stuck.Pass() == GatePassLead::kMaximumLead);
}

// The precise wait's margin: 1 ms before anything is learned, down to its floor on a timer that wakes 80 us late, back
// up when wake-ups get late, never above its bound.
void Margin()
{
    WaitMargin margin;
    CHECK(margin.Margin() == 1000000);
    for (int i = 0; i < 400; ++i) margin.Add(80000);
    CHECK(margin.Late() >= 80000 && margin.Late() < 81000);
    CHECK(margin.Margin() == WaitMargin::kMinimum);
    for (int i = 0; i < 60; ++i) margin.Add(1200000);
    CHECK(margin.Margin() > 500000);
    CHECK(margin.Margin() <= WaitMargin::kMaximum);
    // Early wake-ups count as on time; one preemption counts at most kSampleLimit.
    WaitMargin early;
    for (int i = 0; i < 400; ++i) early.Add(-50000);
    CHECK(early.Late() >= 0 && early.Margin() == WaitMargin::kMinimum);
    WaitMargin spike;
    for (int i = 0; i < 400; ++i) spike.Add(80000);
    spike.Add(1000000000);
    CHECK(spike.Late() <= 80000 + 1000 + WaitMargin::kSampleLimit / WaitMargin::kGain);
    CHECK(spike.Margin() <= WaitMargin::kMaximum);
    CHECK(margin.Samples() == 460);
}

// pacerfix.2: a 3X unmetered clock (first at readiness, 8 buffers). Batch(): a batch handed over at `at` after the game
// waited `wait` on the pacing (-1 sent when evidence is off): its first output ready `production` later, the other two
// 0.5 ms after the previous release (held to their slots).
struct Clock3X
{
    JitterBufferClock clock;
    int64_t submit = 1000 * kMs, gate = 0, production = 3 * kMs;
    bool evidence = true;
    Clock3X() { clock.SetFirstAtReadiness(true); clock.SetCapacity(8); clock.SetUnmetered(true); }
    // sleepsight.1: host = the host's measured low-latency sleep since the last batch (-1: not measured).
    JitterBufferClock::Release Batch(int64_t at, int64_t wait, int64_t slept = 0, int64_t host = -1)
    {
        submit = at;
        const auto first = clock.Plan(at+production, at, true, 3, gate, false, false, evidence ? wait : -1, 0, slept, host);
        gate = first.ns;
        for (int i = 1; i < 3; ++i) gate = clock.Plan(gate+kMs/2, 0, false, 3, gate).ns;
        return first;
    }
};

// 40 batches 20 ms apart with no wait: 6.667 ms outputs (each first flips at readiness, on the cadence).
Clock3X Primed()
{
    Clock3X c;
    for (int b = 0; b < 40; ++b) c.Batch(c.submit+20*kMs, 0);
    return c;
}

// The interval one learned hand-over gives: before + 1/8 of (sample per output - before), within truncation.
bool Learned(const JitterBufferClock& clock, int64_t before, double handoverNs)
{
    const auto expected = int64_t(double(before)+JitterBufferClock::kRateGain*(handoverNs/3.0-double(before)));
    return std::llabs(clock.Interval()-expected) <= 1;
}

// The lock-in: the game could hand a batch over every 15 ms; held by a clock at 20 ms batches it hands each over just in
// time for its first output to be ready 1 ms before that output's slot, and the host measures the extra wait.
Clock3X LockIn(bool evidence, int batches)
{
    Clock3X c = Primed();
    c.evidence = evidence;
    for (int b = 0; b < batches; ++b)
    {
        const int64_t want = c.submit+15*kMs;
        const int64_t at = (std::max)(want, c.clock.Slot(true)-c.production-kMs);
        c.Batch(at, at-want);
    }
    return c;
}

void Evidence()
{
    const Clock3X primed = Primed();
    const int64_t before = primed.clock.Interval();
    CHECK(before == 6666666);
    // On time (17 ms after the last hand-over, ready before its slot), the game waited 2 ms: learned as 15 ms.
    Clock3X a = primed;
    const auto ra = a.Batch(a.submit+17*kMs, 2*kMs);
    CHECK(ra.coupled && !ra.late && ra.held);
    CHECK(a.clock.Compensated() == 1);
    CHECK(Learned(a.clock, before, 15.0*kMs));
    // A wait under kEvidenceMinimum is no evidence of a hold: learned as it is.
    Clock3X b = primed;
    const auto rb = b.Batch(b.submit+17*kMs, 400000);
    CHECK(!rb.coupled && b.clock.Compensated() == 0);
    CHECK(Learned(b.clock, before, 17.0*kMs));
    // At most half the hand-over is taken off.
    Clock3X c = primed;
    CHECK(c.Batch(c.submit+17*kMs, 12*kMs).coupled);
    CHECK(Learned(c.clock, before, 8.5*kMs));
    // A late first output (the GPU or the game set the cadence) learns the hand-over as it is, whatever the wait below
    // kHeldNumerator/kHeldDenominator of it.
    Clock3X d = primed;
    const auto rd = d.Batch(d.submit+25*kMs, 5*kMs);
    CHECK(rd.late && !rd.coupled && d.clock.Compensated() == 0);
    CHECK(Learned(d.clock, before, 25.0*kMs));
    // pacerfix.3: a late first output whose game waited 2/5 of the hand-over or more was held by the pacing: 15 of 25 ms,
    // taken off up to half (12.5 ms learned).
    Clock3X g = primed;
    const auto rg = g.Batch(g.submit+25*kMs, 15*kMs);
    CHECK(rg.late && rg.coupled && g.clock.Compensated() == 1);
    CHECK(Learned(g.clock, before, 12.5*kMs));
    // pacerfix.3: the low-latency sleep counts whole: 2 ms waited and 1 ms slept of 17 ms -> 14 ms.
    Clock3X h = primed;
    const auto rh = h.Batch(h.submit+17*kMs, 2*kMs, kMs);
    CHECK(rh.coupled && h.clock.Compensated() == 1);
    CHECK(Learned(h.clock, before, 14.0*kMs));
    // With evidence the inference rules do not run: coupled and full set, nothing counted as coupled, deep or refused.
    Clock3X e = primed;
    const int64_t at = e.submit+17*kMs;
    CHECK(e.clock.Plan(at+e.production, at, true, 3, e.gate, true, true, 2*kMs).coupled);
    CHECK(e.clock.Coupled() == 0 && e.clock.Deep() == 0 && e.clock.DeepRefused() == 0 && e.clock.Compensated() == 1);
    e.clock.Reset();
    CHECK(e.clock.Compensated() == 1);
    // The lock-in escape: with evidence the clock reaches the game's 5 ms outputs within 40 batches (mirror: 5.011 ms);
    // without it the clock is still above 5.5 ms (mirror: 5.569 ms; only kFirstGap speeds it up).
    const auto with = LockIn(true, 40);
    const auto without = LockIn(false, 40);
    CHECK(std::llabs(with.clock.Interval()-5*kMs) < 100000);
    CHECK(with.clock.Compensated() > 0);
    CHECK(without.clock.Interval() > 5500000);
    CHECK(without.clock.Compensated() == 0);
}

// Gate-ahead: the meter commits each output's slot (Slot()) before its copy is ready and passes it to Plan as `committed`.
// An inner output's release is exactly the clock's own rule; a first output's slot predates learning its batch's
// hand-over, so it may differ by that change, and the learned interval is the same either way. 4X around 16 ms batches
// with jitter, outputs often late, 0.3 ms pass latency, a V-Sync floor in the second half.
void Ahead()
{
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(8);
    clock.SetUnmetered(true);
    clock.SetPassLatency(300000);
    uint32_t random = 7u;
    const auto next = [&random](int64_t low, int64_t high) {
        random = random*1664525u+1013904223u;
        return low+int64_t(uint64_t(random >> 8)%uint64_t(high-low+1));
    };
    int64_t submit = 1000 * kMs;
    bool innerSame = true, firstRule = true, intervalSame = true;
    int moved = 0, outputs = 0;
    for (int batch = 0; batch < 300; ++batch)
    {
        if (batch == 150) clock.SetFloor(8333333);
        submit += 16*kMs+next(-3*kMs, 3*kMs);
        int64_t ready = submit+4*kMs+next(-2*kMs, 2*kMs), previous = 0;
        for (uint32_t i = 0; i < 4; ++i, ++outputs)
        {
            const bool first = i == 0;
            if (!first) ready = previous+next(200000, 8*kMs);
            const int64_t slot = clock.Slot(first);
            const int64_t committed = slot <= 0 ? 0
                : ready < slot ? (std::min)(slot, ready+JitterBufferClock::kMaximumHold) : slot;
            JitterBufferClock twin = clock;
            const auto ra = clock.Plan(ready, first ? submit : 0, first, 4, previous, false, false, -1, committed);
            const auto rb = twin.Plan(ready, first ? submit : 0, first, 4, previous);
            intervalSame = intervalSame && ra.interval == rb.interval;
            if (first)
            {
                const int64_t want = committed
                    ? (std::min)((std::max)(ready, committed), ready+JitterBufferClock::kMaximumHold) : rb.ns;
                firstRule = firstRule && ra.ns == want;
                moved += ra.ns != rb.ns;
            }
            else innerSame = innerSame && ra.ns == rb.ns;
            previous = ra.ns;
        }
    }
    CHECK(outputs == 1200);
    CHECK(innerSame);
    CHECK(firstRule && intervalSame);
    CHECK(moved > 0 && moved < 300);   // mirror: 132
    CHECK(clock.Interval() == 8333333); // the floor
}

// A batch NVIDIA's driver meters is gated at its first output only; the rest flip ungated one interval apart
// (AdvanceUngated). 4X, a hand-over every 20 ms, each first output ready 2 ms after it.
void Ungated()
{
    JitterBufferClock fresh;
    fresh.AdvanceUngated(3);
    CHECK(fresh.Slot(true) == 0 && fresh.Slot(false) == 0);
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(8);
    int64_t submit = 1000 * kMs, last = 0, previous = 0;
    bool spaced = true;
    for (int batch = 0; batch < 40; ++batch)
    {
        submit += 20*kMs;
        const int64_t ready = submit+2*kMs;
        const int64_t slot = clock.Slot(true);
        const int64_t committed = slot <= 0 ? 0
            : ready < slot ? (std::min)(slot, ready+JitterBufferClock::kMaximumHold) : slot;
        last = clock.Plan(ready, submit, true, 4, 0, false, false, -1, committed).ns;
        clock.AdvanceUngated(3);
        if (batch >= 20) spaced = spaced && last-previous == 20*kMs;
        previous = last;
    }
    const int64_t interval = clock.Interval();
    CHECK(interval == 5*kMs);
    CHECK(spaced);
    // The next first output's slot follows the batch's three ungated outputs and kFirstGap; an inner one a full interval.
    CHECK(clock.Slot(true) == last+3*interval+int64_t(JitterBufferClock::kFirstGap*double(interval)));
    CHECK(clock.Slot(false) == last+4*interval);
}

// pacerfix.3: a logged Subnautica 2 start (6X on 3 native buffers, NVIDIA metered from gate 19): each output's readiness,
// its batch's hand-over and the game's measured wait (-1: inner outputs), less 952957 s. The large waits are an older
// rule's holds: it took three hitches in a row for a lasting slowdown and stayed at 16-18.5 ms outputs.
struct LoggedGate { int gate, index, outputs; int64_t ready, submit, wait; bool metered; };
constexpr LoggedGate kStart[] = {
    { 1, 0, 6,  425245200,  423524500,          0, false},
    { 2, 1, 6,  454121800,          0,         -1, false},
    { 3, 2, 6,  471644200,          0,         -1, false},
    { 4, 3, 6,  483141200,          0,         -1, false},
    { 5, 4, 6,  495656500,          0,         -1, false},
    { 6, 5, 6,  511671200,          0,         -1, false},
    { 7, 0, 6,  561366700,  472833300,          0, false},
    { 8, 1, 6,  602468800,          0,         -1, false},
    { 9, 2, 6,  616395600,          0,         -1, false},
    {10, 3, 6,  627245400,          0,         -1, false},
    {11, 4, 6,  637680800,          0,         -1, false},
    {12, 5, 6,  653730500,          0,         -1, false},
    {13, 0, 6,  676996200,  640131400,   34128829, false},
    {14, 1, 6,  715648700,          0,         -1, false},
    {15, 2, 6,  731837800,          0,         -1, false},
    {16, 3, 6,  742461300,          0,         -1, false},
    {17, 4, 6,  758995600,          0,         -1, false},
    {18, 5, 6,  782571100,          0,         -1, false},
    {19, 0, 6,  805635300,  732855500,  106270132, true},
    {20, 0, 6,  916209900,  844371100,   81910833, true},
    {21, 0, 6, 1026105000,  962226900,   88972038, true},
    {22, 0, 6, 1142339700, 1070492900,   93902530, true},
    {23, 0, 6, 1250782300, 1189540900,   69338128, true},
    {24, 0, 6, 1377233000, 1303830600,   91198332, true},
    {25, 0, 6, 1511539800, 1434932400,   74221236, true},
    {26, 0, 6, 1621095000, 1548798500,   93861039, true},
    {27, 0, 6, 1735825400, 1679801200,   78816729, true},
    {28, 0, 6, 1836507200, 1778655800,   94402428, true},
    {29, 0, 6, 1970524000, 1899738900,   85152538, true},
    {30, 0, 6, 2061072200, 2007401500,   76263231, true},
    {31, 0, 6, 2203757400, 2129348100,   80830033, true},
    {32, 0, 6, 2317923700, 2238831300,   71126630, true},
    {33, 0, 6, 2439387400, 2365075800,   84497138, true},
    {34, 0, 6, 2550431600, 2474888700,   92522133, true},
    {35, 0, 6, 2664069200, 2597300600,   82978043, true},
    {36, 0, 6, 2780020200, 2706394000,   96342138, true},
};

void Startup()
{
    constexpr int64_t kBase = 952957000000000;
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(3);
    int64_t previous = 0, highest = 0;
    for (const auto& g : kStart)
    {
        const bool first = g.index == 0;
        const int64_t ready = kBase+g.ready;
        const auto r = clock.Plan(ready, first ? kBase+g.submit : 0, first, uint32_t(g.outputs), previous, false, false,
            first ? g.wait : -1, Committed(clock, first, ready));
        if (g.metered && first) clock.AdvanceUngated(uint32_t(g.outputs-1));
        previous = r.ns;
        if (first) highest = (std::max)(highest, r.interval);
    }
    // Never restarted at the held cadence: under 10 ms outputs (mirror: 9.554 at most), the wait taken on 18 first outputs.
    CHECK(highest < 10*kMs);
    CHECK(clock.Interval() > 9*kMs && clock.Interval() < 10*kMs);
    CHECK(clock.Compensated() == 18);
}

// pacerfix.3: free-running 4X metered, a batch every 6 ms (1.5 ms outputs) with jitter and a 0.2-1.7 ms wait in XeFG's
// Present that no hold of ours caused: the rate stays within 3% of the game's and the baseline near the wait's mean.
void Baseline()
{
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(8);
    Lcg random{3};
    int64_t at = 1000*kMs;
    for (int n = 0; n < 600; ++n)
    {
        at += 6*kMs+random.Next(-300000, 300000);
        const int64_t wait = random.Next(200000, 1700000);
        const int64_t ready = at+400000+random.Next(0, 300000);
        clock.Plan(ready, at, true, 4, 0, false, false, wait, Committed(clock, true, ready));
        clock.AdvanceUngated(3);
    }
    // mirror: 1491463 ns and a baseline of 846666 ns (the wait's mean 0.95 ms).
    CHECK(std::llabs(clock.Interval()-1500000) < 45000);
    CHECK(clock.WaitBaseline() > 700000 && clock.WaitBaseline() < 1300000);
}

// pacerfix.3: a game that hands its batches over at the floor (the Vulkan harness at 6X: 6 outputs every 5.9 ms) is not a
// runaway, so the rate is never restarted (mirror: 0 resets, 1000000 ns).
void Floor()
{
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(10);
    Lcg random{5};
    int64_t at = 1000*kMs;
    for (int n = 0; n < 800; ++n)
    {
        at += 5900000+random.Next(-150000, 150000);
        const int64_t ready = at+5*kMs;
        clock.Plan(ready, at, true, 6, 0, false, false, random.Next(200000, 500000), Committed(clock, true, ready));
        clock.AdvanceUngated(5);
    }
    CHECK(clock.FloorResets() == 0);
    CHECK(clock.Interval() >= 1000000 && clock.Interval() < 1050000);
}

// pacerfix.3: a restarted rate takes the smaller of its first two hand-overs: a 155 ms stall right after a reset is not
// the game's cadence. 6X, 12 ms batches.
void Relearn()
{
    JitterBufferClock clock;
    clock.SetFirstAtReadiness(true);
    clock.SetCapacity(10);
    int64_t at = 1000*kMs;
    const auto batch = [&clock, &at](int64_t step) {
        at += step;
        clock.Plan(at+2*kMs, at, true, 6, 0, false, false, 300000, 0);
        clock.AdvanceUngated(5);
    };
    for (int n = 0; n < 30; ++n) batch(12*kMs);
    CHECK(clock.Interval() == 2*kMs);
    clock.Reset();
    batch(12*kMs);   // no hand-over before it: no rate
    batch(155*kMs);  // the stall: kept, not yet the rate
    CHECK(clock.Interval() == 0);
    batch(12*kMs);   // the smaller of the two
    CHECK(clock.Interval() == 2*kMs);
    // A live count change between the two rescales the kept one (per output).
    clock.Reset();
    batch(12*kMs);
    batch(12*kMs);   // kept: 2 ms an output at 6X
    at += 12*kMs;
    clock.Plan(at+2*kMs, at, true, 4, 0, false, false, 300000, 0); // 4X: 3 ms an output, the kept one rescaled to 3 ms
    CHECK(clock.Interval() == 3*kMs);
}

// heldprobe.1: Evidence's lock-in where the host sees none of the hold (OptiScaler's route: the real frame's own hold is
// 0.2 ms, under kEvidenceMinimum). The game could hand over every 15 ms but times each batch to the slot, 1 ms early.
Clock3X UnseenLock(int batches, int64_t floor = 0)
{
    Clock3X c = Primed();
    if (floor) c.clock.SetFloor(floor);
    for (int b = 0; b < batches; ++b)
    {
        const int64_t want = c.submit+15*kMs;
        const int64_t at = (std::max)(want, c.clock.Slot(true)-c.production-kMs);
        c.Batch(at, 200000);
    }
    return c;
}

void Held()
{
    // The probe reaches the game's 5 ms outputs within 40 batches (mirror: 5000450, 7 decays), nothing compensated.
    const auto forty = UnseenLock(40);
    CHECK(std::llabs(forty.clock.Interval()-5*kMs) < 150000);
    CHECK(forty.clock.Deep() > 0 && forty.clock.Compensated() == 0 && forty.clock.Coupled() == 0);
    // And stays at the game's rate: past it, the first outputs are late and the hand-over is learned (mirror: 4999999).
    const auto longRun = UnseenLock(400);
    CHECK(longRun.clock.Interval() > 4700000 && longRun.clock.Interval() < 5200000);
    // Never under the V-Sync floor (120 Hz is slower than the game): no decay goes below it (mirror: none).
    const auto vsync = UnseenLock(300, 8333333);
    CHECK(vsync.clock.Interval() >= 8333333 && vsync.clock.Deep() == 0);
}

// amdmodel.1: AMD on OptiScaler's route as a closed loop, gating a regression that lost 12-22% output fps at 6X. Nothing
// is metered; the real frame's own hold is 0.2 ms (under kEvidenceMinimum).
struct Clock6X
{
    JitterBufferClock clock;
    int64_t submit = 1000 * kMs, gate = 0, production = 12 * kMs;
    Clock6X() { clock.SetFirstAtReadiness(true); clock.SetCapacity(10); clock.SetUnmetered(true); }
    JitterBufferClock::Release Batch(int64_t at, int64_t wait, int64_t host = -1)
    {
        submit = at;
        const auto first = clock.Plan(at+production, at, true, 6, gate, false, false, wait, 0, 0, host);
        gate = first.ns;
        for (int i = 1; i < 6; ++i) gate = clock.Plan(gate+kMs/2, 0, false, 6, gate).ns;
        return first;
    }
};

// The lock: the game could hand over every `capacity`, but its measured XeLL / Anti-Lag 2 sleep times each batch to the
// slot, `margin` (+-noise) early. Returns the excess over the game's rate in the first 80 batches and the last 200.
struct LockResult { double first80{}, tail{}; uint64_t deep{}; };
template<class Clock> LockResult AmdLock(Clock c, int64_t capacity, int64_t start, int64_t margin, int64_t noise = 0)
{
    Lcg rng{3};
    for (int b = 0; b < 40; ++b) c.Batch(c.submit+start, 0);
    double extra = 0.0, tail = 0.0;
    for (int b = 0; b < 400; ++b)
    {
        const int64_t want = c.submit+capacity;
        const int64_t at = (std::max)(want, c.clock.Slot(true)-c.production-margin+(noise ? rng.Next(-noise, noise) : 0));
        if (b < 80) extra += double(at-c.submit);
        if (b >= 200) tail += double(at-c.submit);
        if constexpr (std::is_same_v<Clock, Clock3X>) c.Batch(at, 200000, 0, at-want);
        else c.Batch(at, 200000, at-want);
    }
    return {extra/80.0/double(capacity)-1.0, tail/200.0/double(capacity)-1.0, c.clock.Deep()};
}

// GPU-bound: the GPU sets an 18 ms cadence (+-jitter) whatever our queue does. sleep: the host's measured sleep (0: Reflex
// off; else XeLL's ~6 ms +-1). The clock's interval over batches 200-799 against the GPU's 6 ms outputs.
struct GpuResult { double mean{}, low{}; uint64_t deep{}, suppressed{}; };
GpuResult GpuBound(int64_t jitter, int64_t sleep)
{
    Lcg rng{7};
    Clock3X c;
    for (int b = 0; b < 40; ++b) c.Batch(c.submit+18*kMs, 0);
    double sum = 0.0;
    int64_t low = INT64_MAX;
    for (int b = 0; b < 800; ++b)
    {
        const int64_t at = c.submit+18*kMs+rng.Next(-jitter, jitter);
        c.production = 3*kMs+rng.Next(-500000, 500000);
        const int64_t host = sleep ? sleep+rng.Next(-kMs, kMs) : 0;
        c.Batch(at, 200000, 0, host);
        if (b >= 200) { sum += double(c.clock.Interval()); low = (std::min)(low, c.clock.Interval()); }
    }
    return {sum/600.0/double(6*kMs)-1.0, double(low)/double(6*kMs)-1.0, c.clock.Deep(), c.clock.HeldSuppressed()};
}

void AmdModel()
{
    // The lock escapes: mirror 2.1-2.9% over the game's rate in the first 80 batches, then holds it (0.00%).
    const LockResult locks[] = {
        AmdLock(Clock3X{}, 15*kMs, 20*kMs, kMs),
        AmdLock(Clock3X{}, 15*kMs, 20*kMs, 3*kMs),
        AmdLock(Clock6X{}, 31*kMs, 40500000, kMs),
        AmdLock(Clock6X{}, 31*kMs, 40500000, 3*kMs),
        AmdLock(Clock6X{}, 31*kMs, 40500000, kMs, 700000),
    };
    for (const auto& lock : locks)
    {
        CHECK(lock.first80 < 0.035);
        CHECK(std::fabs(lock.tail) < 0.01);
        CHECK(lock.deep > 0);
    }
    // GPU-bound with XeLL on: the clock keeps the GPU's rate (mirror +-1 ms: -0.16%, low -2.2%; +-2 ms: -0.53%, -7.9%).
    const auto gpu1 = GpuBound(kMs, 6*kMs);
    CHECK(gpu1.mean > -0.005 && gpu1.low > -0.03 && gpu1.deep <= 2);
    const auto gpu2 = GpuBound(2*kMs, 6*kMs);
    CHECK(gpu2.mean > -0.01 && gpu2.low > -0.09 && gpu2.deep <= 20);
    // Reflex off (the host measured no sleep): no held probe at all (mirror: 43 suppressed).
    const auto off = GpuBound(2*kMs, 0);
    CHECK(off.deep == 0 && off.suppressed > 0 && off.mean > -0.01);
    // A 60 FPS cap through the game's Reflex: the game's frame takes 10 ms, the cap sleeps it the rest (measured). The
    // clock stays at the cap's 5.556 ms outputs (mirror: mean +0.00%, lowest -0.25%, no decay).
    {
        Lcg rng{11};
        Clock3X c;
        for (int b = 0; b < 40; ++b) c.Batch(c.submit+16666667, 0);
        double sum = 0.0;
        int64_t low = INT64_MAX;
        for (int b = 0; b < 600; ++b)
        {
            c.Batch(c.submit+16666667+rng.Next(-100000, 100000), 200000, 0, 16666667-10*kMs);
            if (b >= 100) { sum += double(c.clock.Interval()); low = (std::min)(low, c.clock.Interval()); }
        }
        CHECK(sum/500.0 > 0.99*5555556.0 && double(low) > 0.97*5555556.0 && c.clock.Deep() == 0);
    }
    // UnseenLock's dynamics with the sleep measured as zero: the probe does not run (mirror: 57 suppressed).
    Clock3X z = Primed();
    for (int b = 0; b < 80; ++b)
    {
        const int64_t want = z.submit+15*kMs;
        z.Batch((std::max)(want, z.clock.Slot(true)-z.production-kMs), 200000, 0, 0);
    }
    CHECK(z.clock.Deep() == 0 && z.clock.HeldSuppressed() > 0);
}
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : nullptr;
    const auto run = [&](const char* name, void (*test)()) { if (!only || std::strcmp(only, name) == 0) test(); };
    run("small", SmallChainRule);
    run("pass", PassLatencySpacing);
    run("lead", GateLead);
    run("margin", Margin);
    run("evidence", Evidence);
    run("ahead", Ahead);
    run("ungated", Ungated);
    run("startup", Startup);
    run("baseline", Baseline);
    run("floor", Floor);
    run("relearn", Relearn);
    run("held", Held);
    run("amd", AmdModel);
    std::printf("flip queue: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
