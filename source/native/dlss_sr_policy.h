#pragma once

#include "dlssg_preset_policy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// DLSS Super Resolution and Ray Reconstruction detection and live preset
// switching (standalone). The NGX runtime route (D3D12 and Vulkan) sees every
// SuperSampling and RayReconstruction Create, Evaluate and Release. A selected
// preset reaches the provider through the six *.Hint.Render.Preset.* keys of its
// feature on the game's parameter object at each Create (restored after the call).
//
// Each provider reads its DRS preset key once, when NGX initializes it (310.9.16
// NGXDLAA / NGXDLSSD ReadValuesFromDRS), and a value found there (an NVIDIA app
// override) outranks every later hint. With a preset selected at that read, the
// read answers "not set", so the hints decide for the whole session. With
// game/driver selected, the driver's answer passes. dlss_sr_override.h preserves that
// answer on structurally supported RTX 40 routes and temporarily suppresses the
// effective cached word inside the provider's own preset selector, serialized
// with its cache readers/writer. It restores the original value before returning
// and retains no per-device pointer. Unsupported routes keep the init-time
// suppression/restart behavior below.
//
// Hints are read only at Create. A live change therefore re-creates
// the feature inside the game's own Evaluate, on the game's command list and
// with the game's parameter object (only when it is the object of the game's
// Create). Streamline reuses that object for optimal-settings queries and SR/RR;
// replay the original handle's captured creation scalars/hints for Create and
// restore the current values before Evaluate. It evaluates the new handle in
// place of the game's. The game's handle stays alive until the game releases
// it, so its address cannot be reused while the game still holds it. A
// replaced handle of ours is released on the render thread after a few more
// frames, like Streamline's sl.dlss, which re-creates on a preset change and
// releases the old feature three frames later.
namespace dlss_sr
{
enum class Kind : uint8_t { eSuperResolution, eRayReconstruction };

// NVSDK_NGX_DLSS_Hint_Render_Preset (NGX SDK 310.9.x). The DRS key takes the
// same values. 0 leaves the game's hints and the driver profile untouched.
inline constexpr uint32_t kGameOrDriver = 0;
inline constexpr uint32_t kPresetJ = 10;
inline constexpr uint32_t kPresetK = 11;
inline constexpr uint32_t kPresetL = 12;
inline constexpr uint32_t kPresetM = 13;
inline constexpr uint32_t kDrsPresetSetting = 0x10E41DF3u;
// NVSDK_NGX_RayReconstruction_Hint_Render_Preset: D transformer, E newer
// transformer, F RR2 (310.9.16 requires driver 580 or newer for F).
inline constexpr uint32_t kRrPresetD = 4;
inline constexpr uint32_t kRrPresetE = 5;
inline constexpr uint32_t kRrPresetF = 6;
inline constexpr uint32_t kDrsRrPresetSetting = 0x10E41DF7u;
// The NVIDIA app's "Latest": the provider picks its newest preset per mode.
inline constexpr uint32_t kDrsLatest = 0xFFFFFFu;

constexpr bool IsOverride(uint32_t preset) noexcept
{
    return preset >= kPresetJ && preset <= kPresetM;
}

constexpr bool IsValidSelection(uint32_t preset) noexcept
{
    return preset == kGameOrDriver || IsOverride(preset);
}

constexpr bool IsRrOverride(uint32_t preset) noexcept
{
    return preset >= kRrPresetD && preset <= kRrPresetF;
}

constexpr bool IsValidRrSelection(uint32_t preset) noexcept
{
    return preset == kGameOrDriver || IsRrOverride(preset);
}

constexpr bool IsOverrideFor(Kind kind, uint32_t preset) noexcept
{
    return kind == Kind::eSuperResolution ? IsOverride(preset) : IsRrOverride(preset);
}

constexpr bool IsValidSelectionFor(Kind kind, uint32_t preset) noexcept
{
    return kind == Kind::eSuperResolution ? IsValidSelection(preset) : IsValidRrSelection(preset);
}

// NVSDK_NGX_PerfQuality_Value -> index of the hint key the provider reads.
enum class HintKey : uint8_t { ePerformance, eBalanced, eQuality, eUltraPerformance, eUltraQuality, eDlaa, eCount };
constexpr HintKey HintForQuality(int32_t perfQuality) noexcept
{
    switch (perfQuality)
    {
    case 0: return HintKey::ePerformance;
    case 1: return HintKey::eBalanced;
    case 2: return HintKey::eQuality;
    case 3: return HintKey::eUltraPerformance;
    case 4: return HintKey::eUltraQuality;
    case 5: return HintKey::eDlaa;
    default: return HintKey::eCount;
    }
}
using HintNames = std::array<const char*, static_cast<size_t>(HintKey::eCount)>;
inline constexpr HintNames kHintNames{
    "DLSS.Hint.Render.Preset.Performance", "DLSS.Hint.Render.Preset.Balanced",
    "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.UltraPerformance",
    "DLSS.Hint.Render.Preset.UltraQuality", "DLSS.Hint.Render.Preset.DLAA"};
inline constexpr HintNames kRrHintNames{
    "RayReconstruction.Hint.Render.Preset.Performance", "RayReconstruction.Hint.Render.Preset.Balanced",
    "RayReconstruction.Hint.Render.Preset.Quality", "RayReconstruction.Hint.Render.Preset.UltraPerformance",
    "RayReconstruction.Hint.Render.Preset.UltraQuality", "RayReconstruction.Hint.Render.Preset.DLAA"};
constexpr const HintNames& HintNamesFor(Kind kind) noexcept
{
    return kind == Kind::eSuperResolution ? kHintNames : kRrHintNames;
}

// The keys an SR or RR Create consumes besides the preset hints
// (NGX_D3D12_CREATE_DLSS_EXT, NGX_D3D12_CREATE_DLSSD_EXT, sl.dlss 2.14). A live
// re-create replays these captured values on the exact current parameter object,
// then restores the current query/evaluation values before forwarding Evaluate.
struct CreateKeys
{
    enum : uint32_t
    {
        eWidth = 1u << 0, eHeight = 1u << 1, eOutWidth = 1u << 2, eOutHeight = 1u << 3,
        eCreationNodeMask = 1u << 4, eVisibilityNodeMask = 1u << 5,
        ePerfQuality = 1u << 6, eCreateFlags = 1u << 7, eOutputSubrects = 1u << 8,
        eDenoiseMode = 1u << 9, eRoughnessMode = 1u << 10, eUseHwDepth = 1u << 11,
        eRequired = eWidth | eHeight | eOutWidth | eOutHeight,
    };
    uint32_t present = 0;
    uint32_t width = 0, height = 0, outWidth = 0, outHeight = 0;
    uint32_t creationNodeMask = 0, visibilityNodeMask = 0;
    int32_t perfQuality = -1, createFlags = 0, outputSubrects = 0;
    int32_t denoiseMode = 0;
    uint32_t roughnessMode = 0, useHwDepth = 0;
    uint32_t signedValues = 0;

