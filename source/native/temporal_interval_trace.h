#pragma once

#include <Windows.h>

#include "first_sample_table.h"

#include <array>
#include <cstddef>
#include <cstdint>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace temporal_interval_trace
{
constexpr size_t kFirstSampleHandleCapacity = 32;

struct FirstSampleCounter
{
    uintptr_t handle = 0;
    uint64_t samples = 0;
};

struct Snapshot
{
    bool initialized = false;
    bool enabled = false;
    bool logReady = false;
    uint64_t validSamples = 0;
    uint64_t invalidSamples = 0;
    uint64_t droppedSamples = 0;
    uint32_t seenCountMask = 0;
    uint32_t seenIndexMask = 0;
    int32_t lastCount = 0;
    int32_t lastIndex = 0;
    uint32_t lastPositionNumerator = 0;
    uint32_t lastPositionDenominator = 0;
    std::array<FirstSampleCounter, kFirstSampleHandleCapacity>
        firstSampleCounters{};
};

// Initializes a fixed, lock-free event pool. Record() never opens or writes a
// file; the patch worker drains the queue through Flush().
// Enabled() keeps the counters (the FPS readout). The CSV file is written only
// when RTXMFG_DIAGNOSTIC_INTERVAL_TRACE=1 was set before Initialize (FileEnabled()).
void Initialize(const wchar_t* tempDirectory, const wchar_t* executablePath) noexcept;
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;
bool FileEnabled() noexcept;
void Record(const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, bool descriptorReady) noexcept;
// Shared NGX runtime Evaluate entries carry multiple feature types and have no
// feature-id argument. This filtered form records only a structurally valid
// DLSS-G temporal request, leaving unrelated DLSS/SR traffic untouched.
bool RecordIfValidTemporalSample(const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, bool descriptorReady) noexcept;
void Flush() noexcept;
Snapshot ReadSnapshot() noexcept;
const wchar_t* FileName() noexcept;
const wchar_t* FilePath() noexcept;
}
