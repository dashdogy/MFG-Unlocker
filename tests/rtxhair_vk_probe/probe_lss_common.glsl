// vkhair.1 phase 1a: shared by probe_lss.comp, probe_lss.rgen, probe_lss.rmiss and probe_lss.rchit, a
// hair shader written for RTX 50 (GL_NV_linear_swept_spheres) that main.cpp rewrites with
// source/native/rtx_hair/lss_spirv before creating it on this GPU. The Vulkan twin of
// tests/rtxhair_probe/probe_lss.hlsl.

struct RayIn
{
    vec3 origin;
    float tmax;
    vec3 direction;
    uint mask;
};

// 64 bytes in std430; main.cpp's LssOut mirrors it (as the D3D12 probe's).
struct LssOut
{
    uint status;     // 0 miss, 1 hit (0xFF fill: never written)
    uint isLss;      // gl_HitIsLSSNV / rayQueryIsLSSHitNV
    uint primitive;  // the primitive index as the shader sees it (LSS: the segment)
    uint instanceId;
    vec2 bary;       // the barycentrics / hit attributes as the shader sees them (LSS: x = u)
    float t;         // gl_HitTEXT / rayQueryGetIntersectionTEXT as the shader sees it
    float hitParam;  // ray query: rayQueryGetIntersectionLSSHitValueNV; closest hit: the first hit attribute
    vec4 p;          // LSS endpoint 0 (xyz, radius), only read on LSS hits
    vec4 q;          // LSS endpoint 1
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT Scene;
layout(set = 0, binding = 1, std430) readonly buffer RayBuffer { RayIn rays[]; };
layout(set = 0, binding = 2, std430) writeonly buffer ResultBuffer { LssOut results[]; };
layout(push_constant) uniform Params
{
    uint rayOffset;
    uint rayCount;
    uint cullMask; // instance masks of hair: rays at them cull back faces
    uint unused;
} params;

// Rays at hair (phase 1a: tagged DOTS, masks 0x01 and 0x10; phase 1b: every LSS layout) cull back faces, as
// Witcher 3's shadow and path-tracing rays do: an LSS hit is never culled, so every ribbon must still be hit.
// DOTS ribbons are single-sided; universal hair disables culling on their instances (phase 1a's TLAS sets it
// directly, phase 1b's comes from rtx_hair/vk_dots_builder's instance patch).
uint HairCull(uint mask)
{
    return (mask & params.cullMask) != 0u ? gl_RayFlagsCullBackFacingTrianglesEXT : gl_RayFlagsNoneEXT;
}

LssOut EmptyLss()
{
    LssOut o;
    o.status = 0u;
    o.isLss = 0u;
    o.primitive = 0u;
    o.instanceId = 0u;
    o.bary = vec2(0.0);
    o.t = 0.0;
    o.hitParam = 0.0;
    o.p = vec4(0.0);
    o.q = vec4(0.0);
    return o;
}
