#include "single_module.h"
#include "build_variant.h"
#include "overlay_build.h"
#include "overlay_install.h"
#include "single_overlay.h"
#include "overlay_dxgi_proxy.h"
#include "optiscaler_host.h"
#include "optiscaler_menu_tab.h"
#include "host_process_policy.h"
#include "session_diagnostics.h"
#include "proxy_generated/proxy_export_names.h"
#include "proxy_generated/renamable_names.h"
#include "xess/xess_mod.h"
#include "xess/ngx_provider_registration.h"
#include "xess/cuda/cuda_route.h"
#include "embedded_payload.h"

#include <delayimp.h>
#include <intrin.h>
#include <atomic>
#include <array>
#include <cwchar>
#include <cstring>

namespace
{
HMODULE gSelf = nullptr;
uint32_t gSelectedProxyFamily = UINT32_MAX;
HANDLE gLifetimeClaim = nullptr;
bool gOwnsBackend = false;
bool gDuplicate = false;
std::array<std::atomic<HMODULE>, kMfgProxyFamilyCount> gOriginalModules{};
std::atomic<FARPROC> gForwardTargets[kMfgProxyFamilyCount][kMfgProxyOrdinalLimit]{};
thread_local uint32_t gLoadingFamilies = 0;
thread_local uint32_t gCurrentLoadingFamily = UINT32_MAX;
thread_local const wchar_t* gLoadingPaths[kMfgProxyFamilyCount]{};
static_assert(kMfgProxyFamilyCount <= 32);

constexpr size_t kLongPathCapacity = 32768;

class LongPath
{
public:
    LongPath() noexcept
        : value_(static_cast<wchar_t*>(VirtualAlloc(nullptr,
            kLongPathCapacity * sizeof(wchar_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
    {}
    ~LongPath() noexcept
    {
        if (value_) VirtualFree(value_, 0, MEM_RELEASE);
    }
    LongPath(const LongPath&) = delete;
    LongPath& operator=(const LongPath&) = delete;
    explicit operator bool() const noexcept { return value_ != nullptr; }
    wchar_t* get() const noexcept { return value_; }

private:
    wchar_t* value_ = nullptr;
};

class ScopedFamilyLoad
{
public:
    explicit ScopedFamilyLoad(uint32_t family) noexcept
        : bit_(1u << family), previous_(gCurrentLoadingFamily)
    {
        gLoadingFamilies |= bit_;
        gCurrentLoadingFamily = family;
    }
    ~ScopedFamilyLoad() noexcept
    {
        gCurrentLoadingFamily = previous_;
        gLoadingFamilies &= ~bit_;
    }
    ScopedFamilyLoad(const ScopedFamilyLoad&) = delete;
    ScopedFamilyLoad& operator=(const ScopedFamilyLoad&) = delete;

private:
    uint32_t bit_;
    uint32_t previous_;
};

class ScopedLoadingPath
{
public:
    explicit ScopedLoadingPath(const wchar_t* path) noexcept
        : family_(gCurrentLoadingFamily)
    {
        if (family_ < kMfgProxyFamilyCount)
        {
            previous_ = gLoadingPaths[family_];
            gLoadingPaths[family_] = path;
        }
    }
    ~ScopedLoadingPath() noexcept
    {
        if (family_ < kMfgProxyFamilyCount)
            gLoadingPaths[family_] = previous_;
    }
    ScopedLoadingPath(const ScopedLoadingPath&) = delete;
    ScopedLoadingPath& operator=(const ScopedLoadingPath&) = delete;

private:
    uint32_t family_;
    const wchar_t* previous_ = nullptr;
};

constexpr const wchar_t* kLegacyCoreNames[] = {
    L"RTXMFGCore.dll", L"RTX40MFGCore.dll", L"RTX30MFGCore.dll",
    L"RTX30FGCore.dll", L"RTX30FG3xCore.dll"
};

bool LegacyInstallConflict(const wchar_t* modulePath) noexcept
{
    LongPath scratch;
    if (!scratch) return true;
    wchar_t* const path = scratch.get();
    if (wcscpy_s(path, kLongPathCapacity, modulePath)) return true;
    wchar_t* leaf = wcsrchr(path, L'\\');
    if (!leaf) return true;
    const size_t rootLength = static_cast<size_t>(leaf + 1 - path);

    // v1.2 does not participate in the process claim. A loaded-module check
    // alone therefore permits a later ASI import to activate a second backend.
    // Check only the documented installation roots and conventional ASI
    // subdirectories, before any core or overlay entry. Do not load, inspect
    // exports from, rename, or delete a legacy DLL to decide this policy.
    for (const wchar_t* directory : {L"", L"plugins\\", L"scripts\\", L"asi\\"})
    {
        for (const wchar_t* name : kLegacyCoreNames)
        {
            path[rootLength] = L'\0';
            if (wcscat_s(path, kLongPathCapacity, directory)
                || wcscat_s(path, kLongPathCapacity, name)) return true;
            const DWORD attributes = GetFileAttributesW(path);
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            if ((attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                || (attributes == INVALID_FILE_ATTRIBUTES
                    && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND))
            {
                single_module::Log(L"Mixed or unreadable legacy installation: new backend and menu disabled. "
                    L"Close the game and move the old core/ASI/UI files out of its loader directories.");
                single_module::Log(path);
                return true;
            }
        }
    }
    return false;
}

bool HasLegacyConflict(HMODULE instance, const wchar_t* modulePath) noexcept
{
    for (const wchar_t* name : kLegacyCoreNames)
    {
        HMODULE legacy = GetModuleHandleW(name);
        if (legacy && legacy != instance && GetProcAddress(legacy, "MfgUnlockCoreLoaded"))
        {
            single_module::Log(L"Loaded legacy core retained; new backend and menu disabled.");
            single_module::Log(name);
            return true;
        }
    }
    if (LegacyInstallConflict(modulePath)) return true;

    // An explicitly loaded neutral DLL or ASI can live below the executable.
    // Also cover the executable's normal v1.2 installation and ASI directories.
    LongPath executableScratch;
    if (!executableScratch)
    {
        single_module::Log(L"Executable installation directory allocation failed; new backend and menu disabled.");
        return true;
    }
    wchar_t* const executablePath = executableScratch.get();
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
        static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity)
    {
        single_module::Log(L"Executable installation directory unavailable; new backend and menu disabled.");
        return true;
    }
    const wchar_t* executableLeaf = wcsrchr(executablePath, L'\\');
    const wchar_t* moduleLeaf = wcsrchr(modulePath, L'\\');
    if (executableLeaf && moduleLeaf && executableLeaf - executablePath == moduleLeaf - modulePath
        && _wcsnicmp(executablePath, modulePath, executableLeaf - executablePath) == 0)
        return false;
    return LegacyInstallConflict(executablePath);
}

// A game never runs from the Windows folder. An executable there is a Windows
// program that loaded this DLL itself (rundll32, regsvr32, a scanner's
// sandbox; GitHub issue 111: logs, status file and write probes landed in
// System32). Such a host gets proxy forwarding only. Unknown paths count as
// Windows-hosted.
bool HostedFromWindowsDirectory() noexcept
{
    wchar_t windows[MAX_PATH + 1]{};
    size_t windowsLength = GetSystemWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
    if (!windowsLength || windowsLength >= std::size(windows)) return true;
    while (windowsLength && windows[windowsLength - 1] == L'\\') --windowsLength;
    LongPath scratch;
    if (!scratch) return true;
    const DWORD length = GetModuleFileNameW(nullptr, scratch.get(), static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity) return true;
    const wchar_t* executable = scratch.get();
    if (wcsncmp(executable, L"\\\\?\\", 4) == 0) executable += 4;
    return wcslen(executable) > windowsLength && executable[windowsLength] == L'\\'
        && _wcsnicmp(executable, windows, windowsLength) == 0;
}

// A crash reporter started from the game folder loads the same proxy (GitHub
// issue 116: UnityCrashHandler64.exe ran the backend, gave itself the GPU name
// override and overwrote the game's status file). It gets proxy forwarding only.
bool HostedByCrashReporter() noexcept
{
    static constexpr const wchar_t* kReporters[]{L"UnityCrashHandler64.exe", L"UnityCrashHandler32.exe",
        L"CrashReportClient.exe", L"crashpad_handler.exe", L"REDEngineErrorReporter.exe"};
    LongPath scratch;
    if (!scratch) return false;
    const DWORD length = GetModuleFileNameW(nullptr, scratch.get(), static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity) return false;
    const wchar_t* leaf = wcsrchr(scratch.get(), L'\\');
    leaf = leaf ? leaf + 1 : scratch.get();
    for (const wchar_t* reporter : kReporters)
        if (_wcsicmp(leaf, reporter) == 0) return true;
    return false;
}

// pluginprov.2: a Chromium-family helper (CEF, Electron) started with --type= (host_process_policy.h). A game may start
// its own executable as such helpers, and OptiScaler loads this module in each; the backend there can crash a renderer
// and writes extra logs and status files. Such a host gets proxy forwarding only.
bool HostedByChromiumHelper() noexcept
{
    const wchar_t* commandLine = GetCommandLineW();
    return commandLine && host_process_policy::HasChromiumHelperSwitch(commandLine);
}

bool SelectProxyExports(HMODULE module, uint32_t family) noexcept
{
    // Customize only this loaded copy's own export-address table before any
    // importer can call it. The file stays identical under every supported name.
    // Matching name/ordinal exports share one thunk, and unrelated family names
    // have a null RVA so GetProcAddress reports them as absent.
    const auto base = reinterpret_cast<BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const uint32_t size = nt->OptionalHeader.SizeOfImage;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress >= size || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)
        || directory.Size > size - directory.VirtualAddress) return false;
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    // Public exports are ordinals 4096.. (generate_build_inputs.py); 4100 = XeFGModelName, 4101 = OptiPluginInit.
    constexpr uint32_t kLegacyLimit = 427, kNamedBase = 1024, kPublicLast = 4101;
    if (exports->Base != 1 || exports->NumberOfFunctions != kPublicLast
        || exports->AddressOfFunctions >= size
        || exports->NumberOfFunctions > (size - exports->AddressOfFunctions) / sizeof(DWORD)
        || (exports->AddressOfFunctions & 3u)) return false;
    auto table = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    std::array<DWORD, kLegacyLimit> original{};
    for (uint32_t i = 0; i < kLegacyLimit; ++i)
    {
        const DWORD rva = table[i];
        if (!rva || rva >= size || (rva >= directory.VirtualAddress
            && rva - directory.VirtualAddress < directory.Size)) return false;
        original[i] = rva;
    }
    DWORD protection = 0;
    const SIZE_T bytes = exports->NumberOfFunctions * sizeof(DWORD);
    if (!VirtualProtect(table, bytes, PAGE_READWRITE, &protection)) return false;
    for (uint32_t ordinal = 1; ordinal <= kLegacyLimit; ++ordinal)
        table[ordinal - 1] = MfgProxyKnownOrdinal(family, ordinal) ? original[ordinal - 1] : 0;
    for (uint32_t index = 0; index < kMfgRenamedNameCount; ++index)
    {
        const uint32_t ordinal = family < kMfgProxyFamilyCount ? kMfgRenamedOrdinals[family][index] : 0;
        table[kNamedBase + index - 1] = ordinal && ordinal <= kLegacyLimit ? original[ordinal - 1] : 0;
    }
    bool verified = true;
    for (uint32_t ordinal = 1; ordinal <= kLegacyLimit; ++ordinal)
        verified &= table[ordinal - 1] == (MfgProxyKnownOrdinal(family, ordinal) ? original[ordinal - 1] : 0);
    for (uint32_t index = 0; index < kMfgRenamedNameCount; ++index)
    {
        const uint32_t ordinal = family < kMfgProxyFamilyCount ? kMfgRenamedOrdinals[family][index] : 0;
        verified &= table[kNamedBase + index - 1] == (ordinal && ordinal <= kLegacyLimit ? original[ordinal - 1] : 0);
    }
    DWORD ignored = 0;
    const bool restored = VirtualProtect(table, bytes, protection, &ignored) != FALSE;
    MEMORY_BASIC_INFORMATION memory{};
    return verified && restored && VirtualQuery(table, &memory, sizeof(memory))
        && memory.AllocationBase == module && memory.Protect == protection;
}

HMODULE LoadChainedOriginal(const wchar_t* path) noexcept
{
    ScopedLoadingPath loadingPath(path);
    HMODULE module = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        return nullptr;
    LongPath loadedScratch;
    if (!loadedScratch)
    {
        FreeLibrary(module);
        return nullptr;
    }
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded, static_cast<DWORD>(kLongPathCapacity));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    // The OS loader has already validated the PE image. Reject another copy of
    // this payload so a mistakenly renamed proxy cannot form a forwarding loop.
    if (module == gSelf || !length || length >= kLongPathCapacity
        || _wcsicmp(path, loaded) != 0 || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || GetProcAddress(module, "MfgUnlockSingleModuleQuery"))
    {
        FreeLibrary(module);
        return nullptr;
    }
    HMODULE pinned = nullptr;
    const bool valid = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned)
        && pinned == module;
    FreeLibrary(module);
    return valid ? module : nullptr;
}

HMODULE LoadProxyOriginal(uint32_t family) noexcept
{
    if (family >= kMfgProxyFamilyCount)
        return nullptr;
    HMODULE module = gOriginalModules[family].load(std::memory_order_acquire);
    if (module)
        return module;
    const auto& spec = kMfgProxyFamilies[family];
    LongPath pathScratch;
    if (!pathScratch)
        return nullptr;
    wchar_t* const path = pathScratch.get();
    const DWORD length = GetModuleFileNameW(gSelf, path, static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity)
        return nullptr;
    wchar_t* leaf = wcsrchr(path, L'\\');
    if (!leaf || wcscpy_s(leaf + 1, kLongPathCapacity - (leaf + 1 - path), spec.chained))
        return nullptr;
    const DWORD attributes = GetFileAttributesW(path);
    HMODULE candidate = nullptr;
    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        // A present but invalid original is an error, never a silent fallback.
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
            candidate = LoadChainedOriginal(path);
    }
    else if (const DWORD error = GetLastError();
        spec.systemFallback && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
    {
        candidate = single_module::LoadSystemModule(spec.original);
    }
    if (!candidate)
        return nullptr;
    HMODULE expected = nullptr;
    if (gOriginalModules[family].compare_exchange_strong(expected, candidate,
        std::memory_order_release, std::memory_order_acquire))
        return candidate;
    return expected;
}

bool PinCallable(FARPROC target) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!target || !VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory))
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase == gSelf
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD access = memory.Protect & 0xff;
    if (access != PAGE_EXECUTE && access != PAGE_EXECUTE_READ
        && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
        return false;
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target), &owner) && owner == memory.AllocationBase;
}

