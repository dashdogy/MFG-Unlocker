#include "single_module_status.h"
#include "windows_smoke.h"
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mmsystem.h>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>

namespace
{
bool Check(bool passed, const char* name)
{
    printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    fflush(stdout);
    return passed;
}
HMODULE System(const wchar_t* name)
{
    wchar_t path[32768]{};
    GetSystemDirectoryW(path, 32768);
    wcscat_s(path, L"\\"); wcscat_s(path, name);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
template<class Fn> Fn Proc(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}
std::vector<std::wstring> RuntimeFiles(const std::filesystem::path& directory)
{
    std::vector<std::wstring> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        if (_wcsnicmp(entry.path().filename().c_str(), L"RTXMFG", 6) == 0)
            names.push_back(entry.path().filename().wstring());
    return names;
}
std::filesystem::path TempDirectory()
{
    wchar_t temp[MAX_PATH + 1]{};
    return GetTempPathW(MAX_PATH + 1, temp) ? std::filesystem::path(temp) : std::filesystem::path{};
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 3) return 2;
    const std::filesystem::path path = std::filesystem::absolute(argv[2]);
    // run.py starts this harness as UnityCrashHandler64.exe. pluginprov.2: or with --type=renderer, as BeamNG.drive
    // starts its CEF helpers.
    const bool crashReporter = std::wstring(argv[1]) == L"crash-reporter";
    const bool chromiumHelper = std::wstring(argv[1]) == L"chromium-helper";
    const auto tempBefore = RuntimeFiles(TempDirectory());
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    bool passed = Check(module != nullptr, "single file loads");
    if (!module) { printf("win32=%lu\n", GetLastError()); return 1; }
    const auto query = Proc<MfgSingleModuleQueryFn>(module, "MfgUnlockSingleModuleQuery");
    MfgSingleModuleStatus status{};
    status.size = sizeof(status);
    if (crashReporter)
        passed &= Check(query && query(&status) && status.version == 3
            && status.backendOwner == 0 && !status.duplicateSuppressed, "crash reporter host gets proxy forwarding only");
    else if (chromiumHelper)
        passed &= Check(query && query(&status) && status.version == 3
            && status.backendOwner == 0 && !status.duplicateSuppressed, "Chromium helper host gets proxy forwarding only");
    else
        passed &= Check(query && query(&status) && status.version == 3
            && status.backendOwner == 1 && !status.duplicateSuppressed, "one integrated backend owns initialization");
    for (const auto size : {kMfgSingleModuleStatusSizeV1,kMfgSingleModuleStatusSizeV2,uint32_t(sizeof(status))}) {
        alignas(MfgSingleModuleStatus) unsigned char bytes[sizeof(status)+16];
        memset(bytes,0xA5,sizeof(bytes));
        auto* legacy=reinterpret_cast<MfgSingleModuleStatus*>(bytes);legacy->size=size;
        const bool result=query&&query(legacy);
        bool intact=true;for(size_t i=size;i<sizeof(bytes);++i)intact&=bytes[i]==0xA5;
        passed &= Check(result&&legacy->version==(size==64?1u:size==80?2u:3u)&&intact,
            "status v1/v2/v3 negotiation preserves caller buffer boundary");
    }
    MfgSingleModuleStatus invalid{};
    invalid.size = sizeof(invalid)-1;
    passed &= Check(query && !query(&invalid), "status ABI rejects wrong size");
    passed &= Check(GetModuleHandleW(L"RTX40MFGCore.dll") == nullptr
        && GetModuleHandleW(L"RTX40MFG-UI.addon64") == nullptr, "no split core or UI dependency");
    auto name = path.filename().wstring();
    if (_wcsicmp(name.c_str(), L"version.dll") == 0)
    {
        HMODULE system = System(L"version.dll");
        using Size = DWORD (WINAPI*)(LPCWSTR, LPDWORD);
        using Read = BOOL (WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
        auto size = Proc<Size>(module, "GetFileVersionInfoSizeW");
        auto nativeSize = Proc<Size>(system, "GetFileVersionInfoSizeW");
        auto read = Proc<Read>(module, "GetFileVersionInfoExW");
        auto nativeRead = Proc<Read>(system, "GetFileVersionInfoExW");
        DWORD ignored = 0;
        DWORD bytes = size(path.c_str(), &ignored);
        passed &= Check(bytes && bytes == nativeSize(path.c_str(), &ignored), "version size forwards by name");
        passed &= Check(GetProcAddress(module, MAKEINTRESOURCEA(8)) == reinterpret_cast<FARPROC>(size), "version ordinal 8 matches named export");
        std::vector<unsigned char> actual(bytes+32, 0xA5), expected(bytes+32, 0xA5);
        const BOOL a = read(FILE_VER_GET_LOCALISED, path.c_str(), 0, bytes, actual.data()+16);
        const BOOL b = nativeRead(FILE_VER_GET_LOCALISED, path.c_str(), 0, bytes, expected.data()+16);
        passed &= Check(a && b && actual == expected, "five-argument version API preserves stack and output");
        FreeLibrary(system);
    }
    if (_wcsicmp(name.c_str(), L"dinput8.dll") == 0)
    {
        using Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto create = Proc<Create>(module, "DirectInput8Create");
        IDirectInput8W* input = nullptr;
        const HRESULT result = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
            IID_IDirectInput8W, reinterpret_cast<void**>(&input), nullptr);
        passed &= Check(SUCCEEDED(result) && input, "five-argument DirectInput8Create forwards");
        passed &= Check(GetProcAddress(module, MAKEINTRESOURCEA(1)) == reinterpret_cast<FARPROC>(create), "DirectInput ordinal 1 matches named export");
        if (input) input->Release();
    }
    if (_wcsicmp(name.c_str(), L"winmm.dll") == 0)
    {
        auto clock = Proc<DWORD (WINAPI*)()>(module, "timeGetTime");
        auto caps = Proc<MMRESULT (WINAPI*)(LPTIMECAPS, UINT)>(module, "timeGetDevCaps");
        TIMECAPS actual{};
        const DWORD first = clock();
        Sleep(2);
        passed &= Check(clock() >= first && caps(&actual, sizeof(actual)) == TIMERR_NOERROR
            && actual.wPeriodMin > 0 && actual.wPeriodMax >= actual.wPeriodMin, "WinMM timing APIs forward correctly");
        passed &= Check(GetProcAddress(module, MAKEINTRESOURCEA(2)) != nullptr, "WinMM ordinal-only export retained");
    }
    if (path.extension() == L".dll" && name.find(L"bink") != 0 && _wcsicmp(name.c_str(), L"MFGUnlocker.dll") != 0)
    {
        HMODULE system = System(name.c_str());
        passed &= Check(system && WindowsProxySmoke(module, system, name.c_str()),
            "Windows original API matches with the integrated backend active");
        if (system) FreeLibrary(system);
    }
    if (std::wstring(argv[1]) == L"loader")
    {
        const auto secondPath=path.parent_path()/(_wcsicmp(name.c_str(),L"version.dll")==0?L"winhttp.dll":L"version.dll");
        std::filesystem::copy_file(path,secondPath); HMODULE duplicate = LoadLibraryExW(secondPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto secondQuery = duplicate ? Proc<MfgSingleModuleQueryFn>(duplicate, "MfgUnlockSingleModuleQuery") : nullptr;
        MfgSingleModuleStatus second{};
        second.size = sizeof(second);
        passed &= Check(secondQuery && secondQuery(&second) && !second.backendOwner
            && second.duplicateSuppressed, "second variant suppresses duplicate initialization");
        passed &= Check(query(&status) && status.backendOwner, "first owner survives duplicate load");
        if (duplicate) FreeLibrary(duplicate);
    }
    if (crashReporter || chromiumHelper)
    {
        // A started backend writes its log and status file and probes the folder within this time.
        Sleep(2000);
        const auto beside = RuntimeFiles(path.parent_path());
        for (const auto& file : beside) printf("left %ls\n", file.c_str());
        passed &= Check(beside.empty() && RuntimeFiles(TempDirectory()) == tempBefore,
            crashReporter ? "crash reporter host writes no log, status file or write probe"
                          : "Chromium helper host writes no log, status file or write probe");
        // Not pinned without a backend: FreeLibrary would unmap the query export.
        return passed ? 0 : 1;
    }
    FreeLibrary(module);
    passed &= Check(query(&status) && status.backendOwner, "backend remains pinned after FreeLibrary");
    return passed ? 0 : 1;
}
