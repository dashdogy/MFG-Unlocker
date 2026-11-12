#include "build_variant.h"
#include "unified_control_paths.h"
#include "status_transport.h"
#include "ampere_policy.h"
#include "nvidia_mfg_policy.h"
#include "ui_dynamic_mfg.h"
#include "universal_route_policy.h"
#include "reshade_frontend.h"

#include "backend_bridge.h"
#include "ampere_diagnostics.h"
#include "reflex_control.h"
#include "latency_probe.h"
#include "vsync_control.h"
#include "dlss_sr_policy.h"
#include "dlss5_settings.h"
#include "dlss5_auto.h"
#include "dlss5_presets.h"
#include "physical_key.h"
#include "optiscaler_plugin.h"

#include <imgui.h>
#include <imgui_internal.h>
#include "ui_host.h"
#include "standalone_ui.h"
#include "localization.h"
#include "single_module.h"
#include "witcher_dots/witcher_dots.h"
#include "rtx_hair/universal_hair.h"
#include "gpu_name_override.h"
#include "runtime_log_setting.h"

#include <Psapi.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include "ui_status_json.h"
#include "xess_ui_state.h"
#include "xess/runtime_search.h"
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
using l10n::T;
constexpr const char* kConfigSection = MFG_PRODUCT;

std::atomic<uint32_t> gGpuFamily{0};
bool UiAmpere() noexcept { return (gGpuFamily.load(std::memory_order_acquire) == 2u || gGpuFamily.load(std::memory_order_acquire) == 4u); }

std::atomic<bool> gInitialized{false};
HMODULE gSelf = nullptr;
bool gDefaultDockInitialized = false;
bool gOverlayVisitedThisFrame = false;
bool gOverlayDockedThisFrame = false;
enum class BackendKind : uint32_t
{
    eNone = 0,
    eNativeFile = 2,
};

HMODULE gBackend = nullptr;
MfgUnlockBackendInterface gBackendInterface{};
std::atomic<bool> gBackendConnected{false};
std::atomic<BackendKind> gBackendKind{BackendKind::eNone};
SRWLOCK gBackendConnectionLock = SRWLOCK_INIT;
std::wstring gNativeConfigPath;
std::wstring gNativeStatusPath;
// Display-only state. Control writes always obtain their own current snapshot.
MfgUnlockReShadeSnapshot gLastNativeDisplay{};
std::wstring gLastNativeDisplayPath;
bool gHaveNativeDisplay = false;
using MfgUnlockSampleFrameTelemetryFn = BOOL (WINAPI*)();
MfgUnlockSampleFrameTelemetryFn gSampleFrameTelemetry = nullptr;

bool ConnectBackend() noexcept;

int gMultiplier = 2;
bool gFollowGameMode = true;
bool gDynamicMode = false;
int gDynamicTargetFrameRate = 0;
bool gDynamicLockToRefreshRate = true;
int gDynamicCustomTargetFrameRate = 60;
int gDlssgPreset = 0; // latency.1: game / driver by default
// DLSS Super Resolution preset: 0 game/driver, 10..13 J..M.
uint32_t gDlssSrPreset = 0;
// DLSS 5 tab: the edited settings; sliders save while dragging at
// most every 150 ms and once on release.
dlss5::Settings gDlss5{};
bool gDlss5SaveFailed=false,gDlss5Dirty=false;
// nrscale.1: a Resolution Scale or Pass Count edit waiting for its slider's release (Dlss5ReleaseSave).
bool gDlss5Held=false;
uint64_t gDlss5LastSave=0;
bool gSrPresetSaveFailed = false;
// DLSS Ray Reconstruction preset: 0 game/driver, 4..6 D..F.
uint32_t gDlssRrPreset = 0;
bool gRrPresetSaveFailed = false;
int gVsyncMode = 0;
int gReflexFrameLimitFps = 0;
int gReflexCustomLimitFps = 120;
bool gPresentationSaveFailed = false;
bool gPresetSaveFailed = false;
bool gDynamicExperimental56 = false;
bool gGeneratedOnlyDebug = false;
bool gLastApplyAttempted = false;
bool gLastApplyAccepted = false;
bool gLastNativeConfigPersisted = true;


BOOL BackendGetSnapshot(MfgUnlockReShadeSnapshot* snapshot) noexcept
{
    if (!ConnectBackend() || !gBackendInterface.getSnapshot
        || !gBackendInterface.getSnapshot(snapshot))
    {
        return FALSE;
    }
    return TRUE;
}

BOOL BackendApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug) noexcept
{
    return ConnectBackend() && gBackendInterface.applyControl
        ? gBackendInterface.applyControl(multiplier, dynamicMode,
            dynamicTargetFrameRate, dynamicExperimental56,
            generatedOnlyDebug) : FALSE;
}

bool BackendUsesNativeFile() noexcept
{
    return gBackendKind.load(std::memory_order_acquire)
        == BackendKind::eNativeFile;
}

bool FindJsonValue(const std::string& content, const char* name,
    size_t& offset) noexcept
{
    const std::string key = std::string("\"") + name + "\"";
    const size_t keyOffset = content.find(key);
    if (keyOffset == std::string::npos)
        return false;
    const size_t colon = content.find(':', keyOffset + key.size());
    if (colon == std::string::npos)
        return false;
    offset = content.find_first_not_of(" \t\r\n", colon + 1);
    return offset != std::string::npos;
}

bool ParseJsonBool(const std::string& content, const char* name,
    BOOL& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    if (content.compare(offset, 4, "true") == 0
        && ui_status_json::ValueEnd(content, offset+4))
    {
        value = TRUE;
        return true;
    }
    if (content.compare(offset, 5, "false") == 0
        && ui_status_json::ValueEnd(content, offset+5))
    {
        value = FALSE;
        return true;
    }
    return false;
}

template <typename T>
bool ParseJsonInteger(const std::string& content, const char* name,
    T& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset))
        return false;
    T parsed{};
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data())))
        return false;
    value = parsed;
    return true;
}

bool ParseJsonFloat(const std::string& content, const char* name, float& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)) return false;
    float parsed = 0.0f;
    const auto result = std::from_chars(content.data()+offset,
        content.data()+content.size(), parsed);
    if (result.ec != std::errc{} || !std::isfinite(parsed)
        || !ui_status_json::ValueEnd(content,
            static_cast<size_t>(result.ptr-content.data()))) return false;
    value = parsed;
    return true;
}

bool ParseJsonString(const std::string& content, const char* name,
    std::string& value) noexcept
{
    size_t offset = 0;
    if (!FindJsonValue(content, name, offset)
        || content[offset] != '"')
    {
        return false;
    }
    value.clear();
    for (size_t index = offset + 1; index < content.size(); ++index)
    {
        const char current = content[index];
        if (current == '"')
            return true;
        if (current != '\\')
        {
            value.push_back(current);
            continue;
        }
        if (++index >= content.size())
            return false;
        switch (content[index])
        {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return false;
        }
    }
    return false;
}

bool ReadTextFile(const std::wstring& path, std::string& content) noexcept
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size)
        && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (!validSize)
    {
        CloseHandle(file);
        return false;
    }
    content.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, content.data(),
        static_cast<DWORD>(content.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read || bytesRead != content.size())
    {
        content.clear();
        return false;
    }
    return true;
}

bool ResolveNativeFilePaths() noexcept
{
    std::wstring executablePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size())
        return false;
    executablePath.resize(length);
    const std::filesystem::path directory =
        std::filesystem::path(executablePath).parent_path();
    gNativeConfigPath = unified_control_paths::Config(directory.wstring());
    gNativeStatusPath = unified_control_paths::Status(gNativeConfigPath, directory.wstring());
    return true;
}

BOOL ParseNativeControlSnapshot(const std::string& control,
    MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot))
        return FALSE;
    MfgUnlockReShadeSnapshot output{};
    output.followGameMode = FALSE;
    output.intervalLoggingEnabled = TRUE;
    output.frontendClients = 1;
    strcpy_s(output.patchRoute, "pending");

    if (!ui_status_json::CompleteObject(control)
        || !ParseJsonInteger(control, "multiplier", output.desiredMultiplier)
        || output.desiredMultiplier < 1 || output.desiredMultiplier > 6) return FALSE;
    {
        size_t offset = 0;
        if (FindJsonValue(control, "followGame", offset)
            && !ParseJsonBool(control, "followGame", output.followGameMode)) return FALSE;
        if (FindJsonValue(control, "dynamicTargetFrameRate", offset)
            && (!ParseJsonInteger(control, "dynamicTargetFrameRate", output.dynamicTargetFrameRate)
                || output.dynamicTargetFrameRate > 1000)) return FALSE;
        if (FindJsonValue(control, "dlssgPreset", offset)
            && (!ParseJsonInteger(control, "dlssgPreset", output.dlssgPresetRequested)
                || output.dlssgPresetRequested > 3)) return FALSE;
        // latency.1: as the backend (patcher.cpp TryParseControl), a B saved without "dlssgDefaults" is the old default.
        uint32_t dlssgDefaults = 0;
        if (FindJsonValue(control, "dlssgDefaults", offset)
            && !ParseJsonInteger(control, "dlssgDefaults", dlssgDefaults)) return FALSE;
        if (dlssgDefaults == 0 && output.dlssgPresetRequested == 2) output.dlssgPresetRequested = 0;
        // An SR preset this build does not offer reads as game/driver; it
        // never invalidates the other saved options.
        if (FindJsonValue(control, "dlssSrPreset", offset)
            && (!ParseJsonInteger(control, "dlssSrPreset", output.dlssSr.presetRequested)
                || !dlss_sr::IsValidSelection(output.dlssSr.presetRequested)))
            output.dlssSr.presetRequested = dlss_sr::kGameOrDriver;
        if (FindJsonValue(control, "dlssRrPreset", offset)
            && (!ParseJsonInteger(control, "dlssRrPreset", output.dlssRr.presetRequested)
                || !dlss_sr::IsValidRrSelection(output.dlssRr.presetRequested)))
            output.dlssRr.presetRequested = dlss_sr::kGameOrDriver;
        if (!dlss5::settings_json::Parse(control, output.dlss5)) return FALSE;
        if (FindJsonValue(control, "vsyncMode", offset)
            && (!ParseJsonInteger(control, "vsyncMode", output.vsyncMode)
                || output.vsyncMode > 2)) return FALSE;
        if (FindJsonValue(control, "reflexFrameLimitFps", offset)
            && (!ParseJsonInteger(control, "reflexFrameLimitFps", output.reflexFrameLimitFps)
                || output.reflexFrameLimitFps > 1000)) return FALSE;
        // Legacy Dynamic 5X/6X preferences do not control capacity.
        output.dynamicExperimental56 = FALSE;
        if (FindJsonValue(control, "generatedOnlyDebug", offset)
            && !ParseJsonBool(control, "generatedOnlyDebug", output.generatedOnlyDebug)) return FALSE;
        BOOL legacyValue = FALSE;
        for (const char* name : {"dynamicExperimental56", "selectiveOtaDlssgWrapper"})
            if (FindJsonValue(control, name, offset)
                && !ParseJsonBool(control, name, legacyValue)) return FALSE;
        if (FindJsonValue(control, "intervalLogging", offset)
            && !ParseJsonBool(control, "intervalLogging", output.intervalLoggingEnabled)) return FALSE;
        std::string controlMode;
        if (FindJsonValue(control, "mode", offset))
        {
            if (!ParseJsonString(control, "mode", controlMode)
                || (controlMode != "fixed" && controlMode != "dynamic"
                    && !(controlMode == "follow" && output.followGameMode))) return FALSE;
            output.dynamicMode = controlMode == "dynamic" ? TRUE : FALSE;
        }
        if (output.followGameMode) output.dynamicMode = FALSE;
    }

    *snapshot = output;
    return TRUE;
}

BOOL ReadNativeControlSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    std::string control;
    return ReadTextFile(gNativeConfigPath, control)
        && ParseNativeControlSnapshot(control, snapshot);
}

uint64_t NativeStatusUnixSeconds() noexcept
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks/10000000ull - 11644473600ull;
}

bool ValidNativeStatus(const std::string& status)
{
    if (!ui_status_json::CompleteObject(status)) return false;
    uint32_t version = 0, pid = 0, maximum = 0;
    uint64_t heartbeat = 0, processBirth = 0;
    const uint64_t now = NativeStatusUnixSeconds();
    if (!ParseJsonInteger(status, "version", version)
        || version != MFG_STATUS_VERSION_NUMBER
        || !ParseJsonInteger(status, "pid", pid) || pid != GetCurrentProcessId()
        || !ParseJsonInteger(status, "processBirth", processBirth)
        || !processBirth || processBirth != diagnostic_paths::ProcessBirth()
        || !ParseJsonInteger(status, "heartbeat", heartbeat)
        || heartbeat > now || now-heartbeat > 5
        || !ParseJsonInteger(status, "safeMaximumMultiplier", maximum)
        || maximum < 2 || maximum > 6u) return false;
    // These fields define availability and the menu's shape. Never silently
    // substitute defaults for a missing or ill-typed capability field.
    BOOL value = FALSE;
    for (const char* key : {"bridgeReady", "gameFrameGenerationOn",
            "appliedFrameGenerationOn",
            "activeWrapperObserved", "dynamicMfgSupportKnown", "dynamicMfgSupported"})
        if (!ParseJsonBool(status, key, value)) return false;
    return true;
}

// XeSS-FG state lives beside the shared ReShade snapshot ABI: only the
// integrated menu draws it, so the bridge contract is unchanged.
using XessUiState = xess_ui::State;
XessUiState gXessUi;
// The frame generation the game last turned on; the menu keeps its settings
// while the game's frame generation is off.
std::string gLastGameFrameGeneration = "none";

void ParseXessUiState(const std::string& status) noexcept
{
    try
    {
        gXessUi = xess_ui::Read(status, &ParseJsonBool,
            [](const std::string& text, const char* key, auto& value) {
                return ParseJsonInteger(text, key, value);
            }, &ParseJsonString);
        if (gXessUi.activeFrameGeneration == "xess" || gXessUi.activeFrameGeneration == "dlss")
            gLastGameFrameGeneration = gXessUi.activeFrameGeneration;
    }
    catch (...) {}
}

// One DLSS feature's status keys, prefix "dlssSr" or "dlssRr". Saved intent
// outranks the status' requested preset.
void ParseDlssFeature(const std::string& status, const char* prefix, bool haveSavedControl,
    MfgUnlockDlssFeature& feature) noexcept
{
    char buffer[48]{};
    const auto key = [&](const char* name) { snprintf(buffer, sizeof(buffer), "%s%s", prefix, name); return buffer; };
    if (!haveSavedControl) ParseJsonInteger(status, key("PresetRequested"), feature.presetRequested);
    ParseJsonBool(status, key("Detected"), feature.detected);
    ParseJsonBool(status, key("Running"), feature.running);
    ParseJsonInteger(status, key("LiveFeatures"), feature.liveFeatures);
    ParseJsonInteger(status, key("RenderWidth"), feature.renderWidth);
    ParseJsonInteger(status, key("RenderHeight"), feature.renderHeight);
    ParseJsonInteger(status, key("OutputWidth"), feature.outputWidth);
    ParseJsonInteger(status, key("OutputHeight"), feature.outputHeight);
    ParseJsonInteger(status, key("Quality"), feature.quality);
    ParseJsonInteger(status, key("GameHint"), feature.gameHint);
    ParseJsonInteger(status, key("ActivePreset"), feature.activePreset);
    ParseJsonInteger(status, key("LiveSwitch"), feature.liveSwitch);
    ParseJsonBool(status, key("DrsObserved"), feature.drsObserved);
    ParseJsonBool(status, key("DriverPresetValid"), feature.driverPresetValid);
    ParseJsonInteger(status, key("DriverPreset"), feature.driverPreset);
    ParseJsonBool(status, key("DrsSuppressed"), feature.drsSuppressed);
    ParseJsonBool(status, key("FixedByDriver"), feature.fixedByDriver);
    ParseJsonBool(status, key("DriverOverrideCached"), feature.driverOverrideCached);
}

BOOL WINAPI NativeFileGetSnapshot(MfgUnlockReShadeSnapshot* snapshot)
{
    if (!snapshot || snapshot->structSize < sizeof(*snapshot)) return FALSE;
    MfgUnlockReShadeSnapshot output{};
    const bool haveSavedControl = ReadNativeControlSnapshot(&output) != FALSE;
    std::string status;
    const auto statusSource=status_transport::Read(gNativeStatusPath,status,&ReadTextFile,&ValidNativeStatus);
    if(statusSource==status_transport::Source::None)return FALSE;
    static auto previousSource=status_transport::Source::None;
    if(statusSource!=previousSource){
        single_module::Log(statusSource==status_transport::Source::Fallback
            ?L"MFG_STATUS_UI fresh snapshot source=process-memory"
            :L"MFG_STATUS_UI fresh snapshot source=config-directory");
        previousSource=statusSource;
    }
    {
        // A status heartbeat can precede the latest atomic config save. Saved
        // intent remains authoritative; applied state comes only from status.
        if (!haveSavedControl) ParseJsonBool(status, "followGame", output.followGameMode);
        ParseJsonInteger(status, "version",
            output.statusProtocolVersion);
        if (!ParseJsonInteger(status, "gpuFamily", output.gpuFamily) || output.gpuFamily > 5u)
            return FALSE;
        ParseJsonInteger(status, "gpuAdapterLuid", output.gpuAdapterLuid);
        ParseJsonInteger(status, "gpuSelectionFailure", output.gpuSelectionFailure);
        ParseJsonInteger(status, "graphicsApi", output.graphicsApi);
        gGpuFamily.store(output.gpuFamily, std::memory_order_release);
        ParseXessUiState(status);
        if (UiAmpere())
        {
            if (output.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER) return FALSE;
            ParseJsonBool(status, "ampereProgramReadyMfg", output.ampereProgramReadyMfg);
            ParseJsonInteger(status, "ampereKernelImage", output.ampereKernelImage);
            ParseJsonInteger(status, "ampereNativeCacheStatus", output.ampereNativeCacheStatus);
            ParseJsonInteger(status, "ampereCertifiedMaximum", output.ampereCertifiedMaximum);
            ParseJsonInteger(status, "ampereFailure", output.ampereFailure);
            ParseJsonInteger(status, "presetQueries", output.presetQueries);
            ParseJsonInteger(status, "ampereFirstFailure", output.ampereFirstFailure);
            ParseJsonInteger(status, "ampereFirstNgxResult", output.ampereFirstNgxResult);
            ParseJsonInteger(status, "amperePrimaryFailure", output.amperePrimaryFailure);
            ParseJsonInteger(status, "amperePrimaryNgxResult", output.amperePrimaryNgxResult);
            ParseJsonInteger(status, "ampereLastFailure", output.ampereLastFailure);
            ParseJsonInteger(status, "ampereLastNgxResult", output.ampereLastNgxResult);
            ParseJsonInteger(status, "ampereCreateAttempts", output.ampereCreateAttempts);
            ParseJsonInteger(status, "ampereCreateBlockedBeforeProvider", output.ampereCreateBlockedBeforeProvider);
            ParseJsonInteger(status, "ampereEvaluateAttempts", output.ampereEvaluateAttempts);
            ParseJsonInteger(status, "amperePreparationStage", output.amperePreparationStage);
            ParseJsonInteger(status, "ampereStartupFailureMask", output.ampereStartupFailureMask);
            ParseJsonBool(status, "ampereAdmissionUnavailable", output.ampereAdmissionUnavailable);
            ParseJsonInteger(status, "ampereAdmissionHeldRequests", output.ampereAdmissionHeldRequests);
            ParseJsonInteger(status, "ampereCandidateVersionMajor", output.ampereCandidateVersionMajor);
            ParseJsonInteger(status, "ampereCandidateVersionMinor", output.ampereCandidateVersionMinor);
            ParseJsonInteger(status, "ampereCandidateVersionBuild", output.ampereCandidateVersionBuild);
            ParseJsonInteger(status, "ampereCreatedFeatures", output.ampereCreatedFeatures);
            ParseJsonInteger(status, "ampereEvaluations", output.ampereEvaluations);
            if (!ParseJsonBool(status, "ampereLegacySinglePreset", output.ampereLegacySinglePreset))
                return FALSE;
        }
        ParseJsonBool(status, "bridgeReady", output.bridgeReady);
        ParseJsonBool(status, "loaderCoreImported",
            output.loaderCoreImported);
        ParseJsonBool(status, "mainResolverDiscoveryInstalled",
            output.mainResolverDiscoveryInstalled);
        ParseJsonBool(status, "slInitEntryDetourCurrent",
            output.slInitEntryDetourCurrent);
        ParseJsonBool(status, "slInitIatFallbackInstalled",
            output.slInitIatFallbackInstalled);
        ParseJsonBool(status, "slInitResolverFallbackActive",
            output.slInitResolverFallbackActive);
        ParseJsonBool(status, "slInitControlPathReady",
            output.slInitControlPathReady);
        ParseJsonInteger(status, "slInitCalls", output.slInitCalls);
        ParseJsonInteger(status, "slInitFlagsBefore",
            output.slInitFlagsBefore);
        ParseJsonInteger(status, "slInitFlagsAfter",
            output.slInitFlagsAfter);
        ParseJsonBool(status, "otaPreferencesForced",
            output.otaPreferencesForced);
        ParseJsonBool(status, "otaPreferencesEnabledAtInit",
            output.otaPreferencesEnabledAtInit);
        ParseJsonBool(status, "downloadedStreamlinePluginsEnabledAtInit",
            output.downloadedStreamlinePluginsEnabledAtInit);
        ParseJsonBool(status, "otaProviderPreflightSupported",
            output.otaProviderPreflightSupported);
        ParseJsonBool(status, "otaForceSuppressed",
            output.otaForceSuppressed);
        ParseJsonBool(status, "nvidiaCompatibilityResolved",
            output.nvidiaCompatibilityResolved);
        ParseJsonInteger(status, "nvidiaProfileStatus",
            output.nvidiaProfileStatus);
        std::string nvidiaProfileName;
        if (ParseJsonString(status, "nvidiaProfileName",
                nvidiaProfileName))
        {
            strncpy_s(output.nvidiaProfileName,
                nvidiaProfileName.c_str(), _TRUNCATE);
        }
        ParseJsonInteger(status, "nvidiaCompatibilityTier",
            output.nvidiaCompatibilityTier);
        ParseJsonInteger(status, "nvidiaCompatibilityManifestEntries",
            output.nvidiaCompatibilityManifestEntries);
        ParseJsonInteger(status, "nvidiaPolicyCeilingMultiplier",
            output.nvidiaPolicyCeilingMultiplier);
        ParseJsonInteger(status, "wrapperNativeMaximumMultiplier",
            output.wrapperNativeMaximumMultiplier);
        ParseJsonBool(status, "compatibilityFallback",
            output.compatibilityFallback);
        ParseJsonInteger(status, "compatibilityReason",
            output.compatibilityReason);
        ParseJsonBool(status, "fullStreamlineOtaRequested",
            output.fullStreamlineOtaRequested);
        ParseJsonBool(status, "fullStreamlineOtaEligible",
            output.fullStreamlineOtaEligible);
        ParseJsonBool(status, "downloadedStreamlinePluginsForced",
            output.downloadedStreamlinePluginsForced);
        ParseJsonInteger(status, "streamlineHostVersionMajor",
            output.streamlineHostVersionMajor);
        ParseJsonInteger(status, "streamlineHostVersionMinor",
            output.streamlineHostVersionMinor);
        ParseJsonInteger(status, "streamlineHostVersionBuild",
            output.streamlineHostVersionBuild);
        ParseJsonInteger(status, "streamlineHostVersionPrivate",
            output.streamlineHostVersionPrivate);
        ParseJsonBool(status, "streamlineLoaderDiscoveryInstalled",
            output.streamlineLoaderDiscoveryInstalled);
        ParseJsonInteger(status, "streamlineLoaderDiscoveryCalls",
            output.streamlineLoaderDiscoveryCalls);
        ParseJsonBool(status, "setOptionsEntryDetourCurrent",
            output.setOptionsEntryDetourCurrent);
        ParseJsonBool(status, "setOptionsResolverFallbackActive",
            output.setOptionsResolverFallbackActive);
        ParseJsonInteger(status, "setOptionsResolverFallbackCalls",
            output.setOptionsResolverFallbackCalls);
        ParseJsonBool(status, "setOptionsControlPathReady",
            output.setOptionsControlPathReady);
        ParseJsonBool(status, "getStateEntryDetourCurrent",
            output.getStateEntryDetourCurrent);
        ParseJsonBool(status, "ngxCreateEntryDetourCurrent",
            output.ngxCreateEntryDetourCurrent);
        ParseJsonBool(status, "ngxEvaluateEntryDetourCurrent",
            output.ngxEvaluateEntryDetourCurrent);
        ParseJsonBool(status, "backportReadyAtCreate",
            output.backportReadyAtCreate);
        ParseJsonBool(status, "pipelineMayPredateDetour",
            output.pipelineMayPredateDetour);
        ParseJsonBool(status, "gameFrameGenerationOn",
            output.gameFrameGenerationOn);
        ParseJsonBool(status, "appliedFrameGenerationOn",
            output.appliedFrameGenerationOn);
        ParseJsonBool(status, "streamlineRebuildRequired",
            output.streamlineRebuildRequired);
        ParseJsonBool(status, "perSampleSynthesisReady",
            output.perSampleSynthesisReady);
        ParseJsonBool(status, "highCapabilityPublicationAllowed",
            output.highCapabilityPublicationAllowed);
        ParseJsonBool(status, "dllNotificationRegistered",
            output.dllNotificationRegistered);
        ParseJsonBool(status, "streamlinePluginLoaderHooksInstalled",
            output.pluginLoaderHooksInstalled);
        ParseJsonBool(status, "activeWrapperObserved",
            output.activeWrapperObserved);
        ParseJsonBool(status, "activeWrapperUsesNvidiaOta",
            output.activeWrapperUsesNvidiaOta);
        ParseJsonInteger(status, "activeWrapperVersionMajor",
            output.activeWrapperVersionMajor);
        ParseJsonInteger(status, "activeWrapperVersionMinor",
            output.activeWrapperVersionMinor);
        ParseJsonInteger(status, "activeWrapperVersionBuild",
            output.activeWrapperVersionBuild);
        ParseJsonInteger(status, "activeWrapperVersionPrivate",
            output.activeWrapperVersionPrivate);
        ParseJsonInteger(status,
            "wrapperCompiledMaximumGeneratedFrames",
            output.wrapperCompiledMaximumGeneratedFrames);
        ParseJsonInteger(status, "safeMaximumMultiplier",
            output.safeMaximumMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dlssgPresetRequested", output.dlssgPresetRequested);
        ParseJsonInteger(status, "dlssgPresetLatched", output.dlssgPresetLatched);
        ParseJsonBool(status, "dlssgPresetSelectionFrozen", output.dlssgPresetSelectionFrozen);
        ParseJsonBool(status, "dlssgPresetRestartRequired", output.dlssgPresetRestartRequired);
        ParseJsonInteger(status, "dlssgPresetObserved", output.dlssgPresetObserved);
        ParseJsonBool(status, "dlssgPresetObservedValid", output.dlssgPresetObservedValid);
        ParseJsonInteger(status, "dlssgPresetOverrideReadCount", output.dlssgPresetOverrideReadCount);
        ParseJsonBool(status, "dlssgPresetOverrideInstalled", output.dlssgPresetOverrideInstalled);
        ParseJsonInteger(status, "dlssgPresetOverrideFailure", output.dlssgPresetOverrideFailure);
        ParseJsonInteger(status, "dlssgPresetReadCount", output.dlssgPresetReadCount);
        ParseJsonBool(status, "dlssgPresetCAvailable", output.dlssgPresetCAvailable);
        ParseJsonInteger(status, "dlssgPresetLiveSwitch", output.dlssgPresetLiveSwitch);
        ParseJsonBool(status, "dlssgPresetSwitching", output.dlssgPresetSwitching);
        ParseDlssFeature(status, "dlssSr", haveSavedControl, output.dlssSr);
        ParseDlssFeature(status, "dlssRr", haveSavedControl, output.dlssRr);
        if(!haveSavedControl)dlss5::settings_json::Parse(status,output.dlss5);
        ParseJsonBool(status,"dlss5Detected",output.dlss5Detected);
        ParseJsonBool(status,"dlss5Pending",output.dlss5Pending);
        ParseJsonInteger(status,"dlss5State",output.dlss5State);
        ParseJsonInteger(status,"dlss5Result",output.dlss5Result);
        ParseJsonInteger(status,"dlss5Route",output.dlss5Route);
        ParseJsonInteger(status,"dlss5Stage",output.dlss5Stage);
        ParseJsonInteger(status,"dlss5Passes",output.dlss5Passes);
        ParseJsonInteger(status,"dlss5Width",output.dlss5Width);
        ParseJsonInteger(status,"dlss5Height",output.dlss5Height);
        ParseJsonInteger(status,"dlss5ExtentWidth",output.dlss5ExtentWidth);
        ParseJsonFloat(status,"dlss5EffectiveScale",output.dlss5EffectiveScale);
        ParseJsonFloat(status,"dlss5GpuMs",output.dlss5GpuMs);
        ParseJsonFloat(status,"dlss5BudgetMs",output.dlss5BudgetMs);
        ParseJsonFloat(status,"dlss5ExposureGain",output.dlss5ExposureGain);
        ParseJsonFloat(status,"dlss5FrameMs",output.dlss5FrameMs);
        ParseJsonFloat(status,"dlss5BoostNetworkMs",output.dlss5BoostNetworkMs);
        ParseJsonBool(status,"dlss5ScaleFloorHeld",output.dlss5ScaleFloorHeld);
        ParseJsonBool(status,"dlss5D3D11",output.dlss5D3D11);
        ParseJsonInteger(status,"dlss5D3D11Refusal",output.dlss5D3D11Refusal);
        std::string optiScalerUiOwner;
        if (ParseJsonString(status, "optiScalerUiOwner", optiScalerUiOwner))
            output.optiScalerUiOwner = optiScalerUiOwner == "shared" ? 1u : optiScalerUiOwner == "optiscaler" ? 2u
                : optiScalerUiOwner == "rtxmfg" ? 3u : 0u;
        ParseJsonInteger(status,"dlss5ExtentHeight",output.dlss5ExtentHeight);
        ParseJsonInteger(status,"dlss5Evaluations",output.dlss5Evaluations);
        ParseJsonBool(status, "requestedMultiplierLimited",
            output.requestedMultiplierLimited);
        if (!haveSavedControl) ParseJsonInteger(status, "multiplier", output.desiredMultiplier);
        ParseJsonInteger(status, "appliedMultiplier",
            output.appliedMultiplier);
        if (!haveSavedControl)
            ParseJsonInteger(status, "dynamicTargetFrameRate", output.dynamicTargetFrameRate);
        BOOL appliedTargetValid = FALSE;
        float appliedTarget = 0.0f;
        if (ParseJsonBool(status, "appliedDynamicTargetValid", appliedTargetValid)
            && appliedTargetValid
            && ParseJsonFloat(status, "appliedDynamicTargetFrameRate", appliedTarget)
            && appliedTarget >= 0.0f)
        {
            output.appliedDynamicTargetFrameRate = appliedTarget;
            output.appliedDynamicTargetValid = TRUE;
        }
        ParseJsonInteger(status, "requestRevision",
            output.desiredRevision);
        ParseJsonInteger(status, "appliedRevision",
            output.appliedRevision);
        ParseJsonInteger(status, "actualFramesPresented",
            output.actualFramesPresented);
        ParseJsonInteger(status, "numFramesToGenerateMax",
            output.numFramesToGenerateMax);
        ParseJsonInteger(status, "setOptionsResult",
            output.lastSetOptionsResult);
        ParseJsonInteger(status, "getStateResult",
            output.lastGetStateResult);
        ParseJsonBool(status, "setOptionsSeen",
            output.setOptionsSeen);
        ParseJsonBool(status, "setOptionsAccepted",
            output.setOptionsAccepted);
        ParseJsonBool(status, "getStateSeen",
            output.getStateSeen);
        ParseJsonBool(status, "dynamicMfgSupportKnown",
            output.dynamicMfgSupportKnown);
        ParseJsonBool(status, "dynamicMfgSupported",
            output.dynamicMfgSupported);
        ParseJsonBool(status, "fgVsyncSupportKnown", output.fgVsyncSupportKnown);
        ParseJsonBool(status, "fgVsyncSupported", output.fgVsyncSupported);
        if (!haveSavedControl)
        {
            ParseJsonInteger(status, "vsyncMode", output.vsyncMode);
            ParseJsonInteger(status, "reflexFrameLimitFps", output.reflexFrameLimitFps);
        }
        ParseJsonBool(status, "dynamicVsyncAvailable", output.dynamicVsyncAvailable);
        ParseJsonBool(status, "vsyncControlAvailable", output.vsyncControlAvailable);
        ParseJsonBool(status, "vsyncOverrideApplied", output.vsyncOverrideApplied);
        ParseJsonBool(status, "vsyncPresentationObserved", output.vsyncPresentationObserved);
        ParseJsonInteger(status, "vsyncOriginalInterval", output.vsyncOriginalInterval);
        ParseJsonInteger(status, "vsyncSubmittedInterval", output.vsyncSubmittedInterval);
        ParseJsonInteger(status, "vsyncFailure", output.vsyncFailure);
        ParseJsonBool(status, "vsyncVulkanSwapchain", output.vsyncVulkanSwapchain);
        ParseJsonBool(status, "vsyncVulkanHook", output.vsyncVulkanHook);
        ParseJsonInteger(status, "vsyncOriginalPresentMode", output.vsyncOriginalPresentMode);
        ParseJsonInteger(status, "vsyncSubmittedPresentMode", output.vsyncSubmittedPresentMode);
        ParseJsonBool(status, "reflexControlAvailable", output.reflexControlAvailable);
        ParseJsonBool(status, "reflexAppliedKnown", output.reflexAppliedKnown);
        ParseJsonInteger(status, "reflexAppliedFrameLimitUs", output.reflexAppliedFrameLimitUs);
        ParseJsonBool(status, "reflexLimitPending", output.reflexLimitPending);
        ParseJsonBool(status, "reflexRestorePending", output.reflexRestorePending);
        ParseJsonInteger(status, "reflexStatus", output.reflexStatus);
        ParseJsonInteger(status, "reflexLastResult", output.reflexLastResult);
        ParseJsonInteger(status, "reflexHookMask", output.reflexHookMask);
        ParseJsonInteger(status, "reflexModuleVersionMajor", output.reflexModuleVersionMajor);
        ParseJsonInteger(status, "reflexModuleVersionMinor", output.reflexModuleVersionMinor);
        ParseJsonInteger(status, "reflexModuleVersionPatch", output.reflexModuleVersionPatch);
        ParseJsonInteger(status, "reflexModuleGeneration", output.reflexModuleGeneration);
        ParseJsonBool(status, "intervalLoggingEnabled",
            output.intervalLoggingEnabled);
        ParseJsonBool(status, "intervalLogReady",
            output.intervalLogReady);
        ParseJsonInteger(status, "intervalValidSamples",
            output.intervalValidSamples);
        ParseJsonInteger(status, "intervalInvalidSamples",
            output.intervalInvalidSamples);
        ParseJsonInteger(status, "intervalDroppedSamples",
            output.intervalDroppedSamples);
        ParseJsonInteger(status, "intervalLastCount",
            output.intervalLastCount);
        ParseJsonInteger(status, "intervalLastIndex",
            output.intervalLastIndex);
        ParseJsonInteger(status, "intervalLastPositionNumerator",
            output.intervalLastPositionNumerator);
        ParseJsonInteger(status, "intervalLastPositionDenominator",
            output.intervalLastPositionDenominator);
        ParseJsonInteger(status, "realFpsMilli", output.realFpsMilli);
        ParseJsonInteger(status, "dlssFpsMilli", output.dlssFpsMilli);
        ParseJsonInteger(status, "fpsOutputSource", output.fpsOutputSource);
        ParseJsonInteger(status, "fpsSampleAgeMs", output.fpsSampleAgeMs);
        ParseJsonBool(status, "latencyAvailable", output.latencyAvailable);
        ParseJsonInteger(status, "latencyState", output.latencyState);
        ParseJsonInteger(status, "latencyFrames", output.latencyFrames);
        ParseJsonInteger(status, "latencyExactPermille", output.latencyExactPermille);
        ParseJsonInteger(status, "latencyMeanUs", output.latencyMeanUs);
        ParseJsonInteger(status, "latencyP99Us", output.latencyP99Us);
        ParseJsonInteger(status, "latencyRealUs", output.latencyRealUs);
        ParseJsonInteger(status, "latencyGameUs", output.latencyGameUs);
        ParseJsonInteger(status, "latencyFgUs", output.latencyFgUs);
        ParseJsonInteger(status, "latencyDisplayUs", output.latencyDisplayUs);
        ParseJsonInteger(status, "latencyAgeMs", output.latencyAgeMs);
        ParseJsonBool(status, "uiInputsReady", output.uiInputsReady);
        ParseJsonBool(status, "uiRecompositionEnabled",
            output.uiRecompositionEnabled);
        ParseJsonBool(status, "uiRecompositionForced",
            output.uiRecompositionForced);
        ParseJsonBool(status, "gameUiRecompositionEnabled", output.gameUiRecompositionEnabled);
        if (!haveSavedControl)
            ParseJsonBool(status, "generatedOnlyDebug", output.generatedOnlyDebug);
        ParseJsonBool(status, "appliedGeneratedOnlyDebug",
            output.appliedGeneratedOnlyDebug);
        std::string intervalFile;
        if (ParseJsonString(status, "intervalLogFile", intervalFile))
            strncpy_s(output.intervalLogFile,
                intervalFile.c_str(), _TRUNCATE);
        std::string mode;
        if (!haveSavedControl && ParseJsonString(status, "mode", mode))
            output.dynamicMode = mode == "dynamic" ? TRUE : FALSE;
        std::string appliedMode;
        if (ParseJsonString(status, "appliedMode", appliedMode))
        {
            output.appliedDynamicMode =
                appliedMode == "dynamic" ? TRUE : FALSE;
        }
        std::string route;
        if (ParseJsonString(status, "route", route))
            strncpy_s(output.patchRoute, route.c_str(), _TRUNCATE);
        std::string activeWrapperPath;
        if (ParseJsonString(status, "activeWrapperPath", activeWrapperPath))
            strncpy_s(output.activeWrapperPath,
                activeWrapperPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeWrapperGeneration",
            output.activeWrapperGeneration);
        std::string activeControlPath;
        if (ParseJsonString(status, "activeControlPath", activeControlPath))
            strncpy_s(output.activeControlPath,
                activeControlPath.c_str(), _TRUNCATE);
        std::string activeStatePath;
        if (ParseJsonString(status, "activeStatePath", activeStatePath))
            strncpy_s(output.activeStatePath,
                activeStatePath.c_str(), _TRUNCATE);
        std::string activeControlDetour;
        if (ParseJsonString(status, "activeControlDetour",
                activeControlDetour))
            strncpy_s(output.activeControlDetour,
                activeControlDetour.c_str(), _TRUNCATE);
        std::string activeStateDetour;
        if (ParseJsonString(status, "activeStateDetour", activeStateDetour))
            strncpy_s(output.activeStateDetour,
                activeStateDetour.c_str(), _TRUNCATE);
        std::string activeProviderPath;
        if (ParseJsonString(status, "activeProviderPath", activeProviderPath))
            strncpy_s(output.activeProviderPath,
                activeProviderPath.c_str(), _TRUNCATE);
        ParseJsonInteger(status, "activeProviderVersionMajor",
            output.activeProviderVersionMajor);
        ParseJsonInteger(status, "activeProviderVersionMinor",
            output.activeProviderVersionMinor);
        ParseJsonInteger(status, "activeProviderVersionBuild",
            output.activeProviderVersionBuild);
        ParseJsonInteger(status, "activeProviderVersionPrivate",
            output.activeProviderVersionPrivate);
        ParseJsonInteger(status, "activeProviderGeneration",
            output.activeProviderGeneration);
        std::string providerSelectionSource;
        if (ParseJsonString(status, "providerSelectionSource",
                providerSelectionSource))
            strncpy_s(output.providerSelectionSource,
                providerSelectionSource.c_str(), _TRUNCATE);
        std::string providerCreateDetour;
        if (ParseJsonString(status, "providerCreateDetour",
                providerCreateDetour))
            strncpy_s(output.providerCreateDetour,
                providerCreateDetour.c_str(), _TRUNCATE);
        std::string providerEvaluateDetour;
        if (ParseJsonString(status, "providerEvaluateDetour",
                providerEvaluateDetour))
            strncpy_s(output.providerEvaluateDetour,
                providerEvaluateDetour.c_str(), _TRUNCATE);
        ParseJsonBool(status, "midpointReadyAtFirstCreate",
            output.midpointReadyAtFirstCreate);
        ParseJsonInteger(status, "activeLastCallRevision",
            output.activeLastCallRevision);
        ParseJsonInteger(status, "activeLastAcceptedRevision",
            output.activeLastAcceptedRevision);
        ParseJsonInteger(status, "universalRouteFailure",
            output.universalRouteFailure);
        std::string routeFailureReason;
        if (ParseJsonString(status, "universalRouteFailureReason",
                routeFailureReason))
            strncpy_s(output.universalRouteFailureReason,
                routeFailureReason.c_str(), _TRUNCATE);
        ParseJsonBool(status, "releaseEntryCurrent",
            output.releaseEntryCurrent);
        ParseJsonBool(status, "frameGenerationOffAccepted",
            output.frameGenerationOffAccepted);
        ParseJsonBool(status, "releaseObserved", output.releaseObserved);
        output.perSampleSynthesisReady = output.midpointReadyAtFirstCreate;
        output.highCapabilityPublicationAllowed =
            output.bridgeReady && output.midpointReadyAtFirstCreate;
    }
    *snapshot = output;
    return TRUE;
}

std::string NativeControlJson(uint32_t multiplier, BOOL followGame, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL generatedOnlyDebug, BOOL intervalLogging,
    uint32_t preset, uint32_t vsyncMode = 0, uint32_t reflexFrameLimitFps = 0)
{
    char content[384]{};
    const int length = std::snprintf(content, sizeof(content),
        "{\"followGame\":%s,\"mode\":\"%s\",\"multiplier\":%u,"
        "\"dynamicTargetFrameRate\":%u,\"dlssgPreset\":%u,\"dlssgDefaults\":1,"
        "\"vsyncMode\":%u,\"reflexFrameLimitFps\":%u,"
        "\"generatedOnlyDebug\":%s,\"intervalLogging\":%s,"
        "\"version\":13}\n",
        followGame ? "true" : "false",
        followGame ? "follow"
            : dynamicMode ? "dynamic" : "fixed",
        multiplier,
        dynamicTargetFrameRate,
        preset, vsyncMode, reflexFrameLimitFps,
        generatedOnlyDebug ? "true" : "false", intervalLogging ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content))
        return {};
    return std::string(content, static_cast<size_t>(length));
}

// Carries a raw JSON value of name from the saved control into content when the
// new content lacks it. Writers that rebuild the whole object (DLSS mode,
// preset) then keep the saved XeSS model and UI owner.
void CarryControlField(std::string& content, const std::string& saved, const char* name)
{
    size_t offset = 0, existing = 0;
    if (FindJsonValue(content, name, offset) || !FindJsonValue(saved, name, existing)) return;
    size_t end = existing;
    while (end < saved.size() && saved[end] != ',' && saved[end] != '}'
        && saved[end] != '\r' && saved[end] != '\n') ++end;
    const size_t close = content.find_last_not_of(" \t\r\n");
    if (end == existing || close == std::string::npos || content[close] != '}') return;
    content.insert(close, std::string(",\"") + name + "\":" + saved.substr(existing, end-existing));
}

BOOL WriteNativeControl(const std::string& input)
{
    std::string content = input;
    std::string saved;
    if (ReadTextFile(gNativeConfigPath, saved))
    {
        for (const char* name : {"xessModel", "optiScalerUi", "dlssSrPreset", "dlssRrPreset", "xessVulkan", "xessVulkanFrames",
                 "xemfgStandalone", "xemfgFrames", "xemfgDynamic", "xemfgTargetFps", "xemfgRounding", "xemfgAllow1x",
                 "xemfgTargetAsCap", "xemfgFlipQueue", "latencyPacing", "dlssgDefaults"})
            CarryControlField(content, saved, name);
        for (const auto& field : dlss5::settings_json::kFields)
            CarryControlField(content, saved, field.key);
    }
    if (content.empty() || content.size() > 4096) return FALSE;
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(gNativeConfigPath).parent_path(), error);
    if (error)
        return FALSE;
    const std::wstring temporary = gNativeConfigPath + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD written = 0;
    const BOOL write = WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
        &written, nullptr);
    const BOOL flushed = write && written == content.size()
        && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), gNativeConfigPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI NativeFileApplyControl(uint32_t multiplier, BOOL dynamicMode,
    uint32_t dynamicTargetFrameRate, BOOL dynamicExperimental56,
    BOOL generatedOnlyDebug)
{
    const BOOL followGame = multiplier == 0 && dynamicMode == FALSE;
    MfgUnlockReShadeSnapshot current{};
    if (!NativeFileGetSnapshot(&current)) return FALSE;
    if (!xess_ui::DlssMfgFamily(current.gpuFamily)) return FALSE;
    if (UiAmpere())
    {
        if (!ampere_policy::ControlValid(followGame ? 2u : multiplier, dynamicMode != FALSE,
                dynamicExperimental56 != FALSE, generatedOnlyDebug != FALSE)) return FALSE;
    }
    const uint32_t safeMaximumMultiplier = current.safeMaximumMultiplier;
    if ((!followGame && multiplier > safeMaximumMultiplier)
        || (dynamicMode && (!current.dynamicMfgSupportKnown || !current.dynamicMfgSupported
            || !nvidia_mfg_policy::DynamicRangeFits(current.numFramesToGenerateMax, safeMaximumMultiplier)
            || (UiAmpere() && safeMaximumMultiplier < 6)))
        || current.dlssgPresetRequested > 3)
        return FALSE;
    multiplier = followGame
        ? 2u : std::clamp(multiplier, UiAmpere() ? 1u : 2u, safeMaximumMultiplier);
    dynamicTargetFrameRate = std::min(dynamicTargetFrameRate, 1000u);
    BOOL intervalLogging = UiAmpere() ? FALSE : TRUE;
    if (UiAmpere())
    {
        std::string existingControl;
        if (ReadTextFile(gNativeConfigPath, existingControl))
            ParseJsonBool(existingControl, "intervalLogging", intervalLogging);
    }
    return WriteNativeControl(NativeControlJson(multiplier, followGame, dynamicMode,
        dynamicTargetFrameRate, generatedOnlyDebug, intervalLogging,
        current.dlssgPresetRequested, current.vsyncMode, current.reflexFrameLimitFps));
}

bool SetUnsignedControlField(std::string& content, const char* name, uint32_t value)
{
    size_t offset = 0;
    if (FindJsonValue(content, name, offset))
    {
        uint32_t previous = 0;
        if (!ParseJsonInteger(content, name, previous)) return false;
        size_t end = offset;
        while (end < content.size() && content[end] >= '0' && content[end] <= '9') ++end;
        content.replace(offset, end-offset, std::to_string(value));
    }
    else
    {
        const size_t end = content.find_last_not_of(" \t\r\n");
        if (end == std::string::npos || content[end] != '}') return false;
        content.insert(end, std::string(",\"") + name + "\":" + std::to_string(value));
    }
    return true;
}

// key: "dlssgPreset" (0..3, checked here), "dlssSrPreset" or "dlssRrPreset"
// (the caller validates those values).
BOOL NativeFilePersistPreset(const char* key, uint32_t preset)
{
    if (std::strcmp(key, "dlssgPreset") == 0 && preset > 3) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)) return FALSE;
    }
    else
    {
        // Never overwrite an unreadable existing file as though it were absent.
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            current.vsyncMode, current.reflexFrameLimitFps);
    }
    // Preset changes do not need Dynamic capability or an active FG or DLSS
    // feature. The backend's immutable preset state decides whether the saved
    // request belongs to this process or the next restart.
    if (!SetUnsignedControlField(content, key, preset)
        || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    // latency.1: a DLSS-G preset saved here is a choice, B included (TryParseControl's dlssgDefaults).
    if (std::strcmp(key, "dlssgPreset") == 0 && !SetUnsignedControlField(content, "dlssgDefaults", 1)) return FALSE;
    return WriteNativeControl(content);
}

BOOL NativeFilePersistDlss5(const dlss5::Settings& settings)
{
    std::string content;
    if(ReadTextFile(gNativeConfigPath,content)) {
        MfgUnlockReShadeSnapshot saved{};if(!ParseNativeControlSnapshot(content,&saved))return FALSE;
    }else{
        const auto attributes=GetFileAttributesW(gNativeConfigPath.c_str());const auto error=GetLastError();
        if(attributes!=INVALID_FILE_ATTRIBUTES || (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND))return FALSE;
        MfgUnlockReShadeSnapshot current{};if(!NativeFileGetSnapshot(&current))return FALSE;
        content=NativeControlJson(current.desiredMultiplier,current.followGameMode,current.dynamicMode,
            current.dynamicTargetFrameRate,current.generatedOnlyDebug,current.intervalLoggingEnabled,
            current.dlssgPresetRequested,current.vsyncMode,current.reflexFrameLimitFps);
    }
    if(!dlss5::settings_json::Write(content,settings))return FALSE;
    return WriteNativeControl(content);
}

BOOL NativeFilePersistPresentation(uint32_t vsyncMode, uint32_t reflexFrameLimitFps)
{
    if (vsyncMode > 2 || reflexFrameLimitFps > 1000) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)
            || !SetUnsignedControlField(content, "vsyncMode", vsyncMode)
            || !SetUnsignedControlField(content, "reflexFrameLimitFps", reflexFrameLimitFps)
            || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            vsyncMode, reflexFrameLimitFps);
    }
    return WriteNativeControl(content);
}

// XeMFG standalone's switch, frame count and Dynamic MFG settings, all live: the host moves the game's swap chain to
// XeFG or back (xess/d3d12_host.h Move).
BOOL NativeFilePersistXemfg(bool on, uint32_t frames, const xess_ui::State& x)
{
    frames = std::clamp<uint32_t>(frames, 2, 6);
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            current.vsyncMode, current.reflexFrameLimitFps);
    }
    if (!SetUnsignedControlField(content, "xemfgStandalone", on ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgFrames", frames)
        || !SetUnsignedControlField(content, "xemfgDynamic", x.xemfgDynamic ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgTargetFps", std::min<uint32_t>(x.xemfgTargetFps, 1000))
        || !SetUnsignedControlField(content, "xemfgRounding", std::min<uint32_t>(x.xemfgRounding, 2))
        || !SetUnsignedControlField(content, "xemfgAllow1x", x.xemfgAllow1x ? 1u : 0u)
        || !SetUnsignedControlField(content, "xemfgTargetAsCap", x.xemfgTargetAsCap ? 1u : 0u)
        || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    return WriteNativeControl(content);
}

// latency.1: low-latency pacing (0 auto, 1 on, 2 off). The backend applies it live (patcher.cpp ConfigureXemfg).
BOOL NativeFilePersistLatencyPacing(uint32_t mode)
{
    if (mode > 2) return FALSE;
    std::string content;
    if (ReadTextFile(gNativeConfigPath, content))
    {
        MfgUnlockReShadeSnapshot saved{};
        if (!ParseNativeControlSnapshot(content, &saved)) return FALSE;
    }
    else
    {
        const DWORD attributes = GetFileAttributesW(gNativeConfigPath.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return FALSE;
        MfgUnlockReShadeSnapshot current{};
        if (!NativeFileGetSnapshot(&current)) return FALSE;
        content = NativeControlJson(current.desiredMultiplier, current.followGameMode,
            current.dynamicMode, current.dynamicTargetFrameRate, current.generatedOnlyDebug,
            current.intervalLoggingEnabled, current.dlssgPresetRequested,
            current.vsyncMode, current.reflexFrameLimitFps);
    }
    if (!SetUnsignedControlField(content, "latencyPacing", mode)
        || !SetUnsignedControlField(content, "version", 13)) return FALSE;
    return WriteNativeControl(content);
}

void WINAPI NativeFileSetFrontendAttached(BOOL)
{
}

BOOL WINAPI NativeFileRegisterD3D12Device(ID3D12Device*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Queue(ID3D12CommandQueue*)
{
    return TRUE;
}

BOOL WINAPI NativeFileRegisterD3D12Swapchain(
    IDXGISwapChain*, IUnknown*)
{
    return TRUE;
}

BOOL WINAPI NativeFileUnregisterD3D12Swapchain(IDXGISwapChain*)
{
    return TRUE;
}

bool ConnectBackend() noexcept
{
    if (gBackendConnected.load(std::memory_order_acquire))
        return true;

    AcquireSRWLockExclusive(&gBackendConnectionLock);
    if (gBackendConnected.load(std::memory_order_acquire))
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return true;
    }
    HMODULE core = gSelf;
    HMODULE shim = gSelf;
    if (!core || !shim || !GetProcAddress(core, "MfgUnlockCoreLoaded")
        || !ResolveNativeFilePaths())
    {
        ReleaseSRWLockExclusive(&gBackendConnectionLock);
        return false;
    }
    gBackend = core;
    gBackendInterface = {};
    gSampleFrameTelemetry = reinterpret_cast<
        MfgUnlockSampleFrameTelemetryFn>(GetProcAddress(
            core, "MfgUnlockSampleFrameTelemetry"));
    gBackendInterface.getSnapshot = &NativeFileGetSnapshot;
    gBackendInterface.applyControl = &NativeFileApplyControl;
    gBackendInterface.registerD3D12Device =
        &NativeFileRegisterD3D12Device;
    gBackendInterface.registerD3D12Queue =
        &NativeFileRegisterD3D12Queue;
    gBackendInterface.registerD3D12Swapchain =
        &NativeFileRegisterD3D12Swapchain;
    gBackendInterface.unregisterD3D12Swapchain =
        &NativeFileUnregisterD3D12Swapchain;
    gBackendInterface.setFrontendAttached =
        &NativeFileSetFrontendAttached;
    gBackendKind.store(BackendKind::eNativeFile,
        std::memory_order_release);
    gBackendConnected.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&gBackendConnectionLock);
    return true;
}


void PersistSettings(reshade::api::effect_runtime* runtime)
{
    reshade::set_config_value(runtime, kConfigSection,
        "FollowGameMode", gFollowGameMode);
    reshade::set_config_value(runtime, kConfigSection,
        "Multiplier", gMultiplier);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicMode", gDynamicMode);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicTargetFrameRate", gDynamicTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicLockToRefreshRate", gDynamicLockToRefreshRate);
    reshade::set_config_value(runtime, kConfigSection,
        "DynamicCustomTargetFrameRate", gDynamicCustomTargetFrameRate);
    reshade::set_config_value(runtime, kConfigSection,
        "GeneratedOnlyDebug", gGeneratedOnlyDebug);
}

void ApplySettings(reshade::api::effect_runtime* runtime, bool persist)
{
    MfgUnlockReShadeSnapshot capacity{};
    if (!BackendGetSnapshot(&capacity))
    {
        if (persist)
        {
            gLastApplyAttempted = true;
            gLastApplyAccepted = false;
            gLastNativeConfigPersisted = false;
        }
        return;
    }
    if (UiAmpere())
    {
        gMultiplier = std::clamp(gMultiplier, 1, 6);
    }
    else
    {
        gMultiplier = std::clamp(gMultiplier, 2, 6);
    }
    gDynamicExperimental56 = false;
    gDynamicCustomTargetFrameRate = std::clamp(
        gDynamicCustomTargetFrameRate, 1, 1000);
    gDynamicTargetFrameRate = gDynamicLockToRefreshRate
        ? 0 : gDynamicCustomTargetFrameRate;
    const BOOL accepted = BackendApplyControl(
        gFollowGameMode ? 0u : static_cast<uint32_t>(gMultiplier),
        !gFollowGameMode && gDynamicMode ? TRUE : FALSE,
        static_cast<uint32_t>(gDynamicTargetFrameRate),
        gDynamicExperimental56 ? TRUE : FALSE,
        gGeneratedOnlyDebug ? TRUE : FALSE);
    if (persist)
    {
        const BOOL companionConfigPersisted = TRUE;
        const BOOL nativeConfigPersisted = accepted;
        gLastNativeConfigPersisted = companionConfigPersisted != FALSE
            && nativeConfigPersisted != FALSE;
        gLastApplyAttempted = true;
        gLastApplyAccepted = accepted != FALSE;
        if (accepted) PersistSettings(runtime);
    }
}

void LoadSettings()
{
    gPresetSaveFailed = false;
    gSrPresetSaveFailed = false;
    gRrPresetSaveFailed = false;
    gPresentationSaveFailed = false;
    gDefaultDockInitialized = false;
    reshade::get_config_value(nullptr, kConfigSection,
        "DefaultDockInitialized", gDefaultDockInitialized);

    MfgUnlockReShadeSnapshot snapshot{};
    bool snapshotLoaded = BackendGetSnapshot(&snapshot) != FALSE;
    // Saved intent is independent of a transient status-file read failure.
    if (BackendUsesNativeFile())
    {
        MfgUnlockReShadeSnapshot saved{};
        if (ReadNativeControlSnapshot(&saved)) { snapshot = saved; snapshotLoaded = true; }
    }
    if (snapshotLoaded)
    {
        gFollowGameMode = snapshot.followGameMode != FALSE;
        gMultiplier = static_cast<int>(snapshot.desiredMultiplier);
        gDynamicMode = snapshot.dynamicMode != FALSE;
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
        gDlssSrPreset = dlss_sr::IsValidSelection(snapshot.dlssSr.presetRequested)
            ? snapshot.dlssSr.presetRequested : dlss_sr::kGameOrDriver;
        gDlssRrPreset = dlss_sr::IsValidRrSelection(snapshot.dlssRr.presetRequested)
            ? snapshot.dlssRr.presetRequested : dlss_sr::kGameOrDriver;
        gDlss5=snapshot.dlss5;
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
        gDynamicTargetFrameRate = static_cast<int>(
            snapshot.dynamicTargetFrameRate);
        if (gReflexFrameLimitFps > 0) gReflexCustomLimitFps = gReflexFrameLimitFps;
        gDynamicLockToRefreshRate = gDynamicTargetFrameRate == 0;
        if (!gDynamicLockToRefreshRate)
        {
            gDynamicCustomTargetFrameRate = gDynamicTargetFrameRate;
        }
        else
        {
            reshade::get_config_value(nullptr, kConfigSection,
                "DynamicCustomTargetFrameRate",
                gDynamicCustomTargetFrameRate);
        }
        gDynamicCustomTargetFrameRate = std::clamp(
            gDynamicCustomTargetFrameRate, 1, 1000);
        gDynamicExperimental56 = false;
        gGeneratedOnlyDebug = snapshot.generatedOnlyDebug != FALSE;
    }

    // The early-loaded core owns the universal control file. ReShade may
    // initialize much later, so stale ReShade.ini values must not overwrite
    // the request which was already consumed during Streamline startup.
    // No startup/migration write is allowed merely because status is absent.
}

const char* YesNo(BOOL value) noexcept
{
    return value ? "yes" : "no";
}

// Menu layout. Stock ImGui colours; only status values are tinted.
enum class Tone { Normal, Good, Pending, Bad, Dim };

ImVec4 ToneColor(Tone tone)
{
    switch (tone)
    {
    case Tone::Good: return ImVec4(0.50f, 0.79f, 0.50f, 1.0f);
    case Tone::Pending: return ImVec4(0.88f, 0.69f, 0.31f, 1.0f);
    case Tone::Bad: return ImVec4(0.94f, 0.45f, 0.42f, 1.0f);
    case Tone::Dim: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

void HelpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
}

void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

// Tabs: plain labels over one hairline. The open tab is white and underlined in
// the accent, the others grey (stock ImGui tabs are filled blue boxes).
ImVec2 TabPadding()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    return ImVec2(style.FramePadding.x + font * 0.25f, style.FramePadding.y + font * 0.20f);
}

// The tab bar reads these while it lays out and draws each tab, so they are
// pushed around BeginTabBar and each BeginTabItem, never around a tab's page.
void PushTabStyle()
{
    const ImVec2 padding = TabPadding();
    for (const ImGuiCol col : {ImGuiCol_Tab, ImGuiCol_TabSelected, ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected})
        ImGui::PushStyleColor(col, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
        ImVec2(ImGui::GetFontSize() * 0.25f, ImGui::GetStyle().ItemInnerSpacing.y));
}

void PopTabStyle()
{
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

// BeginTabBar with the tab look; EndTabBar as usual.
bool BeginTabs(const char* id)
{
    PushTabStyle();
    const bool open = ImGui::BeginTabBar(id, ImGuiTabBarFlags_NoTooltip);
    PopTabStyle();
    if (!open) return false;
    const ImRect bar = ImGui::GetCurrentTabBar()->BarRect;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bar.Min.x, bar.Max.y - 1.0f), bar.Max,
        ImGui::GetColorU32(ImGuiCol_Separator));
    return true;
}

// BeginTabItem with the tab look; EndTabItem as usual.
bool TabItem(const char* label, bool marked = false)
{
    // The label colour is set before ImGui draws it: white for the tab shown this
    // frame (its pending selection before the bar's layout) or hovered last frame.
    const ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
    const ImGuiID id = ImGui::GetID(label);
    const ImGuiID shown = bar->NextSelectedTabId ? bar->NextSelectedTabId : bar->VisibleTabId;
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, shown == id || ImGui::GetCurrentContext()->HoveredIdPreviousFrame == id
        ? text : ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text, 0.25f));
    PushTabStyle();
    const bool open = ImGui::BeginTabItem(label);
    PopTabStyle();
    ImGui::PopStyleColor();
    // marked: a dot in the tab's corner (the menu's fonts may lack a bullet); a new tab has no rect on its first frame.
    if (marked && ImGui::GetItemRectSize().x > 0.0f)
    {
        const float radius = ImMax(1.0f, IM_ROUND(ImGui::GetFontSize() * 0.12f));
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetItemRectMax().x - TabPadding().x * 0.5f,
            ImGui::GetItemRectMin().y + TabPadding().y + radius), radius, ImGui::GetColorU32(ImGuiCol_CheckMark));
    }
    if (open)
    {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float thickness = IM_ROUND(ImGui::GetFontSize() * 0.12f);
        const float inset = IM_ROUND(TabPadding().x * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x + inset, max.y - thickness),
            ImVec2(max.x - inset, max.y), ImGui::GetColorU32(ImGuiCol_CheckMark), thickness * 0.5f);
    }
    return open;
}

// Controls stay usable-looking while status is stale, so an open popup keeps
// its appearance across a missed read. Item-level disables inside still dim.
void BeginStaleDisabled(bool stale)
{
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(stale);
    ImGui::PopStyleVar();
}

// Label | value grid shared by controls and status rows. The label column fits
// the widest label drawn last frame (translations differ in length): at least
// 8 em, at most 14 em, the same in every grid so the columns line up.
float gLabelWidth = 0.0f;     // widest label of the previous frame, in pixels
float gLabelWidthSeen = 0.0f; // widest label so far this frame

bool BeginGrid(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_NoSavedSettings)) return false;
    const float font = ImGui::GetFontSize();
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
        std::clamp(gLabelWidth, font * 8.0f, font * 14.0f));
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void RowLabel(const char* label, bool control)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    gLabelWidthSeen = std::max(gLabelWidthSeen, ImGui::CalcTextSize(label).x);
    if (control) ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

// Controls share one width, leaving room for a (?) marker.
void ControlRow(const char* label)
{
    RowLabel(label, true);
    const float font = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(std::max(font * 4.0f,
        std::min(font * 13.0f, ImGui::GetContentRegionAvail().x - font * 1.8f)));
}

void StatusRow(const char* label, Tone tone, const char* format, ...)
{
    RowLabel(label, false);
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// A short coloured value and a dim detail, e.g. "Active  tracing converted hair".
void StatusDetail(const char* label, Tone tone, const char* value, const char* detail)
{
    RowLabel(label, false);
    ImGui::TextColored(ToneColor(tone), "%s", value);
    if (detail && detail[0])
    {
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", detail);
        ImGui::PopTextWrapPos();
    }
}

// Visible notes are for states the user acts on or waits for; explanations
// go into (?) markers.
void Note(Tone tone, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(tone));
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

std::string Grouped(uint64_t value)
{
    std::string digits = std::to_string(value);
    for (ptrdiff_t at = static_cast<ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
        digits.insert(static_cast<size_t>(at), 1, ',');
    return digits;
}

// The overlay draws this beside the menu key, after the panel. English; translated when drawn.
const char* gFooterStatus = "";
Tone gFooterTone = Tone::Dim;

void SetFooter(const char* text, Tone tone)
{
    gFooterStatus = text;
    gFooterTone = tone;
}

// Pinned FPS readout: the overlay draws it in the top-left corner while the
// menu is closed. Toggled beside the FPS row; [Overlay] PinnedFps.
std::atomic<bool> gPinnedFps{false};

// keybinds.1: also the Keybinds tab's FPS readout key.
void SetPinnedFps(bool pinned)
{
    gPinnedFps.store(pinned, std::memory_order_release);
    const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    if (!path.empty()) WritePrivateProfileStringW(L"Overlay", L"PinnedFps", pinned ? L"1" : L"0", path.c_str());
}

// `tooltip` names the frame rates the readout shows (DLSS's by default, XeMFG's beside XeMFG's row).
void PinnedFpsToggle(const char* tooltip = nullptr)
{
    ImGui::SameLine();
    // No vertical frame padding, so the checkbox keeps the status row's height.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    bool pinned = gPinnedFps.load(std::memory_order_acquire);
    if (ImGui::Checkbox(l10n::Label("Pin"), &pinned)) SetPinnedFps(pinned);
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("%s", tooltip ? tooltip : T("Shows the real and DLSS frame rates in the top-left corner\n"
        "while the menu is closed. Saved for the next launch."));
}

// latmon.2: the Latency row below the FPS row, with its own pin ([Overlay] PinnedLatency): pinned, it sits below the
// pinned FPS readout (or in its place) while the menu is closed. Both read latency_probe::ReadLive (every 100 ms).
std::atomic<bool> gPinnedLatency{false};
void SetPinnedLatency(bool pinned)
{
    gPinnedLatency.store(pinned, std::memory_order_release);
    const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    if (!path.empty()) WritePrivateProfileStringW(L"Overlay", L"PinnedLatency", pinned ? L"1" : L"0", path.c_str());
}
void PinnedLatencyToggle()
{
    ImGui::SameLine();
    ImGui::PushID("latency"); // the FPS row's checkbox is also "Pin"
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    bool pinned = gPinnedLatency.load(std::memory_order_acquire);
    if (ImGui::Checkbox(l10n::Label("Pin"), &pinned)) SetPinnedLatency(pinned);
    ImGui::PopStyleVar();
    ImGui::SetItemTooltip("%s", T("Shows the PC latency in the top-left corner\n"
        "while the menu is closed. Saved for the next launch."));
    ImGui::PopID();
}

// latency_monitor::State as the status file gives it.
enum LatencyState : uint32_t
{
    kLatencyOff = 0, kLatencyMeasuring = 1, kLatencyLive = 2, kLatencyNoMarkers = 3, kLatencyNoOutputs = 4,
    kLatencyNoFlipTimes = 5,
};
bool LatencyCurrent(const latency_probe::Live& live)
{
    // latmon.4: the estimator holds the readout over its last 5 s through a gap; up to 6 s since the newest frame.
    return live.state == kLatencyLive && live.meanUs > 0 && live.ageMs <= 6000;
}

void LatencyStatusRow()
{
    const latency_probe::Live live = latency_probe::ReadLive();
    if (!live.available) return; // [Overlay] LatencyMonitor=0
    const char* label = T("Latency");
    if (LatencyCurrent(live))
    {
        StatusRow(label, Tone::Normal, T("%.1f ms, 1%% high %.1f ms"), live.meanUs / 1000.0, live.p99Us / 1000.0);
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted(T("PC latency: from the game starting a frame (its Reflex simulation start)\n"
                "to the first frame showing it reaching the screen."));
            ImGui::Text(T("Game %.1f ms, frame generation %.1f ms, display %.1f ms."), live.gameUs / 1000.0,
                live.fgUs / 1000.0, live.displayUs / 1000.0);
            if (live.realUs)
                ImGui::Text(T("The real frame reaches the screen after %.1f ms."), live.realUs / 1000.0);
            ImGui::TextDisabled("%s", T("Mouse and monitor delay are not included."));
            ImGui::EndTooltip();
        }
    }
    else if (live.state == kLatencyNoMarkers)
        StatusDetail(label, Tone::Pending, T("Unavailable"), T("the game sends no Reflex markers"));
    else if (live.state == kLatencyNoOutputs)
        StatusDetail(label, Tone::Pending, T("Unavailable"), T("no frames to time on this route"));
    else if (live.state == kLatencyNoFlipTimes)
        StatusDetail(label, Tone::Pending, T("Unavailable"), T("Windows reports no flip times here"));
    else
        StatusRow(label, Tone::Dim, "%s", T("Measuring..."));
    PinnedLatencyToggle();
}

const char* DlssgPresetName(uint32_t preset) noexcept
{
    switch (preset)
    {
    case 0: return "Game / driver";
    case 1: return "A";
    case 2: return "B";
    case 3: return "C";
    default: return "Unknown";
    }
}

const char* const kDlssgPresetOptions[]{N_("Game / driver default"), "A", "B", N_("Preset C (performance)")};

const char* const kDlssgPresetHelp = N_("The DLSS frame generation model preset.\n"
    "Preset C is NVIDIA's lighter Frame Generation mode. It needs DLSS-G 310.9.16 or newer,\n"
    "from the NVIDIA app's DLSS override; other providers keep the game / driver preset.\n"
    "A change made while frame generation runs restarts it once, with a short pause;\n"
    "some older DLSS-G versions apply it after a game restart.");
// The DLSS-G preset beside OptiScaler, whose DLSSG output has no preset of its own. A change restarts OptiScaler's
// frame generation like the game's; with it off, its next start reads the preset, which these notes say.
const char* const kOptiDlssgPresetSaved = N_("Preset %s is saved. It applies the next time frame generation starts, "
    "or after a game restart.");
const char* const kOptiDlssgGameSaved = N_("The game / driver preset is saved. It applies the next time frame "
    "generation starts, or after a game restart.");

// A DLSS-G provider is in use (selected, or it has read the preset).
bool DlssgPresetShown(const MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    return snapshot.activeProviderVersionMajor != 0 || snapshot.dlssgPresetReadCount > 0;
}

// The note for a saved preset that frame generation has not picked up yet; nullptr when none. A live switch restarts
// frame generation once; otherwise its next start (said only beside OptiScaler) or a game restart applies it.
const char* DlssgPresetPendingNote(const MfgUnlockReShadeSnapshot& snapshot, bool besideOptiScaler, char* text,
    size_t size) noexcept
{
    const bool pending = snapshot.dlssgPresetSelectionFrozen
        && snapshot.dlssgPresetRequested != snapshot.dlssgPresetLatched;
    const bool live = snapshot.dlssgPresetLiveSwitch == 0;
    if (snapshot.dlssgPresetSwitching || (pending && live && snapshot.gameFrameGenerationOn))
        return T("Restarting frame generation with the new preset...");
    if (!pending || (live && !besideOptiScaler)) return nullptr;
    if (snapshot.dlssgPresetRequested == 0)
        return live ? T(kOptiDlssgGameSaved) : T("The game / driver preset is saved. Restart the game to use it.");
    snprintf(text, size, live ? T(kOptiDlssgPresetSaved) : T("Preset %s is saved. Restart the game to use it."),
        DlssgPresetName(snapshot.dlssgPresetRequested));
    return text;
}

// Preset readback for the Status grid.
void PresetStatusRow(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == 1)
            StatusDetail(T("DLSS-G preset"), Tone::Good, T("Original"), T("observed"));
        else
            StatusDetail(T("DLSS-G preset"), Tone::Pending, T("Original"), T("waiting for the provider"));
        return;
    }
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    char value[32]{};
    snprintf(value, sizeof(value), T("Preset %s"), DlssgPresetName(activePreset));
    if (activePreset == 0)
        StatusDetail(T("DLSS-G preset"), Tone::Normal, T("Game / driver"), T("managed by the game or driver"));
    else if (activePreset == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        StatusDetail(T("DLSS-G preset"), Tone::Pending, value, T("needs DLSS-G 310.9.16; game / driver in use"));
    else if (snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == activePreset
        && snapshot.dlssgPresetOverrideReadCount > 0)
        StatusDetail(T("DLSS-G preset"), Tone::Good, value, T("override supplied"));
    else if (snapshot.dlssgPresetOverrideFailure != 0)
        StatusDetail(T("DLSS-G preset"), Tone::Bad, value, T("override unavailable"));
    else
        StatusDetail(T("DLSS-G preset"), Tone::Pending, value, T("waiting for the provider"));
}

// Preset messages the user acts on.
void PresetNotes(const MfgUnlockReShadeSnapshot& snapshot, bool besideOptiScaler = false)
{
    char pending[256]{};
    if (UiAmpere() && snapshot.ampereLegacySinglePreset)
    {
        Note(Tone::Dim, "%s", snapshot.dlssgPresetRequested == 2
            ? T("This DLSS-G provider supports its original preset only. Preset B stays saved for providers that support it.")
            : T("This DLSS-G provider supports its original preset only."));
    }
    else if (const char* note = DlssgPresetPendingNote(snapshot, besideOptiScaler, pending, sizeof(pending)))
        Note(Tone::Pending, "%s", note);
    const uint32_t activePreset = snapshot.dlssgPresetSelectionFrozen
        ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    if (!(UiAmpere() && snapshot.ampereLegacySinglePreset) && activePreset == 3
        && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        Note(Tone::Dim, T("Preset C needs DLSS-G 310.9.16 or newer (NVIDIA app). This game's"
            " frame generation keeps the game / driver preset."));
    if (gPresetSaveFailed)
        Note(Tone::Bad, T("The preset selection could not be saved."));
}

// Combo text of a preset option: a letter reads as "Preset <letter>", other
// options are msgids. UI thread; the text is valid until the next call.
const char* PresetOption(const char* option) noexcept
{
    static char label[32]{};
    if (option[0] == '\0' || option[1] != '\0') return T(option);
    snprintf(label, sizeof(label), T("Preset %s"), option);
    return label;
}

// Preset control row. Updates snapshot.dlssgPresetRequested to the selection
// for the status row.
void PresetControlRow(MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    // Preset C needs DLSS-G 310.9.16 or newer (RTX 40 and, since 931, the RTX 30 port).
    const int maximumPreset = 3;
    if (!statusStale && snapshot.dlssgPresetRequested <= 3)
        gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
    const int previous = gDlssgPreset;
    bool changed = false;
    const auto& options = kDlssgPresetOptions;
    const bool legacyPreset = UiAmpere() && snapshot.ampereLegacySinglePreset;
    ControlRow(T("DLSS-G preset"));
    ImGui::BeginDisabled(legacyPreset);
    if (ImGui::BeginCombo("##dlssgpreset", legacyPreset ? T("Original preset") : PresetOption(options[std::clamp(gDlssgPreset, 0, 3)])))
    {
        for (int preset = 0; preset <= maximumPreset; ++preset)
        {
            if (ImGui::Selectable(PresetOption(options[preset]), gDlssgPreset == preset))
            {
                gDlssgPreset = preset;
                changed = preset != previous;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    HelpMarker(T(kDlssgPresetHelp));
    if (changed)
    {
        const BOOL saved = NativeFilePersistPreset("dlssgPreset", static_cast<uint32_t>(gDlssgPreset));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresetSaveFailed = !saved;
        if (!saved) gDlssgPreset = previous;
    }
    snapshot.dlssgPresetRequested = static_cast<uint32_t>(gDlssgPreset);
}

// DLSS Super Resolution and Ray Reconstruction. The game runs
// one or the other, so one section shows the controls of the running feature.
const char* SrPresetLetter(uint32_t preset) noexcept
{
    static constexpr const char* kLetters[]{"", "A", "B", "C", "D", "E", "F", "G",
        "H", "I", "J", "K", "L", "M", "N", "O"};
    return preset < std::size(kLetters) ? kLetters[preset] : "";
}

// A render at the output size is DLAA whatever mode the game created it with: MSFS 2024
// creates its DLAA as Quality (GitHub issue 112).
const char* SrQualityName(const MfgUnlockDlssFeature& view) noexcept
{
    if (view.renderWidth && view.renderWidth == view.outputWidth && view.renderHeight == view.outputHeight)
        return "DLAA";
    switch (view.quality)
    {
    case 0: return T("Performance");
    case 1: return T("Balanced");
    case 2: return T("Quality");
    case 3: return T("Ultra Performance");
    case 4: return T("Ultra Quality");
    case 5: return "DLAA";
    default: return "";
    }
}

// Ray Reconstruction while it runs; otherwise the feature the game still has.
bool RayReconstructionShown(const MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    if (snapshot.dlssRr.running) return true;
    if (snapshot.dlssSr.running) return false;
    if (snapshot.dlssRr.liveFeatures || snapshot.dlssSr.liveFeatures)
        return snapshot.dlssRr.liveFeatures && !snapshot.dlssSr.liveFeatures;
    return snapshot.dlssRr.detected && !snapshot.dlssSr.detected;
}

// Whether the feature runs, and the preset of the running feature and its source.
// Status grid rows, labelled like the preset control.
void DlssStatusRows(const MfgUnlockDlssFeature& view, bool rr)
{
    const char* label = rr ? T("DLSS RR") : T("DLSS SR");
    const char* presetLabel = rr ? T("RR preset") : T("SR preset");
    char value[64]{}, detail[160]{};
    if (view.running)
    {
        snprintf(detail, sizeof(detail), "%s, %ux%u -> %ux%u", SrQualityName(view),
            view.renderWidth, view.renderHeight, view.outputWidth, view.outputHeight);
        StatusDetail(label, Tone::Good, T("Running"), detail);
    }
    else if (view.detected)
        StatusDetail(label, Tone::Dim, T("Idle"), T("not evaluated in the last second"));
    else
    {
        StatusDetail(label, Tone::Dim, T("Not detected"), T("waiting for the game to create DLSS"));
        return;
    }
    if (view.fixedByDriver || (view.activePreset == 0 && view.driverOverrideCached))
    {
        // The provider cached the NVIDIA app's override at its initialization.
        if (view.driverPreset == dlss_sr::kDrsLatest)
            snprintf(detail, sizeof(detail), "%s", T("NVIDIA app override: latest"));
        else
            snprintf(detail, sizeof(detail), T("NVIDIA app override: %s"), SrPresetLetter(view.driverPreset));
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), detail);
    }
    else if (view.activePreset != 0)
    {
        // Seen at the provider's DRS read: nothing outranks the creation hints.
        snprintf(value, sizeof(value), T("Preset %s"), SrPresetLetter(view.activePreset));
        StatusDetail(presetLabel, view.drsObserved ? Tone::Good : Tone::Normal, value,
            view.drsObserved ? T("override supplied") : T("sent as the game's preset hint"));
    }
    else if (view.gameHint != 0)
    {
        snprintf(detail, sizeof(detail), T("game hint: %s"), SrPresetLetter(view.gameHint));
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), detail);
    }
    else
        StatusDetail(presetLabel, Tone::Normal, T("Game / driver"), T("DLSS default for this mode"));
}

// The preset choices of DLSS SR and RR, in menu order (shared with OptiScaler's tab and the preset keys).
constexpr uint32_t kSrPresetValues[]{dlss_sr::kGameOrDriver, dlss_sr::kPresetJ, dlss_sr::kPresetK,
    dlss_sr::kPresetL, dlss_sr::kPresetM};
const char* const kSrPresetOptions[]{N_("Game / driver default"), "J", "K", "L", "M"};
constexpr uint32_t kRrPresetValues[]{dlss_sr::kGameOrDriver, dlss_sr::kRrPresetD, dlss_sr::kRrPresetE,
    dlss_sr::kRrPresetF};
const char* const kRrPresetOptions[]{N_("Game / driver default"), "D", "E", "F"};

const char* DlssPresetHelp(bool rr) noexcept
{
    if (rr)
        return T("The DLSS Ray Reconstruction model preset.\n"
            "D is NVIDIA's transformer model; E is the newer transformer model, which games with a\n"
            "depth-of-field guide need. F is NVIDIA's second-generation model (RR2) and needs driver 580\n"
            "or newer; DLSS versions without a preset use their default.\n"
            "A change applies at once while DLSS runs. A preset selected when the game starts also\n"
            "replaces an NVIDIA app preset override for that session.");
    return T("The DLSS Super Resolution model preset.\n"
        "K is NVIDIA's transformer model with the best image quality; J is similar with a little\n"
        "less ghosting and more flicker. L and M are the newer transformer presets NVIDIA uses for\n"
        "Ultra Performance and Performance; DLSS versions without them use their default.\n"
        "A change applies at once while DLSS runs. A preset selected when the game starts also\n"
        "replaces an NVIDIA app preset override for that session.");
}

void DlssPresetControlRow(const MfgUnlockDlssFeature& view, bool rr, bool statusStale)
{
    uint32_t& selected = rr ? gDlssRrPreset : gDlssSrPreset;
    const uint32_t* values = rr ? kRrPresetValues : kSrPresetValues;
    const char* const* options = rr ? kRrPresetOptions : kSrPresetOptions;
    const size_t count = rr ? std::size(kRrPresetValues) : std::size(kSrPresetValues);
    const bool valid = rr ? dlss_sr::IsValidRrSelection(view.presetRequested) : dlss_sr::IsValidSelection(view.presetRequested);
    if (!statusStale && valid) selected = view.presetRequested;
    size_t current = 0;
    for (size_t i = 0; i != count; ++i)
        if (values[i] == selected) current = i;
    const uint32_t previous = selected;
    ControlRow(rr ? T("RR preset") : T("SR preset"));
    if (ImGui::BeginCombo(rr ? "##dlssrrpreset" : "##dlsssrpreset", PresetOption(options[current])))
    {
        for (size_t i = 0; i != count; ++i)
            if (ImGui::Selectable(PresetOption(options[i]), i == current)) selected = values[i];
        ImGui::EndCombo();
    }
    HelpMarker(DlssPresetHelp(rr));
    if (selected != previous)
    {
        const BOOL saved = NativeFilePersistPreset(rr ? "dlssRrPreset" : "dlssSrPreset", selected);
        (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed) = !saved;
        if (!saved) selected = previous;
    }
}

void DlssNotes(const MfgUnlockDlssFeature& view, bool rr)
{
    const uint32_t selected = rr ? gDlssRrPreset : gDlssSrPreset;
    if (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed)
        Note(Tone::Bad, "%s", rr ? T("The DLSS RR preset could not be saved.") : T("The DLSS SR preset could not be saved."));
    if (!view.detected) return;
    if (view.fixedByDriver)
    {
        if (selected != dlss_sr::kGameOrDriver)
            Note(Tone::Pending, "%s", T("The NVIDIA app's DLSS preset override is active in this game session. "
                "Restart the game with a preset selected here to switch presets live."));
        return;
    }
    if (view.drsSuppressed && view.driverPresetValid && view.driverPreset != 0
        && selected == dlss_sr::kGameOrDriver)
        Note(Tone::Dim, "%s", T("The NVIDIA app's DLSS preset override is replaced in this session. "
            "It applies again after a restart with Game / driver selected."));
    if (!view.drsObserved && selected != dlss_sr::kGameOrDriver)
        Note(Tone::Dim, "%s", T("The NVIDIA app's DLSS preset override could not be checked in this session. "
            "If one is set, it takes priority."));
    if (!view.running || selected == view.activePreset) return;
    switch (view.liveSwitch)
    {
    case 1:
        Note(Tone::Pending, "%s", T("This game's DLSS cannot change its preset while running. The preset applies "
            "the next time the game starts DLSS (change the DLSS quality mode or restart the game)."));
        break;
    case 2:
        Note(Tone::Bad, "%s", T("DLSS refused the new preset; the previous one stays in use."));
        break;
    case 3:
        Note(Tone::Pending, "%s", T("Waiting to release the previous DLSS preset..."));
        break;
    default:
        Note(Tone::Pending, "%s", rr ? T("Switching DLSS Ray Reconstruction to the new preset...")
            : T("Switching DLSS Super Resolution to the new preset..."));
        break;
    }
}

// A combo over labels[0..count); disabledMask greys out choices.
bool ChoiceCombo(const char* id,uint32_t& value,const char* const* labels,uint32_t count,uint32_t disabledMask=0)
{
    bool changed=false;
    if(ImGui::BeginCombo(id,labels[value<count?value:0])){
        for(uint32_t i=0;i<count;++i){
            ImGui::BeginDisabled((disabledMask>>i)&1u);
            if(ImGui::Selectable(labels[i],value==i) && value!=i){value=i;changed=true;}
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A slider edit saves while dragging at most every 150 ms, and on release.
bool Dlss5SliderSave(bool edited)
{
    if(edited)gDlss5Dirty=true;
    return gDlss5Dirty && (ImGui::IsItemDeactivatedAfterEdit() || GetTickCount64()-gDlss5LastSave>=150);
}

// nrscale.1: a slider whose value replaces the feature (Resolution Scale, Pass Count) saves once, on release: each save
// creates the network anew on the game's thread (up to 1.6 s at 4K).
bool Dlss5ReleaseSave(bool edited)
{
    if(edited)gDlss5Held=true;
    if(!gDlss5Held || !ImGui::IsItemDeactivatedAfterEdit())return false;
    gDlss5Held=false;
    return true;
}

// dlss5tab.1: the width for the slider or list of a row that holds more than it (`beside`: the width of the rest). The
// row is as wide as the other rows unless that leaves the item under 7 em, and stops short of the (?) marker's room.
float Dlss5ItemWidth(float beside)
{
    const float font=ImGui::GetFontSize();
    const float room=ImGui::GetContentRegionAvail().x-font*1.8f;
    return std::max(font*4.0f,std::min(std::max(font*13.0f,beside+font*7.0f),room)-beside);
}

// A tooltip on the control just drawn, for a control sharing a row with the one the (?) marker explains.
void Dlss5ItemHelp(const char* text)
{
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",text);
}

// dlss5tab.1: a short choice as buttons side by side, sharing the control's width: one click, and every option in view.
bool SegmentedChoice(const char* id,uint32_t& value,const char* const* labels,uint32_t count)
{
    const float spacing=ImGui::GetStyle().ItemInnerSpacing.x;
    const float width=(ImGui::CalcItemWidth()-spacing*static_cast<float>(count-1))/static_cast<float>(count);
    bool changed=false;
    ImGui::PushID(id);
    for(uint32_t i=0;i!=count;++i){
        if(i)ImGui::SameLine(0.0f,spacing);
        const bool on=(value<count?value:0u)==i;
        if(on)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushID(static_cast<int>(i));
        if(ImGui::Button(labels[i],ImVec2(width,0.0f)) && value!=i){value=i;changed=true;}
        ImGui::PopID();
        if(on)ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

// Faces & skin: a slider and a Same as Structure checkbox on one row. Ticked, the strength is -1 (the provider takes
// Structure's, its own default) and the slider shows that value, so unticking jumps nowhere. maskBox: Character Mask.
bool FacesControl(const char* id,float& character,float structure,bool& mask,bool maskBox=false)
{
    const ImGuiStyle& style=ImGui::GetStyle();
    const auto ticked=[&](const char* text){return ImGui::GetFrameHeight()+style.ItemInnerSpacing.x+ImGui::CalcTextSize(text).x+style.ItemSpacing.x;};
    const float box=ticked(T("Same as Structure"))+(maskBox?ticked(T("Mask")):0.0f);
    const bool follows=character<0.0f;
    bool save=false;
    ImGui::PushID(id);
    ImGui::BeginDisabled(!mask);
    ImGui::BeginDisabled(follows);
    float shown=structure;
    ImGui::SetNextItemWidth(Dlss5ItemWidth(box));
    save|=Dlss5SliderSave(ImGui::SliderFloat("##strength",follows?&shown:&character,0.0f,dlss5::kMaxStrength,"%.2f"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool same=follows;
    if(ImGui::Checkbox(l10n::Label("Same as Structure"),&same)){character=same?dlss5::kMinCharacter:structure;save=true;}
    ImGui::EndDisabled();
    if(maskBox){
        ImGui::SameLine();
        save|=ImGui::Checkbox(l10n::Label("Mask"),&mask);
        Dlss5ItemHelp(T("Detects skin and faces so that Faces & skin applies to them."));
    }
    ImGui::PopID();
    return save;
}

// Hold to show original: the key's name, Change key (the next key pressed
// binds it, Esc cancels) and Clear. Keys are read below other mods' input
// hooks, so binding also works while OptiScaler's menu is open.
bool gCompareBinding=false;
bool gCompareHeld[256]{};
// keyColumn: the width the key's name takes before the buttons (keybinds.1's Keybinds tab lines them up); 0 none.
// While binding: the next newly pressed allowed key binds, Esc cancels. True when a key was bound.
bool PollCompareBinding(dlss5::Settings& s)
{
    bool changed=false;
    if(gCompareBinding){
        if(PhysicalKeyDown(VK_ESCAPE))gCompareBinding=false;
        for(int key=0x05;key<0xFF && gCompareBinding;++key){
            const bool down=PhysicalKeyDown(key);
            if(down && !gCompareHeld[key] && dlss5::CompareKeyAllowed(static_cast<uint32_t>(key))){
                s.compareKey=static_cast<uint32_t>(key);gCompareBinding=false;changed=true;
            }
            gCompareHeld[key]=down;
        }
    }
    return changed;
}
// Keys already down (the one that activated the button) do not bind.
void BeginCompareBinding()
{
    gCompareBinding=true;
    for(int key=0;key<256;++key)gCompareHeld[key]=PhysicalKeyDown(key);
}
bool CompareKeyControl(dlss5::Settings& s,float keyColumn=0.0f)
{
    const bool changed=PollCompareBinding(s);
    if(gCompareBinding){
        ImGui::TextUnformatted(T("Press a key... Esc cancels"));
        if(keyColumn>ImGui::GetItemRectSize().x){ImGui::SameLine(0.0f,0.0f);ImGui::Dummy(ImVec2(keyColumn-ImGui::GetItemRectSize().x,0.0f));}
        ImGui::SameLine();
        if(ImGui::Button(l10n::Label("Cancel")))gCompareBinding=false;
        return changed;
    }
    if(s.compareKey)ImGui::TextUnformatted(KeyName(static_cast<int>(s.compareKey)).c_str());
    else ImGui::TextDisabled("%s",T("Not set"));
    if(keyColumn>ImGui::GetItemRectSize().x){ImGui::SameLine(0.0f,0.0f);ImGui::Dummy(ImVec2(keyColumn-ImGui::GetItemRectSize().x,0.0f));}
    ImGui::SameLine();
    if(ImGui::Button(l10n::Label("Change key")))BeginCompareBinding();
    if(s.compareKey){
        ImGui::SameLine();
        if(ImGui::Button(l10n::Label("Clear"))){s.compareKey=0;return true;}
    }
    return changed;
}

const char* Dlss5StateName(uint32_t state,Tone& tone)
{
    switch(static_cast<dlss5::State>(state)){
    case dlss5::State::eOff:tone=Tone::Dim;return T("Off");
    case dlss5::State::eRunning:tone=Tone::Good;return T("Running");
    case dlss5::State::eNotInstalled:tone=Tone::Dim;return T("Provider not found");
    case dlss5::State::eUnsupported:tone=Tone::Dim;return T("GPU or provider unsupported");
    case dlss5::State::eDriverUpdate:tone=Tone::Bad;return T("Driver update required");
    case dlss5::State::eCreateFailed:case dlss5::State::eEvaluateFailed:tone=Tone::Bad;return T("Provider failed");
    case dlss5::State::eOtherMod:tone=Tone::Dim;return T("Provider already in use");
    case dlss5::State::eMissingInputs:tone=Tone::Dim;return T("DLSS inputs unavailable");
    case dlss5::State::eStageUnavailable:tone=Tone::Dim;return T("Hook method unavailable");
    default:tone=Tone::Pending;return T("Waiting for DLSS");
    }
}

// dx11nr.1: why the DirectX 11 bridge leaves the frame as is (status dlss5D3D11Refusal), or null.
const char* Dlss5D3D11RefusalText(uint32_t refusal)
{
    switch(static_cast<dlss5::d3d11::Refusal>(refusal)){
    case dlss5::d3d11::Refusal::eDeferredContext:
        return T("This game evaluates DLSS on a deferred DirectX 11 context. DLSS 5 needs the immediate context, so it leaves the frame as is.");
    case dlss5::d3d11::Refusal::eNoSharedFence:
        return T("DLSS 5 on DirectX 11 needs shared fences (Windows 10 version 1703 or later and a current driver). It leaves the frame as is.");
    case dlss5::d3d11::Refusal::eDevice:
        return T("DLSS 5 could not create its DirectX 12 device for this DirectX 11 game; the log names the reason.");
    case dlss5::d3d11::Refusal::eSharing:
        return T("DLSS 5 could not share textures between this DirectX 11 game and its DirectX 12 device; the log names the reason.");
    case dlss5::d3d11::Refusal::eUntracked:
        return T("DLSS 5 cannot attach to this game's DirectX 11 DLSS: MFG Unlocker did not see it being created (it came before the hook, or another hook covers NVIDIA's entries). The log names the reason.");
    default:return nullptr;
    }
}

// The DLSS 5 tab shows only while its provider (nvngx_dlssnr.dll) sits beside the game or MFG Unlocker,
// or while DLSS 5 runs from one NGX found elsewhere. Saved settings stay while it is hidden.
bool Dlss5TabShown(const MfgUnlockReShadeSnapshot& snapshot) noexcept
{
    return snapshot.dlss5Detected || snapshot.dlss5Route;
}

const char* Dlss5StageHelp() noexcept
{
    return T("Auto processes the DLSS output after reconstruction; Resolution Scale Auto keeps its cost in check.\n"
        "Before upscaling processes the DLSS input at render resolution, before DLSS SR, DLAA or Ray Reconstruction.\n"
        "After upscaling processes the DLSS output after reconstruction.\n"
        "Present processes the final image, game UI included, before presentation and frame generation.");
}

const char* Dlss5PresetsHelp() noexcept
{
    return T("Subtle, Balanced and Strong set Intensity, Style, Structure, Tone and Faces & skin together.\n"
        "Balanced is the provider's own default, Subtle halves its change and Strong goes further.\n"
        "Pass Count under Advanced strengthens any look further, at extra GPU cost.\n"
        "Your saved presets are listed below them. Each loads its look, Hook Method and every other setting; Compare and Debug view stay as they are.\n"
        "Loading a saved preset turns DLSS 5 on. Delete removes the saved preset shown.");
}

// The provider's DLSSNR.Style values, named by what they do (nrnames.1, provider harness on 310.9.16).
const char* Dlss5StyleHelp() noexcept
{
    return T("How the network draws its look. All three cost the same.\n"
        "Neutral changes the game's image the least.\n"
        "Rich keeps skin warm and colourful, and deepens bright areas such as sky.\n"
        "Soft gives a flatter, softer image.");
}

// DLSSNR.Hint.Render.Preset: 0 the provider's choice, 1 its larger network, 2 its smaller one (310.9.16's default).
const char* Dlss5NetworkHelp() noexcept
{
    return T("Which of the provider's two networks runs.\n"
        "Default lets the provider choose: DLSS 5 310.9.16 runs Fast.\n"
        "Quality is the larger network: stronger changes and a little more fine detail, at about 1.7x the GPU time.\n"
        "Fast is the smaller network.");
}

const char* Dlss5FacesHelp(bool characterMask) noexcept
{
    return !characterMask ? T("Off while Character Mask is off.")
        : T("Structure enhancement on detected skin and faces.\nSame as Structure takes the Structure strength, the provider's own default.");
}

const char* Dlss5ScaleHelp() noexcept
{
    return T("Share of the hook method's resolution that the network processes.\n"
        "100 is native, lower is faster, up to 200 supersamples. Values below 25 use 25.\n"
        "Auto lowers it when DLSS 5 takes more than about a third of each rendered frame, not below 75, and raises it again with headroom, never above the slider.");
}

const char* Dlss5HalfRateHelp(bool unavailable) noexcept
{
    return unavailable ? T("Half rate is not available on Vulkan or with the Present hook method yet.")
        : T("Runs the network on every other frame. The frames between apply its last look to their own image, so moving and animated surfaces keep it without trails.\n"
            "About half the GPU cost. Fine detail follows the game's motion vectors where a surface only moved.");
}

// dlss5presets.1: DLSS 5 presets (dlss5_presets.h) of both menus, saved in MFG_PRODUCT "-Dlss5Presets.txt" beside the
// game and read again before each change. Render-thread state, under gUiMutex.
std::vector<dlss5::presets::Entry> gDlss5Presets;
bool gDlss5PresetsRead=false;
// The preset last loaded, saved or imported: the tab shows it, marked once the settings change.
std::string gDlss5PresetSelected;
char gDlss5PresetName[dlss5::presets::kMaxName+1]{};
// A pasted code; a chat message around it fits too.
char gDlss5PresetCode[1024]{};
enum class PresetNote:uint8_t{eNone,eSaved,eDeleted,eImported,eCopied,eCopyFailed,eNoCode,eDamaged,eNewer,eFull,eFileFailed};
PresetNote gDlss5PresetNote=PresetNote::eNone;
std::string gDlss5PresetNoteName;
uint64_t gDlss5PresetNoteAt=0;
// The last import left out settings that only a newer build knows.
bool gDlss5PresetSkipped=false;
// dlss5tab.1: the row under Presets that Save... or Share... opens, one at a time; it closes once its action went through.
enum class PresetRow:uint8_t{eNone,eSave,eShare};
PresetRow gDlss5PresetRow=PresetRow::eNone;
void TogglePresetRow(PresetRow row){gDlss5PresetRow=gDlss5PresetRow==row?PresetRow::eNone:row;}
// "Save..." and "Share...", the words the rows already use, with an id of their own.
const char* PresetRowLabel(PresetRow row,char* buffer,size_t size)
{
    const bool saving=row==PresetRow::eSave;
    snprintf(buffer,size,"%s...##%s",saving?T("Save"):T("Share"),saving?"dlss5presetsaverow":"dlss5presetsharerow");
    return buffer;
}

void SetPresetNote(PresetNote note,std::string name={},bool skipped=false)
{
    gDlss5PresetNote=note;gDlss5PresetNoteName=std::move(name);gDlss5PresetSkipped=skipped;
    gDlss5PresetNoteAt=GetTickCount64();
}

void CopyPresetName(const std::string& name)
{
    snprintf(gDlss5PresetName,sizeof(gDlss5PresetName),"%s",name.c_str());
}

std::wstring Dlss5PresetsPath()
{
    return standalone_ui::SettingsPath(MFG_PRODUCT "-Dlss5Presets.txt");
}

// False when the file is there but cannot be read: the list stays as it was and nothing is written over the file.
bool ReadDlss5Presets()
{
    const std::wstring path=Dlss5PresetsPath();
    if(path.empty())return false;
    std::string content;
    if(ReadTextFile(path,content)){gDlss5Presets=dlss5::presets::ParseStore(content);gDlss5PresetsRead=true;return true;}
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data)){
        // ReadTextFile refuses empty files and files over 1 MB; only an empty one is an empty list.
        if(data.nFileSizeHigh || data.nFileSizeLow)return false;
    }else{
        const DWORD error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)return false;
    }
    gDlss5Presets.clear();gDlss5PresetsRead=true;
    return true;
}

// Written whole beside the old file, then moved over it, as WriteNativeControl does.
bool WriteDlss5Presets()
{
    const std::wstring path=Dlss5PresetsPath();
    if(path.empty())return false;
    const std::string content=dlss5::presets::WriteStore(gDlss5Presets);
    const std::wstring temporary=path+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;
    const bool flushed=WriteFile(file,content.data(),static_cast<DWORD>(content.size()),&written,nullptr)
        && written==content.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(!flushed || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Opened for the game's window when it is in front, else for no window, as Dear ImGui's Win32 clipboard functions do.
bool OpenTextClipboard()
{
    HWND owner=GetForegroundWindow();
    DWORD process=0;
    if(owner)GetWindowThreadProcessId(owner,&process);
    if(process!=GetCurrentProcessId())owner=nullptr;
    for(int attempt=0;attempt!=4;++attempt){
        if(OpenClipboard(owner))return true;
        Sleep(1);
    }
    return false;
}

bool CopyToClipboard(const std::string& text)
{
    const int length=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(length<=0)return false;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(static_cast<size_t>(length)+1)*sizeof(wchar_t));
    if(!memory)return false;
    auto* wide=static_cast<wchar_t*>(GlobalLock(memory));
    if(!wide){GlobalFree(memory);return false;}
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),wide,length);
    wide[length]=L'\0';
    GlobalUnlock(memory);
    if(!OpenTextClipboard()){GlobalFree(memory);return false;}
    // The clipboard owns the memory once SetClipboardData succeeds.
    const bool placed=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory);
    CloseClipboard();
    if(!placed)GlobalFree(memory);
    return placed;
}

bool ReadClipboard(std::string& text)
{
    text.clear();
    if(!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenTextClipboard())return false;
    if(HANDLE data=GetClipboardData(CF_UNICODETEXT)){
        if(const auto* wide=static_cast<const wchar_t*>(GlobalLock(data))){
            const size_t capacity=GlobalSize(data)/sizeof(wchar_t);
            size_t length=0;
            while(length<capacity && length<65536 && wide[length])++length;
            const int bytes=WideCharToMultiByte(CP_UTF8,0,wide,static_cast<int>(length),nullptr,0,nullptr,nullptr);
            if(bytes>0){
                text.resize(static_cast<size_t>(bytes));
                WideCharToMultiByte(CP_UTF8,0,wide,static_cast<int>(length),text.data(),bytes,nullptr,nullptr);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return !text.empty();
}

// A saved preset of `settings` under `name`, as a code would bring it back.
dlss5::presets::Entry PresetEntry(const dlss5::Settings& settings,const std::string& name)
{
    dlss5::presets::Entry entry;
    entry.readable=dlss5::presets::Decode(dlss5::presets::Encode(settings,name),entry.preset)==dlss5::presets::Error::eNone;
    return entry;
}

// The first read when a tab shows the presets; an unreadable file shows an empty list and is never written over.
void ReadDlss5PresetsOnce()
{
    if(gDlss5PresetsRead)return;
    if(!ReadDlss5Presets()){gDlss5PresetsRead=true;SetPresetNote(PresetNote::eFileFailed);}
}

// The saved preset the tab shows: the one last chosen while the settings still match it, else the first that
// matches. `edited`: the one last chosen once the settings no longer match any (kNone otherwise).
size_t ShownPreset(const dlss5::Settings& s,size_t& edited)
{
    using namespace dlss5::presets;
    edited=kNone;
    const size_t chosen=gDlss5PresetSelected.empty()?kNone:FindName(gDlss5Presets,gDlss5PresetSelected);
    if(chosen!=kNone && Records(gDlss5Presets[chosen].preset.settings)==Records(s))return chosen;
    const size_t matching=FindSettings(gDlss5Presets,s);
    if(matching==kNone)edited=chosen;
    return matching;
}

// The look the Presets list shows when no saved preset is: the one the settings match, kLookCount (Custom) for none.
size_t ShownLook(const dlss5::Settings& s,size_t shown,size_t edited)
{
    return shown==dlss5::presets::kNone && edited==dlss5::presets::kNone?dlss5::MatchingLook(s):dlss5::kLookCount;
}

// What the Presets list shows: the saved preset, the one last chosen marked changed, the look, or Custom.
const char* PresetPreview(size_t shown,size_t edited,size_t look,const char* const* looks,char* buffer,size_t size)
{
    if(shown!=dlss5::presets::kNone)return gDlss5Presets[shown].preset.name.c_str();
    if(edited!=dlss5::presets::kNone){
        snprintf(buffer,size,T("%s (changed)"),gDlss5Presets[edited].preset.name.c_str());
        return buffer;
    }
    return look<dlss5::kLookCount?looks[look]:T("Custom");
}

// Look `index` into `s`; no saved preset stays chosen. True when `s` changed: the caller saves it.
bool LoadDlss5Look(dlss5::Settings& s,size_t index)
{
    gDlss5PresetSelected.clear();
    gDlss5PresetNote=PresetNote::eNone;
    const dlss5::Settings before=s;
    dlss5::ApplyLook(s,index);
    return !(before==s);
}

// Saved preset `index` into `s`. True when `s` changed: the caller saves it.
bool LoadDlss5Preset(dlss5::Settings& s,size_t index)
{
    const dlss5::presets::Preset& preset=gDlss5Presets[index].preset;
    gDlss5PresetSelected=preset.name;
    CopyPresetName(preset.name);
    gDlss5PresetNote=PresetNote::eNone;
    const dlss5::Settings applied=dlss5::presets::Apply(s,preset.settings);
    if(applied==s)return false;
    s=applied;
    return true;
}

// The current settings under the name typed (a new numbered name when empty); a saved preset of that name is replaced.
void SaveDlss5Preset(const dlss5::Settings& s)
{
    using namespace dlss5::presets;
    if(!ReadDlss5Presets()){SetPresetNote(PresetNote::eFileFailed);return;}
    std::string name=CleanName(gDlss5PresetName);
    if(name.empty()){
        char numbered[96]{};
        snprintf(numbered,sizeof(numbered),T("Preset %u"),static_cast<unsigned>(ReadableCount(gDlss5Presets)+1));
        name=UniqueName(gDlss5Presets,numbered);
    }
    const size_t existing=FindName(gDlss5Presets,name);
    if(existing==kNone && ReadableCount(gDlss5Presets)>=kMaxPresets){SetPresetNote(PresetNote::eFull);return;}
    Entry entry=PresetEntry(s,name);
    if(existing!=kNone)gDlss5Presets[existing]=std::move(entry);
    else gDlss5Presets.push_back(std::move(entry));
    if(!WriteDlss5Presets()){ReadDlss5Presets();SetPresetNote(PresetNote::eFileFailed);return;}
    gDlss5PresetSelected=name;
    CopyPresetName(name);
    SetPresetNote(PresetNote::eSaved,name);
    gDlss5PresetRow=PresetRow::eNone;
}

// By value: the name may be the entry's own.
void DeleteDlss5Preset(std::string name)
{
    if(!ReadDlss5Presets()){SetPresetNote(PresetNote::eFileFailed);return;}
    const size_t index=dlss5::presets::FindName(gDlss5Presets,name);
    if(index==dlss5::presets::kNone)return;
    gDlss5Presets.erase(gDlss5Presets.begin()+static_cast<ptrdiff_t>(index));
    if(!WriteDlss5Presets()){ReadDlss5Presets();SetPresetNote(PresetNote::eFileFailed);return;}
    if(dlss5::presets::SameName(gDlss5PresetSelected,name))gDlss5PresetSelected.clear();
    SetPresetNote(PresetNote::eDeleted,name);
}

// The current settings as a code on the clipboard (else in the field), named after the preset shown or the name typed.
void ExportDlss5Preset(const dlss5::Settings& s,size_t shown)
{
    const std::string name=shown!=dlss5::presets::kNone?gDlss5Presets[shown].preset.name
        :dlss5::presets::CleanName(gDlss5PresetName);
    const std::string code=dlss5::presets::Encode(s,name);
    if(CopyToClipboard(code)){SetPresetNote(PresetNote::eCopied,name);return;}
    snprintf(gDlss5PresetCode,sizeof(gDlss5PresetCode),"%s",code.c_str());
    SetPresetNote(PresetNote::eCopyFailed);
}

// The code in the field, or on the clipboard when the field is empty, into `s` (DLSS 5 on) and the saved presets,
// unless the same settings are saved already. True when `s` changed: the caller saves it.
bool ImportDlss5Preset(dlss5::Settings& s)
{
    using namespace dlss5::presets;
    std::string text=gDlss5PresetCode;
    if(text.find_first_not_of(" \t\r\n")==std::string::npos && !ReadClipboard(text)){SetPresetNote(PresetNote::eNoCode);return false;}
    Preset preset;
    switch(Decode(text,preset)){
    case Error::eNone:break;
    case Error::eNewer:SetPresetNote(PresetNote::eNewer);return false;
    case Error::eDamaged:SetPresetNote(PresetNote::eDamaged);return false;
    default:SetPresetNote(PresetNote::eNoCode);return false;
    }
    gDlss5PresetCode[0]='\0';
    const dlss5::Settings applied=Apply(s,preset.settings);
    const bool changed=!(applied==s);
    s=applied;
    PresetNote note=PresetNote::eImported;
    std::string name;
    if(!ReadDlss5Presets())note=PresetNote::eFileFailed;
    else if(const size_t same=FindSettings(gDlss5Presets,preset.settings);same!=kNone)name=gDlss5Presets[same].preset.name;
    else if(ReadableCount(gDlss5Presets)>=kMaxPresets)note=PresetNote::eFull;
    else{
        name=UniqueName(gDlss5Presets,preset.name.empty()?std::string(T("Imported preset")):preset.name);
        gDlss5Presets.push_back(PresetEntry(preset.settings,name));
        if(!WriteDlss5Presets()){ReadDlss5Presets();note=PresetNote::eFileFailed;name.clear();}
    }
    gDlss5PresetSelected=name;
    if(!name.empty())CopyPresetName(name);
    SetPresetNote(note,name,preset.skipped!=0);
    if(note==PresetNote::eImported)gDlss5PresetRow=PresetRow::eNone;
    return changed;
}

// The last preset action's result, for 10 s; nullptr when none.
const char* Dlss5PresetNoteText(Tone& tone,char* buffer,size_t size)
{
    if(gDlss5PresetNote==PresetNote::eNone || GetTickCount64()-gDlss5PresetNoteAt>10000)return nullptr;
    const char* name=gDlss5PresetNoteName.c_str();
    tone=Tone::Good;
    switch(gDlss5PresetNote){
    case PresetNote::eSaved:snprintf(buffer,size,T("Saved \"%s\"."),name);break;
    case PresetNote::eDeleted:tone=Tone::Dim;snprintf(buffer,size,T("Deleted \"%s\"."),name);break;
    case PresetNote::eImported:snprintf(buffer,size,T("Imported \"%s\"."),name);break;
    case PresetNote::eCopied:snprintf(buffer,size,"%s",T("Copied the preset code to the clipboard."));break;
    case PresetNote::eCopyFailed:tone=Tone::Pending;
        snprintf(buffer,size,"%s",T("The clipboard is unavailable. The code is in the field to copy by hand."));break;
    case PresetNote::eNoCode:tone=Tone::Bad;snprintf(buffer,size,"%s",T("No preset code found. Codes start with DLSS5-."));break;
    case PresetNote::eDamaged:tone=Tone::Bad;
        snprintf(buffer,size,"%s",T("This preset code is damaged or incomplete. Copy it again in full."));break;
    case PresetNote::eNewer:tone=Tone::Bad;snprintf(buffer,size,"%s",T("This preset code needs a newer MFG Unlocker."));break;
    case PresetNote::eFull:tone=Tone::Bad;
        snprintf(buffer,size,T("%u presets is the most. Delete one to save another."),static_cast<unsigned>(dlss5::presets::kMaxPresets));break;
    case PresetNote::eFileFailed:tone=Tone::Bad;snprintf(buffer,size,"%s",T("The presets file could not be read or written."));break;
    default:return nullptr;
    }
    return buffer;
}

const char* Dlss5PresetSkippedText() noexcept
{
    return T("Some settings in this code need a newer MFG Unlocker and were left out.");
}

const char* Dlss5PresetSaveHelp() noexcept
{
    return T("Saves the current DLSS 5 settings as a preset under this name; a preset with the same name is replaced.\n"
        "Presets are kept beside the game.");
}

const char* Dlss5PresetShareHelp() noexcept
{
    return T("Export copies the current settings to the clipboard as a code starting with DLSS5-, to share with other players.\n"
        "Import applies a pasted code and saves it as a preset. With the field empty, Import reads the clipboard.");
}

// The Presets rows of the menu's DLSS 5 tab, in its Neural Rendering grid. True when `s` changed: the caller saves it.
bool Dlss5PresetRows(dlss5::Settings& s)
{
    using namespace dlss5::presets;
    ReadDlss5PresetsOnce();
    bool changed=false;
    size_t edited=kNone;
    const size_t shown=ShownPreset(s,edited);
    const size_t look=ShownLook(s,shown,edited);
    const char* const looks[]{T("Subtle"),T("Balanced"),T("Strong")};
    static_assert(sizeof(looks)/sizeof(*looks)==dlss5::kLookCount);
    const ImGuiStyle& style=ImGui::GetStyle();
    const float font=ImGui::GetFontSize();
    const auto beside=[&](const char* button){return ImGui::CalcTextSize(button,nullptr,true).x+style.FramePadding.x*2.0f+style.ItemSpacing.x;};
    // The looks, then the saved presets. Save... and Share... open their row below, lit while it is open.
    ControlRow(T("Presets"));
    const char* deleteLabel=T("Delete");
    char saveRow[96]{},shareRow[96]{};
    PresetRowLabel(PresetRow::eSave,saveRow,sizeof(saveRow));
    PresetRowLabel(PresetRow::eShare,shareRow,sizeof(shareRow));
    ImGui::SetNextItemWidth(std::max(font*4.0f,ImGui::GetContentRegionAvail().x-font*1.8f-beside(saveRow)-beside(shareRow)-beside(deleteLabel)));
    char preview[160]{};
    if(ImGui::BeginCombo("##dlss5presets",PresetPreview(shown,edited,look,looks,preview,sizeof(preview)))){
        for(size_t index=0;index!=dlss5::kLookCount;++index)
            if(ImGui::Selectable(looks[index],index==look))changed|=LoadDlss5Look(s,index);
        if(ReadableCount(gDlss5Presets))ImGui::Separator();
        for(size_t index=0;index!=gDlss5Presets.size();++index){
            if(!gDlss5Presets[index].readable)continue;
            ImGui::PushID(static_cast<int>(index));
            if(ImGui::Selectable(gDlss5Presets[index].preset.name.c_str(),index==shown))changed|=LoadDlss5Preset(s,index);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto rowButton=[](const char* label,PresetRow row){
        const bool open=gDlss5PresetRow==row;
        if(open)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked=ImGui::Button(label);
        if(open)ImGui::PopStyleColor();
        if(clicked)TogglePresetRow(row);
    };
    ImGui::SameLine();
    rowButton(saveRow,PresetRow::eSave);
    ImGui::SameLine();
    rowButton(shareRow,PresetRow::eShare);
    ImGui::SameLine();
    const size_t current=shown!=kNone?shown:edited;
    ImGui::BeginDisabled(current==kNone);
    if(ImGui::Button(deleteLabel) && current!=kNone)DeleteDlss5Preset(gDlss5Presets[current].preset.name);
    ImGui::EndDisabled();
    HelpMarker(Dlss5PresetsHelp());
    if(gDlss5PresetRow==PresetRow::eSave){
        ControlRow(T("Save as"));
        const char* saveLabel=T("Save");
        ImGui::SetNextItemWidth(std::max(font*4.0f,ImGui::CalcItemWidth()-beside(saveLabel)));
        const bool named=ImGui::InputTextWithHint("##dlss5presetname",T("Name"),gDlss5PresetName,sizeof(gDlss5PresetName),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if(ImGui::Button(saveLabel) || named)SaveDlss5Preset(s);
        HelpMarker(Dlss5PresetSaveHelp());
    }
    if(gDlss5PresetRow==PresetRow::eShare){
        ControlRow(T("Share"));
        const char* importLabel=T("Import");
        const char* exportLabel=T("Export");
        ImGui::SetNextItemWidth(std::max(font*4.0f,ImGui::GetContentRegionAvail().x-beside(importLabel)-beside(exportLabel)-font*1.8f));
        const bool pasted=ImGui::InputTextWithHint("##dlss5presetcode",T("Paste a code"),gDlss5PresetCode,sizeof(gDlss5PresetCode),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if(ImGui::Button(importLabel) || pasted)changed|=ImportDlss5Preset(s);
        ImGui::SameLine();
        if(ImGui::Button(exportLabel))ExportDlss5Preset(s,shown);
        HelpMarker(Dlss5PresetShareHelp());
    }
    return changed;
}

void Dlss5PresetNotes()
{
    Tone tone=Tone::Normal;
    char note[320]{};
    if(const char* text=Dlss5PresetNoteText(tone,note,sizeof(note))){
        Note(tone,"%s",text);
        if(gDlss5PresetSkipped)Note(Tone::Pending,"%s",Dlss5PresetSkippedText());
    }
}

// dlss5tab.1: the Advanced pages, in both menus' order.
inline constexpr size_t kDlss5PageCount=5;
const char* Dlss5PageName(size_t page)
{
    switch(page){
    case 0:return T("Look & passes");
    case 1:return T("Blend");
    case 2:return T("Scaling");
    case 3:return T("HDR");
    default:return T("Provider");
    }
}

// Whether an Advanced page holds a setting away from its default: its tab is marked. Later-pass values only count
// through Later passes being Custom.
bool Dlss5PageChanged(const dlss5::Settings& s,size_t page)
{
    const dlss5::Settings d{};
    switch(page){
    case 0:return s.structure!=d.structure||s.tone!=d.tone||s.passes!=d.passes||s.laterCustom!=d.laterCustom;
    case 1:return s.blend!=d.blend||s.color!=d.color||s.highlightGuard!=d.highlightGuard||s.highlightLimit!=d.highlightLimit
        ||s.haloDark!=d.haloDark||s.haloBright!=d.haloBright||s.haloRadius!=d.haloRadius;
    case 2:return s.domain!=d.domain||s.downsample!=d.downsample||s.upsample!=d.upsample||s.supersample!=d.supersample
        ||s.reconstruction!=d.reconstruction;
    case 3:return s.encoding!=d.encoding||s.signal!=d.signal||s.diffuseWhiteOverride!=d.diffuseWhiteOverride||s.knee!=d.knee
        ||s.exposure!=d.exposure||s.exposureMode!=d.exposureMode||s.adaptiveExposure!=d.adaptiveExposure
        ||s.adaptiveTarget!=d.adaptiveTarget;
    default:return s.uiCorrection!=d.uiCorrection||s.resources!=d.resources||s.blockHudless!=d.blockHudless;
    }
}

void AppendDetail(char* detail,size_t size,const char* more)
{
    const size_t used=std::char_traits<char>::length(detail);
    if(used+1<size)snprintf(detail+used,size-used,"%s%s",used?", ":"",more);
}

// The GPU cost row's detail: its share of the frame and, with Half rate, the network's own cost (every other frame).
void Dlss5CostDetail(const MfgUnlockReShadeSnapshot& snapshot,char* detail,size_t size)
{
    detail[0]='\0';
    if(snapshot.dlss5FrameMs>0.0f)snprintf(detail,size,T("%.0f%% of each rendered frame"),100.0f*snapshot.dlss5GpuMs/snapshot.dlss5FrameMs);
    if(dlss5::kHalfRateOffered && snapshot.dlss5BoostNetworkMs>=0.0f){
        char ms[32]{},part[96]{};
        snprintf(ms,sizeof(ms),T("%.1f ms"),snapshot.dlss5BoostNetworkMs);
        snprintf(part,sizeof(part),"%s %s",T("Half rate"),ms);
        AppendDetail(detail,size,part);
    }
}

// The Route row's value, filling its detail; nullptr for no row. dx11nr.1: a DirectX 11 game's DLSS 5 runs on a
// DirectX 12 device of MFG Unlocker's, which the row says before a route is known too.
const char* Dlss5RouteRow(const MfgUnlockReShadeSnapshot& snapshot,char* detail,size_t size)
{
    detail[0]='\0';
    if(!snapshot.dlss5Route)return snapshot.dlss5D3D11?T("DirectX 11 to DirectX 12"):nullptr;
    snprintf(detail,size,T("%ux%u, network %ux%u, passes: %u"),snapshot.dlss5Width,snapshot.dlss5Height,
        snapshot.dlss5ExtentWidth,snapshot.dlss5ExtentHeight,snapshot.dlss5Passes);
    if(snapshot.dlss5D3D11)AppendDetail(detail,size,T("DirectX 11 to DirectX 12"));
    return snapshot.dlss5Route==2?T("Direct provider"):T("Native NGX");
}

// The DLSS 5 tab: the RenoDX DLSS add-on's Neural Rendering settings.
void DrawDlss5(const MfgUnlockReShadeSnapshot& snapshot,bool stale)
{
    if(!stale && !gDlss5Dirty && !ImGui::IsAnyItemActive())gDlss5=snapshot.dlss5;
    dlss5::Settings& s=gDlss5;
    const dlss5::Settings before=s;
    bool save=false;
    const bool present=s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent);
    BeginStaleDisabled(stale);

    // dlss5tab.1: what most players change, grouped by purpose, a dependent setting on its parent's row. All else is
    // under Advanced, a page at a time; a closed page keeps its saved values.
    const ImGuiStyle& style=ImGui::GetStyle();
    const float tick=ImGui::GetFrameHeight()+style.ItemSpacing.x;
    const auto ticked=[&](const char* text){return tick+style.ItemInnerSpacing.x+ImGui::CalcTextSize(text).x;};
    Section(T("Neural Rendering"));
    if(BeginGrid("##dlss5nr")){
        // Menu order: Off, Auto (stage 0), then stages 1-3. The Auto entry names
        // the stage Auto runs: the live one once DLSS 5 runs it, else the policy's.
        const char* const fixedStages[]{T("Before upscaling (faster)"),T("After upscaling"),T("Present")};
        uint32_t chosen=static_cast<uint32_t>(dlss5::auto_mode::AutoStage());
        if(s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::eAuto) && snapshot.dlss5Route && !snapshot.dlss5Pending
            && snapshot.dlss5Stage>=1 && snapshot.dlss5Stage<=3)chosen=snapshot.dlss5Stage;
        char automatic[128]{};
        snprintf(automatic,sizeof(automatic),T("Auto: %s"),fixedStages[chosen-1]);
        const char* const stages[]{T("Off"),automatic,fixedStages[0],fixedStages[1],fixedStages[2]};
        uint32_t choice=!s.enabled?0:s.stage==static_cast<uint32_t>(dlss5::Stage::eAuto)?1:s.stage+1;
        // Vulkan runs Auto and After upscaling; Before upscaling and Present stay listed, disabled. The DirectX 11
        // bridge runs all but Present (dx11nr.1).
        const uint32_t unavailable=snapshot.graphicsApi==2u?(1u<<2)|(1u<<4):snapshot.dlss5D3D11?(1u<<4):0u;
        ControlRow(T("Hook Method"));
        if(ChoiceCombo("##dlss5stage",choice,stages,5,unavailable)){s.enabled=choice!=0;if(choice)s.stage=choice==1?0:choice-1;save=true;}
        HelpMarker(Dlss5StageHelp());
        save|=Dlss5PresetRows(s);
        ImGui::EndTable();
    }
    // Notes for the controls above; the save result is the previous frame's.
    if(gDlss5SaveFailed)Note(Tone::Bad,"%s",T("DLSS 5 settings could not be saved."));
    // A saved Before upscaling or Present on a Vulkan game stays selected and leaves the frame as is.
    const bool vulkanStage=snapshot.graphicsApi==2u && s.enabled
        && (s.stage==static_cast<uint32_t>(dlss5::Stage::eRender) || s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent));
    if(vulkanStage)
        Note(Tone::Pending,"%s",T("Before upscaling and Present do not run on Vulkan yet. Choose Auto or After upscaling."));
    else if(snapshot.dlss5D3D11 && present)
        Note(Tone::Pending,"%s",T("Present does not run on DirectX 11 yet. Choose Auto, Before upscaling or After upscaling."));
    else if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eStageUnavailable))
        Note(Tone::Dim,"%s",T("This game's swapchain cannot use the Present hook method; the log names the reason. Before or after upscaling still work."));
    Dlss5PresetNotes();

    Section(T("Look"));
    if(BeginGrid("##dlss5look")){
        ControlRow(T("Intensity"));
        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5intensity",&s.intensity,0.0f,1.0f,"%.2f"));
        HelpMarker(T("Strength of the whole neural rendering effect."));
        const char* const models[]{T("Neutral"),T("Rich"),T("Soft")};
        ControlRow(T("Style"));
        save|=SegmentedChoice("##dlss5model",s.model,models,3);
        HelpMarker(Dlss5StyleHelp());
        ControlRow(T("Faces & skin"));
        save|=FacesControl("dlss5character",s.character,s.structure,s.characterMask,true);
        HelpMarker(Dlss5FacesHelp(s.characterMask));
        ImGui::EndTable();
    }

    Section(T("Performance"));
    if(BeginGrid("##dlss5performance")){
        ControlRow(T("Resolution Scale"));
        ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize()*4.0f,ImGui::CalcItemWidth()-ticked(T("Auto"))));
        save|=Dlss5ReleaseSave(ImGui::SliderFloat("##dlss5scale",&s.scale,0.0f,dlss5::kMaxScale,s.scaleAuto?T("up to %.0f%%"):"%.0f%%"));
        ImGui::SameLine();ImGui::PushID("dlss5scaleauto");
        save|=ImGui::Checkbox(l10n::Label("Auto"),&s.scaleAuto);
        ImGui::PopID();
        HelpMarker(Dlss5ScaleHelp());
        // Boost runs on D3D12 Before and After upscaling; elsewhere it and Motion tolerance stay in place, disabled.
        if constexpr(dlss5::kHalfRateOffered){
            const bool boostUnavailable=snapshot.graphicsApi==2u || (s.enabled && s.stage==static_cast<uint32_t>(dlss5::Stage::ePresent));
            const char* toleranceLabel=T("Motion tolerance");
            const char* toleranceHelp=T("How much a surface may change between frames and still keep the carried fine detail.\n"
                "Strict carries it on fewer moving surfaces, which keep the rebuilt look; Loose carries more detail.");
            ControlRow(T("Half rate"));
            const float width=Dlss5ItemWidth(tick+ImGui::CalcTextSize(toleranceLabel).x+style.ItemSpacing.x);
            ImGui::BeginDisabled(boostUnavailable);
            save|=ImGui::Checkbox("##dlss5boost",&s.boost);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(boostUnavailable || !s.boost);
            ImGui::TextDisabled("%s",toleranceLabel);
            Dlss5ItemHelp(toleranceHelp);
            ImGui::SameLine();
            const char* const tolerances[]{T("Strict"),T("Normal"),T("Loose")};
            ImGui::SetNextItemWidth(width);
            save|=ChoiceCombo("##dlss5boosttolerance",s.boostTolerance,tolerances,3);
            Dlss5ItemHelp(toleranceHelp);
            ImGui::EndDisabled();
            HelpMarker(Dlss5HalfRateHelp(boostUnavailable));
        }
        const char* const presets[]{T("Default"),T("Quality"),T("Fast")};
        ControlRow(T("Network"));
        save|=ChoiceCombo("##dlss5preset",s.preset,presets,3);
        HelpMarker(Dlss5NetworkHelp());
        ImGui::EndTable();
    }

    // Compare is not part of the look: Reset to defaults keeps it. Debug view, inspection rather than look, sits with it.
    Section(T("Compare"));
    if(BeginGrid("##dlss5compare")){
        ControlRow(T("Split view"));
        {
            const float width=Dlss5ItemWidth(tick+ticked(T("Swap sides")));
            save|=ImGui::Checkbox("##dlss5compare",&s.compare);
            ImGui::SameLine();
            ImGui::BeginDisabled(!s.compare);
            ImGui::SetNextItemWidth(width);
            save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5split",&s.compareSplit,dlss5::kMinCompareSplit,dlss5::kMaxCompareSplit,"%.0f%%"));
            Dlss5ItemHelp(T("Split position"));
            ImGui::SameLine();
            save|=ImGui::Checkbox(l10n::Label("Swap sides"),&s.compareSwap);
            Dlss5ItemHelp(T("Split view shows DLSS 5 on the left and the original on the right."));
            ImGui::EndDisabled();
        }
        HelpMarker(T("Shows the original image left of the line and DLSS 5 to its right.\n"
            "Before upscaling draws no line, because the upscaler would treat it as part of the image."));
        ControlRow(T("Hold to show original"));
        save|=CompareKeyControl(s);
        HelpMarker(T("While this key is held, the game shows its original image, so DLSS 5 can be compared in motion.\n"
            "DLSS 5 keeps running, so its GPU cost is unchanged, and the key still reaches the game."));
        const char* const views[]{T("Off"),T("Network input"),T("Network output"),T("Change (x8)")};
        ControlRow(T("Debug view"));
        save|=ChoiceCombo("##dlss5debugview",s.debugView,views,4);
        HelpMarker(T("Shows what the network is given and returns, or where DLSS 5 changes the image (grey: unchanged; lighter or darker: changed).\n"
            "With Split view, on the DLSS 5 side only."));
        ImGui::EndTable();
    }

    Section(T("Status"));
    if(BeginGrid("##dlss5status")){
        Tone tone=Tone::Pending;
        const char* state=Dlss5StateName(snapshot.dlss5State,tone);
        StatusRow(T("State"),stale?Tone::Dim:tone,"%s",stale?T("Waiting for DLSS"):state);
        if(!stale && snapshot.dlss5Route && snapshot.dlss5GpuMs>=0.0f){
            char value[32]{},detail[160]{};
            snprintf(value,sizeof(value),T("%.1f ms"),snapshot.dlss5GpuMs);
            Dlss5CostDetail(snapshot,detail,sizeof(detail));
            StatusDetail(T("GPU cost"),Tone::Normal,value,detail);
            if(dlss5::kHalfRateOffered && snapshot.dlss5BoostNetworkMs>=0.0f)Dlss5ItemHelp(T("every other frame, when the network runs"));
        }
        if(s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale>0.0f){
            char value[32]{},detail[96]{};
            snprintf(value,sizeof(value),"%.0f%%",snapshot.dlss5EffectiveScale);
            if(snapshot.dlss5GpuMs>=0.0f)snprintf(detail,sizeof(detail),T("%.1f of %.1f ms"),snapshot.dlss5GpuMs,snapshot.dlss5BudgetMs);
            else snprintf(detail,sizeof(detail),"%s",T("measuring"));
            StatusDetail(T("Auto scale"),Tone::Normal,value,detail);
        }
        char routeDetail[224]{};
        if(const char* route=Dlss5RouteRow(snapshot,routeDetail,sizeof(routeDetail)))StatusDetail(T("Route"),Tone::Normal,route,routeDetail);
        ImGui::EndTable();
    }
    if(s.enabled && !stale)
        if(const char* refusal=Dlss5D3D11RefusalText(snapshot.dlss5D3D11Refusal))Note(Tone::Dim,"%s",refusal);
    // Notes explaining the state above.
    if(snapshot.dlss5Pending)Note(Tone::Pending,"%s",T("Waiting for DLSS 5 GPU work to complete before applying settings."));
    // Why Resolution Scale Auto runs below the slider.
    if(s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale>0.0f){
        const float most=dlss5::auto_mode::kLevels[dlss5::auto_mode::CeilingLevel(s.scale)];
        if(snapshot.dlss5ScaleFloorHeld)
            Note(Tone::Dim,T("Auto Resolution Scale holds %.0f%%, its quality floor. DLSS 5 still costs more than its budget here, so the frame rate pays instead of detail."),
                snapshot.dlss5EffectiveScale);
        else if(snapshot.dlss5EffectiveScale<most-0.5f)
            Note(Tone::Dim,T("Auto Resolution Scale runs at %.0f%% of up to %.0f%% to keep DLSS 5 within its budget, and rises again with headroom."),
                snapshot.dlss5EffectiveScale,most);
    }
    if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eOtherMod))
        Note(Tone::Dim,"%s",T("Restart with other DLSS 5 add-ons disabled to give MFGUnlocker ownership of the provider."));
    if(snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eCreateFailed)
        || snapshot.dlss5State==static_cast<uint32_t>(dlss5::State::eEvaluateFailed))
        Note(Tone::Bad,"DLSSNR 0x%08X",snapshot.dlss5Result);
    // XeMFG standalone's game chain carries no menu session, where the Present stage runs.
    if(present && xess_ui::XemfgRuns(gXessUi))
        Note(Tone::Pending,"%s",T("The Present stage does not run with XeMFG standalone. Choose Auto or an upscaling stage."));

    // Closed by default; ImGui keeps it, and the page shown, as set for the session. A dot marks a page with a changed setting.
    if(ImGui::CollapsingHeader(l10n::Label("Advanced"))){
        if(BeginTabs("##dlss5advanced")){
            // Passes 2 and later; their rows stay saved while Pass Count is 1.
            if(TabItem(Dlss5PageName(0),Dlss5PageChanged(s,0))){
                if(BeginGrid("##dlss5passes")){
                    ControlRow(T("Structure Intensity"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5structure",&s.structure,0.0f,dlss5::kMaxStrength,"%.2f"));
                    HelpMarker(T("Local detail and structure enhancement.\nAbove 1 goes past the provider's usual range."));
                    ControlRow(T("Tone Intensity"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5tone",&s.tone,0.0f,dlss5::kMaxStrength,"%.2f"));
                    HelpMarker(T("Local tone and contrast enhancement.\nAbove 1 goes past the provider's usual range."));
                    int passes=static_cast<int>(s.passes);
                    ControlRow(T("Pass Count"));
                    if(Dlss5ReleaseSave(ImGui::SliderInt("##dlss5passes",&passes,1,static_cast<int>(dlss5::kMaxPasses),"%d")))save=true;
                    s.passes=static_cast<uint32_t>(std::clamp(passes,1,static_cast<int>(dlss5::kMaxPasses)));
                    HelpMarker(T("Runs the network several times in a row. Every pass is a separate feature and costs GPU time and memory."));
                    const char* const later[]{T("Same as pass 1"),T("Custom")};
                    uint32_t custom=s.laterCustom?1u:0u;
                    ControlRow(T("Later passes"));
                    if(ChoiceCombo("##dlss5later",custom,later,2)){
                        s.laterCustom=custom!=0;save=true;
                        // Custom starts from pass 1's look, so switching changes nothing yet.
                        if(s.laterCustom){s.laterModel=s.model;s.laterIntensity=s.intensity;s.laterStructure=s.structure;s.laterTone=s.tone;s.laterCharacter=s.character;}
                    }
                    HelpMarker(T("Passes 2 and later refine the previous pass's output. Custom gives them their own Style and strengths.\n"
                        "Tone adds up from pass to pass, so later passes usually want less of it."));
                    if(s.laterCustom){
                        ImGui::BeginDisabled(s.passes<2);
                        const char* const models[]{T("Neutral"),T("Rich"),T("Soft")};
                        ControlRow(T("Style"));
                        save|=ChoiceCombo("##dlss5latermodel",s.laterModel,models,3);
                        ControlRow(T("Intensity"));
                        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5laterintensity",&s.laterIntensity,0.0f,1.0f,"%.2f"));
                        ControlRow(T("Structure Intensity"));
                        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5laterstructure",&s.laterStructure,0.0f,dlss5::kMaxStrength,"%.2f"));
                        ControlRow(T("Tone Intensity"));
                        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5latertone",&s.laterTone,0.0f,dlss5::kMaxStrength,"%.2f"));
                        ControlRow(T("Faces & skin"));
                        save|=FacesControl("dlss5latercharacter",s.laterCharacter,s.laterStructure,s.characterMask);
                        ImGui::EndDisabled();
                    }
                    ImGui::EndTable();
                }
                if(s.laterCustom && s.passes<2)Note(Tone::Dim,"%s",T("Later-pass settings apply when Pass Count is 2 or more."));
                ImGui::EndTabItem();
            }

            // Applied once to the last pass's result, in our decode pass.
            if(TabItem(Dlss5PageName(1),Dlss5PageChanged(s,1))){
                if(BeginGrid("##dlss5blend")){
                    ControlRow(T("Blend strength"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5blendstrength",&s.blend,0.0f,dlss5::kMaxBlend,"%.2f"));
                    HelpMarker(T("Scales DLSS 5's change: 0 keeps the game's image, 1 is DLSS 5's result, above 1 exaggerates it."));
                    ControlRow(T("Color strength"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5colorstrength",&s.color,0.0f,dlss5::kMaxColor,"%.2f"));
                    HelpMarker(T("0 keeps the game's colors and takes only DLSS 5's lighting and detail. 1 is DLSS 5's color; above 1 strengthens its color change."));
                    ControlRow(T("Highlight guard"));
                    {
                        char limit[96]{};
                        snprintf(limit,sizeof(limit),"%s %%.1fx",T("Brightening limit"));
                        const float width=Dlss5ItemWidth(tick);
                        save|=ImGui::Checkbox("##dlss5guard",&s.highlightGuard);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!s.highlightGuard);
                        ImGui::SetNextItemWidth(width);
                        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5guardlimit",&s.highlightLimit,dlss5::kMinHighlightLimit,dlss5::kMaxHighlightLimit,limit));
                        ImGui::EndDisabled();
                    }
                    HelpMarker(T("Limits how far DLSS 5 may brighten a pixel, as a multiple of its original brightness, so lights do not turn into bright speckles."));
                    ControlRow(T("Dark halo reduction"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5halodark",&s.haloDark,0.0f,1.0f,"%.2f"));
                    HelpMarker(T("Removes thin dark outlines DLSS 5 adds beside bright edges and keeps its broad lighting change. Can lighten real shadow edges."));
                    ControlRow(T("Bright halo reduction"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5halobright",&s.haloBright,0.0f,1.0f,"%.2f"));
                    HelpMarker(T("Removes thin bright outlines DLSS 5 adds beside dark edges."));
                    int radius=static_cast<int>(s.haloRadius);
                    ControlRow(T("Halo radius"));
                    ImGui::BeginDisabled(s.haloDark<=0.0f && s.haloBright<=0.0f);
                    if(Dlss5SliderSave(ImGui::SliderInt("##dlss5haloradius",&radius,static_cast<int>(dlss5::kMinHaloRadius),static_cast<int>(dlss5::kMaxHaloRadius),"%d")))save=true;
                    s.haloRadius=static_cast<uint32_t>(std::clamp(radius,static_cast<int>(dlss5::kMinHaloRadius),static_cast<int>(dlss5::kMaxHaloRadius)));
                    ImGui::EndDisabled();
                    HelpMarker(T("How far from each pixel to look for the edge, in network pixels."));
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            if(TabItem(Dlss5PageName(2),Dlss5PageChanged(s,2))){
                if(BeginGrid("##dlss5scaling")){
                    const char* const domains[]{"scRGB-nl",T("Signed PQ"),T("Linear")};
                    ControlRow(T("Scaling Domain"));
                    save|=ChoiceCombo("##dlss5domain",s.domain,domains,3);
                    HelpMarker(T("Signal space used for downsampling and upsampling."));
                    const char* const downsample[]{T("Area"),T("Bilinear"),T("Nearest")};
                    ControlRow(T("Downsample Filter"));
                    save|=ChoiceCombo("##dlss5down",s.downsample,downsample,3);
                    const char* const upsample[]{T("Bilinear"),T("Bicubic"),T("Nearest"),T("Edge-aware")};
                    ControlRow(T("Upsample Filter"));
                    save|=ChoiceCombo("##dlss5up",s.upsample,upsample,4);
                    HelpMarker(T("Resolution Scale below 100: how the network's change is enlarged. Edge-aware keeps it from bleeding across edges."));
                    const char* const supersample[]{T("Bilinear"),T("Area")};
                    ControlRow(T("Supersampling Filter"));
                    save|=ChoiceCombo("##dlss5supersample",s.supersample,supersample,2);
                    HelpMarker(T("Resolution Scale above 100: how the network's larger image comes back to the game's size. Area averages every pixel it covers, without aliasing."));
                    const char* const reconstruction[]{T("RGB residual"),T("Processed image")};
                    ControlRow(T("Scale Reconstruction"));
                    save|=ChoiceCombo("##dlss5reconstruction",s.reconstruction,reconstruction,2);
                    HelpMarker(T("RGB residual adds the network's change to the full-resolution image.\n"
                        "Processed image uses the upscaled network output directly."));
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            if(TabItem(Dlss5PageName(3),Dlss5PageChanged(s,3))){
                if(BeginGrid("##dlss5hdr")){
                    const char* const encodings[]{T("Auto"),T("Linear BT.709"),"sRGB","HDR10","scRGB","scRGB-nl"};
                    ControlRow(T("Encoding"));
                    save|=ChoiceCombo("##dlss5encoding",s.encoding,encodings,6);
                    HelpMarker(T("Auto treats the DLSS input and output as linear BT.709 and follows the swapchain format and color space at Present.\n"
                        "Other choices override the encoding of the processed image."));
                    const char* const signals[]{"sRGB",T("PQ SDR"),T("Linear SDR")};
                    ControlRow(T("NR Signal Encoding"));
                    save|=ChoiceCombo("##dlss5signal",s.signal,signals,3);
                    HelpMarker(T("Encoding supplied to the network after the HDR-to-SDR bridge."));
                    const bool explicitEncoding=s.encoding!=static_cast<uint32_t>(dlss5::Encoding::eAuto);
                    // The effective value; a hand edit (explicit encodings only) overrides it.
                    float white=dlss5::DiffuseWhite(s);
                    ControlRow(T("Diffuse White"));
                    ImGui::BeginDisabled(!explicitEncoding);
                    const bool whiteEdited=ImGui::SliderFloat("##dlss5white",&white,dlss5::kMinDiffuseWhite,dlss5::kMaxDiffuseWhite,T("%.0f nits"));
                    if(whiteEdited){s.diffuseWhite=white;s.diffuseWhiteOverride=true;}
                    save|=Dlss5SliderSave(whiteEdited);
                    ImGui::EndDisabled();
                    HelpMarker(T("Reference white of the HDR-to-SDR bridge. Encoding Auto selects it; choose an explicit encoding to set it."));
                    ControlRow(T("Highlight roll-off"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5knee",&s.knee,dlss5::kMinKnee,dlss5::kMaxKnee,"%.2f"));
                    HelpMarker(T("HDR only: up to this share of diffuse white the network sees the image unchanged; brighter values are compressed into the rest. Lower gives highlights more room."));
                    ControlRow(T("Exposure"));
                    save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5exposure",&s.exposure,0.0f,dlss5::kMaxExposure,"%.2fx",ImGuiSliderFlags_Logarithmic));
                    HelpMarker(T("Multiplies the exposure used for neural rendering, independent of the game's exposure."));
                    const char* const exposureModes[]{T("Off"),T("On"),T("Inverse")};
                    ControlRow(T("Use Exposure Value"));
                    save|=ChoiceCombo("##dlss5exposuremode",s.exposureMode,exposureModes,3);
                    HelpMarker(T("On applies the game's DLSS exposure texture before neural rendering; Inverse divides by it."));
                    ControlRow(T("Adaptive exposure"));
                    {
                        const bool gain=s.adaptiveExposure && snapshot.dlss5Route && snapshot.dlss5ExposureGain!=1.0f;
                        char target[96]{};
                        snprintf(target,sizeof(target),"%s %%.2f",T("Adaptive target"));
                        const float width=Dlss5ItemWidth(tick+(gain?ImGui::CalcTextSize("x0.00").x+style.ItemSpacing.x:0.0f));
                        save|=ImGui::Checkbox("##dlss5adaptive",&s.adaptiveExposure);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!s.adaptiveExposure);
                        ImGui::SetNextItemWidth(width);
                        save|=Dlss5SliderSave(ImGui::SliderFloat("##dlss5adaptivetarget",&s.adaptiveTarget,dlss5::kMinAdaptiveTarget,dlss5::kMaxAdaptiveTarget,target));
                        Dlss5ItemHelp(T("The average brightness Adaptive exposure gives the scene the network sees (0.18 is middle grey). Higher shows it a brighter scene."));
                        ImGui::EndDisabled();
                        if(gain){ImGui::SameLine();ImGui::TextDisabled("x%.2f",snapshot.dlss5ExposureGain);}
                    }
                    HelpMarker(T("Keeps the scene the network sees at a steady brightness in HDR games, so the same settings suit games with different exposure.\n"
                        "The game's own brightness is unchanged. SDR color is left as it is."));
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            if(TabItem(Dlss5PageName(4),Dlss5PageChanged(s,4))){
                if(BeginGrid("##dlss5provider")){
                    const char* const uiCorrection[]{T("Auto"),T("Off"),T("On")};
                    ControlRow(T("UI Correction"));
                    save|=ChoiceCombo("##dlss5ui",s.uiCorrection,uiCorrection,3);
                    HelpMarker(T("Auto enables the provider's UI correction at Present, whose image contains the game's UI, and leaves it off for DLSS input and output.\n"
                        "Off never uses it. On always does."));
                    // Only the Present hook method uses these; otherwise they stay saved and disabled in place.
                    ImGui::BeginDisabled(!present);
                    const char* const resources[]{T("Never"),T("Optional"),T("Required")};
                    ControlRow(T("Use DLSS resources"));
                    save|=ChoiceCombo("##dlss5resources",s.resources,resources,3);
                    HelpMarker(T("Present only. Optional reuses the DLSS motion vectors and depth when available, otherwise neutral inputs.\n"
                        "Required skips Present processing without them. Never always uses neutral inputs."));
                    ControlRow(T("Block Hudless"));
                    save|=ImGui::Checkbox("##dlss5hudless",&s.blockHudless);
                    HelpMarker(T("Present only: keeps the game's HUD-less image from DLSS frame generation, so generated frames match processed ones."));
                    ImGui::EndDisabled();
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        if(ImGui::Button(T("Reset to defaults"))){
            dlss5::Settings defaults{};defaults.enabled=s.enabled;defaults.stage=s.stage;
            defaults.compare=s.compare;defaults.compareSplit=s.compareSplit;defaults.compareKey=s.compareKey;
            if(!(defaults==s)){s=defaults;save=true;}
        }
    }
    ImGui::EndDisabled();
    s=dlss5::Sanitized(s);
    if(save){
        gDlss5SaveFailed=!NativeFilePersistDlss5(s);
        gDlss5LastSave=GetTickCount64();gDlss5Dirty=false;
        if(gDlss5SaveFailed)s=before;
    }
}

const char* ReflexPendingMessage(uint32_t status)
{
    switch (static_cast<reflex_control::Status>(status))
    {
    case reflex_control::Status::eWaitingForModule: return T("Waiting for the game's Reflex plugin.");
    case reflex_control::Status::eUnsupportedModule: return T("The loaded Reflex plugin version is unsupported.");
    case reflex_control::Status::eHookUnavailable: return T("Reflex controls are unavailable for this plugin.");
    case reflex_control::Status::eIneligible: return T("The Reflex limit is unavailable for the current FG runtime.");
    case reflex_control::Status::eWaitingForGameOptions: return T("Waiting for game Reflex settings. Change the game's Reflex setting once to retry.");
    case reflex_control::Status::eWaitingForThread: return T("Waiting for the game's Reflex settings thread.");
    case reflex_control::Status::eWaitingForAvailability: return T("Waiting for Reflex availability.");
    case reflex_control::Status::eUnavailable: return T("Reflex is unavailable.");
    case reflex_control::Status::eRestorePending: return T("Restoring the game's Reflex limit...");
    case reflex_control::Status::eCallRejected: return T("The game rejected the Reflex limit.");
    case reflex_control::Status::eUnknownShape: return T("The game's Reflex settings cannot be safely replayed.");
    case reflex_control::Status::eStaleRoute: return T("The Reflex plugin changed. Restart the game.");
    case reflex_control::Status::eBusy: return T("Waiting for the next safe Reflex update.");
    default: return T("Waiting for the game to accept the Reflex limit.");
    }
}

// Set by the control rows; read by the status rows in the same frame.
bool gPresentationChangedThisFrame = false;

// V-Sync and Reflex limit rows for the DLSS Latency and display grid.
// `xemfg` for the XeSS panel under XeMFG standalone, whose FPS limit XeLL applies to XeMFG's output
// (DLSS's Dynamic MFG never pauses it there).
void PresentationControlRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool xemfg = false)
{
    gPresentationChangedThisFrame = false;
    if (!BackendUsesNativeFile()) return;
    if (!statusStale)
    {
        gVsyncMode = static_cast<int>(snapshot.vsyncMode);
        gReflexFrameLimitFps = static_cast<int>(snapshot.reflexFrameLimitFps);
    }
    const int previousLimit = gReflexFrameLimitFps;
    const int previousVsync = gVsyncMode;
    bool changed = false;
    // Off submits the game's frames without V-Sync. A saved On from older
    // builds is not offered and behaves as Game.
    const char* const vsyncModes[] = {T("Game"), T("Off")};
    int vsyncChoice = gVsyncMode == 1 ? 1 : 0;
    ControlRow(T("V-Sync"));
    if (ImGui::Combo("##vsync", &vsyncChoice, vsyncModes, 2))
    {
        gVsyncMode = vsyncChoice;
        changed = true;
    }
    HelpMarker(T("Off submits the game's frames without V-Sync.\n"
        "NVIDIA Control Panel V-Sync settings take priority."));
    const bool dynamic = !xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode);
    ControlRow(T("Reflex FPS limit"));
    ImGui::BeginDisabled(dynamic);
    bool limitEnabled = gReflexFrameLimitFps > 0;
    if (ImGui::Checkbox("##reflexlimit", &limitEnabled) && !dynamic)
    {
        gReflexFrameLimitFps = limitEnabled ? std::clamp(gReflexCustomLimitFps, 1, 1000) : 0;
        changed = true;
    }
    ImGui::SameLine();
    if (limitEnabled)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragInt("##reflexlimitfps", &gReflexCustomLimitFps, 1.0f, 1, 1000, "%d FPS");
        if (ImGui::IsItemDeactivatedAfterEdit() && !dynamic)
        {
            gReflexCustomLimitFps = std::clamp(gReflexCustomLimitFps, 1, 1000);
            gReflexFrameLimitFps = gReflexCustomLimitFps;
            changed = true;
        }
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T("Off"));
    }
    ImGui::EndDisabled();
    HelpMarker(xemfg ? T("Caps the frame rate XeMFG presents.\nXeLL paces the game's frames to it.")
        : T("Caps the game's frame rate through Reflex.\n"
        "Paused while Dynamic MFG is on; the saved limit is kept."));
    if (changed)
    {
        const BOOL saved = NativeFilePersistPresentation(static_cast<uint32_t>(gVsyncMode),
            static_cast<uint32_t>(gReflexFrameLimitFps));
        gLastApplyAttempted = true;
        gLastApplyAccepted = saved != FALSE;
        gLastNativeConfigPersisted = saved != FALSE;
        gPresentationSaveFailed = !saved;
        if (!saved)
        {
            gReflexFrameLimitFps = previousLimit;
            gVsyncMode = previousVsync;
        }
    }
    gPresentationChangedThisFrame = changed;
}

void PresentationNotes(const MfgUnlockReShadeSnapshot& snapshot, bool xemfg = false)
{
    if (!BackendUsesNativeFile()) return;
    if (!xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode) && gReflexFrameLimitFps > 0)
        Note(Tone::Dim, T("The Reflex limit is paused while Dynamic MFG is on. The saved limit is kept."));
    if (gPresentationSaveFailed)
        Note(Tone::Bad, T("The presentation settings could not be saved."));
}

const char* VulkanPresentModeName(uint32_t mode) noexcept
{
    switch (mode)
    {
    case vsync_control::kPresentModeImmediate: return "IMMEDIATE";
    case vsync_control::kPresentModeMailbox: return "MAILBOX";
    case vsync_control::kPresentModeFifo: return "FIFO";
    case vsync_control::kPresentModeFifoRelaxed: return "FIFO_RELAXED";
    }
    return "other";
}

// V-Sync readback for a Vulkan game (vsync_control.h): the present mode is
// fixed when the game creates its swapchain, so a change waits for its next
// one. The caller passes a current snapshot. Returns false when the snapshot
// describes no Vulkan presentation, leaving the DXGI rows to the caller.
bool VulkanVsyncRow(const MfgUnlockReShadeSnapshot& snapshot)
{
    using vsync_control::Failure;
    const auto failure = static_cast<Failure>(snapshot.vsyncFailure);
    if (!snapshot.vsyncVulkanSwapchain)
    {
        // A Vulkan game whose swapchain creation this module cannot see.
        if (snapshot.graphicsApi != 2u || snapshot.vsyncVulkanHook) return false;
        if (snapshot.vsyncMode == 1)
            StatusDetail(T("V-Sync"), Tone::Dim, T("Off not applied"), T("override unavailable"));
        return true;
    }
    if (snapshot.vsyncMode != 1)
    {
        // The live swapchain still carries an earlier Off.
        if (failure == Failure::eSwapchainRecreationPending)
            StatusDetail(T("V-Sync"), Tone::Pending, T("Game"), T("applies when the game recreates its swapchain"));
        return true;
    }
    char detail[64]{};
    if (snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
    {
        // Vulkan API mode names, as in the debug readback; not translated.
        snprintf(detail, sizeof(detail), "Vulkan %s -> %s",
            VulkanPresentModeName(snapshot.vsyncOriginalPresentMode),
            VulkanPresentModeName(snapshot.vsyncSubmittedPresentMode));
        StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
    }
    else if (snapshot.vsyncPresentationObserved)
        StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
    else if (failure == Failure::eSwapchainRecreationPending)
        StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("applies when the game recreates its swapchain"));
    else if (failure == Failure::ePresentModeUnavailable)
        StatusDetail(T("V-Sync"), Tone::Dim, T("Off not applied"), T("not available on this GPU or driver"));
    else
        StatusDetail(T("V-Sync"), failure == Failure::eSwapchainCreateFailed ? Tone::Bad : Tone::Dim,
            T("Off not applied"), T("override unavailable"));
    return true;
}

// V-Sync and Reflex readback for the DLSS Status grid (and the XeSS panel under XeMFG standalone).
void PresentationStatusRows(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool xemfg = false)
{
    if (!BackendUsesNativeFile()) return;
    const bool changed = gPresentationChangedThisFrame;
    char detail[96]{};
    const bool vulkanRow = !statusStale && !changed
        && snapshot.vsyncMode == static_cast<uint32_t>(gVsyncMode) && VulkanVsyncRow(snapshot);
    if (!vulkanRow && gVsyncMode == 1)
    {
        const bool current = !statusStale && !changed && snapshot.vsyncMode == 1;
        if (current && snapshot.vsyncPresentationObserved && snapshot.vsyncOverrideApplied)
        {
            snprintf(detail, sizeof(detail), T("game interval %u submitted as 0"), snapshot.vsyncOriginalInterval);
            StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
        }
        else if (current && snapshot.vsyncPresentationObserved)
            StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
        else if (current && snapshot.vsyncFailure
                == static_cast<uint32_t>(vsync_control::Failure::ePresentFailed))
            StatusDetail(T("V-Sync"), Tone::Bad, T("Off not applied"), T("the game's last Present was not accepted"));
        else if (current)
            StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("waiting for the game's swapchain"));
        else
            StatusRow(T("V-Sync"), Tone::Pending, "%s", T("Applying..."));
    }
    // While XeMFG generates, the game's frames sleep in XeLL, which holds the limit; otherwise in the driver's Reflex.
    if (xemfg && gXessUi.xemfgGenerating && gReflexFrameLimitFps > 0)
    {
        char value[48]{};
        const auto& x = gXessUi;
        if (!statusStale && !changed && x.xemfgFpsLimited && x.xemfgFpsLimit == static_cast<uint32_t>(gReflexFrameLimitFps))
        {
            snprintf(value, sizeof(value), "%d FPS", gReflexFrameLimitFps);
            StatusDetail(T("Reflex limit"), Tone::Good, value, T("applied by XeLL to XeMFG's output"));
        }
        else
        {
            snprintf(value, sizeof(value), T("%d FPS requested"), gReflexFrameLimitFps);
            StatusDetail(T("Reflex limit"), Tone::Pending, value, T("applies with XeMFG's next frame"));
        }
        return;
    }
    const bool dynamic = !xemfg && (snapshot.dynamicMode || snapshot.appliedDynamicMode);
    if (gReflexFrameLimitFps > 0)
    {
        const uint32_t requestedUs = (1000000u + gReflexFrameLimitFps - 1u) / gReflexFrameLimitFps;
        char value[48]{};
        if (!dynamic && !statusStale && !changed && snapshot.reflexAppliedKnown && !snapshot.reflexLimitPending
            && !snapshot.reflexRestorePending && snapshot.reflexAppliedFrameLimitUs == requestedUs
            && snapshot.reflexStatus == static_cast<uint32_t>(reflex_control::Status::eApplied))
        {
            snprintf(value, sizeof(value), "%.2f FPS", 1000000.0 / snapshot.reflexAppliedFrameLimitUs);
            StatusDetail(T("Reflex limit"), Tone::Good, value, T("accepted"));
        }
        else
        {
            snprintf(value, sizeof(value), T("%d FPS requested"), gReflexFrameLimitFps);
            if (snapshot.reflexRestorePending)
                StatusDetail(T("Reflex limit"), Tone::Pending, value, T("restoring the game's Reflex limit"));
            else if (dynamic)
                StatusDetail(T("Reflex limit"), Tone::Dim, value, T("paused for Dynamic MFG"));
            else
                StatusDetail(T("Reflex limit"), Tone::Pending, value, statusStale
                    ? T("Reflex status is temporarily unavailable.") : ReflexPendingMessage(snapshot.reflexStatus));
        }
    }
    else if (snapshot.reflexRestorePending)
        StatusRow(T("Reflex limit"), Tone::Pending, "%s", T("Restoring the game's Reflex limit..."));
}

void DrawPresentationDebug(const MfgUnlockReShadeSnapshot& snapshot)
{
    ImGui::Text("V-Sync: requested=%u available=%u observed=%u applied=%u interval=%u->%u failure=%s",
        snapshot.vsyncMode, snapshot.vsyncControlAvailable, snapshot.vsyncPresentationObserved,
        snapshot.vsyncOverrideApplied, snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval,
        vsync_control::FailureName(static_cast<vsync_control::Failure>(snapshot.vsyncFailure)));
    ImGui::Text("V-Sync Vulkan: swapchain=%u hook=%u presentMode=%s->%s",
        snapshot.vsyncVulkanSwapchain, snapshot.vsyncVulkanHook,
        VulkanPresentModeName(snapshot.vsyncOriginalPresentMode),
        VulkanPresentModeName(snapshot.vsyncSubmittedPresentMode));
    ImGui::Text("Reflex: available=%u accepted=%u limit=%u us pending=%u restore=%u status=%u result=%d",
        snapshot.reflexControlAvailable, snapshot.reflexAppliedKnown, snapshot.reflexAppliedFrameLimitUs,
        snapshot.reflexLimitPending, snapshot.reflexRestorePending, snapshot.reflexStatus,
        snapshot.reflexLastResult);
    ImGui::Text("Reflex module: %u.%u.%u generation=%llu hooks=0x%X",
        snapshot.reflexModuleVersionMajor, snapshot.reflexModuleVersionMinor,
        snapshot.reflexModuleVersionPatch, static_cast<unsigned long long>(snapshot.reflexModuleGeneration),
        snapshot.reflexHookMask);
}

// rtxhair.1: universal hair, on in every game (Witcher 3 included) unless RTXMFG.RtxHair.ini turns it off. The game's
// own RTX hair path runs; this panel shows whether its hair builds are being converted.
void DrawUniversalHair()
{
    const auto status = rtx_hair::universal::ReadSnapshot();
    using rtx_hair::universal::State;
    Section(T("Path traced hair"));
    const bool prepared = status.state == State::Prepared;
    if (BeginGrid("##universalhair"))
    {
        const char* text = T("Preparing");
        const char* detail = "";
        Tone tone = Tone::Pending;
        switch (status.state)
        {
        case State::Off:
        case State::Waiting: break;
        case State::NotAdmitted: text = T("Unavailable"); tone = Tone::Dim; break;
        case State::Failed: text = T("Unavailable"); tone = Tone::Bad; break;
        case State::Prepared:
            if (status.lastBuildAgeMs < 2000) { text = T("Active"); detail = T("universal: game RTX hair converted"); tone = Tone::Good; }
            else if (status.builds) { text = T("Idle"); detail = T("no hair built in the last 2 s"); tone = Tone::Normal; }
            else { text = T("Ready"); detail = T("waiting for game hair"); tone = Tone::Normal; }
            break;
        }
        StatusDetail(T("State"), tone, text, detail);
        HelpMarker(T("Universal hair runs the game's own RTX 50 hair on this GPU. It is on by default; set Universal=0 under [RtxHair] in RTXMFG.RtxHair.ini beside the game to turn it off."));
        if (prepared)
        {
            char updates[64]{};
            snprintf(updates, sizeof(updates), T("%s updates"), Grouped(status.updates).c_str());
            StatusDetail(T("Builds"), Tone::Normal, Grouped(status.builds).c_str(), updates);
            char refused[64]{};
            if (status.shadersRefused) snprintf(refused, sizeof(refused), T("%s not converted"), Grouped(status.shadersRefused).c_str());
            StatusDetail(T("Hair shaders"), status.shadersRefused ? Tone::Pending : Tone::Normal, Grouped(status.shadersRewritten).c_str(), refused);
        }
        ImGui::EndTable();
    }
    if (status.reason[0]) Note(status.state == State::Failed ? Tone::Bad : Tone::Dim, "%s", status.reason);
}

// Diagnostics stay in English (locale/README.md).
void DrawUniversalHairDiagnostics()
{
    const auto status = rtx_hair::universal::ReadSnapshot();
    if (status.state != rtx_hair::universal::State::Prepared) return;
    if (!ImGui::CollapsingHeader(l10n::Label("Hair diagnostics"))) return;
    constexpr double mib = 1024.0 * 1024.0;
    if (BeginGrid("##universalhaircounters"))
    {
        if (status.vulkan && status.ribbons == 2)
            StatusRow("Mode", Tone::Normal, "%s", "universal (Vulkan LSS -> DOTS, 2 ribbons, 4 triangles per segment)");
        else
            StatusRow("Mode", Tone::Normal, "%s", "universal (NVAPI LSS -> DOTS, 3 ribbons, 6 triangles per segment)");
        StatusRow("LSS capability answers", Tone::Normal, "%s", Grouped(status.capsAnswered).c_str());
        StatusRow("LSS pipeline options", Tone::Normal, "%s stripped", Grouped(status.optionsStripped).c_str());
        StatusRow("Prebuilds", Tone::Normal, "%s", Grouped(status.prebuilds).c_str());
        StatusRow("Builds", Tone::Normal, "%s (%s updates)", Grouped(status.builds).c_str(), Grouped(status.updates).c_str());
        StatusRow("Builds without hair", status.inactiveBuilds ? Tone::Pending : Tone::Normal, "%s (%s updates skipped)",
            Grouped(status.inactiveBuilds).c_str(), Grouped(status.skippedUpdates).c_str());
        StatusRow("Build failures", status.failures ? Tone::Bad : Tone::Normal, "%s", Grouped(status.failures).c_str());
        StatusRow("Full rebuilds", Tone::Normal, "%s (refitted hair rebuilt in place)", Grouped(status.rebuilds).c_str());
        StatusRow("Scene builds", status.topLevelUnpatched ? Tone::Pending : Tone::Normal, "%s with hair culling off, %s left as they were",
            Grouped(status.topLevelPatched).c_str(), Grouped(status.topLevelUnpatched).c_str());
        if (status.vulkan) // hairmem.1: what is held now
            StatusRow("Hair BLASes", Tone::Normal, "%s, the game's %.1f MiB (instance copies %.1f MiB)", Grouped(status.hairBlas).c_str(),
                status.gameHairBlasBytes / mib, status.instanceCopyBytes / mib);
        else
            StatusRow("Hair BLASes", Tone::Normal, "%s (instance copies %.1f MiB)", Grouped(status.hairBlas).c_str(), status.instanceCopyBytes / mib);
        if (status.witcherLook)
            StatusRow("Witcher look fixes", status.lookRefused || status.lookFailures ? Tone::Pending : Tone::Normal, "%s",
                status.lookRefused ? "off (the driver refused them)"
                : (Grouped(status.lookShaders) + " shaders" + (status.lookFailures ? ", " + Grouped(status.lookFailures) + " failed" : std::string())
                    + " (RTXMFG.WitcherDOTS.ini)").c_str());
        StatusRow("Vertex pool", Tone::Normal, "%.1f MiB in %s buffers, %s released", status.poolBytes / mib,
            Grouped(status.poolBuffers).c_str(), Grouped(status.evictions).c_str());
        if (status.vulkan)
        {
            StatusRow("Spare buffers", Tone::Normal, "%.1f MiB spare, %.1f MiB retiring, %s reused", status.spareBytes / mib, status.retiredBytes / mib,
                Grouped(status.reuses).c_str());
        }
        ImGui::EndTable();
    }
    if (status.lastError[0]) Note(Tone::Dim, "Last builder message: %s", status.lastError);
}

// Witcher 3 only. The fallback is always prepared there; the game's own
// HairWorks / Path Traced Hair settings turn hair on and off.
void DrawWitcherHair()
{
    if (rtx_hair::universal::Requested()) {DrawUniversalHair();return;}
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable) return;
    Section(T("Path traced hair"));
    const bool active = status.stage == witcher_dots::Stage::Active;
    if (BeginGrid("##hair"))
    {
        if (status.environmentOverride)
            StatusDetail(T("State"), Tone::Dim, T("Off"), "RTXMFG_WITCHER_DOTS=0");
        else
        {
            // witcher_dots::ActivityText as a value and a detail, e.g. "Active" + "tracing converted hair".
            const char* text = T("Unknown");
            const char* detail = "";
            switch (status.stage)
            {
            case witcher_dots::Stage::Disabled: text = T("Disabled"); break;
            case witcher_dots::Stage::WaitingForDevice: text = T("Waiting for early device"); break;
            case witcher_dots::Stage::Preparing: text = T("Preparing"); break;
            case witcher_dots::Stage::UnsupportedGame: text = T("Unsupported game build"); break;
            case witcher_dots::Stage::UnsupportedGpu: text = T("Unsupported GPU"); break;
            case witcher_dots::Stage::NativeLss: text = T("Native LSS"); break;
            case witcher_dots::Stage::Failed: text = T("Unavailable"); break;
            case witcher_dots::Stage::UnsupportedDriver: text = T("Unsupported driver"); break;
            case witcher_dots::Stage::Active:
                if (!status.gameHairTraced) { text = T("Off"); detail = T("game Path Traced Hair setting"); }
                else if (status.lastHairAgeMs < 2000) { text = T("Active"); detail = T("tracing converted hair"); }
                else if (status.builds) { text = T("Idle"); detail = T("no hair traced in the last 2 s"); }
                else { text = T("Ready"); detail = T("waiting for game hair"); }
                break;
            }
            Tone tone = Tone::Normal;
            switch (status.stage)
            {
            case witcher_dots::Stage::Active:
                tone = !status.gameHairTraced ? Tone::Dim : status.lastHairAgeMs < 2000 ? Tone::Good : Tone::Normal;
                break;
            case witcher_dots::Stage::Disabled: tone = Tone::Dim; break;
            case witcher_dots::Stage::Failed: tone = Tone::Bad; break;
            case witcher_dots::Stage::NativeLss: tone = Tone::Normal; break;
            default: tone = Tone::Pending; break;
            }
            StatusDetail(T("State"), tone, text, detail);
            HelpMarker(T("Turn it on or off with HairWorks and Path Traced Hair in the game's graphics settings."));
            if (active || status.builds)
            {
                StatusRow(T("Traced hair"), Tone::Normal, "%u", status.lastHairAgeMs < 2000 ? status.hairInstances : 0u);
                char updates[64]{};
                snprintf(updates, sizeof(updates), T("%s updates"), Grouped(status.updates).c_str());
                StatusDetail(T("Builds"), Tone::Normal, Grouped(status.builds).c_str(), updates);
            }
        }
        ImGui::EndTable();
    }
    if (!status.environmentOverride && status.reason[0])
        Note(active ? Tone::Dim : Tone::Pending, "%s", status.reason);
}

void DrawWitcherHairDiagnostics()
{
    if (rtx_hair::universal::Requested()) {DrawUniversalHairDiagnostics();return;}
    const auto status = witcher_dots::ReadSnapshot();
    if (!status.applicable || status.environmentOverride) return;
    if (status.stage != witcher_dots::Stage::Active && !status.builds) return;
    if (!ImGui::CollapsingHeader(l10n::Label("Hair diagnostics"))) return;
    constexpr double mib = 1024.0 * 1024.0, gib = 1024.0 * 1024.0 * 1024.0;
    // DOTS time per second by hook, sampled once a second.
    static uint64_t sampleTick = 0, sampleBuild = 0, sampleCopy = 0, sampleCalls = 0, sampleInstances = 0,
        sampleWait = 0, sampleHeld = 0;
    static double buildMs = 0, copyMs = 0, callsPerSecond = 0, instancesPerSecond = 0, waitMs = 0, heldMs = 0;
    const uint64_t tick = GetTickCount64();
    if (!sampleTick || tick - sampleTick >= 1000)
    {
        if (sampleTick)
        {
            const double seconds = (tick - sampleTick) / 1000.0;
            buildMs = (status.buildMicroseconds - sampleBuild) / 1000.0 / seconds;
            copyMs = (status.copyMicroseconds - sampleCopy) / 1000.0 / seconds;
            callsPerSecond = (status.copyCalls - sampleCalls) / seconds;
            instancesPerSecond = (status.instancesScanned - sampleInstances) / seconds;
            waitMs = (status.buildLockWaitMicroseconds - sampleWait) / 1000.0 / seconds;
            heldMs = (status.buildLockHeldMicroseconds - sampleHeld) / 1000.0 / seconds;
        }
        sampleTick = tick; sampleBuild = status.buildMicroseconds; sampleCopy = status.copyMicroseconds;
        sampleWait = status.buildLockWaitMicroseconds; sampleHeld = status.buildLockHeldMicroseconds;
        sampleCalls = status.copyCalls; sampleInstances = status.instancesScanned;
    }
    char value[64]{}, detail[128]{};
    ImGui::TextDisabled("Memory");
    if (BeginGrid("##hairmemory"))
    {
        StatusRow("Vertex pool", Tone::Normal, "%.1f MiB", status.geometryBytes / mib);
        StatusRow("Hair BLAS", Tone::Normal, "%.0f MiB", status.hairBlasBytes / mib);
        StatusRow("Scratch", Tone::Normal, "%.0f MiB", status.hairScratchBytes / mib);
        if (status.memoryKnown)
        {
            snprintf(value, sizeof(value), "%.2f of %.2f GiB", status.vramUsage / gib, status.vramBudget / gib);
            snprintf(detail, sizeof(detail), "shared %.2f GiB", status.sharedUsage / gib);
            StatusDetail("Game VRAM", Tone::Normal, value, detail);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("CPU per second");
    if (BeginGrid("##haircpu"))
    {
        snprintf(value, sizeof(value), "%.2f ms", buildMs);
        snprintf(detail, sizeof(detail), "lock wait %.2f, under lock %.2f, other %.2f ms",
            waitMs, heldMs, buildMs > waitMs + heldMs ? buildMs - waitMs - heldMs : 0.0);
        StatusDetail("Hair builds", Tone::Normal, value, detail);
        snprintf(value, sizeof(value), "%.2f ms", copyMs);
        snprintf(detail, sizeof(detail), "%.0f calls, %.0fk instances", callsPerSecond, instancesPerSecond / 1000.0);
        StatusDetail("Instance copies", Tone::Normal, value, detail);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Counters");
    if (BeginGrid("##haircounters"))
    {
        StatusRow("Prebuilds", Tone::Normal, "%s", Grouped(status.prebuilds).c_str());
        StatusRow("Shader libraries", Tone::Normal, "%s", Grouped(status.shaderLibraries).c_str());
        StatusRow("Instance copies", Tone::Normal, "%s", Grouped(status.instanceCopies).c_str());
        snprintf(detail, sizeof(detail), "%s released", Grouped(status.evictions).c_str());
        snprintf(value, sizeof(value), "%u", status.liveOwners);
        StatusDetail("Live hair owners", Tone::Normal, value, detail);
        StatusRow("Pool", Tone::Normal, "%s allocated, %s released, %s returned",
            Grouped(status.poolAllocations).c_str(), Grouped(status.poolReleases).c_str(),
            Grouped(status.poolReturns).c_str());
        StatusRow("Full rebuilds", Tone::Normal, "%s", Grouped(status.fullRebuilds).c_str());
        StatusRow("Fallback checks", Tone::Normal, "%s", Grouped(status.rejected).c_str());
        ImGui::EndTable();
    }
    bool report = status.crashReportRequested;
    static bool reportSaveFailed = false;
    if (ImGui::Checkbox("GPU crash report, next launch", &report))
        reportSaveFailed = !witcher_dots::SetCrashReportRequested(report);
    HelpMarker("Enables D3D12 DRED breadcrumbs for the game's device. If the GPU crashes, the report is written to\n"
        "RTXMFG-witcher3-device-removed-<pid>.txt in the game folder (bin\\x64_dx12), or to %TEMP% if that\n"
        "folder cannot be written. Costs a little GPU time while enabled.");
    if (reportSaveFailed) Note(Tone::Bad, "Could not save the crash report setting.");
    if (status.crashReportArmed) Note(Tone::Good, "Crash report armed for this session.");
    else if (status.crashReportRequested) Note(Tone::Pending, "Crash report starts with the next launch.");
}

constexpr uint32_t kIntelVendor = 0x8086;
constexpr uint32_t kNvidiaVendor = 0x10de;

const char* VendorName(uint32_t vendor) noexcept
{
    switch (vendor)
    {
    case kNvidiaVendor: return "NVIDIA";
    case 0x1002: return "AMD";
    case kIntelVendor: return "Intel";
    default: return T("Unknown");
    }
}

uint32_t GameGpuVendor() noexcept
{
    return xess_ui::Vendor(gXessUi);
}

// Which frame generation the game is using decides which settings are shown.
const char* ActiveFrameGenerationName() noexcept
{
    const auto& active = gXessUi.activeFrameGeneration;
    return active == "xess" ? "XeSS (Intel)" : active == "dlss" ? "DLSS (NVIDIA)" : T("Off");
}

bool XemfgAvailable() noexcept;

// Shown under the frame generation controls until the game turns frame
// generation on. Returns whether it was shown.
bool FrameGenerationOffNote(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (gXessUi.activeFrameGeneration != "none") return false;
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    const bool xessOffered = gXessUi.hooksActive != FALSE;
    if (dlssOffered && xessOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (DLSS or XeSS)."));
    else if (dlssOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (DLSS)."));
    else if (xessOffered)
        Note(Tone::Pending, T("Turn on frame generation in the game's settings (XeSS)."));
    // A game with neither gets frames from XeMFG standalone only.
    else if (XemfgAvailable() && !gXessUi.xemfgStandalone)
        Note(Tone::Pending, T("Turn on DLSS or XeSS frame generation in the game's settings. "
            "If the game has none, turn on XeMFG standalone."));
    else
        Note(Tone::Pending, T("Turn on DLSS or XeSS frame generation in the game's settings."));
    return true;
}

// Game FG and GPU rows; familyDetail adds the DLSS GPU family.
void GameRows(const MfgUnlockReShadeSnapshot& snapshot, bool familyDetail)
{
    const bool off = gXessUi.activeFrameGeneration == "none";
    StatusRow(T("Game FG"), off ? Tone::Dim : Tone::Normal, "%s", ActiveFrameGenerationName());
    const uint32_t vendor = GameGpuVendor();
    const std::string gpu = !vendor ? std::string() : gXessUi.adapter.empty()
        ? std::string(VendorName(vendor)) : gXessUi.adapter;
    const char* family = !familyDetail ? "" : snapshot.gpuFamily == 1u ? "Ada (RTX 40)"
        : snapshot.gpuFamily == 2u ? "Ampere (RTX 30)" : snapshot.gpuFamily == 4u ? "Turing (RTX 20)"
        : snapshot.gpuFamily == 5u ? "Blackwell (RTX 50)" : "";
    if (!gpu.empty())
        StatusDetail(T("GPU"), Tone::Normal, gpu.c_str(), family);
    else if (familyDetail && family[0])
        StatusRow(T("GPU"), Tone::Normal, "%s", family);
    else if (familyDetail)
        StatusRow(T("GPU"), Tone::Pending, "%s", T("Waiting for a supported game GPU"));
}

// GPU name override: for games that remove their DLSS Frame Generation
// option on RTX 30. Saved beside the game; applies at the next game start. Shown on
// RTX 30, and whenever it is saved or active so it can always be turned off.
struct GpuNameView { bool loaded = false; bool saved = false; bool saveFailed = false; };
GpuNameView gGpuName;

bool GpuNameShown() noexcept
{
    if (!gGpuName.loaded)
    {
        gGpuName.saved = gpu_name_override::SavedOn();
        gGpuName.loaded = true;
    }
    return UiAmpere() || gGpuName.saved || gpu_name_override::Read().enabled;
}

void GpuNameControlRow()
{
    if (!GpuNameShown()) return;
    ControlRow(T("Game FG option"));
    bool unlock = gGpuName.saved;
    if (ImGui::Checkbox(l10n::Label("Unlock"), &unlock))
    {
        gGpuName.saveFailed = !gpu_name_override::Save(unlock);
        if (!gGpuName.saveFailed) gGpuName.saved = unlock;
    }
    std::string gpuHint = T("For games that hide DLSS Frame Generation on RTX 30 cards, such as Wuthering Waves.\n"
        "The game then sees an NVIDIA GeForce RTX 4090. DLSS, Streamline, the driver and MFG Unlocker\n"
        "keep using your real GPU. The game's recommended settings may assume an RTX 4090.\n"
        "Online games with anti cheat may treat any mod as a risk. Applies after a game restart.");
    if (gGpuFamily.load(std::memory_order_acquire) == 4u)
    {
        const auto at = gpuHint.find("RTX 30");
        if (at != std::string::npos) gpuHint.replace(at, 6, "RTX 20");
    }
    HelpMarker(gpuHint.c_str());
}

void GpuNameNotes()
{
    if (!GpuNameShown()) return;
    const auto state = gpu_name_override::Read();
    if (gGpuName.saveFailed)
        Note(Tone::Bad, T("The Game FG option could not be saved."));
    else if (gGpuName.saved != state.enabled)
        Note(Tone::Pending, "%s", gGpuName.saved ? T("Restart the game to unlock its Frame Generation option.")
                                                 : T("Restart the game to report your real GPU again."));
    else if (state.enabled && !state.armed)
        Note(Tone::Bad, T("The Game FG option could not be applied this session. See the log."));
    else if (state.enabled && state.reported)
        Note(Tone::Dim, T("The game sees an NVIDIA GeForce RTX 4090 this session."));
    else if (state.enabled && state.unchanged)
        Note(Tone::Dim, T("This GPU is not an RTX 30 card; the game sees its real name."));
}

// GitHub issue 122: the runtime log toggle (runtime_log_setting.h), in every menu's Diagnostics. One view per drawing
// thread (the overlay's render thread, OptiScaler's menu thread).
struct RuntimeLogView { bool loaded = false; bool saved = true; bool saveFailed = false; };
RuntimeLogView gRuntimeLog;

bool RuntimeLogSaved(RuntimeLogView& view)
{
    if (!view.loaded)
    {
        view.saved = runtime_log_setting::SavedOn();
        view.loaded = true;
    }
    return view.saved;
}

void RuntimeLogSave(RuntimeLogView& view, bool on)
{
    view.saveFailed = !runtime_log_setting::Save(on);
    if (!view.saveFailed) view.saved = on;
}

const char* RuntimeLogHelp()
{
    return T("Writes the MFG Unlocker log beside the game. Bug reports need it.\n"
        "Turn it off to stop writing the log. Applies after a game restart.");
}

// A failed save, or a saved choice this session has not applied yet; nullptr otherwise.
const char* RuntimeLogNote(const RuntimeLogView& view, Tone& tone)
{
    const int session = runtime_log_setting::Session();
    tone = view.saveFailed ? Tone::Bad : Tone::Pending;
    if (view.saveFailed) return T("The log setting could not be saved.");
    if (session < 0 || view.saved == (session == 1)) return nullptr;
    return view.saved ? T("Restart the game to write the log again.") : T("Restart the game to stop writing the log.");
}

void RuntimeLogToggle()
{
    bool on = RuntimeLogSaved(gRuntimeLog);
    if (ImGui::Checkbox(l10n::Label("Write log file"), &on))
        RuntimeLogSave(gRuntimeLog, on);
    HelpMarker(RuntimeLogHelp());
    Tone tone = Tone::Pending;
    if (const char* note = RuntimeLogNote(gRuntimeLog, tone))
        Note(tone, "%s", note);
}

// The shared menu beside OptiScaler's: Diagnostics holds only the log toggle.
void RuntimeLogDiagnostics()
{
    Section(T("Advanced"));
    if (ImGui::CollapsingHeader(l10n::Label("Diagnostics")))
        RuntimeLogToggle();
}

// XeSS frame generation: status only. XeSS MFG runs under OptiScaler, whose menu owns its
// controls (the frame generation ratio and the MFG model). Without OptiScaler the game's XeSS frame
// generation stays at Intel's 2x and RTXMFG leaves it untouched.
// XeMFG standalone, MFG Unlocker as the XeFG host without OptiScaler (OptiScaler's XeFG output runs XeMFG
// under OptiScaler). Its rows sit in the frame generation grid of both panels: switched on, the XeSS panel shows on
// every GPU (xess_ui::SettingsSection).
bool gXemfgSaveFailed = false;

bool XemfgAvailable() noexcept
{
    return BackendUsesNativeFile() && !gXessUi.xemfgState.empty() && !gXessUi.optiScaler;
}

// The last custom Dynamic target, kept while "Display refresh rate" is ticked.
int gXemfgCustomTargetFps = 120;

void XemfgControlRows()
{
    auto& x = gXessUi;
    auto wanted = x;
    bool on = x.xemfgStandalone != FALSE;
    int index = static_cast<int>(std::clamp<uint32_t>(x.xemfgSavedFrames ? x.xemfgSavedFrames : 2, 2, 6)) - 2;
    bool changed = false;
    ControlRow(T("XeMFG standalone"));
    changed |= ImGui::Checkbox("##xemfgstandalone", &on);
    HelpMarker(T("MFG Unlocker runs XeSS multi-frame generation itself, without OptiScaler, from the inputs of the "
        "game's DLSS frame generation or of its DLSS, XeSS or FSR 3.1 upscaling. It replaces the game's own frame "
        "generation.\nSwitching it on or off applies at once: MFG Unlocker briefly resizes the game's window so the game "
        "hands its swap chain over. If the game ignores that, it applies at the game's next resolution or window-mode "
        "change, or after a restart."));
    if (on)
    {
        static const char* modes[] = {"2x", "3x", "4x", "5x", "6x"};
        // Dynamic MFG, our OptiScaler XeFG output's (xess/xemfg_dynamic.h), up to the frames set.
        bool dynamic = x.xemfgDynamic != FALSE;
        const char* dynamicLabel = T("Dynamic");
        ControlRow(T("MFG mode"));
        if (ImGui::BeginCombo("##xemfgmode", dynamic ? dynamicLabel : modes[index]))
        {
            for (int i = 0; i < IM_ARRAYSIZE(modes); ++i)
                if (ImGui::Selectable(modes[i], !dynamic && index == i)) { index = i; dynamic = false; changed = true; }
            if (ImGui::Selectable(dynamicLabel, dynamic)) { dynamic = true; changed = true; }
            ImGui::EndCombo();
        }
        HelpMarker(T("Dynamic changes how many frames XeMFG generates every game frame, so its frame rate follows the "
            "target, up to the frames set. The target is not a limit."));
        wanted.xemfgDynamic = dynamic ? TRUE : FALSE;
        if (dynamic)
        {
            ControlRow(T("Up to"));
            changed |= ImGui::Combo("##xemfgmost", &index, modes, IM_ARRAYSIZE(modes));
            bool automatic = x.xemfgTargetFps == 0;
            if (x.xemfgTargetFps) gXemfgCustomTargetFps = static_cast<int>(x.xemfgTargetFps);
            ControlRow(T("Dynamic target"));
            if (ImGui::Checkbox(l10n::Label("Display refresh rate"), &automatic)) changed = true;
            HelpMarker(T("The FPS limit when one is set, otherwise just under the display's refresh rate."));
            if (!automatic)
            {
                ControlRow(T("Target FPS"));
                ImGui::DragInt("##xemfgtarget", &gXemfgCustomTargetFps, 1.0f, 1, 1000, "%d FPS");
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            gXemfgCustomTargetFps = std::clamp(gXemfgCustomTargetFps, 1, 1000);
            wanted.xemfgTargetFps = automatic ? 0u : static_cast<uint32_t>(gXemfgCustomTargetFps);
            // The controller's rounding values: 0 at most, 1 nearest, 2 at least.
            const char* roundings[] = {T("Closest"), T("At least"), T("At most")};
            static constexpr uint32_t kRounding[] = {1, 2, 0};
            int rounding = x.xemfgRounding == 2 ? 1 : x.xemfgRounding == 0 ? 2 : 0;
            ImGui::BeginDisabled(x.xemfgTargetAsCap != FALSE);
            ControlRow(T("Rounding"));
            if (ImGui::Combo("##xemfgrounding", &rounding, roundings, IM_ARRAYSIZE(roundings))) changed = true;
            HelpMarker(T("Closest: the frame rate closest to the target. At least: the fewest frames that reach it. "
                "At most: the most frames that stay under it."));
            wanted.xemfgRounding = kRounding[std::clamp(rounding, 0, 2)];
            bool allow1x = x.xemfgAllow1x != FALSE;
            ControlRow(T("Allow 1X"));
            if (ImGui::Checkbox("##xemfgallow1x", &allow1x)) changed = true;
            HelpMarker(T("Lets Dynamic turn frame generation off when the game's own frames come nearest the target."));
            wanted.xemfgAllow1x = allow1x ? TRUE : FALSE;
            ImGui::EndDisabled();
            bool cap = x.xemfgTargetAsCap != FALSE;
            ControlRow(T("Use target as cap"));
            if (ImGui::Checkbox("##xemfgcap", &cap)) changed = true;
            HelpMarker(T("Holds XeMFG's frame rate at the target with the FPS limit and uses the fewest frames that "
                "reach it, never 1X."));
            wanted.xemfgTargetAsCap = cap ? TRUE : FALSE;
        }
    }
    if (changed)
    {
        gXemfgSaveFailed = !NativeFilePersistXemfg(on, static_cast<uint32_t>(index + 2), wanted);
        if (!gXemfgSaveFailed)
        {
            // Shown at once; the next status read confirms it.
            x = wanted;
            x.xemfgStandalone = on ? TRUE : FALSE;
            x.xemfgSavedFrames = static_cast<uint32_t>(index + 2);
        }
    }
}

// After a failed runtime search, where the pair belongs (the real folders, the game's EXE folder
// named, which Unreal games keep in Binaries\Win64), what was missing, and the restart the once-per-launch search needs.
void XemfgRuntimeNotes()
{
    const auto& x = gXessUi;
    const std::string& why = x.xemfgReason;
    if (why == "runtime_path_too_long")
    {
        Note(Tone::Bad, T("The game's folder path is too long (260 characters or more) for XeMFG to search it."));
        return;
    }
    if (x.xemfgRuntimeSearch.empty())
        Note(Tone::Bad, T("XeMFG needs libxess_fg.dll and libxell.dll in an RTXMFG-XeSS folder beside MFG Unlocker."));
    else
        Note(Tone::Bad, T("XeMFG needs libxess_fg.dll and libxell.dll together in %s (or beside the game's exe in %s). "
            "Restart the game after adding them."), x.xemfgRuntimeSearch.c_str(), x.xemfgRuntimeGame.c_str());
    if (why == "runtime_missing_libxess_fg") Note(Tone::Bad, T("Only libxell.dll was found."));
    else if (why == "runtime_missing_libxell") Note(Tone::Bad, T("Only libxess_fg.dll was found."));
    else if (why == "runtime_split") Note(Tone::Bad, T("The two files are in different folders."));
    else if (why == "runtime_misnamed" && !x.xemfgRuntimeMisnamed.empty())
        Note(Tone::Bad, T("Found %s, not %s: check the file's name and extension."), x.xemfgRuntimeMisnamed.c_str(),
            _strnicmp(x.xemfgRuntimeMisnamed.c_str(), "libxess_fg", 10) == 0 ? "libxess_fg.dll" : "libxell.dll");
}

// oldruntime.1: the pair loaded but lacks exports XeMFG calls (a game's own older copy, such as Cyberpunk's
// libxess_fg 1.1.0.19 beside its exe): the file, its folder and version, and where the tested SDK's pair takes over.
void XemfgOutdatedRuntimeNotes()
{
    const auto& x = gXessUi;
    if (!x.xemfgRuntimeOldVersion.empty())
        Note(Tone::Bad, T("The %s in %s is version %s, which is too old for XeMFG."), x.xemfgRuntimeOldFile.c_str(),
            x.xemfgRuntimeOld.c_str(), x.xemfgRuntimeOldVersion.c_str());
    else
        Note(Tone::Bad, T("The %s in %s is too old for XeMFG."), x.xemfgRuntimeOldFile.c_str(), x.xemfgRuntimeOld.c_str());
    if (!x.xemfgRuntimeNewer.empty())
        Note(Tone::Bad, T("Put libxess_fg.dll and libxell.dll from Intel's XeSS SDK %s in %s, then restart the game."),
            xess_runtime_search::kTestedSdk, x.xemfgRuntimeNewer.c_str());
}

// xemfglive.1: the resize a running game's swap chain moves in did not come (the host's ResizeWindow reasons).
bool XemfgResizeReason(const std::string& why)
{
    return why == "resize_ignored" || why == "resize_ignored_remembered" || why == "window_minimized"
        || why == "child_window" || why == "window_size" || why == "window_rect" || why == "window_gone"
        || why == "set_window_pos" || why == "resize_thread";
}

// xemfglive.1: why the running game's D3D12 swap chain did not move to XeFG, and what applies the switch instead.
void XemfgRestartNotes()
{
    const auto& x = gXessUi;
    const std::string& why = x.xemfgReason;
    if (x.xemfgApi != "d3d12" || why.empty())
        Note(Tone::Pending, T("Restart the game to start XeMFG."));
    else if (why == "resize_ignored_remembered")   // xemfglive.3: [XeMFG] ProxyBackBuffers=1 was written beside the game
        Note(Tone::Pending, T("This game ignores window size changes, so XeMFG starts after a restart. From then on, "
            "XeMFG switches on and off in this game without a restart."));
    else if (XemfgResizeReason(why))
        Note(Tone::Pending, T("The game did not resize its swap chain, so XeMFG has not started yet. It starts at the "
            "game's next resolution or window-mode change, or after a restart."));
    else if (why == "fullscreen_exclusive")
        Note(Tone::Pending, T("XeMFG cannot take over a swap chain in exclusive fullscreen. Switch the game to borderless "
            "or windowed, or restart the game."));
    else
        Note(Tone::Bad, T("XeMFG could not take over the running game's swap chain (%s). Restart the game to start XeMFG."),
            why.c_str());
}

// Notes for the rows above: what XeMFG waits for. Reasons come from the host (xess/d3d12_host.cpp, vk_host.cpp).
void XemfgNotes()
{
    const auto& x = gXessUi;
    if (gXemfgSaveFailed) Note(Tone::Bad, T("The XeMFG setting could not be saved."));
    if (!x.xemfgStandalone)
    {
        // xemfglive.1: switched off while XeFG still has the game's chain (the game has not resized since).
        if (x.xemfgState == "returning")
            Note(Tone::Pending, T("XeMFG is off, but XeFG keeps the game's swap chain until the game's next resolution or "
                "window-mode change, or a restart."));
        // xemfglive.5: XeMFG took the swap chain of the game's DLSS Frame Generation in this session.
        else if (x.xemfgState == "held")
            Note(Tone::Pending, T("XeMFG is off. The game's DLSS frame generation comes back after a restart: it cannot "
                "start again in this session once XeMFG has taken over its swap chain."));
        return;
    }
    const std::string& state = x.xemfgState;
    const std::string& why = x.xemfgReason;
    if (state == "switching")
        Note(Tone::Pending, T("Moving the game's swap chain to XeMFG..."));
    else if (state == "restart")
        XemfgRestartNotes();
    else if (xess_runtime_search::SearchReason(why))
        XemfgRuntimeNotes(); // D3D12 reports these as unavailable, Vulkan as waiting: the same notes for both
    else if (why == "runtime_exports_missing" && !x.xemfgRuntimeOld.empty() && !x.xemfgRuntimeOldFile.empty())
        XemfgOutdatedRuntimeNotes(); // the same on both APIs too
    else if (state == "unavailable")
        Note(Tone::Bad, T("XeMFG could not start: %s"), why.c_str());
    else if (why == "game_frame_generation_off")
        Note(Tone::Pending, T("Turn on DLSS frame generation in the game's settings. XeMFG replaces it."));
    else if (why == "game_fsr_frame_generation")
        Note(Tone::Pending, T("Turn off the game's FSR frame generation. XeMFG does not run on top of it."));
    else if (why == "motion_vector_size")
        Note(Tone::Bad, T("The game's motion vectors are at display resolution. XeMFG cannot use them yet."));
    else if (state == "waiting")
        Note(Tone::Pending, T("Waiting for the game's DLSS, XeSS or FSR 3.1 upscaling, or its DLSS frame generation."));
}

void XemfgStatusRows()
{
    const auto& x = gXessUi;
    const std::string& state = x.xemfgState;
    if (!x.xemfgStandalone || state.empty() || state == "off" || state == "optiscaler") return;
    if (state == "active")
    {
        char value[64]{};
        // Dynamic MFG's count (1x: frame generation off).
        if (x.xemfgDynamicActive) snprintf(value, sizeof(value), T("Dynamic, %ux"), x.xemfgFrames);
        else snprintf(value, sizeof(value), "%ux", x.xemfgFrames);
        StatusRow("XeMFG", Tone::Good, "%s", x.xemfgFrames >= 1 ? value : T("Active"));
        if (x.xemfgDynamicActive && x.xemfgDynamicTargetMilli)
        {
            snprintf(value, sizeof(value), "%.0f FPS", x.xemfgDynamicTargetMilli / 1000.0);
            if (x.xemfgDynamicCap) StatusDetail(T("Dynamic target"), Tone::Normal, value, T("held as cap"));
            else StatusRow(T("Dynamic target"), Tone::Normal, "%s", value);
        }
    }
    else if (state == "switching")
        StatusRow("XeMFG", Tone::Pending, "%s", T("Starting"));
    else if (state == "restart")
        StatusRow("XeMFG", Tone::Pending, "%s", x.xemfgApi == "d3d12" && XemfgResizeReason(x.xemfgReason)
            ? T("Waiting for a resize") : T("Restart needed"));
    else if (state == "unavailable")
        StatusDetail("XeMFG", Tone::Bad, T("Unavailable"), x.xemfgReason.c_str());
    else
        StatusDetail("XeMFG", Tone::Pending, T("Waiting"), x.xemfgReason.c_str());
    const char* source = x.xemfgSource == "streamline" ? "Streamline DLSS-G" : x.xemfgSource == "dlss" ? "DLSS"
        : x.xemfgSource == "xess" ? "XeSS" : x.xemfgSource == "fsr" ? "FSR" : nullptr;
    if (source) StatusRow(T("Inputs"), Tone::Normal, "%s", source);
    // The FPS row and its pin, as in the DLSS panel (DLSS-G's own counters stay at Off while XeMFG replaces
    // it): the game's frames and every frame XeFG presents.
    if (!xess_ui::XemfgRuns(x)) return;
    const char* pinHelp = T("Shows the real and XeMFG frame rates in the top-left corner\n"
        "while the menu is closed. Saved for the next launch.");
    if (x.xemfgGenerating && xess_ui::XemfgFpsCurrent(x))
        StatusRow("FPS", Tone::Normal, T("%.1f real, %.1f XeMFG"), x.xemfgBaseFpsMilli / 1000.0, x.xemfgOutputFpsMilli / 1000.0);
    else if (x.xemfgGenerating)
        StatusRow("FPS", Tone::Dim, "%s", T("Measuring..."));
    else if (gPinnedFps.load(std::memory_order_acquire))
        StatusRow("FPS", Tone::Dim, "%s", T("XeMFG off"));
    else
        return;
    PinnedFpsToggle(pinHelp);
}

// latency.1: low-latency pacing, where XeMFG runs on D3D12 (standalone, or OptiScaler's XeFG) or the backend already
// timed its frames.
bool gLatencyPacingSaveFailed = false;
bool LatencyPacingShown()
{
    const auto& x = gXessUi;
    return x.latencyPacingDetail && (x.latencyPacingFrames > 0 || (xess_ui::XemfgRuns(x) && x.xemfgApi == "d3d12")
        || (x.optiScaler && x.hooksActive));
}

void LatencyPacingControlRow()
{
    auto& x = gXessUi;
    const char* modes[] = {T("Auto"), T("On"), T("Off")};
    int mode = static_cast<int>(std::min<uint32_t>(x.latencyPacing, 2));
    ControlRow(T("Low-latency pacing"));
    if (ImGui::Combo("##latencypacing", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        gLatencyPacingSaveFailed = !NativeFilePersistLatencyPacing(static_cast<uint32_t>(mode));
        if (!gLatencyPacingSaveFailed) x.latencyPacing = static_cast<uint32_t>(mode); // shown at once
    }
    HelpMarker(T("XeMFG holds the game inside Present until the display has room for its next frames, after the game "
        "has read your input. This moves that wait to before the game reads its input, so your actions show sooner. "
        "The frame rate stays the same. Auto turns it on. Applies at once."));
}

void LatencyPacingNotes()
{
    if (gLatencyPacingSaveFailed) Note(Tone::Bad, T("The low-latency pacing setting could not be saved."));
}

// The wait moved before the input, of the game's whole wait in Present, over the backend's last status interval.
void LatencyPacingStatusRow()
{
    const auto& x = gXessUi;
    if (!LatencyPacingShown()) return;
    const char* label = T("Low-latency pacing");
    if (x.latencyPacing == 2)
    {
        StatusRow(label, Tone::Normal, "%s", T("Off"));
        return;
    }
    char value[64]{}, detail[128]{};
    if (!xess_ui::LatencyPacingCurrent(x))
        StatusDetail(label, Tone::Pending, T("Waiting"), T("for XeMFG's frames"));
    else if (x.latencyPacingTakes == 0 && x.latencyPacingMissed > 0)
        StatusDetail(label, Tone::Pending, T("No sleep point"), T("the game's frames reach no sleep before their input"));
    else if (x.latencyPacingSleptMs < 0.05)
        StatusDetail(label, Tone::Normal, T("On"), T("the game is not held in Present"));
    else
    {
        snprintf(value, sizeof(value), T("%.1f ms sooner"), x.latencyPacingSleptMs);
        snprintf(detail, sizeof(detail), T("of %.1f ms the game waited in Present"), x.latencyPacingSlackMs);
        StatusDetail(label, Tone::Good, value, detail);
    }
}

void DrawXessPanel(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    const auto& x = gXessUi;
    static const char* models[] = {N_("MFG Unlocker multi-frame port"), N_("Intel 2x model")};

    Section(T("Frame generation"));
    if (XemfgAvailable())
    {
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xemfg"))
        {
            XemfgControlRows();
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        XemfgNotes();
    }
    // XeMFG standalone needs no frame generation of the game's own (only the game's DLSS switch, noted above).
    if (!x.xemfgStandalone) FrameGenerationOffNote(snapshot);
    if (!x.available)
        Note(Tone::Pending, T("XeSS frame generation support is unavailable in this build."));
    else if (!x.optiScaler && !x.xemfgStandalone)
        Note(Tone::Pending, T("XeSS multi-frame generation runs as XeMFG standalone, or through OptiScaler with XeFG "
            "as its frame generation output. Without either, the game's XeSS frame generation runs at Intel's standard 2x."));
    else if (x.optiScaler)
        Note(Tone::Normal, T("XeSS multi-frame generation runs through OptiScaler. Set the frame "
            "generation ratio and the MFG model in OptiScaler's menu."));
    const uint32_t vendor = GameGpuVendor();
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    if (vendor && vendor != kNvidiaVendor)
        Note(Tone::Dim, T("DLSS multi-frame generation needs an NVIDIA RTX GPU."));
    else if (dlssOffered && gXessUi.gpuVendorId == kNvidiaVendor && !xess_ui::DlssMfgFamily(snapshot.gpuFamily))
        Note(Tone::Dim, T("DLSS multi-frame generation is unavailable on this GPU."));

    // XeMFG standalone paces the game's frames itself (XeLL): the DLSS panel's latency rows, with the FPS
    // limit XeLL applies to XeMFG's output. Otherwise V-Sync only (OptiScaler's menu owns its frame limit).
    const bool xemfgLatency = BackendUsesNativeFile() && x.xemfgStandalone && !x.optiScaler;
    int vsync = snapshot.vsyncMode == 1 ? 1 : 0;
    bool vsyncChanged = false;
    if (xemfgLatency)
    {
        Section(T("Latency and display"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xemfglatency"))
        {
            PresentationControlRows(snapshot, statusStale, true);
            if (LatencyPacingShown()) LatencyPacingControlRow();
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresentationNotes(snapshot, true);
        LatencyPacingNotes();
    }
    else
    {
        Section(T("Display"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xessdisplay"))
        {
            const char* vsyncModes[] = {T("Game"), T("Off")};
            ControlRow(T("V-Sync"));
            vsyncChanged = ImGui::Combo("##xessvsync", &vsync, vsyncModes, IM_ARRAYSIZE(vsyncModes));
            HelpMarker(T("Off submits the game's frames without V-Sync."));
            if (LatencyPacingShown()) LatencyPacingControlRow();
            ImGui::EndTable();
        }
        if (vsyncChanged && !NativeFilePersistPresentation(static_cast<uint32_t>(vsync), snapshot.reflexFrameLimitFps))
            Note(Tone::Bad, T("The V-Sync setting could not be saved."));
        ImGui::EndDisabled();
        LatencyPacingNotes();
    }

    // The preset of the DLSS upscaling the game runs (XeMFG often takes its inputs from it), as in the DLSS panel.
    const bool dlssUpscaling = BackendUsesNativeFile() && (snapshot.dlssSr.running || snapshot.dlssRr.running);
    const bool rayReconstructionShown = dlssUpscaling && RayReconstructionShown(snapshot);
    const MfgUnlockDlssFeature& dlssView = rayReconstructionShown ? snapshot.dlssRr : snapshot.dlssSr;
    if (dlssUpscaling)
    {
        Section(rayReconstructionShown ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##xessdlsssr"))
        {
            DlssPresetControlRow(dlssView, rayReconstructionShown, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        DlssNotes(dlssView, rayReconstructionShown);
    }

    Section(T("Status"));
    char value[64]{}, detail[96]{};
    if (BeginGrid("##xessstatus"))
    {
        GameRows(snapshot, false);
        XemfgStatusRows();
        LatencyStatusRow(); // latmon.2
        LatencyPacingStatusRow();
        if (x.available && x.optiScaler)
        {
            if (xess_ui::AppliedKnown(x))
            {
                snprintf(value, sizeof(value), "%ux", x.applied);
                snprintf(detail, sizeof(detail), T("%u generated frames capacity"), x.capacity);
                StatusDetail(T("Applied"), Tone::Good, value, detail);
            }
            else if (x.initialized)
                StatusRow(T("Applied"), Tone::Pending, "%s", T("Not verified"));
            if (x.modelActive == 1 || x.modelActive == 2)
                StatusRow(T("Model"), Tone::Normal, "%s", T(models[x.modelActive - 1]));
            // The ported model's int8 layers on RTX 20/30/40/50 tensor cores (CUDA in XeFG's own command list).
            if (x.modelActive == 1 && x.tensorCores == 1)
            {
                if (x.tensorCoreLaunches)
                    StatusDetail(T("Tensor cores"), Tone::Good, T("On"), T("the ported model's int8 layers run as CUDA kernels"));
                else
                    StatusDetail(T("Tensor cores"), Tone::Pending, T("Ready"), T("no layer has run yet"));
            }
            else if (x.modelActive == 1 && x.tensorCores == 3)
                StatusDetail(T("Tensor cores"), Tone::Normal, T("Off"), T("XeFGTensorCores = off in OptiScaler.ini"));
            else if (x.modelActive == 1 && x.tensorCores == 2 && xess_ui::Vendor(x) == kNvidiaVendor)
                StatusDetail(T("Tensor cores"), Tone::Normal, T("Off"), T("not available on this GPU or driver"));
        }
        const bool vulkanRow = !xemfgLatency && !statusStale && !vsyncChanged && VulkanVsyncRow(snapshot);
        if (xemfgLatency)
            PresentationStatusRows(snapshot, statusStale, true);
        else if (!vulkanRow && vsync == 1)
        {
            const auto result = !statusStale && !vsyncChanged
                ? xess_ui::VsyncResult(snapshot.vsyncControlAvailable != FALSE,
                    snapshot.vsyncPresentationObserved != FALSE, snapshot.vsyncOverrideApplied != FALSE,
                    snapshot.vsyncOriginalInterval, snapshot.vsyncSubmittedInterval)
                : xess_ui::VsyncReadback::Pending;
            switch (result)
            {
            case xess_ui::VsyncReadback::Overridden:
                snprintf(detail, sizeof(detail), T("game interval %u submitted as 0"), snapshot.vsyncOriginalInterval);
                StatusDetail(T("V-Sync"), Tone::Good, T("Off"), detail);
                break;
            case xess_ui::VsyncReadback::AlreadyOff:
                StatusDetail(T("V-Sync"), Tone::Good, T("Off"), T("the game already presents without V-Sync"));
                break;
            default:
                StatusDetail(T("V-Sync"), Tone::Pending, T("Off requested"), T("presentation override is not verified"));
                break;
            }
        }
        if (dlssUpscaling) DlssStatusRows(dlssView, rayReconstructionShown);
        ImGui::EndTable();
    }
    if (x.available && x.optiScaler && !x.status.empty()) Note(Tone::Dim, "%s", x.status.c_str());
    DrawWitcherHair();

    Section(T("Advanced"));
    if (x.available && ImGui::CollapsingHeader(l10n::Label("XeSS diagnostics")))
    {
        ImGui::Text("OptiScaler %s | hooks %s | initialized %s | enabled %s",
            x.optiScaler ? "detected" : "not detected", x.hooksActive ? "active" : "waiting",
            x.initialized ? "yes" : "no", x.enabled ? "yes" : "no");
        ImGui::Text("Provider frames %u | model %u", x.framesPresented, x.modelActive);
        ImGui::Text("Tensor cores %u | launches %u | fallbacks %u%s%s", x.tensorCores, x.tensorCoreLaunches,
            x.tensorCoreFallbacks, x.tensorCoreReason.empty() ? "" : " | ", x.tensorCoreReason.c_str());
        if (!x.runtimeFailure.empty()) ImGui::TextWrapped("Runtime: %s", x.runtimeFailure.c_str());
    }
    DrawWitcherHairDiagnostics();
}

// OptiScaler's shared UI mode (the default): its menu owns
// frame generation; this menu offers the DLSS SR/RR presets and DLSS 5.
void DrawKeybinds(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool shared, bool d3d11 = false);
const char* KeybindsTabLabel();
// keybindsTab: the overlay draws the menu and reads the keys (gStandaloneDraw).
void DrawOptiScalerSharedTabs(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool keybindsTab)
{
    // Only the features in use: the preset of each running DLSS feature, the DLSS-G preset while a DLSS-G
    // provider is in use, and DLSS 5 while its provider is present.
    const bool superResolution = snapshot.dlssSr.running != FALSE;
    const bool rayReconstruction = snapshot.dlssRr.running != FALSE;
    const bool frameGeneration = DlssgPresetShown(snapshot);
    const bool neuralRendering = Dlss5TabShown(snapshot);
    if (!superResolution && !rayReconstruction && !frameGeneration && !neuralRendering)
    {
        Note(Tone::Pending, "%s", T("Waiting for DLSS"));
        return;
    }
    if (!BeginTabs("##mfgtabs")) return;
    if ((superResolution || rayReconstruction || frameGeneration) && TabItem(T("DLSS presets")))
    {
        Note(Tone::Normal, "%s", T("Frame generation is set in OptiScaler's menu. This menu offers the DLSS presets and DLSS 5."));
        BeginStaleDisabled(statusStale);
        for (const bool rr : {false, true})
        {
            if (!(rr ? rayReconstruction : superResolution)) continue;
            const MfgUnlockDlssFeature& dlssView = rr ? snapshot.dlssRr : snapshot.dlssSr;
            ImGui::PushID(rr ? "rr" : "sr");
            Section(rr ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
            if (BeginGrid("##optiscalerdlss"))
            {
                DlssPresetControlRow(dlssView, rr, statusStale);
                ImGui::EndTable();
            }
            DlssNotes(dlssView, rr);
            ImGui::PopID();
        }
        MfgUnlockReShadeSnapshot presetView = snapshot;
        if (frameGeneration)
        {
            Section(T("Frame generation"));
            if (BeginGrid("##optiscalerdlssg"))
            {
                PresetControlRow(presetView, statusStale);
                ImGui::EndTable();
            }
            PresetNotes(presetView, true);
        }
        Section(T("Status"));
        if (BeginGrid("##optiscalerdlssstatus"))
        {
            if (superResolution) DlssStatusRows(snapshot.dlssSr, false);
            if (rayReconstruction) DlssStatusRows(snapshot.dlssRr, true);
            if (frameGeneration) PresetStatusRow(presetView);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (neuralRendering && TabItem(T("DLSS 5")))
    {
        DrawDlss5(snapshot, statusStale);
        ImGui::EndTabItem();
    }
    // menunotes.1: the preset keys, Pin panel and DLSS 5's Hold to show original beside OptiScaler's menu too.
    if (keybindsTab && TabItem(KeybindsTabLabel()))
    {
        DrawKeybinds(snapshot, statusStale, true);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawOptiScalerShared(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool keybindsTab)
{
    DrawOptiScalerSharedTabs(snapshot, statusStale, keybindsTab);
    RuntimeLogDiagnostics();
}

// menutab.2: RTXMFG's tab in OptiScaler's own menu (hidden UI mode, OptiScaler's plugin API). OptiScaler draws it on
// its menu thread with its own ImGui, through the widget functions of `ui`: nothing below may call this module's ImGui.
// Its text stays English like OptiScaler's menu (T() translates only once RTXMFG's own overlay has a font for it).
// When the tab reads the backend status next; a save reads it again at once, so the controls never show the old value.
ULONGLONG gOptiTabNextRead = 0;

// OptiScaler's own status colours; TextColored maps them for HDR like its menu does.
void OptiScalerTabStatus(const OptiPluginApi& ui, Tone tone, const char* text)
{
    if (tone == Tone::Bad)
        ui.TextColored(0.95f, 0.40f, 0.38f, 1.0f, text);
    else
        ui.TextColored(0.95f, 0.75f, 0.25f, 1.0f, text);
}

// OptiScaler's Combo greys out unavailable choices (bit i of `unavailable`: items[i]); picking one is refused here too.
bool OptiScalerTabCombo(const OptiPluginApi& ui, const char* label, int32_t& current, const char* const* items,
    int32_t count, uint32_t unavailable)
{
    // Combo reads itemDisabled[0..count).
    bool disabled[32]{};
    const bool grey = unavailable != 0 && count > 0 && count <= 32;
    for (int32_t i = 0; grey && i < count; ++i) disabled[i] = ((unavailable >> i) & 1u) != 0;
    const int32_t previous = current;
    if (!ui.Combo(label, &current, items, count, grey ? disabled : nullptr)) return false;
    if (current >= 0 && current < 32 && (unavailable >> current) & 1u)
    {
        current = previous;
        return false;
    }
    return true;
}

// A slider let go this frame; without ItemDeactivatedAfterEdit the slider saves at most every 150 ms while dragging.
bool OptiScalerTabReleased(const OptiPluginApi& ui)
{
    return OPTI_API_HAS(&ui, ItemDeactivatedAfterEdit) && ui.ItemDeactivatedAfterEdit();
}

void OptiScalerTabPreset(const OptiPluginApi& ui, const MfgUnlockDlssFeature& view, bool rr, bool stale)
{
    uint32_t& selected = rr ? gDlssRrPreset : gDlssSrPreset;
    const uint32_t* values = rr ? kRrPresetValues : kSrPresetValues;
    const char* const* options = rr ? kRrPresetOptions : kSrPresetOptions;
    const int32_t count = static_cast<int32_t>(rr ? std::size(kRrPresetValues) : std::size(kSrPresetValues));
    const bool valid = rr ? dlss_sr::IsValidRrSelection(view.presetRequested) : dlss_sr::IsValidSelection(view.presetRequested);
    if (!stale && valid) selected = view.presetRequested;
    // PresetOption reuses one buffer, the combo needs every label at once.
    char labels[std::size(kSrPresetValues)][32]{};
    const char* items[std::size(kSrPresetValues)]{};
    int32_t current = 0;
    for (int32_t i = 0; i != count; ++i)
    {
        snprintf(labels[i], sizeof(labels[i]), "%s", PresetOption(options[i]));
        items[i] = labels[i];
        if (values[i] == selected) current = i;
    }
    if (ui.Combo(rr ? T("RR preset") : T("SR preset"), &current, items, count, nullptr))
    {
        const BOOL saved = NativeFilePersistPreset(rr ? "dlssRrPreset" : "dlssSrPreset", values[current]);
        (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed) = !saved;
        if (saved) selected = values[current];
        gOptiTabNextRead = 0;
    }
    ui.ItemTooltip(DlssPresetHelp(rr));

    char detail[160]{};
    if (view.running)
    {
        snprintf(detail, sizeof(detail), "%s: %s, %ux%u -> %ux%u", rr ? T("DLSS RR") : T("DLSS SR"), SrQualityName(view),
            view.renderWidth, view.renderHeight, view.outputWidth, view.outputHeight);
        ui.TextDisabled(detail);
    }
    if (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed)
        OptiScalerTabStatus(ui, Tone::Bad, rr ? T("The DLSS RR preset could not be saved.") : T("The DLSS SR preset could not be saved."));
    if (view.fixedByDriver)
    {
        if (selected != dlss_sr::kGameOrDriver)
            ui.Text(T("The NVIDIA app's DLSS preset override is active in this game session. "
                "Restart the game with a preset selected here to switch presets live."));
        return;
    }
    if (!view.running || selected == view.activePreset) return;
    if (view.liveSwitch == 1)
        ui.Text(T("This game's DLSS cannot change its preset while running. The preset applies "
            "the next time the game starts DLSS (change the DLSS quality mode or restart the game)."));
    else if (view.liveSwitch == 2)
        OptiScalerTabStatus(ui, Tone::Bad, T("DLSS refused the new preset; the previous one stays in use."));
}

void OptiScalerTabDlssgPreset(const OptiPluginApi& ui, const MfgUnlockReShadeSnapshot& snapshot, bool stale)
{
    if (!stale && snapshot.dlssgPresetRequested <= 3) gDlssgPreset = static_cast<int>(snapshot.dlssgPresetRequested);
    char labels[std::size(kDlssgPresetOptions)][48]{};
    const char* items[std::size(kDlssgPresetOptions)]{};
    for (size_t i = 0; i != std::size(kDlssgPresetOptions); ++i)
    {
        snprintf(labels[i], sizeof(labels[i]), "%s", PresetOption(kDlssgPresetOptions[i]));
        items[i] = labels[i];
    }
    const bool legacyPreset = UiAmpere() && snapshot.ampereLegacySinglePreset;
    // Preset C greys out once the provider has shown it lacks C; a saved C still shows.
    const uint32_t unavailable = snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable ? 1u << 3 : 0u;
    int32_t current = std::clamp(gDlssgPreset, 0, 3);
    ui.Disabled(stale || legacyPreset);
    if (OptiScalerTabCombo(ui, T("DLSS-G preset"), current, items, static_cast<int32_t>(std::size(items)), unavailable)
        && current != gDlssgPreset)
    {
        const BOOL saved = NativeFilePersistPreset("dlssgPreset", static_cast<uint32_t>(current));
        gPresetSaveFailed = !saved;
        if (saved) gDlssgPreset = current;
        gOptiTabNextRead = 0;
    }
    ui.ItemTooltip(T(kDlssgPresetHelp));
    ui.Disabled(stale);

    if (legacyPreset)
    {
        ui.TextDisabled(T("This DLSS-G provider supports its original preset only."));
        return;
    }
    const uint32_t active = snapshot.dlssgPresetSelectionFrozen ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested;
    char value[32]{};
    snprintf(value, sizeof(value), T("Preset %s"), DlssgPresetName(active));
    const char* detail = active == 0 ? T("managed by the game or driver")
        : active == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable
            ? T("needs DLSS-G 310.9.16; game / driver in use")
        : snapshot.dlssgPresetObservedValid && snapshot.dlssgPresetObserved == active && snapshot.dlssgPresetOverrideReadCount > 0
            ? T("override supplied")
        : snapshot.dlssgPresetOverrideFailure != 0 ? T("override unavailable") : T("waiting for the provider");
    char line[160]{};
    snprintf(line, sizeof(line), "DLSS-G: %s, %s", active == 0 ? T("Game / driver") : value, detail);
    ui.TextDisabled(line);
    if (gPresetSaveFailed) OptiScalerTabStatus(ui, Tone::Bad, T("The preset selection could not be saved."));
    char pending[256]{};
    if (const char* note = DlssgPresetPendingNote(snapshot, true, pending, sizeof(pending))) ui.Text(note);
    else if (active == 3 && snapshot.dlssgPresetReadCount > 0 && !snapshot.dlssgPresetCAvailable)
        ui.Text(T("Preset C needs DLSS-G 310.9.16 or newer (NVIDIA app). This game's"
            " frame generation keeps the game / driver preset."));
}

// optinr.1: `text` with the id `id`, so a label shown twice in the tab stays unique. Valid until eight more calls.
const char* OptiScalerTabLabel(const char* text, const char* id) noexcept
{
    static char labels[8][192]{};
    static size_t next = 0;
    char* label = labels[next++ % std::size(labels)];
    snprintf(label, sizeof(labels[0]), "%s##%s", text, id);
    return label;
}

bool OptiScalerTabChoice(const OptiPluginApi& ui, const char* label, uint32_t& value, const char* const* items,
    int32_t count, uint32_t unavailable = 0)
{
    int32_t current = static_cast<int32_t>(std::min<uint32_t>(value, static_cast<uint32_t>(count - 1)));
    if (!OptiScalerTabCombo(ui, label, current, items, count, unavailable)) return false;
    value = static_cast<uint32_t>(current);
    return true;
}

// Faces & skin as the shared menu's FacesControl. `disabled` is the caller's other condition; true when saved.
bool OptiScalerTabFaces(const OptiPluginApi& ui, const char* id, float& character, float structure, bool mask,
    bool disabled, bool& edited, bool& released, const char* tooltip)
{
    const bool follows = character < 0.0f;
    float shown = structure;
    ui.Disabled(disabled || !mask || follows);
    edited |= ui.SliderFloat(OptiScalerTabLabel(T("Faces & skin"), id), follows ? &shown : &character, 0.0f,
        dlss5::kMaxStrength, "%.2f");
    released |= OptiScalerTabReleased(ui);
    if (tooltip) ui.ItemTooltip(tooltip);
    ui.Disabled(disabled || !mask);
    char sameId[64]{};
    snprintf(sameId, sizeof(sameId), "%ssame", id);
    bool same = follows;
    const bool save = ui.Checkbox(OptiScalerTabLabel(T("Same as Structure"), sameId), &same);
    if (save) character = same ? dlss5::kMinCharacter : structure;
    ui.Disabled(disabled);
    return save;
}

// Hold to show original, as CompareKeyControl. The backend reads the key, so it works with only OptiScaler's menu.
bool OptiScalerTabCompareKey(const OptiPluginApi& ui, dlss5::Settings& s)
{
    bool changed = PollCompareBinding(s);
    char line[192]{};
    if (gCompareBinding)
    {
        snprintf(line, sizeof(line), "%s: %s", T("Hold to show original"), T("Press a key... Esc cancels"));
        ui.Text(line);
        ui.SameLine();
        if (ui.Button(OptiScalerTabLabel(T("Cancel"), "dlss5comparecancel"))) gCompareBinding = false;
        return changed;
    }
    const std::string key = s.compareKey ? KeyName(static_cast<int>(s.compareKey)) : std::string(T("Not set"));
    snprintf(line, sizeof(line), "%s: %s", T("Hold to show original"), key.c_str());
    ui.Text(line);
    ui.ItemTooltip(T("While this key is held, the game shows its original image, so DLSS 5 can be compared in motion.\n"
        "DLSS 5 keeps running, so its GPU cost is unchanged, and the key still reaches the game."));
    ui.SameLine();
    if (ui.Button(OptiScalerTabLabel(T("Change key"), "dlss5comparekey"))) BeginCompareBinding();
    if (s.compareKey)
    {
        ui.SameLine();
        if (ui.Button(OptiScalerTabLabel(T("Clear"), "dlss5compareclear")))
        {
            s.compareKey = 0;
            changed = true;
        }
    }
    return changed;
}

// The Advanced section, closed by default; OptiScaler's API has no collapsing header, so a checkbox opens it.
bool gOptiTabDlss5Advanced = false;
size_t gOptiTabDlss5Page = 0;

// The menu's Presets rows in OptiScaler's tab. True when `s` changed: the caller saves it; leaves Disabled(stale) set.
bool OptiScalerTabDlss5Presets(const OptiPluginApi& ui, dlss5::Settings& s, bool stale)
{
    using namespace dlss5::presets;
    ReadDlss5PresetsOnce();
    bool changed = false;
    size_t edited = kNone;
    const size_t shown = ShownPreset(s, edited);
    const size_t look = ShownLook(s, shown, edited);
    const char* const looks[]{T("Subtle"), T("Balanced"), T("Strong")};
    static_assert(sizeof(looks) / sizeof(*looks) == dlss5::kLookCount);
    // The looks, then the saved presets. OptiScaler's Combo shows items[current]: while neither is shown, a last item
    // that cannot be chosen shows what the menu's preview shows (Custom, or the preset last chosen, changed).
    bool disabled[dlss5::kLookCount + kMaxPresets * 2 + 1]{};
    std::vector<const char*> items(looks, looks + dlss5::kLookCount);
    std::vector<size_t> entries(dlss5::kLookCount, kNone);
    int32_t current = look < dlss5::kLookCount ? static_cast<int32_t>(look) : -1;
    for (size_t index = 0; index != gDlss5Presets.size() && items.size() + 1 < std::size(disabled); ++index)
    {
        if (!gDlss5Presets[index].readable) continue;
        if (index == shown) current = static_cast<int32_t>(items.size());
        items.push_back(gDlss5Presets[index].preset.name.c_str());
        entries.push_back(index);
    }
    char preview[160]{};
    if (current < 0)
    {
        current = static_cast<int32_t>(items.size());
        disabled[items.size()] = true;
        items.push_back(PresetPreview(shown, edited, look, looks, preview, sizeof(preview)));
        entries.push_back(kNone);
    }
    ui.Disabled(stale);
    int32_t chosen = current;
    if (ui.Combo(OptiScalerTabLabel(T("Presets"), "dlss5presets"), &chosen, items.data(), static_cast<int32_t>(items.size()),
            disabled)
        && chosen != current && chosen >= 0 && static_cast<size_t>(chosen) < items.size())
    {
        const size_t picked = static_cast<size_t>(chosen);
        if (picked < dlss5::kLookCount) changed |= LoadDlss5Look(s, picked);
        else if (entries[picked] != kNone) changed |= LoadDlss5Preset(s, entries[picked]);
    }
    ui.ItemTooltip(Dlss5PresetsHelp());
    const size_t selected = shown != kNone ? shown : edited;
    // dlss5tab.1: Save... and Share... open their row. OptiScaler's Combo fills the line, so the buttons sit under it.
    char saveRow[96]{}, shareRow[96]{};
    if (ui.Button(PresetRowLabel(PresetRow::eSave, saveRow, sizeof(saveRow)))) TogglePresetRow(PresetRow::eSave);
    ui.ItemTooltip(Dlss5PresetSaveHelp());
    ui.SameLine();
    if (ui.Button(PresetRowLabel(PresetRow::eShare, shareRow, sizeof(shareRow)))) TogglePresetRow(PresetRow::eShare);
    ui.ItemTooltip(Dlss5PresetShareHelp());
    ui.SameLine();
    ui.Disabled(stale || selected == kNone);
    if (ui.Button(OptiScalerTabLabel(T("Delete"), "dlss5presetdelete")) && selected != kNone)
        DeleteDlss5Preset(gDlss5Presets[selected].preset.name);
    ui.Disabled(stale);
    // OptiScaler's InputText draws its label after the field; without one (never null in a table we accept, but
    // checked) Save takes a numbered name and Import reads the clipboard.
    if (gDlss5PresetRow == PresetRow::eSave)
    {
        if (ui.InputText)
        {
            ui.InputText(OptiScalerTabLabel(T("Save as"), "dlss5presetname"), gDlss5PresetName, sizeof(gDlss5PresetName));
            ui.ItemTooltip(Dlss5PresetSaveHelp());
            ui.SameLine();
        }
        if (ui.Button(OptiScalerTabLabel(T("Save"), "dlss5presetsave"))) SaveDlss5Preset(s);
        if (!ui.InputText) ui.ItemTooltip(Dlss5PresetSaveHelp());
    }
    if (gDlss5PresetRow == PresetRow::eShare)
    {
        if (ui.InputText)
        {
            ui.InputText(OptiScalerTabLabel(T("Share"), "dlss5presetcode"), gDlss5PresetCode, sizeof(gDlss5PresetCode));
            ui.ItemTooltip(Dlss5PresetShareHelp());
            ui.SameLine();
        }
        if (ui.Button(OptiScalerTabLabel(T("Import"), "dlss5presetimport"))) changed |= ImportDlss5Preset(s);
        ui.ItemTooltip(Dlss5PresetShareHelp());
        ui.SameLine();
        if (ui.Button(OptiScalerTabLabel(T("Export"), "dlss5presetexport"))) ExportDlss5Preset(s, shown);
        ui.ItemTooltip(Dlss5PresetShareHelp());
    }
    Tone tone = Tone::Normal;
    char note[320]{};
    if (const char* text = Dlss5PresetNoteText(tone, note, sizeof(note)))
    {
        if (tone == Tone::Bad || tone == Tone::Pending) OptiScalerTabStatus(ui, tone, text);
        else ui.Text(text);
        if (gDlss5PresetSkipped) OptiScalerTabStatus(ui, Tone::Pending, Dlss5PresetSkippedText());
    }
    return changed;
}

void OptiScalerTabDlss5(const OptiPluginApi& ui, const MfgUnlockReShadeSnapshot& snapshot, bool stale)
{
    // The shared menu keeps its edits while a slider is held (ImGui::IsAnyItemActive). OptiScaler's API has no
    // such query, so the saved settings come back only half a second after the last edit or save.
    static ULONGLONG lastEdit = 0;
    const ULONGLONG now = GetTickCount64();
    if (!stale && !gDlss5Dirty && !gDlss5Held && now - lastEdit >= 500) gDlss5 = snapshot.dlss5;
    dlss5::Settings& s = gDlss5;
    const dlss5::Settings before = s;
    // letGo: a slider that replaces the feature was released (nrscale.1, Dlss5ReleaseSave).
    bool save = false, edited = false, released = false, letGo = false;
    // OptiScaler's Disabled is one level: each control states its whole condition.
    ui.Disabled(stale);

    // Present needs RTXMFG's swap chain hooks, which the hidden UI mode never arms.
    const char* const fixedStages[]{T("Before upscaling (faster)"), T("After upscaling"), T("Present")};
    uint32_t chosen = static_cast<uint32_t>(dlss5::auto_mode::AutoStage());
    if (s.enabled && s.stage == static_cast<uint32_t>(dlss5::Stage::eAuto) && snapshot.dlss5Route && !snapshot.dlss5Pending
        && snapshot.dlss5Stage >= 1 && snapshot.dlss5Stage <= 3) chosen = snapshot.dlss5Stage;
    char automatic[128]{};
    snprintf(automatic, sizeof(automatic), T("Auto: %s"), fixedStages[chosen - 1]);
    const char* const stages[]{T("Off"), automatic, fixedStages[0], fixedStages[1], fixedStages[2]};
    int32_t choice = !s.enabled ? 0 : s.stage == static_cast<uint32_t>(dlss5::Stage::eAuto) ? 1 : static_cast<int32_t>(s.stage) + 1;
    const uint32_t unavailable = (1u << 4) | (snapshot.graphicsApi == 2u ? 1u << 2 : 0u);
    if (OptiScalerTabCombo(ui, T("Hook Method"), choice, stages, 5, unavailable))
    {
        s.enabled = choice != 0;
        if (choice) s.stage = choice == 1 ? 0 : static_cast<uint32_t>(choice) - 1;
        save = true;
    }
    ui.ItemTooltip(Dlss5StageHelp());

    save |= OptiScalerTabDlss5Presets(ui, s, stale);

    const bool present = s.enabled && s.stage == static_cast<uint32_t>(dlss5::Stage::ePresent);
    const auto slider = [&](const char* text, const char* id, float& value, float minimum, float maximum,
        const char* format, const char* tooltip) {
        edited |= ui.SliderFloat(OptiScalerTabLabel(text, id), &value, minimum, maximum, format);
        released |= OptiScalerTabReleased(ui);
        if (tooltip) ui.ItemTooltip(tooltip);
    };
    // replaces: the value replaces the feature and saves on release (nrscale.1).
    const auto sliderInt = [&](const char* text, const char* id, uint32_t& value, uint32_t minimum, uint32_t maximum,
        const char* tooltip, bool replaces = false) {
        int32_t shown = static_cast<int32_t>(std::clamp(value, minimum, maximum));
        if (ui.SliderInt(OptiScalerTabLabel(text, id), &shown, static_cast<int32_t>(minimum), static_cast<int32_t>(maximum), "%d"))
        {
            value = static_cast<uint32_t>(std::clamp(shown, static_cast<int32_t>(minimum), static_cast<int32_t>(maximum)));
            if (replaces) { gDlss5Held = true; lastEdit = now; }
            else edited = true;
        }
        (replaces ? letGo : released) |= OptiScalerTabReleased(ui);
        if (tooltip) ui.ItemTooltip(tooltip);
    };
    const auto combo = [&](const char* text, const char* id, uint32_t& value, const char* const* items, int32_t count,
        const char* tooltip) {
        save |= OptiScalerTabChoice(ui, OptiScalerTabLabel(text, id), value, items, count);
        if (tooltip) ui.ItemTooltip(tooltip);
    };
    const auto check = [&](const char* text, const char* id, bool& value, const char* tooltip) {
        save |= ui.Checkbox(OptiScalerTabLabel(text, id), &value);
        if (tooltip) ui.ItemTooltip(tooltip);
    };

    // dlss5tab.1: the shared menu's groups, in its order. A control fills the line here, so what shares a row there
    // comes on the next line, and Style stays a list: this API cannot light the chosen button.
    ui.Section(T("Look"));
    ui.Disabled(stale);
    edited |= ui.SliderFloat(T("Intensity"), &s.intensity, 0.0f, 1.0f, "%.2f");
    released |= OptiScalerTabReleased(ui);
    ui.ItemTooltip(T("Strength of the whole neural rendering effect."));

    const char* const models[]{T("Neutral"), T("Rich"), T("Soft")};
    int32_t model = static_cast<int32_t>(std::min<uint32_t>(s.model, 2));
    if (ui.Combo(T("Style"), &model, models, 3, nullptr))
    {
        s.model = static_cast<uint32_t>(model);
        save = true;
    }
    ui.ItemTooltip(Dlss5StyleHelp());

    save |= OptiScalerTabFaces(ui, "dlss5character", s.character, s.structure, s.characterMask, stale, edited, released,
        Dlss5FacesHelp(s.characterMask));
    check(T("Character Mask"), "dlss5mask", s.characterMask, T("Detects skin and faces so that Faces & skin applies to them."));

    ui.Section(T("Performance"));
    ui.Disabled(stale);
    if (ui.SliderFloat(T("Resolution Scale"), &s.scale, 0.0f, dlss5::kMaxScale, s.scaleAuto ? T("up to %.0f%%") : "%.0f%%"))
    {
        gDlss5Held = true;
        lastEdit = now;
    }
    letGo |= OptiScalerTabReleased(ui);
    ui.ItemTooltip(Dlss5ScaleHelp());
    char autoLabel[64]{};
    snprintf(autoLabel, sizeof(autoLabel), "%s##dlss5scaleauto", T("Auto"));
    save |= ui.Checkbox(autoLabel, &s.scaleAuto);

    if constexpr (dlss5::kHalfRateOffered)
    {
        const bool boostUnavailable = snapshot.graphicsApi == 2u || present;
        ui.Disabled(stale || boostUnavailable);
        save |= ui.Checkbox(T("Half rate"), &s.boost);
        ui.ItemTooltip(Dlss5HalfRateHelp(boostUnavailable));
        const char* const tolerances[]{T("Strict"), T("Normal"), T("Loose")};
        ui.Disabled(stale || boostUnavailable || !s.boost);
        combo(T("Motion tolerance"), "dlss5boosttolerance", s.boostTolerance, tolerances, 3,
            T("How much a surface may change between frames and still keep the carried fine detail.\n"
                "Strict carries it on fewer moving surfaces, which keep the rebuilt look; Loose carries more detail."));
        ui.Disabled(stale);
    }
    const char* const presets[]{T("Default"), T("Quality"), T("Fast")};
    combo(T("Network"), "dlss5preset", s.preset, presets, 3, Dlss5NetworkHelp());

    // optinr.1: the rest of the shared menu's DLSS 5 tab. Compare is not part of the look: Reset keeps it.
    ui.Section(T("Compare"));
    ui.Disabled(stale);
    check(T("Split view"), "dlss5compare", s.compare, T("Shows the original image left of the line and DLSS 5 to its right.\n"
        "Before upscaling draws no line, because the upscaler would treat it as part of the image."));
    ui.Disabled(stale || !s.compare);
    slider(T("Split position"), "dlss5split", s.compareSplit, dlss5::kMinCompareSplit, dlss5::kMaxCompareSplit, "%.0f%%", nullptr);
    check(T("Swap sides"), "dlss5swap", s.compareSwap, T("Split view shows DLSS 5 on the left and the original on the right."));
    ui.Disabled(stale);
    save |= OptiScalerTabCompareKey(ui, s);
    const char* const views[]{T("Off"), T("Network input"), T("Network output"), T("Change (x8)")};
    combo(T("Debug view"), "dlss5debugview", s.debugView, views, 4,
        T("Shows what the network is given and returns, or where DLSS 5 changes the image (grey: unchanged; lighter or darker: changed).\n"
            "With Split view, on the DLSS 5 side only."));

    char advancedLabel[96]{};
    snprintf(advancedLabel, sizeof(advancedLabel), "%s##dlss5advanced", T("Advanced"));
    ui.Disabled(false);
    ui.Separator();
    ui.Checkbox(advancedLabel, &gOptiTabDlss5Advanced);
    ui.Disabled(stale);
    if (gOptiTabDlss5Advanced)
    {
        // dlss5tab.1: the shared menu's Advanced pages as a row of buttons, this API having no tab bar: the open page's
        // is disabled, and * marks a page with a changed setting.
        for (size_t page = 0; page != kDlss5PageCount; ++page)
        {
            char pageLabel[96]{};
            snprintf(pageLabel, sizeof(pageLabel), "%s%s##dlss5page%u", Dlss5PageName(page), Dlss5PageChanged(s, page) ? " *" : "",
                static_cast<unsigned>(page));
            if (page) ui.SameLine();
            ui.Disabled(page == gOptiTabDlss5Page);
            if (ui.Button(pageLabel)) gOptiTabDlss5Page = page;
        }
        ui.Disabled(stale);
        ui.Separator();

        switch (gOptiTabDlss5Page)
        {
        case 0:
        {
            slider(T("Structure Intensity"), "dlss5structure", s.structure, 0.0f, dlss5::kMaxStrength, "%.2f",
                T("Local detail and structure enhancement.\nAbove 1 goes past the provider's usual range."));
            slider(T("Tone Intensity"), "dlss5tone", s.tone, 0.0f, dlss5::kMaxStrength, "%.2f",
                T("Local tone and contrast enhancement.\nAbove 1 goes past the provider's usual range."));

            sliderInt(T("Pass Count"), "dlss5passes", s.passes, 1, dlss5::kMaxPasses,
                T("Runs the network several times in a row. Every pass is a separate feature and costs GPU time and memory."), true);
            const char* const later[]{T("Same as pass 1"), T("Custom")};
            uint32_t custom = s.laterCustom ? 1u : 0u;
            if (OptiScalerTabChoice(ui, OptiScalerTabLabel(T("Later passes"), "dlss5later"), custom, later, 2))
            {
                s.laterCustom = custom != 0;
                save = true;
                if (s.laterCustom)
                {
                    s.laterModel = s.model; s.laterIntensity = s.intensity; s.laterStructure = s.structure;
                    s.laterTone = s.tone; s.laterCharacter = s.character;
                }
            }
            ui.ItemTooltip(T("Passes 2 and later refine the previous pass's output. Custom gives them their own Style and strengths.\n"
                "Tone adds up from pass to pass, so later passes usually want less of it."));
            if (s.laterCustom)
            {
                const bool single = s.passes < 2;
                ui.Disabled(stale || single);
                combo(T("Style"), "dlss5latermodel", s.laterModel, models, 3, nullptr);
                slider(T("Intensity"), "dlss5laterintensity", s.laterIntensity, 0.0f, 1.0f, "%.2f", nullptr);
                slider(T("Structure Intensity"), "dlss5laterstructure", s.laterStructure, 0.0f, dlss5::kMaxStrength, "%.2f", nullptr);
                slider(T("Tone Intensity"), "dlss5latertone", s.laterTone, 0.0f, dlss5::kMaxStrength, "%.2f", nullptr);
                save |= OptiScalerTabFaces(ui, "dlss5latercharacter", s.laterCharacter, s.laterStructure, s.characterMask,
                    stale || single, edited, released, nullptr);
                ui.Disabled(stale);
                if (single) ui.TextDisabled(T("Later-pass settings apply when Pass Count is 2 or more."));
            }
            break;
        }
        case 1:
            slider(T("Blend strength"), "dlss5blendstrength", s.blend, 0.0f, dlss5::kMaxBlend, "%.2f",
                T("Scales DLSS 5's change: 0 keeps the game's image, 1 is DLSS 5's result, above 1 exaggerates it."));
            slider(T("Color strength"), "dlss5colorstrength", s.color, 0.0f, dlss5::kMaxColor, "%.2f",
                T("0 keeps the game's colors and takes only DLSS 5's lighting and detail. 1 is DLSS 5's color; above 1 strengthens its color change."));
            check(T("Highlight guard"), "dlss5guard", s.highlightGuard,
                T("Limits how far DLSS 5 may brighten a pixel, as a multiple of its original brightness, so lights do not turn into bright speckles."));
            ui.Disabled(stale || !s.highlightGuard);
            slider(T("Brightening limit"), "dlss5guardlimit", s.highlightLimit, dlss5::kMinHighlightLimit, dlss5::kMaxHighlightLimit,
                "%.1fx", nullptr);
            ui.Disabled(stale);
            slider(T("Dark halo reduction"), "dlss5halodark", s.haloDark, 0.0f, 1.0f, "%.2f",
                T("Removes thin dark outlines DLSS 5 adds beside bright edges and keeps its broad lighting change. Can lighten real shadow edges."));
            slider(T("Bright halo reduction"), "dlss5halobright", s.haloBright, 0.0f, 1.0f, "%.2f",
                T("Removes thin bright outlines DLSS 5 adds beside dark edges."));
            ui.Disabled(stale || (s.haloDark <= 0.0f && s.haloBright <= 0.0f));
            sliderInt(T("Halo radius"), "dlss5haloradius", s.haloRadius, dlss5::kMinHaloRadius, dlss5::kMaxHaloRadius,
                T("How far from each pixel to look for the edge, in network pixels."));
            ui.Disabled(stale);
            break;
        case 2:
        {
            const char* const domains[]{"scRGB-nl", T("Signed PQ"), T("Linear")};
            combo(T("Scaling Domain"), "dlss5domain", s.domain, domains, 3, T("Signal space used for downsampling and upsampling."));
            const char* const downsample[]{T("Area"), T("Bilinear"), T("Nearest")};
            combo(T("Downsample Filter"), "dlss5down", s.downsample, downsample, 3, nullptr);
            const char* const upsample[]{T("Bilinear"), T("Bicubic"), T("Nearest"), T("Edge-aware")};
            combo(T("Upsample Filter"), "dlss5up", s.upsample, upsample, 4,
                T("Resolution Scale below 100: how the network's change is enlarged. Edge-aware keeps it from bleeding across edges."));
            const char* const supersample[]{T("Bilinear"), T("Area")};
            combo(T("Supersampling Filter"), "dlss5supersample", s.supersample, supersample, 2,
                T("Resolution Scale above 100: how the network's larger image comes back to the game's size. Area averages every pixel it covers, without aliasing."));
            const char* const reconstruction[]{T("RGB residual"), T("Processed image")};
            combo(T("Scale Reconstruction"), "dlss5reconstruction", s.reconstruction, reconstruction, 2,
                T("RGB residual adds the network's change to the full-resolution image.\n"
                    "Processed image uses the upscaled network output directly."));
            break;
        }
        case 3:
        {
            const char* const encodings[]{T("Auto"), T("Linear BT.709"), "sRGB", "HDR10", "scRGB", "scRGB-nl"};
            combo(T("Encoding"), "dlss5encoding", s.encoding, encodings, 6,
                T("Auto treats the DLSS input and output as linear BT.709 and follows the swapchain format and color space at Present.\n"
                    "Other choices override the encoding of the processed image."));
            const char* const signals[]{"sRGB", T("PQ SDR"), T("Linear SDR")};
            combo(T("NR Signal Encoding"), "dlss5signal", s.signal, signals, 3,
                T("Encoding supplied to the network after the HDR-to-SDR bridge."));
            float white = dlss5::DiffuseWhite(s);
            ui.Disabled(stale || s.encoding == static_cast<uint32_t>(dlss5::Encoding::eAuto));
            if (ui.SliderFloat(OptiScalerTabLabel(T("Diffuse White"), "dlss5white"), &white, dlss5::kMinDiffuseWhite,
                    dlss5::kMaxDiffuseWhite, T("%.0f nits")))
            {
                s.diffuseWhite = white;
                s.diffuseWhiteOverride = true;
                edited = true;
            }
            released |= OptiScalerTabReleased(ui);
            ui.ItemTooltip(T("Reference white of the HDR-to-SDR bridge. Encoding Auto selects it; choose an explicit encoding to set it."));
            ui.Disabled(stale);
            slider(T("Highlight roll-off"), "dlss5knee", s.knee, dlss5::kMinKnee, dlss5::kMaxKnee, "%.2f",
                T("HDR only: up to this share of diffuse white the network sees the image unchanged; brighter values are compressed into the rest. Lower gives highlights more room."));
            // OptiScaler's slider has no logarithmic scale; the range and value are the shared menu's.
            slider(T("Exposure"), "dlss5exposure", s.exposure, 0.0f, dlss5::kMaxExposure, "%.2fx",
                T("Multiplies the exposure used for neural rendering, independent of the game's exposure."));
            const char* const exposureModes[]{T("Off"), T("On"), T("Inverse")};
            combo(T("Use Exposure Value"), "dlss5exposuremode", s.exposureMode, exposureModes, 3,
                T("On applies the game's DLSS exposure texture before neural rendering; Inverse divides by it."));
            check(T("Adaptive exposure"), "dlss5adaptive", s.adaptiveExposure,
                T("Keeps the scene the network sees at a steady brightness in HDR games, so the same settings suit games with different exposure.\n"
                    "The game's own brightness is unchanged. SDR color is left as it is."));
            if (s.adaptiveExposure && snapshot.dlss5Route && snapshot.dlss5ExposureGain != 1.0f)
            {
                char gain[32]{};
                snprintf(gain, sizeof(gain), "x%.2f", snapshot.dlss5ExposureGain);
                ui.SameLine();
                ui.TextDisabled(gain);
            }
            ui.Disabled(stale || !s.adaptiveExposure);
            slider(T("Adaptive target"), "dlss5adaptivetarget", s.adaptiveTarget, dlss5::kMinAdaptiveTarget, dlss5::kMaxAdaptiveTarget,
                "%.2f", T("The average brightness Adaptive exposure gives the scene the network sees (0.18 is middle grey). Higher shows it a brighter scene."));
            ui.Disabled(stale);
            break;
        }
        default:
        {
            const char* const uiCorrection[]{T("Auto"), T("Off"), T("On")};
            combo(T("UI Correction"), "dlss5ui", s.uiCorrection, uiCorrection, 3,
                T("Auto enables the provider's UI correction at Present, whose image contains the game's UI, and leaves it off for DLSS input and output.\n"
                    "Off never uses it. On always does."));
            // Only the Present hook method uses these, which does not run from OptiScaler's menu: disabled in place.
            ui.Disabled(true);
            const char* const resources[]{T("Never"), T("Optional"), T("Required")};
            combo(T("Use DLSS resources"), "dlss5resources", s.resources, resources, 3,
                T("Present only. Optional reuses the DLSS motion vectors and depth when available, otherwise neutral inputs.\n"
                    "Required skips Present processing without them. Never always uses neutral inputs."));
            check(T("Block Hudless"), "dlss5hudless", s.blockHudless,
                T("Present only: keeps the game's HUD-less image from DLSS frame generation, so generated frames match processed ones."));
            ui.Disabled(stale);
            break;
        }
        }

        ui.Separator();
        if (ui.Button(OptiScalerTabLabel(T("Reset to defaults"), "dlss5reset")))
        {
            dlss5::Settings defaults{};
            defaults.enabled = s.enabled; defaults.stage = s.stage;
            defaults.compare = s.compare; defaults.compareSplit = s.compareSplit; defaults.compareKey = s.compareKey;
            if (!(defaults == s)) { s = defaults; save = true; }
        }
    }
    ui.Disabled(false);
    s = dlss5::Sanitized(s);

    // Sliders save while dragging at most every 150 ms and once on release, as Dlss5SliderSave.
    if (edited)
    {
        gDlss5Dirty = true;
        lastEdit = now;
    }
    if (gDlss5Dirty && (released || now - gDlss5LastSave >= 150)) save = true;
    // nrscale.1: Resolution Scale and Pass Count save once, on release; without ItemDeactivatedAfterEdit once they stood
    // still for half a second, and with it after 5 s (a release the tab never saw: OptiScaler's menu closed mid-drag).
    const ULONGLONG still = OPTI_API_HAS(&ui, ItemDeactivatedAfterEdit) ? 5000 : 500;
    if (gDlss5Held && (letGo || now - lastEdit >= still))
    {
        gDlss5Held = false;
        save = true;
    }
    if (save)
    {
        gDlss5SaveFailed = !NativeFilePersistDlss5(s);
        gDlss5LastSave = now;
        gDlss5Dirty = false;
        if (gDlss5SaveFailed) s = before;
        lastEdit = now;
        gOptiTabNextRead = 0;
    }

    if (gDlss5SaveFailed) OptiScalerTabStatus(ui, Tone::Bad, T("DLSS 5 settings could not be saved."));
    if (present)
        ui.Text(T("Present does not run from OptiScaler's menu. Choose Auto, Before upscaling or After upscaling."));
    else if (snapshot.graphicsApi == 2u && s.enabled && s.stage == static_cast<uint32_t>(dlss5::Stage::eRender))
        ui.Text(T("Before upscaling and Present do not run on Vulkan yet. Choose Auto or After upscaling."));

    ui.Section(T("Status"));
    char line[256]{}, value[48]{}, detail[160]{};
    const auto row = [&](const char* name, const char* shown, const char* more) {
        if (more && more[0]) snprintf(line, sizeof(line), "%s: %s, %s", name, shown, more);
        else snprintf(line, sizeof(line), "%s: %s", name, shown);
    };
    Tone tone = Tone::Pending;
    const char* state = Dlss5StateName(snapshot.dlss5State, tone);
    row(T("State"), stale ? T("Waiting for DLSS") : state, nullptr);
    if (!stale && tone == Tone::Bad) OptiScalerTabStatus(ui, Tone::Bad, line);
    else ui.Text(line);
    if (!stale && snapshot.dlss5Route && snapshot.dlss5GpuMs >= 0.0f)
    {
        snprintf(value, sizeof(value), T("%.1f ms"), snapshot.dlss5GpuMs);
        Dlss5CostDetail(snapshot, detail, sizeof(detail));
        row(T("GPU cost"), value, detail);
        ui.Text(line);
        if (dlss5::kHalfRateOffered && snapshot.dlss5BoostNetworkMs >= 0.0f) ui.ItemTooltip(T("every other frame, when the network runs"));
    }
    if (s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale > 0.0f)
    {
        snprintf(value, sizeof(value), "%.0f%%", snapshot.dlss5EffectiveScale);
        if (snapshot.dlss5GpuMs >= 0.0f) snprintf(detail, sizeof(detail), T("%.1f of %.1f ms"), snapshot.dlss5GpuMs, snapshot.dlss5BudgetMs);
        else snprintf(detail, sizeof(detail), "%s", T("measuring"));
        row(T("Auto scale"), value, detail);
        ui.Text(line);
    }
    if (const char* route = Dlss5RouteRow(snapshot, detail, sizeof(detail)))
    {
        row(T("Route"), route, detail);
        ui.Text(line);
    }
    if (s.enabled && !stale)
        if (const char* refusal = Dlss5D3D11RefusalText(snapshot.dlss5D3D11Refusal)) ui.TextDisabled(refusal);
    if (snapshot.dlss5Pending)
        OptiScalerTabStatus(ui, Tone::Pending, T("Waiting for DLSS 5 GPU work to complete before applying settings."));
    if (s.enabled && s.scaleAuto && snapshot.dlss5Route && snapshot.dlss5EffectiveScale > 0.0f)
    {
        const float most = dlss5::auto_mode::kLevels[dlss5::auto_mode::CeilingLevel(s.scale)];
        if (snapshot.dlss5ScaleFloorHeld)
        {
            snprintf(line, sizeof(line), T("Auto Resolution Scale holds %.0f%%, its quality floor. DLSS 5 still costs more than its budget here, so the frame rate pays instead of detail."),
                snapshot.dlss5EffectiveScale);
            ui.TextDisabled(line);
        }
        else if (snapshot.dlss5EffectiveScale < most - 0.5f)
        {
            snprintf(line, sizeof(line), T("Auto Resolution Scale runs at %.0f%% of up to %.0f%% to keep DLSS 5 within its budget, and rises again with headroom."),
                snapshot.dlss5EffectiveScale, most);
            ui.TextDisabled(line);
        }
    }
    if (snapshot.dlss5State == static_cast<uint32_t>(dlss5::State::eOtherMod))
        ui.TextDisabled(T("Restart with other DLSS 5 add-ons disabled to give MFGUnlocker ownership of the provider."));
    if (snapshot.dlss5State == static_cast<uint32_t>(dlss5::State::eCreateFailed)
        || snapshot.dlss5State == static_cast<uint32_t>(dlss5::State::eEvaluateFailed))
    {
        snprintf(line, sizeof(line), "DLSSNR 0x%08X", snapshot.dlss5Result);
        OptiScalerTabStatus(ui, Tone::Bad, line);
    }
}

// portstatus.1: the XeFG fields the tab's card reads, copied right after this thread's own status read. Integers only,
// so no string of gXessUi is read on OptiScaler's menu thread.
xess_ui::State OptiScalerTabPortView()
{
    xess_ui::State view{};
    view.available = gXessUi.available;
    view.optiScaler = gXessUi.optiScaler;
    view.modelActive = gXessUi.modelActive;
    view.tensorCores = gXessUi.tensorCores;
    view.tensorCoreLaunches = gXessUi.tensorCoreLaunches;
    view.gpuVendorId = gXessUi.gpuVendorId;
    view.adapterVendor = gXessUi.adapterVendor;
    return view;
}

// portstatus.1: the model OptiScaler's XeFG runs (XMX or CUDA port), as the shared menu's Model and Tensor cores rows.
void OptiScalerTabXefg(const OptiPluginApi& ui, const xess_ui::State& x)
{
    ui.Section(T("XeFG"));
    char line[256]{};
    const auto row = [&](const char* name, const char* shown, const char* more) {
        if (more && more[0]) snprintf(line, sizeof(line), "%s: %s, %s", name, shown, more);
        else snprintf(line, sizeof(line), "%s: %s", name, shown);
        ui.Text(line);
    };
    const auto port = xess_ui::ActivePort(x);
    if (port == xess_ui::FgPort::CudaPort) row(T("Model"), T("CUDA port"), T("int8 layers on the tensor cores"));
    else if (port == xess_ui::FgPort::XmxPort) row(T("Model"), T("XMX port"), T("on the shader cores"));
    else row(T("Model"), T("Intel 2x model"), nullptr);
    if (x.providerFailing) row(T("Frame generation"), T("No generated frames"), T("the provider is failing; see the RTXMFG log"));
    if (port != xess_ui::FgPort::Intel2x)
        ui.ItemTooltip(T("XMX port: MFG Unlocker's port of Intel's multi-frame XMX model, run as DXIL shaders on any "
            "GPU. CUDA port: the same model with its int8 layers run as CUDA kernels on RTX tensor cores."));
    switch (xess_ui::PortTensorCoreNote(x))
    {
    case xess_ui::PortTensorCores::Ready: row(T("Tensor cores"), T("Ready"), T("no layer has run yet")); break;
    case xess_ui::PortTensorCores::OffSetting:
        row(T("Tensor cores"), T("Off"), T("XeFGTensorCores = off in OptiScaler.ini"));
        break;
    case xess_ui::PortTensorCores::Unavailable:
        row(T("Tensor cores"), T("Off"), T("not available on this GPU or driver"));
        break;
    default: break;
    }
}

void DrawOptiScalerTabFeatures(const OptiPluginApi& ui)
{
    // The backend rewrites its status about once a second; read it at most four times a second.
    static MfgUnlockReShadeSnapshot snapshot{};
    static xess_ui::State port{};
    static bool haveSnapshot = false, stale = false;
    const ULONGLONG now = GetTickCount64();
    if (now >= gOptiTabNextRead)
    {
        gOptiTabNextRead = now + 250;
        MfgUnlockReShadeSnapshot read{};
        stale = BackendGetSnapshot(&read) == FALSE;
        if (!stale)
        {
            snapshot = read;
            port = OptiScalerTabPortView();
            haveSnapshot = true;
        }
    }

    ui.Text(T("Frame generation is set in OptiScaler's menu. This menu offers the DLSS presets and DLSS 5."));
    if (!haveSnapshot)
    {
        OptiScalerTabStatus(ui, Tone::Pending, T("Waiting for current backend status. Saved settings are preserved."));
        return;
    }
    if (stale) OptiScalerTabStatus(ui, Tone::Pending, T("Status is temporarily unavailable; controls are paused."));

    const bool superResolution = snapshot.dlssSr.running != FALSE;
    const bool rayReconstruction = snapshot.dlssRr.running != FALSE;
    const bool frameGeneration = DlssgPresetShown(snapshot);
    const bool neuralRendering = Dlss5TabShown(snapshot);
    // portstatus.1: XeFG's model first, also in a game without DLSS (no "Waiting for DLSS" then).
    // Each Section closes the card before it; OptiScaler closes the last one after the draw.
    const bool xefg = xess_ui::ActivePort(port) != xess_ui::FgPort::None;
    if (xefg) OptiScalerTabXefg(ui, port);
    if (!superResolution && !rayReconstruction && !frameGeneration && !neuralRendering)
    {
        if (!xefg) ui.TextDisabled(T("Waiting for DLSS"));
        return;
    }
    if (superResolution || rayReconstruction || frameGeneration)
    {
        ui.Section(T("DLSS presets"));
        ui.Disabled(stale);
        if (superResolution) OptiScalerTabPreset(ui, snapshot.dlssSr, false, stale);
        if (rayReconstruction) OptiScalerTabPreset(ui, snapshot.dlssRr, true, stale);
        if (frameGeneration) OptiScalerTabDlssgPreset(ui, snapshot, stale);
        ui.Disabled(false);
    }
    if (neuralRendering)
    {
        ui.Section(T("DLSS 5"));
        OptiScalerTabDlss5(ui, snapshot, stale);
    }
}

// GitHub issue 122: the log toggle, last in the tab and on its own card (the plugin API has no collapsing header).
// OptiScaler's menu thread owns this view.
RuntimeLogView gOptiTabRuntimeLog;

void OptiScalerTabDiagnostics(const OptiPluginApi& ui)
{
    ui.Section(T("Diagnostics"));
    bool on = RuntimeLogSaved(gOptiTabRuntimeLog);
    if (ui.Checkbox(OptiScalerTabLabel(T("Write log file"), "runtimelog"), &on))
        RuntimeLogSave(gOptiTabRuntimeLog, on);
    ui.ItemTooltip(RuntimeLogHelp());
    Tone tone = Tone::Pending;
    if (const char* note = RuntimeLogNote(gOptiTabRuntimeLog, tone))
        OptiScalerTabStatus(ui, tone, note);
}

void DrawOptiScalerTabContent(const OptiPluginApi& ui)
{
    DrawOptiScalerTabFeatures(ui);
    OptiScalerTabDiagnostics(ui);
}

// keybinds.1: the standalone menu's Keybinds tab, its keys and what they do. Bindings are saved in
// RTX40MFG-UI.ini [Keybinds] (keybinds_policy.h) and start unbound. Render-thread state, under gUiMutex.
bool gStandaloneDraw = false; // set while standalone_ui::Draw draws the menu: only there the tab shows
bool gD3D11Session = false;   // dx11menu.1: set while it draws on a DirectX 11 swapchain
struct KeybindState
{
    std::array<keybinds::Binding, keybinds::kActionCount> bindings{};
    std::array<bool, keybinds::kActionCount> down{};
    bool loaded = false, showNote = true, saveFailed = false, textInput = false, tabDrawn = false;
    int menuKey = 0;                     // the overlay's menu key, as last polled
    float keyColumn = 0.0f;              // the widest key name this frame: the buttons after it line up
    int capture = -1;                    // the action whose key is being bound
    std::array<bool, 256> heldAtCapture{};
    keybinds::Action queued = keybinds::Action::eCount;
    std::string note, rowNote;           // the pill's text; a note under the row just bound
    uint64_t noteUntil = 0;
    int rowNoteAction = -1;
    // One change in flight: a frame generation key waits until the status shows the last one (at most 3 s).
    bool pending = false, pendingXemfg = false;
    keybinds::Mode pendingMode{};
    uint64_t pendingSince = 0;
    // keybinds.3: the same for a DLSS preset key, until the status shows the preset requested.
    bool presetPending = false, presetPendingRr = false;
    uint32_t presetPendingValue = 0;
    uint64_t presetPendingSince = 0;
    uint32_t logLines = 0;
};
KeybindState gKeys;
// The tab's strings drawn outside this block, marked here so each catalog keeps the tab's strings together; and an
// existing string translated by name, so its catalog entry keeps its place.
const char* const kKeybindsTab = N_("Keybinds");
const char* KeybindsTabLabel() { return T(kKeybindsTab); }
const char* const kFpsReadoutOn = N_("FPS readout on");
const char* const kFpsReadoutOff = N_("FPS readout off");
constexpr const char* kFollowGame = "Follow game";
constexpr const char* kGameOrDriverPreset = "Game / driver default";

const char* KeybindLabel(keybinds::Action action)
{
    using keybinds::Action;
    switch (action)
    {
    case Action::eRaise: return T("Raise multiplier");
    case Action::eLower: return T("Lower multiplier");
    case Action::eDynamic: return T("Dynamic on/off");
    case Action::eFollowGame: return T(kFollowGame);
    case Action::eOff: return T("Off");
    case Action::eFpsReadout: return T("FPS readout on/off");
    case Action::ePinPanel: return T("Pin panel");
    case Action::ePresetNext: return T("Next preset");
    case Action::ePresetPrevious: return T("Previous preset");
    case Action::ePresetDefault: return T(kGameOrDriverPreset);
    default:
    {
        static const char* fixed[]{"2x", "3x", "4x", "5x", "6x"};
        return fixed[std::clamp(keybinds::FixedMultiplier(action), 2, 6) - 2];
    }
    }
}

std::string BindingName(keybinds::Binding binding)
{
    std::string text;
    if (binding.modifiers & keybinds::kCtrl) text += "Ctrl+";
    if (binding.modifiers & keybinds::kShift) text += "Shift+";
    if (binding.modifiers & keybinds::kAlt) text += "Alt+";
    return text + KeyName(binding.key);
}

uint8_t HeldModifiers() noexcept
{
    return static_cast<uint8_t>((PhysicalKeyDown(VK_CONTROL) ? keybinds::kCtrl : 0)
        | (PhysicalKeyDown(VK_SHIFT) ? keybinds::kShift : 0) | (PhysicalKeyDown(VK_MENU) ? keybinds::kAlt : 0));
}

void LoadKeybinds()
{
    gKeys.loaded = true;
    const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    if (path.empty()) return;
    for (size_t i = 0; i < keybinds::kActionCount; ++i)
    {
        wchar_t text[64]{};
        GetPrivateProfileStringW(L"Keybinds", keybinds::kIniNames[i], L"", text, static_cast<DWORD>(std::size(text)),
            path.c_str());
        gKeys.bindings[i] = keybinds::Parse(text);
    }
    gKeys.showNote = GetPrivateProfileIntW(L"Keybinds", L"ShowNote", 1, path.c_str()) != 0;
}

// An unbound action's line is removed.
void SaveKeybind(size_t index)
{
    const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
    const auto value = keybinds::Format(gKeys.bindings[index]);
    gKeys.saveFailed = path.empty() || !WritePrivateProfileStringW(L"Keybinds", keybinds::kIniNames[index],
        value.empty() ? nullptr : value.c_str(), path.c_str());
}

void LogKeybind(keybinds::Action action, const char* target, const char* outcome, const keybinds::Mode* mode = nullptr,
    int preset = -1)
{
    if (gKeys.logLines >= 200) return;
    ++gKeys.logLines;
    wchar_t line[192]{};
    swprintf_s(line, L"MFG_PROXY_UI keybind: action=%ls target=%hs outcome=%hs%ls", keybinds::kIniNames[static_cast<size_t>(action)],
        target, outcome, mode ? (mode->followGame ? L" mode=follow" : mode->dynamic ? L" mode=dynamic" : L" mode=fixed") : L"");
    if (mode && !mode->followGame)
    {
        wchar_t frames[24]{};
        swprintf_s(frames, L" multiplier=%d", mode->multiplier);
        wcscat_s(line, frames);
    }
    if (preset >= 0)
    {
        wchar_t value[24]{};
        swprintf_s(value, L" preset=%d", preset);
        wcscat_s(line, value);
    }
    single_module::Log(line);
}

// The pill at the top of the screen for 1.5 s, unless the tab's note switch is off.
void KeyNote(const char* format, ...)
{
    if (!gKeys.showNote) return;
    char text[192]{};
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gKeys.note = text;
    gKeys.noteUntil = GetTickCount64() + 1500;
}

std::string KeyModeName(const keybinds::Mode& mode)
{
    if (mode.followGame) return T(kFollowGame);
    if (mode.dynamic) return T("Dynamic");
    if (mode.multiplier <= 1) return T("Off");
    return std::to_string(mode.multiplier) + "x";
}

// The Mode list the key follows: the DLSS frame generation panel's (Off on RTX 30/20, Follow game, fixed modes up to
// the safe maximum, Dynamic while supported; none without the bridge on RTX 40/50), or XeMFG's (2x-6x and Dynamic).
bool KeybindModes(const MfgUnlockReShadeSnapshot& snapshot, bool xemfg, keybinds::Offered& offered,
    keybinds::Mode& current)
{
    if (xemfg)
    {
        offered = {true, false, true, 2, 6};
        current = {false, gXessUi.xemfgDynamic != FALSE,
            static_cast<int>(std::clamp<uint32_t>(gXessUi.xemfgSavedFrames ? gXessUi.xemfgSavedFrames : 2, 2, 6))};
        return true;
    }
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    if (xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily) != xess_ui::Section::Dlss
        || !xess_ui::DlssMfgFamily(snapshot.gpuFamily) || snapshot.gpuFamily == 3u
        || std::strcmp(snapshot.patchRoute, "remix") == 0) return false;
    const bool ampere = UiAmpere();
    offered.any = ampere || snapshot.bridgeReady;
    offered.followGame = true;
    offered.dynamic = ui_dynamic_mfg::Classify(snapshot) == ui_dynamic_mfg::Availability::Supported;
    offered.lowest = ampere ? 1 : 2;
    offered.highest = std::clamp(static_cast<int>(snapshot.safeMaximumMultiplier), 2, 6);
    current = {gFollowGameMode, gDynamicMode, gMultiplier};
    return true;
}

void RunModeKeybind(keybinds::Action action)
{
    MfgUnlockReShadeSnapshot snapshot{};
    if (!BackendGetSnapshot(&snapshot))
    {
        KeyNote("%s", T("Frame generation keys apply once frame generation is ready"));
        LogKeybind(action, "none", "no-status");
        return;
    }
    const bool xemfg = gXessUi.xemfgStandalone && !gXessUi.optiScaler;
    const char* target = xemfg ? "xemfg" : "dlssg";
    const uint64_t now = GetTickCount64();
    if (gKeys.pending)
    {
        const auto& wanted = gKeys.pendingMode;
        const bool shown = gKeys.pendingXemfg
            ? gXessUi.xemfgSavedFrames == static_cast<uint32_t>(wanted.multiplier)
                && (gXessUi.xemfgDynamic != FALSE) == wanted.dynamic
            : (snapshot.followGameMode != FALSE) == wanted.followGame
                && (wanted.followGame || ((snapshot.dynamicMode != FALSE) == wanted.dynamic
                    && snapshot.desiredMultiplier == static_cast<uint32_t>(wanted.multiplier)))
                && !(snapshot.gameFrameGenerationOn && snapshot.desiredRevision != snapshot.appliedRevision);
        if (!shown && now - gKeys.pendingSince < 3000)
        {
            KeyNote("%s", T("Applying the last change..."));
            LogKeybind(action, target, "pending");
            return;
        }
        gKeys.pending = false;
    }
    keybinds::Offered offered{};
    keybinds::Mode current{};
    if (!KeybindModes(snapshot, xemfg, offered, current) || !offered.any)
    {
        KeyNote("%s", T("Frame generation keys apply once frame generation is ready"));
        LogKeybind(action, target, "unavailable");
        return;
    }
    const auto result = keybinds::Select(action, current, offered);
    const char* format = xemfg ? T("XeMFG: %s") : T("Frame generation: %s");
    switch (result.outcome)
    {
    case keybinds::Outcome::eUnavailable:
        KeyNote(T("%s is unavailable right now"), KeybindLabel(action));
        LogKeybind(action, target, "unavailable");
        return;
    case keybinds::Outcome::eHighest:
        KeyNote(T("Highest mode here: %s"), (std::to_string(std::clamp(offered.highest, 2, 6)) + "x").c_str());
        LogKeybind(action, target, "highest", &current);
        return;
    case keybinds::Outcome::eLowest:
        KeyNote(T("Lowest mode: %s"), "2x");
        LogKeybind(action, target, "lowest", &current);
        return;
    case keybinds::Outcome::eUnchanged:
        KeyNote(format, KeyModeName(current).c_str());
        LogKeybind(action, target, "unchanged", &current);
        return;
    case keybinds::Outcome::eChanged:
        break;
    }
    bool saved = false;
    if (xemfg)
    {
        auto wanted = gXessUi;
        wanted.xemfgDynamic = result.mode.dynamic ? TRUE : FALSE;
        saved = NativeFilePersistXemfg(true, static_cast<uint32_t>(result.mode.multiplier), wanted) != FALSE;
        gXemfgSaveFailed = !saved;
        if (saved)
        {
            // As the menu's XeMFG rows: shown at once, the next status read confirms it.
            gXessUi = wanted;
            gXessUi.xemfgStandalone = TRUE;
            gXessUi.xemfgSavedFrames = static_cast<uint32_t>(result.mode.multiplier);
        }
    }
    else
    {
        const keybinds::Mode before{gFollowGameMode, gDynamicMode, gMultiplier};
        gFollowGameMode = result.mode.followGame;
        gDynamicMode = result.mode.dynamic;
        gMultiplier = result.mode.multiplier;
        ApplySettings(nullptr, true);
        saved = gLastApplyAccepted;
        if (!saved)
        {
            gFollowGameMode = before.followGame;
            gDynamicMode = before.dynamic;
            gMultiplier = before.multiplier;
        }
    }
    LogKeybind(action, target, saved ? "changed" : "refused", &result.mode);
    if (!saved)
    {
        KeyNote("%s", T("The change could not be saved"));
        return;
    }
    gKeys.pending = true;
    gKeys.pendingXemfg = xemfg;
    gKeys.pendingMode = result.mode;
    gKeys.pendingSince = now;
    KeyNote(format, KeyModeName(result.mode).c_str());
}

// keybinds.3: the DLSS preset keys follow the menu's shared SR/RR preset row. The DLSS panel always has it; the XeSS
// panel only while the game runs DLSS upscaling; neither after an adapter change or under RTX Remix. The row shows
// Ray Reconstruction's preset while it runs, else Super Resolution's (RayReconstructionShown).
bool PresetKeysOffered(const MfgUnlockReShadeSnapshot& snapshot)
{
    if (snapshot.gpuFamily == 3u || std::strcmp(snapshot.patchRoute, "remix") == 0) return false;
    const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
    return xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily) != xess_ui::Section::Xess
        || snapshot.dlssSr.running || snapshot.dlssRr.running;
}

const char* PresetName(uint32_t preset)
{
    return preset == dlss_sr::kGameOrDriver ? T(kGameOrDriverPreset) : SrPresetLetter(preset);
}

void RunPresetKeybind(keybinds::Action action)
{
    MfgUnlockReShadeSnapshot snapshot{};
    if (!BackendGetSnapshot(&snapshot) || !PresetKeysOffered(snapshot))
    {
        KeyNote("%s", T("DLSS preset keys apply while the game runs DLSS"));
        LogKeybind(action, "none", "unavailable");
        return;
    }
    const bool rr = RayReconstructionShown(snapshot);
    const char* target = rr ? "rr" : "sr";
    const uint64_t now = GetTickCount64();
    if (gKeys.presetPending)
    {
        const auto& pending = gKeys.presetPendingRr ? snapshot.dlssRr : snapshot.dlssSr;
        if (pending.presetRequested != gKeys.presetPendingValue && now - gKeys.presetPendingSince < 3000)
        {
            KeyNote("%s", T("Applying the last change..."));
            LogKeybind(action, target, "pending");
            return;
        }
        gKeys.presetPending = false;
    }
    // As the combo: the status' requested preset when valid, else the one saved here.
    const MfgUnlockDlssFeature& view = rr ? snapshot.dlssRr : snapshot.dlssSr;
    uint32_t& selected = rr ? gDlssRrPreset : gDlssSrPreset;
    const bool valid = rr ? dlss_sr::IsValidRrSelection(view.presetRequested) : dlss_sr::IsValidSelection(view.presetRequested);
    const uint32_t current = valid ? view.presetRequested : selected;
    const uint32_t next = rr ? keybinds::StepPreset(action, kRrPresetValues, std::size(kRrPresetValues), current)
        : keybinds::StepPreset(action, kSrPresetValues, std::size(kSrPresetValues), current);
    const char* format = rr ? T("RR preset: %s") : T("SR preset: %s");
    if (next == current)
    {
        KeyNote(format, PresetName(current));
        LogKeybind(action, target, "unchanged", nullptr, static_cast<int>(current));
        return;
    }
    const BOOL saved = NativeFilePersistPreset(rr ? "dlssRrPreset" : "dlssSrPreset", next);
    (rr ? gRrPresetSaveFailed : gSrPresetSaveFailed) = !saved;
    LogKeybind(action, target, saved ? "changed" : "refused", nullptr, static_cast<int>(next));
    if (!saved)
    {
        KeyNote("%s", T("The change could not be saved"));
        return;
    }
    selected = next;
    gKeys.presetPending = true;
    gKeys.presetPendingRr = rr;
    gKeys.presetPendingValue = next;
    gKeys.presetPendingSince = now;
    KeyNote(format, PresetName(next));
}

void BeginKeyCapture(size_t index)
{
    gKeys.capture = static_cast<int>(index);
    gKeys.rowNoteAction = -1;
    // Keys already down (the click on Change key) do not bind.
    for (int key = 0; key < 256; ++key) gKeys.heldAtCapture[key] = PhysicalKeyDown(key);
}

// A key the menu key or Hold to show original already uses stays theirs; one another action used moves here.
void BindKey(size_t index, keybinds::Binding binding)
{
    gKeys.capture = -1;
    gKeys.rowNote.clear();
    gKeys.rowNoteAction = static_cast<int>(index);
    const char* owner = nullptr;
    if (!binding.modifiers && binding.key == gKeys.menuKey) owner = T("the menu key");
    else if (!binding.modifiers && gDlss5.compareKey && binding.key == gDlss5.compareKey) owner = T("Hold to show original");
    char text[192]{};
    if (owner)
    {
        snprintf(text, sizeof(text), T("%s is already used by %s"), BindingName(binding).c_str(), owner);
        gKeys.rowNote = text;
        return;
    }
    for (size_t other = 0; other < keybinds::kActionCount; ++other)
        if (other != index && gKeys.bindings[other] == binding)
        {
            gKeys.bindings[other] = {};
            SaveKeybind(other);
            snprintf(text, sizeof(text), T("Moved from %s"), KeybindLabel(static_cast<keybinds::Action>(other)));
            gKeys.rowNote = text;
        }
    gKeys.bindings[index] = binding;
    gKeys.down[index] = true; // the key just pressed to bind it does not also run it
    SaveKeybind(index);
}

void CaptureKey()
{
    if (gKeys.capture < 0) return;
    if (PhysicalKeyDown(VK_ESCAPE)) { gKeys.capture = -1; return; }
    for (int key = 1; key < 0xFF && gKeys.capture >= 0; ++key)
    {
        if (!keybinds::Bindable(key)) continue;
        const bool down = PhysicalKeyDown(key);
        if (down && !gKeys.heldAtCapture[key])
            BindKey(static_cast<size_t>(gKeys.capture), {static_cast<uint8_t>(key), HeldModifiers()});
        gKeys.heldAtCapture[key] = down;
    }
}

// One action's row: its key, Change key (the next key pressed with any Ctrl, Shift or Alt held binds it; Esc cancels)
// and Clear. never: the frame generation shown has no such mode (Off on RTX 40/50, Follow game and Off with XeMFG).
void KeybindRow(keybinds::Action action, bool never)
{
    const size_t index = static_cast<size_t>(action);
    auto& binding = gKeys.bindings[index];
    ImGui::PushID(static_cast<int>(index));
    ControlRow(KeybindLabel(action));
    ImGui::BeginDisabled(never);
    // The key's name (or the binding prompt) takes the column's width, so every row's buttons line up.
    const auto padToColumn = [] {
        if (gKeys.keyColumn > ImGui::GetItemRectSize().x)
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::Dummy(ImVec2(gKeys.keyColumn - ImGui::GetItemRectSize().x, 0.0f));
        }
        ImGui::SameLine();
    };
    if (gKeys.capture == static_cast<int>(index))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("Press a key... Esc cancels"));
        padToColumn();
        if (ImGui::Button(l10n::Label("Cancel"))) gKeys.capture = -1;
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        if (binding.key) ImGui::TextUnformatted(BindingName(binding).c_str());
        else ImGui::TextDisabled("%s", T("Not set"));
        padToColumn();
        if (ImGui::Button(l10n::Label("Change key"))) BeginKeyCapture(index);
        if (binding.key)
        {
            ImGui::SameLine();
            if (ImGui::Button(l10n::Label("Clear")))
            {
                binding = {};
                gKeys.rowNoteAction = -1;
                SaveKeybind(index);
            }
        }
    }
    ImGui::EndDisabled();
    if (never) HelpMarker(T("The frame generation in use has no such mode."));
    else if (action == keybinds::Action::eRaise || action == keybinds::Action::eLower)
        HelpMarker(T("Steps through the modes the Mode list offers."));
    else if (action == keybinds::Action::ePresetNext || action == keybinds::Action::ePresetPrevious)
        HelpMarker(T("Steps through the presets of the DLSS the game runs: Super Resolution's (J, K, L, M) or, while "
            "Ray Reconstruction runs, Ray Reconstruction's (D, E, F). After the last one it starts again."));
    if (gKeys.rowNoteAction == static_cast<int>(index) && !gKeys.rowNote.empty())
        Note(Tone::Pending, "%s", gKeys.rowNote.c_str());
    else if (binding.key && !binding.modifiers && binding.key == gKeys.menuKey)
        Note(Tone::Pending, T("Also used by %s"), T("the menu key"));
    else if (binding.key && !binding.modifiers && gDlss5.compareKey && binding.key == gDlss5.compareKey)
        Note(Tone::Pending, T("Also used by %s"), T("Hold to show original"));
    ImGui::PopID();
}

// shared: beside OptiScaler's menu, whose frame generation and FPS overlay are its own (keybinds::SharedAction).
// d3d11: a DirectX 11 game (dx11menu.1), with no frame generation, FPS readout or DLSS preset to act on
// (keybinds::D3D11Action): Pin panel and DLSS 5's own key only.
void DrawKeybinds(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale, bool shared, bool d3d11)
{
    if (!gKeys.loaded) LoadKeybinds();
    gKeys.tabDrawn = true;
    CaptureKey();
    if (!statusStale && !gDlss5Dirty && !ImGui::IsAnyItemActive()) gDlss5 = snapshot.dlss5;
    const bool compareRow = BackendUsesNativeFile() && Dlss5TabShown(snapshot);
    // The key names' column: Not set, the binding prompt and every name shown below, in all three grids.
    gKeys.keyColumn = std::max(ImGui::CalcTextSize(T("Not set")).x, ImGui::CalcTextSize(T("Press a key... Esc cancels")).x);
    for (size_t i = 0; i < keybinds::kActionCount; ++i)
        if (gKeys.bindings[i].key && (!d3d11 || keybinds::D3D11Action(static_cast<keybinds::Action>(i))))
            gKeys.keyColumn = std::max(gKeys.keyColumn, ImGui::CalcTextSize(BindingName(gKeys.bindings[i]).c_str()).x);
    if (compareRow && gDlss5.compareKey)
        gKeys.keyColumn = std::max(gKeys.keyColumn, ImGui::CalcTextSize(KeyName(static_cast<int>(gDlss5.compareKey)).c_str()).x);
    Note(Tone::Dim, "%s", T("Keys work with the menu open or closed, and still reach the game."));
    HelpMarker(T("To bind a combination, hold Ctrl, Shift or Alt and press the key. A combination avoids the game's "
        "own keys."));
    if (!d3d11)
    {
        const bool xemfg = gXessUi.xemfgStandalone && !gXessUi.optiScaler;
        const bool ampere = !xemfg && UiAmpere();
        Section(T("Frame generation"));
        if (shared)
            Note(Tone::Dim, "%s", T("Frame generation is set in OptiScaler's menu, so its keys are not offered here."));
        else if (BeginGrid("##keysfg"))
        {
            for (size_t i = 0; i <= static_cast<size_t>(keybinds::Action::e6x); ++i)
            {
                const auto action = static_cast<keybinds::Action>(i);
                KeybindRow(action, (action == keybinds::Action::eOff && !ampere)
                    || (action == keybinds::Action::eFollowGame && xemfg));
            }
            ImGui::EndTable();
        }
        // keybinds.3: one set of preset keys for the menu's shared SR/RR preset row, and which one they change now.
        Section(T("DLSS presets"));
        if (BeginGrid("##keyspresets"))
        {
            KeybindRow(keybinds::Action::ePresetNext, false);
            KeybindRow(keybinds::Action::ePresetPrevious, false);
            KeybindRow(keybinds::Action::ePresetDefault, false);
            ImGui::EndTable();
        }
        if (PresetKeysOffered(snapshot))
            Note(Tone::Dim, T("Now applies to %s"),
                RayReconstructionShown(snapshot) ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
        else
            Note(Tone::Dim, "%s", T("DLSS preset keys apply while the game runs DLSS"));
    }
    Section(T("Display"));
    if (BeginGrid("##keysdisplay"))
    {
        if (!shared && !d3d11) KeybindRow(keybinds::Action::eFpsReadout, false);
        KeybindRow(keybinds::Action::ePinPanel, false);
        ImGui::EndTable();
    }
    // The DLSS 5 tab's own Hold to show original key (dlss5CompareKey), also here while that tab shows.
    if (compareRow)
    {
        Section(T("DLSS 5"));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##keysdlss5"))
        {
            ControlRow(T("Hold to show original"));
            if (CompareKeyControl(gDlss5, gKeys.keyColumn))
            {
                gDlss5SaveFailed = !NativeFilePersistDlss5(gDlss5);
                gDlss5LastSave = GetTickCount64();
                gDlss5Dirty = false;
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    // No key that acts in a DirectX 11 game shows a note (Pin panel shows the panel itself).
    if (!d3d11 && ImGui::Checkbox(l10n::Label("Show a note when a key changes a setting"), &gKeys.showNote))
    {
        const auto path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        gKeys.saveFailed = path.empty() || !WritePrivateProfileStringW(L"Keybinds", L"ShowNote",
            gKeys.showNote ? L"1" : L"0", path.c_str());
        if (!gKeys.showNote) gKeys.noteUntil = 0;
    }
    if (gKeys.saveFailed)
        Note(Tone::Bad, "%s", T("The keybinds work for this session, but could not be saved."));
}

// dx11menu.1: the menu on a DirectX 11 game's swapchain. Frame generation, XeMFG and the DLSS presets act through
// DirectX 12 or Vulkan only, so their tabs and keys are not offered. DLSS 5's tab follows its provider as everywhere;
// the Keybinds tab (Pin panel) and the footer's settings are always offered.
void DrawD3D11(const MfgUnlockReShadeSnapshot& snapshot, bool statusStale)
{
    const bool neuralRendering = BackendUsesNativeFile() && Dlss5TabShown(snapshot);
    if (!neuralRendering) Note(Tone::Pending, "%s", T("Waiting for DLSS"));
    if (!BeginTabs("##mfgtabs")) return;
    if (neuralRendering && TabItem(T("DLSS 5")))
    {
        DrawDlss5(snapshot, statusStale);
        ImGui::EndTabItem();
    }
    if (TabItem(T(kKeybindsTab)))
    {
        DrawKeybinds(snapshot, statusStale, false, true);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DrawSettings(reshade::api::effect_runtime* runtime)
{
    gOverlayVisitedThisFrame = true;
    gOverlayDockedThisFrame = ImGui::IsWindowDocked();
    if (gOverlayDockedThisFrame && !gDefaultDockInitialized)
    {
        gDefaultDockInitialized = true;
        reshade::set_config_value(runtime, kConfigSection,
            "DefaultDockInitialized", true);
    }

    MfgUnlockReShadeSnapshot snapshot{};
    bool haveSnapshot = BackendGetSnapshot(&snapshot) != FALSE;
    bool statusStale = false;
    bool nativeFile = false;
    nativeFile = BackendUsesNativeFile();
    if (nativeFile)
    {
        if (gLastNativeDisplayPath != gNativeStatusPath)
        {
            gLastNativeDisplayPath = gNativeStatusPath;
            gHaveNativeDisplay = false;
        }
        if (haveSnapshot)
        {
            gLastNativeDisplay = snapshot;
            gHaveNativeDisplay = true;
        }
        else
        {
            statusStale = true;
            if (gHaveNativeDisplay) { snapshot = gLastNativeDisplay; haveSnapshot = true; }
        }
    }
    if (!haveSnapshot)
    {
        if (nativeFile)
        {
            SetFooter(N_("Waiting for status"), Tone::Pending);
            Note(Tone::Pending, T("Waiting for current backend status. Saved settings are preserved."));
            return;
        }
        SetFooter(N_("Not connected"), Tone::Bad);
        Note(Tone::Bad, T("%s backend is not loaded."), MFG_PRODUCT);
        ImGui::TextWrapped(T("The integrated core is unavailable. Check the "
            "startup log and restart the game with one %s module."), MFG_PRODUCT);
        return;
    }
    if (nativeFile && statusStale)
        SetFooter(N_("Paused"), Tone::Pending);
    else
        SetFooter("", Tone::Dim);
    if (gD3D11Session)
    {
        DrawD3D11(snapshot, statusStale);
        return;
    }
    if (nativeFile && snapshot.optiScalerUiOwner == 1u)
    {
        DrawOptiScalerShared(snapshot, statusStale, gStandaloneDraw);
        return;
    }

    {
        const bool dlssOffered = snapshot.setOptionsSeen || snapshot.activeWrapperObserved;
        if (xess_ui::SettingsSection(gXessUi, gLastGameFrameGeneration, dlssOffered, snapshot.gpuFamily)
            == xess_ui::Section::Xess)
        {
            // DLSS 5 keeps its tab beside XeSS frame generation (XeMFG standalone included) while its
            // provider is present, as with OptiScaler.
            // keybinds.1: the standalone menu's Keybinds tab comes last.
            const bool neuralRendering = BackendUsesNativeFile() && Dlss5TabShown(snapshot);
            if ((neuralRendering || gStandaloneDraw) && BeginTabs("##mfgtabs"))
            {
                if (TabItem(T("Frame generation")))
                {
                    DrawXessPanel(snapshot, statusStale);
                    ImGui::EndTabItem();
                }
                if (neuralRendering && TabItem(T("DLSS 5")))
                {
                    DrawDlss5(snapshot, statusStale);
                    ImGui::EndTabItem();
                }
                if (gStandaloneDraw && TabItem(T(kKeybindsTab)))
                {
                    DrawKeybinds(snapshot, statusStale, false);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            else
                DrawXessPanel(snapshot, statusStale);
            return;
        }
    }
    if (snapshot.gpuFamily == 3u)
    {
        Note(Tone::Pending, T("The game changed graphics adapters. Restart the game to select a new GPU."));
        return;
    }

    MfgUnlockReShadeSnapshot presetView = snapshot;
    if (std::strcmp(snapshot.patchRoute, "remix") == 0)
    {
        Section(T("Frame generation"));
        Note(Tone::Normal, T("RTX Remix detected. Frame generation and its multiplier are managed "
            "in the RTX Remix menu."));
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##remixfg"))
        {
            PresetControlRow(presetView, statusStale);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        PresetNotes(presetView);
        Section(T("Status"));
        if (BeginGrid("##remixstatus"))
        {
            StatusRow(T("Provider patch"), snapshot.backportReadyAtCreate ? Tone::Good : Tone::Pending, "%s",
                snapshot.backportReadyAtCreate ? T("Ready at feature creation") : T("Waiting for frame generation"));
            PresetStatusRow(presetView);
            ImGui::EndTable();
        }
        return;
    }
    const int safeMaximumMultiplier = std::clamp(
        static_cast<int>(snapshot.safeMaximumMultiplier), 2, 6);
    const auto dynamicAvailability = ui_dynamic_mfg::Classify(snapshot);
    const bool dynamicCapabilityKnown = dynamicAvailability != ui_dynamic_mfg::Availability::Checking;
    const bool dynamicSupported = dynamicAvailability == ui_dynamic_mfg::Availability::Supported;
    const bool dynamicUnsupported = dynamicAvailability == ui_dynamic_mfg::Availability::Unavailable;
    const bool wrapperControlUnavailable = snapshot.activeWrapperObserved
        && snapshot.universalRouteFailure
            == static_cast<uint32_t>(universal_route_policy::Failure::eActiveWrapperUnpatched);
    bool settingsChanged = false;
    static const char* fixedModes[] = {
        "2x", "3x", "4x", "5x", "6x"
    };

    // A status protocol from another build makes every value below unreliable.
    const bool protocolMismatch = snapshot.statusProtocolVersion != MFG_STATUS_VERSION_NUMBER;
    if (protocolMismatch)
        Note(Tone::Bad, T("Restart with the matching single-module build. "
            "The current status protocol is older than this UI expects."));

    // DLSS 5 has its own tab while its provider is present, and the standalone menu a Keybinds tab; else no tab bar.
    const bool dlss5Tab = BackendUsesNativeFile() && Dlss5TabShown(snapshot);
    const bool tabs = (dlss5Tab || gStandaloneDraw) && BeginTabs("##mfgtabs");
    if (!tabs || TabItem(T("Frame generation")))
    {
        Section(T("Frame generation"));
        // Keep an already-open popup and its appearance stable across a missed
        // read. Disabled input prevents cached capabilities from authorizing writes.
        BeginStaleDisabled(statusStale);
        if (BeginGrid("##dlssfg"))
        {
            // Capability can disappear in menus and during map transitions. Rendering
            // that state must not overwrite the user's persisted Dynamic request.
            if (UiAmpere())
            {
                gDynamicExperimental56 = false;
                gGeneratedOnlyDebug = false;
                static const char* ampereModes[]{N_("Off"), "2x FG", "3x MFG", "4x MFG",
                    N_("5x MFG (experimental)"), N_("6x MFG (experimental)")};
                const char* dynamicLabel = T(ui_dynamic_mfg::Label(dynamicAvailability));
                const char* selectedMode = gFollowGameMode ? T("Follow game")
                    : gDynamicMode ? dynamicLabel : T(ampereModes[std::clamp(gMultiplier, 1, 6) - 1]);
                ControlRow(T("Mode"));
                if (ImGui::BeginCombo("##framegeneration", selectedMode))
                {
                    if (ImGui::Selectable(T("Follow game"), gFollowGameMode))
                    { gFollowGameMode = true; gDynamicMode = false; settingsChanged = true; }
                    for (int value = 1; value <= 6; ++value)
                    {
                        ImGui::BeginDisabled(value > safeMaximumMultiplier);
                        if (ImGui::Selectable(T(ampereModes[value - 1]), !gFollowGameMode && !gDynamicMode && gMultiplier == value)
                            && value <= safeMaximumMultiplier)
                        { gFollowGameMode = false; gMultiplier = value; gDynamicMode = false; settingsChanged = true; }
                        ImGui::EndDisabled();
                    }
                    ImGui::BeginDisabled(!dynamicSupported);
                    if (ImGui::Selectable(dynamicLabel, gDynamicMode) && dynamicSupported)
                    { gFollowGameMode = false; gMultiplier = std::max(gMultiplier, 2); gDynamicMode = true; settingsChanged = true; }
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
            }
            else
            {
                const char* dynamicLabel = T(ui_dynamic_mfg::Label(dynamicAvailability));
                const char* currentMode = !snapshot.bridgeReady
                    ? T("Unavailable")
                    : gFollowGameMode
                    ? T("Follow game")
                    : gDynamicMode
                        ? dynamicLabel
                        : fixedModes[
                            std::clamp(gMultiplier, 2, 6) - 2];
                ControlRow(T("MFG mode"));
                ImGui::BeginDisabled(!snapshot.bridgeReady);
                if (ImGui::BeginCombo("##mfgmode", currentMode))
                {
                    if (ImGui::Selectable(T("Follow game"), gFollowGameMode))
                    {
                        gFollowGameMode = true;
                        gDynamicMode = false;
                        settingsChanged = true;
                    }
                    const int listedMaximum = snapshot.nvidiaPolicyCeilingMultiplier >= 2
                        && snapshot.nvidiaPolicyCeilingMultiplier <= 6
                        ? static_cast<int>(snapshot.nvidiaPolicyCeilingMultiplier) : safeMaximumMultiplier;
                    for (int multiplier = 2; multiplier <= listedMaximum;
                         ++multiplier)
                    {
                        const bool selected = !gFollowGameMode && !gDynamicMode
                            && gMultiplier == multiplier;
                        ImGui::BeginDisabled(multiplier > safeMaximumMultiplier);
                        if (ImGui::Selectable(fixedModes[multiplier - 2], selected)
                            && multiplier <= safeMaximumMultiplier)
                        {
                            gFollowGameMode = false;
                            gDynamicMode = false;
                            gMultiplier = multiplier;
                            settingsChanged = true;
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::BeginDisabled(!dynamicSupported);
                    if (ImGui::Selectable(dynamicLabel, gDynamicMode)
                        && dynamicSupported)
                    {
                        gFollowGameMode = false;
                        gDynamicMode = true;
                        settingsChanged = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }
            HelpMarker(T("Follow game uses the game's own frame generation setting.\n"
                "Dynamic adjusts frame generation toward a target frame rate; it does not set a hard FPS limit."));
            if (!gFollowGameMode && gDynamicMode)
            {
                ImGui::BeginDisabled(!dynamicSupported);
                ControlRow(T("Dynamic target"));
                if (ImGui::Checkbox(l10n::Label("Display refresh rate"), &gDynamicLockToRefreshRate))
                    settingsChanged = true;
                if (!gDynamicLockToRefreshRate)
                {
                    ControlRow(T("Target FPS"));
                    ImGui::DragInt("##dynamictarget", &gDynamicCustomTargetFrameRate, 1.0f, 1, 1000, "%d FPS");
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        settingsChanged = true;
                }
                ImGui::EndDisabled();
            }
            PresetControlRow(presetView, statusStale);
            GpuNameControlRow();
            // Switched on, the XeSS panel with XeMFG's own rows replaces this one.
            if (XemfgAvailable()) XemfgControlRows();
            ImGui::EndTable();
        }
        XemfgNotes();
        // Notes for the controls above: what frame generation waits for, then
        // how the selection applies. The game-off note replaces the generic waits.
        // admitoff.1: on RTX 30/20 a failed startup admission holds the game's
        // DLSS-G at Off; that replaces the game-off note and the waits.
        const bool admissionHeld = UiAmpere() && snapshot.ampereAdmissionUnavailable;
        if (admissionHeld)
            Note(Tone::Bad, T("DLSS frame generation could not start on this GPU, so it stays off "
                "for this session. Restart the game to try again."));
        const bool gameFgOffNoted = !admissionHeld && FrameGenerationOffNote(snapshot);
        const uint32_t vendor = GameGpuVendor();
        if (vendor && vendor != kNvidiaVendor)
            Note(Tone::Dim, T("DLSS multi-frame generation needs an NVIDIA RTX GPU."));
        if (!protocolMismatch && !snapshot.bridgeReady && !admissionHeld)
        {
            if (wrapperControlUnavailable)
                Note(Tone::Pending, T("Frame generation stays under the game's control. "
                    "This wrapper has no verified MFG override support."));
            else if (snapshot.intervalValidSamples && !snapshot.setOptionsSeen)
                Note(Tone::Pending, T("DLSS-G activity observed; waiting for the game's FG control calls."));
            else if (!gameFgOffNoted)
                Note(Tone::Pending, "%s", snapshot.universalRouteFailure
                    == static_cast<uint32_t>(universal_route_policy::Failure::eAwaitingOptionsRequest)
                    ? T("Waiting for the game to request frame generation.")
                    : T("Waiting for an active DLSS-G pipeline."));
        }
        if (!admissionHeld && (snapshot.streamlineRebuildRequired
            || snapshot.pipelineMayPredateDetour))
        {
            Note(Tone::Pending, T("Toggle frame generation off then on, or restart "
                "the game, to recreate the pipeline with this selection."));
        }
        if (!UiAmpere())
        {
            if (dynamicUnsupported)
                Note(Tone::Dim, T("Dynamic MFG is unavailable with the current limit or runtime. "
                    "Its full frame-generation range must fit the limit."));
            else if (!gFollowGameMode && gDynamicMode && !dynamicSupported)
                Note(Tone::Pending, T("Dynamic selection is saved; waiting for runtime support."));
            if (!gFollowGameMode && !gDynamicMode
                && (!snapshot.bridgeReady || gMultiplier > safeMaximumMultiplier))
                Note(Tone::Pending, T("Saved MFG request: %dx (unavailable in this session)"), gMultiplier);
        }
        if (gLastApplyAttempted && !gPresetSaveFailed
            && (!gLastApplyAccepted || !gLastNativeConfigPersisted))
        {
            if (gLastApplyAccepted)
                Note(Tone::Bad, T("Request accepted, but the configuration was not saved."));
            else
                Note(Tone::Bad, T("The selection could not be applied or saved."));
        }
        PresetNotes(presetView);
        GpuNameNotes();

        Section(T("Latency and display"));
        if (BeginGrid("##dlsslatency"))
        {
            PresentationControlRows(snapshot, statusStale);
            ImGui::EndTable();
        }
        PresentationNotes(snapshot);

        // Ray Reconstruction replaces Super Resolution in the game, so their
        // controls share one section.
        const bool rayReconstructionShown = RayReconstructionShown(snapshot);
        const MfgUnlockDlssFeature& dlssView = rayReconstructionShown ? snapshot.dlssRr : snapshot.dlssSr;
        Section(rayReconstructionShown ? T("DLSS Ray Reconstruction") : T("DLSS Super Resolution"));
        if (BeginGrid("##dlsssr"))
        {
            DlssPresetControlRow(dlssView, rayReconstructionShown, statusStale);
            ImGui::EndTable();
        }
        DlssNotes(dlssView, rayReconstructionShown);
        ImGui::EndDisabled();

        // Every readback in one grid, in the order of the controls above.
        Section(T("Status"));
        char value[64]{}, detail[128]{};
        if (BeginGrid("##dlssstatus"))
        {
            GameRows(snapshot, true);
            const bool fgKnown = snapshot.setOptionsSeen && snapshot.setOptionsAccepted;
            snprintf(detail, sizeof(detail), "%s", !snapshot.setOptionsSeen ? T("game intent: unknown")
                : snapshot.gameFrameGenerationOn ? T("game intent: on") : T("game intent: off"));
            if (admissionHeld)
                StatusDetail(T("FG state"), Tone::Bad, T("Unavailable"), detail);
            else
                StatusDetail(T("FG state"), !fgKnown ? Tone::Dim : snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal,
                    !fgKnown ? T("Unknown") : snapshot.appliedFrameGenerationOn ? T("On") : T("Off"), detail);
            if (snapshot.followGameMode)
                StatusRow(T("Requested"), Tone::Normal, "%s", T("Follow game"));
            else if (snapshot.appliedRevision == 0 || snapshot.dynamicMode != snapshot.appliedDynamicMode
                || (!snapshot.dynamicMode && snapshot.desiredMultiplier != snapshot.appliedMultiplier))
                StatusRow(T("Requested"), Tone::Pending, snapshot.dynamicMode ? T("Dynamic") : "%ux",
                    snapshot.desiredMultiplier);
            if (snapshot.appliedRevision != 0)
            {
                snprintf(value, sizeof(value), snapshot.appliedDynamicMode ? T("Dynamic, %ux") : "%ux",
                    snapshot.appliedMultiplier);
                StatusRow(T("Applied"), snapshot.appliedFrameGenerationOn ? Tone::Good : Tone::Normal, "%s", value);
            }
            const char* nvidiaList = !snapshot.nvidiaCompatibilityResolved ? T("NVIDIA list unavailable")
                : snapshot.nvidiaCompatibilityTier == 2 ? T("NVIDIA lists no MFG for this game")
                : snapshot.nvidiaCompatibilityTier != 4 && snapshot.nvidiaCompatibilityTier != 6
                    ? T("game not on NVIDIA's list") : nullptr;
            if (!nvidiaList)
            {
                snprintf(detail, sizeof(detail), T("NVIDIA lists %ux"), snapshot.nvidiaCompatibilityTier);
                nvidiaList = detail;
            }
            if (wrapperControlUnavailable)
                StatusDetail(T("Limit"), Tone::Bad, T("Unavailable"), T("the current wrapper has no verified MFG override"));
            else if (snapshot.activeWrapperObserved)
            {
                snprintf(value, sizeof(value), snapshot.compatibilityFallback ? T("%dx (safe fallback)") : "%dx",
                    safeMaximumMultiplier);
                StatusDetail(T("Limit"), Tone::Normal, value, nvidiaList);
            }
            else
                StatusDetail(T("Limit"), Tone::Pending, T("Waiting"), T("for an active DLSS-G wrapper"));
            char limitHelp[256]{};
            if (snapshot.nvidiaCompatibilityTier == 2)
                snprintf(limitHelp, sizeof(limitHelp), "%s",
                    T("NVIDIA lists ordinary Frame Generation only. The mod keeps its override limit at 2x."));
            else if (snapshot.nvidiaPolicyCeilingMultiplier > static_cast<uint32_t>(safeMaximumMultiplier))
                snprintf(limitHelp, sizeof(limitHelp),
                    T("The listed %ux ceiling requires a compatible runtime. The current limit is %dx."),
                    snapshot.nvidiaPolicyCeilingMultiplier, safeMaximumMultiplier);
            else
                snprintf(limitHelp, sizeof(limitHelp), "%s",
                    T("The highest mode the current DLSS-G runtime supports, and the ceiling\n"
                    "NVIDIA's override list gives this game."));
            HelpMarker(limitHelp);
            if (!gFollowGameMode && gDynamicMode)
            {
                if (gDynamicLockToRefreshRate) snprintf(value, sizeof(value), "%s", T("Display refresh rate"));
                else snprintf(value, sizeof(value), "%d FPS", gDynamicCustomTargetFrameRate);
                if (!snapshot.appliedDynamicTargetValid)
                    StatusDetail(T("Dynamic target"), Tone::Pending, value, T("waiting for an accepted Dynamic request"));
                else
                {
                    if (snapshot.appliedDynamicTargetFrameRate == 0.0f)
                        snprintf(detail, sizeof(detail), "%s", T("applied: display refresh rate"));
                    else
                        snprintf(detail, sizeof(detail), T("applied: %.2f FPS"), snapshot.appliedDynamicTargetFrameRate);
                    StatusDetail(T("Dynamic target"), Tone::Good, value, detail);
                }
            }
            const bool fpsSampleCurrent = snapshot.realFpsMilli > 0
                && snapshot.fpsSampleAgeMs <= 2000;
            if (snapshot.appliedFrameGenerationOn && fpsSampleCurrent)
            {
                if (snapshot.dlssFpsMilli > 0)
                    StatusRow("FPS", Tone::Normal, T("%.1f real, %.1f DLSS"),
                        static_cast<double>(snapshot.realFpsMilli) / 1000.0,
                        static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
                else
                    StatusRow("FPS", Tone::Normal, T("%.1f real, DLSS unavailable"),
                        static_cast<double>(snapshot.realFpsMilli) / 1000.0);
                PinnedFpsToggle();
            }
            else if (snapshot.appliedFrameGenerationOn && gSampleFrameTelemetry)
            {
                StatusRow("FPS", Tone::Dim, "%s", T("Measuring..."));
                PinnedFpsToggle();
            }
            else if (gPinnedFps.load(std::memory_order_acquire))
            {
                // Keep a pinned readout reachable while frame generation is off.
                StatusRow("FPS", Tone::Dim, "%s", T("DLSS FG off"));
                PinnedFpsToggle();
            }
            LatencyStatusRow(); // latmon.2
            PresetStatusRow(presetView);
            PresentationStatusRows(snapshot, statusStale);
            DlssStatusRows(dlssView, rayReconstructionShown);
            ImGui::EndTable();
        }
        DrawWitcherHair();

        Section(T("Advanced"));
        if (ImGui::CollapsingHeader(l10n::Label("Diagnostics")))
        {
            if (!UiAmpere())
            {
                ImGui::BeginDisabled(statusStale);
                if (ImGui::Checkbox("Generated frames only",
                        &gGeneratedOnlyDebug))
                {
                    settingsChanged = true;
                }
                ImGui::EndDisabled();
                HelpMarker("Debug: shows only generated frames.");
            }
            RuntimeLogToggle();
            if (ImGui::TreeNode("Presentation"))
            {
                ImGui::Text("Runtime FG V-Sync capability: %s", !snapshot.fgVsyncSupportKnown
                    ? "unknown" : snapshot.fgVsyncSupported ? "supported" : "unavailable");
                DrawPresentationDebug(snapshot);
                ImGui::TreePop();
            }
            if (UiAmpere() && ImGui::TreeNode(snapshot.gpuFamily == 4u ? "Turing RTX" : "Ampere"))
            {
                ImGui::Text("%s | %s | FG preset: %s", snapshot.gpuFamily == 4u ? "Turing SM75" : "Ampere SM86",
                    snapshot.graphicsApi == 2u ? "Vulkan" : snapshot.graphicsApi == 1u ? "D3D12" : "API pending",
                    snapshot.ampereLegacySinglePreset ? "Original preset"
                        : DlssgPresetName(snapshot.dlssgPresetSelectionFrozen
                            ? snapshot.dlssgPresetLatched : snapshot.dlssgPresetRequested));
                ImGui::Text("Ampere provider: %s | Preset query: %s",
                    snapshot.ampereProgramReadyMfg ? "MFG program prepared" : "pending",
                    snapshot.presetQueries ? "observed" : "pending");
                ImGui::Text("Kernel program: %s | Cache status: %u", snapshot.ampereKernelImage == 2 ? "native SM86"
                    : snapshot.ampereKernelImage == 1 ? (snapshot.gpuFamily == 4u ? "SM75 PTX" : "SM86 PTX") : "pending", snapshot.ampereNativeCacheStatus);
                if (snapshot.ampereFailure)
                    ImGui::TextWrapped("Ampere failure: %u (%s)", snapshot.ampereFailure,
                        ampere_diagnostics::FailureName(snapshot.ampereFailure));
                ImGui::Text("Create attempts: %llu | blocked before NGX: %llu | created: %llu",
                    static_cast<unsigned long long>(snapshot.ampereCreateAttempts),
                    static_cast<unsigned long long>(snapshot.ampereCreateBlockedBeforeProvider),
                    static_cast<unsigned long long>(snapshot.ampereCreatedFeatures));
                ImGui::Text("Evaluation attempts: %llu | accepted: %llu",
                    static_cast<unsigned long long>(snapshot.ampereEvaluateAttempts),
                    static_cast<unsigned long long>(snapshot.ampereEvaluations));
                if (snapshot.ampereCandidateVersionMajor)
                    ImGui::Text("Observed provider candidate: %u.%u.%u",
                        snapshot.ampereCandidateVersionMajor, snapshot.ampereCandidateVersionMinor,
                        snapshot.ampereCandidateVersionBuild);
                ImGui::Text("Program preparation: %s (%u)",
                    ampere_diagnostics::PreparationName(snapshot.amperePreparationStage), snapshot.amperePreparationStage);
                if (snapshot.ampereStartupFailureMask)
                    ImGui::TextWrapped("Startup incomplete: %s%s%s%s%s%s%s%s",
                        (snapshot.ampereStartupFailureMask & 0x01u) ? "Streamline startup; " : "",
                        (snapshot.ampereStartupFailureMask & 0x02u) ? "NGX runtime route; " : "",
                        (snapshot.ampereStartupFailureMask & 0x04u) ? "provider route; " : "",
                        (snapshot.ampereStartupFailureMask & 0x08u) ? "startup capacity; " : "",
                        (snapshot.ampereStartupFailureMask & 0x10u) ? "D3D12/Vulkan device; " : "",
                        (snapshot.ampereStartupFailureMask & 0x20u) ? "Ampere adapter; " : "",
                        (snapshot.ampereStartupFailureMask & 0x40u) ? "kernel program; " : "",
                        (snapshot.ampereStartupFailureMask & 0x80u) ? "fatal boundary; " : "");
                if (snapshot.ampereAdmissionUnavailable)
                    ImGui::Text("DLSS-G held Off: admission unavailable | held requests: %llu",
                        static_cast<unsigned long long>(snapshot.ampereAdmissionHeldRequests));
                if (snapshot.amperePrimaryFailure)
                    ImGui::TextWrapped("Current first failure: %u (%s) | NGX: 0x%08X",
                        snapshot.amperePrimaryFailure, ampere_diagnostics::FailureName(snapshot.amperePrimaryFailure),
                        snapshot.amperePrimaryNgxResult);
                if (snapshot.ampereFirstFailure)
                    ImGui::TextWrapped("First failure this run: %u (%s) | NGX: 0x%08X",
                        snapshot.ampereFirstFailure, ampere_diagnostics::FailureName(snapshot.ampereFirstFailure),
                        snapshot.ampereFirstNgxResult);
                if (snapshot.ampereLastFailure && snapshot.ampereLastFailure != snapshot.amperePrimaryFailure)
                    ImGui::TextWrapped("Latest failure: %u (%s) | NGX: 0x%08X",
                        snapshot.ampereLastFailure, ampere_diagnostics::FailureName(snapshot.ampereLastFailure),
                        snapshot.ampereLastNgxResult);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Active route"))
            {
                ImGui::Text("Core protocol: %u | Bridge: %s",
                    snapshot.statusProtocolVersion,
                    snapshot.bridgeReady ? "ready" : "not ready");
                ImGui::Text("Fail-closed reason: %s (%u)",
                    snapshot.universalRouteFailureReason[0]
                        ? snapshot.universalRouteFailureReason : "none",
                    snapshot.universalRouteFailure);
                ImGui::TextWrapped("Wrapper: %s",
                    snapshot.activeWrapperPath[0]
                        ? snapshot.activeWrapperPath : "waiting for a real call");
                ImGui::Text("Wrapper version: %u.%u.%u.%u | generation: %llu",
                    snapshot.activeWrapperVersionMajor,
                    snapshot.activeWrapperVersionMinor,
                    snapshot.activeWrapperVersionBuild,
                    snapshot.activeWrapperVersionPrivate,
                    static_cast<unsigned long long>(
                        snapshot.activeWrapperGeneration));
                ImGui::Text("Control: %s (%s) | State: %s (%s)",
                    snapshot.activeControlPath[0]
                        ? snapshot.activeControlPath : "none",
                    snapshot.activeControlDetour[0]
                        ? snapshot.activeControlDetour : "none",
                    snapshot.activeStatePath[0]
                        ? snapshot.activeStatePath : "none",
                    snapshot.activeStateDetour[0]
                        ? snapshot.activeStateDetour : "none");
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Active provider"))
            {
                ImGui::TextWrapped("Provider: %s",
                    snapshot.activeProviderPath[0]
                        ? snapshot.activeProviderPath : "waiting for FG Create");
                ImGui::Text("Provider version: %u.%u.%u.%u | generation: %llu",
                    snapshot.activeProviderVersionMajor,
                    snapshot.activeProviderVersionMinor,
                    snapshot.activeProviderVersionBuild,
                    snapshot.activeProviderVersionPrivate,
                    static_cast<unsigned long long>(
                        snapshot.activeProviderGeneration));
                ImGui::Text("Selected by: %s | Create: %s | Evaluate: %s",
                    snapshot.providerSelectionSource[0]
                        ? snapshot.providerSelectionSource : "none",
                    snapshot.providerCreateDetour[0]
                        ? snapshot.providerCreateDetour : "none",
                    snapshot.providerEvaluateDetour[0]
                        ? snapshot.providerEvaluateDetour : "none");
                ImGui::Text("Midpoint ready at first Create: %s",
                    YesNo(snapshot.midpointReadyAtFirstCreate));
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Control and lifecycle"))
            {
                ImGui::Text("Requested/applied revision: %llu/%llu",
                    static_cast<unsigned long long>(snapshot.desiredRevision),
                    static_cast<unsigned long long>(snapshot.appliedRevision));
                ImGui::Text("Route last call/accepted revision: %llu/%llu",
                    static_cast<unsigned long long>(
                        snapshot.activeLastCallRevision),
                    static_cast<unsigned long long>(
                        snapshot.activeLastAcceptedRevision));
                ImGui::Text("FG applied/intent: %s/%s | Off accepted: %s | Release observed: %s",
                    snapshot.appliedFrameGenerationOn ? "on" : "off",
                    snapshot.gameFrameGenerationOn ? "on" : "off",
                    YesNo(snapshot.frameGenerationOffAccepted),
                    YesNo(snapshot.releaseObserved));
                ImGui::Text("Release entry: %s | Recreate required: %s",
                    snapshot.releaseEntryCurrent ? "covered" : "unavailable",
                    YesNo(snapshot.streamlineRebuildRequired));
                ImGui::Text("NVIDIA max: %s | Available max: %s",
                    snapshot.nvidiaCompatibilityTier == 4
                            || snapshot.nvidiaCompatibilityTier == 6
                        ? (snapshot.nvidiaCompatibilityTier == 6 ? "6x" : "4x")
                        : snapshot.nvidiaCompatibilityTier == 2 ? "no MFG override listed" : "unknown",
                    snapshot.activeWrapperObserved
                        ? (safeMaximumMultiplier == 6 ? "6x"
                            : safeMaximumMultiplier == 5 ? "5x"
                            : safeMaximumMultiplier == 4 ? "4x"
                            : safeMaximumMultiplier == 3 ? "3x" : "2x")
                        : "detecting");
                if (snapshot.getStateSeen && snapshot.lastGetStateResult == 0)
                {
                    ImGui::Text("Runtime: %u presented | max %u generated",
                        snapshot.actualFramesPresented,
                        snapshot.numFramesToGenerateMax);
                }
                ImGui::Text("DLSS FPS source: %s", snapshot.fpsOutputSource == 4 ? "DLSS-G presented-frame count"
                    : snapshot.fpsOutputSource == 2 ? "DXGI present count"
                    : snapshot.fpsOutputSource == 1 ? "present callback" : "unavailable");
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Temporal interval trace"))
            {
                ImGui::Text("Log: %s | Samples: %llu valid, %llu invalid, %llu dropped",
                    snapshot.intervalLogReady ? "ready" : "off",
                    static_cast<unsigned long long>(snapshot.intervalValidSamples),
                    static_cast<unsigned long long>(snapshot.intervalInvalidSamples),
                    static_cast<unsigned long long>(snapshot.intervalDroppedSamples));
                if (snapshot.intervalValidSamples > 0)
                {
                    ImGui::Text("Last interval: count %d | index %d | position %u/%u",
                        snapshot.intervalLastCount, snapshot.intervalLastIndex,
                        snapshot.intervalLastPositionNumerator,
                        snapshot.intervalLastPositionDenominator);
                }
                if (snapshot.intervalLogFile[0])
                    ImGui::Text("Trace: %s, beside the runtime log", snapshot.intervalLogFile);
                ImGui::TreePop();
            }
            ImGui::Text("UI recomposition: %s", snapshot.gameUiRecompositionEnabled ? "on (the game's own)" : "off");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("DLSS-G UI recomposition is left to the game; RTXMFG never enables it.\n"
                    "An NVIDIA frame generation preset can still turn it on in the driver.");
        }
        DrawWitcherHairDiagnostics();
        if (tabs) ImGui::EndTabItem();
    }
    if (tabs)
    {
        if (dlss5Tab && TabItem(T("DLSS 5")))
        {
            DrawDlss5(snapshot, statusStale);
            ImGui::EndTabItem();
        }
        if (gStandaloneDraw && TabItem(T(kKeybindsTab)))
        {
            DrawKeybinds(snapshot, statusStale, false);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (settingsChanged)
        ApplySettings(runtime, true);
}
}

namespace standalone_ui
{
void Initialize(HMODULE self)
{
    gSelf = self;
    if (!gInitialized.exchange(true, std::memory_order_acq_rel))
    {
        LoadSettings();
        const auto path = SettingsPath("RTX40MFG-UI.ini");
        gPinnedFps.store(!path.empty() && GetPrivateProfileIntW(L"Overlay", L"PinnedFps", 0, path.c_str()) == 1,
            std::memory_order_release);
        gPinnedLatency.store(!path.empty() && GetPrivateProfileIntW(L"Overlay", L"PinnedLatency", 0, path.c_str()) == 1,
            std::memory_order_release);
    }
}
bool PinnedFps()
{
    return gPinnedFps.load(std::memory_order_acquire);
}
bool PinnedLatency()
{
    return gPinnedLatency.load(std::memory_order_acquire);
}
void DrawOptiScalerTab(const OptiPluginApi& ui) noexcept
{
    try
    {
        // RTXMFG's own overlay initializes this panel; the hidden UI mode never starts it.
        Initialize(single_module::Self());
        DrawOptiScalerTabContent(ui);
    }
    catch (...)
    {
    }
}
void Draw(bool d3d11)
{
    gLabelWidth = gLabelWidthSeen;
    gLabelWidthSeen = 0.0f;
    gStandaloneDraw = true;
    gD3D11Session = d3d11;
    gKeys.tabDrawn = false;
    DrawSettings(nullptr);
    gStandaloneDraw = false;
    gD3D11Session = false;
    // A key being bound ends with its tab; typing into a field holds the keys back.
    if (!gKeys.tabDrawn) gKeys.capture = -1;
    gKeys.textInput = ImGui::GetIO().WantTextInput;
}

keybinds::Action PollKeybinds(int menuKey)
{
    if (!gKeys.loaded) LoadKeybinds();
    gKeys.menuKey = menuKey;
    if (gKeys.capture >= 0) return keybinds::Action::eCount;
    auto pressed = keybinds::Action::eCount;
    uint8_t modifiers = 0;
    bool modifiersRead = false;
    for (size_t i = 0; i < keybinds::kActionCount; ++i)
    {
        const auto binding = gKeys.bindings[i];
        // A binding equal to the menu key stays the menu's.
        if (!binding.key || (!binding.modifiers && binding.key == menuKey)) { gKeys.down[i] = false; continue; }
        if (!modifiersRead) { modifiers = HeldModifiers(); modifiersRead = true; }
        const bool down = PhysicalKeyDown(binding.key) && modifiers == binding.modifiers;
        if (down && !gKeys.down[i] && !gKeys.textInput && pressed == keybinds::Action::eCount)
            pressed = static_cast<keybinds::Action>(i);
        gKeys.down[i] = down;
    }
    return pressed;
}
void QueueKeybind(keybinds::Action action)
{
    gKeys.queued = action;
}
bool KeybindFrameWanted()
{
    return gKeys.queued != keybinds::Action::eCount || (gKeys.noteUntil && GetTickCount64() < gKeys.noteUntil);
}
void RunQueuedKeybind()
{
    const auto action = gKeys.queued;
    if (action == keybinds::Action::eCount) return;
    gKeys.queued = keybinds::Action::eCount;
    try
    {
        if (action == keybinds::Action::eFpsReadout)
        {
            const bool on = !gPinnedFps.load(std::memory_order_acquire);
            SetPinnedFps(on);
            KeyNote("%s", on ? T(kFpsReadoutOn) : T(kFpsReadoutOff));
            LogKeybind(action, "display", on ? "on" : "off");
        }
        else if (keybinds::ModeAction(action)) RunModeKeybind(action);
        else if (keybinds::PresetAction(action)) RunPresetKeybind(action);
    }
    catch (...) {}
}
const char* KeybindNote()
{
    if (!gKeys.noteUntil || GetTickCount64() >= gKeys.noteUntil) { gKeys.noteUntil = 0; return nullptr; }
    return gKeys.note.c_str();
}
bool KeybindCapturing()
{
    return gKeys.capture >= 0;
}
bool TextInputActive()
{
    return gKeys.textInput;
}
void CancelKeybindCapture()
{
    gKeys.capture = -1;
    gKeys.textInput = false;
}
void DrawFooterStatus()
{
    if (!gFooterStatus[0]) return;
    const char* text = T(gFooterStatus);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ToneColor(gFooterTone), "%s", text);
    if (std::strcmp(gFooterStatus, "Paused") == 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Status is temporarily unavailable; controls are paused."));
}
// latmon.2: the pinned latency readout, drawn below the FPS readout (overlay_platform.cpp), four times a second.
void DrawPinnedLatency()
{
    static char text[96]{};
    static bool live = false;
    static ULONGLONG nextRead = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextRead)
    {
        nextRead = now + 250;
        const latency_probe::Live reading = latency_probe::ReadLive();
        live = false;
        if (!reading.available)
            snprintf(text, sizeof(text), "%s", T("Latency unavailable"));
        else if (LatencyCurrent(reading))
        {
            snprintf(text, sizeof(text), T("Latency %.1f ms | 1%% high %.1f"), reading.meanUs / 1000.0,
                reading.p99Us / 1000.0);
            live = true;
        }
        else if (reading.state == kLatencyNoMarkers)
            snprintf(text, sizeof(text), "%s", T("Latency: needs Reflex"));
        else if (reading.state == kLatencyNoOutputs || reading.state == kLatencyNoFlipTimes)
            snprintf(text, sizeof(text), "%s", T("Latency unavailable"));
        else
            snprintf(text, sizeof(text), "%s", T("Latency --"));
    }
    ImGui::TextColored(ToneColor(live ? Tone::Good : Tone::Dim), "%s", text);
}
void DrawPinnedFps()
{
    // The same values as the menu's FPS row. The backend rewrites its status
    // about once a second; read it at most twice a second during play.
    static char text[96]{};
    static bool live = false;
    static ULONGLONG nextRead = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextRead)
    {
        nextRead = now + 500;
        MfgUnlockReShadeSnapshot snapshot{};
        if (!BackendGetSnapshot(&snapshot))
        {
            snprintf(text, sizeof(text), "%s", T("FPS status unavailable"));
            live = false;
        }
        // XeMFG standalone's frame rates (the snapshot read above refreshed gXessUi from the same status).
        else if (xess_ui::XemfgRuns(gXessUi))
        {
            const auto& x = gXessUi;
            live = x.xemfgGenerating && xess_ui::XemfgFpsCurrent(x);
            if (!x.xemfgGenerating)
                snprintf(text, sizeof(text), "%s", T("XeMFG off"));
            else if (!live)
                snprintf(text, sizeof(text), "%s", T("Real -- | XeMFG --"));
            else
                snprintf(text, sizeof(text), T("Real %.0f | XeMFG %.0f"), x.xemfgBaseFpsMilli / 1000.0,
                    x.xemfgOutputFpsMilli / 1000.0);
        }
        else if (!snapshot.appliedFrameGenerationOn)
        {
            snprintf(text, sizeof(text), "%s", T("DLSS FG off"));
            live = false;
        }
        else if (snapshot.realFpsMilli == 0 || snapshot.fpsSampleAgeMs > 2000)
        {
            snprintf(text, sizeof(text), "%s", T("Real -- | DLSS --"));
            live = false;
        }
        else
        {
            const double real = static_cast<double>(snapshot.realFpsMilli) / 1000.0;
            if (snapshot.dlssFpsMilli > 0)
                snprintf(text, sizeof(text), T("Real %.0f | DLSS %.0f"), real,
                    static_cast<double>(snapshot.dlssFpsMilli) / 1000.0);
            else
                snprintf(text, sizeof(text), T("Real %.0f | DLSS --"), real);
            live = true;
        }
    }
    // The menu's status green while values are live; its dim text otherwise.
    ImGui::TextColored(ToneColor(live ? Tone::Good : Tone::Dim), "%s", text);
}
}