FARPROC ResolveRecursiveTarget(uint32_t family, uint32_t ordinal) noexcept
{
    if (family >= kMfgProxyFamilyCount || !gLoadingPaths[family])
        return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        gLoadingPaths[family], &module) || !module || module == gSelf)
        return nullptr;
    LongPath loadedScratch;
    if (!loadedScratch)
        return nullptr;
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded, static_cast<DWORD>(kLongPathCapacity));
    const auto base = reinterpret_cast<const BYTE*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!length || length >= kLongPathCapacity
        || _wcsicmp(gLoadingPaths[family], loaded) != 0
        || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || GetProcAddress(module, "MfgUnlockSingleModuleQuery"))
        return nullptr;
    const char* name = MfgProxyExportName(family, ordinal);
    FARPROC target = GetProcAddress(module, name ? name : MAKEINTRESOURCEA(ordinal));
    MEMORY_BASIC_INFORMATION memory{};
    if (!target || !VirtualQuery(reinterpret_cast<const void*>(target), &memory, sizeof(memory))
        || memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != module
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return nullptr;
    const DWORD access = memory.Protect & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ
        || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY ? target : nullptr;
}

FARPROC WINAPI DelayImportHook(unsigned event, PDelayLoadInfo info)
{
    if (event != dliNotePreLoadLibrary || !info || !info->szDll)
        return nullptr;
    // The version proxy must never import itself to read a provider version.
    // The documented delay-load notification supplies the explicit System32
    // image for these ordinary Windows APIs in every filename variant.
    if (_stricmp(info->szDll, "version.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"version.dll"));
    if (_stricmp(info->szDll, "d3dcompiler_47.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"d3dcompiler_47.dll"));
    if (_stricmp(info->szDll, "dxgi.dll") == 0)
        return reinterpret_cast<FARPROC>(single_module::LoadSystemModule(L"dxgi.dll"));
    return nullptr;
}

}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = &DelayImportHook;

namespace single_module
{
HMODULE Self() noexcept { return gSelf; }
bool OwnsBackend() noexcept { return gOwnsBackend; }
// OptiScaler mode hides every RTXMFG surface: OptiScaler's own menu owns the
// controls. Already-wrapped objects stay in the chain but stop drawing and
// capturing input.
bool OverlayAllowed() noexcept { return gOwnsBackend && !optiscaler_host::UiHidden(); }

HMODULE LoadSystemModule(const wchar_t* basename) noexcept
{
    if (!basename || wcschr(basename, L'\\') || wcschr(basename, L'/'))
        return nullptr;
    LongPath pathScratch;
    if (!pathScratch)
        return nullptr;
    wchar_t* const path = pathScratch.get();
    const UINT count = GetSystemDirectoryW(path, static_cast<UINT>(kLongPathCapacity));
    if (!count || count >= kLongPathCapacity - 256)
        return nullptr;
    if (wcscat_s(path, kLongPathCapacity, L"\\")
        || wcscat_s(path, kLongPathCapacity, basename))
        return nullptr;
    ScopedLoadingPath loadingPath(path);
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module || module == gSelf)
    {
        if (module) FreeLibrary(module);
        return nullptr;
    }
    LongPath loadedScratch;
    if (!loadedScratch)
    {
        FreeLibrary(module);
        return nullptr;
    }
    wchar_t* const loaded = loadedScratch.get();
    const DWORD length = GetModuleFileNameW(module, loaded,
        static_cast<DWORD>(kLongPathCapacity));
    if (!length || length >= kLongPathCapacity || _wcsicmp(path, loaded) != 0)
    {
        FreeLibrary(module);
        return nullptr;
    }
    // Original entry pointers can be cached by the game for its whole life.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module)
    {
        FreeLibrary(module);
        return nullptr;
    }
    FreeLibrary(module); // The permanent pin owns the lifetime now.
    return module;
}

namespace {
// A ring the existing backend worker drains every 100 ms into the log file
// (lines from before the file opens, and from Witcher DOTS). A full ring drops
// the newest lines and the drain reports how many.
constexpr size_t kUiLogChars = 768;
struct UiLogLine { wchar_t text[kUiLogChars]{}; };
UiLogLine gUiLog[192];
SRWLOCK gUiLogLock = SRWLOCK_INIT;
uint32_t gUiLogHead = 0, gUiLogCount = 0, gUiLogDropped = 0; // gUiLogLock
}
void DrainLog(void (*sink)(const wchar_t*)) noexcept
{
    for (;;)
    {
        wchar_t line[kUiLogChars]{};
        uint32_t dropped = 0;
        AcquireSRWLockExclusive(&gUiLogLock);
        const bool have = gUiLogCount != 0;
        if (have)
        {
            wcscpy_s(line, gUiLog[gUiLogHead].text);
            gUiLogHead = (gUiLogHead + 1) % std::size(gUiLog);
            --gUiLogCount;
        }
        else
        {
            dropped = gUiLogDropped;
            gUiLogDropped = 0;
        }
        ReleaseSRWLockExclusive(&gUiLogLock); // The sink runs without the lock.
        if (!have)
        {
            if (dropped)
            {
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
                wchar_t stamp[session_diagnostics::kStampChars];
                session_diagnostics::Stamp(stamp);
                swprintf_s(line, L"%s%u log lines dropped (log queue full)", stamp, dropped);
#else
                swprintf_s(line, L"%u log lines dropped (log queue full)", dropped);
#endif
                sink(line);
            }
            return;
        }
        sink(line);
    }
}
// The crash and exit records flush the queue without waiting; a lock held by a stopped or crashed thread
// leaves the rest queued.
bool TryDrainLog(void (*sink)(const wchar_t*)) noexcept
{
    for (size_t taken = 0; taken < std::size(gUiLog); ++taken)
    {
        wchar_t line[kUiLogChars];
        if (!TryAcquireSRWLockExclusive(&gUiLogLock)) return false;
        const bool have = gUiLogCount != 0;
        if (have)
        {
            wcscpy_s(line, gUiLog[gUiLogHead].text);
            gUiLogHead = (gUiLogHead + 1) % std::size(gUiLog);
            --gUiLogCount;
        }
        ReleaseSRWLockExclusive(&gUiLogLock);
        if (!have) return true;
        sink(line);
    }
    return true;
}
void Log(const wchar_t* text) noexcept
{
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    // Stamped when queued: the worker writes the queue up to 100 ms later.
    wchar_t stamp[session_diagnostics::kStampChars];
    session_diagnostics::Stamp(stamp);
#endif
    AcquireSRWLockExclusive(&gUiLogLock);
    if (gUiLogCount < std::size(gUiLog))
    {
        const auto index = (gUiLogHead + gUiLogCount) % std::size(gUiLog);
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
        swprintf_s(gUiLog[index].text, L"%s%.*s", stamp, static_cast<int>(kUiLogChars - session_diagnostics::kStampChars),
            text ? text : L"(null)");
#else
        wcsncpy_s(gUiLog[index].text, text ? text : L"(null)", _TRUNCATE);
#endif
        ++gUiLogCount;
    }
    else
        ++gUiLogDropped;
    ReleaseSRWLockExclusive(&gUiLogLock);
    OutputDebugStringW(L"[" MFG_PRODUCT_W L" single] ");
    OutputDebugStringW(text ? text : L"(null)");
    OutputDebugStringW(L"\n");
}
}

extern "C" FARPROC WINAPI MfgProxyResolve(uint32_t family, uint32_t ordinal) noexcept
{
    const DWORD savedError = GetLastError();
    if (family < kMfgProxyFamilyCount && ordinal < kMfgProxyOrdinalLimit)
    {
        FARPROC target = gForwardTargets[family][ordinal].load(std::memory_order_acquire);
        const uint32_t bit = 1u << family;
        if (!target && (gLoadingFamilies & bit))
            target = ResolveRecursiveTarget(family, ordinal);
        if (!target && !(gLoadingFamilies & bit))
        {
            // Never hold a proxy-owned gate around LoadLibraryExW. Independent
            // attempts may converge on the same pinned module and callable;
            // a caller already holding the loader lock can always progress.
            ScopedFamilyLoad loading(family);
            if (HMODULE module = LoadProxyOriginal(family))
            {
                const char* name = MfgProxyExportName(family, ordinal);
                target = GetProcAddress(module, name ? name : MAKEINTRESOURCEA(ordinal));
                if (!PinCallable(target))
                    target = nullptr;
                if (target)
                {
                    FARPROC expected = nullptr;
                    if (!gForwardTargets[family][ordinal].compare_exchange_strong(expected, target,
                        std::memory_order_release, std::memory_order_acquire))
                        target = expected;
                }
            }
        }
        if (target)
        {
            SetLastError(savedError);
            return target;
        }
        wchar_t message[512]{};
        swprintf_s(message, L"Cannot forward %s export #%u. Check the original %s beside this loader%s.",
            kMfgProxyFamilies[family].original, ordinal, kMfgProxyFamilies[family].chained,
            kMfgProxyFamilies[family].systemFallback ? L" or the Windows System32 DLL" : L" (required)");
        single_module::Log(message);
    }
    const ULONG_PTR detail[] = { family, ordinal };
    RaiseException(0xC0000139u, EXCEPTION_NONCONTINUABLE, 2, detail);
    return nullptr;
}

extern "C" FARPROC WINAPI MfgProxyResolveRenamed(uint32_t kind, uint32_t key) noexcept
{
    const uint32_t family = gSelectedProxyFamily;
    uint32_t ordinal = 0;
    if (family < kMfgProxyFamilyCount)
    {
        if (kind == 0 && MfgProxyKnownOrdinal(family, key)) ordinal = key;
        else if (kind == 1 && key < kMfgRenamedNameCount) ordinal = kMfgRenamedOrdinals[family][key];
    }
    // Unsupported names/ordinals never resolve against a different DLL family.
    return MfgProxyResolve(ordinal ? family : UINT32_MAX, ordinal);
}

extern "C" __declspec(dllexport) BOOL WINAPI
MfgUnlockSingleModuleQuery(MfgSingleModuleStatus* status)
{
    // Version-1 consumers pass the 64-byte layout and receive populated
    // version-1 fields; version-2 consumers also receive the bounded
    // coordinator state. Any other size is rejected as before.
    if (!status || (status->size != sizeof(*status)
            && status->size != kMfgSingleModuleStatusSizeV1
            && status->size != kMfgSingleModuleStatusSizeV2))
        return FALSE;
    const uint32_t requested = status->size;
    MfgSingleModuleStatus value{};
    value.size = requested;
    value.version = requested == kMfgSingleModuleStatusSizeV2 ? 2 : 3;
    value.backendOwner = gOwnsBackend;
    value.duplicateSuppressed = gDuplicate;
    single_overlay::ReadStatus(value);
    single_overlay::proxy::ReadStatus(value);
    value.overlayInstallState = static_cast<uint32_t>(single_overlay::install::Current());
    value.overlayInstallReason = static_cast<uint32_t>(single_overlay::install::UnavailableReason());
    value.overlayActivatedTargets = single_overlay::install::ActivatedTargetCount();
    value.overlaySuspendedThreads = single_overlay::install::SuspendedThreadCount();
    memcpy(status, &value, requested);
    if (requested == kMfgSingleModuleStatusSizeV1)
        status->version = 1;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
#if MFG_UNLOCK_SESSION_DIAGNOSTICS
    // The module is pinned, so this is the process exit (reserved is non-null) recorded as the session end.
    if (reason == DLL_PROCESS_DETACH && gOwnsBackend)
        session_diagnostics::OnProcessDetach(reserved != nullptr);
#endif
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    gSelf = instance;
    // Process attach is serialized by the loader. Keep this buffer off the
    // stack: even ignored thread notifications otherwise reserve its 64 KiB
    // before the reason check, overflowing small driver-thread stacks.
    static wchar_t modulePath[32768]{};
    const DWORD pathLength = GetModuleFileNameW(instance, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (!pathLength || pathLength >= std::size(modulePath)) return FALSE;
    const wchar_t* basename = wcsrchr(modulePath, L'\\');
    basename = basename ? basename + 1 : modulePath;
    for (uint32_t i = 0; i < kMfgProxyFamilyCount; ++i)
        if (_wcsicmp(basename, kMfgProxyFamilies[i].original) == 0)
        {
            gSelectedProxyFamily = i;
            break;
        }
    if (!SelectProxyExports(instance, gSelectedProxyFamily)) return FALSE;
    DisableThreadLibraryCalls(instance);
    // No claim, hooks, worker, log, status file or write probe.
    if (HostedFromWindowsDirectory() || HostedByCrashReporter() || HostedByChromiumHelper()) return TRUE;

    wchar_t claim[96]{};
    swprintf_s(claim, L"Local\\RTX40MFG-SingleModule-%lu", GetCurrentProcessId());
    gLifetimeClaim = CreateMutexW(nullptr, FALSE, claim);
    if (!gLifetimeClaim)
        return FALSE;
    gDuplicate = GetLastError() == ERROR_ALREADY_EXISTS;
    // Forwarding remains available after suppression. Claim-aware integrated
    // copies honor the mutex; unmodified split cores require the install guard.
    if (!gDuplicate) gDuplicate = HasLegacyConflict(instance, modulePath);

    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(instance), &pinned) || pinned != instance)
    {
        CloseHandle(gLifetimeClaim);
        gLifetimeClaim = nullptr;
        return FALSE;
    }
    if (gDuplicate)
    {
        single_module::Log(L"Backend/menu initialization suppressed by the process claim or legacy installation guard; proxy forwarding remains active.");
        return TRUE;
    }
    gOwnsBackend = true;
    // slim.1: a packed payload group that fails to decode is logged once, then its route stays off.
    embedded_payload::SetLog(&single_module::Log);
    // Decide OptiScaler cooperation before any hook is armed. As a plugin,
    // RTXMFG is loaded from OptiScaler's DllMain, so its image is mapped here
    // (not necessarily initialized: only its resources are read).
    optiscaler_host::ObserveProcessStart(instance);
    if (optiscaler_host::Detected())
    {
        single_module::Log(L"OptiScaler detected at startup; its menu owns the frame-generation controls unless optiScalerUi is \"rtxmfg\" (without the key, RTXMFG's tab in OptiScaler's menu offers the DLSS presets and DLSS 5 once OptiScaler hands its plugin API, else RTXMFG's menu while one runs; \"shared\" keeps RTXMFG's menu, \"optiscaler\" only the tab).");
        if (optiscaler_host::DefaultUiMode() == optiscaler_host::UiMode::Hidden)
            single_module::Log(L"ForzaTech executable: RTXMFG's menu stays hidden beside OptiScaler (no factory, swapchain or input wrapping) unless optiScalerUi is \"shared\".");
    }
    if (!MfgUnlockCoreEntry(instance, reason, reserved))
    {
        gOwnsBackend = false;
        return FALSE;
    }
    // Bounded application import publication only; no UI discovery worker.
    // In OptiScaler mode the menu routes stay unbound inside ResolveProc; the
    // Witcher DOTS device import is still published here.
    single_overlay::InstallKnownModules();
    return TRUE;
}

// OptiScaler ASI plugin contract (loaded from its plugins folder, called from
// its DllMain under the loader lock). Backend setup already ran in DllMain;
// this only records the caller identity.
extern "C" __declspec(dllexport) __declspec(noinline) void InitializeASI()
{
    if (gDuplicate || !gOwnsBackend) return;
    HMODULE caller = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &caller))
        caller = nullptr;
    single_module::Log(optiscaler_host::AcceptPluginCaller(caller)
        ? L"OptiScaler plugin host accepted (InitializeASI)."
        : L"InitializeASI caller is not OptiScaler; ignored.");
}

