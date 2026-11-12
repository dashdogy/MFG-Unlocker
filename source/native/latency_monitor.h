#pragma once
#include "latency_probe_ring.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// latmon.2: the live PC-latency estimator behind the menu's Latency row. latency_probe.cpp feeds it the probe's
// events (latency_probe_ring.h) on the backend's 100 ms loop and publishes Read() for the status file. No Windows
// calls (tests/xemfg_standalone); tests/analyze_latency_probe.py applies the same rules offline.
//
// - A real frame: the game's Present on its own chain (frame generation's hand-over), with the frame of the newest
//   present-start marker set before that Present began (on its thread when that thread sets them). Its
//   start: that frame's simulation-start marker.
// - Its outputs: XeFG's Presents labelled with the frame whose real output the game's Present presented; otherwise the
//   output Presents from this hand-over to the next (DLSS-G's presenter beneath Streamline presents in order, so an
//   output of a later frame closes the earlier ones).
// - An output's flip: a later frame-statistics sample naming its present count. Exact when the sample's sync refresh is
//   the refresh it reports; otherwise estimated from the refresh period of exact samples.
// - Latency: start -> the first output's flip (and -> the real, last output's flip); parts: game (start -> hand-over),
//   frame generation (hand-over -> the first output's Present), display (that Present -> its flip).
namespace latency_monitor
{
using latency_probe::Event;
using latency_probe::Kind;

enum class State : uint32_t
{
    eOff = 0,         // not running (nothing in the menu or pinned wants it)
    eMeasuring = 1,
    eLive = 2,
    eNoMarkers = 3,   // the game sends no Reflex simulation-start markers we can see
    eNoOutputs = 4,   // no output Presents we can time on this route
    eNoFlipTimes = 5, // outputs, but Windows reports no flip times for them
};

struct Stats
{
    State state = State::eOff;
    uint32_t frames = 0;          // real frames in the window
    uint32_t exactPermille = 0;   // of those, first flips read exactly
    int64_t meanFirst = 0, p99First = 0, meanReal = 0, meanGame = 0, meanFg = 0, meanDisplay = 0; // QPC ticks
    int64_t newest = 0;           // the newest frame's first flip (QPC), 0 none
    bool widened = false;         // latmon.4: too few frames in the last second; the means are over kTailSeconds
};

class Estimator
{
public:
    static constexpr size_t kOutputs = 256, kFrames = 64, kSims = 128, kSamples = 512, kThreads = 8, kPeriods = 16,
        kMarks = 32;
    static constexpr uint32_t kMinimumFrames = 5;

    // Forgets every frame and sample; keeps SetFrequency's timings.
    void Reset() noexcept
    {
        const int64_t wait = waitTicks_, stale = staleTicks_;
        *this = Estimator{};
        waitTicks_ = wait;
        staleTicks_ = stale;
    }

    // latmon.4: after a gap in the events (the probe's queue refused some), forgets the frames, outputs and markers
    // still being paired, which the gap could pair wrongly, and keeps the measured frames: the readout stays.
    void Resync() noexcept
    {
        outputs_ = {};
        frames_ = {};
        threads_ = {};
        marks_ = {};
        hasAnchor_ = false;
        lastExactChain_ = 0;
    }

    void Feed(const Event& e) noexcept
    {
        if (!activeSince_) activeSince_ = e.qpc;
        if (e.qpc > newest_) newest_ = e.qpc;
        switch (e.kind)
        {
        case Kind::eMarker: OnMarker(e); break;
        case Kind::eGamePresent: OnHandOver(e); break;
        case Kind::eOutputPresent: OnOutput(e); break;
        case Kind::eFrameStats: OnStats(e); break;
        case Kind::eXefgOutput:
        {
            auto& slot = Thread(e.thread);
            slot.label = e.a;
            slot.labelGenerated = e.sub != 0;
            slot.hasLabel = true;
            break;
        }
        default: break;
        }
    }

