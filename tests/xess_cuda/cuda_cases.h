#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <random>
#include <string>
#include <cstring>

namespace xefg_cuda_cases
{
// ---- inputs (the semantics of each family) -------------------------------------------------------
struct Texture { uint32_t w{}, h{}, slices{1}; std::vector<uint32_t> words; }; // [slice][h][w] x 4 words
struct Case
{
    std::map<uint32_t, Texture> textures;              // SRV register -> Texture2DArray<uint4>
    std::map<uint32_t, std::vector<uint32_t>> buffers; // SRV register -> StructuredBuffer<uint4>
    std::vector<uint32_t> constants;                   // b0
    uint32_t outW{}, outH{}, outSlices{1};             // output texture (padding included)
    uint32_t groupsX{}, groupsY{}, groupsZ{1};
    uint32_t components{4};                            // t0 and u0 texel words: 4 R32G32B32A32_UINT, 2 R32G32_UINT
    std::string label;
};
Texture RandomTexture(std::mt19937& rng, uint32_t w, uint32_t h, uint32_t slices, bool realistic)
{
    Texture t; t.w = w; t.h = h; t.slices = slices; t.words.resize(size_t(w)*h*slices*4);
    std::uniform_int_distribution<uint32_t> any(0, 0xffffffffu); std::uniform_int_distribution<int> act(0, 127);
    for (auto& v : t.words)
        v = realistic ? uint32_t(act(rng)) | uint32_t(act(rng)) << 8 | uint32_t(act(rng)) << 16 | uint32_t(act(rng)) << 24 : any(rng);
    return t;
}
std::vector<uint32_t> RandomWords(std::mt19937& rng, size_t n)
{
    std::uniform_int_distribution<uint32_t> any(0, 0xffffffffu);
    std::vector<uint32_t> v(n); for (auto& x : v) x = any(rng); return v;
}
std::vector<uint32_t> RandomFloats(std::mt19937& rng, size_t n, float lo, float hi)
{
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<uint32_t> v(n); for (auto& x : v) { const float f = d(rng); memcpy(&x, &f, 4); } return v;
}

enum class Family { Pool, Decoder, Flow };
struct Spec
{
    const char* name; Family family;
    uint32_t in, out; bool pool; uint32_t tx, ty, rowsPerThread; // pooled/unpooled: Intel thread mapping
    uint32_t groupW, groupH;                                        // decoder: Intel group footprint
    uint32_t nit, nout, zgroups;                                    // flow
};
const Spec kSpecs[] = {
    {"aa016078", Family::Pool, 1, 1, true, 16, 16, 2},
    {"198d6bf8", Family::Pool, 1, 2, true, 4, 16, 2},
    {"dcd8becf", Family::Pool, 2, 4, true, 4, 8, 2},
    {"efbe3788", Family::Pool, 4, 8, true, 8, 4, 2},
    {"50f0c205", Family::Pool, 8, 4, false, 8, 8, 2},
    {"a3c3caa1", Family::Pool, 4, 2, false, 8, 4, 2},
    {"9481d452", Family::Pool, 1, 1, false, 4, 8, 1},
    {"06e97933", Family::Pool, 2, 1, false, 4, 8, 1},
    {"3ed03daf", Family::Decoder, 1, 1, false, 0, 0, 0, 16, 32},
    {"748b0baf", Family::Decoder, 4, 4, false, 0, 0, 0, 8, 16},
    {"cb391582", Family::Decoder, 2, 2, false, 0, 0, 0, 32, 8},
    {"fc1b8a04", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 14, 8, 28},
    {"3459485", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 14, 8, 28},
    {"3452733", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 14, 8, 28},
    {"55e5d6b9", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 4, 16, 4},
    {"34517926", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 4, 16, 4},
    {"3453044", Family::Flow, 0, 0, false, 0, 0, 0, 0, 0, 4, 16, 4},
};
const char* kModes[] = {"plain", "padded", "region", "fuzz", "rg32"};

// w x h = the layer's logical output size. mode: plain; padded (output texture larger than the footprint);
// region (decoder/flow constants smaller than the textures); fuzz (unread constant words random);
// rg32 (8-channel flow layers: input and output tensors as R32G32_UINT, as XeFG stores them).
Case Make(const Spec& s, uint32_t w, uint32_t h, uint32_t seed, bool realistic, int mode)
{
    std::mt19937 rng(seed); Case c;
    c.label = std::string(s.name) + " " + std::to_string(w) + "x" + std::to_string(h) + " seed " + std::to_string(seed) +
        (realistic ? " realistic " : " random ") + kModes[mode];
    const uint32_t padW = mode == 1 ? 13 : 0, padH = mode == 1 ? 7 : 0;
    std::uniform_int_distribution<uint32_t> any(0, 0xffffffffu);
    if (s.family == Family::Pool)
    {
        const uint32_t iw = s.pool ? 2*w : w, ih = s.pool ? 2*h : h;
        c.textures[0] = RandomTexture(rng, iw, ih, s.in, realistic);
        c.buffers[1] = RandomWords(rng, size_t(s.out)*s.in*144*4);
        c.buffers[2] = RandomFloats(rng, size_t(s.out)*16, 2e-5f/s.in, 4e-4f/s.in);
        c.buffers[3] = RandomFloats(rng, size_t(s.out)*16, -30.0f, 90.0f);
        c.constants.assign(4, 0);
        if (mode == 3) for (auto& v : c.constants) v = any(rng);
        c.outW = w + padW; c.outH = h + padH; c.outSlices = s.out;
        const uint32_t thx = s.pool ? w : (w + 1)/2, thy = s.pool ? h : (h + s.rowsPerThread - 1)/s.rowsPerThread;
        c.groupsX = (thx + s.tx - 1)/s.tx; c.groupsY = (thy + s.ty - 1)/s.ty;
    }
    else if (s.family == Family::Decoder)
    {
        const uint32_t wl = (w + 1)/2, hl = (h + 1)/2;
        c.textures[0] = RandomTexture(rng, wl, hl, s.in, realistic);
        c.textures[4] = RandomTexture(rng, w, h, s.in, realistic);
        c.buffers[1] = RandomWords(rng, size_t(s.out)*s.in*144*4);
        c.buffers[2] = RandomFloats(rng, size_t(s.out)*16, 2e-5f/s.in, 4e-4f/s.in);
        c.buffers[3] = RandomFloats(rng, size_t(s.out)*16, -30.0f, 90.0f);
        const uint32_t W = mode == 2 ? w - 3 : w, H = mode == 2 ? h - 2 : h;
        c.constants = {W, H, 0, 0};
        if (mode == 3) { c.constants[2] = any(rng); c.constants[3] = any(rng); }
        c.outW = w + padW; c.outH = h + padH; c.outSlices = s.out;
        c.groupsX = (w + s.groupW - 1)/s.groupW; c.groupsY = (h + s.groupH - 1)/s.groupH;
    }
    else
    {
        const uint32_t slices = s.nout == 8 ? 2*s.nit : s.nit;
        c.textures[0] = RandomTexture(rng, w, h + 1, slices, realistic);
        c.buffers[1] = RandomWords(rng, size_t(s.zgroups)*s.nit*36*(s.nout/4)*4);
        const uint32_t accBits = std::string(s.name) == "fc1b8a04" ? 16 : std::string(s.name) == "3452733" ? 19
            : std::string(s.name) == "3459485" || std::string(s.name) == "3453044" ? 18 : 17;
        const int range = 3 << (accBits - 1);
        std::uniform_int_distribution<int> bias(-range, range), neg(0, 600), mul(1, 600), sh(8, 14);
        std::vector<uint32_t> b(size_t(s.zgroups)*s.nout), n(size_t(s.zgroups)*s.nout);
        for (auto& v : b) v = uint32_t(bias(rng));
        for (auto& v : n) v = uint32_t(neg(rng));
        // buffers are uint4 elements: pad to whole elements
        b.resize((b.size() + 3) & ~size_t(3)); n.resize((n.size() + 3) & ~size_t(3));
        c.buffers[2] = b; c.buffers[3] = n;
        const uint32_t s1 = uint32_t(sh(rng)), s2 = uint32_t(sh(rng));
        const uint32_t W = mode == 2 ? w - 2 : w, H = mode == 2 ? h - 1 : h, Wo = mode == 2 ? w - 5 : w, Ho = mode == 2 ? h - 3 : h;
        c.constants = {0, 0, W, H, Wo, Ho, 0, 0, 0, uint32_t(mul(rng)), s1 | (s2 << 8), 0};
        if (mode == 3) for (int k : {0, 1, 6, 7, 8, 11}) c.constants[k] = any(rng);
        c.outW = w + padW; c.outH = h + 1 + padH; c.outSlices = s.zgroups;
        if (mode == 4) c.components = 2;
        c.groupsX = (w + 7)/8; c.groupsY = (h + 7)/8; c.groupsZ = s.zgroups;
    }
    return c;
}

}
