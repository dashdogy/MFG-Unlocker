#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include "ampere_mfg_limits.h"

namespace ampere_wrapper
{
enum class Profile : uint32_t
{ eUnknown, eCompact, eExtended, eLegacyPacer, eLegacyCompact, eLegacySmall, eLegacyLarge, eLegacyDirect };
// Read-only allocation view. The private layout is admitted by independently
// matched startup, swap-chain, buffer-loop and NGX creation code windows.
struct Layout
{
    HMODULE module{};
    uintptr_t contextSlot = 0;
    uintptr_t createBegin = 0, createEnd = 0;
    Profile profile = Profile::eUnknown;
    uint32_t maximumGeneratedFrames = 0;
    bool frameIdAvailable = true;
};
struct Capacity
{
    uintptr_t context = 0, swapchain = 0;
    uintptr_t buffersBegin = 0, buffersEnd = 0, buffersCapacity = 0;
    std::array<uintptr_t, ampere_mfg::kPresentationBuffers> buffers{};
    uintptr_t ringOwner = 0;
    uint32_t maximumGeneratedFrames = 0, bufferCount = 0;
    bool operator==(const Capacity&) const = default;
};
// The wrapper's swap-chain change path (its ResizeBuffers, ResizeBuffers1 and
// SetFullscreenState post hooks; postSwapchainChanged in sl.dlss_g 2.14) puts
// new per-buffer objects for the same swap chain into the same vector, with no
// new NGX Create. Returns how many entries changed when that is the only
// difference from `stored`, otherwise 0. `current` must come from Read(),
// which proves its entries non-null and distinct.
inline uint32_t RefreshedBuffers(const Capacity& stored, const Capacity& current) noexcept
{
    if (!stored.context || current.context != stored.context || current.swapchain != stored.swapchain
        || current.ringOwner != stored.ringOwner || current.buffersBegin != stored.buffersBegin
        || current.buffersEnd != stored.buffersEnd || current.buffersCapacity != stored.buffersCapacity
        || current.maximumGeneratedFrames != stored.maximumGeneratedFrames
        || current.bufferCount != stored.bufferCount || current.bufferCount > current.buffers.size()) return 0;
    uint32_t changed = 0;
    for (size_t i = 0; i < current.buffers.size(); ++i)
    {
        if (i < current.bufferCount) changed += current.buffers[i] != stored.buffers[i];
        else if (current.buffers[i] != stored.buffers[i]) return 0;
    }
    return changed;
}
// Failure reporting only. Observed fields do not certify an allocation.
struct Observation
{
    Capacity capacity{};
    uint32_t maximum = 0, bufferCount = 0;
    bool contextReadable = false, buffersReadable = false;
};
bool Discover(HMODULE, Layout&) noexcept;
bool BeforeSwapchain(const Layout&) noexcept;
// Capacity.maximumGeneratedFrames is the ring the wrapper actually allocated:
// 1 on a legacy 2x layout; on an MFG layout the maximum N (1..5) its startup
// cached, with exactly N+1 distinct buffers.
bool Read(const Layout&, Capacity&) noexcept;
bool Matches(const Layout&, const Capacity&) noexcept;
Observation InspectFailure(const Layout&) noexcept;
// Failure reporting only: the first check Read() would fail now,
// in Read()'s order, or "readable". It never admits an allocation.
const char* ReadFailure(const Layout&) noexcept;
bool CreationOnStack(const Layout&) noexcept;
}
