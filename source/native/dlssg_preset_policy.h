#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dlssg_preset
{
// Public NVAPI ABI: NVDRS_SETTING_V1 and the DLSS-FG preset key. These are
// independent of DLSS Super Resolution and Ray Reconstruction preset keys.
inline constexpr uint32_t kSettingId = 0x10E41DF1u;
inline constexpr uint32_t kGameOrDriver = 0;
inline constexpr uint32_t kPresetA = 1;
inline constexpr uint32_t kPresetB = 2;
// DLSS-G 310.9.16 and newer: the NVIDIA app's Frame Generation "Performance mode", which replaces the
// UI/ray-traced optical-flow model with a lighter algorithm. Earlier providers ignore the value.
inline constexpr uint32_t kPresetC = 3;
inline constexpr int32_t kOk = 0;
inline constexpr int32_t kSettingNotFound = -160;

#pragma pack(push, 4)
union SettingValue
{
    uint32_t dword;
    struct { uint32_t length; uint8_t data[4096]; } binary;
    uint16_t text[2048];
    uint64_t qword;
};
struct Setting
{
    uint32_t version;
    uint16_t name[2048];
    uint32_t id;
    uint32_t type;
    uint32_t location;
    uint32_t currentPredefined;
    uint32_t predefinedValid;
    SettingValue predefined;
    SettingValue current;
};
#pragma pack(pop)
static_assert(sizeof(Setting) == 12320);
static_assert(offsetof(Setting, current) == 8220);
inline constexpr uint32_t kSettingVersion = sizeof(Setting) | (1u << 16);

// A and B are the original Frame Generation models; C exists only in providers that implement it
// (dlssg_provider_policy::ImplementsPresetC). Other reserved driver letters are not model-support proof.
constexpr bool IsOverride(uint32_t preset) noexcept
{
    return preset == kPresetA || preset == kPresetB || preset == kPresetC;
}

constexpr bool IsValidSelection(uint32_t preset) noexcept
{
    return preset == kGameOrDriver || IsOverride(preset);
}

inline bool CompatibleRead(uint32_t id, int32_t result, const Setting& setting) noexcept
{
    return id == kSettingId && setting.version == kSettingVersion
        && (result == kSettingNotFound
            || (result == kOk && setting.id == id && setting.type == 0));
}

// The caller establishes provider identity and writable buffer ownership, and
// passes C only to a provider that implements it. Driver errors and unknown
// versions/types retain the original result.
inline bool OverrideRead(uint32_t id, int32_t result, Setting& setting, uint32_t preset) noexcept
{
    if (!IsOverride(preset) || !CompatibleRead(id, result, setting))
        return false;
    if (result == kSettingNotFound)
    {
        setting = {};
        setting.version = kSettingVersion;
        setting.id = id;
        constexpr char name[] = "Override DLSS-FG preset";
        for (size_t i = 0; i < sizeof(name); ++i)
            setting.name[i] = static_cast<uint16_t>(name[i]);
    }
    setting.location = 0; // NVDRS_CURRENT_PROFILE, for this returned value only.
    setting.currentPredefined = 0;
    std::memset(&setting.current, 0, sizeof(setting.current));
    setting.current.dword = preset;
    return true;
}

// The provider whose preset reads the status reports. RTX 40/50:
// the provider the active Create selected. The RTX 30/20 translated route
// selects none there; its reader hook records reads against its own prepared
// provider (`translated`), so that provider is the one to report.
constexpr uintptr_t StatusProvider(uintptr_t selected, bool translatedRoute, uintptr_t translated) noexcept
{
    return translatedRoute && translated ? translated : selected;
}
}
