#pragma once

#include "dlss_sr_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// DLSS 5 (NVIDIA's DLSS Neural Rendering, NGX feature "DLSSNR", provider
// nvngx_dlssnr.dll) on the game's own DLSS (standalone).
//
// The installed NGX runtime names feature 18 DLSSNR. This module can use
// its native feature route or a user-provided compatible provider directly.
// It owns separate parameters, color/output textures and explicit queue fences.
// Neural Create and first Evaluate occur on separate completed submissions;
// settings replacement and release require actual completion, never frame delays.
// No NVIDIA files or GPU architecture declarations are changed on disk.
namespace dlss5
{
inline constexpr uint32_t kFeature = 18;

namespace keys
{
// Create.
inline constexpr const char* kWidth = "DLSSNR.Width";
inline constexpr const char* kHeight = "DLSSNR.Height";
inline constexpr const char* kPreset = "DLSSNR.Hint.Render.Preset";
inline constexpr const char* kPerfQuality = "PerfQualityValue";
inline constexpr const char* kScalingRatio = "DLSSNR.ScalingRatio";
inline constexpr const char* kCreationNodeMask = "CreationNodeMask";
inline constexpr const char* kVisibilityNodeMask = "VisibilityNodeMask";
// Evaluate.
inline constexpr const char* kColor = "DLSSNR.Color";
inline constexpr const char* kOutput = "DLSSNR.Output";
inline constexpr const char* kMotion = "DLSSNR.MVec";
inline constexpr const char* kDepth = "DLSSNR.Depth";
inline constexpr const char* kMotionScaleX = "DLSSNR.MVecScaleX";
inline constexpr const char* kMotionScaleY = "DLSSNR.MVecScaleY";
inline constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
inline constexpr const char* kReset = "DLSSNR.Reset";
inline constexpr const char* kEnabled = "DLSSNR.Enabled";
// The provider's image controls (310.9.16 reads each of these keys).
inline constexpr const char* kIntensity = "DLSSNR.Intensity";
inline constexpr const char* kStyle = "DLSSNR.Style";
inline constexpr const char* kLocalStructure = "DLSSNR.LocalStructureStrength";
inline constexpr const char* kLocalTone = "DLSSNR.LocalToneStrength";
inline constexpr const char* kSkinStructure = "DLSSNR.SkinStructureStrength";
inline constexpr const char* kAutoMask = "DLSSNR.UseAutoMask";
inline constexpr const char* kUiCorrection = "DLSSNR.UICorrection";
// A resource key and its subrect keys (base X, base Y, width, height).
struct ResourceKeys
{
    const char* resource;
    std::array<const char*, 4> rect;
};
inline constexpr ResourceKeys kColorKeys{kColor, {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
    "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight"}};
inline constexpr ResourceKeys kOutputKeys{kOutput, {"DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
    "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight"}};
inline constexpr ResourceKeys kMotionKeys{kMotion, {"DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
    "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight"}};
inline constexpr ResourceKeys kDepthKeys{kDepth, {"DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
    "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight"}};
// Capability parameters (NVSDK_NGX_D3D12_GetCapabilityParameters).
inline constexpr const char* kAvailable = "DLSSNR.Available";
inline constexpr const char* kNeedsUpdatedDriver = "DLSSNR.NeedsUpdatedDriver";
inline constexpr const char* kMinDriverMajor = "DLSSNR.MinDriverVersionMajor";
inline constexpr const char* kMinDriverMinor = "DLSSNR.MinDriverVersionMinor";
inline constexpr const char* kFeatureInitResult = "DLSSNR.FeatureInitResult";
}

// NVSDK_NGX_PerfQuality_Value MaxQuality: the network runs at the extent it is
// created for. Resolution Scale resamples into that extent instead.
inline constexpr uint32_t kQualityFull = 2;

// Hook Method. Off is Settings::enabled == false; the saved stage is the last
// one picked.
enum class Stage : uint32_t
{
    // Picked by DLSS 5 Auto (dlss5_auto.h): after upscaling.
    eAuto = 0,
    // The DLSS SR/DLAA/RR input, at render resolution, before reconstruction.
    eRender = 1,
    // The DLSS SR/DLAA/RR output, after successful reconstruction.
    eUpscaled = 2,
    // The swapchain buffer before presentation.
    ePresent = 3,
};

// The choices of each setting, in menu order. The values are part of the
// native control file and the status protocol.
enum class Resources : uint32_t { eNever = 0, eOptional = 1, eRequired = 2 };
enum class UiCorrection : uint32_t { eAuto = 0, eOff = 1, eOn = 2 };
enum class Encoding : uint32_t { eAuto = 0, eLinear = 1, eSrgb = 2, eHdr10 = 3, eScrgb = 4, eScrgbNonlinear = 5 };
enum class SignalEncoding : uint32_t { eSrgb = 0, ePqSdr = 1, eLinearSdr = 2 };
enum class ScalingDomain : uint32_t { eScrgbNonlinear = 0, eSignedPq = 1, eLinear = 2 };
enum class DownsampleFilter : uint32_t { eArea = 0, eBilinear = 1, eNearest = 2 };
// Edge-aware: bilinear taps weighted by how close each tap's network input is
// to the full-resolution image there, so the change does not cross edges.
enum class UpsampleFilter : uint32_t { eBilinear = 0, eBicubic = 1, eNearest = 2, eEdgeAware = 3 };
// dlss5noratio.1: a saved Ratio (2) falls back to RGB residual; the decode shader keeps its unreachable branch.
enum class Reconstruction : uint32_t { eResidual = 0, eProcessedImage = 1 };
enum class ExposureMode : uint32_t { eOff = 0, eOn = 1, eInverse = 2 };
// Above 100 % Resolution Scale: how the network's extent comes back down.
enum class SupersampleFilter : uint32_t { eBilinear = 0, eArea = 1 };
// Boost's detail thresholds (BoostThresholds).
enum class BoostTolerance : uint32_t { eStrict = 0, eNormal = 1, eLoose = 2 };
// Half rate (Boost): frames between network runs rebuild the look from their own
// image (BoostGrid), so only fine detail moves along the motion vectors.
inline constexpr bool kHalfRateOffered = true;
// What the DLSS 5 side of the image shows instead of the result.
enum class DebugView : uint32_t { eOff = 0, eNetworkInput = 1, eNetworkOutput = 2, eChange = 3 };

// The defaults generation of saved settings (dlss5DefaultsVersion): 2 brought
// the provider's own look and Resolution Scale Auto off, 3 Adaptive exposure
// off; configs without the key were written with generation 1 (MigrateDefaults).
inline constexpr uint32_t kDefaultsVersion = 3;

// Saved settings (dlss5* keys in the native control file, dlss5_settings.h).
// Ranges follow the RenoDX DLSS add-on's Neural Rendering page; the look's
// defaults are the provider's own.
struct Settings
{
    bool enabled = false;
    uint32_t stage = static_cast<uint32_t>(Stage::eAuto);
    uint32_t resources = static_cast<uint32_t>(Resources::eOptional);
    uint32_t uiCorrection = static_cast<uint32_t>(UiCorrection::eAuto);
    uint32_t encoding = static_cast<uint32_t>(Encoding::eAuto);
    // Used with an explicit encoding once it was set by hand.
    float diffuseWhite = 100.0f;
    bool diffuseWhiteOverride = false;
    // DLSSNR.Intensity, Style, Local/Skin strengths and UseAutoMask. 310.9.16
    // reads absent keys as Intensity, Structure and Tone 1 and Skin -1 (which
    // follows Structure where the mask finds skin): those are the defaults.
    float intensity = 1.0f;
    uint32_t model = 0;
    float structure = 1.0f;
    float tone = 1.0f;
    bool characterMask = true;
    float character = -1.0f;
    // Percent of the stage's extent the network processes (Scaling). With
    // scaleAuto, the most Auto may use (dlss5_auto.h ScaleController); off by
    // default, so the network sees the whole stage.
    float scale = 100.0f;
    bool scaleAuto = false;
    uint32_t domain = static_cast<uint32_t>(ScalingDomain::eScrgbNonlinear);
    uint32_t downsample = static_cast<uint32_t>(DownsampleFilter::eArea);
    uint32_t upsample = static_cast<uint32_t>(UpsampleFilter::eBilinear);
    uint32_t reconstruction = static_cast<uint32_t>(Reconstruction::eResidual);
    // DLSSNR.Hint.Render.Preset (menu: Network): 0 the provider's choice, 1 Quality (its larger network),
    // 2 Fast (its smaller one; 310.9.16 runs it for 0, bit-identical). DLSSNR.Style is Model (menu: Style).
    uint32_t preset = 0;
    // Chained network passes, each its own feature.
    uint32_t passes = 1;
    uint32_t signal = static_cast<uint32_t>(SignalEncoding::eSrgb);
    bool blockHudless = true;
    float exposure = 1.0f;
    uint32_t exposureMode = static_cast<uint32_t>(ExposureMode::eOn);
    // Adaptive exposure (dlss5_auto.h ExposureController), HDR color only. Off
    // by default: it runs a second eye adaptation beside the game's; off, the
    // network sees the game's own exposure, as with SDR color.
    bool adaptiveExposure = false;
    // Boost: the network runs on every other evaluation; the evaluations
    // between carry its last change along the motion vectors (D3D12 Before and
    // After upscaling). Not part of the feature: switching it recreates nothing.
    bool boost = false;
    // Compare, not part of the look: Split view shows the original left of
    // compareSplit percent of the width; holding compareKey (a virtual-key
    // code, 0 = none) shows the original everywhere.
    bool compare = false;
    float compareSplit = 50.0f;
    uint32_t compareKey = 0;
    // Advanced. Passes 2 and later: with laterCustom their own
    // Model, Intensity, Structure, Tone and Faces & skin (PassSettings).
    bool laterCustom = false;
    uint32_t laterModel = 0;
    float laterIntensity = 1.0f, laterStructure = 1.0f, laterTone = 1.0f, laterCharacter = -1.0f;
    // Blend, in the decode pass after the last network pass: the change times
    // blend; color 0 keeps the game's hue (brightness changes only); the
    // highlight guard caps brightening at highlightLimit times the original
    // luminance; halo* take back the fine part of the change beside brighter
    // (dark) or darker (bright) surroundings within haloRadius network texels.
    float blend = 1.0f, color = 1.0f;
    bool highlightGuard = false;
    float highlightLimit = 2.0f;
    float haloDark = 0.0f, haloBright = 0.0f;
    uint32_t haloRadius = 2;
    uint32_t supersample = static_cast<uint32_t>(SupersampleFilter::eBilinear);
    // Where the HDR-to-SDR bridge's shoulder starts (unbounded color only).
    float knee = 0.8f;
    // The log-average luminance Adaptive exposure aims for.
    float adaptiveTarget = 0.18f;
    uint32_t boostTolerance = static_cast<uint32_t>(BoostTolerance::eNormal);
    // Inspect: a debug view on the DLSS 5 side; Split view's original on the right.
    uint32_t debugView = static_cast<uint32_t>(DebugView::eOff);
    bool compareSwap = false;
    uint32_t defaultsVersion = kDefaultsVersion;
    friend bool operator==(const Settings&, const Settings&) = default;
};

inline constexpr uint32_t kMaxPasses = 10;
// Structure, Tone and Faces & skin past 1 (310.9.16 keeps strengthening them;
// it clamps Intensity at 1) and Faces & skin -1, which follows Structure (the
// provider's own default).
inline constexpr float kMaxStrength = 2.0f, kMinCharacter = -1.0f;
inline constexpr float kMaxBlend = 2.0f, kMaxColor = 2.0f;
inline constexpr float kMinHighlightLimit = 1.0f, kMaxHighlightLimit = 8.0f;
inline constexpr uint32_t kMinHaloRadius = 1, kMaxHaloRadius = 4;
inline constexpr float kMinKnee = 0.5f, kMaxKnee = 0.95f;
inline constexpr float kMinAdaptiveTarget = 0.05f, kMaxAdaptiveTarget = 0.5f;
inline constexpr float kMinScale = 25.0f, kMaxScale = 200.0f;
inline constexpr float kMinDiffuseWhite = 48.0f, kMaxDiffuseWhite = 500.0f;
inline constexpr float kMaxExposure = 100.0f;
inline constexpr float kMinCompareSplit = 5.0f, kMaxCompareSplit = 95.0f;

// Keys the compare key may be: not the left/right/middle mouse buttons,
// Escape (cancels binding) or the Windows keys.
constexpr bool CompareKeyAllowed(uint32_t key) noexcept
{
    return key == 0 || (key >= 0x05 && key <= 0xFE && key != 0x1B && key != 0x5B && key != 0x5C);
}
inline constexpr uint32_t kMaxExtent = 16384;

inline float Clamped(float value, float minimum, float maximum, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

// The dlss5* keys of the native control file and the status protocol
// (dlss5_settings.h), shared by every reader and writer and by Sanitized.
namespace settings_json
{
enum class Kind : uint8_t { eBool, eUnsigned, eFloat };

struct Field
{
    const char* key;
    Kind kind;
    float minimum, maximum;
    bool Settings::* flag;
    uint32_t Settings::* integer;
    float Settings::* real;
};

inline constexpr Field Flag(const char* key, bool Settings::* member) { return {key, Kind::eBool, 0, 1, member, nullptr, nullptr}; }
inline constexpr Field Choice(const char* key, uint32_t minimum, uint32_t maximum, uint32_t Settings::* member)
{ return {key, Kind::eUnsigned, static_cast<float>(minimum), static_cast<float>(maximum), nullptr, member, nullptr}; }
inline constexpr Field Real(const char* key, float minimum, float maximum, float Settings::* member)
{ return {key, Kind::eFloat, minimum, maximum, nullptr, nullptr, member}; }

inline constexpr Field kFields[] = {
    Flag("dlss5", &Settings::enabled),
    Choice("dlss5HookStage", 0, 3, &Settings::stage),
    Choice("dlss5UseDlssResources", 0, 2, &Settings::resources),
    Choice("dlss5UiCorrection", 0, 2, &Settings::uiCorrection),
    Choice("dlss5Encoding", 0, 5, &Settings::encoding),
    Real("dlss5DiffuseWhite", kMinDiffuseWhite, kMaxDiffuseWhite, &Settings::diffuseWhite),
    Flag("dlss5DiffuseWhiteOverride", &Settings::diffuseWhiteOverride),
    Real("dlss5OverallIntensity", 0.0f, 1.0f, &Settings::intensity),
    Choice("dlss5Model", 0, 2, &Settings::model),
    Real("dlss5StructureIntensity", 0.0f, kMaxStrength, &Settings::structure),
    Real("dlss5ToneIntensity", 0.0f, kMaxStrength, &Settings::tone),
    Flag("dlss5CharacterMask", &Settings::characterMask),
    Real("dlss5CharacterIntensity", kMinCharacter, kMaxStrength, &Settings::character),
    // 0 % selects the minimum processing extent.
    Real("dlss5ResolutionScale", 0.0f, kMaxScale, &Settings::scale),
    Flag("dlss5ResolutionScaleAuto", &Settings::scaleAuto),
    Choice("dlss5ScalingDomain", 0, 2, &Settings::domain),
    Choice("dlss5DownsampleFilter", 0, 2, &Settings::downsample),
    Choice("dlss5UpsampleFilter", 0, 3, &Settings::upsample),
    // 2 was Ratio (V1.7.0): still read so an older saved file loads; Sanitized makes it RGB residual.
    Choice("dlss5ScaleReconstruction", 0, 2, &Settings::reconstruction),
    Choice("dlss5Preset", 0, 2, &Settings::preset),
    Choice("dlss5PassCount", 1, kMaxPasses, &Settings::passes),
    Choice("dlss5SignalEncoding", 0, 2, &Settings::signal),
    Flag("dlss5BlockHudless", &Settings::blockHudless),
    Real("dlss5Exposure", 0.0f, kMaxExposure, &Settings::exposure),
    Choice("dlss5UseExposureValue", 0, 2, &Settings::exposureMode),
    Flag("dlss5AdaptiveExposure", &Settings::adaptiveExposure),
    Flag("dlss5Boost", &Settings::boost),
    Flag("dlss5CompareSplitView", &Settings::compare),
    Real("dlss5CompareSplit", kMinCompareSplit, kMaxCompareSplit, &Settings::compareSplit),
    Choice("dlss5CompareKey", 0, 254, &Settings::compareKey),
    // Advanced.
    Flag("dlss5LaterPassesCustom", &Settings::laterCustom),
    Choice("dlss5LaterModel", 0, 2, &Settings::laterModel),
    Real("dlss5LaterIntensity", 0.0f, 1.0f, &Settings::laterIntensity),
    Real("dlss5LaterStructure", 0.0f, kMaxStrength, &Settings::laterStructure),
    Real("dlss5LaterTone", 0.0f, kMaxStrength, &Settings::laterTone),
    Real("dlss5LaterCharacter", kMinCharacter, kMaxStrength, &Settings::laterCharacter),
    Real("dlss5BlendStrength", 0.0f, kMaxBlend, &Settings::blend),
    Real("dlss5ColorStrength", 0.0f, kMaxColor, &Settings::color),
    Flag("dlss5HighlightGuard", &Settings::highlightGuard),
    Real("dlss5HighlightLimit", kMinHighlightLimit, kMaxHighlightLimit, &Settings::highlightLimit),
    Real("dlss5HaloDark", 0.0f, 1.0f, &Settings::haloDark),
    Real("dlss5HaloBright", 0.0f, 1.0f, &Settings::haloBright),
    Choice("dlss5HaloRadius", kMinHaloRadius, kMaxHaloRadius, &Settings::haloRadius),
    Choice("dlss5SupersampleFilter", 0, 1, &Settings::supersample),
    Real("dlss5HdrKnee", kMinKnee, kMaxKnee, &Settings::knee),
    Real("dlss5AdaptiveTarget", kMinAdaptiveTarget, kMaxAdaptiveTarget, &Settings::adaptiveTarget),
    Choice("dlss5BoostTolerance", 0, 2, &Settings::boostTolerance),
    Choice("dlss5DebugView", 0, 3, &Settings::debugView),
    Flag("dlss5CompareSwap", &Settings::compareSwap),
    // The defaults generation (MigrateDefaults, kDefaultsVersion). Any later value parses and is not migrated.
    Choice("dlss5DefaultsVersion", 0, 65535, &Settings::defaultsVersion),
};
}

// Every number back in its kFields range: a non-finite value or a choice
// outside it takes the default, a finite value is clamped into it.
inline Settings Sanitized(Settings settings) noexcept
{
    const Settings defaults{};
    for (const settings_json::Field& field : settings_json::kFields)
    {
        // Pass Count, the compare key and Halo radius follow their own rules
        // below; the defaults generation is left as it is.
        if (field.integer == &Settings::passes || field.integer == &Settings::compareKey
            || field.integer == &Settings::haloRadius || field.integer == &Settings::defaultsVersion)
            continue;
        if (field.kind == settings_json::Kind::eFloat)
            settings.*field.real = Clamped(settings.*field.real, field.minimum, field.maximum, defaults.*field.real);
        else if (field.kind == settings_json::Kind::eUnsigned
            && (settings.*field.integer < static_cast<uint32_t>(field.minimum)
                || settings.*field.integer > static_cast<uint32_t>(field.maximum)))
            settings.*field.integer = defaults.*field.integer;
    }
    // Faces & skin is -1 (follows Structure) or 0 and above.
    if (settings.character < 0.0f) settings.character = kMinCharacter;
    if (settings.laterCharacter < 0.0f) settings.laterCharacter = kMinCharacter;
    if (settings.reconstruction > 1) settings.reconstruction = defaults.reconstruction;
    settings.passes = std::clamp(settings.passes, 1u, kMaxPasses);
    if (!CompareKeyAllowed(settings.compareKey)) settings.compareKey = defaults.compareKey;
    settings.haloRadius = std::clamp(settings.haloRadius, kMinHaloRadius, kMaxHaloRadius);
    if (!kHalfRateOffered) settings.boost = false;
    return settings;
}

// The settings network pass `pass` (0 = the first) runs with: passes 2 and
// later take the later-pass Model and strengths when laterCustom is on.
inline Settings PassSettings(const Settings& settings, uint32_t pass) noexcept
{
    Settings result = settings;
    if (pass == 0 || !settings.laterCustom) return result;
    result.model = settings.laterModel;
    result.intensity = settings.laterIntensity;
    result.structure = settings.laterStructure;
    result.tone = settings.laterTone;
    result.character = settings.laterCharacter;
    return result;
}

// The Model passes 2 and later run (part of the feature, like Model).
inline uint32_t LaterModel(const Settings& settings) noexcept
{
    return settings.passes > 1 ? PassSettings(settings, 1).model : settings.model;
}

// Boost detail (network signal, 0..1): where a texel's 3x3 luminance stays
// within `stableBelow` of where it came from, it keeps the fine detail carried
// along the motion vectors; beyond `unstableAbove` it has the rebuilt look only.
struct BoostThreshold { float stableBelow, unstableAbove; };
inline BoostThreshold BoostThresholds(uint32_t tolerance) noexcept
{
    switch (static_cast<BoostTolerance>(tolerance))
    {
    case BoostTolerance::eStrict: return {0.01f, 0.03f};
    case BoostTolerance::eLoose: return {0.04f, 0.12f};
    default: return {0.02f, 0.06f};
    }
}

// Boost's look model: on each network run, a least-squares affine map from the network input to its output per cell
// and luminance bin (bilateral guided upsampling, Chen et al. 2016), applied to the next evaluation's own input. The
// cell grows with the extent (16 texels up to about 1080 lines; a multiple of 16, the fit's thread group). lambda
// pulls each map toward the identity plus its mean change, so flat or empty cells invent no contrast.
inline constexpr uint32_t kBoostBins = 8;
inline constexpr float kBoostLambda = 0.005f;
struct BoostGrid { uint32_t cell, bins, cellsX, cellsY; };
inline bool BoostGridFor(uint32_t width, uint32_t height, BoostGrid& grid) noexcept
{
    if (!width || !height || width > kMaxExtent || height > kMaxExtent) return false;
    const uint32_t scale = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(height / 1080.0)));
    grid.cell = 16 * scale;
    grid.bins = kBoostBins;
    grid.cellsX = (width + grid.cell - 1) / grid.cell;
    grid.cellsY = (height + grid.cell - 1) / grid.cell;
    return true;
}

