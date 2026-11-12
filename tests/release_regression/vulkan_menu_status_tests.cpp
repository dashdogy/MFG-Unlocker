// Vulkan menu status: the V-Sync Off present-mode choice at swapchain
// creation and its status, and the DLSS output rate from DLSS-G's own
// presented-frame count. Deterministic; no GPU, window or game.
#include "present_counter.h"
#include "vsync_control.h"
#include <cstdio>

namespace
{
unsigned gChecks = 0, gFailures = 0;
void Check(bool passed, const char* message)
{
    ++gChecks;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed) ++gFailures;
}

using vsync_control::Failure;
constexpr uint32_t kImmediate = vsync_control::kPresentModeImmediate;
constexpr uint32_t kMailbox = vsync_control::kPresentModeMailbox;
constexpr uint32_t kFifo = vsync_control::kPresentModeFifo;
constexpr uint32_t kFifoRelaxed = vsync_control::kPresentModeFifoRelaxed;

vsync_control::VulkanSurfaceModes Modes(bool immediate, bool mailbox)
{
    vsync_control::VulkanSurfaceModes modes{};
    modes.queried = true;
    modes.immediate = immediate;
    modes.mailbox = mailbox;
    return modes;
}

// One application vkCreateSwapchainKHR through the wrapper's two calls.
vsync_control::VulkanSwapchainSubmission Create(uint32_t mode, const vsync_control::VulkanSurfaceModes& modes,
    uint64_t handle, int32_t result = 0)
{
    const auto submission = vsync_control::BeforeVulkanSwapchain(mode, modes);
    vsync_control::AfterVulkanSwapchain(submission, result, result >= 0 ? handle : 0);
    return submission;
}

void VsyncCases()
{
    vsync_control::Configure(0);
    auto snapshot = vsync_control::ReadSnapshot();
    Check(!snapshot.vulkanSwapchain && !snapshot.vulkanHook
        && snapshot.failure == Failure::eNoApplicationSwapchain, "Game before any swapchain: nothing observed");
    vsync_control::VulkanSwapchainHookInstalled();
    Check(vsync_control::ReadSnapshot().vulkanHook, "installed creation wrapper is reported");

    auto submission = Create(kFifo, {}, 1);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kFifo && !submission.changed, "Game: FIFO swapchain created unchanged");
    Check(snapshot.vulkanSwapchain && !snapshot.matched && !snapshot.overrideApplied
        && snapshot.failure == Failure::eNone && snapshot.originalPresentMode == kFifo
        && snapshot.submittedPresentMode == kFifo, "Game: live FIFO swapchain reported without an override");

    vsync_control::Configure(1);
    Check(vsync_control::OffRequested(), "Off request visible to the creation wrapper");
    snapshot = vsync_control::ReadSnapshot();
    Check(!snapshot.matched && snapshot.available && snapshot.failure == Failure::eSwapchainRecreationPending
        && snapshot.submittedPresentMode == kFifo, "Off over a live FIFO swapchain waits for the next swapchain");

    submission = Create(kFifo, Modes(true, true), 2);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kImmediate && submission.changed, "Off: FIFO replaced with listed IMMEDIATE");
    Check(snapshot.matched && snapshot.overrideApplied && snapshot.failure == Failure::eNone
        && snapshot.originalPresentMode == kFifo && snapshot.submittedPresentMode == kImmediate
        && snapshot.originalInterval == 0 && snapshot.submittedInterval == 0,
        "Off: recreated swapchain reports FIFO -> IMMEDIATE");
    vsync_control::VulkanSwapchainDestroyed(1);
    Check(vsync_control::ReadSnapshot().overrideApplied, "destroying the retired swapchain keeps the live record");

    vsync_control::Configure(0);
    snapshot = vsync_control::ReadSnapshot();
    Check(!snapshot.matched && snapshot.failure == Failure::eSwapchainRecreationPending,
        "Game over an overridden swapchain waits for the next swapchain");
    submission = Create(kFifo, {}, 3);
    vsync_control::VulkanSwapchainDestroyed(2);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kFifo && snapshot.failure == Failure::eNone
        && snapshot.submittedPresentMode == kFifo, "Game: next swapchain keeps the game's FIFO");

    vsync_control::Configure(1);
    submission = Create(kFifoRelaxed, Modes(false, true), 4);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kMailbox && snapshot.overrideApplied
        && snapshot.originalPresentMode == kFifoRelaxed && snapshot.submittedPresentMode == kMailbox,
        "Off without IMMEDIATE: FIFO_RELAXED replaced with listed MAILBOX");

    submission = Create(kFifo, Modes(false, false), 5);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kFifo && !snapshot.matched
        && snapshot.failure == Failure::ePresentModeUnavailable,
        "Off with neither mode listed: FIFO kept, reported unavailable");

    auto constrained = Modes(true, true);
    constrained.unsupported = true;
    submission = Create(kFifo, constrained, 6);
    Check(submission.presentMode == kFifo
        && vsync_control::ReadSnapshot().failure == Failure::eSwapchainUnsupported,
        "Off with a present-mode list in the creation chain: FIFO kept");
    submission = Create(kFifo, {}, 7);
    Check(submission.presentMode == kFifo
        && vsync_control::ReadSnapshot().failure == Failure::eSwapchainUnsupported,
        "Off without a surface mode read: FIFO kept");
    submission = Create(1000361000u, Modes(true, true), 8);
    Check(submission.presentMode == 1000361000u
        && vsync_control::ReadSnapshot().failure == Failure::eSwapchainUnsupported,
        "Off leaves an unknown present mode untouched");

    submission = Create(kMailbox, Modes(true, true), 9);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kMailbox && !submission.changed && snapshot.matched
        && !snapshot.overrideApplied && snapshot.failure == Failure::eNone,
        "Off: game MAILBOX already presents without V-Sync");

    submission = Create(kFifo, Modes(true, false), 10, -1000001004);
    snapshot = vsync_control::ReadSnapshot();
    Check(submission.presentMode == kImmediate && !snapshot.matched
        && snapshot.failure == Failure::eSwapchainCreateFailed && snapshot.presentResult == -1000001004,
        "failed creation is reported, not shown as applied");

    Create(kFifo, Modes(true, false), 11);
    vsync_control::VulkanSwapchainDestroyed(11);
    snapshot = vsync_control::ReadSnapshot();
    Check(!snapshot.vulkanSwapchain && !snapshot.matched && snapshot.submittedPresentMode == 0
        && snapshot.failure == Failure::eNoApplicationSwapchain, "destroying the live swapchain clears the record");

    vsync_control::Configure(1);
    const auto present = vsync_control::BeforeApplicationPresent(1, 0, true);
    vsync_control::AfterApplicationPresent(present, S_OK);
    snapshot = vsync_control::ReadSnapshot();
    Check(present.interval == 0 && !snapshot.vulkanSwapchain && snapshot.overrideApplied
        && snapshot.originalInterval == 1, "DXGI Off path unchanged and reported as DXGI");
}

