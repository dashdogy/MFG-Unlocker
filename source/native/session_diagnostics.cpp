#include "session_diagnostics.h"

#if MFG_UNLOCK_SESSION_DIAGNOSTICS
#include <DbgHelp.h> // types only: MiniDumpWriteDump is resolved when a crash is written
#include <intrin.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <vector>

namespace session_diagnostics
{
namespace
{
constexpr size_t kLineChars = 1024;
constexpr size_t kPathChars = 1024;
constexpr DWORD kHeapCorruption = 0xC0000374;
constexpr DWORD kStackBufferOverrun = 0xC0000409;
constexpr DWORD kCppException = 0xE06D7363;
// The unhandled-exception filter waits this long for the writer thread (text first, then the minidump).
constexpr DWORD kFatalWaitMs = 15000;

HANDLE gFile = nullptr; // the runtime log's OS handle, shared with its CRT stream
FlushFunction gFlush = nullptr;
std::atomic<bool> gStarted{false};
wchar_t gDumpPath[kPathChars]{};

struct StateText
{
    wchar_t state[512];
    wchar_t counters[512];
    ULONGLONG tick;
};
StateText gStates[2]{};
std::atomic<uint32_t> gStateSlot{0};
std::atomic<bool> gStateSet{false};

constexpr size_t kSeenSlots = 1024;
uint64_t gSeen[kSeenSlots]{};
size_t gSeenCount = 0;
SRWLOCK gSeenLock = SRWLOCK_INIT;
std::atomic<unsigned long long> gSuppressed{0};

// The last first-chance faults (any thread), for the crash and exit records. A slot is valid when its order matches.
struct Fault
{
    std::atomic<uint64_t> order{0};
    DWORD code = 0, thread = 0;
    ULONGLONG tick = 0;
    ULONG64 address = 0;
    ULONG_PTR access = 0, target = 0;
    bool hasTarget = false;
};
constexpr size_t kRecent = 32;
Fault gRecent[kRecent];
std::atomic<uint64_t> gRecentNext{0};
// Distinct fault sites written when they happen; later new sites are listed only in the crash and exit records.
constexpr uint32_t kInlineSites = 8;
std::atomic<ULONG64> gInlineSites[kInlineSites]{};
std::atomic<uint32_t> gInlineCount{0};
thread_local bool tObserving = false;

LPTOP_LEVEL_EXCEPTION_FILTER gPrevious = nullptr;
HANDLE gFatalRequest = nullptr, gFatalDone = nullptr, gWriter = nullptr;
EXCEPTION_POINTERS* gFatalPointers = nullptr;
DWORD gFatalThread = 0;
std::atomic<bool> gFatalClaimed{false}, gFatalText{false};
// The first-chance dump of a GPU driver fault (DumpDriverFault), written by the same writer thread.
HANDLE gEarlyRequest = nullptr, gEarlyDone = nullptr;
EXCEPTION_POINTERS* gEarlyPointers = nullptr;
DWORD gEarlyThread = 0;
std::atomic<bool> gEarlyClaimed{false};
wchar_t gEarlyDumpPath[kPathChars]{};

// ---- raw output (no CRT stream lock, no heap) -------------------------------------------------------------------
void WriteBytes(const wchar_t* text, int length) noexcept
{
    if (!gFile || length <= 0) return;
    char bytes[kLineChars * 3];
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, length, bytes, static_cast<int>(sizeof(bytes)), nullptr, nullptr);
    if (count <= 0) return;
    DWORD written = 0;
    WriteFile(gFile, bytes, static_cast<DWORD>(count), &written, nullptr);
}

// One WriteFile per line, so a record never interleaves inside another writer's line.
void WriteLine(const wchar_t* text, bool stamp) noexcept
{
    wchar_t line[kLineChars];
    size_t used = 0;
    if (stamp)
    {
        wchar_t prefix[kStampChars];
        Stamp(prefix);
        used = wcslen(prefix);
        wmemcpy(line, prefix, used);
    }
    const size_t length = wcsnlen(text ? text : L"", kLineChars - used - 3);
    wmemcpy(line + used, text ? text : L"", length);
    used += length;
    line[used++] = L'\r';
    line[used++] = L'\n';
    WriteBytes(line, static_cast<int>(used));
}

void Line(const wchar_t* format, ...) noexcept
{
    wchar_t text[kLineChars - kStampChars - 3];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);
    WriteLine(text, true);
}

// ---- describing addresses and stacks ----------------------------------------------------------------------------
void Describe(ULONG64 address, wchar_t* output, size_t capacity) noexcept
{
    PVOID base = nullptr;
    if (address && RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &base) && base)
    {
        wchar_t path[kPathChars];
        const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(base), path, static_cast<DWORD>(kPathChars));
        const wchar_t* name = L"module";
        if (length && length < kPathChars)
        {
            const wchar_t* slash = wcsrchr(path, L'\\');
            name = slash ? slash + 1 : path;
        }
        swprintf_s(output, capacity, L"%s+0x%llX", name,
            static_cast<unsigned long long>(address - reinterpret_cast<ULONG64>(base)));
        return;
    }
    swprintf_s(output, capacity, L"0x%llX", static_cast<unsigned long long>(address));
}