    // `now` and `frequency`: QueryPerformanceCounter's. The 1% high is over the last kTailSeconds (a second holds too
    // few frames for a percentile); the means over the last second of flips, or over kTailSeconds with fewer than
    // kMinimumFrames in it (a hitch, a menu), so the readout holds through a short gap rather than going to Measuring.
    static constexpr int64_t kTailSeconds = 5;
    Stats Read(int64_t now, int64_t frequency) const noexcept
    {
        Stats stats{};
        if (!activeSince_ || frequency <= 0) { stats.state = State::eMeasuring; return stats; }
        struct Window
        {
            int64_t sumFirst = 0, sumReal = 0, sumGame = 0, sumFg = 0, sumDisplay = 0, newest = 0;
            uint32_t count = 0, reals = 0, exact = 0;
            void Add(const Sample& sample) noexcept
            {
                ++count;
                sumFirst += sample.first; sumGame += sample.game; sumFg += sample.fg; sumDisplay += sample.display;
                if (sample.real >= 0) { sumReal += sample.real; ++reals; }
                if (sample.exact) ++exact;
                newest = std::max(newest, sample.flip);
            }
        };
        std::array<int64_t, kSamples> tail{};
        Window second{}, tailWindow{};
        uint32_t tailCount = 0;
        for (const auto& sample : samples_)
        {
            if (!sample.used || sample.flip > now + frequency || sample.flip < now - kTailSeconds * frequency) continue;
            tail[tailCount++] = sample.first;
            tailWindow.Add(sample);
            if (sample.flip >= now - frequency) second.Add(sample);
        }
        const bool widened = second.count < kMinimumFrames;
        const Window& window = widened ? tailWindow : second;
        stats.frames = window.count;
        if (window.count >= kMinimumFrames)
        {
            const uint32_t count = window.count;
            stats.state = State::eLive;
            stats.widened = widened;
            stats.newest = window.newest;
            stats.meanFirst = window.sumFirst / count;
            stats.meanGame = window.sumGame / count;
            stats.meanFg = window.sumFg / count;
            stats.meanDisplay = window.sumDisplay / count;
            stats.meanReal = window.reals ? window.sumReal / window.reals : 0;
            stats.exactPermille = window.exact * 1000u / count;
            const size_t rank = (static_cast<size_t>(tailCount) * 99 + 99) / 100 - 1; // 99th percentile, nearest rank
            std::nth_element(tail.begin(), tail.begin() + rank, tail.begin() + tailCount);
            stats.p99First = tail[rank];
            return stats;
        }
        const int64_t quiet = 2 * frequency;
        const auto silent = [&](int64_t last) { return !last || now - last > quiet; };
        if (now - activeSince_ <= quiet) stats.state = State::eMeasuring;
        else if (silent(lastHandOver_) || silent(lastOutput_)) stats.state = State::eNoOutputs;
        else if (silent(lastSimStart_)) stats.state = State::eNoMarkers;
        else if (silent(lastFlip_)) stats.state = State::eNoFlipTimes;
        else stats.state = State::eMeasuring;
        return stats;
    }

    // Diagnostics (the log): frames measured and dropped, outputs never matched, flips estimated. latmon.4 splits
    // `implausible` by reason: the first flip or the hand-over before the frame's start (beforeStart), the first
    // output before the hand-over or its flip before its Present (beforePresent), over a second (tooLong); `clamped`:
    // flips named in a refresh that began before their Present (immediate, tearing flips), placed at the Present.
    struct Counters
    {
        uint64_t frames, noStart, noFlip, noOutputs, implausible, overwritten, unassigned, inferred, interpolated;
        uint64_t beforeStart, beforePresent, tooLong, clamped;
    };
    Counters ReadCounters() const noexcept { return counters_; }

private:
    struct Output
    {
        uint64_t id = 0, chain = 0, label = 0, frame = 0;
        int64_t entry = 0, returned = 0, flip = 0;
        uint32_t thread = 0, count = 0;
        bool labelled = false, generated = false, exact = false;
    };
    struct Frame
    {
        uint64_t seq = 0, token = 0, first = 0, last = 0;
        int64_t handOver = 0, returned = 0, firstEntry = 0, lastEntry = 0;
        uint32_t thread = 0;
        bool hasToken = false, closed = false, done = false;
    };
    struct Sim { uint64_t token = 0; int64_t qpc = 0; bool used = false; };
    // A present-start marker: the frame the game's next Present carries.
    struct Mark { uint64_t token = 0; int64_t qpc = 0; uint32_t thread = 0; bool used = false, taken = false; };
    struct ThreadSlot
    {
        uint32_t thread = 0;
        uint64_t label = 0, lastOutput = 0;
        bool used = false, hasLabel = false, labelGenerated = false;
    };
    struct Sample { int64_t flip = 0, first = 0, real = -1, game = 0, fg = 0, display = 0; bool exact = false, used = false; };

