#version 460
// rtxhair-spirv fixture: a closest hit written for hardware LSS, shared with ordinary triangles (it asks
// gl_HitIsLSSNV), reading the endpoints, radii, u (the first hit attribute), the primitive and the
// distance. Expect: Rewritten, one hit entry point, three LSS built-ins, three redirected reads.
#extension GL_EXT_ray_tracing : require
#extension GL_NV_linear_swept_spheres : require

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 attributes;

float Helper()
{
    return gl_HitLSSRadiiNV[0] + gl_HitLSSRadiiNV[1] + float(gl_PrimitiveID);
}

void main()
{
    if (gl_HitIsLSSNV)
    {
        vec3 a = gl_HitLSSPositionsNV[0];
        vec3 b = gl_HitLSSPositionsNV[1];
        payload = vec4(mix(a, b, attributes.x), Helper() + gl_HitTEXT);
    }
    else
        payload = vec4(attributes, gl_HitTEXT, float(gl_PrimitiveID));
}
