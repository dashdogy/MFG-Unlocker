// RtxHairProbe: universal RTX hair on D3D12 (rtxhair.1), phases 0, 1a and 1b.
//
// Universal RTX hair needs a hit shader to recover an LSS segment's endpoints and radii without
// game-specific buffers. This probe asks whether NVAPI triangle position fetch (BLAS flag
// ALLOW_DATA_ACCESS_EX; shader ops 86 and 98) works on this GPU, and whether a DOTS segment
// decodes back to its endpoints from the hit triangle alone.
//
// RtxHairProbe [--out <report.json>] [--adapter <index>] [--seed <n>]
//              [--nvapi <dir>] [--dxc <dir>] [--hlsl <probe.hlsl>]
//
// Scene: synthetic LSS strands (tapered to a zero tip, constant, thinning, growing) converted
// to DOTS on the CPU; four BLASes in one TLAS, each reached by its own instance mask:
//   0x01 DOTS, tagged      NVAPI build with ALLOW_DATA_ACCESS_EX; 3 low mantissa bits of every
//                          coordinate set to the tag (the candidate "is LSS" marker)
//   0x08 DOTS, plain       same, untagged (decode error without the tag)
//   0x04 mesh, data access a game-like indexed grid, NVAPI build with ALLOW_DATA_ACCESS_EX
//   0x02 mesh, plain       the same grid, plain D3D12 build (what a game's own BLAS is):
//                          reports what position fetch returns there (informational)
// Rays are traced twice: inline RayQuery in a compute shader (committed op 98) and a DXR
// pipeline's closest hit (op 86).
//
// Phase 1a: probe_lss.hlsl, a hair shader written for RTX 50 (NVAPI LSS ops), rewritten by
// source/native/rtx_hair (lss_ir / lss_dxil) and traced on the same scene both ways: LSS hits
// on the tagged DOTS, plain triangle hits everywhere else (see the phase 1a checks).
//
// Exit codes: 0 pass; 1 a check failed (the report says which); 2 setup failure;
// 3 position fetch unavailable (op unsupported, or neither shader could be created).
#include "probe_paths.h"
#include "rtx_hair/dots_builder.h"
#include "rtx_hair/instance_patch.h"
#include "rtx_hair/lss_dxil.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <nvapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// source/native/rtx_hair/nvapi_lss.h declares NVAPI's ABI itself: it must match nvapi.h.
namespace rtxnv = rtx_hair::nvapi;
static_assert(sizeof(rtxnv::GeometryDescEx) == sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX));
static_assert(offsetof(rtxnv::GeometryDescEx, lss) == offsetof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX, lss));
static_assert(sizeof(rtxnv::LssDesc) == sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_LSS_DESC));
static_assert(offsetof(rtxnv::LssDesc, vertexRadiusBuffer) == offsetof(NVAPI_D3D12_RAYTRACING_GEOMETRY_LSS_DESC, vertexRadiusBuffer));
static_assert(offsetof(rtxnv::LssDesc, indexBuffer) == offsetof(NVAPI_D3D12_RAYTRACING_GEOMETRY_LSS_DESC, indexBuffer));
static_assert(offsetof(rtxnv::LssDesc, primitiveFormat) == offsetof(NVAPI_D3D12_RAYTRACING_GEOMETRY_LSS_DESC, primitiveFormat));
static_assert(sizeof(rtxnv::BuildInputsEx) == sizeof(NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX));
static_assert(sizeof(rtxnv::BuildDescEx) == sizeof(NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX));
static_assert(sizeof(rtxnv::PrebuildParams) == sizeof(NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_V1));
static_assert(sizeof(rtxnv::BuildParams) == sizeof(NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_V1));
static_assert(rtxnv::kPrebuildParamsVersion == NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER);
static_assert(rtxnv::kBuildParamsVersion == NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER);
static_assert(rtxnv::kBuildAllowDataAccess == NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DATA_ACCESS_EX);
static_assert(rtxnv::kBuildPerformUpdate == NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE_EX);
static_assert(rtxnv::kGeometryLss == NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_LSS_EX);
static_assert(rtxnv::kLssSuccessiveImplicit == NVAPI_D3D12_RAYTRACING_LSS_PRIMITIVE_FORMAT_SUCCESSIVE_IMPLICIT);

