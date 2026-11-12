#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
namespace intel_native
{
// OptiScaler owns Dynamic and context lifetime. Intel keeps its native model
// and pacing. Only proven cold contexts admit five generated frames.
inline constexpr uint8_t kStockGenerated=3, kGenerated=5;
struct CapacitySite
{
    uint32_t immediate{}, config{};
    uint32_t flagOffset{0x188}, countOffset{0x18c};
    uint32_t forwardProperties{}, forwardCount{}, coreTable{}, forwardParameters{};
};
bool FindCapacity(const uint8_t*, CapacitySite&, std::string& failure) noexcept;
bool FindXellGate(const uint8_t*, uint32_t& immediate, std::string& failure) noexcept;
bool Raise(HMODULE, uint32_t rva, uint8_t before, uint8_t after, std::string& failure) noexcept;
struct Receipt
{
    HMODULE fg{}, ll{};
    uint32_t fgRva{}, llRva{}, maximum{}, capacity{}, active{}, pending{};
    // The XeLL the native FG binds ("libxell"/"igxell"), its
    // libxell version, and whether that module has a count gate to raise.
    const char* llBinding{""};
    bool llGated{}, igxellLoaded{};
    uint16_t llMajor{}, llMinor{}, llPatch{};
    std::wstring llPath;
};
// The XeLL module native SetLatencyReduction binds, by the
// driver's own rule (igxess_fg 1.3.3.93): libxell.dll must be loaded; when
// libxell is above 1.0.0 and igxell.dll is already loaded, the driver unwraps
// libxell's context into igxell's and binds igxell, otherwise it binds libxell
// with the caller's context. Loading igxell ourselves flips a built-in libxell
// context onto igxell (a crash), so nothing is ever loaded here. A
// libxell binding must be the route's verified libxell. Caller FreeLibrary's
// the successful receipt.ll reference; failure owns no reference.
bool SelectXeLL(HMODULE nativeFg, HMODULE routeLl, Receipt&, std::string& failure) noexcept;
// Read-only: the binding the driver would choose now ("libxell", "igxell",
// "none" without libxell, "unknown" without its version), and the published one.
const char* DriverXellBinding() noexcept;
const char* PublishedXellBinding() noexcept;
// After native Create, before the first host properties query and Init.
bool Prepare(HMODULE shim, HMODULE routeLl, void* handle, Receipt&, std::string& failure) noexcept;
// Reads genuine allocation/counts after Init; never manufactures values.
bool Readback(HMODULE shim, void* handle, Receipt&, std::string& failure) noexcept;
// Read-only ownership/forwarding qualification; makes no provider API calls.
bool Qualify(HMODULE shim, void* handle, Receipt&, std::string& failure) noexcept;
}
