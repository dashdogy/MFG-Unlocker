// CPU checks of XeMFG standalone's policy (source/native/xess/standalone_policy.h).
#include "xess/standalone_policy.h"
#include "xess/ngx_provider_policy.h"
#include "xess/xemfg_dynamic.h"
#include "xess/module_list.h"
#include "host_process_policy.h"
#include "xess/host_port_policy.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
using namespace xemfg_standalone;
int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const char* what, int line)
{
    ++gChecks;
    if (!condition) { ++gFailures; std::printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void Ids()
{
    FrameIds ids;
    // Upscaler frames: the one after the last present.
    CHECK(ids.Next() == 1);
    ids.Presented(1);
    CHECK(ids.Next() == 2 && ids.Last() == 1);
    // Streamline tokens map after the last present and keep their spacing.
    const uint32_t first = ids.FromToken(5000);
    CHECK(first == 2 && ids.Tokens());
    CHECK(ids.FromToken(5001) == 3);
    CHECK(ids.FromToken(5003) == 5);
    ids.Presented(3);
    // A frame already presented gets no id; the next ones keep mapping.
    CHECK(ids.FromToken(5000) == 0);
    CHECK(ids.FromToken(5002) == 4);
    // Ids never go back.
    ids.Presented(2);
    CHECK(ids.Last() == 3);
    // A restarted token counter (far behind) maps right after the last present again.
    CHECK(ids.FromToken(7) == 4);
    CHECK(ids.FromToken(8) == 5);
    // Back to the upscaler path, then tokens again: a fresh offset above the last present.
    ids.Presented(10);
    ids.LeaveTokens();
    CHECK(!ids.Tokens() && ids.Next() == 11);
    CHECK(ids.FromToken(9) == 11);
    // A huge jump re-bases instead of skipping millions of ids.
    CHECK(ids.FromToken(9 + 0x200000) == 11);

    // The present's id: the marker's frame, else the oldest pending frame, else the next.
    const uint32_t pending[] = {14, 12, 13};
    CHECK(PresentId(11, 15, pending, 3) == 15);
    CHECK(PresentId(11, 0, pending, 3) == 12);
    CHECK(PresentId(11, 11, pending, 3) == 12);   // a marker for a presented frame is stale
    CHECK(PresentId(14, 0, pending, 3) == 15);

    // The ring finds, reuses the oldest slot and lists pending complete frames.
    FrameRing<4> ring;
    for (uint32_t id = 1; id <= 6; ++id) { auto& r = ring.At(id); r.depth = r.motion = id != 5; }
    CHECK(!ring.Find(1) && !ring.Find(2) && ring.Find(3) && ring.Find(6));
    uint32_t out[4]{};
    const size_t count = ring.Pending(3, out, 4);
    CHECK(count == 2);   // 4 and 6 (5 has no inputs)
}

// idanchor.1: Subnautica 2 at XeMFG's start: the render thread's frame 895 mapped first, two ahead of the present of 893,
// whose tags were refused. Without the anchor every tag comes after its id was presented, for the session. A present's
// id less the offset is its frame's token (the presenter's TokenOfPresentId).
void PresentAnchor()
{
    // A refused tag or constant is noted as the host does (NoteStreamlineRefusedLocked).
    const auto tag = [](FrameIds& ids, uint32_t token) {
        const uint32_t id = ids.FromToken(token);
        if (!id) ids.Refused(token);
        return id;
    };
    const auto run = [&](FrameIds& ids, uint32_t first, uint32_t& refused, uint32_t& wrong) {
        refused = wrong = 0;
        for (uint32_t t = first; t < first + 100; ++t)
        {
            const uint32_t marker = ids.FromPresentToken(t);
            const uint32_t id = PresentId(ids.Last(), marker, nullptr, 0);
            ids.Presented(id);
            if (int64_t(id) - ids.Offset() != int64_t(t)) ++wrong;
            if (!tag(ids, t + 2)) ++refused;
        }
    };
    uint32_t refused = 0, wrong = 0;
    FrameIds ahead;
    ahead.Presented(1570);
    CHECK(ahead.FromToken(895) == 1571);
    CHECK(tag(ahead, 893) == 0 && tag(ahead, 894) == 0);   // 1569, 1570: presented
    run(ahead, 893, refused, wrong);
    CHECK(refused == 0 && wrong == 0 && ahead.Anchors() == 1);

    // An offset left behind while presents outran the tokens (a hitch's fills), by less than a restart's gap.
    FrameIds stale;
    stale.Presented(621);
    CHECK(stale.FromToken(1) == 622);
    stale.Presented(1570);
    CHECK(tag(stale, 893) == 0);   // 1514: presented
    run(stale, 893, refused, wrong);
    CHECK(refused == 0 && wrong == 0 && stale.Anchors() == 1);

    // A map that was right stays: no anchor.
    FrameIds right;
    right.Presented(10);
    CHECK(right.FromToken(500) == 11);
    run(right, 500, refused, wrong);
    CHECK(refused == 0 && wrong == 0 && right.Anchors() == 0);
    // A second marker of the presented frame, or an older one, anchors nothing.
    const int64_t offset = right.Offset();
    CHECK(right.FromPresentToken(599) == 0 && right.FromPresentToken(590) == 0);
    CHECK(right.Offset() == offset && right.Anchors() == 0);

    // A game that sends the present-start marker after its Present: the frame's inputs were taken (no refusal), the
    // present found them as the oldest pending frame, and the late marker anchors nothing.
    FrameIds late;
    late.Presented(10);
    uint32_t marker = 0;
    wrong = 0;
    for (uint32_t t = 500; t < 600; ++t)
    {
        const uint32_t inputs = tag(late, t);
        const uint32_t pending[] = {inputs};
        const uint32_t id = PresentId(late.Last(), marker, pending, inputs ? 1 : 0);
        late.Presented(id);
        if (!inputs || id != inputs) ++wrong;
        marker = late.FromPresentToken(t);
    }
    CHECK(wrong == 0 && late.Anchors() == 0);
}

void Sources()
{
    // tagframe.1: a frameless (legacy) Streamline tag belongs to the frame the render-submit marker named, while those
    // markers are recent, not to the next frame the game already named (Cyberpunk 2077); else the latest token.
    CHECK(LegacyTagToken(11, 10, true) == 10);
    CHECK(LegacyTagToken(11, 10, false) == 11);
    CHECK(LegacyTagToken(11, 0, true) == 11);
    CHECK(LegacyTagToken(0, 0, false) == 0);
    // Streamline always; the upscalers only after Streamline's hold.
    CHECK(SourceAdmitted(Source::eStreamline, 5000, 4990));
    CHECK(!SourceAdmitted(Source::eDlss, 5000, 4500));
    CHECK(SourceAdmitted(Source::eDlss, 5600, 4500));
    CHECK(SourceAdmitted(Source::eFsr, 5000, 0));
    CHECK(!SourceAdmitted(Source::eNone, 5000, 0));
    // The first source keeps the frame.
    FrameRecord record;
    CHECK(Claim(record, Source::eFsr));
    CHECK(Claim(record, Source::eFsr));
    CHECK(!Claim(record, Source::eXess));
    // Streamline's tags take a frame an upscaler reached first; an upscaler never takes it back.
    CHECK(Claim(record, Source::eStreamline) && record.source == Source::eStreamline);
    CHECK(!Claim(record, Source::eDlss) && record.source == Source::eStreamline);
    CHECK(std::strcmp(SourceName(Source::eStreamline), "streamline") == 0);
    // Streamline lifecycles: valid only now (or until evaluate) needs the command list XeFG copies on.
    CHECK(StreamlineValidity(0, true) == Validity::eOnlyNow);
    CHECK(StreamlineValidity(0, false) == Validity::eSkip);
    CHECK(StreamlineValidity(1, false) == Validity::eUntilPresent);
    CHECK(StreamlineValidity(2, true) == Validity::eOnlyNow);
    CHECK(StreamlineValidity(2, false) == Validity::eSkip);
    CHECK(StreamlineValidity(9, true) == Validity::eSkip);
    // Tag extents: empty = whole resource, clamped inside it.
    auto area = TagArea(0, 0, 0, 0, 1920, 1080);
    CHECK(area.x == 0 && area.width == 1920 && area.height == 1080);
    area = TagArea(100, 50, 3000, 2000, 1920, 1080);
    CHECK(area.x == 100 && area.y == 50 && area.width == 1820 && area.height == 1030);
}

void Conventions()
{
    // Streamline's normalizing scale becomes pixels of the motion vectors' extent.
    auto scale = StreamlineMotionScale(1.0f / 1280.0f, 1.0f / 720.0f, 1280, 720);
    CHECK(Near(scale.first, 1.0f) && Near(scale.second, 1.0f));
    scale = StreamlineMotionScale(-0.5f, 0.25f, 2000, 1000);
    CHECK(Near(scale.first, -1000.0f) && Near(scale.second, 250.0f));
    // NGX DLSS-G's MvecScale is already pixels: the harness's pixel vectors arrive as 1.0, Enshrouded's
    // normalized ones as the 2560x1440 extent. Same size copy keeps it; a resampled copy scales with it.
    scale = NgxDlssgMotionScale(1.0f, 1.0f, 1280, 720, 1280, 720);
    CHECK(Near(scale.first, 1.0f) && Near(scale.second, 1.0f));
    scale = NgxDlssgMotionScale(2560.0f, 1440.0f, 2560, 1440, 2560, 1440);
    CHECK(Near(scale.first, 2560.0f) && Near(scale.second, 1440.0f));
    scale = NgxDlssgMotionScale(-3840.0f, 2160.0f, 3840, 2160, 2560, 1440);
    CHECK(Near(scale.first, -2560.0f) && Near(scale.second, 1440.0f));
    scale = NgxDlssgMotionScale(2.0f, 3.0f, 0, 0, 1920, 1080);
    CHECK(Near(scale.first, 2.0f) && Near(scale.second, 3.0f));
    // XeSS: pixels pass; NDC velocity (y up) becomes half-extent pixels with y flipped.
    scale = XessMotionScale(2.0f, 3.0f, false, 1920, 1080);
    CHECK(Near(scale.first, 2.0f) && Near(scale.second, 3.0f));
    scale = XessMotionScale(1.0f, 1.0f, true, 1920, 1080);
    CHECK(Near(scale.first, 960.0f) && Near(scale.second, -540.0f));
    // FidelityFX API states as its D3D12 backend maps them; unknown combinations refused.
    uint32_t state = 1234;
    CHECK(FfxStateToD3d12(1u << 2, state) && state == 0x40);
    CHECK(FfxStateToD3d12((1u << 2) | (1u << 3), state) && state == 0xC0);
    CHECK(FfxStateToD3d12((1u << 2) | (1u << 4), state) && state == 0xAC3);
    CHECK(FfxStateToD3d12(1u << 1, state) && state == 0x8);
    CHECK(!FfxStateToD3d12((1u << 1) | (1u << 3), state));
    // The projection: left-handed perspective, near/far swapped for inverted depth, infinite far.
    float m[16]{};
    Projection(16.0f / 9.0f, 0.1f, 1000.0f, 1.0471976f, false, m);
    CHECK(Near(m[5], 1.0f / std::tan(0.5235988f)) && Near(m[0], m[5] * 9.0f / 16.0f));
    CHECK(Near(m[11], 1.0f) && Near(m[10], 1000.0f / 999.9f) && Near(m[14], -0.1f * 1000.0f / 999.9f));
    Projection(1.0f, 0.1f, 1000.0f, 1.0471976f, true, m);
    CHECK(Near(m[10], 0.1f / (0.1f - 1000.0f)) && Near(m[14], -1000.0f * m[10]));
    Projection(1.0f, 0.1f, INFINITY, 1.0f, false, m);
    CHECK(Near(m[10], 1.0f) && Near(m[14], -0.1f));
    Projection(1.0f, 0.1f, INFINITY, 1.0f, true, m);
    CHECK(Near(m[10], 0.0f) && Near(m[14], 0.1f));
    // No camera: 60 degrees, 0.1 / 10000.
    Projection(1.0f, 0, 0, 0, false, m);
    CHECK(Near(m[5], 1.0f / std::tan(0.5235988f)) && Near(m[10], 10000.0f / 9999.9f));
}

void Generation()
{
    GenerationInputs in;
    CHECK(std::strcmp(WhyNotGenerating(in), "off") == 0);
    in.enabled = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "no_inputs") == 0);
    in.depth = in.motion = true;
    CHECK(WhyNotGenerating(in) == nullptr);
    // The game's DLSS-G switch gates XeMFG once the game drives DLSS-G.
    in.dlssgSeen = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "game_frame_generation_off") == 0);
    in.gameFrameGeneration = true;
    CHECK(WhyNotGenerating(in) == nullptr);
    in.sizesMatch = false;
    CHECK(std::strcmp(WhyNotGenerating(in), "motion_vector_size") == 0);
    in.sizesMatch = true;
    in.fsrFrameGeneration = true;
    CHECK(std::strcmp(WhyNotGenerating(in), "game_fsr_frame_generation") == 0);
}

