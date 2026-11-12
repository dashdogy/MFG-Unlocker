// rtxhair.1 phase 0: NVAPI triangle position fetch and the DOTS endpoint decode.
// Compiled twice by main.cpp: -T cs_6_6 -E CSMain -D PROBE_COMPUTE (inline RayQuery,
// committed op 98) and -T lib_6_6 -D PROBE_LIBRARY (DXR closest hit, op 86).
#define NV_SHADER_EXTN_SLOT u127
#define NV_SHADER_EXTN_REGISTER_SPACE space99
#include "nvHLSLExtns.h"

struct RayIn
{
    float3 origin;
    float tmax;
    float3 direction;
    uint mask;
};

// 112 bytes; main.cpp's HitOut mirrors it.
struct HitOut
{
    uint status; // 0 miss, 1 triangle hit
    uint instanceId;
    uint primitive;
    uint geometry;
    float2 bary;
    float t;
    uint hitKind;
    float3 v0;
    uint tagged; // all nine fetched coordinates carry the DOTS tag
    float3 v1;
    uint pad1;
    float3 v2;
    uint pad2;
    float4 p; // decoded LSS endpoint 0 (xyz, radius)
    float4 q; // decoded LSS endpoint 1
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<RayIn> Rays : register(t1);
RWStructuredBuffer<HitOut> Results : register(u0);
cbuffer Params : register(b0)
{
    uint RayCount;
    uint TagMask;
    uint TagValue;
    uint Unused;
};

// converter.hlsl's ribbon half-width factor.
static const float kRibbon = 1.04719755120; // pi/3: rtx_hair/lss_ir.h kRibbon

// A DOTS segment is 6 triangles, kept in LSS primitive order: per ribbon (three,
// 60 degrees apart) (A, B, C) then (A, D, B), with A = p + a*rp*k, B = q - a*rq*k,
// C = q + a*rq*k, D = p - a*rp*k and the ribbon axis a perpendicular to q - p.
// Any triangle alone gives back p, q, rp and rq.
void DotsDecode(float3 v0, float3 v1, float3 v2, uint primitive, out float4 p, out float4 q)
{
    float flip = 1.0; // one winding (rtx_hair/lss_ir.h kWindings)
    if ((primitive & 1) == 0)
    {
        float3 qc = 0.5 * (v1 + v2);
        float3 d = v2 - v1;
        float len = length(d);
        float3 a = len > 0 ? d * (flip / len) : float3(0, 0, 0);
        float rq = len / (2 * kRibbon);
        float rp = dot(v0 - qc, a) / kRibbon;
        p = float4(v0 - a * (rp * kRibbon), rp);
        q = float4(qc, rq);
    }
    else
    {
        float3 pc = 0.5 * (v0 + v1);
        float3 d = v0 - v1;
        float len = length(d);
        float3 a = len > 0 ? d * (flip / len) : float3(0, 0, 0);
        float rp = len / (2 * kRibbon);
        float rq = -dot(v2 - pc, a) / kRibbon;
        p = float4(pc, rp);
        q = float4(v2 + a * (rq * kRibbon), rq);
    }
}

bool Tagged(float3 v)
{
    uint3 bits = asuint(v) & TagMask;
    return all(bits == TagValue);
}

void Fill(inout HitOut o, float3x3 v)
{
    o.v0 = v[0];
    o.v1 = v[1];
    o.v2 = v[2];
    o.tagged = (Tagged(v[0]) && Tagged(v[1]) && Tagged(v[2])) ? 1u : 0u;
    DotsDecode(v[0], v[1], v[2], o.primitive, o.p, o.q);
}

RayDesc MakeRay(RayIn r)
{
    RayDesc d;
    d.Origin = r.origin;
    d.Direction = r.direction;
    d.TMin = 0.0;
    d.TMax = r.tmax;
    return d;
}

#ifdef PROBE_COMPUTE
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= RayCount)
        return;
    RayIn r = Rays[i];
    RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
    rq.TraceRayInline(Scene, RAY_FLAG_NONE, r.mask, MakeRay(r));
    while (rq.Proceed())
    {
    }
    HitOut o = (HitOut)0;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        o.status = 1;
        o.instanceId = rq.CommittedInstanceID();
        o.primitive = rq.CommittedPrimitiveIndex();
        o.geometry = rq.CommittedGeometryIndex();
        o.bary = rq.CommittedTriangleBarycentrics();
        o.t = rq.CommittedRayT();
        o.hitKind = rq.CommittedTriangleFrontFace() ? 0xFEu : 0xFFu;
        Fill(o, NvRtCommittedTriangleObjectPositions(rq));
    }
    Results[i] = o;
}
#endif

#ifdef PROBE_LIBRARY
struct Payload
{
    uint unused;
};

[shader("raygeneration")]
void RayGen()
{
    uint i = DispatchRaysIndex().x;
    if (i >= RayCount)
        return;
    RayIn r = Rays[i];
    Payload payload;
    payload.unused = 0;
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, r.mask, 0, 1, 0, MakeRay(r), payload);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    Results[DispatchRaysIndex().x] = (HitOut)0;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    HitOut o = (HitOut)0;
    o.status = 1;
    o.instanceId = InstanceID();
    o.primitive = PrimitiveIndex();
    o.geometry = GeometryIndex();
    o.bary = attributes.barycentrics;
    o.t = RayTCurrent();
    o.hitKind = HitKind();
    Fill(o, NvRtTriangleObjectPositions());
    Results[DispatchRaysIndex().x] = o;
}
#endif
