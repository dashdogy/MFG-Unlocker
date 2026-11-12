// shadPS4 model: an executable linked against mincore imports GetProcAddress from the library-loader API-set
// contract, and no KERNEL32.dll descriptor provides it. RTXMFG loads beside it as the statically imported
// version.dll and must still route the executable's GetProcAddress through its resolver hook: that hook is how a
// Vulkan game's instance and device reach the XeMFG DLSS-G provider (apisetgpa.1).
#include "single_module_status.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
// GitHub issue 122: the candidate rewrites its status file every 30 s; this test polls it, so it asks for every second.
[[maybe_unused]] static const bool kStatusFileEverySecond = SetEnvironmentVariableW(L"RTXMFG_DIAGNOSTIC_STATUS_HEARTBEAT", L"1") != FALSE;

namespace
{
bool Check(bool passed, const char* name)
{
    printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    fflush(stdout);
    return passed;
}

std::filesystem::path ModulePath(HMODULE module)
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? std::filesystem::path(path) : std::filesystem::path();
}

HMODULE Owner(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

// The DLL name of the import descriptor that lists GetProcAddress, or null.
const char* GetProcAddressImport()
{
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         descriptor->Name; ++descriptor)
    {
        if (!descriptor->OriginalFirstThunk) continue;
        for (auto* name = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
             name->u1.AddressOfData; ++name)
            if (!IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal) && !strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + name->u1.AddressOfData)->Name, "GetProcAddress"))
                return reinterpret_cast<const char*>(base + descriptor->Name);
    }
    return nullptr;
}
}

int wmain()
{
    constexpr char kContract[] = "api-ms-win-core-libraryloader-";
    const char* import = GetProcAddressImport();
    printf("GetProcAddress import: %s\n", import ? import : "(none)");
    bool passed = Check(import && !_strnicmp(import, kContract, sizeof(kContract) - 1),
        "executable imports GetProcAddress from the library-loader API set (mincore layout)");

    const auto executable = ModulePath(nullptr);
    HMODULE proxy = GetModuleHandleW(L"version.dll");
    const auto proxyPath = ModulePath(proxy);
    passed &= Check(proxy && !proxyPath.empty() && proxyPath.parent_path() == executable.parent_path(),
        "RTXMFG loaded as the startup version.dll import");
    if (!proxy) return 1;

    const auto query = reinterpret_cast<MfgSingleModuleQueryFn>(
        GetProcAddress(proxy, "MfgUnlockSingleModuleQuery"));
    MfgSingleModuleStatus status{};
    status.size = sizeof(status);
    passed &= Check(query && query(&status) && status.backendOwner == 1 && !status.duplicateSuppressed,
        "integrated backend initialized during process startup");

    // The static version.dll import keeps this process linked to the proxy.
    DWORD ignored = 0;
    passed &= Check(GetFileVersionInfoSizeW(proxyPath.c_str(), &ignored) != 0, "static version import forwards to Windows");

    const auto statusPath = proxyPath.parent_path() / L"RTXMFG-Universal.status.json";
    const std::string key = "\"mainResolverDiscoveryInstalled\":";
    std::string statusText;
    for (int attempt = 0; attempt < 150 && statusText.find(key) == std::string::npos; ++attempt)
    {
        Sleep(100);
        std::ifstream file(statusPath, std::ios::binary);
        statusText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    passed &= Check(statusText.find(key + "true") != std::string::npos,
        "main-executable resolver discovery installed on the API-set import");

    // Taking a dllimport address reads the slot.
    passed &= Check(Owner(reinterpret_cast<const void*>(&GetProcAddress)) == proxy,
        "main-executable GetProcAddress import runs through RTXMFG");

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    using TickFn = ULONGLONG (WINAPI*)();
    const auto tick = kernel32 ? reinterpret_cast<TickFn>(GetProcAddress(kernel32, "GetTickCount64")) : nullptr;
    passed &= Check(tick && tick() != 0, "the hooked GetProcAddress still resolves exports");
    return passed ? 0 : 1;
}
