#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace dlssg_provider_policy
{
struct VersionTriplet
{
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
};

// Only provider builds which passed the complete immutable layout and payload
// contract belong here. The fourth file-version component is intentionally not
// part of eligibility because it did not change the validated provider layout.
inline constexpr std::array<VersionTriplet, 16> kSupportedVersions{{
    {310, 1, 0},
    {310, 2, 0},
    {310, 2, 1},
    {310, 3, 0},
    {310, 4, 0},
    {310, 5, 0},
    {310, 5, 2},
    {310, 5, 3},
    {310, 6, 0},
    {310, 7, 0},
    {310, 7, 128},
    {310, 7, 129},
    {310, 8, 0},
    {310, 9, 0},
    {310, 9, 1},
    // NVIDIA app beta: Frame Generation Preset C.
    {310, 9, 16},
}};

inline constexpr char kD3d12ImplementationExport[] =
    "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl";
inline constexpr char kDirectSrImplementationExport[] =
    "NVSDK_NGX_DirectSR_Create";

// DLSS-G providers expose the device-parameter population entry. DLSS 5
// Super Resolution providers expose DirectSR instead and must never enter the
// frame-generation mutation path, even when both families share the same
// embedded 310.x version and generic NGX exports.
constexpr bool HasDlssgExportIdentity(bool populateDeviceParameters,
    bool directSr) noexcept
{
    return populateDeviceParameters && !directSr;
}

static_assert(HasDlssgExportIdentity(true, false));
static_assert(!HasDlssgExportIdentity(false, false));
static_assert(!HasDlssgExportIdentity(true, true));

constexpr bool IsSupportedVersion(VersionTriplet version) noexcept
{
    for (const auto supported : kSupportedVersions)
    {
        if (version.major == supported.major
            && version.minor == supported.minor
            && version.build == supported.build)
        {
            return true;
        }
    }
    return false;
}

// Builds whose kernels the RTX 30 port has no SM86 program for. Every RTX 30 admission (OTA selection,
// preparation, runtime routing) rejects them before any provider mutation. Empty: 310.9.16
// (Preset C, BlendCandidatesFGLite) has its RTX 30 profile; its new kernels compile for SM86 unchanged.
inline constexpr std::array<VersionTriplet, 0> kAdaOnlyVersions{};

constexpr bool IsAdaOnlyVersion(VersionTriplet version) noexcept
{
    for (const auto adaOnly : kAdaOnlyVersions)
    {
        if (version.major == adaOnly.major && version.minor == adaOnly.minor
            && version.build == adaOnly.build)
            return true;
    }
    return false;
}

constexpr bool IsSupportedAmpereVersion(VersionTriplet version) noexcept
{
    return IsSupportedVersion(version) && !IsAdaOnlyVersion(version);
}

// The provider maps driver preset value 3 to Preset C from 310.9.16; earlier builds ignore the value.
constexpr bool ImplementsPresetC(VersionTriplet version) noexcept
{
    return version.major == 310 && version.minor == 9 && version.build == 16;
}

static_assert(IsSupportedVersion({310, 1, 0}));
static_assert(IsSupportedVersion({310, 2, 0}));
static_assert(IsSupportedVersion({310, 2, 1}));
static_assert(IsSupportedVersion({310, 3, 0}));
static_assert(IsSupportedVersion({310, 4, 0}));
static_assert(IsSupportedVersion({310, 5, 0}));
static_assert(IsSupportedVersion({310, 5, 2}));
static_assert(IsSupportedVersion({310, 5, 3}));
static_assert(IsSupportedVersion({310, 6, 0}));
static_assert(IsSupportedVersion({310, 7, 0}));
static_assert(IsSupportedVersion({310, 7, 128}));
static_assert(IsSupportedVersion({310, 7, 129}));
static_assert(IsSupportedVersion({310, 8, 0}));
static_assert(IsSupportedVersion({310, 9, 0}));
static_assert(IsSupportedVersion({310, 9, 1}));
static_assert(!IsSupportedVersion({310, 7, 1}));
static_assert(!IsSupportedVersion({310, 5, 1}));
static_assert(!IsSupportedVersion({310, 6, 1}));
static_assert(!IsSupportedVersion({310, 8, 1}));
static_assert(!IsSupportedVersion({310, 9, 2}));
static_assert(IsSupportedVersion({310, 9, 16}));
static_assert(!IsSupportedVersion({310, 9, 15}) && !IsSupportedVersion({310, 9, 17}));
static_assert(IsSupportedAmpereVersion({310, 9, 1}) && IsSupportedAmpereVersion({310, 9, 16}));
static_assert(ImplementsPresetC({310, 9, 16}) && !ImplementsPresetC({310, 9, 1})
    && !ImplementsPresetC({310, 9, 0}));
static_assert([] {
    for (const auto version : kAdaOnlyVersions)
        if (!IsSupportedVersion(version)) return false;
    return true;
}(), "Ada-only builds must be eligible on Ada");

bool ReadProviderVersion(
    const wchar_t* path, VersionTriplet& version) noexcept;
bool SupportedProviderVersionMatches(const wchar_t* path) noexcept;
bool IsDlssgImplementationModule(HMODULE module) noexcept;
bool IsSupportedProvider(HMODULE module, const wchar_t* path) noexcept;
// Reuses embedded-version eligibility only while owning a process-lifetime
// reference to that loaded module. Export identity is still checked on every
// call. Retention prevents unloading and base-address reuse across generations.
bool IsSupportedRetainedProvider(HMODULE module) noexcept;
// RTX 30 admissions: the shared rules without the Ada-only builds.
bool IsSupportedAmpereProvider(HMODULE module, const wchar_t* path) noexcept;
bool IsSupportedRetainedAmpereProvider(HMODULE module) noexcept;
// A retained eligible provider whose embedded version implements Preset C.
bool ProviderImplementsPresetC(HMODULE module) noexcept;
}
