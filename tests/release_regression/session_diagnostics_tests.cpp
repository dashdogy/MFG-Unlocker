// The beta runtime log's stamps, environment header, first-chance and unhandled-exception
// records, minidump and session end. The parent never loads the DLL: it runs this executable twice as a child that
// does (one crashes, one calls ExitProcess) and reads each child's runtime log.
#include "diagnostic_paths.h"

#include <Windows.h>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    return text;
}

// This process's own session (the log may still hold the previous child's until the worker rotates it): its
// "Session: process <pid> " line, then `fragment` after it.
// The log a session that found the runtime log busy writes instead.
std::wstring BusyFallbackLog(DWORD pid)
{
    const std::wstring log = diagnostic_paths::RuntimeLog();
    return log.substr(0, log.size() - 4) + L"." + std::to_wstring(pid) + L".log";
}

bool WaitForOwnLog(const char* fragment, DWORD timeoutMs, const std::wstring& path)
{
    char session[64]{};
    sprintf_s(session, "Session: process %lu ", GetCurrentProcessId());
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline)
    {
        const std::string text = ReadShared(path);
        const size_t start = text.find(session);
        if (start != std::string::npos && text.find(fragment, start) != std::string::npos) return true;
        Sleep(50);
    }
    // What the child saw, for the parent's failure report.
    const std::string text = ReadShared(path);
    if (FILE* note = _wfopen(L"session-diagnostics-child.txt", L"a"))
    {
        fprintf(note, "pid %lu waited for '%s' after '%s' in %ls (%zu bytes): %.300s\n", GetCurrentProcessId(), fragment,
            session, path.c_str(), text.size(), text.c_str());
        fclose(note);
    }
    return false;
}

__declspec(noinline) void HandledFault()
{
    __try { *reinterpret_cast<volatile int*>(0x18) = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) void UnhandledFault()
{
    *reinterpret_cast<volatile int*>(0x10) = 7;
}

int Child(const std::wstring& mode, const wchar_t* dll)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Before the DLL loads: this executable and the DLL each probe the game folder with the same probe file name
    // (pid + a per-module sequence), so probing concurrently with the worker could send one of them to %TEMP%.
    const std::wstring log = mode == L"child-busy" ? BusyFallbackLog(GetCurrentProcessId()) : diagnostic_paths::RuntimeLog();
    if (!LoadLibraryW(dll)) return 20;
    if (!WaitForOwnLog("System: Windows ", 20000, log)) return 21;
    if (!WaitForOwnLog("State: route=", 20000, log)) return 22;
    if (mode == L"child-busy") ExitProcess(9);
    HandledFault();
    Sleep(300);
    if (mode == L"child-exit") ExitProcess(7);
    UnhandledFault();
    return 23;
}

