#pragma once
// Universal RTX hair (rtxhair.1): run shaders written for hardware LSS (NVAPI linear
// swept spheres, RTX 50) on DOTS hair (crossed ribbons per segment, below) on GPUs
// without LSS.
//
// Every NVAPI LSS op is replaced, in DXIL text, by NVAPI triangle position fetch
// (ops 86 / 98) on the hit triangle, from which the segment's endpoints and radii are
// decoded: the DOTS BLAS is built with ALLOW_DATA_ACCESS_EX, so no game buffer, root
// parameter or per-game anchor is involved.
#include <cstdint>
#include <string>
#include <string_view>

namespace rtx_hair {
// The "is LSS" marker: the low mantissa bits of all nine coordinates of a DOTS
// triangle (written by the converter, below 8 ulp). A game triangle carries it only
// by chance (2^-27).
inline constexpr uint32_t kTagMask = 0x7u;
inline constexpr uint32_t kTagValue = 0x5u;
// DOTS: three ribbons 60 degrees apart around each segment, each two triangles (6 per
// segment, dots_converter.hlsl). One winding: an LSS hit is never culled, so universal
// hair sets TRIANGLE_CULL_DISABLE on every instance of a DOTS BLAS when the game builds
// its scene (instance_patch.h). kWindings = 2 would add each triangle reversed instead.
inline constexpr uint32_t kRibbons = 3;
inline constexpr uint32_t kWindings = 1;
inline constexpr uint32_t kTrianglesPerSegment = kRibbons * 2 * kWindings;
// Half-width of a DOTS ribbon in LSS radii: pi/3, so the strand's width averaged over view
// angles is exactly its diameter (between 0.907 and 1.047 of it at any one angle).
inline constexpr float kRibbon = 1.04719755120f;
// The rewritten shaders report a hit on the round strand of kHitRadius LSS radii, which
// encloses every DOTS ribbon (as witcher_dots's RoundHitDistance): a shading point inside
// the ribbons' reach would let the strand's own ribbon edges shadow the rays leaving it.
inline constexpr float kHitRadius = kRibbon;

struct IrRewriteReport
{
    uint32_t functions{};     // functions that use an NVAPI LSS op
    uint32_t hitContexts{};   // closest/any hit: one vertex fetch at entry
    uint32_t queryContexts{}; // ray query: one vertex fetch per block and query
    uint32_t ops{};           // NVAPI LSS ops replaced
    uint32_t reads{};         // primitive index, barycentric and hit-distance reads redirected
};

// Whether DXIL text (DXC disassembly) stores an NVAPI LSS opcode (104-107, 112-119).
bool UsesLss(std::string_view ir);

// Rewrites DXIL text for DOTS hair. Supported: NvRtLssObjectPositionsAndRadii and
// NvRtIsLssHit in closest/any hit shaders; NvRtCommittedLssObjectPositionsAndRadii,
// NvRtCommittedIsLss and NvRtCommittedLssHitParameter on ray queries. In those
// functions, reads of the primitive index, barycentrics and hit distance return the LSS
// values (segment, (u, 0), distance to the round strand) when the hit carries the DOTS
// marker, and are unchanged otherwise. Anything else involving LSS (candidate and
// HitObject ops, an unrecognized sequence) fails: the caller keeps the original shader.
// The output is unfinalized IR for DXC's optimizer (-hlsl-dxilload ... -hlsl-dxilfinalize).
bool RewriteLssIr(std::string_view ir, std::string& out, IrRewriteReport& report, std::string& error);
}
