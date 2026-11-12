#include <Windows.h>
#include "ampere_wrapper_capacity.h"
#include "ui_status_json.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool pass, const char* message)
{
    ++checks;
    failures += !pass;
    std::cout << (pass ? "PASS " : "FAIL ") << message << '\n';
}
std::string WaitStatus(const std::filesystem::path& path, const char* marker)
{
    const ULONGLONG started = GetTickCount64();
    std::string text;
    do
    {
        std::ifstream file(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(file), {});
        // Do not deny atomic status replacement throughout the polling sleep.
        file.close();
        if (ui_status_json::CompleteObject(text) && text.find(marker) != std::string::npos)
            return text;
        Sleep(20);
    } while (GetTickCount64() - started < 7000);
    return text;
}
bool WaitUnloaded(const wchar_t* path)
{
    const ULONGLONG started = GetTickCount64();
    do
    {
        if (!GetModuleHandleW(path)) return true;
        // A concurrent inventory scan owns only a temporary loader reference.
        Sleep(10);
    } while (GetTickCount64() - started < 1000);
    return false;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto status = std::filesystem::current_path() / L"probe.status.json";
    SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH", status.c_str());
    SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH",
        (std::filesystem::current_path() / L"probe.config.json").c_str());
    HMODULE host = LoadLibraryW(L"sl.interposer.dll");
    Check(host != nullptr, "isolated interposer fixture loads");
    if (!host) return 2;
    using LoadProbe = HMODULE(WINAPI*)(const wchar_t*);
    auto load = reinterpret_cast<LoadProbe>(GetProcAddress(host, "FixtureLoadProbe"));
    Check(load != nullptr, "host exposes a native loader probe");
    if (!load) return 2;

    // Establish that this exact DLL has no independent self-retention. No
    // NVIDIA plugin startup, device, NGX feature or provider call is executed.
    HMODULE probe = load(argv[2]);
    Check(probe != nullptr, "exact local DLSS-G probe loads without RTXMFG");
    if (!probe) return 2;
    ampere_wrapper::Layout layout{};
    Check(ampere_wrapper::Discover(probe, layout), "probe has the independently verified allocation layout which triggered retention");
    Check(GetProcAddress(probe, "slGetPluginFunction") != nullptr, "probe exports the Streamline plugin resolver");
    Check(FreeLibrary(probe) != FALSE, "native host releases its probe reference");
    Check(GetModuleHandleW(argv[2]) == nullptr, "native probe unloads before the mod loads");
    if (failures) return 1;

    HMODULE core = LoadLibraryW(argv[1]);
    Check(core != nullptr, "exact integrated candidate loads");
    if (!core) return 2;
    const auto initial = WaitStatus(status, "\"streamlineLoaderDiscoveryInstalled\":true");
    Check(initial.find("\"streamlineLoaderDiscoveryInstalled\":true") != std::string::npos,
        "candidate intercepts the host loader before the probe");
    Check(initial.find("\"gpuFamily\":0") != std::string::npos,
        "no D3D12 adapter has selected a backend");
    if (failures) return 1;

    unsigned probes = 0;
    for (unsigned generation = 0; generation < 3; ++generation)
    {
        probe = load(argv[2]);
        Check(probe != nullptr, "intercepted host loads the next probe generation");
        if (!probe) break;
        ++probes;
        Check(FreeLibrary(probe) != FALSE, "intercepted host releases its probe reference");
        const bool unloaded = WaitUnloaded(argv[2]);
        Check(unloaded, "unknown-GPU discovery allows the probe generation to unload");
        if (!unloaded) break;
    }
    const std::string marker = "\"streamlineLoaderDiscoveryCalls\":" + std::to_string(probes) + ',';
    const auto observed = WaitStatus(status, marker.c_str());
    Check(probes != 0 && observed.find(marker) != std::string::npos,
        "every loaded probe crossed the real candidate's loader-return inspection");
    Check(observed.find("\"gpuFamily\":0") != std::string::npos,
        "probe discovery leaves GPU selection unknown");
    std::cout << checks << " checks, " << failures << " failures\n";
    // The mod owns a process-lifetime worker; let process exit drain it.
    return failures ? 1 : 0;
}
