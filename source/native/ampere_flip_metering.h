#pragma once
#include <cstddef>
#include <cstdint>

// RTX 30: the DLSS-G Streamline wrapper's flip-metering presentation.
//
// The 2.14 wrapper probes NVIDIA flip metering through nvapi_QueryInterface
// (flipMetering.cpp initialize): SetFlipConfig and its V2 capability query.
// A consumer RTX 30 driver accepts SetFlipConfig; after 30 "good" feedback
// states the wrapper presents through flip metering, which sizes its
// clone.dlfg-output pool to two surfaces instead of count + 1, so generated
// frames of one batch share an output. By default these two interfaces are
// withheld and the wrapper keeps the CPU-paced path (distinct outputs; the path
// the A10G GRID driver takes, final-present validated at 4x/6x). Every other
// NVAPI interface is forwarded unchanged.
//
// Flip-metering path ([Compatibility] Rtx30FlipMetering in RTX40MFG-UI.ini;
// on by default, kOnByDefault; =0 turns it off): both
// interfaces are forwarded, SetFlipConfig through an observer. In flip-metering
// mode the wrapper runs frame generation on its pacer command context, so each
// generated frame is presented before the next index writes the shared output;
// this is the mode RTX 40 uses (sl.dlss_g 2.14.1 on the 4080: every index of a
// batch shares one output). The wrapper passes nFramesPerBatch > 0 only while
// its flip-metering flag is set (setFlipConfig, 2.14.1/2.14.16), so a repeated
// output inside one batch is admitted only while the last observed call says
// so. Anything else keeps the distinct-output rule.
namespace ampere_flip_metering
{
inline constexpr uint32_t kSetFlipConfig = 0xF3148C42u;
inline constexpr uint32_t kFlipConfigCaps = 0x6194B19Du;

// The two flip-metering interfaces (withheld unless the flip-metering path is on).
constexpr bool Hidden(uint32_t id) noexcept
{
    return id == kSetFlipConfig || id == kFlipConfigCaps;
}

static_assert(Hidden(kSetFlipConfig) && Hidden(kFlipConfigCaps));
static_assert(!Hidden(0x0150E828u) && !Hidden(0x85A6C2A0u)); // Initialize, GetRaytracingCaps

// NV_SET_FLIP_CONFIG_PARAMS as the wrapper fills it: version, nFramesPerBatch,
// feedback (out), nPresentedFramesPerBatch, timePerBatchMs, minControl.
inline constexpr uint32_t kParamsV1 = 0x10018u, kParamsV2 = 0x20018u;
inline constexpr uint32_t kParamsBytes = 0x18u;
inline constexpr uint32_t kMaximumFramesPerBatch = 6; // 6x: five generated frames and the real one

constexpr bool KnownVersion(uint32_t version) noexcept
{
    return version == kParamsV1 || version == kParamsV2;
}

enum class Observation : uint8_t { eIgnored, eMetered, eUnmetered };

// One SetFlipConfig call: metered only for a known layout, a batch size the
// fixed capacity covers, and a call the driver accepted (NVAPI_OK). A refused
// or unknown call never admits repeated outputs.
constexpr Observation Classify(uint32_t version, uint32_t framesPerBatch, int32_t status) noexcept
{
    if (!KnownVersion(version)) return Observation::eIgnored;
    if (status != 0) return Observation::eUnmetered;
    if (framesPerBatch == 0) return Observation::eUnmetered;
    return framesPerBatch <= kMaximumFramesPerBatch ? Observation::eMetered : Observation::eUnmetered;
}

static_assert(Classify(kParamsV2, 4, 0) == Observation::eMetered);
static_assert(Classify(kParamsV1, 2, 0) == Observation::eMetered);
static_assert(Classify(kParamsV2, 0, 0) == Observation::eUnmetered);
static_assert(Classify(kParamsV2, 4, -9) == Observation::eUnmetered);
static_assert(Classify(kParamsV2, 7, 0) == Observation::eUnmetered);
static_assert(Classify(0x30018u, 4, 0) == Observation::eIgnored);

// The V2 generation (Streamline 2.11+): its flipMetering initialize asks for
// the V2 capability interface (mov ecx, 0x6194B19D) and its setFlipConfig
// passes nFramesPerBatch only while the flip-metering flag is set (decoded in
// 2.11.0 86E55063 and 2.14.16 3ED9538D). Older wrappers keep both interfaces
// withheld even with the flip-metering path on.
bool ContainsCapsQuery(const uint8_t* data, size_t bytes) noexcept;
bool ImageQueriesCaps(void* module) noexcept; // executable sections of a loaded image

enum class Setting : uint8_t { eDefault, eOff, eOn, eInvalid };
inline constexpr wchar_t kSection[] = L"Compatibility";
inline constexpr wchar_t kKey[] = L"Rtx30FlipMetering";
inline constexpr bool kOnByDefault = true;

// Missing file or key is eDefault; 0, false and off are off; 1, true and on are
// on (any case); anything else is invalid and follows the default.
Setting ReadSetting(const wchar_t* iniPath) noexcept;
constexpr bool Enabled(Setting setting) noexcept
{
    return setting == Setting::eOn || (setting != Setting::eOff && kOnByDefault);
}
static_assert(Enabled(Setting::eOn) && !Enabled(Setting::eOff)
    && Enabled(Setting::eDefault) == kOnByDefault && Enabled(Setting::eInvalid) == kOnByDefault);

// Latched once per process, before the wrapper's startup probe; later calls
// keep the first value.
void Configure(bool optIn) noexcept;
bool Configured() noexcept;
bool OptIn() noexcept;

// Reads version and nFramesPerBatch from the wrapper's params (SEH-guarded).
bool ReadParams(const void* params, uint32_t& version, uint32_t& framesPerBatch) noexcept;
// Records one forwarded SetFlipConfig call. Returns true when it changed Active().
bool Observe(uint32_t version, uint32_t framesPerBatch, int32_t status) noexcept;
// SetFlipConfig is forwarded through the observer (flip-metering path only).
void MarkObserving() noexcept;
bool Observing() noexcept;
// The last classified call was metered, while the flip-metering path is on.
bool Active() noexcept;

struct Counters
{
    uint64_t calls = 0, metered = 0, unmetered = 0, ignored = 0, transitions = 0;
    uint32_t lastFramesPerBatch = 0, lastVersion = 0;
};
Counters Read() noexcept;

#if MFG_AMPERE_FLIP_METERING_TESTING
namespace testing { void Reset() noexcept; }
#endif
}