    bool Complete() const noexcept { return (present & eRequired) == eRequired; }
    friend bool operator==(const CreateKeys&, const CreateKeys&) = default;
};

// Why the selection is not live on the evaluated feature (0 = it is, or nothing is pending).
enum class Unavailable : uint32_t
{
    eNone = 0,
    // Another parameter object, or the captured keys cannot be safely replayed.
    eParameters = 1,
    // The provider refused the re-create.
    eCreateFailed = 2,
    // Too many replaced handles are still waiting for release.
    eCapacity = 3,
    // The NGX runtime route of this graphics API is not covered (D3D12 and
    // Vulkan are).
    eGraphicsApi = 4,
    // The provider cached an NVIDIA app preset override at its initialization.
    eDriverOverride = 5,
};

// Release a replaced handle only after the game evaluated its feature this
// many more times (one per rendered frame) and this much time passed.
inline constexpr uint64_t kRetireFrames = 4;
inline constexpr uint64_t kRetireMs = 250;
// A feature counts as running while the game evaluated it this recently.
inline constexpr uint64_t kRunningMs = 1000;

// The NGX runtime entry of a Create: a live re-create
// enters the same one as the game's Create.
enum class CreateApi : uint8_t { eD3D12, eVulkan, eVulkan1 };

struct CreateRecord
{
    size_t route = 0;
    uintptr_t handle = 0;
    uintptr_t parameters = 0;
    Kind kind = Kind::eSuperResolution;
    CreateApi api = CreateApi::eD3D12;
    // Vulkan CreateFeature1 only: the VkDevice the game named (identity; the
    // game keeps it alive while its feature lives).
    uintptr_t device = 0;
    CreateKeys keys{};
    std::array<uint32_t, kHintNames.size()> hints{};
    uint32_t hintsPresent = 0, hintsSigned = 0;
    // The game's own hint for its quality mode (0 when absent).
    uint32_t gameHint = 0;
    // The selection this Create received (0 = game/driver).
    uint32_t preset = 0;
};

enum class Action : uint8_t
{
    // Evaluate `handle` (the game's own or the current replacement).
    eForward,
    // Create a replacement for `preset` with the game's parameter object, then
    // evaluate it; report the outcome through Recreated.
    eRecreate,
};

struct Plan
{
    Action action = Action::eForward;
    uintptr_t handle = 0;
    uint32_t preset = 0;
    CreateRecord creation{};
};

struct FeatureView
{
    bool valid = false;
    CreateKeys keys{};
    uint32_t gameHint = 0;
    // Preset of the handle the game's evaluations currently reach.
    uint32_t activePreset = 0;
    bool replaced = false;
    Unavailable unavailable = Unavailable::eNone;
};

struct Counters
{
    uint64_t releases = 0;
    uint64_t recreateFailures = 0;
    uint64_t srCreates = 0;
    uint64_t rrCreates = 0;
    uint32_t liveSr = 0;
    uint32_t liveRr = 0;
    uint32_t retiredPending = 0;
    // A successful Create could not be recorded (table full): never switch it.
    bool overflow = false;
};

// Pure bookkeeping for one process. The caller serializes every call (one
// mutex) and performs every native Create/Evaluate/Release itself. Selections
// passed in belong to the feature's kind (SR or RR presets).
template <size_t Features = 8, size_t Retired = 8>
class Tracker
{
public:
    bool Created(const CreateRecord& record, uint64_t nowMs) noexcept
    {
        if (!record.handle) return false;
        ++(record.kind == Kind::eSuperResolution ? counters_.srCreates : counters_.rrCreates);
        // A recycled address of a feature the game already released.
        if (Feature* stale = Find(record.route, record.handle)) *stale = {};
        for (auto& feature : features_)
        {
            if (feature.record.handle) continue;
            feature = {};
            feature.record = record;
            feature.activePreset = record.preset;
            feature.createdMs = nowMs;
            Count();
            return true;
        }
        counters_.overflow = true;
        return false;
    }

