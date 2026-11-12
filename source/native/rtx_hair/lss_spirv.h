#pragma once
// vkhair.1: universal RTX hair on Vulkan. SPIR-V written for hardware LSS (SPV_NV_linear_swept_spheres,
// RTX 50) rewritten to run on DOTS hair through triangle position fetch (SPV_KHR_ray_tracing_position_fetch):
// the Vulkan twin of lss_ir.h, with the same decode, marker and round-strand hit distance. Words in, words
// out (spirv_module.h); no compiler, no SPIRV-Tools.
//
// Hit shaders (any hit, closest hit): HitLSSPositionsNV, HitLSSRadiiNV and HitIsLSSNV become Private
// variables that each such entry point fills at its start from HitTriangleVertexPositionsKHR. In those
// modules, reads of PrimitiveId, RayTmaxKHR / HitTNV and the hit attributes return the LSS values (the
// segment, the distance to the round strand, u in the first float) when the hit carries the DOTS marker,
// and are unchanged otherwise. HitKind is left alone (GLSL_NV_linear_swept_spheres: it does not tell LSS
// hits apart).
// Ray queries: OpRayQueryGetIntersectionLSSPositionsNV / LSSRadiiNV / LSSHitValueNV / IsLSSHitNV become
// OpRayQueryGetIntersectionTriangleVertexPositionsKHR on the same query and intersection plus the decode;
// in functions that use them, the primitive index, T and barycentrics reads are redirected the same way.
// An LSS intersection is taken to report the triangle intersection type (the GLSL extension says the type
// does not tell LSS apart), which is what a DOTS hit reports.
// Not rewritten (the module takes the neutral form): sphere primitives, SER hit-object LSS ops, an LSS
// built-in or redirected built-in used other than by loads and access chains, or one shared with a
// function that a non-hit entry point reaches.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rtx_hair {
struct SpirvRewriteReport
{
    uint32_t hitEntryPoints{}; // any-hit / closest-hit entry points that decode the hit at their start
    uint32_t queryContexts{};  // ray-query decodes emitted (cached per block and query state)
    uint32_t ops{};            // LSS ray-query ops replaced
    uint32_t builtIns{};       // LSS built-in variables replaced
    uint32_t reads{};          // primitive index, hit distance and attribute / barycentric reads redirected
    uint32_t neutralised{};    // neutral form: LSS and sphere reads that now return zero / false
    std::string note;          // why the full rewrite fell back to the neutral form, or what was left alone
};

enum class SpirvRewrite
{
    Unchanged, // no LSS or sphere capability: not touched (out is empty)
    Rewritten, // LSS reads decoded from the hit triangle
    Neutral,   // LSS could not be rewritten: every LSS and sphere read is zero / false, so the module is
               // still valid on a device without LSS (its hair is missing in that pass)
    Failed,    // not even the neutral form (a malformed module, or an opcode the grammar does not know)
};

// Whether the module declares RayTracingLinearSweptSpheresGeometryNV or RayTracingSpheresGeometryNV
// (looks at its capability instructions only, which come first).
bool SpirvUsesLss(std::span<const uint32_t> words);

// hairmem.1: the DOTS layout the decode reads: triangles per segment and the ribbon half-width in LSS radii (the
// round strand's radius too). The default is the converter's default, three ribbons (lss_ir.h).
struct LssDecodeLayout
{
    uint32_t trianglesPerSegment = 6;
    float ribbon = 1.04719755120f;
};

// The full rewrite, else the neutral form. `error` says why the full rewrite failed (Neutral) or why
// nothing could be produced (Failed). `layout` must match the converter's (vk_dots_builder.h DotsOptions).
SpirvRewrite RewriteLssSpirv(std::span<const uint32_t> words, std::vector<uint32_t>& out, SpirvRewriteReport& report, std::string& error,
    const LssDecodeLayout& layout = {});

// The neutral form alone (tests, and modules a caller already knows the full rewrite refuses).
SpirvRewrite NeutraliseLssSpirv(std::span<const uint32_t> words, std::vector<uint32_t>& out, SpirvRewriteReport& report, std::string& error);
}
