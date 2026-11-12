// GPU name override in the shipping DLL. The runner's case directory
// holds RTX40MFG-UI.ini beside this executable; the mode adds the setting before
// the DLL loads. Off and unrecognized values must leave dxgi.dll untouched. On
// must publish the detours from the worker before startup status, and on a GPU
// that is not RTX 30 every description stays real.
#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "diagnostic_paths.h"
#include "ui_status_json.h"
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace
{
std::string ReadShared(const std::wstring& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[65536];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read) text.append(buffer, read);
    CloseHandle(file);
    return text;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring mode = argv[1];
    if (mode != L"off" && mode != L"on" && mode != L"invalid") return 2;
    const auto directory = std::filesystem::current_path();
    SetEnvironmentVariableW(L"RTX_MFG_CONFIG_PATH", (directory / L"startup.json").c_str());
    SetEnvironmentVariableW(L"RTX_MFG_STATUS_PATH", (directory / L"startup.status.json").c_str());
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto ini = std::filesystem::path(self).parent_path() / L"RTX40MFG-UI.ini";
    if (mode == L"on" && !WritePrivateProfileStringW(L"Compatibility", L"GpuNameOverride", L"1", ini.c_str())) return 2;
    if (mode == L"invalid" && !WritePrivateProfileStringW(L"Compatibility", L"GpuNameOverride", L"maybe", ini.c_str())) return 2;

    // Real descriptions and dxgi.dll's GetDesc1 entry bytes before the DLL exists here.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    std::vector<std::pair<Microsoft::WRL::ComPtr<IDXGIAdapter1>, DXGI_ADAPTER_DESC1>> adapters;
    for (UINT index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) adapters.emplace_back(adapter, desc);
    }
    if (adapters.empty()) return 2;
    auto* const entry = static_cast<const unsigned char*>((*reinterpret_cast<void***>(adapters.front().first.Get()))[10]);
    std::array<unsigned char, 16> before{};
    memcpy(before.data(), entry, before.size());

    if (!LoadLibraryW(argv[2])) return 1;
    std::string status;
    for (unsigned attempt = 0; attempt < 240; ++attempt)
    {
        std::ifstream file(directory / L"startup.status.json");
        status = {std::istreambuf_iterator<char>(file), {}};
        if (ui_status_json::CompleteObject(status)) break;
        Sleep(25);
    }
    const std::wstring logPath = diagnostic_paths::RuntimeLog();
    std::string log = ReadShared(logPath);

    unsigned failures = 0;
    auto check = [&](bool value, const char* text) { printf("%s %s\n", value ? "PASS" : "FAIL", text); if (!value) ++failures; };
    auto logged = [&](const char* fragment) { return log.find(fragment) != std::string::npos; };
    check(ui_status_json::CompleteObject(status), "fresh DLL publishes complete startup status");
    // Later builds append their own tokens, so gpuname.1 need not be the last one.
    check(logged("+gpuname.1 ") || logged("+gpuname.1+"), "the runtime log names the gpuname.1 build");
    const bool changed = memcmp(before.data(), entry, before.size()) != 0;
    if (mode == L"on")
    {
        check(logged("GPU name override: enabled=1 armed=1 methods=GetDesc,GetDesc1,GetDesc2,GetDesc3;"),
            "enabled override is armed on all four adapter methods before startup status");
        check(changed && entry[0] == 0xE9, "dxgi.dll GetDesc1 now enters the detour");
    }
    else
    {
        check(!logged("GPU name override: enabled="), "no override is armed");
        check(!changed, "dxgi.dll GetDesc1 is byte-identical");
        if (mode == L"invalid")
            check(logged("GPU name override: off; [Compatibility] GpuNameOverride in ")
                && logged("RTX40MFG-UI.ini is not 1 or 0"), "an unrecognized value is logged and stays off");
        else
            check(!logged("GPU name override"), "off by default writes nothing about the override");
    }

    // This executable is the game: an RTX 30 adapter is renamed only when enabled.
    bool rtx30Present = false, otherNvidia = false;
    for (const auto& [adapter, real] : adapters)
    {
        DXGI_ADAPTER_DESC1 seen{};
        const bool rtx30 = real.VendorId == 0x10DE && (wcsstr(real.Description, L"RTX 30") || wcsstr(real.Description, L"RTX 20"));
        rtx30Present |= rtx30;
        otherNvidia |= real.VendorId == 0x10DE && !rtx30;
        const bool expectReported = mode == L"on" && rtx30;
        const bool ok = SUCCEEDED(adapter->GetDesc1(&seen)) && seen.VendorId == real.VendorId
            && !memcmp(&seen.AdapterLuid, &real.AdapterLuid, sizeof(LUID))
            && seen.DedicatedVideoMemory == real.DedicatedVideoMemory
            && (expectReported ? seen.DeviceId == 0x2684 && !wcscmp(seen.Description, L"NVIDIA GeForce RTX 4090")
                               : seen.DeviceId == real.DeviceId && !wcscmp(seen.Description, real.Description));
        printf("INFO adapter vendor=0x%04X device=0x%04X '%ls' -> '%ls'\n", real.VendorId, real.DeviceId,
            real.Description, seen.Description);
        check(ok, expectReported ? "the game's RTX 30 adapter reports the RTX 40 identity"
                                 : "the game's adapter keeps its real identity");
    }
    if (mode == L"on" && otherNvidia)
    {
        // The worker drains the module log into the runtime log about every 100 ms.
        bool noted = false;
        for (unsigned attempt = 0; attempt < 100 && !noted; ++attempt)
        {
            log = ReadShared(logPath);
            noted = logged("vendor=0x10DE left unchanged (only NVIDIA RTX 20/30 adapters are reported as RTX 40)");
            if (!noted) Sleep(50);
        }
        check(noted, "the detour saw the game's query of a non-RTX 30 NVIDIA adapter and left it unchanged");
    }
    if (!rtx30Present) check(!logged("reported as 'NVIDIA GeForce RTX 4090'"), "nothing was reported without an RTX 30 adapter");
    printf("COMPLETE gpu name override mode=%ls failures=%u\n", mode.c_str(), failures);
    return failures ? 1 : 0;
}
