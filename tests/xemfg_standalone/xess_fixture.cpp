// Test fixture: a stand-in libxess with the public D3D12 entry points XeMFG standalone hooks
// (source/native/xess/d3d12_host.cpp DiscoverUpscalers). Execute records nothing itself: the harness checks what the
// host tagged for XeFG before forwarding here.
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace
{
std::atomic<uint32_t> gCalls{0};
std::atomic<const void*> gLast{nullptr};
}

extern "C"
{
struct FixtureVersion { uint16_t major, minor, patch, reserved; };

__declspec(dllexport) __declspec(noinline) int32_t xessGetVersion(FixtureVersion* version)
{
    if (!version) return -4;
    version->major = 2;
    version->minor = 0;
    version->patch = 2;
    version->reserved = 0;
    return 0;
}

__declspec(dllexport) __declspec(noinline) int32_t xessD3D12Execute(void* context, void* list, const void* params)
{
    if (!context || !list || !params) return -4;
    gLast.store(params, std::memory_order_relaxed);
    gCalls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// xess_d3d12_init_params_t's head: output resolution, quality, init flags (INVERTED_DEPTH).
__declspec(dllexport) __declspec(noinline) int32_t xessD3D12GetInitParams(void* context, void* params)
{
    if (!context || !params) return -4;
    auto* fields = static_cast<uint32_t*>(params);
    fields[0] = 640;
    fields[1] = 480;
    fields[2] = 103;
    fields[3] = 1u << 1;
    return 0;
}

__declspec(dllexport) __declspec(noinline) int32_t xessGetVelocityScale(void* context, float* x, float* y)
{
    if (!context || !x || !y) return -4;
    *x = 1.0f;
    *y = 1.0f;
    return 0;
}

__declspec(dllexport) uint32_t FixtureExecuteCalls() { return gCalls.load(std::memory_order_relaxed); }
}
