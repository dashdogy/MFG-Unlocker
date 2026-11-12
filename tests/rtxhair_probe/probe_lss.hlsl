// rtxhair.1 phase 1a: a hair shader written for RTX 50 (NVAPI LSS ops), run on RTX 40
// after source/native/rtx_hair/lss_rewrite.cpp rewrote it. Compiled twice by main.cpp:
// -T cs_6_6 -E CSMain -D PROBE_COMPUTE (inline RayQuery: committed ops 114, 106, 117)
// and -T lib_6_6 -D PROBE_LIBRARY (closest hit: ops 112, 104).
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

// 64 bytes; main.cpp's LssOut mirrors it.
struct LssOut
{
    uint status;    // 0 miss, 1 hit
    uint isLss;     // NvRtIsLssHit / NvRtCommittedIsLss
    uint primitive; // PrimitiveIndex as the shader sees it (LSS: the segment)
    uint instanceId;
    float2 bary;    // barycentrics as the shader sees them (LSS: x = u)
    float t;        // RayTCurrent / CommittedRayT as the shader sees it
    float hitParam; // ray query: NvRtCommittedLssHitParameter; closest hit: barycentrics.x
    float4 p;       // LSS endpoint 0 (xyz, radius), only read on LSS hits
    float4 q;       // LSS endpoint 1
};

// Rays at tagged DOTS hair (instance masks 0x01, 0x10-0x80) cull back faces, as Witcher 3's
// shadow and path-tracing rays do: an LSS hit is never culled, so every ribbon must still be
// hit. DOTS ribbons are single-sided; universal hair disables culling on their instances
// (rtx_hair/instance_patch.hlsl, applied to phase 1b's TLAS; phase 1a's sets it directly).
uint HairCull(uint mask)
{
    return (mask & 0xF1u) != 0 ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE;
}

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<RayIn> Rays : register(t1);
RWStructuredBuffer<LssOut> Results : register(u0);
cbuffer Params : register(b0)
{
    uint RayCount;
    uint Unused0;
    uint Unused1;
    uint Unused2;
};

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
    rq.TraceRayInline(Scene, HairCull(r.mask), r.mask, MakeRay(r));
    while (rq.Proceed())
    {
    }
    LssOut o = (LssOut)0;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        o.status = 1;
        o.isLss = NvRtCommittedIsLss(rq) ? 1u : 0u;
        o.primitive = rq.CommittedPrimitiveIndex();
        o.instanceId = rq.CommittedInstanceID();
        o.bary = rq.CommittedTriangleBarycentrics();
        o.t = rq.CommittedRayT();
        if (o.isLss)
        {
            o.hitParam = NvRtCommittedLssHitParameter(rq);
            float2x4 m = NvRtCommittedLssObjectPositionsAndRadii(rq);
            o.p = m[0];
            o.q = m[1];
        }
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
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE | HairCull(r.mask), r.mask, 0, 1, 0, MakeRay(r), payload);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    Results[DispatchRaysIndex().x] = (LssOut)0;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    LssOut o = (LssOut)0;
    o.status = 1;
    o.isLss = NvRtIsLssHit() ? 1u : 0u;
    o.primitive = PrimitiveIndex();
    o.instanceId = InstanceID();
    o.bary = attributes.barycentrics;
    o.t = RayTCurrent();
    o.hitParam = attributes.barycentrics.x;
    if (o.isLss)
    {
        float2x4 m = NvRtLssObjectPositionsAndRadii();
        o.p = m[0];
        o.q = m[1];
    }
    Results[DispatchRaysIndex().x] = o;
}
#endif