// Looks: Intensity, Model, Structure, Tone and Faces & skin together.
// Balanced is the default, the provider's own look; Subtle halves its change;
// Strong goes further. Faces & skin -1 follows Structure. Pass Count and
// Resolution Scale stay as set: they change the GPU cost.
struct Look
{
    float intensity;
    uint32_t model;
    float structure, tone, character;
};
inline constexpr Look kLooks[] = {
    {0.5f, 0, 1.0f, 1.0f, -1.0f}, // Subtle
    {1.0f, 0, 1.0f, 1.0f, -1.0f}, // Balanced
    {1.0f, 1, 1.5f, 1.5f, -1.0f}, // Strong
};
inline constexpr size_t kLookCount = std::size(kLooks);
// The looks of defaults generation 1 (the RenoDX add-on's defaults), in kLooks order.
inline constexpr Look kOldLooks[] = {
    {0.5f, 0, 0.7f, 0.3f, 0.5f},
    {1.0f, 0, 0.7f, 0.3f, 0.5f},
    {1.0f, 1, 1.0f, 1.0f, 0.8f},
};

// The look the settings match, kLookCount when none does (Custom).
inline size_t MatchingLook(const Settings& settings) noexcept
{
    const auto same = [](float left, float right) noexcept { return std::abs(left - right) < 0.005f; };
    for (size_t index = 0; index != kLookCount; ++index)
    {
        const Look& look = kLooks[index];
        if (same(settings.intensity, look.intensity) && settings.model == look.model
            && same(settings.structure, look.structure) && same(settings.tone, look.tone)
            && same(settings.character, look.character))
            return index;
    }
    return kLookCount;
}

