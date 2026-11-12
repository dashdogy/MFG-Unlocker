#version 460
// vkhair.1 phase 1a, pass 2: a miss writes an empty result.
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "probe_lss_common.glsl"

layout(location = 0) rayPayloadInEXT uint payload;

void main()
{
    results[params.rayOffset + gl_LaunchIDEXT.x] = EmptyLss();
}