namespace
{
constexpr uint32_t kTagMask = 0x7u, kTagValue = 0x5u;
constexpr float kRibbon = rtx_hair::kRibbon;                       // pi/3: three ribbons per segment
constexpr uint32_t kTriangles = rtx_hair::kTrianglesPerSegment;     // 12: three ribbons, both windings
constexpr uint32_t kVertices = kTriangles * 3;
constexpr uint32_t kStrands = 64, kSegmentsPerStrand = 8, kGrid = 32;
constexpr UINT kExtSlot = 127, kExtSpace = 99;

[[noreturn]] void Fail(int code, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "RtxHairProbe: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::exit(code);
}

#define CHECK_HR(expr) \
    do { const HRESULT hr_ = (expr); if (FAILED(hr_)) Fail(2, "%s failed: 0x%08lx (line %d)", #expr, static_cast<unsigned long>(hr_), __LINE__); } while (0)

struct F3 { float x{}, y{}, z{}; };
F3 operator+(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
F3 operator-(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
F3 operator*(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 Cross(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(F3 a) { return std::sqrt(Dot(a, a)); }
F3 Normalize(F3 a) { const float l = Length(a); return l > 0 ? a * (1.0f / l) : F3{}; }
struct F4 { F3 xyz; float w{}; };

struct RayIn { F3 origin; float tmax{}; F3 direction; uint32_t mask{}; };
static_assert(sizeof(RayIn) == 32);
struct HitOut
{
    uint32_t status, instanceId, primitive, geometry;
    float bary[2], t;
    uint32_t hitKind;
    F3 v0; uint32_t tagged;
    F3 v1; uint32_t pad1;
    F3 v2; uint32_t pad2;
    float p[4], q[4];
};
static_assert(sizeof(HitOut) == 112);
// probe_lss.hlsl's LssOut.
struct LssOut
{
    uint32_t status, isLss, primitive, instanceId;
    float bary[2], t, hitParam;
    float p[4], q[4];
};
static_assert(sizeof(LssOut) == 64);

// Distance from x to the swept sphere of (p, rp)-(q, rq): the minimum over s of
// |x - c(s)| - r(s), which is convex in s. Independent of the shader's rounded-cone code.
double SweptDistance(const double x[3], const F4& p, const F4& q)
{
    auto f = [&](double s) {
        const double c[3] = {p.xyz.x + s * (q.xyz.x - p.xyz.x), p.xyz.y + s * (q.xyz.y - p.xyz.y), p.xyz.z + s * (q.xyz.z - p.xyz.z)};
        const double d[3] = {x[0] - c[0], x[1] - c[1], x[2] - c[2]};
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - (p.w + s * (q.w - p.w));
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 100; ++i)
    {
        const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
        if (f(a) < f(b)) hi = b;
        else lo = a;
    }
    return std::min({f(0), f(1), f(0.5 * (lo + hi))});
}

// Moller-Trumbore in double; b1, b2 are DXR's barycentrics (weights of vertices 1 and 2).
bool RayTriangle(const double o[3], const double d[3], F3 a, F3 b, F3 c, double& t, double& b1, double& b2)
{
    const double e1[3] = {double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z}, e2[3] = {double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z};
    const double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-18) return false;
    const double tv[3] = {o[0] - a.x, o[1] - a.y, o[2] - a.z};
    b1 = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
    const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
    b2 = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
    t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    return b1 >= -1e-6 && b2 >= -1e-6 && b1 + b2 <= 1 + 1e-6;
}

// The same tessellation as rtx_hair/dots_converter.hlsl (18 vertices per segment: three
// ribbons 60 degrees apart, one winding), on the CPU.
void Tessellate(F4 p, F4 q, F3* out)
{
    const F3 d = q.xyz - p.xyz;
    const F3 t = d * (1.0f / std::sqrt(Dot(d, d)));
    const F3 a{std::fabs(t.x), std::fabs(t.y), std::fabs(t.z)};
    const F3 reference = (a.x <= a.y && a.x <= a.z) ? F3{1, 0, 0} : ((a.y <= a.z) ? F3{0, 1, 0} : F3{0, 0, 1});
    const F3 s = Normalize(Cross(t, reference));
    const F3 v = Cross(t, s);
    for (int face = 0; face < 3; ++face)
    {
        const F3 axis = face == 0 ? s : (face == 1 ? s * 0.5f + v * 0.86602540378f : s * -0.5f + v * 0.86602540378f);
        const F3 A = p.xyz + axis * (p.w * kRibbon), B = q.xyz - axis * (q.w * kRibbon);
        const F3 C = q.xyz + axis * (q.w * kRibbon), D = p.xyz - axis * (p.w * kRibbon);
        F3* f = out + face * 6;
        f[0] = A; f[1] = B; f[2] = C; f[3] = A; f[4] = D; f[5] = B;
    }
}

// The previous DOTS shape, for the coverage comparison only: two ribbons 90 degrees apart
// at half-width 1.1107 radii (witcher_dots/converter.hlsl), one winding (4 triangles).
void TessellateTwoRibbons(F4 p, F4 q, F3* out)
{
    constexpr float k = 1.11072073454f;
    const F3 d = q.xyz - p.xyz;
    const F3 t = d * (1.0f / std::sqrt(Dot(d, d)));
    const F3 a{std::fabs(t.x), std::fabs(t.y), std::fabs(t.z)};
    const F3 reference = (a.x <= a.y && a.x <= a.z) ? F3{1, 0, 0} : ((a.y <= a.z) ? F3{0, 1, 0} : F3{0, 0, 1});
    const F3 s = Normalize(Cross(t, reference));
    const F3 v = Cross(t, s);
    for (int face = 0; face < 2; ++face)
    {
        const F3 axis = face == 0 ? s : v;
        const F3 A = p.xyz + axis * (p.w * k), B = q.xyz - axis * (q.w * k);
        const F3 C = q.xyz + axis * (q.w * k), D = p.xyz - axis * (p.w * k);
        F3* f = out + face * 6;
        f[0] = A; f[1] = B; f[2] = C; f[3] = A; f[4] = D; f[5] = B;
    }
}

float Tag(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits = (bits & ~kTagMask) | kTagValue;
    std::memcpy(&value, &bits, 4);
    return value;
}

// Strand coverage of a tessellation: rays perpendicular to a straight segment of radius 1,
// from 720 view angles around it, at offsets -1.4..1.4 from its axis (step 0.002), across its
// middle. Per angle the covered width relative to the true diameter; over all rays the
// share where the triangles and the round strand disagree (hit vs miss).
struct Coverage
{
    double mean{}, min{1e9}, max{}, meanAbsError{}, mismatch{};
};
Coverage MeasureCoverage(void (*tessellate)(F4, F4, F3*), uint32_t triangles)
{
    const F4 p{{0, 0, 0}, 1.0f}, q{{0, 0, 4}, 1.0f};
    std::vector<F3> v(static_cast<size_t>(triangles) * 3);
    tessellate(p, q, v.data());
    constexpr int kViews = 720, kSteps = 1401;
    constexpr double kSpan = 1.4, kStep = 2 * kSpan / (kSteps - 1);
    Coverage c;
    uint64_t rays = 0, disagree = 0;
    for (int view = 0; view < kViews; ++view)
    {
        const double phi = 2 * 3.14159265358979323846 * view / kViews;
        const double d[3] = {std::cos(phi), std::sin(phi), 0}, e[3] = {-std::sin(phi), std::cos(phi), 0};
        uint32_t covered = 0;
        for (int i = 0; i < kSteps; ++i)
        {
            const double offset = -kSpan + i * kStep;
            const double o[3] = {-10 * d[0] + offset * e[0], -10 * d[1] + offset * e[1], 2.0};
            bool hit = false;
            for (uint32_t t = 0; t < triangles && !hit; ++t)
            {
                double tt, b1, b2;
                hit = RayTriangle(o, d, v[t * 3], v[t * 3 + 1], v[t * 3 + 2], tt, b1, b2) && tt > 0;
            }
            covered += hit ? 1 : 0;
            ++rays;
            if (hit != (std::fabs(offset) <= 1.0)) ++disagree;
        }
        const double width = covered * kStep / 2.0;
        c.mean += width;
        c.min = std::min(c.min, width);
        c.max = std::max(c.max, width);
        c.meanAbsError += std::fabs(width - 1.0);
    }
    c.mean /= kViews;
    c.meanAbsError /= kViews;
    c.mismatch = static_cast<double>(disagree) / static_cast<double>(rays);
    return c;
}

std::string CoverageJson(const Coverage& c)
{
    char text[256]{};
    std::snprintf(text, sizeof(text), "{\"meanWidth\": %.4f, \"minWidth\": %.4f, \"maxWidth\": %.4f, \"meanAbsError\": %.4f, \"rayMismatch\": %.5f}",
        c.mean, c.min, c.max, c.meanAbsError, c.mismatch);
    return text;
}

// The round strand the rewritten shaders report hits on: kHitRadius LSS radii, enclosing
// every DOTS ribbon (rtx_hair/lss_ir.h).
F4 Shell(F4 v)
{
    v.w *= rtx_hair::kHitRadius;
    return v;
}

// probe.hlsl's DotsDecode, on the CPU.
void DotsDecode(F3 v0, F3 v1, F3 v2, uint32_t primitive, F4& p, F4& q)
{
    // Two windings (rtx_hair/lss_ir.h kWindings): the second half is reversed, its axis too.
    const float flip = rtx_hair::kWindings == 2 && (primitive % kTriangles) >= kTriangles / 2 ? -1.0f : 1.0f;
    if ((primitive & 1) == 0)
    {
        const F3 qc = (v1 + v2) * 0.5f, d = v2 - v1;
        const float len = Length(d);
        const F3 a = len > 0 ? d * (flip / len) : F3{};
        const float rq = len / (2 * kRibbon), rp = Dot(v0 - qc, a) / kRibbon;
        p = {v0 - a * (rp * kRibbon), rp};
        q = {qc, rq};
    }
    else
    {
        const F3 pc = (v0 + v1) * 0.5f, d = v0 - v1;
        const float len = Length(d);
        const F3 a = len > 0 ? d * (flip / len) : F3{};
        const float rp = len / (2 * kRibbon), rq = -Dot(v2 - pc, a) / kRibbon;
        p = {pc, rp};
        q = {v2 + a * (rq * kRibbon), rq};
    }
}

bool SameBits(F3 a, F3 b) { return std::memcmp(&a, &b, sizeof(F3)) == 0; }

// IEEE half precision, round to nearest even (what an engine writes), and back (what
// HLSL's f16tof32 reads).
uint16_t ToHalf(float value)
{
    uint32_t x;
    std::memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    if (exponent <= 0)
    {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t h = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (h & 1))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;
    return static_cast<uint16_t>(sign | h);
}

float FromHalf(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0)
    {
        const float v = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -v : v;
    }
    const uint32_t bits = exponent == 31 ? sign | 0x7F800000 | (mantissa << 13) : sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}


struct Options
{
    std::filesystem::path nvapi = RTXHAIR_NVAPI_DIR, dxc = RTXHAIR_DXC_DIR, hlsl = RTXHAIR_PROBE_HLSL, lssHlsl = RTXHAIR_LSS_HLSL, converterHlsl = RTXHAIR_CONVERTER_HLSL,
        patchHlsl = RTXHAIR_PATCH_HLSL, out, dump;
    int adapter = -1;
    uint32_t seed = 1;
};

Options Parse(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--out" && more) o.out = argv[++i];
        else if (a == L"--adapter" && more) o.adapter = _wtoi(argv[++i]);
        else if (a == L"--seed" && more) o.seed = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (a == L"--nvapi" && more) o.nvapi = argv[++i];
        else if (a == L"--dxc" && more) o.dxc = argv[++i];
        else if (a == L"--hlsl" && more) o.hlsl = argv[++i];
        else if (a == L"--lss-hlsl" && more) o.lssHlsl = argv[++i];
        else if (a == L"--converter-hlsl" && more) o.converterHlsl = argv[++i];
        else if (a == L"--patch-hlsl" && more) o.patchHlsl = argv[++i];
        else if (a == L"--dump" && more) o.dump = argv[++i]; // rewritten IR of the phase 1a shaders
        else Fail(2, "unknown or incomplete argument: %ls", a.c_str());
    }
    o.nvapi.make_preferred(); o.dxc.make_preferred(); o.hlsl.make_preferred(); o.lssHlsl.make_preferred(); o.converterHlsl.make_preferred(); o.patchHlsl.make_preferred();
    o.dxc = std::filesystem::absolute(o.dxc);
    return o;
}

// NVAPI through nvapi_QueryInterface (System32 dispatcher), as the product resolves it.
struct Nvapi
{
    NvAPI_Status(__cdecl* initialize)() = nullptr;
    NvAPI_Status(__cdecl* driver)(NvU32*, NvAPI_ShortString) = nullptr;
    NvAPI_Status(__cdecl* slotSpace)(IUnknown*, NvU32, NvU32) = nullptr;
    NvAPI_Status(__cdecl* opSupported)(ID3D12Device*, NvU32, bool*) = nullptr;
    NvAPI_Status(__cdecl* caps)(ID3D12Device*, NVAPI_D3D12_RAYTRACING_CAPS_TYPE, void*, size_t) = nullptr;
    NvAPI_Status(__cdecl* prebuild)(ID3D12Device5*, NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS*) = nullptr;
    NvAPI_Status(__cdecl* build)(ID3D12GraphicsCommandList4*, const NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS*) = nullptr;
};

Nvapi LoadNvapi()
{
    const HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) Fail(2, "nvapi64.dll not found in System32");
    using Query = void*(__cdecl*)(unsigned);
    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
    if (!query) Fail(2, "nvapi_QueryInterface missing");
    Nvapi n;
    n.initialize = reinterpret_cast<decltype(n.initialize)>(query(0x0150e828));
    n.driver = reinterpret_cast<decltype(n.driver)>(query(0x2926aaad));
    n.slotSpace = reinterpret_cast<decltype(n.slotSpace)>(query(0xac2dfeb5));
    n.opSupported = reinterpret_cast<decltype(n.opSupported)>(query(0x3dfacec8));
    n.caps = reinterpret_cast<decltype(n.caps)>(query(0x85a6c2a0));
    n.prebuild = reinterpret_cast<decltype(n.prebuild)>(query(0x8d025b77));
    n.build = reinterpret_cast<decltype(n.build)>(query(0xe24ead45));
    if (!n.initialize || !n.slotSpace || !n.caps || !n.prebuild || !n.build)
        Fail(2, "an NVAPI interface is missing (initialize/slot space/caps/prebuild/build)");
    if (n.initialize() != NVAPI_OK) Fail(2, "NvAPI_Initialize failed");
    return n;
}

// DXC from an explicit folder: dxil.dll is loaded first so dxcompiler signs the output.
struct Compiler
{
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> include;
    std::string source, lssSource, converterSource, patchSource; // probe.hlsl, probe_lss.hlsl, rtx_hair/dots_converter.hlsl, rtx_hair/instance_patch.hlsl
    std::wstring includeDir;
};

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) Fail(2, "cannot read %ls", path.c_str());
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

Compiler LoadCompiler(const Options& o)
{
    const auto dxil = (o.dxc / L"dxil.dll").wstring(), dxc = (o.dxc / L"dxcompiler.dll").wstring();
    if (!LoadLibraryExW(dxil.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) Fail(2, "cannot load %ls", dxil.c_str());
    const HMODULE module = LoadLibraryExW(dxc.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) Fail(2, "cannot load %ls", dxc.c_str());
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
    if (!create) Fail(2, "DxcCreateInstance missing");
    Compiler c;
    CHECK_HR(create(CLSID_DxcUtils, IID_PPV_ARGS(&c.utils)));
    CHECK_HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c.compiler)));
    CHECK_HR(c.utils->CreateDefaultIncludeHandler(&c.include));
    c.source = ReadText(o.hlsl);
    c.lssSource = ReadText(o.lssHlsl);
    c.converterSource = ReadText(o.converterHlsl);
    c.patchSource = ReadText(o.patchHlsl);
    c.includeDir = o.nvapi.wstring();
    return c;
}

// Returns the signed DXIL, or an empty blob with the compiler's errors in `errors`.
ComPtr<IDxcBlob> Compile(Compiler& c, const std::string& source, const wchar_t* target, const wchar_t* entry, const wchar_t* define,
    std::string& errors)
{
    std::vector<const wchar_t*> args{L"-T", target, L"-HV", L"2021", L"-I", c.includeDir.c_str(), L"-D", define, L"-O3"};
    if (entry) { args.push_back(L"-E"); args.push_back(entry); }
    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    CHECK_HR(c.compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), c.include.Get(), IID_PPV_ARGS(&result)));
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlob> object;
    if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object && object->GetBufferSize())
        return object;
    ComPtr<IDxcBlobUtf8> messages;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr)) && messages && messages->GetStringLength())
        errors = messages->GetStringPointer();
    else
        errors = "compile failed";
    return nullptr;
}

struct Gpu
{
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 value{};
    std::vector<ComPtr<ID3D12Resource>> keep; // upload buffers alive until the list ran

