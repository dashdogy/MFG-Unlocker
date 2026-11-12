// rtxhair.1: a top-level build's instance descs, copied with TRIANGLE_CULL_DISABLE set on
// every instance of a DOTS hair BLAS. An LSS hit is never culled, and DOTS ribbons are
// single-sided (dots_converter.hlsl), so rays that cull back faces (Witcher 3's shadow and
// path-tracing rays do) must not cull hair. Everything else is copied unchanged.
//
// The game's instance descs are read through a root SRV at their GPU address (aligned to
// 16 bytes by D3D12; the offset below covers any other 4-byte alignment).
cbuffer Params : register(b0)
{
    uint Count;       // instance descs
    uint TableCount;  // hair BLAS addresses in Table
    uint InputOffset; // bytes from the root SRV address to the first desc
    uint Pad;
};
ByteAddressBuffer Input : register(t0);
ByteAddressBuffer Table : register(t1); // sorted uint64 GPU addresses of DOTS hair BLASes
RWByteAddressBuffer Output : register(u0);

static const uint kCullDisable = 0x1; // D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE

bool Less(uint2 a, uint2 b)
{
    return a.y < b.y || (a.y == b.y && a.x < b.x);
}

bool Hair(uint2 address)
{
    uint lo = 0, hi = TableCount;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2;
        if (Less(Table.Load2(mid * 8), address))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < TableCount && all(Table.Load2(lo * 8) == address);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Count)
        return;
    uint at = InputOffset + id.x * 64;
    uint4 a = Input.Load4(at), b = Input.Load4(at + 16), c = Input.Load4(at + 32), d = Input.Load4(at + 48);
    // d: InstanceID | InstanceMask << 24, InstanceContributionToHitGroupIndex | Flags << 24,
    // AccelerationStructure (low, high).
    if (Hair(d.zw))
        d.y |= kCullDisable << 24;
    uint written = id.x * 64; // ("out" is an HLSL keyword)
    Output.Store4(written, a);
    Output.Store4(written + 16, b);
    Output.Store4(written + 32, c);
    Output.Store4(written + 48, d);
}