DWORD RunChild(const wchar_t* mode, const wchar_t* dll)
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + mode + L" \"" + dll + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return 0xFFFFFFFF;
    DWORD code = 0xFFFFFFFE;
    if (WaitForSingleObject(process.hProcess, 90000) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    else
        TerminateProcess(process.hProcess, 0xFFFFFFFE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

unsigned failures = 0;
void Check(bool value, const char* text)
{
    printf("%s %s\n", value ? "PASS" : "FAIL", text);
    if (!value) ++failures;
}

bool AllStamped(const std::vector<std::string>& lines)
{
    static const std::regex stamp(R"(^\[\d\d:\d\d:\d\d\.\d{3}\] )");
    for (const auto& line : lines)
        if (!std::regex_search(line, stamp))
        {
            printf("INFO unstamped line: %.160s\n", line.c_str());
            return false;
        }
    return !lines.empty();
}

size_t CountLines(const std::vector<std::string>& lines, const char* first, const char* second)
{
    size_t count = 0;
    for (const auto& line : lines)
        if (line.find(first) != std::string::npos && line.find(second) != std::string::npos) ++count;
    return count;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    const std::wstring mode = argv[1];
    if (mode == L"child-crash" || mode == L"child-exit" || mode == L"child-busy") return Child(mode, argv[2]);
    if (mode != L"diagnostics") return 2;

    const std::wstring logPath = diagnostic_paths::RuntimeLog();
    const std::wstring dumpPath = logPath.substr(0, logPath.size() - 4) + L".crash.dmp";
    DeleteFileW(dumpPath.c_str());
    const char* exe = "SessionDiagnosticsTests.exe+0x";

    const DWORD crashCode = RunChild(L"child-crash", argv[2]);
    printf("INFO crash child exit 0x%08lX\n", crashCode);
    const std::string crash = ReadShared(logPath);
    const auto crashLines = Lines(crash);
    const auto has = [](const std::string& text, const char* fragment) { return text.find(fragment) != std::string::npos; };
    Check(crashCode == EXCEPTION_ACCESS_VIOLATION, "crash child ended with the access violation");
    Check(has(crash, "+diag.1 outputPullMask"), "banner names diag.1");
    Check(AllStamped(crashLines), "every crash-session line carries a local-time stamp");
    Check(has(crash, "Session diagnostics (beta): every line carries local time"), "diagnostics header");
    Check(has(crash, "Session: process ") && has(crash, " local (UTC"), "session start time");
    Check(has(crash, "System: Windows "), "system line");
    Check(has(crash, "Executable: ") && has(crash, "Backend module: "), "executable and module versions");
    Check(has(crash, "State: route="), "state line");
    Check(CountLines(crashLines, "First-chance exception 0xC0000005 (access violation, writing 0x18) at ", exe) == 1,
        "handled fault recorded once at first chance with its module offset");
    Check(CountLines(crashLines, "Unhandled exception 0xC0000005 (access violation, writing 0x10) at ", exe) == 1,
        "unhandled fault recorded with its module offset");
    Check(has(crash, "  registers: rip=") && has(crash, "  stack ("), "registers and stack");
    Check(CountLines(crashLines, "    #", exe) >= 2, "stack frames resolve into this executable");
    Check(has(crash, "  last state (sampled ") && has(crash, "  first-chance faults this session: "),
        "crash record carries the last state and recent faults");
    Check(CountLines(crashLines, " ago: 0xC0000005 (access violation, writing 0x18) at ", exe) == 1,
        "recent faults list the handled fault");
    Check(has(crash, "  minidump: ") && has(crash, " bytes)"), "minidump written");
    {
        std::ifstream dump(dumpPath, std::ios::binary);
        char signature[4]{};
        dump.read(signature, 4);
        dump.seekg(0, std::ios::end);
        const auto size = static_cast<long long>(dump.tellg());
        printf("INFO minidump %lld bytes\n", size);
        Check(dump.good() && std::string(signature, 4) == "MDMP" && size > 10000, "minidump file is a minidump");
    }
    printf("INFO crash session end present: %d\n", has(crash, "Session end:") ? 1 : 0);
    {
        // The next launch must be able to reopen the crashed session's log: measured the way the worker opens it
        // (write access, sharing reads only).
        const ULONGLONG started = GetTickCount64();
        DWORD error = 0;
        for (;;)
        {
            const HANDLE file = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); error = 0; break; }
            error = GetLastError();
            if (GetTickCount64() - started > 30000) break;
            Sleep(100);
        }
        printf("INFO crashed session's log writable again after %llu ms (last error %lu)\n",
            GetTickCount64() - started, error);
    }

    const DWORD exitCode = RunChild(L"child-exit", argv[2]);
    printf("INFO exit child exit %lu\n", exitCode);
    if (exitCode != 7)
    {
        std::ifstream note("session-diagnostics-child.txt");
        for (std::string line; std::getline(note, line);) printf("INFO child: %.400s\n", line.c_str());
    }
    const std::string exited = ReadShared(logPath);
    const auto exitLines = Lines(exited);
    Check(exitCode == 7, "exit child ended with its ExitProcess code");
    Check(AllStamped(exitLines), "every exit-session line carries a local-time stamp");
    Check(CountLines(exitLines, "Session end: process exit (ExitProcess, or main returned) after ", " s, on thread ") == 1,
        "session end recorded at process exit");
    Check(CountLines(exitLines, "  exit stack: ", exe) >= 1, "exit stack names the caller of ExitProcess");
    Check(has(exited, "  first-chance faults this session: ")
            && CountLines(exitLines, " ago: 0xC0000005 (access violation, writing 0x18) at ", exe) == 1,
        "session end lists the handled fault");
    Check(has(exited, "  last state (sampled ") && has(exited, "  repeated hook-state lines suppressed: "),
        "session end carries the last state and suppressed repeats");
    Check(!has(exited, "Unhandled exception"), "no unhandled-exception record on a clean exit");
    const std::string previous = ReadShared(logPath.substr(0, logPath.size() - 4) + L".previous.log");
    Check(has(previous, "Unhandled exception 0xC0000005"), "the crash session was kept as the previous log");

    // A runtime log another process still holds without write sharing (a crashed session's closing handles, a scan
    // on close): the session writes <log>.<pid>.log instead of none.
    DWORD busyPid = 0;
    {
        const HANDLE hold = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(hold != INVALID_HANDLE_VALUE, "parent holds the runtime log without write or delete sharing");
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" child-busy \"" + argv[2] + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        DWORD code = 0xFFFFFFFF;
        if (CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            busyPid = process.dwProcessId;
            if (WaitForSingleObject(process.hProcess, 90000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
            else TerminateProcess(process.hProcess, 0xFFFFFFFE);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
        printf("INFO busy child %lu exit %lu\n", busyPid, code);
        Check(code == 9, "busy-log child found its fallback log and exited");
    }
    const std::string busy = busyPid ? ReadShared(BusyFallbackLog(busyPid)) : std::string{};
    Check(has(busy, "could not be opened (error ") && has(busy, "this session logs to "), "fallback log says why");
    Check(AllStamped(Lines(busy)) && has(busy, "Session end: process exit"), "fallback log is a complete session");
    Check(ReadShared(logPath) == exited, "the held runtime log was left untouched");
    printf("%s session diagnostics (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
