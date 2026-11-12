#pragma once

#include <atomic>
#include <mutex>

namespace ada_gate_hold
{
// GitHub issue 149: on RTX 40, frame generation is held at 2X while the DLSS-G provider lacks the multi-frame
// count/index check we restore (DLSS-G before 310.7), because counts above 1 freeze it. A game can ship an old,
// unpatchable provider beside NVIDIA's OTA one that NGX runs (Indiana Jones), so the last inspection must not decide:
// - the provider a frame-generation Create uses decides, and only another Create changes it;
// - before any Create, a provider without the check holds 2X only while no supported provider with it has been
//   inspected (one with it is what NGX can pick).
enum class Source
{
    eInspection, // a loaded provider was inspected (PatchNgxMfgGate)
    eCreate,     // a frame-generation Create is about to use this provider
};

enum class Change
{
    eNone,
    eHeld,   // frame generation is now held at 2X
    eLifted, // the hold is lifted
};

class Hold
{
public:
    // `missing`: the provider is supported but has no check to restore; false: it has the check (found or restored).
    Change Note(bool missing, Source source) noexcept
    {
        std::lock_guard lock(lock_);
        if (source == Source::eInspection)
        {
            if (!missing) gatedSeen_ = true;
            if (fromCreate_ || (missing && gatedSeen_)) return Change::eNone;
        }
        else
            fromCreate_ = true;
        const bool was = held_.exchange(missing, std::memory_order_acq_rel);
        return was == missing ? Change::eNone : missing ? Change::eHeld : Change::eLifted;
    }

    // Read on every DLSS-G state and options call: lock-free.
    bool Held() const noexcept { return held_.load(std::memory_order_acquire); }

private:
    std::mutex lock_;
    bool fromCreate_{}, gatedSeen_{}; // under lock_
    std::atomic<bool> held_{false};
};
}