    // True when an Evaluate of `handle` with `selection` would try a re-create,
    // so the caller reads the parameter object's create keys first.
    bool NeedsKeys(size_t route, uintptr_t handle, uint32_t selection, bool fixedByDriver) const noexcept
    {
        const Feature* feature = Find(route, handle);
        return feature && !feature->creating && !fixedByDriver
            && IsValidSelectionFor(feature->record.kind, selection) && selection != feature->activePreset
            && selection != feature->record.preset && selection != feature->failedSelection;
    }

    bool Tracked(size_t route, uintptr_t handle, Kind* kind = nullptr) const noexcept
    {
        const Feature* feature = Find(route, handle);
        if (feature && kind) *kind = feature->record.kind;
        return feature != nullptr;
    }

    // Decides which handle an Evaluate of the game's `handle` reaches. `keys`
    // is null when the caller did not read them (NeedsKeys was false); a
    // re-create then waits for the next Evaluate. `fixedByDriver`: the provider
    // cached an NVIDIA app override, so no hint can change its preset. A
    // replaced handle due for release is reported by DueRetired, not here.
    Plan Evaluate(size_t route, uintptr_t handle, uintptr_t parameters, const CreateKeys* keys,
        uint32_t selection, uint64_t nowMs, bool fixedByDriver = false) noexcept
    {
        Feature* feature = Find(route, handle);
        if (!feature) return {Action::eForward, handle, 0};
        ++feature->evaluates;
        feature->lastEvaluateMs = nowMs;
        const uintptr_t active = feature->replacement ? feature->replacement : handle;
        // Another thread is re-creating this feature: keep the current handle.
        if (feature->creating) return {Action::eForward, active, feature->activePreset};
        if (!IsValidSelectionFor(feature->record.kind, selection) || selection == feature->activePreset)
        {
            if (selection == feature->activePreset) feature->unavailable = Unavailable::eNone;
            return {Action::eForward, active, feature->activePreset};
        }
        // Back to the preset of the game's own handle: no Create needed.
        if (selection == feature->record.preset)
        {
            if (!Retire(*feature, feature->replacement, nowMs))
            {
                feature->unavailable = Unavailable::eCapacity;
                return {Action::eForward, active, feature->activePreset};
            }
            feature->replacement = 0;
            feature->activePreset = feature->record.preset;
            feature->unavailable = Unavailable::eNone;
            Count();
            return {Action::eForward, handle, feature->activePreset};
        }
        if (feature->failedSelection == selection)
            return {Action::eForward, active, feature->activePreset};
        if (fixedByDriver) return Fail(*feature, selection, Unavailable::eDriverOverride);
        if (!keys) return {Action::eForward, active, feature->activePreset};
        if (parameters != feature->record.parameters || !feature->record.keys.Complete() || !keys->Complete()
            || (keys->present & feature->record.keys.present) != feature->record.keys.present)
            return Fail(*feature, selection, Unavailable::eParameters);
        if (feature->replacement && RetiredFree() == 0)
            return Fail(*feature, selection, Unavailable::eCapacity);
        feature->creating = true;
        return {Action::eRecreate, active, selection, feature->record};
    }