void Rates()
{
    // The FPS window samples after a second; a counter that goes back restarts it.
    FpsWindow fps;
    fps.Add(1000, 100, 600);
    CHECK(fps.sampleMs == 0 && fps.baseMilli == 0);
    fps.Add(1500, 115, 690);
    CHECK(fps.sampleMs == 0);
    fps.Add(2000, 130, 780);
    CHECK(fps.sampleMs == 2000 && fps.baseMilli == 30000 && fps.outputMilli == 180000);
    fps.Add(3250, 160, 960);
    CHECK(fps.sampleMs == 3250 && fps.baseMilli == 24000 && fps.outputMilli == 144000);
    fps.Add(3300, 5, 30);   // a new chain: restart, keep the last sample
    CHECK(fps.sampleMs == 3250 && fps.startBase == 5 && fps.startOutput == 30);
    fps.Add(4300, 35, 210);
    CHECK(fps.sampleMs == 4300 && fps.baseMilli == 30000 && fps.outputMilli == 180000);
    // The FPS limit counts presented frames, as XeLL does: the presented frames' interval; off; 1 FPS.
    CHECK(LimitIntervalUs(0) == 0);
    CHECK(LimitIntervalUs(120) == 8333);
    CHECK(LimitIntervalUs(117) == 8547);
    CHECK(LimitIntervalUs(60) == 16667);
    CHECK(LimitIntervalUs(1) == 1000000);
    // pacerfix.2: the automatic output cap under V-Sync (refresh - refresh^2/3600, whole FPS rounded down after 0.01 of
    // slack); none without a refresh period or outside 23-1000 Hz.
    CHECK(AutoCapFps(0) == 0);
    CHECK(AutoCapFps(-1) == 0);
    CHECK(AutoCapFps(16666667) == 59);   // 60 Hz (58.9999988)
    CHECK(AutoCapFps(16683350) == 58);   // 59.94 Hz
    CHECK(AutoCapFps(8333333) == 116);   // 120 Hz
    CHECK(AutoCapFps(6944444) == 138);   // 144 Hz
    CHECK(AutoCapFps(4166667) == 224);   // 240 Hz (223.99998)
    CHECK(AutoCapFps(50000000) == 0);    // 20 Hz
    CHECK(AutoCapFps(500000) == 0);      // 2000 Hz
}