    ThreadSlot& Thread(uint32_t thread) noexcept
    {
        for (auto& slot : threads_) if (slot.used && slot.thread == thread) return slot;
        auto& slot = threads_[nextThread_++ % kThreads];
        slot = ThreadSlot{};
        slot.used = true;
        slot.thread = thread;
        return slot;
    }
    Output* FindOutput(uint64_t id) noexcept
    {
        if (!id) return nullptr;
        auto& output = outputs_[id % kOutputs];
        return output.id == id ? &output : nullptr;
    }
    void OnMarker(const Event& e) noexcept
    {
        if (e.sub == 0) // simulation start: the first one per frame counts
        {
            for (auto& sim : sims_)
            {
                if (!sim.used || sim.token != e.a) continue;
                // latmon.4: the same frame id over a second later is a new frame (the game reused the id).
                if (e.qpc - sim.qpc > staleTicks_) { sim.qpc = e.qpc; lastSimStart_ = e.qpc; }
                return;
            }
            sims_[nextSim_++ % kSims] = Sim{e.a, e.qpc, true};
            lastSimStart_ = e.qpc;
        }
        else if (e.sub == 4) // present start: the frame the game's next Present carries
            marks_[nextMark_++ % kMarks] = Mark{e.a, e.qpc, e.thread, true, false};
    }

    // latmon.4: the newest present-start marker set before this Present began, on its own thread when that thread sets
    // them: the Present's event arrives when the call returns, and a game that sets its markers on another thread may
    // by then have set the next frame's. With none before it, the first set while the call ran. It and every older
    // marker are taken.
    bool TakeMark(const Event& e, uint64_t& token) noexcept
    {
        const auto newest = [&](bool sameThread) {
            Mark* pick = nullptr;
            for (auto& mark : marks_)
            {
                if (!mark.used || mark.taken || mark.qpc > e.qpc || (sameThread && mark.thread != e.thread)) continue;
                if (!pick || mark.qpc > pick->qpc) pick = &mark;
            }
            return pick;
        };
        Mark* pick = newest(true);
        if (!pick) pick = newest(false);
        if (!pick)
        {
            const int64_t returned = static_cast<int64_t>(e.a);
            for (auto& mark : marks_)
                if (mark.used && !mark.taken && mark.qpc > e.qpc && mark.qpc <= returned && (!pick || mark.qpc < pick->qpc))
                    pick = &mark;
        }
        if (!pick) return false;
        token = pick->token;
        const int64_t taken = pick->qpc;
        for (auto& mark : marks_) if (mark.used && mark.qpc <= taken) mark.taken = true;
        return true;
    }

    void OnHandOver(const Event& e) noexcept
    {
        if (e.sub & latency_probe::kRoleNested) return;
        Frame frame{};
        frame.seq = nextFrame_++;
        frame.handOver = e.qpc;
        frame.returned = static_cast<int64_t>(e.a);
        frame.thread = e.thread;
        frame.hasToken = TakeMark(e, frame.token);
        auto& place = frames_[frame.seq % kFrames];
        if (place.seq && !place.done) ++counters_.overwritten;
        place = frame;
        lastHandOver_ = e.qpc;
        AssignLabelled(place);
        AssignWindows(e.qpc);
        Complete();
    }