inline void ApplyLook(Settings& settings, size_t index) noexcept
{
    if (index >= kLookCount) return;
    const Look& look = kLooks[index];
    settings.intensity = look.intensity;
    settings.model = look.model;
    settings.structure = look.structure;
    settings.tone = look.tone;
    settings.character = look.character;
}

// Settings saved with an older defaults generation (`from`) on the new
// defaults. Before generation 2: an untouched old look becomes its new one,
// later-pass strengths at their old defaults take the new ones, and Resolution
// Scale Auto at its old default goes off. Before generation 3: Adaptive exposure
// goes off. Anything else the user set stays.
inline void MigrateDefaults(Settings& settings, uint32_t from) noexcept
{
    const auto same = [](float left, float right) noexcept { return std::abs(left - right) < 0.005f; };
    if (from < 3) settings.adaptiveExposure = false;
    settings.defaultsVersion = kDefaultsVersion;
    if (from >= 2) return;
    for (size_t index = 0; index != std::size(kOldLooks); ++index)
    {
        const Look& old = kOldLooks[index];
        if (same(settings.intensity, old.intensity) && settings.model == old.model && same(settings.structure, old.structure)
            && same(settings.tone, old.tone) && same(settings.character, old.character))
        {
            ApplyLook(settings, index);
            break;
        }
    }
    if (same(settings.laterIntensity, 1.0f) && same(settings.laterStructure, 0.7f) && same(settings.laterTone, 0.3f)
        && same(settings.laterCharacter, 0.5f))
    {
        const Settings defaults{};
        settings.laterStructure = defaults.laterStructure;
        settings.laterTone = defaults.laterTone;
        settings.laterCharacter = defaults.laterCharacter;
    }
    if (settings.scaleAuto && same(settings.scale, 100.0f)) settings.scaleAuto = false;
}

