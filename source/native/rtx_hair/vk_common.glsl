// vkhair.3: shared by vk_dots_converter.comp and vk_instance_patch.comp (vk_dots_builder.cpp). Both read the
// game's buffers through buffer device addresses held as uvec2 (GL_EXT_buffer_reference_uvec2), so they need
// neither descriptors nor 64-bit integers: only bufferDeviceAddress, which every ray tracing device enables.
// Their parameters sit in a slot of vk_dots_builder's ring, written by vkCmdUpdateBuffer in the game's command
// buffer; the ring's address is a specialization constant and the slot is the dispatch's base workgroup z
// (vkCmdDispatchBase), so the game's push constants and descriptor sets are never touched.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(constant_id = 0) const uint kRingLow = 0u;
layout(constant_id = 1) const uint kRingHigh = 0u;
const uint kSlotBytes = 256u;

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Word
{
    uint value;
};

uvec2 Offset(uvec2 base, uint add)
{
    uint low = base.x + add;
    return uvec2(low, base.y + (low < base.x ? 1u : 0u));
}

// base + offset + index * stride, in 64 bits.
uvec2 At(uvec2 base, uint offset, uint index, uint stride)
{
    uint high, low;
    umulExtended(index, stride, high, low);
    uvec2 a = Offset(base, offset);
    uint sum = a.x + low;
    return uvec2(sum, a.y + high + (sum < a.x ? 1u : 0u));
}

uvec2 Slot()
{
    return Offset(uvec2(kRingLow, kRingHigh), gl_WorkGroupID.z * kSlotBytes);
}

uint Load32(uvec2 address)
{
    return Word(address).value;
}

uint Load16(uvec2 address)
{
    uint w = Word(uvec2(address.x & ~3u, address.y)).value;
    return (address.x & 2u) != 0u ? (w >> 16) : (w & 0xFFFFu);
}

uint Load8(uvec2 address)
{
    uint w = Word(uvec2(address.x & ~3u, address.y)).value;
    return (w >> ((address.x & 3u) * 8u)) & 0xFFu;
}

void Store32(uvec2 address, uint value)
{
    Word(address).value = value;
}
