// vkhair.1 phase 0: shared by probe.comp, probe.rgen, probe.rmiss and probe.rchit (main.cpp builds
// them with the Vulkan SDK's glslangValidator). The Vulkan twin of tests/rtxhair_probe/probe.hlsl.

struct RayIn
{
    vec3 origin;
    float tmax;
    vec3 direction;
    uint mask;
};

// 112 bytes in std430; main.cpp's HitOut mirrors it (as the D3D12 probe's).
struct HitOut
{
    uint status; // 0 miss, 1 triangle hit (main.cpp fills the buffer with 0xFF: never written)
    uint instanceId;
    uint primitive;
    uint geometry;
    vec2 bary;
    float t;
    uint hitKind;
    vec3 v0;
    uint tagged; // all nine fetched coordinates carry the DOTS tag
    vec3 v1;
    uint pad1;
    vec3 v2;
    uint pad2;
    vec4 p; // decoded LSS endpoint 0 (xyz, radius)
    vec4 q; // decoded LSS endpoint 1
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT Scene;
layout(set = 0, binding = 1, std430) readonly buffer RayBuffer { RayIn rays[]; };
layout(set = 0, binding = 2, std430) writeonly buffer ResultBuffer { HitOut results[]; };
layout(push_constant) uniform Params
{
    uint rayOffset; // this dispatch traces rays[rayOffset + i] into results[rayOffset + i]
    uint rayCount;
    uint tagMask;
    uint tagValue;
} params;

// Ribbon half-width in LSS radii: pi/3 (rtx_hair/lss_ir.h kRibbon).
const float kRibbon = 1.04719755120;

// A DOTS segment is 6 triangles in LSS primitive order: per ribbon (three, 60 degrees apart)
// (A, B, C) then (A, D, B), with A = p + a*rp*k, B = q - a*rq*k, C = q + a*rq*k, D = p - a*rp*k and the
// ribbon axis a perpendicular to q - p. Any triangle alone gives back p, q, rp and rq.
void DotsDecode(vec3 v0, vec3 v1, vec3 v2, uint primitive, out vec4 p, out vec4 q)
{
    if ((primitive & 1u) == 0u)
    {
        vec3 qc = 0.5 * (v1 + v2);
        vec3 d = v2 - v1;
        float len = length(d);
        vec3 a = len > 0.0 ? d / len : vec3(0.0);
        float rq = len / (2.0 * kRibbon);
        float rp = dot(v0 - qc, a) / kRibbon;
        p = vec4(v0 - a * (rp * kRibbon), rp);
        q = vec4(qc, rq);
    }
    else
    {
        vec3 pc = 0.5 * (v0 + v1);
        vec3 d = v0 - v1;
        float len = length(d);
        vec3 a = len > 0.0 ? d / len : vec3(0.0);
        float rp = len / (2.0 * kRibbon);
        float rq = -dot(v2 - pc, a) / kRibbon;
        p = vec4(pc, rp);
        q = vec4(v2 + a * (rq * kRibbon), rq);
    }
}

bool Tagged(vec3 v)
{
    uvec3 bits = floatBitsToUint(v) & uvec3(params.tagMask);
    return all(equal(bits, uvec3(params.tagValue)));
}

HitOut Empty()
{
    HitOut o;
    o.status = 0u;
    o.instanceId = 0u;
    o.primitive = 0u;
    o.geometry = 0u;
    o.bary = vec2(0.0);
    o.t = 0.0;
    o.hitKind = 0u;
    o.v0 = vec3(0.0);
    o.tagged = 0u;
    o.v1 = vec3(0.0);
    o.pad1 = 0u;
    o.v2 = vec3(0.0);
    o.pad2 = 0u;
    o.p = vec4(0.0);
    o.q = vec4(0.0);
    return o;
}

void Fill(inout HitOut o, vec3 v0, vec3 v1, vec3 v2)
{
    o.v0 = v0;
    o.v1 = v1;
    o.v2 = v2;
    o.tagged = (Tagged(v0) && Tagged(v1) && Tagged(v2)) ? 1u : 0u;
    vec4 p, q;
    DotsDecode(v0, v1, v2, o.primitive, p, q);
    o.p = p;
    o.q = q;
}
