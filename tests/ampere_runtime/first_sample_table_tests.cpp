// GitHub issue 158: the overlay read "Real -- | DLSS --" after 32 DLSS-G recreations, because the per-handle
// first-sample table never freed a slot. The production table (32 slots) reuses its least recently used slot.
#include "first_sample_table.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
template <size_t N> size_t Find(const temporal_interval_trace::FirstSampleTable<N>& table, uintptr_t handle)
{
    for (size_t i = 0; i < N; ++i) if (table.Handle(i) == handle) return i;
    return N;
}
template <size_t N> uint64_t Total(const temporal_interval_trace::FirstSampleTable<N>& table)
{
    uint64_t total = 0;
    for (size_t i = 0; i < N; ++i) total += table.Samples(i);
    return total;
}
// The FPS readers' rule (patcher.cpp): the largest per-slot increase over a window is the window's real frames.
template <size_t N> struct Window
{
    std::array<uint64_t, N> previous{};
    uint64_t Next(const temporal_interval_trace::FirstSampleTable<N>& table)
    {
        uint64_t frames = 0;
        for (size_t i = 0; i < N; ++i) {
            const uint64_t current = table.Samples(i);
            if (current >= previous[i]) frames = std::max(frames, current - previous[i]);
            previous[i] = current;
        }
        return frames;
    }
};
int main()
{
    using temporal_interval_trace::FirstSampleTable;
    {
        // The reporter's case: one feature at a time, recreated 50 times, 32 slots.
        FirstSampleTable<32> table;
        Window<32> window;
        bool everyHandleCounted = true, everyWindowCounted = true;
        for (uintptr_t handle = 1; handle <= 50; ++handle) {
            for (int frame = 0; frame < 60; ++frame) table.Record(handle * 0x1000);
            const size_t slot = Find(table, handle * 0x1000);
            everyHandleCounted &= slot < 32;
            everyWindowCounted &= window.Next(table) == 60;
        }
        Check(everyHandleCounted, "50 successive handles each get a slot in a 32-slot table");
        Check(everyWindowCounted, "every window after a recreation reads 60 real frames, also past the 32nd handle");
        Check(Total(table) == 50u * 60u, "no sample is dropped across 50 recreations");
        Check(Find(table, 1 * 0x1000) == 32 && Find(table, 18 * 0x1000) == 32 && Find(table, 19 * 0x1000) < 32,
            "the oldest 18 handles were the ones reused; the last 32 keep their slots");
    }
    {
        // A live feature that keeps evaluating keeps its slot while short-lived ones come and go.
        FirstSampleTable<4> table;
        const uintptr_t live = 0xABC000;
        table.Record(live);
        const size_t slot = Find(table, live);
        uint64_t before = table.Samples(slot);
        bool liveKept = true;
        for (uintptr_t handle = 1; handle <= 20; ++handle) {
            table.Record(handle);
            table.Record(live);
            liveKept &= Find(table, live) == slot;
        }
        Check(liveKept, "a handle in use is never the one reused");
        Check(table.Samples(slot) == before + 20, "the live handle's count kept rising");
    }
    {
        // A reused slot's count never goes down: readers skip a slot whose count fell.
        FirstSampleTable<2> table;
        for (int i = 0; i < 5; ++i) table.Record(1);
        for (int i = 0; i < 3; ++i) table.Record(2);
        const uint64_t slotOfOne = Find(table, 1), countBefore = table.Samples(slotOfOne);
        table.Record(3);
        Check(Find(table, 3) == slotOfOne && table.Samples(slotOfOne) == countBefore + 1,
            "the reused slot continues its count (5 -> 6), not restarting at 1");
    }
    {
        FirstSampleTable<2> table;
        table.Record(0);
        Check(Total(table) == 0 && table.Handle(0) == 0 && table.Handle(1) == 0, "a null handle is not recorded");
        Check(table.Handle(2) == 0 && table.Samples(2) == 0, "an out of range read returns zero");
    }
    {
        // Threads recording their own handles at once lose no samples.
        FirstSampleTable<32> table;
        std::vector<std::thread> threads;
        for (uintptr_t t = 1; t <= 8; ++t)
            threads.emplace_back([&table, t] { for (int i = 0; i < 20000; ++i) table.Record(t * 0x10); });
        for (auto& thread : threads) thread.join();
        bool each = true;
        for (uintptr_t t = 1; t <= 8; ++t) { const size_t slot = Find(table, t * 0x10); each &= slot < 32 && table.Samples(slot) == 20000; }
        Check(each && Total(table) == 8u * 20000u, "8 threads x 20000 samples on their own handles are all counted");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
