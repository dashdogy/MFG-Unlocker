#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Frame-generation feature lifetime on the NGX runtime D3D12 route (live preset
// switching) and its Vulkan route (Create and CreateFeature1 alike). ampere_backend
// counts every runtime-route FG Create while it runs and records each successful
// one by route and handle until its exact successful Release. A provider FG Create
// outside every such Create, or a successful Create that cannot be recorded, makes
// the tracker inexact for the process: it then never reports quiescence.
namespace fg_lifetime
{
struct Snapshot
{
    uint32_t createsInFlight = 0;
    uint32_t liveFeatures = 0;
    uint64_t trackedCreates = 0;
    uint64_t trackedReleases = 0;
    bool exact = true;
};

template <size_t Capacity = 16>
class Tracker
{
public:
    // Sequentially consistent, before the native Create can reach a provider
    // preset read (see dlssg_preset::SetCreateQuiescenceProbe).
    void BeginCreate() noexcept { inFlight_.fetch_add(1, std::memory_order_seq_cst); }
    void EndCreate() noexcept { inFlight_.fetch_sub(1, std::memory_order_seq_cst); }

    // Called before EndCreate of the same Create, so a feature is never both
    // unrecorded and out of flight.
    void Created(size_t route, uintptr_t handle) noexcept
    {
        if (!handle) return;
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_)
        {
            if (entry.handle) continue;
            entry = {route, handle};
            live_.fetch_add(1, std::memory_order_seq_cst);
            created_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        inexact_.store(true, std::memory_order_seq_cst);
    }

    // Only after the native Release of this exact route and handle succeeded.
    void Released(size_t route, uintptr_t handle) noexcept
    {
        if (!handle) return;
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_)
        {
            if (entry.handle != handle || entry.route != route) continue;
            entry = {};
            live_.fetch_sub(1, std::memory_order_seq_cst);
            released_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // A provider-entry FG Create: it must be nested in a tracked runtime Create.
    void ObserveProviderCreate() noexcept
    {
        if (inFlight_.load(std::memory_order_seq_cst) == 0)
            inexact_.store(true, std::memory_order_seq_cst);
    }

    bool Quiescent() const noexcept
    {
        return inFlight_.load(std::memory_order_seq_cst) == 0 && live_.load(std::memory_order_seq_cst) == 0
            && !inexact_.load(std::memory_order_seq_cst);
    }

    Snapshot Read() const noexcept
    {
        return {inFlight_.load(std::memory_order_seq_cst), live_.load(std::memory_order_seq_cst),
            created_.load(std::memory_order_relaxed), released_.load(std::memory_order_relaxed),
            !inexact_.load(std::memory_order_seq_cst)};
    }

private:
    struct Entry
    {
        size_t route = SIZE_MAX;
        uintptr_t handle = 0;
    };
    std::array<Entry, Capacity> entries_{};
    std::mutex mutex_;
    std::atomic<uint32_t> inFlight_{0};
    std::atomic<uint32_t> live_{0};
    std::atomic<uint64_t> created_{0};
    std::atomic<uint64_t> released_{0};
    std::atomic<bool> inexact_{false};
};

// Spans one runtime-route FG Create, including every early rejection.
template <typename TrackerType>
class CreateScope
{
public:
    CreateScope(TrackerType& tracker, bool active) noexcept : tracker_(active ? &tracker : nullptr)
    {
        if (tracker_) tracker_->BeginCreate();
    }
    ~CreateScope()
    {
        if (tracker_) tracker_->EndCreate();
    }
    CreateScope(const CreateScope&) = delete;
    CreateScope& operator=(const CreateScope&) = delete;

private:
    TrackerType* tracker_ = nullptr;
};
}
