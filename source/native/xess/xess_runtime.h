#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace xess_runtime
{
// Original baseline for fixtures and provenance. runtime_layout.h admits compatible
// loaded instruction families independently of this digest. The mod never ships
// these DLLs: it uses the pair the process loaded (under OptiScaler, the copy its
// XeFG output uses). Anything else keeps Intel's behaviour (one generated frame on
// non-Intel GPUs).
struct Profile
{
    const char* label;
    const char* fgSha256;
    const char* llSha256;
    const char* fgVersion;
    const char* llVersion;
};
inline constexpr Profile kSdk302{
    "XeSS SDK 3.0.2",
    "ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27",
    "d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7",
    "1.3.1.78", "1.3.2.10"};

enum class Source : uint32_t
{
    eNone = 0,     // No profiled pair: Intel's provider stays in use.
    eGameCopy = 1, // The loaded pair has a verified compatible instruction layout.
};
inline const char* SourceName(Source source) noexcept
{
    return source == Source::eGameCopy ? "game" : "none";
}

std::filesystem::path ModulePath(HMODULE module);
}