// menutab.2: OptiScaler's plugin API (optiscaler_plugin.h). OptiScaler calls it right after InitializeASI, from
// its DllMain under the loader lock; optiscaler_menu_tab keeps the API only from the identified OptiScaler image
// and registers RTXMFG's tab in its menu while the UI mode is hidden.
extern "C" __declspec(dllexport) __declspec(noinline) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin)
{
    if (gDuplicate || !gOwnsBackend) return false;
    const bool accepted = optiscaler_menu_tab::AcceptApi(api, plugin);
    single_module::Log(accepted ? L"OptiScaler plugin API accepted (OptiPluginInit)."
        : L"OptiPluginInit refused: the API table is not the identified OptiScaler's; ignored.");
    if (accepted)
        single_module::Log(optiscaler_menu_tab::Fork2Layout()
            ? L"OptiScaler plugin API layout: MFG Unlocker fork version 2 (before the upstream merge), adapted."
            : L"OptiScaler plugin API layout: upstream.");
    // pluginprov.1: registering only stores the entry, so it's fine under the loader lock. It lists the provider in
    // OptiScaler's menu; nothing runs unless FGNvngxReplacement selects it.
    if (accepted && optiscaler_menu_tab::RegisterProvider(ngx_provider_registration::kName, OptiProvider_Dlssg,
        &ngx_provider_registration::GetProc, nullptr))
    {
        ngx_provider_registration::NoteRegistered();
        single_module::Log(L"OptiScaler plugin API: XeMFG registered as the FG Nvngx provider (no shim file needed).");
    }
    return accepted;
}

// Optional ASI plugin export OptiScaler's XeFG card shows as "XeFG Model" (OptiScaler fork).
// "2X": XeFG's own 2x model (XeFGModel = default; OptiScaler's stock label for it); "port": our ported MFG model;
// "CUDA-port": the ported model with its int8 layers run on tensor cores. Empty until XeFG has been created
// (OptiScaler then shows its stock label). Called from OptiScaler's menu every frame it is open: reads published
// state only.
extern "C" __declspec(dllexport) __declspec(noinline) const char* XeFGModelName()
{
    if (gDuplicate || !gOwnsBackend) return "";
    const auto snapshot = xess_mod::ReadSnapshot();
    const uint32_t model = snapshot ? snapshot->modelActive : 0;
    if (model == 2) return "2X";
    if (model == 1) return xefg_cuda::Launched() ? "CUDA-port" : "port";
    return "";
}
