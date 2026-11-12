#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdio>

// Beta-build session diagnostics for the runtime log, compiled with
// MFG_UNLOCK_SESSION_DIAGNOSTICS. Local-time stamps on every line, a one-time environment header, the worker's state,
// first-chance and unhandled-exception records with a minidump, and a "Session end" record at process exit. Crash and
// exit records go straight to the log's OS handle: no CRT stream lock and no heap, so a fault inside either still
// reaches the file. The exception handlers only observe; they never change how an exception is handled.
namespace session_diagnostics
{
using LogFunction = void (*)(const wchar_t*);
// Lock-free flush of lines that are queued for the log (single_module::TryDrainLog).
using FlushFunction = bool (*)(LogFunction sink);

// "[HH:MM:SS.mmm] " in local time, the same clock and format family as OptiScaler's log.
constexpr size_t kStampChars = 16;
void Stamp(wchar_t (&output)[kStampChars]) noexcept;

// After the runtime log opens (patch worker, outside the loader lock): keeps the log's OS handle, starts the waiting
// crash writer thread, adds the vectored first-chance observer and the unhandled-exception filter (the previous
// filter is called after ours), then logs the environment header through `log`.
void Start(FILE* runtimeLog, const wchar_t* logPath, LogFunction log, FlushFunction flushQueued) noexcept;

// The worker's state (compared) and counters (not compared), kept for the crash and exit records. True when the
// state differs from the previous one. Patch worker only.
bool PublishState(const wchar_t* state, const wchar_t* counters) noexcept;

// Hook-state lines repeat on every module rescan: true for the first time a line is seen (log it), false for a
// repeat (counted, reported in the heartbeat and the session end).
bool FirstOccurrence(const wchar_t* line) noexcept;
unsigned long long SuppressedRepeats() noexcept;

// A pre-stamped line written straight to the log handle (the sink of the queued-line flush).
void WriteRaw(const wchar_t* line) noexcept;

// DLL_PROCESS_DETACH of the backend module (loader lock held): flushes the queued lines, then the session end.
void OnProcessDetach(bool processTerminating) noexcept;

// The patch worker's current start-up step (a literal). When the worker has not published its
// first state 15 s after Start, the writer thread logs the step and writes <log>.hang.dmp once (threads and handles).
void StartupStep(const wchar_t* step) noexcept;

// The first serious first-chance fault inside a GPU driver (a driver-store module): its tick and
// address; false while there was none. DescribeAddress gives module+offset.
bool DriverFault(ULONGLONG& tick, ULONG64& address) noexcept;
void DescribeAddress(ULONG64 address, wchar_t* output, size_t capacity) noexcept;
}