// x64 unwind of a copied context; a corrupt stack ends the walk.
size_t Unwind(const CONTEXT& start, ULONG64* frames, size_t capacity) noexcept
{
    CONTEXT context = start;
    size_t count = 0;
    __try
    {
        while (count < capacity && context.Rip)
        {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (entry)
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData,
                    &establisher, nullptr);
            }
            else
            {
                // A leaf function: the return address is on top of the stack.
                if (!context.Rsp) break;
                context.Rip = *reinterpret_cast<const ULONG64*>(context.Rsp);
                context.Rsp += 8;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

void StackText(const ULONG64* frames, size_t count, wchar_t* output, size_t capacity) noexcept
{
    output[0] = L'\0';
    size_t used = 0;
    for (size_t i = 0; i < count && used + 8 < capacity; ++i)
    {
        wchar_t where[160];
        Describe(frames[i], where, _countof(where));
        const int written = swprintf_s(output + used, capacity - used, L"%s%s", i ? L" < " : L"", where);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
    }
    if (!count) wcscpy_s(output, capacity, L"unavailable");
}

const wchar_t* CodeName(DWORD code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return L"access violation";
    case EXCEPTION_IN_PAGE_ERROR: return L"in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return L"privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return L"stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return L"integer overflow";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return L"noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION: return L"invalid disposition";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return L"datatype misalignment";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return L"floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return L"floating-point invalid operation";
    case EXCEPTION_FLT_STACK_CHECK: return L"floating-point stack check";
    case kHeapCorruption: return L"heap corruption";
    case kStackBufferOverrun: return L"fail-fast / stack buffer overrun";
    case kCppException: return L"C++ exception";
    default: return L"exception";
    }
}

// Faults worth recording at first chance. C++ exceptions and debugger/thread-name exceptions are routine.
bool Serious(DWORD code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW: case EXCEPTION_NONCONTINUABLE_EXCEPTION: case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_STACK_CHECK: case kHeapCorruption: case kStackBufferOverrun:
        return true;
    default:
        return false;
    }
}

void FillFault(Fault& fault, const EXCEPTION_RECORD& record) noexcept
{
    fault.code = record.ExceptionCode;
    fault.thread = GetCurrentThreadId();
    fault.tick = GetTickCount64();
    fault.address = reinterpret_cast<ULONG64>(record.ExceptionAddress);
    fault.hasTarget = record.NumberParameters >= 2
        && (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR);
    fault.access = fault.hasTarget ? record.ExceptionInformation[0] : 0;
    fault.target = fault.hasTarget ? record.ExceptionInformation[1] : 0;
}

void AccessText(const Fault& fault, wchar_t* output, size_t capacity) noexcept
{
    output[0] = L'\0';
    if (!fault.hasTarget) return;
    const wchar_t* kind = fault.access == 0 ? L"reading" : fault.access == 1 ? L"writing" : fault.access == 8
        ? L"executing" : L"accessing";
    swprintf_s(output, capacity, L", %s 0x%llX", kind, static_cast<unsigned long long>(fault.target));
}