// Changes that replace the feature (its extent, count, preset or model).
// Every other setting applies to the next evaluation. Compares the settings a
// stage runs with: Hook Method Auto and Resolution Scale Auto already resolved.
inline bool SameFeature(const Settings& left, const Settings& right) noexcept
{
    return left.stage == right.stage && left.scale == right.scale
        && left.passes == right.passes && left.preset == right.preset && left.model == right.model
        && LaterModel(left) == LaterModel(right);
}

// nrscale.1: the video memory a create at `width` x `height` takes, from a
// measured create (`bytes` at `measuredWidth` x `measuredHeight`): grown with
// the extent's area, never shrunk. 0 while nothing was measured.
inline uint64_t ReplacementBytes(uint64_t bytes, uint32_t measuredWidth, uint32_t measuredHeight, uint32_t width,
    uint32_t height) noexcept
{
    if (!measuredWidth || !measuredHeight) return bytes;
    const double growth = std::max(1.0, static_cast<double>(width) * height / (static_cast<double>(measuredWidth) * measuredHeight));
    return static_cast<uint64_t>(static_cast<double>(bytes) * growth);
}

// nrscale.1: whether a replacement of `bytes` fits beside the live feature in
// the process's video memory budget (unknown memory: yes). Over budget, Windows
// pages and the game stalls for seconds.
inline bool RoomBeside(uint64_t usage, uint64_t budget, uint64_t bytes) noexcept
{
    return !budget || !bytes || (usage <= budget && bytes <= budget - usage);
}

