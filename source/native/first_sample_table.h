#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace temporal_interval_trace
{
// Real-frame counts per DLSS-G feature handle (one count per first sample of a
// batch). Games that release and recreate DLSS-G on cutscenes, loading screens
// or menus make a new handle each time (GitHub issue 158), so a full table
// reuses its least recently used slot: a released handle ages out first.
//
// A slot's count never goes down, also across reuse: the FPS readers take each
// slot's increase over a window, so a reused slot keeps counting from where it
// stood and the window's increase is the new handle's frames.
template <size_t Capacity>
class FirstSampleTable
{
    static_assert(Capacity > 0);

public:
    void Record(uintptr_t handle) noexcept
    {
        if (!handle)
            return;
        // A lost race (another thread took the chosen slot) scans again; a
        // handle that still finds no slot after that is dropped, not looped on.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            size_t oldest = Capacity;
            uint64_t oldestUse = UINT64_MAX;
            uintptr_t oldestHandle = 0;
            for (size_t index = 0; index < Capacity; ++index)
            {
                uintptr_t current = handles_[index].load(std::memory_order_acquire);
                if (current == handle)
                {
                    Touch(index);
                    return;
                }
                if (current == 0)
                {
                    uintptr_t empty = 0;
                    if (handles_[index].compare_exchange_strong(empty, handle,
                            std::memory_order_acq_rel, std::memory_order_acquire)
                        || empty == handle)
                    {
                        Touch(index);
                        return;
                    }
                    current = empty;
                }
                const uint64_t use = lastUse_[index].load(std::memory_order_relaxed);
                if (use < oldestUse)
                {
                    oldestUse = use;
                    oldest = index;
                    oldestHandle = current;
                }
            }
            if (oldest == Capacity)
                return;
            uintptr_t expected = oldestHandle;
            if (handles_[oldest].compare_exchange_strong(expected, handle,
                    std::memory_order_acq_rel, std::memory_order_acquire)
                || expected == handle)
            {
                Touch(oldest);
                return;
            }
        }
    }

    uintptr_t Handle(size_t index) const noexcept
    {
        return index < Capacity ? handles_[index].load(std::memory_order_acquire) : 0;
    }

    uint64_t Samples(size_t index) const noexcept
    {
        return index < Capacity ? counts_[index].load(std::memory_order_relaxed) : 0;
    }

private:
    void Touch(size_t index) noexcept
    {
        counts_[index].fetch_add(1, std::memory_order_relaxed);
        lastUse_[index].store(clock_.fetch_add(1, std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }

    std::array<std::atomic<uintptr_t>, Capacity> handles_{};
    std::array<std::atomic<uint64_t>, Capacity> counts_{};
    std::array<std::atomic<uint64_t>, Capacity> lastUse_{};
    std::atomic<uint64_t> clock_{0};
};
}
