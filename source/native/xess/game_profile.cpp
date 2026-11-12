#include "game_profile.h"
#include "frame_time_profile.h"
#include "port/port_support.h"
#include <mutex>
namespace xess_mod {
namespace {
frame_time_profile::Profile FrameTimeExecutableProfile() noexcept
{
    static std::once_flag once;
    static auto matched = frame_time_profile::Profile::eUnknown;
    std::call_once(once, [] {
        try {
            std::wstring path(32768,L'\0');
            const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!n || n >= path.size()) return;
            path.resize(n);
            const auto* leaf = wcsrchr(path.c_str(), L'\\');
            const std::wstring_view name{leaf ? leaf+1 : path.c_str()};
            // Avoid hashing other games; candidate names do not admit a
            // profile until the exact executable digest has been verified.
            if (!frame_time_profile::EqualAsciiInsensitive(name, std::wstring_view{L"Cyberpunk2077.exe"})
                && !frame_time_profile::EqualAsciiInsensitive(name, std::wstring_view{L"witcher3.exe"})) return;
            matched = frame_time_profile::MatchExecutableIdentity(name, sha256(path));
        } catch (...) { matched = frame_time_profile::Profile::eUnknown; }
    });
    return matched;
}
}
bool CyberpunkProfile() noexcept
{
    return FrameTimeExecutableProfile() == frame_time_profile::Profile::eCyberpunkSeconds;
}
bool WitcherProfile() noexcept
{
    return FrameTimeExecutableProfile() == frame_time_profile::Profile::eWitcher3Seconds;
}
void LogSdkModule(void* value, const char* kind) noexcept
{
    try {
        const auto module = static_cast<HMODULE>(value);
        std::wstring path(32768,L'\0');
        const auto n = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        if (!n || n >= path.size()) return;
        path.resize(n);
        uint16_t major{},minor{},patch{};
        if (!strcmp(kind,"fg") || !strcmp(kind,"intel_native_fg")) {
            const auto get = reinterpret_cast<decltype(&xefgSwapChainGetVersion)>(GetProcAddress(module,"xefgSwapChainGetVersion"));
            xefg_swapchain_version_t v{}; if (!get || get(&v)!=0) return;
            major=v.major; minor=v.minor; patch=v.patch;
        } else {
            const auto get = reinterpret_cast<decltype(&xellGetVersion)>(GetProcAddress(module,"xellGetVersion"));
            xell_version_t v{}; if (!get || get(&v)!=0) return;
            major=v.major; minor=v.minor; patch=v.patch;
        }
        WriteLog("native_sdk_module", "\"kind\":\""+std::string(kind)+"\",\"path\":\""+escape(utf8(path))+
            "\",\"sha256\":\""+sha256(path)+"\",\"api_version\":\""+std::to_string(major)+"."+
            std::to_string(minor)+"."+std::to_string(patch)+"\"");
    } catch (...) { WriteLog("native_sdk_module", "\"identity_log_unavailable\":true"); }
}
namespace {
void LogModuleIdentity(const char* stage, void* value, const std::string& fields, bool withHash) noexcept
{
    try {
        std::wstring path(32768,L'\0');
        const auto n = GetModuleFileNameW(static_cast<HMODULE>(value),path.data(),static_cast<DWORD>(path.size()));
        path.resize(n && n < path.size() ? n : 0);
        std::string line = fields+",\"path\":\""+escape(utf8(path))+"\"";
        if (withHash) {
            std::string hash = "unavailable";
            if (!path.empty()) try { hash = sha256(path); } catch (...) {}
            line += ",\"sha256\":\""+hash+"\"";
        }
        WriteLog(stage, line);
    } catch (...) {}
}
}
void LogSdkCandidateRejected(void* value, const std::string& fields) noexcept
{
    LogModuleIdentity("sdk_candidate_rejected", value, fields, true);
}
void LogSdkModuleSeen(void* value, const std::string& fields, bool withHash) noexcept
{
    LogModuleIdentity("sdk_module_seen", value, fields, withHash);
}
}