// `run` with what its feature was created with (`applied`): the settings a
// live feature evaluates with while its replacement for `run` is created.
// Everything else, such as strengths, applies at once.
inline Settings Keeping(Settings run, const Settings& applied) noexcept
{
    run.stage = applied.stage; run.scale = applied.scale; run.passes = applied.passes;
    run.preset = applied.preset; run.model = applied.model;
    run.laterCustom = applied.laterCustom; run.laterModel = applied.laterModel;
    run.laterIntensity = applied.laterIntensity; run.laterStructure = applied.laterStructure;
    run.laterTone = applied.laterTone; run.laterCharacter = applied.laterCharacter;
    return run;
}

// The network's extent for a stage extent: Resolution Scale, 25 % at the
// least (also for 0 %) and the D3D12 texture limit at the most.
inline bool ProcessingExtent(uint32_t width, uint32_t height, float scale,
    uint32_t& outWidth, uint32_t& outHeight) noexcept
{
    if (!width || !height || width > kMaxExtent || height > kMaxExtent) return false;
    const double factor = static_cast<double>(Clamped(scale, kMinScale, kMaxScale, 100.0f)) / 100.0;
    const auto axis = [factor](uint32_t value) noexcept {
        return static_cast<uint32_t>(std::clamp<double>(std::round(value * factor), 1.0, kMaxExtent));
    };
    outWidth = axis(width);
    outHeight = axis(height);
    return true;
}