// The thrown C++ type of an unhandled 0xE06D7363 (x64 ThrowInfo; every field is an image-relative offset).
void CppTypeText(const EXCEPTION_RECORD& record, wchar_t* output, size_t capacity) noexcept
{
    output[0] = L'\0';
    if (record.ExceptionCode != kCppException || record.NumberParameters < 4) return;
    __try
    {
        const ULONG_PTR image = record.ExceptionInformation[3];
        const auto* throwInfo = reinterpret_cast<const int*>(record.ExceptionInformation[2]);
        if (!image || !throwInfo) return;
        const auto* types = reinterpret_cast<const int*>(image + throwInfo[3]);
        if (types[0] < 1) return;
        const auto* catchable = reinterpret_cast<const int*>(image + types[1]);
        const char* name = reinterpret_cast<const char*>(image + catchable[1]) + 16; // TypeDescriptor name
        swprintf_s(output, capacity, L", type %.160hs", name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { output[0] = L'\0'; }
}

double SecondsSince(ULONGLONG tick) noexcept
{
    const ULONGLONG now = GetTickCount64();
    return now > tick ? double(now - tick) / 1000.0 : 0.0;
}

double ProcessSeconds() noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    GetSystemTimeAsFileTime(&now);
    const auto value = [](const FILETIME& time) { return (ULONGLONG(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return value(now) > value(created) ? double(value(now) - value(created)) / 1e7 : 0.0;
}

// The last state, the recent first-chance faults and the suppressed repeats (crash and exit records).
void WriteSummary() noexcept
{
    if (gStateSet.load(std::memory_order_acquire))
    {
        const StateText& state = gStates[gStateSlot.load(std::memory_order_acquire)];
        Line(L"  last state (sampled %.1f s earlier): %s | %s", SecondsSince(state.tick), state.state, state.counters);
    }
    const uint64_t next = gRecentNext.load(std::memory_order_acquire);
    if (!next)
        Line(L"  first-chance faults this session: none");
    else
    {
        Line(L"  first-chance faults this session: %llu (the last %llu, oldest first):",
            static_cast<unsigned long long>(next), static_cast<unsigned long long>(std::min<uint64_t>(next, 8)));
        for (uint64_t order = next > 8 ? next - 7 : 1; order <= next; ++order)
        {
            const Fault& fault = gRecent[(order - 1) % kRecent];
            if (fault.order.load(std::memory_order_acquire) != order) continue;
            wchar_t where[160], access[96];
            Describe(fault.address, where, _countof(where));
            AccessText(fault, access, _countof(access));
            Line(L"    %.3f s ago: 0x%08lX (%s%s) at %s on thread %lu", SecondsSince(fault.tick), fault.code,
                CodeName(fault.code), access, where, fault.thread);
        }
    }
    Line(L"  repeated hook-state lines suppressed: %llu", gSuppressed.load(std::memory_order_relaxed));
}

// ---- first chance -----------------------------------------------------------------------------------------------
bool NewInlineSite(ULONG64 address, uint32_t& slot) noexcept
{
    const uint32_t known = std::min(gInlineCount.load(std::memory_order_acquire), kInlineSites);
    for (uint32_t i = 0; i < known; ++i)
        if (gInlineSites[i].load(std::memory_order_relaxed) == address) return false;
    slot = gInlineCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kInlineSites) return false;
    gInlineSites[slot].store(address, std::memory_order_relaxed);
    return true;
}

// The 16 bytes at `address` as hex, or "unreadable".
void CodeBytes(ULONG64 address, wchar_t* output, size_t capacity) noexcept
{
    unsigned char bytes[16]{};
    __try { memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { wcscpy_s(output, capacity, L"unreadable"); return; }
    size_t used = 0;
    output[0] = L'\0';
    for (size_t i = 0; i < sizeof(bytes); ++i)
    {
        const int written = swprintf_s(output + used, capacity - used, i ? L" %02X" : L"%02X", bytes[i]);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
    }
}

// Its own frame holds the formatting buffers, so the observer's frame stays small.
__declspec(noinline) void WriteFirstChance(const Fault& fault, const CONTEXT& context, uint32_t slot, bool driver) noexcept
{
    wchar_t where[160], access[96], stack[640];
    ULONG64 frames[40];
    Describe(fault.address, where, _countof(where));
    AccessText(fault, access, _countof(access));
    const size_t count = Unwind(context, frames, _countof(frames));
    const size_t first = std::min<size_t>(count, 12);
    StackText(frames, first, stack, _countof(stack));
    Line(L"First-chance exception 0x%08lX (%s%s) at %s on thread %lu; stack: %s (observed only: the code that "
        L"raised it may handle it)%s", fault.code, CodeName(fault.code), access, where, fault.thread, stack,
        slot + 1 == kInlineSites ? L"; later new fault sites are listed only in crash and exit records" : L"");
    // The rest of the stack, 12 frames a line (a GPU driver's own frames can fill the first 12 and leave the caller
    // unknown).
    for (size_t at = first; at < count; at += 12)
    {
        StackText(frames + at, std::min<size_t>(count - at, 12), stack, _countof(stack));
        Line(L"  stack continues: %s", stack);
    }
    // A GPU driver's fault is placed without the dump.
    if (!driver) return;
    Line(L"  registers: rip=%016llX rsp=%016llX rbp=%016llX rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX "
        L"rsi=%016llX rdi=%016llX", context.Rip, context.Rsp, context.Rbp, context.Rax, context.Rbx, context.Rcx,
        context.Rdx, context.Rsi, context.Rdi);
    Line(L"  registers: r8=%016llX r9=%016llX r10=%016llX r11=%016llX r12=%016llX r13=%016llX r14=%016llX "
        L"r15=%016llX eflags=%08lX", context.R8, context.R9, context.R10, context.R11, context.R12, context.R13,
        context.R14, context.R15, context.EFlags);
    wchar_t code[64];
    CodeBytes(fault.address, code, _countof(code));
    Line(L"  code at the fault address: %s", code);
}

void DumpDriverFault(EXCEPTION_POINTERS* pointers, ULONG64 address) noexcept;
bool InDriverStore(ULONG64 address) noexcept;
// The first GPU driver fault, for the worker's "still running" line.
std::atomic<ULONGLONG> gDriverFaultTick{0};
std::atomic<ULONG64> gDriverFaultAddress{0};

LONG CALLBACK Observe(EXCEPTION_POINTERS* pointers) noexcept
{
    const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
    if (!record || tObserving || !Serious(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    tObserving = true;
    const DWORD error = GetLastError();
    const uint64_t order = gRecentNext.fetch_add(1, std::memory_order_acq_rel) + 1;
    Fault& fault = gRecent[(order - 1) % kRecent];
    FillFault(fault, *record);
    fault.order.store(order, std::memory_order_release);
    uint32_t slot = 0;
    // An exhausted stack gets no formatting; the record keeps it.
    if (record->ExceptionCode != EXCEPTION_STACK_OVERFLOW && pointers->ContextRecord && gFile
        && NewInlineSite(fault.address, slot))
    {
        const bool driver = InDriverStore(fault.address);
        if (driver && !gDriverFaultTick.load(std::memory_order_acquire))
        {
            gDriverFaultAddress.store(fault.address, std::memory_order_relaxed);
            gDriverFaultTick.store(fault.tick ? fault.tick : 1, std::memory_order_release);
        }
        WriteFirstChance(fault, *pointers->ContextRecord, slot, driver);
        DumpDriverFault(pointers, fault.address);
    }
    SetLastError(error);
    tObserving = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- unhandled exceptions ---------------------------------------------------------------------------------------
using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
// Resolved at Start, so no dump has to load a module behind a held loader lock.
MiniDumpWriteDumpFunction gWriteDump = nullptr;
constexpr MINIDUMP_TYPE kCrashDump = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
    | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithHandleData);
// The start-up hang dump: every thread's stack, thread info and handles; no indirectly referenced memory.
constexpr MINIDUMP_TYPE kHangDump = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
    | MiniDumpWithHandleData);
constexpr ULONGLONG kStartupHangMs = 15000;
ULONGLONG gStartTick = 0;
std::atomic<const wchar_t*> gStartupStep{L"start"};
wchar_t gHangDumpPath[kPathChars]{};

void WriteDump(EXCEPTION_POINTERS* pointers, DWORD thread, const wchar_t* path, MINIDUMP_TYPE type = kCrashDump) noexcept
{
    if (!path[0]) return;
    const HMODULE dbghelp = gWriteDump ? nullptr : LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto write = gWriteDump ? gWriteDump : dbghelp
        ? reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!write)
    {
        Line(L"  minidump: not written (dbghelp.dll unavailable, error %lu)", GetLastError());
        return;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Line(L"  minidump: not written (%s: error %lu)", path, GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{thread, pointers, FALSE};
    // A process with an unusual address space can refuse the fuller dump and leave an empty file (shadPS4:
    // 0x80070057). A smaller dump still holds the threads, stacks and modules.
    const MINIDUMP_TYPE attempts[] = {type, kHangDump, MiniDumpNormal};
    BOOL written = FALSE;
    DWORD firstError = 0, error = 0;
    bool reduced = false;
    for (const MINIDUMP_TYPE attempt : attempts)
    {
        if (reduced && attempt == type) continue;
        LARGE_INTEGER start{};
        SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        written = write(GetCurrentProcess(), GetCurrentProcessId(), file, attempt, pointers ? &exception : nullptr, nullptr, nullptr);
        if (written) break;
        error = GetLastError();
        if (!reduced) firstError = error;
        reduced = true;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (written && reduced)
        Line(L"  minidump: %s (%lld bytes, reduced after error 0x%08lX)", path, size.QuadPart, firstError);
    else if (written)
        Line(L"  minidump: %s (%lld bytes)", path, size.QuadPart);
    else
        Line(L"  minidump: failed (error 0x%08lX)", error);
}

void WriteFatal(EXCEPTION_POINTERS* pointers, DWORD thread, bool dump) noexcept
{
    if (gFlush) gFlush(&WriteRaw); // the lines queued before the crash come first
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    Fault fault;
    FillFault(fault, record);
    wchar_t where[160], access[96], type[200];
    Describe(fault.address, where, _countof(where));
    AccessText(fault, access, _countof(access));
    CppTypeText(record, type, _countof(type));
    Line(L"Unhandled exception 0x%08lX (%s%s%s) at %s on thread %lu", fault.code, CodeName(fault.code), access, type,
        where, thread);
    if (const CONTEXT* context = pointers->ContextRecord)
    {
        Line(L"  registers: rip=%016llX rsp=%016llX rbp=%016llX rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX "
            L"rsi=%016llX rdi=%016llX", context->Rip, context->Rsp, context->Rbp, context->Rax, context->Rbx,
            context->Rcx, context->Rdx, context->Rsi, context->Rdi);
        Line(L"  registers: r8=%016llX r9=%016llX r10=%016llX r11=%016llX r12=%016llX r13=%016llX r14=%016llX "
            L"r15=%016llX eflags=%08lX", context->R8, context->R9, context->R10, context->R11, context->R12,
            context->R13, context->R14, context->R15, context->EFlags);
        ULONG64 frames[32];
        const size_t count = Unwind(*context, frames, _countof(frames));
        Line(L"  stack (%zu frames, innermost first):", count);
        for (size_t i = 0; i < count; ++i)
        {
            Describe(frames[i], where, _countof(where));
            Line(L"    #%02zu %s", i, where);
        }
    }
    WriteSummary();
    gFatalText.store(true, std::memory_order_release);
    if (dump) WriteDump(pointers, thread, gDumpPath);
    else Line(L"  minidump: not written (the writer thread was unavailable)");
}

// The worker went silent during start-up (seen inside Steam's own module loading). The dump's stacks say which lock
// it waits on.
void WriteHang() noexcept
{
    if (gFlush) gFlush(&WriteRaw);
    const wchar_t* step = gStartupStep.load(std::memory_order_acquire);
    Line(L"Start-up watchdog: the patch worker has not published its state %.1f s after start (last step: %s); "
        L"writing a minidump of every thread", SecondsSince(gStartTick), step ? step : L"?");
    WriteDump(nullptr, 0, gHangDumpPath, kHangDump);
}

// The crash record (once, then the thread ends); before it, the first-chance driver fault dump and the start-up hang
// dump.
DWORD WINAPI WriterThread(void*) noexcept
{
    const HANDLE requests[2] = {gFatalRequest, gEarlyRequest};
    bool watching = gHangDumpPath[0] != L'\0';
    for (;;)
    {
        DWORD wait = INFINITE;
        if (watching)
        {
            const ULONGLONG elapsed = GetTickCount64() - gStartTick;
            wait = elapsed >= kStartupHangMs ? 0 : static_cast<DWORD>(kStartupHangMs - elapsed);
        }
        const DWORD signaled = WaitForMultipleObjects(gEarlyRequest ? 2 : 1, requests, FALSE, wait);
        if (signaled == WAIT_TIMEOUT)
        {
            watching = false;
            if (!gStateSet.load(std::memory_order_acquire)) WriteHang();
            continue;
        }
        if (signaled == WAIT_OBJECT_0 + 1)
        {
            if (gEarlyPointers) WriteDump(gEarlyPointers, gEarlyThread, gEarlyDumpPath);
            SetEvent(gEarlyDone);
            continue;
        }
        if (signaled == WAIT_OBJECT_0 && gFatalPointers) WriteFatal(gFatalPointers, gFatalThread, true);
        break;
    }
    SetEvent(gFatalDone);
    return 0;
}

// The writer cannot load dbghelp while the crashing thread holds the loader lock (PEB+0x110).
bool OwnsLoaderLock() noexcept
{
    const auto* peb = reinterpret_cast<const unsigned char*>(__readgsqword(0x60));
    const auto* lock = peb ? *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + 0x110) : nullptr;
    return lock && reinterpret_cast<DWORD_PTR>(lock->OwningThread) == GetCurrentThreadId();
}

// A module loaded from the driver store: the GPU vendors' user-mode drivers (nvwgf2umx, amdxc64, igd12umd64, ...).
bool InDriverStore(ULONG64 address) noexcept
{
    PVOID base = nullptr;
    if (!address || !RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &base) || !base) return false;
    wchar_t path[kPathChars];
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(base), path, static_cast<DWORD>(kPathChars));
    if (!length || length >= kPathChars) return false;
    for (DWORD i = 0; i < length; ++i)
        if (path[i] >= L'A' && path[i] <= L'Z') path[i] = static_cast<wchar_t>(path[i] - L'A' + L'a');
    return wcsstr(path, L"\\driverstore\\") != nullptr;
}

// The first serious fault inside a GPU driver is dumped at first chance, before whatever ends the process (such a
// session can end with no unhandled record, minidump, WER report or engine crash folder). The faulting thread waits
// for the writer thread (at most kFatalWaitMs) and the exception then goes on unchanged.
void DumpDriverFault(EXCEPTION_POINTERS* pointers, ULONG64 address) noexcept
{
    if (!gEarlyRequest || !gEarlyDone || !gWriter || !gEarlyDumpPath[0] || !InDriverStore(address) || OwnsLoaderLock()
        || WaitForSingleObject(gWriter, 0) != WAIT_TIMEOUT || gEarlyClaimed.exchange(true, std::memory_order_acq_rel))
        return;
    Line(L"  writing a minidump of this GPU driver fault (first chance, once per session)");
    gEarlyPointers = pointers;
    gEarlyThread = GetCurrentThreadId();
    SetEvent(gEarlyRequest);
    WaitForSingleObject(gEarlyDone, kFatalWaitMs);
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* pointers) noexcept
{
    if (pointers && pointers->ExceptionRecord && gFile && !gFatalClaimed.exchange(true, std::memory_order_acq_rel))
    {
        bool done = false;
        if (gWriter && !OwnsLoaderLock() && WaitForSingleObject(gWriter, 0) == WAIT_TIMEOUT)
        {
            gFatalPointers = pointers;
            gFatalThread = GetCurrentThreadId();
            SetEvent(gFatalRequest);
            done = WaitForSingleObject(gFatalDone, kFatalWaitMs) == WAIT_OBJECT_0;
        }
        if (!done && !gFatalText.load(std::memory_order_acquire))
            WriteFatal(pointers, GetCurrentThreadId(), false);
    }
    return gPrevious ? gPrevious(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

// ---- environment ------------------------------------------------------------------------------------------------
struct Registry
{
    decltype(&RegOpenKeyExW) open = nullptr;
    decltype(&RegQueryValueExW) query = nullptr;
    decltype(&RegEnumKeyExW) enumerate = nullptr;
    decltype(&RegCloseKey) close = nullptr;
    bool Load() noexcept
    {
        // Resolved here so the module keeps its kernel32/user32/gdi32-only static imports.
        const HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!advapi) return false;
        open = reinterpret_cast<decltype(open)>(GetProcAddress(advapi, "RegOpenKeyExW"));
        query = reinterpret_cast<decltype(query)>(GetProcAddress(advapi, "RegQueryValueExW"));
        enumerate = reinterpret_cast<decltype(enumerate)>(GetProcAddress(advapi, "RegEnumKeyExW"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(advapi, "RegCloseKey"));
        return open && query && enumerate && close;
    }
    bool String(HKEY key, const wchar_t* name, wchar_t* output, DWORD chars) const noexcept
    {
        DWORD type = 0, bytes = (chars - 1) * sizeof(wchar_t);
        output[0] = L'\0';
        if (query(key, name, nullptr, &type, reinterpret_cast<BYTE*>(output), &bytes) != ERROR_SUCCESS
            || (type != REG_SZ && type != REG_EXPAND_SZ)) { output[0] = L'\0'; return false; }
        output[std::min<DWORD>(bytes / sizeof(wchar_t), chars - 1)] = L'\0';
        return output[0] != L'\0';
    }
    bool Dword(HKEY key, const wchar_t* name, DWORD& value) const noexcept
    {
        DWORD type = 0, bytes = sizeof(value);
        return query(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS
            && type == REG_DWORD;
    }
};

void Trim(wchar_t* text) noexcept
{
    size_t start = 0, length = wcslen(text);
    while (start < length && text[start] == L' ') ++start;
    while (length > start && text[length - 1] == L' ') --length;
    wmemmove(text, text + start, length - start);
    text[length - start] = L'\0';
}

// "1.6.0.0, product "A Plague Tale: Requiem" 1.6.0.0" from the file's version resource.
void VersionText(const wchar_t* path, wchar_t* output, size_t capacity) noexcept
{
    wcscpy_s(output, capacity, L"no version resource");
    try
    {
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
        if (!size) return;
        std::vector<BYTE> data(size);
        if (!GetFileVersionInfoW(path, 0, size, data.data())) return;
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed) return;
        int used = swprintf_s(output, capacity, L"%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
            LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
        struct Translation { WORD language, codePage; }* translation = nullptr;
        if (used <= 0 || !VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation),
                &length) || !translation || length < sizeof(Translation)) return;
        const auto field = [&](const wchar_t* name) -> const wchar_t* {
            wchar_t query[96];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translation->language, translation->codePage, name);
            wchar_t* value = nullptr;
            UINT chars = 0;
            return VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &chars) && value && chars
                ? value : nullptr;
        };
        const wchar_t* product = field(L"ProductName");
        const wchar_t* productVersion = field(L"ProductVersion");
        if (product || productVersion)
            swprintf_s(output + used, capacity - size_t(used), L", product \"%.80s\" %.40s", product ? product : L"",
                productVersion ? productVersion : L"");
    }
    catch (...) {}
}

void LogEnvironment(LogFunction log, bool observer) noexcept
{
    wchar_t line[kLineChars];
    swprintf_s(line, L"Session diagnostics (beta): every line carries local time; first-chance faults (%u distinct sites "
        L"written as they happen%s) and unhandled exceptions are recorded with a stack; minidump %s (the first GPU "
        L"driver fault at first chance: .fault.dmp beside it); \"Session end\" closes a normal exit. A log that stops "
        L"without it was ended abruptly (fail-fast, TerminateProcess or Task Manager, a hang, or power loss).",
        kInlineSites, observer ? L"" : L"; vectored observer unavailable", gDumpPath[0] ? gDumpPath : L"unavailable");
    log(line);

    FILETIME created{}, exited{}, kernel{}, user{}, local{};
    SYSTEMTIME start{};
    TIME_ZONE_INFORMATION zone{};
    const DWORD zoneId = GetTimeZoneInformation(&zone);
    const long bias = -(zone.Bias + (zoneId == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : zone.StandardBias));
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) && FileTimeToLocalFileTime(&created, &local)
        && FileTimeToSystemTime(&local, &start))
    {
        swprintf_s(line, L"Session: process %lu started %04u-%02u-%02u %02u:%02u:%02u.%03u local (UTC%c%02ld:%02ld); "
            L"log opened %.1f s later", GetCurrentProcessId(), start.wYear, start.wMonth, start.wDay, start.wHour,
            start.wMinute, start.wSecond, start.wMilliseconds, bias < 0 ? L'-' : L'+', std::abs(bias) / 60,
            std::abs(bias) % 60, ProcessSeconds());
        log(line);
    }

    Registry registry;
    const bool haveRegistry = registry.Load();
    RTL_OSVERSIONINFOW os{sizeof(os)};
    using RtlGetVersionFunction = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (const auto getVersion = ntdll ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr)
        getVersion(&os);
    wchar_t displayVersion[32]{}, cpu[128]{};
    DWORD revision = 0;
    HKEY key = nullptr;
    if (haveRegistry && registry.open(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
            KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        registry.String(key, L"DisplayVersion", displayVersion, _countof(displayVersion));
        registry.Dword(key, L"UBR", revision);
        registry.close(key);
    }
    if (haveRegistry && registry.open(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
            KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        registry.String(key, L"ProcessorNameString", cpu, _countof(cpu));
        registry.close(key);
        Trim(cpu);
    }
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    using WineVersionFunction = const char*(__cdecl*)();
    const auto wine = ntdll ? reinterpret_cast<WineVersionFunction>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    swprintf_s(line, L"System: Windows %lu (%lu.%lu.%lu.%lu%s%s), CPU %s with %lu logical processors, RAM %.1f GB "
        L"(%.1f GB free)%s%hs", os.dwMajorVersion == 10 && os.dwBuildNumber >= 22000 ? 11ul : os.dwMajorVersion,
        os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, revision, displayVersion[0] ? L", " : L"", displayVersion,
        cpu[0] ? cpu : L"unknown", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
        double(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0), double(memory.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0),
        wine ? L", Wine " : L"", wine ? wine() : "");
    log(line);

    // Display adapters in use (EnumDisplayDevices skips removed GPUs the driver class key still lists), then their
    // driver entries.
    wchar_t active[8][128]{};
    uint32_t activeCount = 0;
    DISPLAY_DEVICEW device{sizeof(device)};
    for (DWORD index = 0; activeCount < _countof(active) && EnumDisplayDevicesW(nullptr, index, &device, 0); ++index)
    {
        bool seen = false;
        for (uint32_t i = 0; i < activeCount; ++i) seen = seen || wcscmp(active[i], device.DeviceString) == 0;
        if (!seen && device.DeviceString[0]) wcscpy_s(active[activeCount++], device.DeviceString);
        device = DISPLAY_DEVICEW{sizeof(device)};
    }
    uint32_t listed = 0;
    HKEY adapters = nullptr;
    if (haveRegistry && registry.open(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
            KEY_ENUMERATE_SUB_KEYS, &adapters) == ERROR_SUCCESS)
    {
        for (DWORD index = 0; index < 32; ++index)
        {
            wchar_t name[64]{};
            DWORD nameChars = _countof(name);
            if (registry.enumerate(adapters, index, name, &nameChars, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            HKEY entry = nullptr;
            if (registry.open(adapters, name, 0, KEY_QUERY_VALUE, &entry) != ERROR_SUCCESS) continue;
            wchar_t description[128]{}, version[64]{}, date[32]{}, provider[64]{};
            registry.String(entry, L"DriverDesc", description, _countof(description));
            registry.String(entry, L"DriverVersion", version, _countof(version));
            registry.String(entry, L"DriverDate", date, _countof(date));
            registry.String(entry, L"ProviderName", provider, _countof(provider));
            registry.close(entry);
            bool inUse = activeCount == 0;
            for (uint32_t i = 0; i < activeCount; ++i) inUse = inUse || _wcsicmp(active[i], description) == 0;
            if (!description[0] || !inUse) continue;
            // NVIDIA's public number is the last digit of the third field and the fourth: 32.0.15.6094 = 560.94.
            wchar_t publicVersion[48]{};
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (wcsstr(provider, L"NVIDIA") && swscanf_s(version, L"%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            {
                const unsigned number = (c % 10) * 10000 + d;
                swprintf_s(publicVersion, L" (NVIDIA %u.%02u)", number / 100, number % 100);
            }
            swprintf_s(line, L"Display adapter: %s, driver %s%s dated %s by %s", description, version[0] ? version : L"?",
                publicVersion, date[0] ? date : L"?", provider[0] ? provider : L"?");
            log(line);
            ++listed;
        }
        registry.close(adapters);
    }
    if (!listed)
        for (uint32_t i = 0; i < activeCount; ++i)
        {
            swprintf_s(line, L"Display adapter: %s (no driver entry)", active[i]);
            log(line);
        }

    wchar_t path[kPathChars]{}, version[256]{};
    const DWORD exeLength = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(kPathChars));
    if (exeLength && exeLength < kPathChars)
    {
        VersionText(path, version, _countof(version));
        swprintf_s(line, L"Executable: %s, file %s", path, version);
        log(line);
    }
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LogEnvironment), &self))
    {
        const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(kPathChars));
        if (length && length < kPathChars)
        {
            VersionText(path, version, _countof(version));
            swprintf_s(line, L"Backend module: %s, file %s", path, version);
            log(line);
        }
    }
}
}