// The host's Dynamic MFG driver (xemfg_dynamic.h); the controllers have their own simulations.
uint32_t RunDynamic(DynamicFrames& dynamic, const DynamicSettings& s, double gameMs, double seconds, uint32_t frames,
    int focus = 1, double* nowMs = nullptr)
{
    double now = nowMs ? *nowMs : 0.0;
    for (double end = now + seconds * 1000.0; now < end; now += gameMs) {
        DynamicFrames::Frame f;
        f.nowMs = now;
        f.frameMs = gameMs;
        f.refreshFps = 120.0;
        f.focus = focus;
        f.frames = frames;
        frames = dynamic.Update(f, s);
    }
    if (nowMs) *nowMs = now;
    return frames;
}

void Dynamic()
{
    // The target: the one set, at most the FPS limit; else the limit; else just under the refresh rate.
    DynamicSettings s;
    s.enabled = true;
    CHECK(DynamicTargetFps(s, 120.0) == 116.0);
    CHECK(DynamicTargetFps(s, 144.0) == 138.0);
    CHECK(DynamicTargetFps(s, 0.0) == 0.0);
    s.limitFps = 117;
    CHECK(DynamicTargetFps(s, 120.0) == 117.0);
    s.targetFps = 100;
    CHECK(DynamicTargetFps(s, 120.0) == 100.0);
    s.targetFps = 200;
    CHECK(DynamicTargetFps(s, 120.0) == 117.0);
    s.limitFps = 0;
    CHECK(DynamicTargetFps(s, 120.0) == 200.0);

    // A light game (100 real fps, frames free) and a 116 target: from 6X the count comes down; nearest is 1X (100 against
    // 200 at 2X), or 2X without 1X.
    s = DynamicSettings{};
    s.enabled = true;
    DynamicFrames dynamic;
    uint32_t frames = RunDynamic(dynamic, s, 10.0, 90.0, 6);
    CHECK(frames == 1 && dynamic.Active() && dynamic.TargetFps() == 116.0 && !dynamic.CapActive());
    s.allow1x = false;
    DynamicFrames no1x;
    CHECK(RunDynamic(no1x, s, 10.0, 90.0, 6) == 2);
    // A heavy game (25 real fps): up to the most frames set (4X here), never past it.
    s.allow1x = true;
    s.maxFrames = 4;
    DynamicFrames heavy;
    CHECK(RunDynamic(heavy, s, 40.0, 30.0, 2) == 4);

    // In the background the count stays as it is; back in front it decides again.
    s.maxFrames = 6;
    DynamicFrames focus;
    double now = 0.0;
    CHECK(RunDynamic(focus, s, 10.0, 30.0, 6, 0, &now) == 6);
    CHECK(RunDynamic(focus, s, 10.0, 90.0, 6, 1, &now) == 1);

    // Use target as cap: the host's limit holds the target; the fewest frames that reach it, never 1X.
    s.targetAsCap = true;
    DynamicFrames cap;
    frames = RunDynamic(cap, s, 10.0, 60.0, 6);
    CHECK(cap.CapActive() && cap.CapFps() == 116.0 && frames >= 2);
    // Switched off: everything goes, the most frames come back.
    s.enabled = false;
    CHECK(cap.Update({}, s) == 6 && !cap.Active() && cap.CapFps() == 0.0);
}

