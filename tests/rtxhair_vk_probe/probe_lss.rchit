#version 460
// vkhair.1 phase 1a, pass 2: a closest hit written for hardware LSS (gl_HitIsLSSNV, the endpoint and
// radius built-ins, u in the first hit attribute), rewritten by rtx_hair/lss_spirv.
#extension GL_EXT_ray_tracing : require
#extension GL_NV_linear_swept_spheres : require
#extension GL_GOOGLE_include_directive : require
#include "probe_lss_common.glsl"

layout(location = 0) rayPayloadInEXT uint payload;
hitAttributeEXT vec2 attributes;

void main()
{
    LssOut o = EmptyLss();
    o.status = 1u;
    o.isLss = gl_HitIsLSSNV ? 1u : 0u;
    o.primitive = uint(gl_PrimitiveID);
    o.instanceId = uint(gl_InstanceCustomIndexEXT);
    o.bary = attributes;
    o.t = gl_HitTEXT;
    o.hitParam = attributes.x;
    if (gl_HitIsLSSNV)
    {
        o.p = vec4(gl_HitLSSPositionsNV[0], gl_HitLSSRadiiNV[0]);
        o.q = vec4(gl_HitLSSPositionsNV[1], gl_HitLSSRadiiNV[1]);
    }
    results[params.rayOffset + gl_LaunchIDEXT.x] = o;
}