void Stamp(wchar_t (&output)[kStampChars]) noexcept
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    swprintf_s(output, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

void Start(FILE* runtimeLog, const wchar_t* logPath, LogFunction log, FlushFunction flushQueued) noexcept
{
    if (!runtimeLog || !log || gStarted.load(std::memory_order_acquire)) return;
    const intptr_t handle = _get_osfhandle(_fileno(runtimeLog));
    if (handle == -1 || handle == -2) return;
    gFile = reinterpret_cast<HANDLE>(handle);
    gFlush = flushQueued;
    const size_t length = logPath ? wcslen(logPath) : 0;
    if (length > 4 && length + 16 < kPathChars && _wcsicmp(logPath + length - 4, L".log") == 0)
    {
        wmemcpy(gDumpPath, logPath, length - 4);
        wcscpy_s(gDumpPath + length - 4, kPathChars - (length - 4), L".crash.dmp");
        wmemcpy(gEarlyDumpPath, logPath, length - 4);
        wcscpy_s(gEarlyDumpPath + length - 4, kPathChars - (length - 4), L".fault.dmp");
        wmemcpy(gHangDumpPath, logPath, length - 4);
        wcscpy_s(gHangDumpPath + length - 4, kPathChars - (length - 4), L".hang.dmp");
    }
    gStartTick = GetTickCount64();
    // The dump writer, resolved now (outside the loader lock). dbgcore.dll holds the MiniDumpWriteDump that
    // dbghelp.dll forwards to; dbghelp.dll itself is not loaded by name here, because a System32 copy loaded this early
    // would then satisfy the game's later load of a dbghelp.dll in its own folder (OptiScaler installs under that name).
    if (const HMODULE core = LoadLibraryExW(L"dbgcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        gWriteDump = reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(core, "MiniDumpWriteDump"));
    gFatalRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gFatalDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gEarlyRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gEarlyDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (gFatalRequest && gFatalDone)
        gWriter = CreateThread(nullptr, 0, &WriterThread, nullptr, 0, nullptr);
    if (gWriter)
    {
        using SetDescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        if (const auto describe = kernelbase
                ? reinterpret_cast<SetDescriptionFunction>(GetProcAddress(kernelbase, "SetThreadDescription")) : nullptr)
            describe(gWriter, L"RTXMFG crash writer");
    }
    const PVOID observer = AddVectoredExceptionHandler(1, &Observe);
    gPrevious = SetUnhandledExceptionFilter(&Unhandled);
    gStarted.store(true, std::memory_order_release);
    LogEnvironment(log, observer != nullptr);
}

bool PublishState(const wchar_t* state, const wchar_t* counters) noexcept
{
    const uint32_t current = gStateSlot.load(std::memory_order_relaxed);
    const bool changed = !gStateSet.load(std::memory_order_relaxed)
        || wcscmp(gStates[current].state, state ? state : L"") != 0;
    StateText& next = gStates[current ^ 1u];
    wcsncpy_s(next.state, state ? state : L"", _TRUNCATE);
    wcsncpy_s(next.counters, counters ? counters : L"", _TRUNCATE);
    next.tick = GetTickCount64();
    gStateSlot.store(current ^ 1u, std::memory_order_release);
    gStateSet.store(true, std::memory_order_release);
    return changed;
}

bool FirstOccurrence(const wchar_t* line) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t* character = line ? line : L""; *character; ++character)
    {
        hash ^= static_cast<uint16_t>(*character);
        hash *= 1099511628211ull;
    }
    if (!hash) hash = 1;
    AcquireSRWLockExclusive(&gSeenLock);
    size_t slot = hash % kSeenSlots;
    for (size_t probe = 0; probe < kSeenSlots; ++probe, slot = (slot + 1) % kSeenSlots)
    {
        if (gSeen[slot] == hash)
        {
            ReleaseSRWLockExclusive(&gSeenLock);
            gSuppressed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!gSeen[slot])
        {
            // A full table logs every further line.
            if (gSeenCount < kSeenSlots * 3 / 4)
            {
                gSeen[slot] = hash;
                ++gSeenCount;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&gSeenLock);
    return true;
}

unsigned long long SuppressedRepeats() noexcept
{
    return gSuppressed.load(std::memory_order_relaxed);
}

void WriteRaw(const wchar_t* line) noexcept
{
    WriteLine(line, false);
}

void StartupStep(const wchar_t* step) noexcept
{
    gStartupStep.store(step, std::memory_order_release);
}

bool DriverFault(ULONGLONG& tick, ULONG64& address) noexcept
{
    tick = gDriverFaultTick.load(std::memory_order_acquire);
    address = gDriverFaultAddress.load(std::memory_order_relaxed);
    return tick != 0;
}

void DescribeAddress(ULONG64 address, wchar_t* output, size_t capacity) noexcept
{
    Describe(address, output, capacity);
}

void OnProcessDetach(bool processTerminating) noexcept
{
    if (!gStarted.load(std::memory_order_acquire) || !gFile) return;
    if (gFlush) gFlush(&WriteRaw);
    Line(L"Session end: %s after %.1f s, on thread %lu%s", processTerminating
            ? L"process exit (ExitProcess, or main returned)" : L"backend module unloaded", ProcessSeconds(),
        GetCurrentThreadId(), gFatalClaimed.load(std::memory_order_acquire)
            ? L", after the unhandled-exception record above" : L"");
    // Who ended the process: the exiting thread's own stack (ntdll's shutdown, then the caller of ExitProcess).
    void* frames[40]{};
    const USHORT count = RtlCaptureStackBackTrace(0, _countof(frames), frames, nullptr);
    for (USHORT first = 0; first < count; first += 5)
    {
        wchar_t text[kLineChars - 64]{};
        size_t used = 0;
        for (USHORT i = first; i < count && i < first + 5; ++i)
        {
            wchar_t where[160];
            Describe(reinterpret_cast<ULONG64>(frames[i]), where, _countof(where));
            const int written = swprintf_s(text + used, _countof(text) - used, L"%s#%02u %s", i == first ? L"" : L"  ",
                static_cast<unsigned>(i), where);
            if (written <= 0) break;
            used += static_cast<size_t>(written);
        }
        Line(L"  exit stack: %s", text);
    }
    WriteSummary();
}
}
#endif