// The encoding of the stage's color. Auto treats native DLSS SR/DLAA/RR input
// and output as linear BT.709; the Present stage passes the swapchain's
// encoding (PresentEncoding) as `automatic`.
inline Encoding ResolvedEncoding(const Settings& settings, Encoding automatic = Encoding::eLinear) noexcept
{
    const auto encoding = static_cast<Encoding>(settings.encoding);
    return encoding == Encoding::eAuto ? automatic : encoding;
}

// Encoding Auto at the Present stage, from the swapchain buffer's DXGI format
// and color space: RGBA16F is scRGB, RGB10A2 with the ST.2084/BT.2020 color
// space HDR10, everything else (8-bit, SDR RGB10A2) sRGB.
inline constexpr uint32_t kFormatRgba16Float = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT
inline constexpr uint32_t kFormatRgb10A2 = 24;       // DXGI_FORMAT_R10G10B10A2_UNORM
inline constexpr uint32_t kColorSpaceHdr10 = 12;     // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
inline Encoding PresentEncoding(uint32_t format, uint32_t colorSpace) noexcept
{
    if (format == kFormatRgba16Float) return Encoding::eScrgb;
    if (format == kFormatRgb10A2 && colorSpace == kColorSpaceHdr10) return Encoding::eHdr10;
    return Encoding::eSrgb;
}

// Reference white of the HDR-to-SDR bridge, in nits. Auto: 100 for linear and
// sRGB color, the BT.2408 reference white (203) for HDR10 and scRGB.
inline float DiffuseWhite(const Settings& settings, Encoding automatic = Encoding::eLinear) noexcept
{
    const Encoding resolved = ResolvedEncoding(settings, automatic);
    if (settings.encoding != static_cast<uint32_t>(Encoding::eAuto) && settings.diffuseWhiteOverride)
        return Clamped(settings.diffuseWhite, kMinDiffuseWhite, kMaxDiffuseWhite, 100.0f);
    return resolved == Encoding::eLinear || resolved == Encoding::eSrgb ? 100.0f : 203.0f;
}

// The stage's linear color is unbounded (HDR) for the HDR10 and scRGB
// encodings, and for linear color of a DLSS feature created with IsHDR.
inline bool UnboundedColor(const Settings& settings, Encoding automatic, int32_t createFlags) noexcept
{
    const Encoding encoding = ResolvedEncoding(settings, automatic);
    // 1 << 0: NVSDK_NGX_DLSS_Feature_Flags_IsHDR (kHdrFlag).
    return encoding == Encoding::eHdr10 || encoding == Encoding::eScrgb || encoding == Encoding::eScrgbNonlinear
        || (encoding == Encoding::eLinear && (createFlags & (1 << 0)));
}

// Multiplier from decoded linear color to bridge units (1 = diffuse white):
// linear and sRGB color has white at 1 = 100 nits, scRGB (and decoded HDR10)
// 1 = 80 nits.
inline float WhiteScale(const Settings& settings, Encoding automatic = Encoding::eLinear) noexcept
{
    const Encoding resolved = ResolvedEncoding(settings, automatic);
    const float unitNits = resolved == Encoding::eLinear || resolved == Encoding::eSrgb ? 100.0f : 80.0f;
    return unitNits / DiffuseWhite(settings, automatic);
}

// UI Correction Auto turns DLSSNR.UICorrection on at the Present stage, whose
// image contains the game's UI. The 310.9.16 and 310.8 providers accept it
// without UI inputs (tests/dlss5_provider_harness --present --ui-correction 2).
inline constexpr bool kPresentAutoUiCorrection = true;

// DLSSNR.UICorrection: Off leaves it off, On enables it. Auto leaves it off
// for native reconstruction input/output (no UI in them) and follows
// kPresentAutoUiCorrection at Present.
inline int UiCorrectionValue(const Settings& settings, Stage stage = Stage::eUpscaled) noexcept
{
    if (settings.uiCorrection == static_cast<uint32_t>(UiCorrection::eAuto))
        return stage == Stage::ePresent && kPresentAutoUiCorrection ? 1 : 0;
    return settings.uiCorrection == static_cast<uint32_t>(UiCorrection::eOn) ? 1 : 0;
}

// Use DLSS resources at the Present stage: the depth and motion vectors of
// the latest DLSS SR/RR evaluation (`captured`), or neutral inputs (zero
// motion, constant depth). Required processes nothing without them.
enum class PresentInputs : uint32_t { eNeutral, eCaptured, eSkip };
inline PresentInputs ChoosePresentInputs(const Settings& settings, bool captured) noexcept
{
    switch (static_cast<Resources>(settings.resources))
    {
    case Resources::eNever: return PresentInputs::eNeutral;
    case Resources::eRequired: return captured ? PresentInputs::eCaptured : PresentInputs::eSkip;
    default: return captured ? PresentInputs::eCaptured : PresentInputs::eNeutral;
    }
}

// A capture belongs to the frame being presented or the one before it
// (`presents` counts Present-stage presents; the capture saw `at`).
inline bool CaptureCurrent(uint64_t presents, uint64_t at) noexcept
{
    return at && presents >= at && presents - at <= 1;
}

// Block Hudless on a copy of the game's Streamline tags: every tag of `type`
// (kBufferTypeHUDLessColor) loses its resource, which untags it. Returns the
// number of tags changed.
template <class Tag>
uint32_t WithholdTags(Tag* tags, uint32_t count, uint32_t type) noexcept
{
    uint32_t withheld = 0;
    for (uint32_t index = 0; tags && index < count; ++index)
    {
        if (static_cast<uint32_t>(tags[index].type) != type || !tags[index].resource) continue;
        tags[index].resource = nullptr;
        ++withheld;
    }
    return withheld;
}

