#pragma once
#include <string>
namespace xess_mod {
bool CyberpunkProfile() noexcept;
bool WitcherProfile() noexcept;
void LogSdkModule(void* module, const char* kind) noexcept;
// rejectlog.1: sdk_candidate_rejected with `fields` (role, reason, export, api_version or status) plus the module's
// path and file hash ("unavailable" when it can't be read).
void LogSdkCandidateRejected(void* module, const std::string& fields) noexcept;
// sdkscan.1: sdk_module_seen with `fields` plus the module's path (and file hash when `withHash`), for each
// libxess_fg* / libxell* module the SDK discovery finds, whatever it decides about it.
void LogSdkModuleSeen(void* module, const std::string& fields, bool withHash) noexcept;
}