    void Submit()
    {
        CHECK_HR(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        CHECK_HR(queue->Signal(fence.Get(), ++value));
        CHECK_HR(fence->SetEventOnCompletion(value, event));
        WaitForSingleObject(event, INFINITE);
        CHECK_HR(device->GetDeviceRemovedReason());
        CHECK_HR(allocator->Reset());
        CHECK_HR(list->Reset(allocator.Get(), nullptr));
        keep.clear();
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = std::max<UINT64>(size, 256);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        CHECK_HR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }

    ComPtr<ID3D12Resource> Upload(const void* data, UINT64 size)
    {
        auto resource = Buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        CHECK_HR(resource->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, static_cast<size_t>(size));
        resource->Unmap(0, nullptr);
        return resource;
    }
};

std::string AdapterName(IDXGIAdapter1* adapter, DXGI_ADAPTER_DESC1& desc)
{
    adapter->GetDesc1(&desc);
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}

struct Blas
{
    const char* name;
    uint32_t mask, instanceId;
    float offsetX;
    bool dataAccess, viaNvapi, dots, tagged;
    ComPtr<ID3D12Resource> as;
    UINT64 sizeWith{}, sizeWithout{}; // NVAPI prebuild ResultDataMaxSizeInBytes with / without ALLOW_DATA_ACCESS_EX
};

struct Group
{
    uint32_t rays{}, hits{}, wrongInstance{}, exact{}, sameSetOtherOrder{}, zero{}, tagged{};
    double posError{}, radiusError{}, shaderPosError{}, shaderRadiusError{};
    std::vector<std::string> samples; // first mismatches, for the report
};

std::string Json(const std::string& s)
{
    std::string out;
    for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    const Options options = Parse(argc, argv);
    std::mt19937 random(options.seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto range = [&](float a, float b) { return a + (b - a) * unit(random); };

    // Device on the NVIDIA adapter (or --adapter).
    ComPtr<IDXGIFactory4> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    std::string adapterName;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        adapterName = AdapterName(adapter.Get(), adapterDesc);
        if (options.adapter >= 0 ? static_cast<int>(i) == options.adapter : adapterDesc.VendorId == 0x10de) break;
        adapter.Reset();
    }
    if (!adapter) Fail(2, "no NVIDIA adapter (use --adapter)");
    Gpu gpu;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&gpu.device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
    if (FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))
        || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1
        || FAILED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
        || model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        Fail(2, "%s: needs DXR 1.1 and shader model 6.6", adapterName.c_str());
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)));
    CHECK_HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)));
    CHECK_HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)));
    CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // NVAPI: driver, ray tracing caps, extension op support.
    Nvapi nv = LoadNvapi();
    NvU32 driver = 0;
    NvAPI_ShortString branch{};
    if (nv.driver) nv.driver(&driver, branch);
    struct Cap { const char* name; NVAPI_D3D12_RAYTRACING_CAPS_TYPE type; uint32_t value; NvAPI_Status status; };
    Cap caps[] = {
        {"clusterOperations", NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS},
        {"spheres", NVAPI_D3D12_RAYTRACING_CAPS_TYPE_SPHERES},
        {"linearSweptSpheres", NVAPI_D3D12_RAYTRACING_CAPS_TYPE_LINEAR_SWEPT_SPHERES},
    };
    for (auto& c : caps) { c.value = 0; c.status = nv.caps(gpu.device.Get(), c.type, &c.value, sizeof(c.value)); }
    struct Op { uint32_t code; const char* name; bool supported; NvAPI_Status status; };
    Op ops[] = {
        {86, "TRIANGLE_OBJECT_POSITIONS"}, {97, "CANDIDATE_TRIANGLE_OBJECT_POSITIONS"},
        {98, "COMMITTED_TRIANGLE_OBJECT_POSITIONS"}, {99, "HIT_OBJECT_GET_TRIANGLE_OBJECT_POSITIONS"},
        {104, "LSS_OBJECT_POSITIONS_AND_RADII"}, {106, "COMMITTED_LSS_OBJECT_POSITIONS_AND_RADII"},
        {112, "IS_LSS_HIT"}, {114, "COMMITTED_IS_LSS"},
    };
    for (auto& op : ops) { op.supported = false; op.status = nv.opSupported ? nv.opSupported(gpu.device.Get(), op.code, &op.supported) : NVAPI_NO_IMPLEMENTATION; }
    if (nv.slotSpace(gpu.device.Get(), kExtSlot, kExtSpace) != NVAPI_OK) Fail(2, "NvAPI_D3D12_SetNvShaderExtnSlotSpace failed");

    // Synthetic LSS strands: kSegmentsPerStrand + 1 vertices each; segment j = (v[j], v[j+1]).
    std::vector<F4> truthP, truthQ; // per segment, object space
    for (uint32_t s = 0; s < kStrands; ++s)
    {
        F3 at{range(-0.5f, 0.5f), range(-0.5f, 0.5f), range(-0.5f, 0.5f)};
        F3 dir = Normalize({range(-1, 1), range(-1, 1), range(-1, 1)});
        const float r0 = range(0.004f, 0.02f);
        std::vector<F4> vertices;
        for (uint32_t j = 0; j <= kSegmentsPerStrand; ++j)
        {
            const float u = static_cast<float>(j) / kSegmentsPerStrand;
            float r = r0;
            switch (s % 4)
            {
            case 0: r = r0 * (1.0f - u); break;          // tapered to a zero tip
            case 1: break;                               // constant
            case 2: r = r0 * (1.0f - 0.75f * u); break;  // thinning
            default: r = 0.002f + 0.013f * u; break;     // growing
            }
            vertices.push_back({at, r});
            dir = Normalize(dir + F3{range(-0.3f, 0.3f), range(-0.3f, 0.3f), range(-0.3f, 0.3f)});
            at = at + dir * range(0.05f, 0.12f);
        }
        for (uint32_t j = 0; j < kSegmentsPerStrand; ++j) { truthP.push_back(vertices[j]); truthQ.push_back(vertices[j + 1]); }
    }
    const uint32_t segments = static_cast<uint32_t>(truthP.size());
    std::vector<F3> dotsPlain(segments * kVertices), dotsTagged;
    for (uint32_t s = 0; s < segments; ++s) Tessellate(truthP[s], truthQ[s], &dotsPlain[s * kVertices]);
    dotsTagged = dotsPlain;
    for (auto& v : dotsTagged) { v.x = Tag(v.x); v.y = Tag(v.y); v.z = Tag(v.z); }

    // Game-like mesh: an indexed, jittered height-field grid.
    std::vector<F3> meshVertices;
    std::vector<uint32_t> meshIndices;
    for (uint32_t y = 0; y <= kGrid; ++y)
        for (uint32_t x = 0; x <= kGrid; ++x)
        {
            const float fx = static_cast<float>(x) / kGrid - 0.5f, fy = static_cast<float>(y) / kGrid - 0.5f;
            meshVertices.push_back({fx + range(-0.004f, 0.004f), fy + range(-0.004f, 0.004f),
                0.1f * std::sin(3 * fx) * std::cos(2 * fy) + range(-0.003f, 0.003f)});
        }
    for (uint32_t y = 0; y < kGrid; ++y)
        for (uint32_t x = 0; x < kGrid; ++x)
        {
            const uint32_t a = y * (kGrid + 1) + x, b = a + 1, c = a + kGrid + 1, d = c + 1;
            meshIndices.insert(meshIndices.end(), {a, b, c, b, d, c});
        }
    const uint32_t meshTriangles = static_cast<uint32_t>(meshIndices.size() / 3);

    auto dotsTaggedBuffer = gpu.Upload(dotsTagged.data(), dotsTagged.size() * sizeof(F3));
    auto dotsPlainBuffer = gpu.Upload(dotsPlain.data(), dotsPlain.size() * sizeof(F3));
    auto meshVertexBuffer = gpu.Upload(meshVertices.data(), meshVertices.size() * sizeof(F3));
    auto meshIndexBuffer = gpu.Upload(meshIndices.data(), meshIndices.size() * sizeof(uint32_t));

    Blas blases[] = {
        {"dotsTagged", 0x01, 10, 0.0f, true, true, true, true},
        {"dotsPlain", 0x08, 11, 4.0f, true, true, true, false},
        {"meshDataAccess", 0x04, 21, 12.0f, true, true, false, false},
        {"meshPlain", 0x02, 20, 8.0f, false, false, false, false},
    };
    using Flags = NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX;
    const auto exFlags = [](bool dataAccess) {
        return static_cast<Flags>(NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE_EX
            | (dataAccess ? NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DATA_ACCESS_EX : 0));
    };
    std::vector<ComPtr<ID3D12Resource>> scratch;
    for (auto& blas : blases)
    {
        NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX geometry{};
        geometry.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX;
        geometry.flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        auto& tri = geometry.triangles;
        tri.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        tri.VertexBuffer.StrideInBytes = sizeof(F3);
        if (blas.dots)
        {
            tri.VertexBuffer.StartAddress = (blas.tagged ? dotsTaggedBuffer : dotsPlainBuffer)->GetGPUVirtualAddress();
            tri.VertexCount = segments * kVertices;
            tri.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
        else
        {
            tri.VertexBuffer.StartAddress = meshVertexBuffer->GetGPUVirtualAddress();
            tri.VertexCount = static_cast<UINT>(meshVertices.size());
            tri.IndexFormat = DXGI_FORMAT_R32_UINT;
            tri.IndexCount = static_cast<UINT>(meshIndices.size());
            tri.IndexBuffer = meshIndexBuffer->GetGPUVirtualAddress();
        }
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX inputs{};
        inputs.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.numDescs = 1;
        inputs.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.geometryDescStrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX);
        inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO with{}, without{};
        NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS prebuild{};
        prebuild.version = NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER;
        prebuild.pDesc = &inputs;
        inputs.flags = exFlags(true);
        prebuild.pInfo = &with;
        if (nv.prebuild(gpu.device.Get(), &prebuild) != NVAPI_OK) Fail(2, "%s: NVAPI prebuild (data access) failed", blas.name);
        inputs.flags = exFlags(false);
        prebuild.pInfo = &without;
        if (nv.prebuild(gpu.device.Get(), &prebuild) != NVAPI_OK) Fail(2, "%s: NVAPI prebuild failed", blas.name);
        blas.sizeWith = with.ResultDataMaxSizeInBytes;
        blas.sizeWithout = without.ResultDataMaxSizeInBytes;
        const auto& info = blas.dataAccess ? with : without;
        blas.as = gpu.Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scratch.push_back(gpu.Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
        if (blas.viaNvapi)
        {
            inputs.flags = exFlags(blas.dataAccess);
            NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX desc{};
            desc.destAccelerationStructureData = blas.as->GetGPUVirtualAddress();
            desc.inputs = inputs;
            desc.scratchAccelerationStructureData = scratch.back()->GetGPUVirtualAddress();
            NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS params{};
            params.version = NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER;
            params.pDesc = &desc;
            if (nv.build(gpu.list.Get(), &params) != NVAPI_OK) Fail(2, "%s: NVAPI build failed", blas.name);
        }
        else
        {
            // The game's own path: plain D3D12, no data access. Its size comes from D3D12's prebuild.
            D3D12_RAYTRACING_GEOMETRY_DESC plain{};
            plain.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            plain.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            plain.Triangles = tri;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            desc.Inputs.NumDescs = 1;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.pGeometryDescs = &plain;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO d3d{};
            gpu.device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &d3d);
            if (d3d.ResultDataMaxSizeInBytes > info.ResultDataMaxSizeInBytes || d3d.ScratchDataSizeInBytes > info.ScratchDataSizeInBytes)
            {
                blas.as = gpu.Buffer(d3d.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                scratch.back() = gpu.Buffer(d3d.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            desc.DestAccelerationStructureData = blas.as->GetGPUVirtualAddress();
            desc.ScratchAccelerationStructureData = scratch.back()->GetGPUVirtualAddress();
            gpu.list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
    }
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    gpu.list->ResourceBarrier(1, &uav);

    // TLAS: one instance per BLAS, translated apart along x.
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    for (const auto& blas : blases)
    {
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
        instance.Transform[0][3] = blas.offsetX;
        instance.InstanceID = blas.instanceId;
        instance.InstanceMask = blas.mask;
        // Culling off, as universal hair's instance patch sets it on DOTS instances (phase 1b tests the patch).
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = blas.as->GetGPUVirtualAddress();
        instances.push_back(instance);
    }
    auto instanceBuffer = gpu.Upload(instances.data(), instances.size() * sizeof(instances[0]));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top{};
    top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top.Inputs.NumDescs = static_cast<UINT>(instances.size());
    top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    gpu.device->GetRaytracingAccelerationStructurePrebuildInfo(&top.Inputs, &topInfo);
    auto tlas = gpu.Buffer(topInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto topScratch = gpu.Buffer(topInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    top.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    top.ScratchAccelerationStructureData = topScratch->GetGPUVirtualAddress();
    gpu.list->BuildRaytracingAccelerationStructure(&top, 0, nullptr);
    gpu.list->ResourceBarrier(1, &uav);
    gpu.Submit();

    // Rays. DOTS: across each segment's axis at a random point and angle (the hit may be a
    // neighbouring segment; checks use the segment that was hit). Mesh: down onto a triangle.
    std::vector<RayIn> rays;
    std::vector<uint32_t> rayBlas;
    for (uint32_t b = 0; b < 4; ++b)
    {
        const auto& blas = blases[b];
        const uint32_t count = blas.dots ? segments : std::min(meshTriangles, segments);
        for (uint32_t i = 0; i < count; ++i)
        {
            RayIn ray{};
            ray.mask = blas.mask;
            if (blas.dots)
            {
                const F3 p = truthP[i].xyz, q = truthQ[i].xyz;
                const F3 m = p + (q - p) * range(0.25f, 0.75f);
                const F3 n = Normalize(Cross(Normalize(q - p), Normalize({range(-1, 1), range(-1, 1), range(-1, 1)})));
                ray.origin = m + n * 0.25f;
                ray.direction = n * -1.0f;
                ray.tmax = 0.5f;
            }
            else
            {
                const F3 a = meshVertices[meshIndices[i * 3]], c1 = meshVertices[meshIndices[i * 3 + 1]], c2 = meshVertices[meshIndices[i * 3 + 2]];
                const F3 centroid = (a + c1 + c2) * (1.0f / 3);
                ray.origin = centroid + F3{0, 0, 1};
                ray.direction = {0, 0, -1};
                ray.tmax = 2.0f;
            }
            ray.origin.x += blas.offsetX;
            rays.push_back(ray);
            rayBlas.push_back(b);
        }
    }
    const UINT rayCount = static_cast<UINT>(rays.size());
    auto rayBuffer = gpu.Upload(rays.data(), rays.size() * sizeof(RayIn));

    // Root signature: TLAS, rays, results, the NVAPI extension UAV, constants.
    D3D12_DESCRIPTOR_RANGE extRange{};
    extRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    extRange.NumDescriptors = 1;
    extRange.BaseShaderRegister = kExtSlot;
    extRange.RegisterSpace = kExtSpace;
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 1;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &extRange;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 0;
    params[4].Constants.Num32BitValues = 4;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 5;
    rootDesc.pParameters = params;
    ComPtr<ID3DBlob> serialized, rootErrors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors)))
        Fail(2, "root signature: %s", rootErrors ? static_cast<const char*>(rootErrors->GetBufferPointer()) : "?");
    ComPtr<ID3D12RootSignature> root;
    CHECK_HR(gpu.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));

    // The extension UAV: a real 256-byte structured buffer (NvShaderExtnStruct) the driver intercepts.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    CHECK_HR(gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    auto extBuffer = gpu.Buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_UNORDERED_ACCESS_VIEW_DESC extView{};
    extView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    extView.Buffer.NumElements = 1;
    extView.Buffer.StructureByteStride = 256;
    gpu.device->CreateUnorderedAccessView(extBuffer.Get(), nullptr, &extView, heap->GetCPUDescriptorHandleForHeapStart());

    uint32_t constants[4] = {rayCount, kTagMask, kTagValue, 0};
    // What the helpers below trace: phase 0 and 1a use this scene, phase 1b its own.
    D3D12_GPU_VIRTUAL_ADDRESS traceTlas = tlas->GetGPUVirtualAddress(), traceRays = rayBuffer->GetGPUVirtualAddress();
    UINT traceCount = rayCount;
    Compiler compiler = LoadCompiler(options);

    struct Pass { const char* name; bool created{}; std::string error; std::vector<HitOut> results; Group groups[4]; };
    Pass passes[2] = {{"rayQuery"}, {"closestHit"}};

    auto bind = [&](ID3D12Resource* results) {
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        gpu.list->SetDescriptorHeaps(1, heaps);
        gpu.list->SetComputeRootSignature(root.Get());
        gpu.list->SetComputeRootShaderResourceView(0, traceTlas);
        gpu.list->SetComputeRootShaderResourceView(1, traceRays);
        gpu.list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
        gpu.list->SetComputeRootDescriptorTable(3, heap->GetGPUDescriptorHandleForHeapStart());
        gpu.list->SetComputeRoot32BitConstants(4, 4, constants, 0);
    };
    auto readBack = [&](ID3D12Resource* results, UINT64 bytes) {
        auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = results;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = results;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        gpu.list->ResourceBarrier(2, barriers);
        gpu.list->CopyBufferRegion(readback.Get(), 0, results, 0, bytes);
        gpu.Submit();
        void* mapped = nullptr;
        const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
        CHECK_HR(readback->Map(0, &read, &mapped));
        std::vector<uint8_t> out(static_cast<size_t>(bytes));
        std::memcpy(out.data(), mapped, out.size());
        readback->Unmap(0, nullptr);
        return out;
    };
    // One thread per ray: a compute shader, or a DXR library (RayGen, Miss, HitGroup =
    // ClosestHit). False with `error` when the driver refuses the pipeline.
    auto runCompute = [&](const void* code, size_t size, UINT stride, std::vector<uint8_t>& out, std::string& error) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = {code, size};
        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = gpu.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
        {
            char text[64];
            std::snprintf(text, sizeof(text), "CreateComputePipelineState 0x%08lx", static_cast<unsigned long>(hr));
            error = text;
            return false;
        }
        const UINT64 bytes = static_cast<UINT64>(traceCount) * stride;
        auto results = gpu.Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        bind(results.Get());
        gpu.list->SetPipelineState(pso.Get());
        gpu.list->Dispatch((traceCount + 63) / 64, 1, 1);
        out = readBack(results.Get(), bytes);
        return true;
    };
    auto runRays = [&](const void* code, size_t size, UINT stride, std::vector<uint8_t>& out, std::string& error) {
        D3D12_DXIL_LIBRARY_DESC library{};
        library.DXILLibrary = {code, size};
        D3D12_HIT_GROUP_DESC hitGroup{};
        hitGroup.HitGroupExport = L"HitGroup";
        hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroup.ClosestHitShaderImport = L"ClosestHit";
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
        D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
        D3D12_STATE_SUBOBJECT subobjects[] = {
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
            {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
            {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
        };
        D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, static_cast<UINT>(std::size(subobjects)), subobjects};
        ComPtr<ID3D12StateObject> state;
        const HRESULT hr = gpu.device->CreateStateObject(&stateDesc, IID_PPV_ARGS(&state));
        if (FAILED(hr))
        {
            char text[64];
            std::snprintf(text, sizeof(text), "CreateStateObject 0x%08lx", static_cast<unsigned long>(hr));
            error = text;
            return false;
        }
        ComPtr<ID3D12StateObjectProperties> properties;
        CHECK_HR(state.As(&properties));
        // Shader table: raygen at 0, miss at 64, hit group at 128 (64-byte table alignment).
        std::vector<uint8_t> table(192, 0);
        const wchar_t* names[] = {L"RayGen", L"Miss", L"HitGroup"};
        for (int i = 0; i < 3; ++i)
        {
            const void* id = properties->GetShaderIdentifier(names[i]);
            if (!id) Fail(2, "no shader identifier for %ls", names[i]);
            std::memcpy(table.data() + i * 64, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
        auto tableBuffer = gpu.Upload(table.data(), table.size());
        const UINT64 bytes = static_cast<UINT64>(traceCount) * stride;
        auto results = gpu.Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        bind(results.Get());
        gpu.list->SetPipelineState1(state.Get());
        const auto base = tableBuffer->GetGPUVirtualAddress();
        D3D12_DISPATCH_RAYS_DESC dispatch{};
        dispatch.RayGenerationShaderRecord = {base, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        dispatch.MissShaderTable = {base + 64, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        dispatch.HitGroupTable = {base + 128, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        dispatch.Width = traceCount;
        dispatch.Height = 1;
        dispatch.Depth = 1;
        gpu.list->DispatchRays(&dispatch);
        gpu.keep.push_back(tableBuffer);
        out = readBack(results.Get(), bytes);
        return true;
    };
    auto unpack = [&](const std::vector<uint8_t>& raw, auto& out) {
        out.resize(traceCount);
        std::memcpy(out.data(), raw.data(), traceCount * sizeof(out[0]));
    };

    // Phase 0, pass 1: inline RayQuery in a compute shader (op 98); pass 2: closest hit (op 86).
    for (int k = 0; k < 2; ++k)
    {
        auto& pass = passes[k];
        const bool library = k == 1;
        std::vector<uint8_t> raw;
        if (auto dxil = Compile(compiler, compiler.source, library ? L"lib_6_6" : L"cs_6_6", library ? nullptr : L"CSMain",
                library ? L"PROBE_LIBRARY=1" : L"PROBE_COMPUTE=1", pass.error))
        {
            pass.created = library ? runRays(dxil->GetBufferPointer(), dxil->GetBufferSize(), sizeof(HitOut), raw, pass.error)
                                   : runCompute(dxil->GetBufferPointer(), dxil->GetBufferSize(), sizeof(HitOut), raw, pass.error);
            if (pass.created) unpack(raw, pass.results);
        }
    }

    // Phase 1a: probe_lss.hlsl rewritten by rtx_hair, same scene, same two ways.
    struct LssGroup
    {
        uint32_t rays{}, hits{}, lss{}, wrong{}, segmentOk{}, expectSurface{}, surfaceOk{}, fallback{}, inside{}, fallbackOk{}, triangleOk{};
        double decodeError{}, surfaceError{}, uError{}, triangleError{};
        std::vector<std::string> samples;
    };
    struct LssPass
    {
        const char* name;
        bool rewritten{}, created{};
        std::string error;
        rtx_hair::IrRewriteReport report{};
        size_t originalBytes{}, rewrittenBytes{};
        std::vector<std::byte> dxil; // the rewritten shader, traced again in phase 1b
        std::vector<LssOut> results;
        LssGroup groups[4];
    };
    LssPass lssPasses[2] = {{"lssRayQuery"}, {"lssClosestHit"}};
    rtx_hair::DxcTools tools;
    std::string toolsError;
    const bool toolsLoaded = rtx_hair::LoadDxcTools(options.dxc, tools, toolsError);
    for (int k = 0; k < 2 && toolsLoaded; ++k)
    {
        auto& pass = lssPasses[k];
        const bool library = k == 1;
        auto dxil = Compile(compiler, compiler.lssSource, library ? L"lib_6_6" : L"cs_6_6", library ? nullptr : L"CSMain",
            library ? L"PROBE_LIBRARY=1" : L"PROBE_COMPUTE=1", pass.error);
        if (!dxil) continue;
        const auto* first = static_cast<const std::byte*>(dxil->GetBufferPointer());
        pass.originalBytes = dxil->GetBufferSize();
        std::vector<std::byte> rewritten;
        std::string ir;
        pass.rewritten = rtx_hair::RewriteDxil(tools, {first, first + dxil->GetBufferSize()}, rewritten, pass.report, pass.error, &ir);
        pass.rewrittenBytes = rewritten.size();
        pass.dxil = rewritten;
        if (!options.dump.empty())
        {
            std::filesystem::create_directories(options.dump);
            std::ofstream(options.dump / (std::string(pass.name) + ".rewritten.ll"), std::ios::binary) << ir;
            std::string final, error;
            if (pass.rewritten && rtx_hair::DisassembleDxil(tools, rewritten, final, error))
                std::ofstream(options.dump / (std::string(pass.name) + ".final.ll"), std::ios::binary) << final;
        }
        if (!pass.rewritten) continue;
        std::vector<uint8_t> raw;
        pass.created = library ? runRays(rewritten.data(), rewritten.size(), sizeof(LssOut), raw, pass.error)
                               : runCompute(rewritten.data(), rewritten.size(), sizeof(LssOut), raw, pass.error);
        if (pass.created) unpack(raw, pass.results);
    }

    // Phase 1b: the strands as NVAPI LSS geometry, built through rtx_hair::DotsBuilder (the
    // converter on this command list, then the DOTS triangle build) and traced again by the
    // rewritten shaders. A: Witcher's layout (float4 position + radius, R32 indices,
    // successive implicit), built, then updated in place. B: half positions and radii, a
    // list of vertex pairs, no index buffer. C: packed float3, one radius for every vertex,
    // R16 indices. D: an unsupported position format: every primitive inactive, nothing hit.
    struct LayoutCase
    {
        const char* name;
        uint32_t mask, instanceId;
        float offsetX;
        std::vector<F4> p, q; // per LSS primitive, as the GPU reads them
        std::vector<F3> dots; // CPU DOTS of p, q (tagged), for ribbon fallbacks
        ComPtr<ID3D12Resource> source, radius, index, as, scratch;
        rtxnv::GeometryDescEx geometry{};
        rtxnv::Status prebuild{-1}, build{-1}, update{-1};
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        LssGroup groups[2]; // per LSS pass
    };
    LayoutCase layouts[4] = {{"successive32", 0x10, 30, 20.0f}, {"list16", 0x20, 31, 24.0f}, {"successiveConstant", 0x40, 32, 28.0f},
        {"unsupported", 0x80, 33, 32.0f}};
    const uint32_t vertsPerStrand = kSegmentsPerStrand + 1;
    std::vector<F4> strandVertices;
    std::vector<uint32_t> segmentStart; // first vertex of each segment
    for (uint32_t s = 0; s < kStrands; ++s)
    {
        for (uint32_t j = 0; j < kSegmentsPerStrand; ++j)
        {
            strandVertices.push_back(truthP[s * kSegmentsPerStrand + j]);
            segmentStart.push_back(s * vertsPerStrand + j);
        }
        strandVertices.push_back(truthQ[s * kSegmentsPerStrand + kSegmentsPerStrand - 1]);
    }
    {
        auto& L = layouts[0];
        L.source = gpu.Upload(strandVertices.data(), strandVertices.size() * sizeof(F4));
        L.index = gpu.Upload(segmentStart.data(), segmentStart.size() * sizeof(uint32_t));
        auto& g = L.geometry.lss;
        g.vertexCount = static_cast<uint32_t>(strandVertices.size());
        g.indexCount = segments;
        g.primitiveCount = segments;
        g.vertexPositionBuffer = {L.source->GetGPUVirtualAddress(), sizeof(F4)};
        g.vertexPositionFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.vertexRadiusBuffer = {L.source->GetGPUVirtualAddress() + 12, sizeof(F4)};
        g.vertexRadiusFormat = DXGI_FORMAT_R32_FLOAT;
        g.indexBuffer = {L.index->GetGPUVirtualAddress(), 4};
        g.indexFormat = DXGI_FORMAT_R32_UINT;
        g.primitiveFormat = rtxnv::kLssSuccessiveImplicit;
        L.p = truthP;
        L.q = truthQ;
    }
    {
        auto& L = layouts[1];
        std::vector<uint16_t> positions, radii;
        for (uint32_t i = 0; i < segments; ++i)
            for (int k = 0; k < 2; ++k)
            {
                const F4 v = k ? truthQ[i] : truthP[i];
                const uint16_t h[3] = {ToHalf(v.xyz.x), ToHalf(v.xyz.y), ToHalf(v.xyz.z)};
                positions.insert(positions.end(), {h[0], h[1], h[2], 0});
                radii.push_back(ToHalf(v.w));
                (k ? L.q : L.p).push_back({{FromHalf(h[0]), FromHalf(h[1]), FromHalf(h[2])}, FromHalf(radii.back())});
            }
        L.source = gpu.Upload(positions.data(), positions.size() * sizeof(uint16_t));
        L.radius = gpu.Upload(radii.data(), radii.size() * sizeof(uint16_t));
        auto& g = L.geometry.lss;
        g.vertexCount = 2 * segments;
        g.primitiveCount = segments;
        g.vertexPositionBuffer = {L.source->GetGPUVirtualAddress(), 8};
        g.vertexPositionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g.vertexRadiusBuffer = {L.radius->GetGPUVirtualAddress(), 2};
        g.vertexRadiusFormat = DXGI_FORMAT_R16_FLOAT;
        g.primitiveFormat = rtxnv::kLssList;
    }
    {
        auto& L = layouts[2];
        constexpr float radius = 0.008f;
        std::vector<F3> packed;
        for (const auto& v : strandVertices) packed.push_back(v.xyz);
        std::vector<uint16_t> starts;
        for (uint32_t v : segmentStart) starts.push_back(static_cast<uint16_t>(v));
        L.source = gpu.Upload(packed.data(), packed.size() * sizeof(F3));
        L.radius = gpu.Upload(&radius, sizeof(radius));
        L.index = gpu.Upload(starts.data(), starts.size() * sizeof(uint16_t));
        auto& g = L.geometry.lss;
        g.vertexCount = static_cast<uint32_t>(packed.size());
        g.indexCount = segments;
        g.primitiveCount = segments;
        g.vertexPositionBuffer = {L.source->GetGPUVirtualAddress(), sizeof(F3)};
        g.vertexPositionFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.vertexRadiusBuffer = {L.radius->GetGPUVirtualAddress(), 0};
        g.vertexRadiusFormat = DXGI_FORMAT_R32_FLOAT;
        g.indexBuffer = {L.index->GetGPUVirtualAddress(), 0};
        g.indexFormat = DXGI_FORMAT_R16_UINT;
        g.primitiveFormat = rtxnv::kLssSuccessiveImplicit;
        for (uint32_t i = 0; i < segments; ++i)
        {
            L.p.push_back({truthP[i].xyz, radius});
            L.q.push_back({truthQ[i].xyz, radius});
        }
    }
    {
        auto& L = layouts[3];
        L.geometry.lss = layouts[0].geometry.lss;
        L.geometry.lss.vertexPositionFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        L.p = truthP;
        L.q = truthQ;
    }
    for (auto& L : layouts)
    {
        L.geometry.type = rtxnv::kGeometryLss;
        L.geometry.flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        L.dots.resize(static_cast<size_t>(segments) * kVertices);
        for (uint32_t i = 0; i < segments; ++i) Tessellate(L.p[i], L.q[i], &L.dots[static_cast<size_t>(i) * kVertices]);
        for (auto& v : L.dots) { v.x = Tag(v.x); v.y = Tag(v.y); v.z = Tag(v.z); }
    }
    rtx_hair::DotsBuilder builder;
    std::string builderError;
    bool builderReady = false;
    uint32_t restores = 0, builderCalls = 0;
    // The fallback (universal_hair.cpp, when a list's compute bindings cannot be restored):
    // a fresh BLAS of inactive triangles, then an update of it, which must be skipped.
    rtxnv::Status inactiveBuild = rtxnv::kError, inactiveUpdate = rtxnv::kError;
    uint32_t restoresBeforeInactive = 0;
    bool inactiveUpdateRecorded = false; // the skipped update handed state back, i.e. recorded something
    ComPtr<ID3D12Resource> inactiveAs, inactiveScratch;
    // The instance patch (universal hair's hair-instance culling switch): phase 1b's TLAS is
    // built from its copy of instances whose culling is on; its output is read back.
    rtx_hair::InstancePatcher patcher;
    std::string patcherError;
    bool patcherReady = false, patchRecorded = false;
    uint32_t patchedOk = 0, patchedChecked = 0;
    ComPtr<ID3D12Resource> patchedInstances;
    std::vector<int> instanceLayout;
    rtx_hair::nvapi::Status rebuildUpdate = rtx_hair::nvapi::kError;
    {
        std::vector<std::byte> patchDxil;
        patcherError = toolsLoaded ? std::string() : toolsError;
        patcherReady = toolsLoaded && rtx_hair::CompileHlsl(tools, compiler.patchSource, L"cs_6_0", patchDxil, patcherError)
            && patcher.Initialize(gpu.device.Get(), patchDxil, patcherError);
    }
    {
        // Compiled as universal_hair.cpp does in a game: rtx_hair::CompileHlsl, validated and signed.
        std::string converterError = toolsLoaded ? std::string() : toolsError;
        std::vector<std::byte> converter;
        if (toolsLoaded && rtx_hair::CompileHlsl(tools, compiler.converterSource, L"cs_6_0", converter, converterError))
            builderReady = builder.Initialize(gpu.device.Get(), reinterpret_cast<rtxnv::PrebuildFn>(nv.prebuild),
                reinterpret_cast<rtxnv::BuildFn>(nv.build), converter, builderError);
        else
            builderError = "converter: " + converterError;
    }
    ComPtr<ID3D12Resource> tlas1b, tlas1bScratch, instances1bBuffer, rays1bBuffer;
    std::vector<RayIn> rays1b;
    std::vector<uint32_t> rayLayout;
    if (builderReady)
    {
        // The game's compute state the builder must hand back: this probe's root signature.
        auto restore = [&](ID3D12GraphicsCommandList4* list) {
            ++restores;
            list->SetComputeRootSignature(root.Get());
        };
        // The second update of layout 0 becomes a full rebuild in place.
        builder.SetRebuildPolicy(2, 1u << 30, 0);
        for (int k = 0; k < 4; ++k)
        {
            auto& L = layouts[k];
            rtxnv::BuildInputsEx inputs{};
            inputs.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.numDescs = 1;
            inputs.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.geometryDescStrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX);
            inputs.pGeometryDescs = &L.geometry;
            L.prebuild = builder.Prebuild(inputs, L.info);
            if (L.prebuild != rtxnv::kOk) continue;
            L.as = gpu.Buffer(L.info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            L.scratch = gpu.Buffer(std::max(L.info.ScratchDataSizeInBytes, L.info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            rtxnv::BuildDescEx desc{L.as->GetGPUVirtualAddress(), inputs, 0, L.scratch->GetGPUVirtualAddress()};
            const rtxnv::BuildParams params{rtxnv::kBuildParamsVersion, &desc, 0, nullptr};
            gpu.list->SetComputeRootSignature(root.Get());
            ++builderCalls;
            L.build = builder.Build(gpu.list.Get(), params, restore);
            if (k == 0 && L.build == rtxnv::kOk)
            {
                // An in-place update, as a game refits animated hair every frame.
                gpu.list->ResourceBarrier(1, &uav);
                desc.inputs.flags |= rtxnv::kBuildPerformUpdate;
                desc.sourceAccelerationStructureData = desc.destAccelerationStructureData;
                ++builderCalls;
                L.update = builder.Build(gpu.list.Get(), params, restore);
                gpu.list->ResourceBarrier(1, &uav);
                ++builderCalls;
                rebuildUpdate = builder.Build(gpu.list.Get(), params, restore);
            }
            // Supported layouts are hair for the instance patch; the unsupported one is left
            // out, so its instance must come through unchanged.
            if (L.build == rtxnv::kOk && k < 3) patcher.Add(L.as->GetGPUVirtualAddress());
            if (k == 0 && L.build == rtxnv::kOk)
            {
                inactiveAs = gpu.Buffer(L.info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                inactiveScratch = gpu.Buffer(std::max(L.info.ScratchDataSizeInBytes, L.info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                rtxnv::BuildDescEx fallback{inactiveAs->GetGPUVirtualAddress(), inputs, 0, inactiveScratch->GetGPUVirtualAddress()};
                const rtxnv::BuildParams fallbackParams{rtxnv::kBuildParamsVersion, &fallback, 0, nullptr};
                gpu.list->ResourceBarrier(1, &uav);
                restoresBeforeInactive = restores;
                inactiveBuild = builder.BuildInactive(gpu.list.Get(), fallbackParams);
                gpu.list->ResourceBarrier(1, &uav);
                fallback.inputs.flags |= rtxnv::kBuildPerformUpdate;
                fallback.sourceAccelerationStructureData = fallback.destAccelerationStructureData;
                inactiveUpdate = builder.Build(gpu.list.Get(), fallbackParams, restore);
                inactiveUpdateRecorded = restores != restoresBeforeInactive; // checked now: later layouts restore again
            }
        }
        gpu.list->ResourceBarrier(1, &uav);
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances1b;
        for (int k = 0; k < 4; ++k)
        {
            const auto& L = layouts[k];
            if (!L.as || L.build != rtxnv::kOk) continue;
            instanceLayout.push_back(k);
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
            instance.Transform[0][3] = L.offsetX;
            instance.InstanceID = L.instanceId;
            instance.InstanceMask = L.mask;
            instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE; // as a game's LSS instance: culling on
            instance.AccelerationStructure = L.as->GetGPUVirtualAddress();
            instances1b.push_back(instance);
        }
        if (!instances1b.empty())
        {
            instances1bBuffer = gpu.Upload(instances1b.data(), instances1b.size() * sizeof(instances1b[0]));
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top1b{};
            top1b.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            top1b.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            top1b.Inputs.NumDescs = static_cast<UINT>(instances1b.size());
            top1b.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            top1b.Inputs.InstanceDescs = instances1bBuffer->GetGPUVirtualAddress();
            if (patcherReady)
            {
                const UINT64 bytes = instances1b.size() * sizeof(instances1b[0]);
                patchedInstances = gpu.Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                patchRecorded = patcher.Record(gpu.list.Get(), instances1bBuffer->GetGPUVirtualAddress(), static_cast<uint32_t>(instances1b.size()),
                    patchedInstances->GetGPUVirtualAddress());
                D3D12_RESOURCE_BARRIER barriers[2]{};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barriers[0].UAV.pResource = patchedInstances.Get();
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = patchedInstances.Get();
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                gpu.list->ResourceBarrier(2, barriers);
                if (patchRecorded) top1b.Inputs.InstanceDescs = patchedInstances->GetGPUVirtualAddress();
            }
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo1b{};
            gpu.device->GetRaytracingAccelerationStructurePrebuildInfo(&top1b.Inputs, &topInfo1b);
            tlas1b = gpu.Buffer(topInfo1b.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            tlas1bScratch = gpu.Buffer(topInfo1b.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            top1b.DestAccelerationStructureData = tlas1b->GetGPUVirtualAddress();
            top1b.ScratchAccelerationStructureData = tlas1bScratch->GetGPUVirtualAddress();
            gpu.list->BuildRaytracingAccelerationStructure(&top1b, 0, nullptr);
            gpu.list->ResourceBarrier(1, &uav);
        }
        gpu.Submit();
        // The patched instances: culling off exactly on the hair instances, every other byte
        // as the game wrote it.
        if (patchRecorded)
        {
            const UINT64 bytes = instances1b.size() * sizeof(instances1b[0]);
            auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition.pResource = patchedInstances.Get();
            toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            gpu.list->ResourceBarrier(1, &toCopy);
            gpu.list->CopyBufferRegion(readback.Get(), 0, patchedInstances.Get(), 0, bytes);
            gpu.Submit();
            void* mapped = nullptr;
            const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
            CHECK_HR(readback->Map(0, &read, &mapped));
            for (size_t i = 0; i < instances1b.size(); ++i)
            {
                D3D12_RAYTRACING_INSTANCE_DESC want = instances1b[i], got{};
                if (instanceLayout[i] < 3) want.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
                std::memcpy(&got, static_cast<const uint8_t*>(mapped) + i * sizeof(got), sizeof(got));
                ++patchedChecked;
                if (std::memcmp(&got, &want, sizeof(got)) == 0) ++patchedOk;
            }
            readback->Unmap(0, nullptr);
        }
        // Rays across each segment's axis, per built layout (D included: nothing may be hit).
        for (uint32_t k = 0; k < 4 && tlas1b; ++k)
        {
            const auto& L = layouts[k];
            if (!L.as || L.build != rtxnv::kOk) continue;
            for (uint32_t i = 0; i < segments; ++i)
            {
                const F3 p = truthP[i].xyz, q = truthQ[i].xyz;
                const F3 m = p + (q - p) * range(0.25f, 0.75f);
                const F3 n = Normalize(Cross(Normalize(q - p), Normalize({range(-1, 1), range(-1, 1), range(-1, 1)})));
                RayIn ray{};
                ray.origin = m + n * 0.25f;
                ray.origin.x += L.offsetX;
                ray.direction = n * -1.0f;
                ray.tmax = 0.5f;
                ray.mask = L.mask;
                rays1b.push_back(ray);
                rayLayout.push_back(k);
            }
        }
        if (!rays1b.empty())
        {
            rays1bBuffer = gpu.Upload(rays1b.data(), rays1b.size() * sizeof(RayIn));
            traceTlas = tlas1b->GetGPUVirtualAddress();
            traceRays = rays1bBuffer->GetGPUVirtualAddress();
            traceCount = static_cast<UINT>(rays1b.size());
            constants[0] = traceCount;
            for (int k = 0; k < 2; ++k)
            {
                auto& pass = lssPasses[k];
                if (pass.dxil.empty()) continue;
                std::vector<uint8_t> raw;
                std::string error;
                const bool ran = k == 1 ? runRays(pass.dxil.data(), pass.dxil.size(), sizeof(LssOut), raw, error)
                                        : runCompute(pass.dxil.data(), pass.dxil.size(), sizeof(LssOut), raw, error);
                if (!ran) continue;
                std::vector<LssOut> results;
                unpack(raw, results);
                for (UINT i = 0; i < traceCount; ++i)
                {
                    auto& L = layouts[rayLayout[i]];
                    LssGroup& g = L.groups[k];
                    const LssOut& hit = results[i];
                    const RayIn& ray = rays1b[i];
                    ++g.rays;
                    if (hit.status != 1) continue;
                    ++g.hits;
                    auto sample = [&](const char* what) {
                        if (g.samples.size() >= 4) return;
                        char text[256];
                        std::snprintf(text, sizeof(text), "ray %u: %s (isLss %u prim %u t %g bary %g %g hitParam %g)", i, what, hit.isLss,
                            hit.primitive, hit.t, hit.bary[0], hit.bary[1], hit.hitParam);
                        g.samples.push_back(text);
                    };
                    if (hit.instanceId != L.instanceId) { ++g.wrong; sample("wrong instance"); continue; }
                    if (hit.isLss) ++g.lss;
                    if (!hit.isLss || hit.primitive >= segments) { ++g.wrong; sample("not an LSS hit on a segment"); continue; }
                    const F4 tp = L.p[hit.primitive], tq = L.q[hit.primitive];
                    const F4 sp = Shell(tp), sq = Shell(tq); // the strand hits are reported on
                    const double decode = std::max({std::fabs(hit.p[0] - tp.xyz.x), std::fabs(hit.p[1] - tp.xyz.y), std::fabs(hit.p[2] - tp.xyz.z),
                        std::fabs(hit.q[0] - tq.xyz.x), std::fabs(hit.q[1] - tq.xyz.y), std::fabs(hit.q[2] - tq.xyz.z),
                        std::fabs(hit.p[3] - tp.w), std::fabs(hit.q[3] - tq.w)});
                    g.decodeError = std::max(g.decodeError, decode);
                    if (decode < 2e-4) ++g.segmentOk;
                    else sample("endpoints differ from the segment");
                    const double o[3] = {double(ray.origin.x) - L.offsetX, ray.origin.y, ray.origin.z};
                    const double d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
                    auto at = [&](double t, double x[3]) { for (int c = 0; c < 3; ++c) x[c] = o[c] + d[c] * t; };
                    double start[3];
                    at(0.0, start);
                    const bool inside = SweptDistance(start, sp, sq) <= 0;
                    bool enters = false;
                    for (int s = 1; s <= 4000 && !enters && !inside; ++s)
                    {
                        double x[3];
                        at(ray.tmax * s / 4000.0, x);
                        enters = SweptDistance(x, sp, sq) < 0;
                    }
                    double x[3];
                    at(hit.t, x);
                    if (!enters)
                    {
                        ++g.fallback;
                        if (inside) ++g.inside;
                        bool ribbon = false;
                        for (uint32_t tri = 0; tri < kTriangles && !ribbon; ++tri)
                        {
                            const F3* v = &L.dots[(static_cast<size_t>(hit.primitive) * kTriangles + tri) * 3];
                            double tt, b1, b2;
                            ribbon = RayTriangle(o, d, v[0], v[1], v[2], tt, b1, b2) && std::fabs(tt - hit.t) < 1e-4;
                        }
                        if (ribbon) ++g.fallbackOk;
                        else sample("fallback distance is not a ribbon's");
                    }
                    else
                    {
                        ++g.expectSurface;
                        const double surface = std::fabs(SweptDistance(x, sp, sq));
                        g.surfaceError = std::max(g.surfaceError, surface);
                        bool earlier = false;
                        for (int s = 0; s < 400 && hit.t > 1e-4; ++s)
                        {
                            double y[3];
                            at((hit.t - 1e-4) * s / 400.0, y);
                            earlier = earlier || SweptDistance(y, sp, sq) < -1e-6;
                        }
                        if (surface < 5e-5 && !earlier) ++g.surfaceOk;
                        else sample(earlier ? "an earlier entry into the strand" : "hit distance off the strand surface");
                    }
                    const double axis[3] = {double(tq.xyz.x) - tp.xyz.x, double(tq.xyz.y) - tp.xyz.y, double(tq.xyz.z) - tp.xyz.z};
                    const double len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
                    const double along = ((x[0] - tp.xyz.x) * axis[0] + (x[1] - tp.xyz.y) * axis[1] + (x[2] - tp.xyz.z) * axis[2]) / len2;
                    const double u = std::clamp(along, 0.0, 1.0);
                    g.uError = std::max({g.uError, std::fabs(hit.bary[0] - u), std::fabs(hit.hitParam - u), std::fabs(double(hit.bary[1]))});
                }
            }
        }
    }
    const rtx_hair::BuilderStats builderStats = builder.Stats();

    auto expected = [&](uint32_t b, uint32_t primitive, F3 out[3]) {
        const auto& blas = blases[b];
        if (blas.dots)
        {
            const auto& source = blas.tagged ? dotsTagged : dotsPlain;
            for (int k = 0; k < 3; ++k) out[k] = source[primitive * 3 + k];
        }
        else
            for (int k = 0; k < 3; ++k) out[k] = meshVertices[meshIndices[primitive * 3 + k]];
    };
    for (auto& pass : passes)
    {
        if (pass.results.empty()) continue;
        for (UINT i = 0; i < rayCount; ++i)
        {
            const uint32_t b = rayBlas[i];
            const auto& blas = blases[b];
            const HitOut& hit = pass.results[i];
            Group& g = pass.groups[b];
            ++g.rays;
            if (hit.status != 1) continue;
            ++g.hits;
            if (hit.instanceId != blas.instanceId) { ++g.wrongInstance; continue; }
            const uint32_t primitives = blas.dots ? segments * kTriangles : meshTriangles;
            if (hit.primitive >= primitives) { ++g.wrongInstance; continue; }
            F3 want[3];
            expected(b, hit.primitive, want);
            const F3 got[3] = {hit.v0, hit.v1, hit.v2};
            const bool exact = SameBits(got[0], want[0]) && SameBits(got[1], want[1]) && SameBits(got[2], want[2]);
            if (exact) ++g.exact;
            else
            {
                bool permuted = true;
                for (int k = 0; k < 3 && permuted; ++k)
                    permuted = SameBits(got[k], want[0]) || SameBits(got[k], want[1]) || SameBits(got[k], want[2]);
                if (permuted) ++g.sameSetOtherOrder;
                if (g.samples.size() < 4)
                {
                    char text[256];
                    std::snprintf(text, sizeof(text), "prim %u got (%g %g %g) (%g %g %g) (%g %g %g) want (%g %g %g)", hit.primitive,
                        got[0].x, got[0].y, got[0].z, got[1].x, got[1].y, got[1].z, got[2].x, got[2].y, got[2].z, want[0].x, want[0].y, want[0].z);
                    g.samples.push_back(text);
                }
            }
            if (Dot(got[0], got[0]) == 0 && Dot(got[1], got[1]) == 0 && Dot(got[2], got[2]) == 0) ++g.zero;
            if (hit.tagged) ++g.tagged;
            if (blas.dots)
            {
                const uint32_t segment = hit.primitive / kTriangles;
                F4 p, q;
                DotsDecode(got[0], got[1], got[2], hit.primitive, p, q);
                const F4 tp = truthP[segment], tq = truthQ[segment];
                auto posErr = [](F3 a, F3 b) { return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}); };
                g.posError = std::max<double>({g.posError, posErr(p.xyz, tp.xyz), posErr(q.xyz, tq.xyz)});
                g.radiusError = std::max<double>({g.radiusError, std::fabs(p.w - tp.w), std::fabs(q.w - tq.w)});
                const F3 sp{hit.p[0], hit.p[1], hit.p[2]}, sq{hit.q[0], hit.q[1], hit.q[2]};
                g.shaderPosError = std::max<double>({g.shaderPosError, posErr(sp, tp.xyz), posErr(sq, tq.xyz)});
                g.shaderRadiusError = std::max<double>({g.shaderRadiusError, std::fabs(hit.p[3] - tp.w), std::fabs(hit.q[3] - tq.w)});
            }
        }
    }

    // Phase 1a checks. Tagged DOTS: the shader must see an LSS hit on the segment it hit,
    // with that segment's endpoints, a hit distance on the round strand (first entry, by
    // the CPU's independent swept-sphere distance) or, where the ray only grazes a
    // ribbon outside the strand, the ribbon's own distance, and u on the segment.
    // Everything else: a plain triangle hit, exactly as without the rewrite.
    for (auto& pass : lssPasses)
    {
        if (pass.results.empty()) continue;
        for (UINT i = 0; i < rayCount; ++i)
        {
            const uint32_t b = rayBlas[i];
            const auto& blas = blases[b];
            const LssOut& hit = pass.results[i];
            LssGroup& g = pass.groups[b];
            ++g.rays;
            if (hit.status != 1) continue;
            ++g.hits;
            auto sample = [&](const char* what) {
                if (g.samples.size() >= 4) return;
                char text[256];
                std::snprintf(text, sizeof(text), "ray %u: %s (isLss %u prim %u t %g bary %g %g hitParam %g)", i, what, hit.isLss, hit.primitive,
                    hit.t, hit.bary[0], hit.bary[1], hit.hitParam);
                g.samples.push_back(text);
            };
            if (hit.instanceId != blas.instanceId) { ++g.wrong; sample("wrong instance"); continue; }
            if (hit.isLss) ++g.lss;
            const RayIn& ray = rays[i];
            const double o[3] = {double(ray.origin.x) - blas.offsetX, ray.origin.y, ray.origin.z};
            const double d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
            if (blas.tagged)
            {
                if (!hit.isLss || hit.primitive >= segments) { ++g.wrong; sample("not an LSS hit on a segment"); continue; }
                const F4 tp = truthP[hit.primitive], tq = truthQ[hit.primitive];
                const F4 sp = Shell(tp), sq = Shell(tq); // the strand hits are reported on
                const double decode = std::max({std::fabs(hit.p[0] - tp.xyz.x), std::fabs(hit.p[1] - tp.xyz.y), std::fabs(hit.p[2] - tp.xyz.z),
                    std::fabs(hit.q[0] - tq.xyz.x), std::fabs(hit.q[1] - tq.xyz.y), std::fabs(hit.q[2] - tq.xyz.z),
                    std::fabs(hit.p[3] - tp.w), std::fabs(hit.q[3] - tq.w)});
                g.decodeError = std::max(g.decodeError, decode);
                if (decode < 2e-4) ++g.segmentOk;
                else sample("endpoints differ from the segment");
                auto at = [&](double t, double x[3]) { for (int c = 0; c < 3; ++c) x[c] = o[c] + d[c] * t; };
                // Every probe ray has TMin 0. A ray that starts inside this segment's
                // strand (it passes close to a neighbouring strand) has no entry to find.
                double start[3];
                at(0.0, start);
                const bool inside = SweptDistance(start, sp, sq) <= 0;
                bool enters = false;
                for (int s = 1; s <= 4000 && !enters && !inside; ++s)
                {
                    double x[3];
                    at(ray.tmax * s / 4000.0, x);
                    enters = SweptDistance(x, sp, sq) < 0;
                }
                double x[3];
                at(hit.t, x);
                if (!enters)
                {
                    // No entry from outside (a ribbon grazed outside the strand, or a start
                    // inside it): the shader must keep the ribbon's own distance, that of one
                    // of the segment's eight DOTS triangles.
                    ++g.fallback;
                    if (inside) ++g.inside;
                    bool ribbon = false;
                    for (uint32_t tri = 0; tri < kTriangles && !ribbon; ++tri)
                    {
                        const F3* v = &dotsTagged[(static_cast<size_t>(hit.primitive) * kTriangles + tri) * 3];
                        double tt, b1, b2;
                        ribbon = RayTriangle(o, d, v[0], v[1], v[2], tt, b1, b2) && std::fabs(tt - hit.t) < 1e-4;
                    }
                    if (ribbon) ++g.fallbackOk;
                    else sample(inside ? "starts inside the strand, distance is not a ribbon's" : "fallback distance is not a ribbon's");
                }
                else
                {
                    ++g.expectSurface;
                    const double surface = std::fabs(SweptDistance(x, sp, sq));
                    g.surfaceError = std::max(g.surfaceError, surface);
                    bool earlier = false;
                    for (int s = 0; s < 400 && hit.t > 1e-4; ++s)
                    {
                        double y[3];
                        at((hit.t - 1e-4) * s / 400.0, y);
                        earlier = earlier || SweptDistance(y, sp, sq) < -1e-6;
                    }
                    if (surface < 5e-5 && !earlier) ++g.surfaceOk;
                    else sample(earlier ? "an earlier entry into the strand" : "hit distance off the strand surface");
                }
                const double axis[3] = {double(tq.xyz.x) - tp.xyz.x, double(tq.xyz.y) - tp.xyz.y, double(tq.xyz.z) - tp.xyz.z};
                const double len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
                const double along = ((x[0] - tp.xyz.x) * axis[0] + (x[1] - tp.xyz.y) * axis[1] + (x[2] - tp.xyz.z) * axis[2]) / len2;
                const double u = std::clamp(along, 0.0, 1.0);
                g.uError = std::max({g.uError, std::fabs(hit.bary[0] - u), std::fabs(hit.hitParam - u), std::fabs(double(hit.bary[1]))});
            }
            else
            {
                if (hit.isLss) { ++g.wrong; sample("LSS hit on plain triangles"); continue; }
                const uint32_t primitives = blas.dots ? segments * kTriangles : meshTriangles;
                if (hit.primitive >= primitives) { ++g.wrong; sample("primitive out of range"); continue; }
                F3 v[3];
                expected(b, hit.primitive, v);
                double t, b1, b2;
                if (!RayTriangle(o, d, v[0], v[1], v[2], t, b1, b2)) { sample("ray misses the reported triangle"); continue; }
                // The hit point the shader's barycentrics describe, against the exact one (scene
                // units): a ribbon triangle near a tapered tip is micro-units wide, so its
                // barycentrics alone amplify float32 rounding by the inverse of that width.
                const auto point = [&](double w1, double w2, int c) {
                    const double a = c == 0 ? v[0].x : c == 1 ? v[0].y : v[0].z, e1 = (c == 0 ? v[1].x : c == 1 ? v[1].y : v[1].z) - a,
                                 e2 = (c == 0 ? v[2].x : c == 1 ? v[2].y : v[2].z) - a;
                    return a + w1 * e1 + w2 * e2;
                };
                double error = std::fabs(hit.t - t);
                for (int c = 0; c < 3; ++c) error = std::max(error, std::fabs(point(hit.bary[0], hit.bary[1], c) - point(b1, b2, c)));
                g.triangleError = std::max(g.triangleError, error);
                if (error < 1e-4) ++g.triangleOk;
                else sample("triangle hit changed");
            }
        }
    }

    // Strand coverage (CPU): how wide the DOTS triangles make a strand look against its true
    // diameter, for this tessellation and the previous two-ribbon one.
    const Coverage coverage = MeasureCoverage(&Tessellate, kTriangles), twoRibbons = MeasureCoverage(&TessellateTwoRibbons, 4);
    // Verdict. Tolerances: positions 2e-4 (scene ~1 unit, tag <= 7 ulp), radii 1e-4.
    std::vector<std::string> failures;
    if (std::fabs(coverage.mean - 1.0) > 0.01 || coverage.min < 0.89 || coverage.max > 1.06 || coverage.meanAbsError > 0.045
        || coverage.mismatch >= twoRibbons.mismatch)
        failures.push_back("coverage: three-ribbon strand width outside 0.89..1.06 of its diameter, or not closer than two ribbons");
    bool anyPass = false;
    for (const auto& pass : passes)
    {
        if (!pass.created) { failures.push_back(std::string(pass.name) + ": not created: " + pass.error); continue; }
        anyPass = true;
        for (uint32_t b = 0; b < 4; ++b)
        {
            const auto& g = pass.groups[b];
            const auto& blas = blases[b];
            const std::string where = std::string(pass.name) + "/" + blas.name + ": ";
            if (g.hits * 10 < g.rays * 9) failures.push_back(where + "fewer than 90% of rays hit");
            if (g.wrongInstance) failures.push_back(where + "hits on the wrong instance or primitive");
            if (!blas.dataAccess) continue; // informational: what fetch returns without the flag
            if (g.exact != g.hits - g.wrongInstance) failures.push_back(where + "fetched positions not bit-exact");
            if (blas.dots && (g.shaderPosError > 2e-4 || g.shaderRadiusError > 1e-4)) failures.push_back(where + "decode error over tolerance");
            if (blas.tagged ? g.tagged != g.hits - g.wrongInstance : g.tagged != 0) failures.push_back(where + "tag detection wrong");
        }
    }
    if (!toolsLoaded) failures.push_back("lss: DXC tools: " + toolsError);
    for (const auto& pass : lssPasses)
    {
        if (!toolsLoaded) break;
        if (!pass.rewritten) { failures.push_back(std::string(pass.name) + ": not rewritten: " + pass.error); continue; }
        if (!pass.created) { failures.push_back(std::string(pass.name) + ": rewritten shader refused: " + pass.error); continue; }
        for (uint32_t b = 0; b < 4; ++b)
        {
            const auto& g = pass.groups[b];
            const auto& blas = blases[b];
            const std::string where = std::string(pass.name) + "/" + blas.name + ": ";
            if (g.hits * 10 < g.rays * 9) failures.push_back(where + "fewer than 90% of rays hit");
            if (g.wrong) failures.push_back(where + "wrong hits (see samples)");
            if (blas.tagged)
            {
                if (g.lss != g.hits) failures.push_back(where + "not every hit reported as LSS");
                if (g.segmentOk != g.hits) failures.push_back(where + "endpoints not decoded within 2e-4");
                if (g.surfaceOk != g.expectSurface) failures.push_back(where + "hit distance not on the strand surface");
                if (g.fallback * 100 > g.hits * 15) failures.push_back(where + "more than 15% ribbon-only hits");
                if (g.fallbackOk != g.fallback) failures.push_back(where + "a ribbon-only hit without the ribbon's distance");
                if (g.uError > 2e-3) failures.push_back(where + "u / barycentrics off");
            }
            else
            {
                if (g.lss) failures.push_back(where + "plain triangles reported as LSS");
                if (g.triangleOk != g.hits - g.wrong) failures.push_back(where + "triangle hits changed by the rewrite");
            }
        }
    }
    // Phase 1b verdict.
    if (!builderReady) failures.push_back("build: DOTS builder not ready: " + builderError);
    else
    {
        if (restores != builderCalls) failures.push_back("build: the caller's compute state was not handed back once per build");
        for (int k = 0; k < 4; ++k)
        {
            const auto& L = layouts[k];
            const std::string where = std::string("build/") + L.name + ": ";
            if (L.prebuild != rtxnv::kOk) { failures.push_back(where + "prebuild failed (" + std::to_string(L.prebuild) + ")"); continue; }
            if (L.build != rtxnv::kOk) { failures.push_back(where + "build failed (" + std::to_string(L.build) + "): " + builder.LastError()); continue; }
            if (k == 0 && L.update != rtxnv::kOk) failures.push_back(where + "in-place update failed (" + std::to_string(L.update) + ")");
            for (int p = 0; p < 2; ++p)
            {
                if (lssPasses[p].dxil.empty()) continue;
                const auto& g = L.groups[p];
                const std::string at = where + lssPasses[p].name + ": ";
                if (k == 3)
                {
                    if (g.hits) failures.push_back(at + "an unsupported layout was hit (it must be inactive)");
                    continue;
                }
                if (g.hits * 10 < g.rays * 9) failures.push_back(at + "fewer than 90% of rays hit");
                if (g.wrong) failures.push_back(at + "wrong hits (see samples)");
                if (g.lss != g.hits) failures.push_back(at + "not every hit reported as LSS");
                if (g.segmentOk != g.hits) failures.push_back(at + "endpoints not decoded within 2e-4");
                if (g.surfaceOk != g.expectSurface) failures.push_back(at + "hit distance not on the strand surface");
                if (g.fallbackOk != g.fallback) failures.push_back(at + "a ribbon-only hit without the ribbon's distance");
                if (g.fallback * 100 > g.hits * 15) failures.push_back(at + "more than 15% ribbon-only hits");
                if (g.uError > 2e-3) failures.push_back(at + "u / barycentrics off");
            }
        }
        if (builderStats.invalidGeometries != 1) failures.push_back("build: expected exactly one rejected (inactive) geometry");
        if (inactiveBuild != rtxnv::kOk || builderStats.inactiveBuilds != 1)
            failures.push_back("build: the inactive fallback build failed (" + std::to_string(inactiveBuild) + ")");
        if (inactiveUpdate != rtxnv::kOk || builderStats.skippedUpdates != 1 || inactiveUpdateRecorded)
            failures.push_back("build: an update of an inactive BLAS was not skipped without recording");
        if (rebuildUpdate != rtxnv::kOk || builderStats.rebuilds != 1 || builderStats.updates != 2)
            failures.push_back("build: the second update of layout 0 was not rebuilt in place (" + std::to_string(rebuildUpdate) + ")");
        if (!patcherReady) failures.push_back("patch: instance patch not ready: " + patcherError);
        else if (!patchRecorded) failures.push_back("patch: the instance patch recorded nothing");
        else if (patchedChecked == 0 || patchedOk != patchedChecked)
            failures.push_back("patch: hair instances' culling not disabled exactly (or another byte changed)");
    }
    bool fetchUnsupported = false;
    for (const auto& op : ops)
        if ((op.code == 86 || op.code == 98) && op.status == NVAPI_OK && !op.supported) fetchUnsupported = true;
    const int exitCode = !anyPass ? 3 : (failures.empty() ? 0 : (fetchUnsupported ? 3 : 1));

    // Report.
    std::ostringstream json;
    json << "{\n  \"adapter\": \"" << Json(adapterName) << "\", \"vendorId\": " << adapterDesc.VendorId << ", \"deviceId\": " << adapterDesc.DeviceId
         << ",\n  \"driver\": " << driver << ", \"branch\": \"" << Json(branch) << "\", \"seed\": " << options.seed
         << ",\n  \"segments\": " << segments << ", \"meshTriangles\": " << meshTriangles << ", \"rays\": " << rayCount << ",\n  \"caps\": {";
    for (size_t i = 0; i < std::size(caps); ++i)
        json << (i ? ", " : "") << "\"" << caps[i].name << "\": {\"status\": " << caps[i].status << ", \"value\": " << caps[i].value << "}";
    json << "},\n  \"ops\": {";
    for (size_t i = 0; i < std::size(ops); ++i)
        json << (i ? ", " : "") << "\"" << ops[i].code << "\": {\"name\": \"" << ops[i].name << "\", \"status\": " << ops[i].status
             << ", \"supported\": " << (ops[i].supported ? "true" : "false") << "}";
    json << "},\n  \"blas\": [";
    for (size_t i = 0; i < std::size(blases); ++i)
        json << (i ? ", " : "") << "{\"name\": \"" << blases[i].name << "\", \"dataAccess\": " << (blases[i].dataAccess ? "true" : "false")
             << ", \"bytesWithDataAccess\": " << blases[i].sizeWith << ", \"bytesWithout\": " << blases[i].sizeWithout << "}";
    json << "],\n  \"passes\": [";
    for (size_t p = 0; p < 2; ++p)
    {
        const auto& pass = passes[p];
        json << (p ? ",\n" : "\n") << "    {\"name\": \"" << pass.name << "\", \"created\": " << (pass.created ? "true" : "false")
             << ", \"error\": \"" << Json(pass.error) << "\", \"groups\": [";
        for (uint32_t b = 0; b < 4; ++b)
        {
            const auto& g = pass.groups[b];
            json << (b ? ", " : "") << "\n      {\"blas\": \"" << blases[b].name << "\", \"rays\": " << g.rays << ", \"hits\": " << g.hits
                 << ", \"wrongInstance\": " << g.wrongInstance << ", \"exact\": " << g.exact << ", \"sameSetOtherOrder\": " << g.sameSetOtherOrder
                 << ", \"zero\": " << g.zero << ", \"tagged\": " << g.tagged << ", \"posError\": " << g.posError << ", \"radiusError\": " << g.radiusError
                 << ", \"shaderPosError\": " << g.shaderPosError << ", \"shaderRadiusError\": " << g.shaderRadiusError << ", \"samples\": [";
            for (size_t s = 0; s < g.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(g.samples[s]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ],\n  \"lss\": {\"tools\": \"" << Json(toolsLoaded ? std::string("loaded") : toolsError) << "\", \"passes\": [";
    for (size_t p = 0; p < 2; ++p)
    {
        const auto& pass = lssPasses[p];
        json << (p ? ",\n" : "\n") << "    {\"name\": \"" << pass.name << "\", \"rewritten\": " << (pass.rewritten ? "true" : "false")
             << ", \"created\": " << (pass.created ? "true" : "false") << ", \"error\": \"" << Json(pass.error) << "\", \"bytes\": ["
             << pass.originalBytes << ", " << pass.rewrittenBytes << "], \"report\": {\"functions\": " << pass.report.functions
             << ", \"hitContexts\": " << pass.report.hitContexts << ", \"queryContexts\": " << pass.report.queryContexts << ", \"ops\": "
             << pass.report.ops << ", \"reads\": " << pass.report.reads << "}, \"groups\": [";
        for (uint32_t b = 0; b < 4; ++b)
        {
            const auto& g = pass.groups[b];
            json << (b ? ", " : "") << "\n      {\"blas\": \"" << blases[b].name << "\", \"rays\": " << g.rays << ", \"hits\": " << g.hits
                 << ", \"lss\": " << g.lss << ", \"wrong\": " << g.wrong << ", \"segmentOk\": " << g.segmentOk << ", \"expectSurface\": "
                 << g.expectSurface << ", \"surfaceOk\": " << g.surfaceOk << ", \"fallback\": " << g.fallback << ", \"inside\": " << g.inside << ", \"fallbackOk\": " << g.fallbackOk
                 << ", \"triangleOk\": " << g.triangleOk
                 << ", \"decodeError\": " << g.decodeError << ", \"surfaceError\": " << g.surfaceError << ", \"uError\": " << g.uError
                 << ", \"triangleError\": " << g.triangleError << ", \"samples\": [";
            for (size_t s = 0; s < g.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(g.samples[s]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ]},\n  \"build\": {\"ready\": " << (builderReady ? "true" : "false") << ", \"error\": \"" << Json(builderError)
         << "\", \"lastError\": \"" << Json(builder.LastError()) << "\", \"restores\": " << restores << ", \"calls\": " << builderCalls
         << ", \"stats\": {\"prebuilds\": " << builderStats.prebuilds << ", \"builds\": " << builderStats.builds << ", \"updates\": "
         << builderStats.updates << ", \"geometries\": " << builderStats.geometries << ", \"primitives\": " << builderStats.primitives
         << ", \"invalidGeometries\": " << builderStats.invalidGeometries << ", \"failures\": " << builderStats.failures
         << ", \"inactiveBuilds\": " << builderStats.inactiveBuilds << ", \"skippedUpdates\": " << builderStats.skippedUpdates
         << ", \"inactiveBuild\": " << inactiveBuild << ", \"inactiveUpdate\": " << inactiveUpdate << ", \"rebuilds\": " << builderStats.rebuilds
         << ", \"rebuildUpdate\": " << rebuildUpdate << ", \"patch\": {\"ready\": " << (patcherReady ? "true" : "false") << ", \"recorded\": "
         << (patchRecorded ? "true" : "false") << ", \"instancesOk\": " << patchedOk << ", \"instances\": " << patchedChecked << ", \"hairBlas\": "
         << patcher.Size() << "}, \"poolBuffers\": "
         << builderStats.poolBuffers << ", \"poolBytes\": " << builderStats.poolBytes << "}, \"layouts\": [";
    for (int k = 0; k < 4; ++k)
    {
        const auto& L = layouts[k];
        json << (k ? ",\n" : "\n") << "    {\"name\": \"" << L.name << "\", \"prebuild\": " << L.prebuild << ", \"build\": " << L.build
             << ", \"update\": " << L.update << ", \"resultBytes\": " << L.info.ResultDataMaxSizeInBytes << ", \"scratchBytes\": "
             << L.info.ScratchDataSizeInBytes << ", \"passes\": [";
        for (int p = 0; p < 2; ++p)
        {
            const auto& g = L.groups[p];
            json << (p ? ", " : "") << "\n      {\"pass\": \"" << lssPasses[p].name << "\", \"rays\": " << g.rays << ", \"hits\": " << g.hits
                 << ", \"lss\": " << g.lss << ", \"wrong\": " << g.wrong << ", \"segmentOk\": " << g.segmentOk << ", \"expectSurface\": "
                 << g.expectSurface << ", \"surfaceOk\": " << g.surfaceOk << ", \"fallback\": " << g.fallback << ", \"inside\": " << g.inside
                 << ", \"fallbackOk\": " << g.fallbackOk << ", \"decodeError\": " << g.decodeError << ", \"surfaceError\": " << g.surfaceError
                 << ", \"uError\": " << g.uError << ", \"samples\": [";
            for (size_t s = 0; s < g.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(g.samples[s]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ]},\n  \"coverage\": {\"threeRibbons\": " << CoverageJson(coverage) << ", \"twoRibbons\": " << CoverageJson(twoRibbons)
         << "},\n  \"failures\": [";
    for (size_t i = 0; i < failures.size(); ++i) json << (i ? ", " : "") << "\"" << Json(failures[i]) << "\"";
    json << "],\n  \"exitCode\": " << exitCode << "\n}\n";
    const std::string report = json.str();
    std::fwrite(report.data(), 1, report.size(), stdout);
    if (!options.out.empty())
    {
        std::ofstream file(options.out, std::ios::binary);
        file << report;
        if (!file) Fail(2, "cannot write %ls", options.out.c_str());
    }
    std::fprintf(stderr, "RtxHairProbe: %s (exit %d)\n", exitCode == 0 ? "PASS" : (exitCode == 3 ? "position fetch unavailable" : "FAIL"), exitCode);
    return exitCode;
}