// What the menu shows. Values are part of the status protocol (dlss5State).
enum class State : uint32_t
{
    // Off in the menu.
    eOff = 0,
    // On; no DLSS Super Resolution, DLAA or Ray Reconstruction evaluated yet.
    eWaitingForDlss = 1,
    eRunning = 2,
    // NGX found no DLSSNR provider (nvngx_dlssnr.dll).
    eNotInstalled = 3,
    // The provider refused this GPU or driver (its architecture check, or NGX's).
    eUnsupported = 4,
    // NGX asks for a newer driver.
    eDriverUpdate = 5,
    // CreateFeature failed for another reason (dlss5Result has the code).
    eCreateFailed = 6,
    // EvaluateFeature failed (dlss5Result has the code); retried after a settings change.
    eEvaluateFailed = 7,
    // Another component created DLSSNR in this process (a ReShade add-on or an
    // OptiScaler fork); ours stays off so the two never run together.
    eOtherMod = 8,
    // The game's DLSS evaluation has no usable output, depth or motion vectors.
    eMissingInputs = 9,
    // The selected Hook Method is not available in this build.
    eStageUnavailable = 10,
};

// NVSDK_NGX_Result values (nvsdk_ngx_defs.h) a failed Create maps to.
inline constexpr uint32_t kResultFeatureNotSupported = 0xBAD00001u;
inline constexpr uint32_t kResultPlatformError = 0xBAD00002u;
inline constexpr uint32_t kResultFeatureNotFound = 0xBAD00004u;
inline constexpr uint32_t kResultOutOfDate = 0xBAD0000Cu;
inline constexpr uint32_t kResultDenied = 0xBAD00011u;

constexpr State CreateFailureState(uint32_t result) noexcept
{
    switch (result)
    {
    case kResultFeatureNotFound: return State::eNotInstalled;
    case kResultFeatureNotSupported: case kResultPlatformError: case kResultDenied: case 0xBAD0000Bu: return State::eUnsupported;
    case kResultOutOfDate: return State::eDriverUpdate;
    default: return State::eCreateFailed;
    }
}

namespace d3d11
{
// dx11nr.1: why the DirectX 11 bridge (dlss5_d3d11.h) leaves the game's evaluations as they are. Values are part
// of the status protocol (dlss5D3D11Refusal).
enum class Refusal : uint32_t
{
    eNone = 0,
    // DLSS is evaluated on a deferred context; the bridge records on the immediate context only.
    eDeferredContext = 1,
    // No ID3D11Device5/ID3D11DeviceContext4 (D3D11.4 shared fences).
    eNoSharedFence = 2,
    // The bridge's DirectX 12 device (or NGX's D3D12 entries for it) is unavailable, or on another adapter.
    eDevice = 3,
    // A shared texture or the shared fence could not be created or opened.
    eSharing = 4,
    // The game's DLSS feature was created before the D3D11 route was hooked: DLSS 5 does not attach to it.
    eUntracked = 5,
};
// A refusal that keeps DLSS 5 off the game (the state shows Unsupported while DLSS 5 is on).
constexpr bool Blocking(Refusal refusal) noexcept { return refusal != Refusal::eNone && refusal != Refusal::eUntracked; }
}

// The game's DLSS evaluation, as read from its parameter object and the
// feature's Create keys (dlss_sr::CreateKeys).
struct Source
{
    // Output and Color: the reconstruction's output and input color.
    uintptr_t output = 0, color = 0, depth = 0, motion = 0, exposure = 0;
    // DLSS.Output.Subrect.Base.*, DLSS.Input.Color/Depth/MV.Subrect.Base.*.
    uint32_t outputX = 0, outputY = 0, colorX = 0, colorY = 0, depthX = 0, depthY = 0, motionX = 0, motionY = 0;
    // DLSS.Render.Subrect.Dimensions.* (0 when absent: the Create render size).
    uint32_t renderWidth = 0, renderHeight = 0;
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    // DLSS.Pre.Exposure (0 when absent).
    float preExposure = 0.0f;
    bool reset = false;
};

struct Rect
{
    int32_t x = 0, y = 0, width = 0, height = 0;
    friend bool operator==(const Rect&, const Rect&) = default;
};

// The stage color DLSSNR processes, built from a Source.
struct Frame
{
    // color: the stage's color (read); output: where the result goes. At the
    // Upscaled stage both are the reconstruction output (in place). At the
    // Render stage the result goes to a substitute input of ours.
    uintptr_t color = 0, output = 0, depth = 0, motion = 0, exposure = 0;
    Rect colorRect{}, outputRect{}, depthRect{}, motionRect{};
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    float preExposure = 0.0f;
    int32_t depthInverted = 0;
    int32_t reset = 0;
    // The stage extent (before Resolution Scale).
    uint32_t width = 0, height = 0;
};

inline constexpr int32_t kHdrFlag = 1 << 0;           // NVSDK_NGX_DLSS_Feature_Flags_IsHDR
inline constexpr int32_t kDepthInvertedFlag = 1 << 3; // NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
inline constexpr int32_t kMotionLowResFlag = 1 << 1;  // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes

constexpr bool FitsInt(uint32_t value) noexcept { return value <= 0x7FFFFFFFu; }

// Why the game's DLSS evaluation cannot feed DLSSNR, for the log
// ("DLSS 5 inputs unavailable: ..."). `key` names the NGX key concerned.
enum class InputGap : uint32_t
{
    eNone = 0,
    // The DLSS Create had no DLSS.Feature.Create.Flags, or no Width, Height,
    // OutWidth or OutHeight (or a zero one).
    eCreateKeys,
    // The evaluation has no Color, Output, Depth or MotionVectors resource.
    eResource,
    // A subrect key holds a negative value.
    eSubrectKey,
    // DLSS.Render.Subrect.Dimensions beyond the Create Width/Height. DLSS SR
    // 310.9.16 reads such a subrect whole (it does not clamp to the Create
    // size), so the region it used is not the Create-size one this stage would
    // assume.
    eRenderSize,
    // A size or offset beyond INT_MAX.
    eRange,
    // A non-finite motion-vector scale.
    eMotionScale,
};
struct InputsWhy
{
    InputGap gap = InputGap::eNone;
    const char* key = "";
};

