#version 460
// rtxhair-spirv fixture: sphere primitives (not converted). Expect: Neutral (every sphere and LSS read is
// zero / false), valid without the LSS capability.
#extension GL_EXT_ray_tracing : require
#extension GL_NV_linear_swept_spheres : require

layout(location = 0) rayPayloadInEXT vec4 payload;

void main()
{
    if (gl_HitIsSphereNV)
        payload = vec4(gl_HitSpherePositionNV, gl_HitSphereRadiusNV);
    else if (gl_HitIsLSSNV)
        payload = vec4(gl_HitLSSPositionsNV[0], gl_HitLSSRadiiNV[0]);
    else
        payload = vec4(0.0);
}