    void OnOutput(const Event& e) noexcept
    {
        const bool xefg = (e.sub & latency_probe::kRoleXefgOutput) != 0;
        if (!xefg && !(e.sub & latency_probe::kRoleStreamlineBase)) return;
        Output output{};
        output.id = nextOutput_++;
        output.chain = e.c;
        output.entry = e.qpc;
        output.returned = static_cast<int64_t>(e.a);
        output.thread = e.thread;
        auto& slot = Thread(e.thread);
        if (xefg && slot.hasLabel)
        {
            output.labelled = true;
            output.label = slot.label;
            output.generated = slot.labelGenerated;
            slot.hasLabel = false;
        }
        auto& place = outputs_[output.id % kOutputs];
        if (place.id && !place.frame) ++counters_.unassigned;
        place = output;
        slot.lastOutput = output.id;
        lastOutput_ = e.qpc;
    }

    void OnStats(const Event& e) noexcept
    {
        auto& slot = Thread(e.thread);
        Output* output = FindOutput(slot.lastOutput);
        slot.lastOutput = 0;
        if (!output) return;
        if (e.sub & latency_probe::kStatsCount) output->count = static_cast<uint32_t>(e.a >> 32);
        if (!(e.sub & latency_probe::kStatsFrame)) return;
        const uint32_t present = static_cast<uint32_t>(e.a & 0xFFFFFFFFu);
        const uint64_t refresh = e.b, syncRefresh = e.c;
        const int64_t syncQpc = static_cast<int64_t>(e.d);
        if (!present || syncQpc <= 0) return;
        const bool exact = refresh == syncRefresh;
        if (exact)
        {
            if (lastExactChain_ == output->chain && syncRefresh > lastExactRefresh_ && syncQpc > lastExactQpc_)
                periods_[nextPeriod_++ % kPeriods] =
                    (syncQpc - lastExactQpc_) / static_cast<int64_t>(syncRefresh - lastExactRefresh_);
            lastExactChain_ = output->chain;
            lastExactRefresh_ = syncRefresh;
            lastExactQpc_ = syncQpc;
        }
        int64_t flip = 0;
        if (exact) flip = syncQpc;
        else if (syncRefresh > refresh)
        {
            const int64_t period = Period();
            if (period > 0) flip = syncQpc - static_cast<int64_t>(syncRefresh - refresh) * period;
        }
        if (flip <= 0) return;
        bool anchor = exact;
        // The output that present count belongs to, newest first.
        for (uint64_t id = nextOutput_ - 1; id && id + kOutputs >= nextOutput_; --id)
        {
            Output* candidate = FindOutput(id);
            if (!candidate || candidate->chain != output->chain || candidate->count != present) continue;
            int64_t placed = flip;
            bool placedExact = exact;
            if (placed < candidate->entry)
            {
                // latmon.4: the refresh named began before this Present did: an immediate (tearing) flip inside it,
                // shown by the time of this sample. Within one refresh it is placed at the Present's return (the
                // earliest the image can be up); further off, the sample is not this present's.
                const int64_t period = Period();
                const int64_t tolerance = period > 0 ? period : staleTicks_ / 50;
                if (candidate->entry - placed <= tolerance)
                {
                    placed = std::min(std::max(candidate->returned, candidate->entry), e.qpc);
                    placedExact = false;
                    ++counters_.clamped;
                }
                else
                    placed = 0;
                anchor = false;
            }
            if (placed && (!candidate->flip || (placedExact && !candidate->exact)))
            {
                if (!exact && !candidate->flip) ++counters_.inferred;
                candidate->flip = placed;
                candidate->exact = placedExact;
                lastFlip_ = e.qpc;
            }
            break;
        }
        if (anchor) Interpolate(output->chain, present, syncQpc, e.qpc);
        Complete();
    }

