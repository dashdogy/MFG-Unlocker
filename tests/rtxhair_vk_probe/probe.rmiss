#version 460
// vkhair.1 phase 0, pass 2: a miss writes an empty result.
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "probe_common.glsl"

layout(location = 0) rayPayloadInEXT uint payload;

void main()
{
    results[params.rayOffset + gl_LaunchIDEXT.x] = Empty();
}
