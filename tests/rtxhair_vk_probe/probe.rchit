#version 460
// vkhair.1 phase 0, pass 2: closest hit; the hit triangle's vertices through BuiltIn
// HitTriangleVertexPositionsKHR (VK_KHR_ray_tracing_position_fetch).
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_tracing_position_fetch : require
#extension GL_GOOGLE_include_directive : require
#include "probe_common.glsl"

layout(location = 0) rayPayloadInEXT uint payload;
hitAttributeEXT vec2 attributes;

void main()
{
    HitOut o = Empty();
    o.status = 1u;
    o.instanceId = uint(gl_InstanceCustomIndexEXT);
    o.primitive = uint(gl_PrimitiveID);
    o.geometry = uint(gl_GeometryIndexEXT);
    o.bary = attributes;
    o.t = gl_HitTEXT;
    o.hitKind = gl_HitKindEXT;
    Fill(o, gl_HitTriangleVertexPositionsEXT[0], gl_HitTriangleVertexPositionsEXT[1], gl_HitTriangleVertexPositionsEXT[2]);
    results[params.rayOffset + gl_LaunchIDEXT.x] = o;
}
