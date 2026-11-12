// ngxprov.1: the provider API and its refusal (ngx_provider.h).
#include "ngx_provider.h"
#include "ngx_provider_policy.h"
#include "d3d12_provider.h"
#include "xess_mod.h"
#include "xemfg_host_common.h"
#include "vulkan/vk_provider.h"
#include "vulkan/xefg_internal.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace ngx_provider
{
namespace
{
using xemfg_host::Escape, xemfg_host::Log;

// Written once, before gRefused publishes it.
constexpr size_t kReasonSize = 96;
char gReason[kReasonSize]{};
std::atomic<bool> gWriting{false}, gRefused{false}, gMaxLogged{false}, gOffered{false};

void CopyOut(const char* text, char* reason, uint32_t capacity) noexcept
{
    if (!reason || !capacity) return;
    const size_t length = strnlen(text, capacity - 1);
    std::memcpy(reason, text, length);
    reason[length] = '\0';
}

// The XeFG image of the pair the SDK route drives (OptiScaler's, once it loaded one). ActiveSdkPair reads published
// owners and Resolve reads the loaded image: no lock, no load, so it is safe under the loader lock.
bool PairRefused(char* reason, uint32_t capacity) noexcept
{
    try {
        HMODULE fg{}, ll{};
        if (!xess_mod::ActiveSdkPair(fg, ll)) return false;
        xefg_internal::Api api;
        std::string why;
        if (xefg_internal::Resolve(fg, api, why)) return false;
        const std::string text = "xefg_" + why;
        CopyOut(text.c_str(), reason, capacity);
        return true;
    } catch (...) {
        return false;
    }
}

uint32_t RefusalEntry(char* reason, uint32_t capacity) { return Refused(reason, capacity); }
uint32_t MaxGeneratedEntry() { return MaxGeneratedFrames(); }

const MfgXeMFGProviderApi kApi{
    sizeof(MfgXeMFGProviderApi),
    MFG_XEMFG_PROVIDER_API_VERSION,
    xess_vulkan_provider::VulkanInit,
    xess_vulkan_provider::VulkanCreateFeature,
    xess_vulkan_provider::VulkanEvaluate,
    xess_vulkan_provider::ReleaseFeature,
    xess_vulkan_provider::Shutdown,
    MaxGeneratedEntry,
    RefusalEntry,
    xess_d3d12_provider::Init,
    xess_d3d12_provider::CreateFeature,
    xess_d3d12_provider::Evaluate,
    xess_d3d12_provider::ReleaseFeature,
    xess_d3d12_provider::Shutdown,
};
static_assert(offsetof(MfgXeMFGProviderApi, Refusal) == MFG_XEMFG_PROVIDER_API_SIZE_VULKAN);
}

void NoteRefusal(const std::string& reason) noexcept
{
    if (gRefused.load(std::memory_order_acquire) || gWriting.exchange(true)) return;
    try {
        const char* text = reason.empty() ? "refused" : reason.c_str();
        CopyOut(text, gReason, kReasonSize);
        gRefused.store(true, std::memory_order_release);
        Log("ngx_provider_unavailable", "\"reason\":\"" + Escape(gReason) + "\"");
    } catch (...) {
        gRefused.store(true, std::memory_order_release);
    }
}

uint32_t Refused(char* reason, uint32_t capacity) noexcept
{
    if (gRefused.load(std::memory_order_acquire)) {
        CopyOut(gReason, reason, capacity);
        return 1;
    }
    if (PairRefused(reason, capacity)) return 1;
    CopyOut("", reason, capacity);
    return 0;
}

uint32_t MaxGeneratedFrames() noexcept
{
    char reason[kReasonSize]{};
    const bool refused = Refused(reason, kReasonSize) != 0;
    if (!gMaxLogged.exchange(true))
        Log("ngx_provider_max", std::string("\"generated\":") + (refused ? "0" : std::to_string(kMaxGenerated)) +
            (refused ? ",\"reason\":\"" + Escape(reason) + "\"" : std::string()));
    if (!refused) gOffered.store(true, std::memory_order_release);
    return refused ? 0 : kMaxGenerated;
}

bool Offered() noexcept
{
    return gOffered.load(std::memory_order_acquire) && !gRefused.load(std::memory_order_acquire);
}

bool WholeBatch() noexcept
{
    static const bool whole = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"RTXMFG_PROVIDER_WHOLE_BATCH", value, DWORD(std::size(value)));
        return length == 1 && value[0] == L'1';
    }();
    return whole;
}

const MfgXeMFGProviderApi* GetApi(uint32_t version) noexcept
{
    return version == MFG_XEMFG_PROVIDER_API_VERSION ? &kApi : nullptr;
}
}

// vkprov.1: a beta's provider shim (mfgunlocker_xemfg.dll) finds this export in the process. NULL for another version.
extern "C" __declspec(dllexport) const MfgXeMFGProviderApi* MfgXeMFGProviderGetApi(uint32_t version)
{
    return ngx_provider::GetApi(version);
}