    // latmon.3: a statistics sample names only the latest flip. DLSS-G hands a whole batch over at once and the driver
    // meters its flips, so the samples after each Present see about one flip per batch, rarely a batch's first. The
    // presents between two exact anchors are spread evenly from the last placed flip to the newer anchor, never before
    // their Present began: after a long game frame the presenter idles, so such a present flips at its Present and the
    // rest spread from there.
    void Interpolate(uint64_t chain, uint32_t present, int64_t flip, int64_t now) noexcept
    {
        if (hasAnchor_ && anchorChain_ == chain && present > anchorCount_)
        {
            const uint32_t gap = present - anchorCount_;
            const int64_t span = flip - anchorFlip_;
            // At most 32 presents apart, 0.25-100 ms per present.
            if (gap >= 2 && gap <= 32 && span >= static_cast<int64_t>(gap) * (staleTicks_ / 4000)
                && span <= static_cast<int64_t>(gap) * (staleTicks_ / 10))
            {
                std::array<Output*, 32> between{};
                size_t count = 0;
                for (uint64_t id = nextOutput_ - 1; id && id + kOutputs >= nextOutput_; --id)
                {
                    Output* candidate = FindOutput(id);
                    if (!candidate || candidate->chain != chain || candidate->count <= anchorCount_
                        || candidate->count >= present) continue;
                    if (count < between.size()) between[count++] = candidate;
                }
                std::sort(between.begin(), between.begin() + static_cast<std::ptrdiff_t>(count),
                    [](const Output* left, const Output* right) { return left->count < right->count; });
                uint32_t fromCount = anchorCount_;
                int64_t fromFlip = anchorFlip_;
                for (size_t index = 0; index < count; ++index)
                {
                    Output* candidate = between[index];
                    if (candidate->flip)
                    {
                        // Known already (its own sample): the spread continues from it.
                        if (candidate->flip >= fromFlip && candidate->flip <= flip)
                        {
                            fromCount = candidate->count;
                            fromFlip = candidate->flip;
                        }
                        continue;
                    }
                    int64_t placed = fromFlip + (flip - fromFlip) * static_cast<int64_t>(candidate->count - fromCount)
                        / static_cast<int64_t>(present - fromCount);
                    placed = std::max(placed, candidate->entry);
                    if (placed > flip) continue;
                    candidate->flip = placed;
                    candidate->exact = false;
                    ++counters_.interpolated;
                    lastFlip_ = now;
                    fromCount = candidate->count;
                    fromFlip = placed;
                }
            }
        }
        if (!hasAnchor_ || anchorChain_ != chain || present > anchorCount_)
        {
            hasAnchor_ = true;
            anchorChain_ = chain;
            anchorCount_ = present;
            anchorFlip_ = flip;
        }
    }

    int64_t Period() const noexcept
    {
        std::array<int64_t, kPeriods> values{};
        size_t count = 0;
        for (const auto value : periods_) if (value > 0) values[count++] = value;
        if (!count) return 0;
        std::nth_element(values.begin(), values.begin() + count / 2, values.begin() + count);
        return values[count / 2];
    }

    void Attach(Output& output, Frame& frame) noexcept
    {
        output.frame = frame.seq;
        if (!frame.first || output.entry < frame.firstEntry) { frame.first = output.id; frame.firstEntry = output.entry; }
        if (!frame.last || output.entry >= frame.lastEntry) { frame.last = output.id; frame.lastEntry = output.entry; }
        // One presenter presents in order: an output of this frame closes every earlier one.
        for (auto& older : frames_) if (older.seq && older.seq < frame.seq) older.closed = true;
    }

    // XeFG: the batch whose real output the game's Present presented (on its thread, inside the call).
    void AssignLabelled(Frame& frame) noexcept
    {
        bool found = false;
        uint64_t label = 0;
        for (const auto& output : outputs_)
            if (output.id && !output.frame && output.labelled && !output.generated && output.thread == frame.thread
                && output.entry >= frame.handOver && output.entry <= frame.returned)
            { label = output.label; found = true; break; }
        if (!found) return;
        for (auto& output : outputs_)
            if (output.id && !output.frame && output.labelled && output.label == label) Attach(output, frame);
        frame.closed = true;
    }