// sapacing.1: the flip queue per host, the control key and the OptiScaler.ini override (ChooseFlipQueue).
bool Is(const FlipQueueChoice& choice, bool enabled, const wchar_t* setting)
{
    if (choice.enabled != enabled) return false;
    if (!setting || !choice.setting) return setting == choice.setting;
    return std::wcscmp(choice.setting, setting) == 0;
}

void Queue()
{
    const auto choose = [](bool iniSet, bool iniOn, QueueHost host, uint32_t key) {
        return ChooseFlipQueue(FlipQueueInputs{iniSet, iniOn, host, key});
    };
    // D3D12 standalone host: on with auto, the key turns it on or off.
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 0), true, L"auto (XeMFG standalone)"));
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 1), true, L"on (xemfgFlipQueue)"));
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 2), false, L"off (xemfgFlipQueue)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 0), true, L"auto (Vulkan host)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 1), true, L"on (xemfgFlipQueue)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 2), false, L"off (xemfgFlipQueue)"));
    // A key outside 0..2 is auto (SetStandaloneFlipQueue stores 0 for it anyway).
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 7), true, L"auto (XeMFG standalone)"));
    // OptiScaler.ini's XeFGFlipQueue wins on every host, the key included; its own value is logged.
    CHECK(Is(choose(true, false, QueueHost::eD3d12, 1), false, nullptr));
    CHECK(Is(choose(true, true, QueueHost::eD3d12, 2), true, nullptr));
    CHECK(Is(choose(true, true, QueueHost::eVulkan, 0), true, nullptr));
    CHECK(Is(choose(true, false, QueueHost::eOther, 0), false, nullptr));
    // Other devices (OptiScaler's contexts, a game's own XeFG): the key does not reach them; auto is on.
    CHECK(Is(choose(false, true, QueueHost::eOther, 2), true, nullptr));
    CHECK(Is(choose(false, true, QueueHost::eOther, 0), true, nullptr));
    // iniOn false without iniSet cannot come from ReadXefgFlipQueue any more; it still means off.
    CHECK(Is(choose(false, false, QueueHost::eOther, 1), false, nullptr));
    CHECK(Is(choose(false, false, QueueHost::eD3d12, 1), true, L"on (xemfgFlipQueue)"));
}

