#pragma once

#include <cstdint>

// RTX 30 startup for OptiScaler's private Streamline (its DLSS-G output).
// The Ampere route normally starts at the game's own slInit (early boundary)
// and slSetD3DDevice (capability bootstrap and startup admission). The private
// sl.dlss_g caches its maximum at its own plug-in startup, which OptiScaler runs
// from its D3D12CreateDevice hook, before the game's slSetD3DDevice. So the
// private slSetD3DDevice carries a startup of its own: the whole startup when
// the game has no Streamline, or a second bracket beside the game's before the
// first FG Create. Either way only when its boundary is as early as the game's
// would be. Pure decision; tested in tests/ampere_runtime.
namespace optiscaler_private_streamline
{
struct Inputs
{
    // An earlier private call already took the RTX 30 startup.
    bool admittedBefore = false;
    // The game's own interposer is bound or its slInit ran (the early boundary).
    bool gameStreamline = false;
    // No RTX 30 FG Create has been attempted yet. Beside the game's Streamline,
    // a bracket after one could only refuse its bootstrap, fatally.
    bool beforeFirstCreate = false;
    // The private slSetD3DDevice was hooked when its interposer mapped, before
    // OptiScaler could call its slInit...
    bool hookedAtLoad = false;
    // ...and the RTX 30 declaration was armed then, so the DLSS-G provider its
    // slInit mapped was declared as it mapped.
    bool armedAtLoad = false;
    // This call's device selected the verified RTX 30 adapter.
    bool ampereDevice = false;
};

enum class Decision : uint8_t
{
    eAdmit,
    // Same as the game's repeated slSetD3DDevice: through the startup again.
    eRepeat,
    // The game's Streamline exists and FG was already created there.
    eRefuseGameStreamline,
    eRefuseLate,
    eRefuseDevice,
};

// The device is observed only when it can change the decision, so a refused
// boundary selects nothing it would not have selected before.
constexpr bool NeedsDevice(const Inputs& in) noexcept
{
    return in.admittedBefore
        || ((!in.gameStreamline || in.beforeFirstCreate) && in.hookedAtLoad && in.armedAtLoad);
}

constexpr Decision Decide(const Inputs& in) noexcept
{
    if (in.admittedBefore) return Decision::eRepeat;
    if (in.gameStreamline && !in.beforeFirstCreate) return Decision::eRefuseGameStreamline;
    if (!in.hookedAtLoad || !in.armedAtLoad) return Decision::eRefuseLate;
    if (!in.ampereDevice) return Decision::eRefuseDevice;
    return Decision::eAdmit;
}

// Startup runs for an admitted boundary, and again for a repeat on the RTX 30 device.
constexpr bool RunsStartup(Decision decision, bool ampereDevice) noexcept
{
    return decision == Decision::eAdmit || (decision == Decision::eRepeat && ampereDevice);
}

constexpr const char* DecisionName(Decision decision) noexcept
{
    switch (decision)
    {
    case Decision::eAdmit: return "admit";
    case Decision::eRepeat: return "repeat";
    case Decision::eRefuseGameStreamline: return "game-streamline";
    case Decision::eRefuseLate: return "late-boundary";
    case Decision::eRefuseDevice: return "not-rtx30-device";
    }
    return "unknown";
}
}
