#pragma once
// sdkscan.1: the process's loaded modules for the XeSS discoveries. psapi's list is read at whatever size it reports
// (a fixed buffer missed modules), and Toolhelp's list is added on request or when psapi fails. Toolhelp takes the
// loader lock for its snapshot: never from a thread that holds it, nor while holding a lock that a loader-lock holder
// could wait for.
#include <windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <vector>

namespace xess_mod {
struct ModuleListStats
{
    size_t psapi = 0;           // modules psapi listed (0 when it failed)
    size_t toolhelp = 0;        // modules Toolhelp listed (0 when it failed or didn't run)
    DWORD psapiError = 0;       // GetLastError of the failed psapi call (ERROR_INSUFFICIENT_BUFFER: the list kept growing)
    DWORD toolhelpError = 0;    // GetLastError of the failed snapshot
    bool toolhelpRan = false;
};

enum class ToolhelpList
{
    eFallback,   // only when psapi fails
    eAlways,     // beside psapi's list
};

// Fills `modules` (deduplicated, psapi's order first). Returns false only when no list could be read.
inline bool LoadedModules(std::vector<HMODULE>& modules, ModuleListStats& stats, ToolhelpList toolhelp) noexcept
{
    modules.clear();
    stats = {};
    try {
        std::vector<HMODULE> buffer(1024);
        bool psapiListed = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            DWORD bytes = 0;
            if (!EnumProcessModulesEx(GetCurrentProcess(), buffer.data(), static_cast<DWORD>(buffer.size()*sizeof(HMODULE)),
                    &bytes, LIST_MODULES_64BIT)) {
                stats.psapiError = GetLastError();
                break;
            }
            const size_t needed = bytes / sizeof(HMODULE);
            if (needed <= buffer.size()) {
                buffer.resize(needed);
                psapiListed = true;
                break;
            }
            buffer.resize(needed + 64);   // loads between the calls grow the list
            stats.psapiError = ERROR_INSUFFICIENT_BUFFER;
        }
        if (psapiListed) {
            stats.psapiError = 0;
            stats.psapi = buffer.size();
            for (HMODULE module : buffer)
                if (module && std::find(modules.begin(), modules.end(), module) == modules.end()) modules.push_back(module);
        }
        if (toolhelp == ToolhelpList::eAlways || !psapiListed) {
            stats.toolhelpRan = true;
            HANDLE snapshot = INVALID_HANDLE_VALUE;
            // ERROR_BAD_LENGTH: the module list changed during the snapshot; Microsoft says to retry.
            for (int attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
                snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
                if (snapshot == INVALID_HANDLE_VALUE) {
                    stats.toolhelpError = GetLastError();
                    if (stats.toolhelpError != ERROR_BAD_LENGTH) break;
                }
            }
            if (snapshot != INVALID_HANDLE_VALUE) {
                stats.toolhelpError = 0;
                MODULEENTRY32W entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
                    ++stats.toolhelp;
                    const auto module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
                    if (module && std::find(modules.begin(), modules.end(), module) == modules.end()) modules.push_back(module);
                    entry.dwSize = sizeof(entry);
                }
                CloseHandle(snapshot);
            }
        }
    } catch (...) {
        return !modules.empty();
    }
    return stats.psapi != 0 || stats.toolhelp != 0;
}
}
