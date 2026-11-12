#version 460
// rtxhair-spirv fixture: a fragment shader's ray query asking whether hair shadows the pixel and how
// far along the strand. Expect: Rewritten, two LSS ops.
#extension GL_EXT_ray_query : require
#extension GL_NV_linear_swept_spheres : require

layout(binding = 0) uniform accelerationStructureEXT scene;
layout(location = 0) in vec3 worldPosition;
layout(location = 0) out vec4 color;

void main()
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, worldPosition, 0.01, vec3(0.0, 1.0, 0.0), 50.0);
    rayQueryProceedEXT(q);
    float shade = 1.0;
    if (rayQueryIsLSSHitNV(q, true))
        shade = 0.5 * rayQueryGetIntersectionLSSHitValueNV(q, true);
    color = vec4(shade);
}
