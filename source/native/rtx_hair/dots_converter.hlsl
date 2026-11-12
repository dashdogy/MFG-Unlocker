// rtxhair.1: one NVAPI LSS geometry (any supported layout) to DOTS hair: per LSS
// primitive, three ribbons 60 degrees apart around the segment, each two triangles
// (18 float3 vertices, in primitive order). One winding: rays that cull back faces still
// see every ribbon because universal hair sets TRIANGLE_CULL_DISABLE on the instances of
// these BLASes (instance_patch.hlsl), as an LSS hit is never culled. Three ribbons keep
// the strand's apparent width within -9%/+5% of its diameter at every view angle (two
// ribbons: -21%/+11%), exact on average. Every coordinate carries the "is LSS" tag in its
// low mantissa bits (lss_ir.h kTagMask / kTagValue). Triangle t of a primitive (lss_ir.h
// kTrianglesPerSegment = 6): t & 1 odd (A, D, B) else even (A, B, C), ribbon t / 2.
//
// The game's buffers are read through root SRVs at their GPU addresses (aligned down to
// 4 bytes; the remainder is in the offsets below). Root descriptors are not bounds
// checked, so every read is bounded by the counts. A primitive that is invalid or out
// of range gets NaN vertices: an inactive triangle that is never hit.
cbuffer Params : register(b0)
{
    uint PrimitiveCount;
    uint VertexCount;
    uint IndexCount;
    uint Flags; // bit 0: successive implicit (else list); bit 1: write every primitive inactive
    uint PositionOffset;
    uint PositionStride;
    uint PositionFormat; // 0 R32G32B32_FLOAT, 1 R32G32_FLOAT, 2 R16G16B16A16_FLOAT, 3 R16G16_FLOAT, 4 R16G16B16A16_SNORM, 5 R16G16_SNORM
    uint Pad0;
    uint RadiusOffset;
    uint RadiusStride; // 0: one radius for every vertex
    uint RadiusFormat; // 0 R32_FLOAT, 1 R16_FLOAT
    uint Pad1;
    uint IndexOffset;
    uint IndexStride;
    uint IndexFormat; // 0 none, 1 R32_UINT, 2 R16_UINT, 3 R8_UINT
    uint OutputOffset;
};
ByteAddressBuffer Positions : register(t0);
ByteAddressBuffer Radii : register(t1);
ByteAddressBuffer Indices : register(t2);
RWByteAddressBuffer Output : register(u0);

static const float kRibbon = 1.04719755120; // pi/3: lss_ir.h kRibbon
static const uint kTagMask = 0x7;
static const uint kTagValue = 0x5;

uint Load16(ByteAddressBuffer b, uint address)
{
    uint word = b.Load(address & ~3u);
    return (address & 2) ? (word >> 16) : (word & 0xFFFF);
}

uint Load8(ByteAddressBuffer b, uint address)
{
    return (b.Load(address & ~3u) >> ((address & 3) * 8)) & 0xFF;
}

float Snorm16(uint v)
{
    int s = (int)(v << 16) >> 16;
    return max((float)s / 32767.0, -1.0);
}

bool ReadIndex(uint i, out uint v)
{
    v = i;
    if (IndexFormat == 0)
        return true;
    if (i >= IndexCount)
        return false;
    uint size = IndexFormat == 1 ? 4 : (IndexFormat == 2 ? 2 : 1);
    uint address = IndexOffset + i * (IndexStride != 0 ? IndexStride : size);
    v = IndexFormat == 1 ? Indices.Load(address) : (IndexFormat == 2 ? Load16(Indices, address) : Load8(Indices, address));
    return true;
}

float4 ReadVertex(uint v)
{
    uint a = PositionOffset + v * PositionStride;
    float3 p;
    if (PositionFormat == 0)
        p = asfloat(Positions.Load3(a));
    else if (PositionFormat == 1)
        p = float3(asfloat(Positions.Load2(a)), 0);
    else if (PositionFormat == 2)
        p = float3(f16tof32(Load16(Positions, a)), f16tof32(Load16(Positions, a + 2)), f16tof32(Load16(Positions, a + 4)));
    else if (PositionFormat == 3)
        p = float3(f16tof32(Load16(Positions, a)), f16tof32(Load16(Positions, a + 2)), 0);
    else if (PositionFormat == 4)
        p = float3(Snorm16(Load16(Positions, a)), Snorm16(Load16(Positions, a + 2)), Snorm16(Load16(Positions, a + 4)));
    else
        p = float3(Snorm16(Load16(Positions, a)), Snorm16(Load16(Positions, a + 2)), 0);
    uint r = RadiusOffset + v * RadiusStride;
    float radius = RadiusFormat == 0 ? asfloat(Radii.Load(r)) : f16tof32(Load16(Radii, r));
    return float4(p, radius);
}

float3 Tag(float3 v)
{
    return asfloat((asuint(v) & ~kTagMask) | kTagValue);
}

static const uint kVerticesPerPrimitive = 18;

void Store(uint primitive, uint j, float3 v)
{
    Output.Store3(OutputOffset + (primitive * kVerticesPerPrimitive + j) * 12, asuint(Tag(v)));
}

void Inactive(uint primitive)
{
    float nan = asfloat(0x7FC00000u);
    [unroll] for (uint j = 0; j < kVerticesPerPrimitive; ++j)
        Store(primitive, j, float3(nan, nan, nan));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint primitive = id.x;
    if (primitive >= PrimitiveCount)
        return;
    uint first, second;
    bool ok = (Flags & 2) == 0;
    if (Flags & 1)
    {
        ok = ok && ReadIndex(primitive, first);
        second = first + 1;
    }
    else
    {
        ok = ok && ReadIndex(primitive * 2, first) && ReadIndex(primitive * 2 + 1, second);
    }
    if (!ok || first >= VertexCount || second >= VertexCount)
    {
        Inactive(primitive);
        return;
    }
    float4 p = ReadVertex(first);
    float4 q = ReadVertex(second);
    float3 d = q.xyz - p.xyz;
    float length2 = dot(d, d);
    if (!all(isfinite(p)) || !all(isfinite(q)) || p.w < 0 || q.w < 0 || (p.w == 0 && q.w == 0) || !isfinite(length2) || length2 <= 1.0e-20)
    {
        Inactive(primitive);
        return;
    }
    // Three ribbons around the segment, 60 degrees apart, starting at converter.hlsl's axis s.
    float3 t = d * rsqrt(length2);
    float3 a = abs(t);
    float3 reference = (a.x <= a.y && a.x <= a.z) ? float3(1, 0, 0) : ((a.y <= a.z) ? float3(0, 1, 0) : float3(0, 0, 1));
    float3 s = normalize(cross(t, reference));
    float3 v = cross(t, s);
    [unroll] for (uint face = 0; face < 3; ++face)
    {
        float3 axis = face == 0 ? s : (face == 1 ? 0.5 * s + 0.86602540378 * v : -0.5 * s + 0.86602540378 * v);
        float3 A = p.xyz + axis * (p.w * kRibbon);
        float3 B = q.xyz - axis * (q.w * kRibbon);
        float3 C = q.xyz + axis * (q.w * kRibbon);
        float3 D = p.xyz - axis * (p.w * kRibbon);
        uint f = face * 6;
        Store(primitive, f + 0, A);
        Store(primitive, f + 1, B);
        Store(primitive, f + 2, C);
        Store(primitive, f + 3, A);
        Store(primitive, f + 4, D);
        Store(primitive, f + 5, B);
    }
}