    // Other outputs: once a later hand-over exists, the latest hand-over at or before an output owns it (hand-overs
    // come from one game thread in order, so none before it can still arrive).
    void AssignWindows(int64_t newestHandOver) noexcept
    {
        for (auto& output : outputs_)
        {
            if (!output.id || output.frame || output.labelled || output.entry >= newestHandOver) continue;
            Frame* owner = nullptr;
            for (auto& frame : frames_)
                if (frame.seq && frame.handOver <= output.entry && (!owner || frame.handOver > owner->handOver))
                    owner = &frame;
            if (owner) Attach(output, *owner);
            else { output.frame = UINT64_MAX; ++counters_.unassigned; } // before every hand-over we still hold
        }
    }

    const Sim* FindSim(uint64_t token) const noexcept
    {
        for (const auto& sim : sims_) if (sim.used && sim.token == token) return &sim;
        return nullptr;
    }

    // Frames whose first output has flipped become samples; a frame waits a quarter second for its last output's flip.
    void Complete() noexcept
    {
        const int64_t now = newest_;
        for (auto& frame : frames_)
        {
            if (!frame.seq || frame.done) continue;
            Output* first = FindOutput(frame.first);
            Output* last = FindOutput(frame.last);
            const int64_t age = now - frame.handOver;
            if (!first || !first->flip)
            {
                // A second without outputs, or without its first output's flip: the frame is not measurable.
                if (age > staleTicks_)
                {
                    frame.done = true;
                    if (first) ++counters_.noFlip;
                    else ++counters_.noOutputs;
                }
                continue;
            }
            const bool lastKnown = frame.closed && last && last->flip;
            if (!lastKnown && age <= waitTicks_) continue;
            frame.done = true;
            const Sim* start = frame.hasToken ? FindSim(frame.token) : nullptr;
            if (!start) { ++counters_.noStart; continue; }
            Sample sample{};
            sample.used = true;
            sample.flip = first->flip;
            sample.first = first->flip - start->qpc;
            sample.real = lastKnown ? last->flip - start->qpc : -1;
            sample.game = frame.handOver - start->qpc;
            sample.fg = first->entry - frame.handOver;
            sample.display = first->flip - first->entry;
            sample.exact = first->exact;
            uint64_t* reason = nullptr;
            if (sample.first <= 0 || sample.game < 0) reason = &counters_.beforeStart;
            else if (sample.fg < 0 || sample.display < 0) reason = &counters_.beforePresent;
            else if (sample.first > staleTicks_) reason = &counters_.tooLong;
            if (reason)
            {
                ++*reason;
                ++counters_.implausible;
                continue;
            }
            samples_[nextSample_++ % kSamples] = sample;
            ++counters_.frames;
        }
    }

public:
    // QPC ticks: how long a frame waits for its real output's flip, and when a frame without flips is given up. The
    // caller sets them from the QPC frequency (latency_probe.cpp); the defaults are for a 10 MHz counter.
    void SetFrequency(int64_t frequency) noexcept
    {
        if (frequency <= 0) return;
        waitTicks_ = frequency / 4;
        staleTicks_ = frequency;
    }

private:
    std::array<Output, kOutputs> outputs_{};
    std::array<Frame, kFrames> frames_{};
    std::array<Sim, kSims> sims_{};
    std::array<Sample, kSamples> samples_{};
    std::array<ThreadSlot, kThreads> threads_{};
    std::array<Mark, kMarks> marks_{};
    std::array<int64_t, kPeriods> periods_{};
    uint64_t nextOutput_ = 1, nextFrame_ = 1, nextSim_ = 0, nextSample_ = 0, nextThread_ = 0, nextPeriod_ = 0;
    uint64_t nextMark_ = 0, lastExactChain_ = 0, lastExactRefresh_ = 0, anchorChain_ = 0;
    uint32_t anchorCount_ = 0;
    int64_t anchorFlip_ = 0;
    bool hasAnchor_ = false;
    int64_t lastExactQpc_ = 0;
    int64_t activeSince_ = 0, newest_ = 0, lastHandOver_ = 0, lastOutput_ = 0, lastSimStart_ = 0, lastFlip_ = 0;
    int64_t waitTicks_ = 2'500'000, staleTicks_ = 10'000'000;
    Counters counters_{};
};
}
