// RTX 50 (Blackwell) selection and menu policy: adapter identity, family
// selection, Vulkan admission, Streamline OTA, menu family and Dynamic MFG.
#include "blackwell_gpu.h"
#include "gpu_selection_policy.h"
#include "nvidia_mfg_policy.h"
#include "streamline_ota_policy.h"
#include "ui_dynamic_mfg.h"
#include "xess_ui_state.h"

#include <cstdio>
#include <stdexcept>

namespace
{
LUID Luid(DWORD low, LONG high = 0) noexcept
{
    LUID luid{};
    luid.LowPart = low;
    luid.HighPart = high;
    return luid;
}

blackwell_gpu::CudaDevice Device(DWORD low, int major, int minor) noexcept
{
    return {Luid(low), major, minor};
}
}

int main()
{
    unsigned n = 0;
    auto check = [&](bool passed, const char* why) { ++n; if (!passed) throw std::runtime_error(why); };
    try
    {
        using blackwell_gpu::CudaDevice;
        using blackwell_gpu::MatchAdapter;
        int major = 0, minor = 0;
        const CudaDevice rtx5090[]{Device(42, 12, 0)};
        check(MatchAdapter(Luid(42), rtx5090, 1, major, minor) && major == 12 && minor == 0,
            "an RTX 50 (CUDA 12.0) adapter is identified by its LUID");
        const CudaDevice gb10[]{Device(42, 12, 1)};
        check(MatchAdapter(Luid(42), gb10, 1, major, minor), "any CUDA 12.x (sm_120 family) part is Blackwell RTX");
        const CudaDevice ada[]{Device(42, 8, 9)}, ampere[]{Device(42, 8, 6)}, turing[]{Device(42, 7, 5)};
        check(!MatchAdapter(Luid(42), ada, 1, major, minor) && major == 8 && minor == 9, "RTX 40 is not RTX 50");
        check(!MatchAdapter(Luid(42), ampere, 1, major, minor) && !MatchAdapter(Luid(42), turing, 1, major, minor),
            "RTX 30 and RTX 20 are not RTX 50");
        const CudaDevice datacenter[]{Device(42, 10, 0)}, future[]{Device(42, 13, 0)};
        check(!MatchAdapter(Luid(42), datacenter, 1, major, minor), "data-center Blackwell (10.0) has no DLSS-G");
        check(!MatchAdapter(Luid(42), future, 1, major, minor), "an unknown later architecture fails closed");
        const CudaDevice mixed[]{Device(7, 8, 9), Device(42, 12, 0)};
        check(MatchAdapter(Luid(42), mixed, 2, major, minor) && !MatchAdapter(Luid(7), mixed, 2, major, minor),
            "with an RTX 40 and an RTX 50 installed, only the RTX 50's LUID is RTX 50");
        check(!MatchAdapter(Luid(43), rtx5090, 1, major, minor) && major == 0, "another adapter's LUID does not match");
        const CudaDevice duplicate[]{Device(42, 12, 0), Device(42, 12, 0)};
        check(!MatchAdapter(Luid(42), duplicate, 2, major, minor), "a LUID on two CUDA devices fails closed");
        check(!MatchAdapter(Luid(0), rtx5090, 1, major, minor) && !MatchAdapter(Luid(42), nullptr, 0, major, minor),
            "a zero LUID or an empty enumeration fails closed");
        check(Luid(42, 1).HighPart == 1 && !MatchAdapter(Luid(42, 1), rtx5090, 1, major, minor),
            "both LUID halves must match");

        using gpu_dispatch::Family;
        gpu_dispatch::Selection selection;
        check(selection.Observe(Family::eBlackwell, 42) && selection.family == Family::eBlackwell,
            "RTX 50 is a selectable GPU family");
        check(selection.Observe(Family::eBlackwell, 42), "re-observing the same RTX 50 keeps the selection");
        check(!selection.Observe(Family::eAda, 42) && selection.family == Family::eConflict,
            "RTX 50 then RTX 40 on one adapter is a conflict");
        gpu_dispatch::Selection other;
        check(other.Observe(Family::eBlackwell, 42) && !other.Observe(Family::eBlackwell, 43)
            && other.family == Family::eConflict, "a second adapter after RTX 50 is a conflict");
        check(static_cast<uint32_t>(Family::eBlackwell) == 5, "RTX 50 is family 5 in the status file");

        using gpu_dispatch::AllowVulkanCreate;
        using gpu_dispatch::AllowVulkanEvaluate;
        check(AllowVulkanCreate(Family::eBlackwell, true, false) && AllowVulkanCreate(Family::eBlackwell, true, true),
            "native RTX 50 Vulkan FG is never blocked");
        check(AllowVulkanEvaluate(Family::eBlackwell, true, false), "native RTX 50 Vulkan FG Evaluate passes");
        check(!AllowVulkanCreate(Family::eAda, true, false) && !AllowVulkanEvaluate(Family::eConflict, false, true),
            "RTX 40 keeps its adapter gate and a conflict still blocks");

        using namespace streamline_ota_policy;
        const auto selected = Apply(0, true, true, true, Gpu::eBlackwell, Gpu::eUnknown);
        check(selected.reason == Reason::eSelectedBlackwell && selected.allowOtaForced
            && selected.loadDownloadedPluginsForced, "an official 6X title loads the coherent OTA set on RTX 50");
        check(std::string(ReasonName(Reason::eSelectedBlackwell)) == "selected-blackwell", "OTA reason name");
        check(Apply(0, false, true, true, Gpu::eBlackwell, Gpu::eUnknown).reason == Reason::eNotListed
            && !Apply(0, false, true, true, Gpu::eBlackwell, Gpu::eUnknown).allowOtaForced, "unlisted titles unchanged");
        check(!Apply(0, true, false, true, Gpu::eBlackwell, Gpu::eUnknown).allowOtaForced
            && !Apply(0, true, true, false, Gpu::eBlackwell, Gpu::eUnknown).allowOtaForced,
            "RTX 50 keeps the host and loader requirements");
        check(InventoryGpu(true, 1, 0x1B0, false) == Gpu::eUnknown, "RTX 50 is never selected from NVAPI early");

        check(xess_ui::DlssMfgFamily(5) && !xess_ui::DlssMfgFamily(3) && !xess_ui::DlssMfgFamily(0)
            && !xess_ui::DlssMfgFamily(6), "the menu offers DLSS controls on RTX 50, not on unknown values");
        xess_ui::State xess{};
        xess.available = TRUE;
        xess.hooksActive = TRUE;
        xess.gpuVendorId = xess_ui::kNvidiaVendor;
        check(xess_ui::SettingsSection(xess, "", true, 5) == xess_ui::Section::Dlss,
            "an RTX 50 game offering DLSS-G opens on the DLSS page");
        check(xess_ui::SettingsSection(xess, "", true, 0) == xess_ui::Section::Xess,
            "an unidentified NVIDIA GPU keeps the XeSS page");

        // portstatus.1: OptiScaler's MFG Unlocker tab names the XMX port or the CUDA port.
        using xess_ui::FgPort;
        using xess_ui::PortTensorCores;
        xess_ui::State port = xess;
        port.modelActive = 1;
        check(xess_ui::ActivePort(port) == FgPort::None, "no XeFG card without OptiScaler");
        port.optiScaler = TRUE;
        port.available = FALSE;
        check(xess_ui::ActivePort(port) == FgPort::None, "no XeFG card without the XeSS backend");
        port.available = TRUE;
        port.modelActive = 0;
        check(xess_ui::ActivePort(port) == FgPort::None, "no XeFG card before XeFG picks a model");
        port.modelActive = 2;
        port.tensorCores = 1;
        port.tensorCoreLaunches = 9;
        check(xess_ui::ActivePort(port) == FgPort::Intel2x
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::None, "Intel's 2x model has no tensor-core note");
        port.modelActive = 1;
        check(xess_ui::ActivePort(port) == FgPort::CudaPort
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::None, "a launched layer makes it the CUDA port");
        port.tensorCoreLaunches = 0;
        check(xess_ui::ActivePort(port) == FgPort::XmxPort
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::Ready,
            "a ready route without a launch is still the XMX port");
        port.tensorCores = 3;
        port.tensorCoreLaunches = 4;
        check(xess_ui::ActivePort(port) == FgPort::XmxPort
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::OffSetting,
            "XeFGTensorCores = off keeps the XMX port, launches or not");
        port.tensorCores = 2;
        check(xess_ui::PortTensorCoreNote(port) == PortTensorCores::Unavailable,
            "an NVIDIA GPU without the route says why");
        port.tensorCores = 0;
        check(xess_ui::ActivePort(port) == FgPort::XmxPort
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::None, "an undecided route has no note yet");
        port.tensorCores = 2;
        port.gpuVendorId = 0;
        port.adapterVendor = 0x1002;
        check(xess_ui::ActivePort(port) == FgPort::XmxPort
            && xess_ui::PortTensorCoreNote(port) == PortTensorCores::None, "other vendors run the XMX port without a note");

        MfgUnlockReShadeSnapshot state{};
        state.gpuFamily = 5;
        state.safeMaximumMultiplier = 4;
        state.numFramesToGenerateMax = 3;
        state.dynamicMfgSupportKnown = TRUE;
        state.dynamicMfgSupported = TRUE;
        check(ui_dynamic_mfg::Classify(state) == ui_dynamic_mfg::Availability::Supported,
            "RTX 50 Dynamic at 4x has no RTX 30 full-capacity restriction");
        state.numFramesToGenerateMax = 5;
        check(ui_dynamic_mfg::Classify(state) == ui_dynamic_mfg::Availability::Unavailable,
            "a 6x Dynamic range still cannot fit a 4x cap");

        using namespace nvidia_mfg_policy;
        // RTX 50's wrapper clamp is native: the unpatched compiled maximum is its capacity.
        check(DecideCapacity(Tier::eUnknown, true, 3).effectiveMaximumMultiplier == 4
            && DecideCapacity(Tier::eSixX, true, 5).effectiveMaximumMultiplier == 6
            && DecideCapacity(Tier::eNoMfgOverride, true, 5).effectiveMaximumMultiplier == 2,
            "RTX 50 capacity follows the compiled maximum and NVIDIA's per-title ceiling");
        check(DecideCapacity(Tier::eSixX, false, 5).effectiveMaximumMultiplier == 2,
            "a wrapper with foreign bytes at the clamp offers no MFG override");

        std::printf("BLACKWELL_POLICY_PASS checks=%u\n", n);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL %s after %u checks\n", error.what(), n);
        return 1;
    }
}