// False when the evaluation cannot feed DLSSNR (missing resources or sizes),
// with the reason in `why`. Motion vectors at render resolution unless the
// DLSS feature was created without MVLowRes (then at output resolution, like
// DLSS reads them).
inline bool Build(const Source& source, const dlss_sr::CreateKeys& create, Stage stage,
    bool resetHistory, Frame& frame, InputsWhy* why = nullptr) noexcept
{
    frame = {};
    const auto refuse = [why](InputGap gap, const char* key) noexcept {
        if (why) *why = {gap, key};
        return false;
    };
    const bool render = stage == Stage::eRender;
    const uintptr_t color = render ? source.color : source.output;
    if (!color) return refuse(InputGap::eResource, render ? "Color" : "Output");
    if (!source.depth) return refuse(InputGap::eResource, "Depth");
    if (!source.motion) return refuse(InputGap::eResource, "MotionVectors");
    if (!create.Complete()) return refuse(InputGap::eCreateKeys, "Width/Height/OutWidth/OutHeight");
    const uint32_t outWidth = create.outWidth, outHeight = create.outHeight;
    uint32_t renderWidth = source.renderWidth ? source.renderWidth : create.width;
    uint32_t renderHeight = source.renderHeight ? source.renderHeight : create.height;
    if (!outWidth || !outHeight || !renderWidth || !renderHeight)
        return refuse(InputGap::eCreateKeys, "Width/Height/OutWidth/OutHeight");
    // Dynamic resolution never renders beyond the Create size, but a game may round its render size up where DLSS SR's
    // Create rounded down; DLSS SR reads such a subrect whole, so one pixel over is taken as it is (ValidateResources
    // still bounds it by the textures). More than that (e.g. empty tag extents passing the whole resource) is refused.
    if (renderWidth>create.width+1 || renderHeight>create.height+1)
        return refuse(InputGap::eRenderSize, "DLSS.Render.Subrect.Dimensions");
    const bool lowResMotion = (create.createFlags & kMotionLowResFlag) != 0;
    const uint32_t motionWidth = lowResMotion ? renderWidth : outWidth;
    const uint32_t motionHeight = lowResMotion ? renderHeight : outHeight;
    for (const uint32_t value : {outWidth, outHeight, renderWidth, renderHeight, source.outputX, source.outputY,
             source.colorX, source.colorY, source.depthX, source.depthY, source.motionX, source.motionY})
        if (!FitsInt(value)) return refuse(InputGap::eRange, "subrect");
    if (!std::isfinite(source.motionScaleX) || !std::isfinite(source.motionScaleY))
        return refuse(InputGap::eMotionScale, "MV.Scale");
    frame.color = frame.output = color;
    frame.depth = source.depth;
    frame.motion = source.motion;
    frame.exposure = source.exposure;
    frame.preExposure = std::isfinite(source.preExposure) && source.preExposure > 0.0f ? source.preExposure : 0.0f;
    frame.width = render ? renderWidth : outWidth;
    frame.height = render ? renderHeight : outHeight;
    frame.colorRect = frame.outputRect = {static_cast<int32_t>(render ? source.colorX : source.outputX),
        static_cast<int32_t>(render ? source.colorY : source.outputY),
        static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)};
    frame.depthRect = {static_cast<int32_t>(source.depthX), static_cast<int32_t>(source.depthY),
        static_cast<int32_t>(renderWidth), static_cast<int32_t>(renderHeight)};
    frame.motionRect = {static_cast<int32_t>(source.motionX), static_cast<int32_t>(source.motionY),
        static_cast<int32_t>(motionWidth), static_cast<int32_t>(motionHeight)};
    // DLSS's own scale (game units -> pixels of the motion-vector rect); the
    // provider maps that rect onto the color rect itself.
    frame.motionScaleX = source.motionScaleX;
    frame.motionScaleY = source.motionScaleY;
    frame.depthInverted = (create.createFlags & kDepthInvertedFlag) ? 1 : 0;
    frame.reset = source.reset || resetHistory ? 1 : 0;
    return true;
}


// Tracks recorded work until its exact queue submission and fence completion.
// A discarded/unsubmitted list never becomes safe merely because time passed.
struct Flight {
    uintptr_t recording = 0;
    uint64_t submitted = 0;
    bool failed = false;
    bool CanRecord(uintptr_t command) const noexcept {
        return command && !failed && (!recording || recording==command) && submitted<UINT64_MAX-1;
    }
    bool Record(uintptr_t command) noexcept {
        if (!CanRecord(command)) return false;
        recording=command; return true;
    }
    bool Matches(uintptr_t command) const noexcept { return recording && recording==command; }
    uint64_t Next() const noexcept { return submitted<UINT64_MAX-1 ? submitted+1 : 0; }
    bool Signaled(uintptr_t command,uint64_t value,bool success) noexcept {
        if (!Matches(command) || value!=Next() || !value || !success) { failed=true; return false; }
        submitted=value; recording=0; return true;
    }
    bool Drained(uint64_t completed) const noexcept {
        return !recording && !failed && completed!=UINT64_MAX && completed>=submitted;
    }
};
inline bool RectFits(const Rect& rect,uint64_t width,uint32_t height) noexcept {
    return rect.x>=0 && rect.y>=0 && rect.width>0 && rect.height>0
        && uint64_t(rect.x)+uint32_t(rect.width)<=width
        && uint64_t(rect.y)+uint32_t(rect.height)<=height;
}
}