void TelemetryCases()
{
    using frame_telemetry::DlssgPresentedCounter;
    using frame_telemetry::DlssgPresentedRateMilli;
    constexpr uint64_t frequency = 10'000'000; // 100 ns ticks
    constexpr int64_t frameTicks = 166'667;    // 60 real fps
    DlssgPresentedCounter counter;
    int64_t tick = 1'000'000;
    uint64_t realFrame = 100;
    const auto run = [&](unsigned frames, uint32_t presented, unsigned queriesPerFrame) {
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            ++realFrame;
            tick += frameTicks;
            for (unsigned query = 0; query < queriesPerFrame; ++query)
                counter.Sample(realFrame, presented, tick + query);
        }
    };
    run(5, 4, 1);
    auto start = counter.Totals();
    const int64_t windowStart = tick;
    run(30, 4, 1);
    auto end = counter.Totals();
    uint32_t rate = DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - windowStart), tick);
    Check(rate >= 239'000 && rate <= 241'000, "4x at 60 real fps, one query per frame: 240 DLSS fps");

    start = end;
    const int64_t secondStart = tick;
    run(30, 4, 3);
    end = counter.Totals();
    rate = DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - secondStart), tick);
    Check(rate >= 239'000 && rate <= 241'000, "three queries per real frame read one batch, not three");

    start = end;
    const int64_t gapStart = tick;
    run(10, 6, 1);
    ++realFrame; tick += frameTicks; // a real frame without a query
    run(19, 6, 1);
    end = counter.Totals();
    Check(DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - gapStart), tick) == 0,
        "a real frame without a sample makes the window unavailable");

    start = end;
    const int64_t sparseStart = tick;
    for (unsigned frame = 0; frame < 30; ++frame)
    {
        ++realFrame;
        tick += frameTicks;
        if (frame % 2 == 0) counter.Sample(realFrame, 6, tick);
    }
    end = counter.Totals();
    Check(DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - sparseStart), tick) == 0,
        "queries every other real frame are no measurement");

    run(2, 6, 1); // resume one query per frame before the window
    start = counter.Totals();
    const int64_t staleStart = tick;
    run(30, 6, 1);
    end = counter.Totals();
    Check(DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - staleStart),
        tick + frequency) == 0, "a stale latest sample is no measurement");
    Check(DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - staleStart), tick) > 0,
        "the same window is a measurement while current");

    start = end;
    const int64_t resetStart = tick;
    realFrame = 5; // the real-frame key went backwards (handles reset)
    run(30, 6, 1);
    end = counter.Totals();
    Check(DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - resetStart), tick) == 0,
        "a real-frame discontinuity makes the window unavailable");

    run(2, 1, 1); // a window's first commit is the frame before it
    start = counter.Totals();
    const int64_t realStart = tick;
    run(30, 1, 1);
    end = counter.Totals();
    rate = DlssgPresentedRateMilli(start, end, 30, frequency, static_cast<uint64_t>(tick - realStart), tick);
    Check(rate >= 59'000 && rate <= 61'000, "batches of one (no generated frames) measure the real rate");
    Check(DlssgPresentedRateMilli(start, end, 0, frequency, static_cast<uint64_t>(tick - realStart), tick) == 0,
        "no real frames in the window is no measurement");
}
}

int main()
{
    VsyncCases();
    TelemetryCases();
    std::printf("vulkan_menu_status checks=%u failures=%u\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
