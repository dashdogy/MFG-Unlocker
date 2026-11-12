#version 460
// rtxhair-spirv fixture: an ordinary closest hit with no LSS. Expect: Unchanged (not even parsed).
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 attributes;

void main()
{
    payload = vec4(attributes, gl_HitTEXT, float(gl_PrimitiveID));
}