    // Outcome of an eRecreate. Returns the handle to evaluate now; `adopted`
    // is false when the game's feature is gone and the caller must release
    // `created` itself (it was never evaluated).
    uintptr_t Recreated(size_t route, uintptr_t handle, uint32_t preset, uintptr_t created,
        uint64_t nowMs, bool& adopted) noexcept
    {
        adopted = false;
        Feature* feature = Find(route, handle);
        if (!feature || !feature->creating)
            return created ? 0 : handle;
        feature->creating = false;
        const uintptr_t previous = feature->replacement ? feature->replacement : handle;
        if (!created)
        {
            ++counters_.recreateFailures;
            Fail(*feature, preset, Unavailable::eCreateFailed);
            return previous;
        }
        if (feature->replacement && !Retire(*feature, feature->replacement, nowMs))
        {
            // Evaluate checked capacity; nothing else retires concurrently.
            ++counters_.recreateFailures;
            Fail(*feature, preset, Unavailable::eCapacity);
            return previous;
        }
        adopted = true;
        feature->replacement = created;
        feature->activePreset = preset;
        feature->failedSelection = UINT32_MAX;
        feature->unavailable = Unavailable::eNone;
        Count();
        return created;
    }

    // Handles of ours due for release: not evaluated for kRetireFrames later
    // evaluations of their feature and kRetireMs. Removed from the table.
    template <size_t N>
    size_t DueRetired(uint64_t nowMs, std::array<uintptr_t, N>& out) noexcept
    {
        size_t count = 0;
        for (auto& retired : retired_)
        {
            if (!retired.handle || count == N) continue;
            const Feature* owner = Find(retired.route, retired.owner);
            const bool frames = !owner || owner->evaluates - retired.evaluates >= kRetireFrames;
            if (!frames || nowMs - retired.ms < kRetireMs) continue;
            out[count++] = retired.handle;
            retired = {};
        }
        Count();
        return count;
    }

    // The game releases `handle`: every handle of ours that replaced it is
    // released first (the game owns GPU completion of its feature, and ours
    // were used no later). Returns their count in `out`.
    template <size_t N>
    size_t Releasing(size_t route, uintptr_t handle, std::array<uintptr_t, N>& out) noexcept
    {
        size_t count = 0;
        Feature* feature = Find(route, handle);
        if (!feature) return 0;
        if (feature->replacement && count < N) out[count++] = feature->replacement;
        for (auto& retired : retired_)
        {
            if (!retired.handle || retired.route != route || retired.owner != handle || count == N) continue;
            out[count++] = retired.handle;
            retired = {};
        }
        feature->replacement = 0;
        return count;
    }

    // After the game's own Release of `handle` returned `success`.
    void Released(size_t route, uintptr_t handle, bool success) noexcept
    {
        Feature* feature = Find(route, handle);
        if (!feature) return;
        if (success)
        {
            ++counters_.releases;
            *feature = {};
        }
        else
        {
            // The game still holds it; its own preset is what remains.
            feature->activePreset = feature->record.preset;
        }
        Count();
    }

    // The game's feature of `kind` evaluated most recently (or created most recently).
    FeatureView Primary(Kind kind = Kind::eSuperResolution) const noexcept
    {
        const Feature* best = nullptr;
        for (const auto& feature : features_)
        {
            if (!feature.record.handle || feature.record.kind != kind) continue;
            if (!best || Recency(feature) > Recency(*best)) best = &feature;
        }
        if (!best) return {};
        FeatureView view{};
        view.valid = true;
        view.keys = best->record.keys;
        view.gameHint = best->record.gameHint;
        view.activePreset = best->activePreset;
        view.replaced = best->replacement != 0;
        view.unavailable = best->unavailable;
        return view;
    }

    bool Running(Kind kind, uint64_t nowMs) const noexcept
    {
        for (const auto& feature : features_)
            if (feature.record.handle && feature.record.kind == kind && feature.evaluates
                && nowMs - feature.lastEvaluateMs <= kRunningMs) return true;
        return false;
    }

