// Gears of War E-Day model: a Themida-packed executable whose import descriptors
// keep only FirstThunk (OriginalFirstThunk = 0). zero_import_names.py rewrites
// this executable that way after linking. RTXMFG loads beside it as the
// statically imported version.dll; a DllMain that reads the bound FirstThunk
// as name RVAs fails process startup with 0xc0000142.
#include "single_module_status.h"
#include <Windows.h>
#include <cstdio>
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
}

int wmain()
{
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t descriptors = 0;
    bool withoutNames = directory.VirtualAddress != 0;
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         withoutNames && descriptor->Name; ++descriptor, ++descriptors)
        withoutNames = descriptor->OriginalFirstThunk == 0;
    bool passed = Check(withoutNames && descriptors >= 2,
        "executable imports keep only FirstThunk (Themida layout)");

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

    wchar_t systemPath[32768]{};
    GetSystemDirectoryW(systemPath, 32768);
    wcscat_s(systemPath, L"\\version.dll");
    HMODULE system = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Size = DWORD (WINAPI*)(LPCWSTR, LPDWORD);
    const auto nativeSize = system ? reinterpret_cast<Size>(GetProcAddress(system, "GetFileVersionInfoSizeW")) : nullptr;
    DWORD ignored = 0;
    const DWORD forwarded = GetFileVersionInfoSizeW(proxyPath.c_str(), &ignored);
    passed &= Check(nativeSize && forwarded && forwarded == nativeSize(proxyPath.c_str(), &ignored),
        "static version import forwards to Windows");
    if (system) FreeLibrary(system);

    // Without a name table the GetProcAddress slot cannot be identified, so
    // the main-executable resolver discovery fails closed and the slot keeps
    // the loader's binding. Taking a dllimport address reads the slot.
    const void* bound = reinterpret_cast<const void*>(&GetProcAddress);
    const HMODULE owner = Owner(bound);
    passed &= Check(owner && owner != proxy && owner != GetModuleHandleW(nullptr),
        "main-executable GetProcAddress import left to the loader");

    // Keep the process alive through the patch worker's startup pass on this
    // layout; it publishes the status file beside the module afterwards.
    const auto statusPath = proxyPath.parent_path() / L"RTXMFG-Universal.status.json";
    const std::string key = "\"mainResolverDiscoveryInstalled\":";
    std::string statusText;
    for (int attempt = 0; attempt < 150 && statusText.find(key) == std::string::npos; ++attempt)
    {
        Sleep(100);
        std::ifstream file(statusPath, std::ios::binary);
        statusText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    passed &= Check(statusText.find(key + "false") != std::string::npos,
        "patch worker completes startup; main-executable resolver discovery fails closed");
    return passed ? 0 : 1;
}