// winequeue.1: under Wine/Proton auto is off on every host; the ini and the key still decide.
void WineQueue()
{
    const auto choose = [](bool iniSet, bool iniOn, QueueHost host, uint32_t key) {
        FlipQueueInputs inputs{iniSet, iniOn, host, key};
        inputs.wine = true;
        return ChooseFlipQueue(inputs);
    };
    // Auto: off on OptiScaler's contexts, the D3D12 standalone host and the Vulkan host.
    CHECK(Is(choose(false, true, QueueHost::eOther, 0), false, L"auto (Wine/Proton)"));
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 0), false, L"auto (Wine/Proton)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 0), false, L"auto (Wine/Proton)"));
    // A key outside 0..2 is auto there too, and the key does not reach other devices.
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 7), false, L"auto (Wine/Proton)"));
    CHECK(Is(choose(false, true, QueueHost::eOther, 1), false, L"auto (Wine/Proton)"));
    CHECK(Is(choose(true, true, QueueHost::eOther, 0), true, nullptr));
    CHECK(Is(choose(true, true, QueueHost::eVulkan, 2), true, nullptr));
    CHECK(Is(choose(true, false, QueueHost::eD3d12, 1), false, nullptr));
    CHECK(Is(choose(false, true, QueueHost::eD3d12, 1), true, L"on (xemfgFlipQueue)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 1), true, L"on (xemfgFlipQueue)"));
    CHECK(Is(choose(false, true, QueueHost::eVulkan, 2), false, L"off (xemfgFlipQueue)"));
    // Off Wine nothing changes.
    CHECK(Is(ChooseFlipQueue(FlipQueueInputs{false, true, QueueHost::eOther, 0}), true, nullptr));
    CHECK(Is(ChooseFlipQueue(FlipQueueInputs{false, true, QueueHost::eD3d12, 0, false}), true, L"auto (XeMFG standalone)"));
}

bool Is(const FlipMeteringChoice& choice, bool enabled, const char* reason)
{
    if (choice.enabled != enabled) return false;
    if (!reason || !choice.reason) return reason == choice.reason;
    return std::strcmp(choice.reason, reason) == 0;
}

// metergate.1: NVIDIA flip metering only where the driver reports SetFlipConfig V2 (or the ini forces it).
void Metering()
{
    using R = FlipMeteringReport;
    // The driver reports support: metered (sl.dlss_g meters after the same query).
    CHECK(Is(ChooseFlipMetering(true, true, false, R::eSupported), true, nullptr));
    CHECK(Is(ChooseFlipMetering(true, true, true, R::eSupported), true, nullptr));
    // GTX 1660 on 621.01: SetFlipConfig returns NVAPI_OK, but without the report every output stays gated.
    CHECK(Is(ChooseFlipMetering(true, true, false, R::eUnsupported), false, "driver_unsupported"));
    // Drivers before R595 have no query: not proven, not metered.
    CHECK(Is(ChooseFlipMetering(true, true, false, R::eQueryMissing), false, "support_query_missing"));
    CHECK(Is(ChooseFlipMetering(true, true, false, R::eQueryFailed), false, "support_query_failed"));
    // XeFGFlipMetering = on uses it whatever the report says; off wins over everything.
    CHECK(Is(ChooseFlipMetering(true, true, true, R::eUnsupported), true, "forced"));
    CHECK(Is(ChooseFlipMetering(true, true, true, R::eQueryMissing), true, "forced"));
    CHECK(Is(ChooseFlipMetering(true, false, false, R::eSupported), false, "setting_off"));
    CHECK(Is(ChooseFlipMetering(true, false, true, R::eSupported), false, "setting_off"));
    // No driver SetFlipConfig (AMD, fakenvapi, older drivers): off, the caller logs why.
    CHECK(Is(ChooseFlipMetering(false, true, true, R::eSupported), false, nullptr));
    CHECK(Is(ChooseFlipMetering(false, true, false, R::eQueryMissing), false, nullptr));
    CHECK(std::strcmp(FlipMeteringReportName(R::eSupported), "supported") == 0);
    CHECK(std::strcmp(FlipMeteringReportName(R::eUnsupported), "unsupported") == 0);
    CHECK(std::strcmp(FlipMeteringReportName(R::eQueryMissing), "query_missing") == 0);
    CHECK(std::strcmp(FlipMeteringReportName(R::eQueryFailed), "query_failed") == 0);
}

// hosthandle.1: a call on a host-created XeFG context is the host's, whatever module it returns into.
void Routing()
{
    CHECK(HostRoutedCall(true, false));   // OptiScaler's own call
    CHECK(HostRoutedCall(false, true));   // R04: REFramework's detour on OptiScaler's context
    CHECK(HostRoutedCall(true, true));
    CHECK(!HostRoutedCall(false, false)); // the game's own XeFG context
}

// optisleep.1: the libxell probes act on OptiScaler's calls only, and only the Present markers bracket the Present.
void Probes()
{
    CHECK(HostProbeActs(true, false));    // OptiScaler's XeLL call
    CHECK(!HostProbeActs(true, true));    // our standalone host's call, or one through our Hook_xellSleep
    CHECK(!HostProbeActs(false, false));  // no OptiScaler: Hook_xellSleep is the sleep point
    CHECK(!HostProbeActs(false, true));
    CHECK(PresentMarkerEdge(4) == 1);     // XELL_PRESENT_START
    CHECK(PresentMarkerEdge(5) == -1);    // XELL_PRESENT_END
    for (uint32_t marker : {0u, 1u, 2u, 3u, 6u, 7u, 0xFFFFFFFFu}) CHECK(PresentMarkerEdge(marker) == 0);
}

// sdkscan.1: the XeSS discoveries' module list (source/native/xess/module_list.h): psapi's list read at the size it
// reports, Toolhelp's added on request, each module once.
void Modules()
{
    std::vector<HMODULE> modules;
    xess_mod::ModuleListStats stats;
    CHECK(xess_mod::LoadedModules(modules, stats, xess_mod::ToolhelpList::eAlways));
    CHECK(stats.psapi > 0 && stats.psapiError == 0);
    CHECK(stats.toolhelpRan && stats.toolhelp > 0 && stats.toolhelpError == 0);
    const auto has = [&](HMODULE module) { return std::find(modules.begin(), modules.end(), module) != modules.end(); };
    CHECK(has(GetModuleHandleW(nullptr)));
    CHECK(has(GetModuleHandleW(L"kernel32.dll")));
    CHECK(has(GetModuleHandleW(L"ntdll.dll")));
    CHECK(modules.size() >= stats.psapi && modules.size() >= stats.toolhelp);
    std::vector<HMODULE> sorted = modules;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    // Fallback (a Create call's discovery pass, the upscaler discovery): psapi works here, so its list alone.
    std::vector<HMODULE> psapiOnly;
    xess_mod::ModuleListStats psapiStats;
    CHECK(xess_mod::LoadedModules(psapiOnly, psapiStats, xess_mod::ToolhelpList::eFallback));
    CHECK(!psapiStats.toolhelpRan && psapiStats.toolhelp == 0 && psapiOnly.size() == psapiStats.psapi);
    HMODULE extra = LoadLibraryW(L"version.dll");
    CHECK(extra != nullptr);
    CHECK(xess_mod::LoadedModules(modules, stats, xess_mod::ToolhelpList::eAlways));
    CHECK(has(extra));
    if (extra) FreeLibrary(extra);
}

// ngxprov.1: the Nvngx replacement provider's API-free rules (source/native/xess/ngx_provider_policy.h).
void Provider()
{
    using namespace ngx_provider;
    // Batches: index 1 starts one, the following indices continue it; a repeated or earlier index, a new count, or
    // no batch yet starts one. A skipped index still continues the batch.
    CHECK(NewBatch(1, 0, 3, 0, 0));
    CHECK(NewBatch(2, 0, 3, 0, 0));
    CHECK(!NewBatch(2, 1, 3, 3, 1));
    CHECK(!NewBatch(3, 2, 3, 3, 1));
    CHECK(NewBatch(1, 3, 3, 3, 1));
    CHECK(NewBatch(2, 2, 3, 3, 1));
    CHECK(NewBatch(2, 1, 5, 3, 1));
    CHECK(!NewBatch(3, 1, 3, 3, 1));
    // Depth and motion: equal sizes pass, alignment padding of either texture gives the area both cover (Wuthering
    // Waves: 604x452 depth, 603x452 motion), and a real scale difference is refused.
    {
        const auto same = DepthMotionExtent(1920, 1080, 1920, 1080);
        CHECK(same.width == 1920 && same.height == 1080);
        const auto wuwa = DepthMotionExtent(604, 452, 603, 452);
        CHECK(wuwa.width == 603 && wuwa.height == 452);
        const auto motionPadded = DepthMotionExtent(1280, 720, 1296, 736);
        CHECK(motionPadded.width == 1280 && motionPadded.height == 720);
        CHECK(DepthMotionExtent(3840, 2160, 1920, 1080).width == 0);
        CHECK(DepthMotionExtent(1920, 1080, 1920, 1112).width == 0);
        CHECK(DepthMotionExtent(60, 60, 30, 30).width == 0);
        CHECK(DepthMotionExtent(0, 452, 603, 452).width == 0);
    }
    // ngxrect.1: the game's rectangles first, the whole textures by the padding rule otherwise.
    {
        const InputRect none{};
        const InputRect view{0, 0, 2560, 1440};
        // The First Berserker: Khazan: the picture in the corner of a larger depth texture.
        const auto khazan = ChooseDepthMotionRects(view, view, 2560, 2048, 2560, 1440);
        CHECK(khazan.fromSubrects && khazan.depth == view && khazan.motion == view);
        // The same textures without rectangles, or with whole-texture ones that disagree, say nothing: refused.
        CHECK(ChooseDepthMotionRects(none, none, 2560, 2048, 2560, 1440).depth.width == 0);
        CHECK(ChooseDepthMotionRects({0, 0, 2560, 2048}, view, 2560, 2048, 2560, 1440).depth.width == 0);
        // Absent rectangles keep the padding rule.
        const auto padded = ChooseDepthMotionRects(none, none, 604, 452, 603, 452);
        CHECK(!padded.fromSubrects && padded.depth == (InputRect{0, 0, 603, 452}) && padded.motion == padded.depth);
        // Rectangles a texel apart take the area both cover.
        const auto titans = ChooseDepthMotionRects({0, 0, 1288, 724}, {0, 0, 1287, 724}, 1288, 724, 1287, 724);
        CHECK(titans.fromSubrects && titans.depth == (InputRect{0, 0, 1287, 724}) && titans.motion == titans.depth);
        // Each rectangle keeps its own offset.
        const auto offset = ChooseDepthMotionRects({8, 16, 1280, 720}, {0, 0, 1280, 720}, 1920, 1080, 1920, 1080);
        CHECK(offset.fromSubrects && offset.depth == (InputRect{8, 16, 1280, 720}) && offset.motion == (InputRect{0, 0, 1280, 720}));
        // A rectangle outside its texture is ignored, without wrapping; equal textures still pass whole.
        const auto outside = ChooseDepthMotionRects({1000, 0, 1280, 720}, {0, 0, 1280, 720}, 1920, 1080, 1920, 1080);
        CHECK(!outside.fromSubrects && outside.depth == (InputRect{0, 0, 1920, 1080}));
        const auto wrapped = ChooseDepthMotionRects({0xFFFFFFFFu, 0, 1280, 720}, {0, 0, 1280, 720}, 1920, 1080, 1920, 1080);
        CHECK(!wrapped.fromSubrects && wrapped.depth == (InputRect{0, 0, 1920, 1080}));
        // Rectangles of a real scale difference are refused too.
        CHECK(ChooseDepthMotionRects({0, 0, 3840, 2160}, {0, 0, 1920, 1080}, 3840, 2160, 1920, 1080).depth.width == 0);
    }
    // OptiScaler's rule in custom\<name>\dlssg: nvngx_dlssg.dll, else the first DLL exporting NGX's init, else the first
    // DLL. libxell.dll and libxess_fg.dll beside the shim export none, so the shim still runs.
    const auto shimExports = [](std::wstring_view name) { return SameName(name, L"mfgunlocker_xemfg.dll"); };
    const auto noneExport = [](std::wstring_view) { return false; };
    const auto allExport = [](std::wstring_view) { return true; };
    const std::vector<std::wstring> shimOnly{L"mfgunlocker_xemfg.dll"};
    CHECK(OptiscalerDlssgPick(shimOnly, shimExports) == L"mfgunlocker_xemfg.dll");
    const std::vector<std::wstring> xell{L"libxell.dll", L"mfgunlocker_xemfg.dll"};
    CHECK(OptiscalerDlssgPick(xell, shimExports) == L"mfgunlocker_xemfg.dll");
    const std::vector<std::wstring> xefg{L"libxess_fg.dll", L"libxell.dll", L"mfgunlocker_xemfg.dll"};
    CHECK(OptiscalerDlssgPick(xefg, shimExports) == L"mfgunlocker_xemfg.dll");
    // Without a DLL exporting NGX's init the first DLL is what OptiScaler loads.
    CHECK(OptiscalerDlssgPick(xell, noneExport) == L"libxell.dll");
    // Another provider listed first takes the shim's place.
    const std::vector<std::wstring> other{L"dlssg_to_fsr3.dll", L"mfgunlocker_xemfg.dll"};
    CHECK(OptiscalerDlssgPick(other, allExport) == L"dlssg_to_fsr3.dll");
    const std::vector<std::wstring> named{L"libxess_fg.dll", L"mfgunlocker_xemfg.dll", L"NVNGX_DLSSG.DLL"};
    CHECK(OptiscalerDlssgPick(named, shimExports) == L"NVNGX_DLSSG.DLL");
    const std::vector<std::wstring> notes{L"readme.txt", L"Mfgunlocker_xemfg.DLL"};
    CHECK(SameName(OptiscalerDlssgPick(notes, shimExports), L"mfgunlocker_xemfg.dll"));
    const std::vector<std::wstring> none{L"readme.txt", L"backup.dll.bak", L".dll"};
    CHECK(OptiscalerDlssgPick(none, allExport).empty());
    CHECK(IsDll(L"a.DLL") && !IsDll(L".dll") && !IsDll(L"dll"));
    // D3D12: back-buffer copies and outputs in the UAV-capable member of the back buffer's group.
    CHECK(ColorFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(ColorFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(ColorFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS) == DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(ColorFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS) == DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK(ColorFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(ColorFormat(DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_R11G11B10_FLOAT);
    CHECK(ColorFormat(DXGI_FORMAT_R32_FLOAT) == DXGI_FORMAT_UNKNOWN);
    CHECK(ColorFormat(DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_UNKNOWN);
    CHECK(SameColorGroup(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM));
    CHECK(!SameColorGroup(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM));
    CHECK(!SameColorGroup(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT));
    CHECK(!SameColorGroup(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN));
    CHECK(IsDepthStencilFormat(DXGI_FORMAT_D32_FLOAT) && IsDepthStencilFormat(DXGI_FORMAT_D24_UNORM_S8_UINT));
    CHECK(!IsDepthStencilFormat(DXGI_FORMAT_R32_TYPELESS) && !IsDepthStencilFormat(DXGI_FORMAT_R32_FLOAT));
    // The network on this route: the ini's XeFGModel first, then the control file's 2x, else the XMX port on every GPU.
    CHECK(ProviderUsesPort(1, false) && ProviderUsesPort(1, true));
    CHECK(!ProviderUsesPort(2, false) && !ProviderUsesPort(0, true));
    CHECK(ProviderUsesPort(0, false));
    // pluginprov.2: the outputs each evaluation records.
    CHECK((ExecuteRange(true, 1, 1, false) == OutputRange{true, 0, 1, 1}));
    CHECK((ExecuteRange(true, 1, 3, false) == OutputRange{true, 0, 1, 1}));
    CHECK((ExecuteRange(false, 2, 3, false) == OutputRange{true, 1, 1, 2}));
    CHECK((ExecuteRange(false, 3, 3, false) == OutputRange{true, 2, 1, 3}));
    CHECK((ExecuteRange(true, 2, 3, false) == OutputRange{true, 0, 2, 2}));   // index 1 skipped: outputs 0 and 1
    CHECK((ExecuteRange(true, 1, 5, true) == OutputRange{true, 0, 0, 5}));    // the whole batch at once, as before
    CHECK(!ExecuteRange(false, 2, 5, true).record);
    CHECK(!ExecuteRange(false, 4, 3, false).record && !ExecuteRange(true, 0, 3, false).record);
    // pluginprov.2: GPU timing per real frame, from each evaluation's three stamps.
    {
        constexpr uint64_t kHz = 1'000'000;   // 1 tick = 1 us
        GpuTiming timing;
        CHECK(!timing.Add({0, 0, 0}, true, kHz));               // not written yet
        CHECK(!timing.Add({100, 90, 200}, true, kHz));          // out of order
        CHECK(!timing.Add({100, 100, 2'000'200}, true, kHz));   // longer than a second
        CHECK(!timing.Add({100, 200, 300}, true, 0));           // no frequency
        CHECK(timing.Batches() == 0 && timing.TotalMs(kHz) == 0.0);
        CHECK(timing.Add({1000, 1500, 4000}, true, kHz));       // the batch's first evaluation: 0.5 ms of copies, 3 ms
        CHECK(timing.Add({5000, 5000, 6000}, false, kHz));      // a later evaluation: 1 ms
        CHECK(timing.Batches() == 1 && timing.InputsMs(kHz) == 0.5 && timing.TotalMs(kHz) == 4.0);
        CHECK(timing.Add({7000, 8000, 9000}, true, kHz));
        CHECK(timing.Batches() == 2 && timing.InputsMs(kHz) == 0.75 && timing.TotalMs(kHz) == 3.0);
        timing.Reset();
        CHECK(timing.Batches() == 0 && timing.InputsMs(kHz) == 0.0);
    }
    // pluginprov.2: D3D12 depth history as a plain color texture; stencil and color formats keep their own.
    CHECK(ReadableDepthFormat(DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_R32_FLOAT);
    CHECK(ReadableDepthFormat(DXGI_FORMAT_R32_TYPELESS) == DXGI_FORMAT_R32_FLOAT);
    CHECK(ReadableDepthFormat(DXGI_FORMAT_D16_UNORM) == DXGI_FORMAT_R16_UNORM);
    CHECK(ReadableDepthFormat(DXGI_FORMAT_D24_UNORM_S8_UINT) == DXGI_FORMAT_UNKNOWN);
    CHECK(ReadableDepthFormat(DXGI_FORMAT_R32G8X24_TYPELESS) == DXGI_FORMAT_UNKNOWN);
    CHECK(ReadableDepthFormat(DXGI_FORMAT_R32_FLOAT) == DXGI_FORMAT_UNKNOWN);
}

// pluginprov.2: Chromium-family helpers (host_process_policy.h) get proxy forwarding only.
void ChromiumHelper()
{
    using host_process_policy::HasChromiumHelperSwitch;
    // BeamNG.drive's CEF renderer, shortened.
    CHECK(HasChromiumHelperSwitch(L"\"E:\\SteamLibrary\\steamapps\\common\\BeamNG.drive\\Bin64\\BeamNG.drive.x64.exe\" "
                                  L"--type=renderer --user-data-dir=\"C:\\Users\\x\\cef_150\" --no-sandbox /prefetch:1"));
    CHECK(HasChromiumHelperSwitch(L"BeamNG.drive.x64.exe --no-sandbox --type=gpu-process"));
    CHECK(HasChromiumHelperSwitch(L"app.exe\t--type=utility --utility-sub-type=network.mojom.NetworkService"));
    CHECK(HasChromiumHelperSwitch(L"--type=crashpad-handler"));
    // The game (the browser process) has no --type= switch.
    CHECK(!HasChromiumHelperSwitch(L"\"E:\\SteamLibrary\\steamapps\\common\\BeamNG.drive\\Bin64\\BeamNG.drive.x64.exe\""));
    CHECK(!HasChromiumHelperSwitch(L""));
    CHECK(!HasChromiumHelperSwitch(L"game.exe -dx12 -nosplash"));
    // Not a switch of its own: inside a quoted argument, glued to another word, or without a value.
    CHECK(!HasChromiumHelperSwitch(L"game.exe \"--type=renderer\""));
    CHECK(!HasChromiumHelperSwitch(L"game.exe \"C:\\a --type=renderer\\b\""));
    CHECK(!HasChromiumHelperSwitch(L"game.exe --archive-type=renderer"));
    CHECK(!HasChromiumHelperSwitch(L"game.exe x--type=renderer"));
    CHECK(!HasChromiumHelperSwitch(L"game.exe --type="));
    CHECK(!HasChromiumHelperSwitch(L"game.exe --type= --other"));
    CHECK(!HasChromiumHelperSwitch(L"game.exe --type"));
    CHECK(!HasChromiumHelperSwitch(L"game.exe --TYPE=renderer"));
}

// hostport.1: OptiScaler's own XeFG publishes the port, also after the game's own XeFG probe came and went.
void HostPort()
{
    using xess_host_port::MayPublishHostPort;
    using xess_host_port::PortState;
    // Nothing published yet: OptiScaler's context comes first.
    CHECK(MayPublishHostPort(PortState{}));
    // Dying Light: The Beast: the game probed XeFG (create, properties, destroy) first. Its port was restored with the
    // context; the unlock without the 2x-model site stayed on the same image.
    CHECK(MayPublishHostPort({false, false, true, true, false}));
    // A live tracked context or a published model: the caller adopts or falls back, never publishes over them.
    CHECK(!MayPublishHostPort({true, false, false, false, false}));
    CHECK(!MayPublishHostPort({true, false, true, true, false}));
    CHECK(!MayPublishHostPort({false, true, true, true, false}));
    CHECK(!MayPublishHostPort({false, true, false, false, false}));
    // The tracked route chose the 2x model: its unlock has the 2x-model site, so the host keeps the 2x model too.
    CHECK(!MayPublishHostPort({false, false, true, true, true}));
    // An unlock on another provider image.
    CHECK(!MayPublishHostPort({false, false, true, false, false}));
    CHECK(!MayPublishHostPort({false, false, true, false, true}));
}
}

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "ids" || which == "all") Ids();
    if (which == "anchor" || which == "all") PresentAnchor();
    if (which == "sources" || which == "all") Sources();
    if (which == "conventions" || which == "all") Conventions();
    if (which == "generation" || which == "all") Generation();
    if (which == "rates" || which == "all") Rates();
    if (which == "dynamic" || which == "all") Dynamic();
    if (which == "queue" || which == "all") Queue();
    if (which == "wine" || which == "all") WineQueue();
    if (which == "metering" || which == "all") Metering();
    if (which == "routing" || which == "all") Routing();
    if (which == "probes" || which == "all") Probes();
    if (which == "modules" || which == "all") Modules();
    if (which == "provider" || which == "all") Provider();
    if (which == "chromium" || which == "all") ChromiumHelper();
    if (which == "hostport" || which == "all") HostPort();
    std::printf("%s: %d checks, %d failures\n", which.c_str(), gChecks, gFailures);
    return gFailures ? 1 : 0;
}
