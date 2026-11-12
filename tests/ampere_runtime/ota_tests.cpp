#include "streamline_ota_policy.h"
#include "streamline_state_policy.h"
#include "ampere_provider_arch.h"
#include "nvidia_mfg_policy.h"
#include <iostream>
using namespace streamline_ota_policy;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{ ++checks; failures += !ok; std::cout << (ok ? "PASS " : "FAIL ") << text << '\n'; }
int main()
{
    constexpr uint64_t original = 0x201;
    const auto batman = nvidia_mfg_policy::FindTier(L"LEGO Batman: Legacy of the Dark Knight");
    Check(batman == nvidia_mfg_policy::Tier::eSixX, "LEGO Batman resolves through the existing NVIDIA 6x manifest");
    const auto fallback = nvidia_mfg_policy::DecideCapacity(batman, true, 1);
    Check(fallback.effectiveMaximumMultiplier == 2 && fallback.fallback,
        "a listed 6x title stays at 2x on an audited legacy wrapper");
    const auto inventory = [](bool detected, uint32_t count, uint32_t architecture, uint32_t implementation) {
        return InventoryGpu(detected, count, architecture,
            ampere_provider_arch::IsAmpereRtx(architecture, implementation));
    };
    Check(inventory(true, 1, 0x170, 4) == Gpu::eAmpere, "trusted single RTX 30 inventory is usable before device selection");
    Check(inventory(true, 1, 0x190, 2) == Gpu::eAda, "trusted single Ada inventory is usable before device selection");
    Check(inventory(false, 1, 0x170, 4) == Gpu::eUnknown, "failed discovery grants no OTA evidence");
    Check(inventory(true, 0, 0x170, 4) == Gpu::eUnknown
        && inventory(true, 2, 0x170, 4) == Gpu::eUnknown, "absent or ambiguous NVIDIA inventory stays unknown");
    Check(inventory(true, 1, 0x170, 0) == Gpu::eUnknown
        && inventory(true, 1, 0x170, 11) == Gpu::eUnknown, "GA100 and Tegra do not enable the RTX route");
    Check(inventory(true, 1, 0x180, 4) == Gpu::eUnknown, "unrecognized architecture stays unknown");
    for (Gpu gpu : {Gpu::eAda, Gpu::eAmpere})
    {
        const auto early = Apply(original, true, true, true, Gpu::eUnknown, gpu);
        Check(early.flags == (original | kAllowOta | kLoadDownloadedPlugins)
            && early.allowOtaForced && early.loadDownloadedPluginsForced
            && early.fullOtaRequested && !early.fullOtaSuppressed,
            "official 6x profile requests the coherent downloaded set before slSetD3DDevice");
        const auto selected = Apply(original, true, true, true, gpu, Gpu::eUnknown);
        Check(selected.flags == early.flags, "established rendering family also enables the full set");
        for (unsigned fault = 0; fault < 4; ++fault)
        {
            const auto denied = Apply(original, fault != 0, fault != 1, fault != 2,
                Gpu::eUnknown, gpu, fault == 3);
            Check(denied.flags == original && !denied.allowOtaForced && !denied.loadDownloadedPluginsForced,
                "unlisted title, incompatible host, missing loader or native diagnostic pin preserves original flags");
        }
        const auto already = Apply(original | kAllowOta | kLoadDownloadedPlugins, true, true, true, Gpu::eUnknown, gpu);
        Check(!already.allowOtaForced && !already.loadDownloadedPluginsForced
            && !already.fullOtaSuppressed, "already enabled native flags remain accurately reported");
    }
    const auto unknown = Apply(original, true, true, true, Gpu::eUnknown, Gpu::eUnknown);
    Check(unknown.flags == original && unknown.fullOtaRequested && unknown.fullOtaSuppressed
        && unknown.reason == Reason::eUnknownGpu, "unknown GPU logs the pending request without forcing plugins");
    const auto conflict = Apply(original, true, true, true, Gpu::eConflict, Gpu::eAmpere);
    Check(conflict.flags == original && conflict.reason == Reason::eConflict,
        "authoritative adapter conflict overrides early RTX inventory");
    const auto pinned = Apply(original | kAllowOta, true, true, true, Gpu::eAmpere, Gpu::eUnknown, true);
    Check(pinned.flags == (original | kAllowOta) && pinned.reason == Reason::eNativePinned,
        "native Streamline diagnostic pin does not remove host flags or alter DLSS-G selection");
    {
        // GitHub issue 137: Pragmata's slInit flags 0xA9 (state tracking off, no manual hooking).
        using StateReason = streamline_state_policy::Reason;
        using streamline_state_policy::kDisableClStateTracking;
        using streamline_state_policy::kUseManualHooking;
        constexpr uint64_t pragmata = 0xA9;
        const auto restored = streamline_state_policy::Apply(pragmata, true, 2);
        Check(restored.flags == 0xA8 && restored.reason == StateReason::eRestored,
            "DLSS 5 on a tracking-off proxy host turns Streamline's state tracking back on and nothing else");
        const auto noDlss5 = streamline_state_policy::Apply(pragmata, false, 2);
        Check(noDlss5.flags == pragmata && noDlss5.reason == StateReason::eNoDlss5,
            "without DLSS 5 the host's flags stay as they are");
        constexpr uint64_t manual = kDisableClStateTracking | kUseManualHooking;
        const auto manualHost = streamline_state_policy::Apply(manual, true, 2);
        Check(manualHost.flags == manual && manualHost.reason == StateReason::eManualHooking,
            "a manual-hooking host keeps its flags (Streamline restores nothing for it)");
        const auto tracking = streamline_state_policy::Apply(0xA8, true, 2);
        Check(tracking.flags == 0xA8 && tracking.reason == StateReason::eHostTracks, "a host that tracks already is unchanged");
        Check(streamline_state_policy::Apply(pragmata, true, 0).flags == pragmata
            && streamline_state_policy::Apply(pragmata, true, 1).reason == StateReason::eHostVersion,
            "an unknown or non-2.x interposer keeps its flags");
        const auto ota = Apply(pragmata, true, true, true, Gpu::eAda, Gpu::eUnknown);
        Check(streamline_state_policy::Apply(ota.flags, true, 2).flags == (ota.flags & ~kDisableClStateTracking),
            "the state-tracking change composes with the full OTA flags");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