    const Counters& Read() const noexcept { return counters_; }

private:
    struct Feature
    {
        CreateRecord record{};
        uintptr_t replacement = 0;
        uint32_t activePreset = 0;
        // Selection whose live switch failed on this feature; retried only
        // after the selection changes.
        uint32_t failedSelection = UINT32_MAX;
        Unavailable unavailable = Unavailable::eNone;
        bool creating = false;
        uint64_t evaluates = 0;
        uint64_t lastEvaluateMs = 0;
        uint64_t createdMs = 0;
    };
    struct RetiredHandle
    {
        size_t route = 0;
        uintptr_t handle = 0;
        uintptr_t owner = 0;
        uint64_t evaluates = 0;
        uint64_t ms = 0;
    };

    static uint64_t Recency(const Feature& feature) noexcept
    {
        return feature.lastEvaluateMs > feature.createdMs ? feature.lastEvaluateMs : feature.createdMs;
    }

    Feature* Find(size_t route, uintptr_t handle) noexcept
    {
        if (!handle) return nullptr;
        for (auto& feature : features_)
            if (feature.record.handle == handle && feature.record.route == route) return &feature;
        return nullptr;
    }
    const Feature* Find(size_t route, uintptr_t handle) const noexcept
    {
        return const_cast<Tracker*>(this)->Find(route, handle);
    }

    size_t RetiredFree() const noexcept
    {
        size_t free = 0;
        for (const auto& retired : retired_) free += retired.handle == 0;
        return free;
    }

    bool Retire(const Feature& feature, uintptr_t handle, uint64_t nowMs) noexcept
    {
        if (!handle) return true;
        for (auto& retired : retired_)
        {
            if (retired.handle) continue;
            retired = {feature.record.route, handle, feature.record.handle, feature.evaluates, nowMs};
            return true;
        }
        return false;
    }

    Plan Fail(Feature& feature, uint32_t selection, Unavailable reason) noexcept
    {
        feature.creating = false;
        feature.failedSelection = selection;
        feature.unavailable = reason;
        return {Action::eForward, feature.replacement ? feature.replacement : feature.record.handle,
            feature.activePreset, feature.record};
    }

    void Count() noexcept
    {
        uint32_t sr = 0, rr = 0, pending = 0;
        for (const auto& feature : features_)
        {
            if (!feature.record.handle) continue;
            (feature.record.kind == Kind::eSuperResolution ? sr : rr) += 1;
        }
        for (const auto& retired : retired_) pending += retired.handle != 0;
        counters_.liveSr = sr;
        counters_.liveRr = rr;
        counters_.retiredPending = pending;
    }

    std::array<Feature, Features> features_{};
    std::array<RetiredHandle, Retired> retired_{};
    Counters counters_{};
};

// The DRS preset key a provider reads for its feature.
constexpr bool KindForDrsKey(uint32_t id, Kind& kind) noexcept
{
    if (id == kDrsPresetSetting) { kind = Kind::eSuperResolution; return true; }
    if (id == kDrsRrPresetSetting) { kind = Kind::eRayReconstruction; return true; }
    return false;
}

// A provider reads its DRS preset key at its initialization: the application
// profile, then the global one, taking the first value set in the profile
// itself (location 0). Same NVDRS_SETTING_V1 contract as the DLSS-G key in
// dlssg_preset_policy.h.
inline bool CompatibleDrsRead(uint32_t id, int32_t result, const dlssg_preset::Setting& setting) noexcept
{
    Kind kind{};
    return KindForDrsKey(id, kind) && setting.version == dlssg_preset::kSettingVersion
        && (result == dlssg_preset::kSettingNotFound
            || (result == dlssg_preset::kOk && setting.id == id && setting.type == 0));
}

// The value the provider accepts from this read, if any.
inline bool DriverValue(uint32_t id, int32_t result, const dlssg_preset::Setting& setting, uint32_t& value) noexcept
{
    if (result != dlssg_preset::kOk || !CompatibleDrsRead(id, result, setting) || setting.location != 0)
        return false;
    value = setting.current.dword;
    return true;
}

// With a preset of that feature selected, the provider's read finds no
// override, so the per-Create hints decide. `selection` is the selection of
// the key's feature.
inline bool SuppressDrsRead(uint32_t id, int32_t result, uint32_t selection, int32_t& replaced) noexcept
{
    (void)result;
    Kind kind{};
    if (!KindForDrsKey(id, kind) || !IsOverrideFor(kind, selection)) return false;
    replaced = dlssg_preset::kSettingNotFound;
    return true;
}

// The provider cached an override at its initialization that no hint changes.
constexpr bool FixedByDriver(bool observed, bool valueValid, uint32_t value, bool suppressed) noexcept
{
    return observed && valueValid && value != 0 && !suppressed;
}
}
