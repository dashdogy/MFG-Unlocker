#include "rtx_hair/lss_spirv.h"

#include "rtx_hair/lss_ir.h"
#include "rtx_hair/spirv_module.h"

#include <map>
#include <set>
#include <utility>

namespace rtx_hair {
namespace {
static_assert(LssDecodeLayout{}.trianglesPerSegment == kTrianglesPerSegment && LssDecodeLayout{}.ribbon == kRibbon && kHitRadius == kRibbon,
    "the default decode layout is lss_ir.h's");

// hairmem.1: the layout RewriteLssSpirv was given, for Decode (set for the length of that call on its thread).
thread_local LssDecodeLayout tLayout{};

using spirv::Inst;
using spirv::Make;
using spirv::Module;
using spirv::Role;
using spirv::Word;

enum : uint16_t
{
    kOpLine = 8,
    kOpExtInst = 12,
    kOpCapability = 17,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeArray = 28,
    kOpTypeStruct = 30,
    kOpTypePointer = 32,
    kOpConstant = 43,
    kOpConstantNull = 46,
    kOpFunctionParameter = 55,
    kOpFunctionCall = 57,
    kOpVariable = 59,
    kOpLoad = 61,
    kOpStore = 62,
    kOpAccessChain = 65,
    kOpInBoundsAccessChain = 66,
    kOpCompositeConstruct = 80,
    kOpCompositeExtract = 81,
    kOpCompositeInsert = 82,
    kOpCopyObject = 83,
    kOpBitcast = 124,
    kOpFAdd = 129,
    kOpFSub = 131,
    kOpFMul = 133,
    kOpUDiv = 134,
    kOpFDiv = 136,
    kOpUMod = 137,
    kOpVectorTimesScalar = 142,
    kOpDot = 148,
    kOpAll = 155,
    kOpIsNan = 156,
    kOpIsInf = 157,
    kOpLogicalEqual = 164,
    kOpLogicalOr = 166,
    kOpLogicalAnd = 167,
    kOpLogicalNot = 168,
    kOpSelect = 169,
    kOpIEqual = 170,
    kOpUGreaterThanEqual = 174,
    kOpFOrdLessThan = 184,
    kOpFOrdGreaterThan = 186,
    kOpFOrdLessThanEqual = 188,
    kOpFOrdGreaterThanEqual = 190,
    kOpBitwiseAnd = 199,
    kOpLabel = 248,
    kOpNoLine = 317,
    kOpRayQueryInitialize = 4473,
    kOpRayQueryTerminate = 4474,
    kOpRayQueryGenerateIntersection = 4475,
    kOpRayQueryConfirmIntersection = 4476,
    kOpRayQueryProceed = 4477,
    kOpRayQueryGetIntersectionType = 4479,
    kOpRayQueryGetIntersectionTriangleVertexPositions = 5340,
    kOpRayQueryGetIntersectionSpherePosition = 5427,
    kOpRayQueryGetIntersectionSphereRadius = 5428,
    kOpRayQueryGetIntersectionLssPositions = 5429,
    kOpRayQueryGetIntersectionLssRadii = 5430,
    kOpRayQueryGetIntersectionLssHitValue = 5431,
    kOpHitObjectGetSpherePosition = 5432,
    kOpHitObjectIsLssHit = 5437,
    kOpRayQueryIsSphereHit = 5438,
    kOpRayQueryIsLssHit = 5439,
    kOpRayQueryGetRayTMin = 6016,
    kOpRayQueryGetIntersectionT = 6018,
    kOpRayQueryGetIntersectionPrimitiveIndex = 6023,
    kOpRayQueryGetIntersectionBarycentrics = 6024,
    kOpRayQueryGetIntersectionObjectRayDirection = 6027,
    kOpRayQueryGetIntersectionObjectRayOrigin = 6028,
};
constexpr uint32_t kStorageInput = 1, kStoragePrivate = 6, kStorageHitAttribute = 5339;
constexpr uint32_t kModelAnyHit = 5315, kModelClosestHit = 5316;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kBuiltInPrimitiveId = 7, kBuiltInObjectRayOrigin = 5323, kBuiltInObjectRayDirection = 5324, kBuiltInRayTmin = 5325,
                   kBuiltInRayTmax = 5326, kBuiltInHitT = 5332, kBuiltInHitKind = 5333, kBuiltInHitTriangleVertexPositions = 5335,
                   kBuiltInHitIsSphere = 5359, kBuiltInHitIsLss = 5360, kBuiltInHitSpherePosition = 5361, kBuiltInHitLssPositions = 5396,
                   kBuiltInHitSphereRadius = 5420, kBuiltInHitLssRadii = 5421;
constexpr uint32_t kCapRayTracingPositionFetch = 5336, kCapRayQueryPositionFetch = 5391, kCapSpheres = 5418, kCapLss = 5419;
constexpr uint32_t kGlslSqrt = 31, kGlslNMin = 79, kGlslNMax = 80;
constexpr const char* kExtLss = "SPV_NV_linear_swept_spheres";
constexpr const char* kExtPositionFetch = "SPV_KHR_ray_tracing_position_fetch";

bool SphereBuiltIn(uint32_t b) { return b == kBuiltInHitIsSphere || b == kBuiltInHitSpherePosition || b == kBuiltInHitSphereRadius; }
bool LssBuiltIn(uint32_t b) { return b == kBuiltInHitIsLss || b == kBuiltInHitLssPositions || b == kBuiltInHitLssRadii; }
bool LssQueryOp(uint16_t op)
{
    return op == kOpRayQueryGetIntersectionLssPositions || op == kOpRayQueryGetIntersectionLssRadii || op == kOpRayQueryGetIntersectionLssHitValue
        || op == kOpRayQueryIsLssHit;
}
// Sphere ray-query ops and every SER hit-object sphere / LSS op: not rewritten (neutral form only).
bool OtherPrimitiveOp(uint16_t op)
{
    return op == kOpRayQueryGetIntersectionSpherePosition || op == kOpRayQueryGetIntersectionSphereRadius || op == kOpRayQueryIsSphereHit
        || (op >= kOpHitObjectGetSpherePosition && op <= kOpHitObjectIsLssHit);
}

Word PointeeOf(const Module& m, Word pointerType)
{
    const Inst* p = m.Global(pointerType);
    return p && p->Op() == kOpTypePointer && p->w.size() == 4 ? p->w[3] : 0;
}

// The global OpVariable `id`, for editing in place (looked up again after any interning).
Inst* MutableVariable(Module& m, Word id)
{
    for (auto& g : m.globals)
        if (g.Op() == kOpVariable && g.w.size() >= 4 && g.w[2] == id) return &g;
    return nullptr;
}

bool ConstantU32(const Module& m, Word id, uint32_t& value)
{
    const Inst* c = m.Global(id);
    if (!c || c->Op() != kOpConstant || c->w.size() != 4 || m.ScalarWidth(c->w[1]) != 32) return false;
    value = c->w[3];
    return true;
}

// Every global OpVariable: {id, storage class}.
std::vector<std::pair<Word, uint32_t>> GlobalVariables(const Module& m)
{
    std::vector<std::pair<Word, uint32_t>> out;
    for (const auto& g : m.globals)
        if (g.Op() == kOpVariable && g.w.size() >= 4) out.push_back({g.w[2], g.w[3]});
    return out;
}

// Functions (indices) that reference `id`.
bool Users(const Module& m, Word id, std::set<size_t>& users, std::string& error)
{
    std::vector<spirv::IdOperand> ids;
    for (size_t k = 0; k < m.functions.size(); ++k)
        for (const auto& inst : m.functions[k].body)
        {
            if (!m.IdOperands(inst, ids))
            {
                error = "opcode " + std::to_string(inst.Op()) + " does not decode against the grammar";
                return false;
            }
            for (const auto& o : ids)
                if (o.role == Role::Reference && inst.w[o.word] == id) users.insert(k);
        }
    return true;
}

// Points every use of global variable `from` in the functions at `to` (a variable of `storage` with the
// same pointee type; `to` may be `from` itself), moving the access chains built on it to `storage`.
// Only OpLoad and access chains may use it.
bool Redirect(Module& m, Word from, Word to, uint32_t storage, std::string& error)
{
    std::vector<spirv::IdOperand> ids;
    for (auto& f : m.functions)
    {
        std::set<Word> chains; // access chains rooted at `from` (ids are function-local)
        for (auto& inst : f.body)
        {
            if (!m.IdOperands(inst, ids))
            {
                error = "opcode " + std::to_string(inst.Op()) + " does not decode against the grammar";
                return false;
            }
            const auto ours = [&](Word id) { return id == from || chains.count(id) != 0; };
            bool uses = false;
            for (const auto& o : ids)
                if (o.role == Role::Reference && ours(inst.w[o.word])) uses = true;
            if (!uses) continue;
            const uint16_t op = inst.Op();
            constexpr size_t kBase = 3; // OpLoad's pointer, an access chain's base
            bool ok = (op == kOpLoad || op == kOpAccessChain || op == kOpInBoundsAccessChain) && inst.w.size() > kBase;
            for (const auto& o : ids)
                if (ok && o.role == Role::Reference && o.word != kBase && ours(inst.w[o.word])) ok = false;
            if (!ok)
            {
                error = "variable %" + std::to_string(from) + " is used by opcode " + std::to_string(op) + " (only loads and access chains are rewritten)";
                return false;
            }
            if (inst.w[kBase] == from) inst.w[kBase] = to;
            if (op != kOpLoad)
            {
                const Word pointee = PointeeOf(m, inst.w[1]);
                if (!pointee)
                {
                    error = "an access chain whose result type is not a pointer";
                    return false;
                }
                inst.w[1] = m.Pointer(storage, pointee);
                chains.insert(inst.w[2]);
            }
        }
    }
    return true;
}

// Moves the pointer type or null constant `id` in front of global variable `var` when it comes after it
// (interning appends at the end; SPIR-V declares before use). Its own operand, the pointee type, is already
// in front of `var`, since the variable's old pointer type used it.
void Hoist(Module& m, Word id, Word var)
{
    auto& g = m.globals;
    size_t from = g.size(), to = g.size();
    for (size_t i = 0; i < g.size(); ++i)
    {
        const Inst& x = g[i];
        if ((x.Op() == kOpTypePointer && x.w.size() >= 2 && x.w[1] == id) || (x.Op() == kOpConstantNull && x.w.size() >= 3 && x.w[2] == id)) from = i;
        if (x.Op() == kOpVariable && x.w.size() >= 3 && x.w[2] == var) to = i;
    }
    if (from == g.size() || to == g.size() || from < to) return;
    Inst moved = std::move(g[from]);
    g.erase(g.begin() + static_cast<ptrdiff_t>(from));
    g.insert(g.begin() + static_cast<ptrdiff_t>(to), std::move(moved));
    m.Reindex();
}

// A global variable moved to Private in place (optionally with an initializer), its decorations gone.
bool ToPrivate(Module& m, Word var, Word initializer, std::string& error)
{
    const Inst* v = m.Global(var);
    const Word pointee = v ? PointeeOf(m, v->w[1]) : 0;
    if (!pointee)
    {
        error = "a built-in variable without a pointer type";
        return false;
    }
    const Word pointer = m.Pointer(kStoragePrivate, pointee); // may append: look the variable up afterwards
    Hoist(m, pointer, var);
    if (initializer) Hoist(m, initializer, var);
    Inst* edit = MutableVariable(m, var);
    if (!edit)
    {
        error = "a built-in that is not a global variable";
        return false;
    }
    *edit = initializer ? Make(kOpVariable, {pointer, var, kStoragePrivate, initializer}) : Make(kOpVariable, {pointer, var, kStoragePrivate});
    m.RemoveDecorations(var);
    m.Reindex();
    return Redirect(m, var, var, kStoragePrivate, error);
}

void RemoveLssDeclarations(Module& m)
{
    m.RemoveCapability(kCapLss);
    m.RemoveCapability(kCapSpheres);
    m.RemoveExtension(kExtLss);
}

struct Types
{
    Word f, v2, v3, u, b, u3, b3, arr3v3;
    explicit Types(Module& m)
        : f(m.Float32()), v2(m.Vector(f, 2)), v3(m.Vector(f, 3)), u(m.Int(32, false)), b(m.Bool()), u3(m.Vector(u, 3)), b3(m.Vector(b, 3)),
          arr3v3(m.Array(v3, 3))
    {
    }
};

class Emitter
{
public:
    Emitter(Module& m, const Types& t, std::vector<Inst>& out) : m_(m), t_(t), out_(out), glsl_(m.Glsl()) {}

    Word Op(uint16_t op, Word type, std::initializer_list<Word> args) { return OpTo(op, type, m_.NewId(), args); }
    Word OpTo(uint16_t op, Word type, Word result, std::initializer_list<Word> args)
    {
        std::vector<Word> w{type, result};
        w.insert(w.end(), args.begin(), args.end());
        out_.push_back(Make(op, w));
        return result;
    }
    void Store(Word pointer, Word value) { out_.push_back(Make(kOpStore, {pointer, value})); }
    Word Load(Word type, Word pointer) { return Op(kOpLoad, type, {pointer}); }
    Word Extract(Word type, Word composite, uint32_t index) { return Op(kOpCompositeExtract, type, {composite, index}); }
    Word Insert(Word type, Word object, Word composite, uint32_t index) { return Op(kOpCompositeInsert, type, {object, composite, index}); }
    Word Ext(Word type, uint32_t instruction, std::initializer_list<Word> args)
    {
        const Word id = m_.NewId();
        std::vector<Word> w{type, id, glsl_, instruction};
        w.insert(w.end(), args.begin(), args.end());
        out_.push_back(Make(kOpExtInst, w));
        return id;
    }

    Word K(float value) { return m_.F32(value); }
    Word KU(uint32_t value) { return m_.U32(value); }
    // Scalars (f) and 3-vectors (v3).
    Word Add(Word type, Word a, Word b) { return Op(kOpFAdd, type, {a, b}); }
    Word Sub(Word type, Word a, Word b) { return Op(kOpFSub, type, {a, b}); }
    Word Mul(Word a, Word b) { return Op(kOpFMul, t_.f, {a, b}); }
    Word Div(Word a, Word b) { return Op(kOpFDiv, t_.f, {a, b}); }
    Word Scale(Word v, Word s) { return Op(kOpVectorTimesScalar, t_.v3, {v, s}); }
    Word Dot(Word a, Word b) { return Op(kOpDot, t_.f, {a, b}); }
    Word Sqrt(Word x) { return Ext(t_.f, kGlslSqrt, {x}); }
    Word Min(Word a, Word b) { return Ext(t_.f, kGlslNMin, {a, b}); }
    Word Max(Word a, Word b) { return Ext(t_.f, kGlslNMax, {a, b}); }
    Word Sel(Word type, Word condition, Word a, Word b) { return Op(kOpSelect, type, {condition, a, b}); }
    Word And(Word a, Word b) { return Op(kOpLogicalAnd, t_.b, {a, b}); }
    Word Or(Word a, Word b) { return Op(kOpLogicalOr, t_.b, {a, b}); }
    Word Not(Word a) { return Op(kOpLogicalNot, t_.b, {a}); }
    Word Cmp(uint16_t op, Word a, Word b) { return Op(op, t_.b, {a, b}); }
    Word Finite(Word x) { return Not(Or(Op(kOpIsNan, t_.b, {x}), Op(kOpIsInf, t_.b, {x}))); }

private:
    Module& m_;
    const Types& t_;
    std::vector<Inst>& out_;
    Word glsl_;
};

// What a decode starts from: the hit triangle and the hit.
struct Hit
{
    Word v0, v1, v2; // vec3: the triangle's object-space vertices, in index order
    Word prim;       // uint: the DOTS triangle index
    Word valid;      // bool: a triangle hit
    Word o, d;       // vec3: object-space ray origin and direction
    Word t, tmin;    // float: the hit distance the hardware reports, the ray's TMin
};
// The decoded LSS hit (lss_ir.cpp's Context).
struct Context
{
    Word lss;    // bool: a triangle hit carrying the DOTS marker
    Word seg;    // uint: the segment (LSS primitive) index
    Word p, q;   // vec3: endpoints
    Word rp, rq; // float: their radii
    Word t;      // float: the hit distance the shader should see
    Word u;      // float: the hit parameter along the segment, 0..1
};

// lss_ir.cpp's EmitContext in SPIR-V: the marker, the decode from any one DOTS triangle, the distance to
// the round strand (kHitRadius radii) and u.
Context Decode(Emitter& e, const Types& ty, const Hit& h)
{
    Context c{};
    // Marker: all nine coordinates.
    const Word mask = e.Op(kOpCompositeConstruct, ty.u3, {e.KU(kTagMask), e.KU(kTagMask), e.KU(kTagMask)});
    const Word value = e.Op(kOpCompositeConstruct, ty.u3, {e.KU(kTagValue), e.KU(kTagValue), e.KU(kTagValue)});
    Word tag = 0;
    for (const Word v : {h.v0, h.v1, h.v2})
    {
        const Word bits = e.Op(kOpBitcast, ty.u3, {v});
        const Word low = e.Op(kOpBitwiseAnd, ty.u3, {bits, mask});
        const Word ok = e.Op(kOpAll, ty.b, {e.Op(kOpIEqual, ty.b3, {low, value})});
        tag = tag ? e.And(tag, ok) : ok;
    }
    c.lss = e.And(h.valid, tag);
    // Segment, and the triangle's parity and winding (as lss_ir.cpp).
    const uint32_t trianglesPerSegment = tLayout.trianglesPerSegment;
    const float ribbon = tLayout.ribbon, hitRadius = tLayout.ribbon; // the round strand encloses the ribbons (lss_ir.h kHitRadius)
    c.seg = e.Op(kOpUDiv, ty.u, {h.prim, e.KU(trianglesPerSegment)});
    const Word local = e.Op(kOpUMod, ty.u, {h.prim, e.KU(trianglesPerSegment)});
    const Word parity = e.Op(kOpBitwiseAnd, ty.u, {local, e.KU(1)});
    const Word even = e.Cmp(kOpIEqual, parity, e.KU(0));
    Word forward = even;
    if constexpr (kWindings == 2)
    {
        const Word reversed = e.Cmp(kOpUGreaterThanEqual, local, e.KU(trianglesPerSegment / 2));
        forward = e.Cmp(kOpLogicalEqual, even, e.Not(reversed));
    }
    // Endpoints and radii (lss_ir.cpp's decode).
    const Word x0 = e.Sel(ty.v3, even, h.v0, h.v2);
    const Word m = e.Scale(e.Add(ty.v3, e.Sel(ty.v3, even, h.v1, h.v0), e.Sel(ty.v3, even, h.v2, h.v1)), e.K(0.5f));
    const Word dv = e.Sub(ty.v3, e.Sel(ty.v3, even, h.v2, h.v0), h.v1);
    const Word len = e.Sqrt(e.Dot(dv, dv));
    const Word inv = e.Sel(ty.f, e.Cmp(kOpFOrdGreaterThan, len, e.K(0.0f)), e.Div(e.K(1.0f), len), e.K(0.0f));
    const Word a = e.Scale(dv, inv);
    const Word rm = e.Mul(len, e.K(0.5f / ribbon));
    const Word sign = e.Sel(ty.f, forward, e.K(1.0f), e.K(-1.0f));
    const Word rx = e.Mul(e.Mul(sign, e.Dot(e.Sub(ty.v3, x0, m), a)), e.K(1.0f / ribbon));
    const Word xe = e.Sub(ty.v3, x0, e.Scale(a, e.Mul(e.Mul(sign, rx), e.K(ribbon))));
    c.p = e.Sel(ty.v3, even, xe, m);
    c.q = e.Sel(ty.v3, even, m, xe);
    c.rp = e.Sel(ty.f, even, rx, rm);
    c.rq = e.Sel(ty.f, even, rm, rx);
    // Distance to the round strand, solved in the segment's own frame (lss_ir.cpp says why).
    const Word zero = e.K(0.0f), one = e.K(1.0f), big = e.K(1.0e20f);
    const Word dl = e.Sqrt(e.Dot(h.d, h.d));
    const Word dinv = e.Div(one, dl);
    const Word rd = e.Scale(h.d, dinv);
    const Word ba = e.Sub(ty.v3, c.q, c.p);
    const Word m0 = e.Dot(ba, ba);
    const Word length = e.Sqrt(m0);
    const Word linv = e.Sel(ty.f, e.Cmp(kOpFOrdGreaterThan, length, zero), e.Div(one, length), zero);
    const Word axis = e.Scale(ba, linv);
    const Word oa = e.Sub(ty.v3, h.o, c.p);
    const Word z0 = e.Dot(oa, axis);
    const Word p0 = e.Sub(ty.v3, oa, e.Scale(axis, z0)); // origin: perpendicular to the axis
    const Word dz = e.Dot(rd, axis);
    const Word dp = e.Sub(ty.v3, rd, e.Scale(axis, dz)); // direction: perpendicular part
    const Word hp = e.Mul(c.rp, e.K(hitRadius)), hq = e.Mul(c.rq, e.K(hitRadius));
    const Word slope = e.Mul(e.Sub(ty.f, hp, hq), linv); // sine of the cone's half-angle
    const Word cos2 = e.Sub(ty.f, one, e.Mul(slope, slope));
    const Word dd = e.Dot(dp, dp);
    const Word t0 = e.Div(e.Sub(ty.f, zero, e.Dot(p0, dp)), dd); // closest approach to the axis
    const Word p1 = e.Add(ty.v3, p0, e.Scale(dp, t0));
    const Word z1 = e.Add(ty.f, z0, e.Mul(dz, t0));
    const Word c1 = e.Sub(ty.f, hp, e.Mul(slope, z1));
    const Word qa = e.Sub(ty.f, e.Mul(cos2, dd), e.Mul(e.Mul(slope, slope), e.Mul(dz, dz)));
    const Word qb = e.Mul(e.Mul(slope, dz), c1);
    const Word qc = e.Sub(ty.f, e.Mul(cos2, e.Dot(p1, p1)), e.Mul(c1, c1));
    const Word disc = e.Sub(ty.f, e.Mul(qb, qb), e.Mul(qa, qc));
    const Word tb = e.Add(ty.f, t0, e.Div(e.Sub(ty.f, e.Sub(ty.f, zero, qb), e.Sqrt(e.Max(disc, zero))), qa));
    const Word zh = e.Add(ty.f, z0, e.Mul(dz, tb));
    const Word rh = e.Sub(ty.f, hp, e.Mul(slope, zh));                    // body radius there (the near nappe: >= 0)
    const Word foot = e.Sub(ty.f, zh, e.Div(e.Mul(rh, slope), cos2));     // the touching sphere's centre on the axis
    const Word body = e.And(e.And(e.And(e.Cmp(kOpFOrdGreaterThanEqual, disc, zero), e.Cmp(kOpFOrdGreaterThan, qa, zero)),
                                e.And(e.Finite(tb), e.Cmp(kOpFOrdGreaterThan, cos2, zero))),
        e.And(e.And(e.Cmp(kOpFOrdGreaterThanEqual, foot, zero), e.Cmp(kOpFOrdLessThanEqual, foot, length)), e.Cmp(kOpFOrdGreaterThanEqual, rh, zero)));
    const auto cap = [&](Word centre, Word radius) {
        const Word tc = e.Dot(e.Sub(ty.v3, centre, h.o), rd);
        const Word w = e.Sub(ty.v3, e.Add(ty.v3, h.o, e.Scale(rd, tc)), centre);
        const Word hh = e.Sub(ty.f, e.Mul(radius, radius), e.Dot(w, w));
        return e.Sel(ty.f, e.Cmp(kOpFOrdGreaterThanEqual, hh, zero), e.Sub(ty.f, tc, e.Sqrt(e.Max(hh, zero))), big);
    };
    const Word caps = e.Min(cap(c.p, hp), cap(c.q, hq));
    const Word tn = e.Sel(ty.f, body, e.Min(tb, caps), caps);
    const Word ta = e.Mul(tn, dinv);
    const Word ok = e.And(e.Or(body, e.Cmp(kOpFOrdLessThan, caps, e.K(1.0e19f))), e.And(e.Finite(ta), e.Cmp(kOpFOrdGreaterThanEqual, ta, h.tmin)));
    c.t = e.Sel(ty.f, e.And(c.lss, ok), ta, h.t);
    // u: the hit point projected on the segment.
    const Word at = e.Add(ty.v3, h.o, e.Scale(h.d, c.t));
    const Word along = e.Div(e.Dot(e.Sub(ty.v3, at, c.p), ba), m0);
    const Word uRaw = e.Sel(ty.f, e.Cmp(kOpFOrdGreaterThan, m0, zero), along, zero);
    c.u = e.Max(e.Min(uRaw, one), zero);
    return c;
}

// Whether the LSS u can be put into a value of `type` (hit attributes: a float, a float vector, or a
// struct whose first member is one of those).
bool HoldsU(const Module& m, const Types& ty, Word type, int depth = 0)
{
    if (type == ty.f) return true;
    const Inst* t = m.Global(type);
    if (!t) return false;
    if (t->Op() == kOpTypeVector && t->w.size() == 4) return t->w[2] == ty.f;
    if (t->Op() == kOpTypeStruct && t->w.size() >= 3 && depth == 0) return HoldsU(m, ty, t->w[2], 1);
    return false;
}

// `value` (attributes as the hardware reports them) with u in its first float and 0 in the second when
// the hit is LSS (the barycentrics of a DOTS triangle otherwise).
Word PutU(Emitter& e, const Module& m, const Types& ty, Word type, Word value, const Context& c)
{
    if (type == ty.f) return e.Sel(ty.f, c.lss, c.u, value);
    // Copied out first: emitting may intern constants and move the globals.
    const Inst* t = m.Global(type);
    const bool vector = t->Op() == kOpTypeVector;
    const Word second = t->w[2], count = t->w.size() > 3 ? t->w[3] : 0;
    if (vector)
    {
        const Word zero = e.K(0.0f);
        Word out = e.Insert(type, e.Sel(ty.f, c.lss, c.u, e.Extract(ty.f, value, 0)), value, 0);
        if (count >= 2) out = e.Insert(type, e.Sel(ty.f, c.lss, zero, e.Extract(ty.f, value, 1)), out, 1);
        return out;
    }
    const Word member = second; // the struct's first member
    return e.Insert(type, PutU(e, m, ty, member, e.Extract(member, value, 0), c), value, 0);
}

struct Entry
{
    size_t index;
    uint32_t model;
    Word function;
};

std::vector<Entry> Entries(const Module& m)
{
    std::vector<Entry> out;
    for (size_t i = 0; i < m.entryPoints.size(); ++i)
        if (m.entryPoints[i].w.size() >= 4) out.push_back({i, m.entryPoints[i].w[1], m.entryPoints[i].w[2]});
    return out;
}

void AddInterface(Module& m, size_t entry, Word var)
{
    Inst& e = m.entryPoints[entry];
    size_t first = 0;
    spirv::ReadString(e, 3, &first);
    for (size_t k = first; k < e.w.size(); ++k)
        if (e.w[k] == var) return;
    e.w.push_back(var);
}

// Index of the first instruction after the entry block's OpVariables.
size_t PrologueIndex(const spirv::Function& f)
{
    size_t i = 1;
    while (i < f.body.size() && f.body[i].Op() == kOpFunctionParameter) ++i;
    if (i >= f.body.size() || f.body[i].Op() != kOpLabel) return 0;
    size_t after = ++i;
    for (; i < f.body.size(); ++i)
    {
        const uint16_t op = f.body[i].Op();
        if (op == kOpVariable) after = i + 1;
        else if (op != kOpLine && op != kOpNoLine && op != kOpExtInst) break;
    }
    return after;
}

struct BuiltInVariable
{
    Word var{}, pointee{};
};
// The module's Input variable decorated `builtIn`, else a new one of `type`.
BuiltInVariable InputBuiltIn(Module& m, uint32_t builtIn, Word type)
{
    for (const auto& [id, storage] : GlobalVariables(m))
    {
        uint32_t b = 0;
        if (storage == kStorageInput && m.BuiltInOf(id, b) && b == builtIn) return {id, PointeeOf(m, m.TypeOf(id))};
    }
    const Word var = m.Variable(kStorageInput, type);
    m.Decorate(var, kDecorationBuiltIn, {builtIn});
    return {var, type};
}

// A built-in read redirected to its LSS value in hit shaders.
struct Redirected
{
    Word original, copy, pointee;
    enum Kind
    {
        Primitive,
        Distance,
        Attributes,
    } kind;
};

bool IsU32Int(const Module& m, Word type)
{
    const Inst* t = m.Global(type);
    return t && t->Op() == 21 && t->w.size() == 4 && t->w[2] == 32;
}

class Rewriter
{
public:
    Rewriter(Module& m, SpirvRewriteReport& report) : m_(m), report_(report) {}

    bool Run(std::string& error)
    {
        if (m_.version < 0x00010400u) return Fail(error, "SPIR-V older than 1.4 (LSS needs 1.4)");
        uint32_t b = 0;
        for (const auto& [id, storage] : GlobalVariables(m_))
            if (m_.BuiltInOf(id, b) && SphereBuiltIn(b)) return Fail(error, "sphere built-ins are not converted");
        bool queries = false;
        for (const auto& f : m_.functions)
            for (const auto& inst : f.body)
            {
                if (OtherPrimitiveOp(inst.Op())) return Fail(error, "sphere or hit-object LSS op " + std::to_string(inst.Op()) + " is not converted");
                queries = queries || LssQueryOp(inst.Op());
            }
        Types ty(m_);
        ty_ = &ty;
        Reach();
        bool hits = false;
        for (const auto& [id, storage] : GlobalVariables(m_))
            if (m_.BuiltInOf(id, b) && LssBuiltIn(b)) hits = true;
        if (hits && !HitBuiltIns(error)) return false;
        if (queries && !Queries(error)) return false;
        RemoveLssDeclarations(m_);
        if (hits) m_.AddCapability(kCapRayTracingPositionFetch);
        if (queries) m_.AddCapability(kCapRayQueryPositionFetch);
        if (hits || queries) m_.AddExtension(kExtPositionFetch);
        return true;
    }

private:
    static bool Fail(std::string& error, std::string text)
    {
        error = std::move(text);
        return false;
    }

    // Which entry points reach each function.
    void Reach()
    {
        std::map<Word, size_t> byId;
        for (size_t k = 0; k < m_.functions.size(); ++k) byId[m_.functions[k].Id()] = k;
        std::vector<std::set<size_t>> callees(m_.functions.size());
        for (size_t k = 0; k < m_.functions.size(); ++k)
            for (const auto& inst : m_.functions[k].body)
                if (inst.Op() == kOpFunctionCall && inst.w.size() >= 4)
                    if (const auto it = byId.find(inst.w[3]); it != byId.end()) callees[k].insert(it->second);
        reachedBy_.assign(m_.functions.size(), {});
        entries_ = Entries(m_);
        for (const auto& e : entries_)
        {
            const auto it = byId.find(e.function);
            if (it == byId.end()) continue;
            std::vector<size_t> stack{it->second};
            std::set<size_t> seen;
            while (!stack.empty())
            {
                const size_t k = stack.back();
                stack.pop_back();
                if (!seen.insert(k).second) continue;
                reachedBy_[k].push_back(e.model);
                for (const size_t c : callees[k]) stack.push_back(c);
            }
            if (e.model == kModelAnyHit || e.model == kModelClosestHit) entryFunction_[e.index] = it->second;
        }
    }

    // Every function using `var` is reached by hit entry points only.
    bool HitOnly(Word var, std::string& error)
    {
        std::set<size_t> users;
        if (!Users(m_, var, users, error)) return false;
        for (const size_t k : users)
            for (const uint32_t model : reachedBy_[k])
                if (model != kModelAnyHit && model != kModelClosestHit)
                    return Fail(error, "built-in %" + std::to_string(var) + " is read in a function that a non-hit entry point reaches");
        return true;
    }

    bool HitBuiltIns(std::string& error)
    {
        const Types& ty = *ty_;
        // The LSS built-ins become Private variables, filled by every hit entry point.
        Word lss[3] = {}; // positions, radii, is-LSS
        uint32_t b = 0;
        for (const auto& [id, storage] : GlobalVariables(m_))
        {
            if (!m_.BuiltInOf(id, b) || !LssBuiltIn(b)) continue;
            const Word pointee = PointeeOf(m_, m_.TypeOf(id));
            const bool shape = b == kBuiltInHitIsLss ? pointee == ty.b : SameArray(pointee, b == kBuiltInHitLssPositions ? ty.v3 : ty.f, 2);
            if (!shape) return Fail(error, "LSS built-in %" + std::to_string(id) + " has an unexpected type");
            if (!HitOnly(id, error)) return false;
            lss[b == kBuiltInHitLssPositions ? 0 : (b == kBuiltInHitLssRadii ? 1 : 2)] = id;
        }
        for (const Word id : lss)
        {
            if (!id) continue;
            if (!ToPrivate(m_, id, 0, error)) return false;
            ++report_.builtIns;
        }
        // Reads redirected in hit shaders: primitive, hit distance, hit attributes.
        std::vector<Redirected> redirected;
        for (const auto& [id, storage] : GlobalVariables(m_))
        {
            Redirected::Kind kind = Redirected::Primitive;
            if (storage == kStorageHitAttribute) kind = Redirected::Attributes;
            else if (storage == kStorageInput && m_.BuiltInOf(id, b) && b == kBuiltInPrimitiveId) kind = Redirected::Primitive;
            else if (storage == kStorageInput && m_.BuiltInOf(id, b) && (b == kBuiltInRayTmax || b == kBuiltInHitT)) kind = Redirected::Distance;
            else continue;
            std::set<size_t> users;
            if (!Users(m_, id, users, error)) return false;
            if (users.empty()) continue;
            const Word pointee = PointeeOf(m_, m_.TypeOf(id));
            if (kind == Redirected::Attributes && !HoldsU(m_, ty, pointee))
            {
                report_.note += "hit attributes of an unexpected type are left as the hardware reports them; ";
                continue;
            }
            if (kind == Redirected::Primitive && !IsU32Int(m_, pointee)) return Fail(error, "PrimitiveId is not a 32-bit integer");
            if (kind == Redirected::Distance && pointee != ty.f) return Fail(error, "the hit distance is not a 32-bit float");
            if (!HitOnly(id, error)) return false;
            redirected.push_back({id, 0, pointee, kind});
        }
        for (auto& r : redirected)
        {
            r.copy = m_.Variable(kStoragePrivate, r.pointee);
            if (!Redirect(m_, r.original, r.copy, kStoragePrivate, error)) return false;
            ++report_.reads;
        }
        // Each hit entry point decodes its hit at the start and fills them.
        for (const auto& [entry, k] : entryFunction_)
        {
            const BuiltInVariable tri = InputBuiltIn(m_, kBuiltInHitTriangleVertexPositions, ty.arr3v3);
            const BuiltInVariable prim = InputBuiltIn(m_, kBuiltInPrimitiveId, m_.Int(32, true));
            const BuiltInVariable kind = InputBuiltIn(m_, kBuiltInHitKind, ty.u);
            const BuiltInVariable origin = InputBuiltIn(m_, kBuiltInObjectRayOrigin, ty.v3);
            const BuiltInVariable direction = InputBuiltIn(m_, kBuiltInObjectRayDirection, ty.v3);
            const BuiltInVariable tmax = InputBuiltIn(m_, kBuiltInRayTmax, ty.f);
            const BuiltInVariable tmin = InputBuiltIn(m_, kBuiltInRayTmin, ty.f);
            if (tri.pointee != ty.arr3v3 && !SameArray(tri.pointee, ty.v3, 3)) return Fail(error, "HitTriangleVertexPositionsKHR has an unexpected type");
            if (!IsU32Int(m_, prim.pointee) || !IsU32Int(m_, kind.pointee) || origin.pointee != ty.v3 || direction.pointee != ty.v3 || tmax.pointee != ty.f
                || tmin.pointee != ty.f)
                return Fail(error, "a hit built-in has an unexpected type");
            std::vector<Inst> pro;
            Emitter e(m_, ty, pro);
            const Word vertices = e.Load(tri.pointee, tri.var);
            Hit h{};
            h.v0 = e.Extract(ty.v3, vertices, 0);
            h.v1 = e.Extract(ty.v3, vertices, 1);
            h.v2 = e.Extract(ty.v3, vertices, 2);
            const Word primitive = e.Load(prim.pointee, prim.var);
            h.prim = prim.pointee == ty.u ? primitive : e.Op(kOpBitcast, ty.u, {primitive});
            const Word hitKind = e.Load(kind.pointee, kind.var);
            h.valid = e.Cmp(kOpUGreaterThanEqual, kind.pointee == ty.u ? hitKind : e.Op(kOpBitcast, ty.u, {hitKind}), e.KU(254)); // triangle front / back face
            h.o = e.Load(ty.v3, origin.var);
            h.d = e.Load(ty.v3, direction.var);
            h.t = e.Load(ty.f, tmax.var);
            h.tmin = e.Load(ty.f, tmin.var);
            const Context c = Decode(e, ty, h);
            if (lss[0]) e.Store(lss[0], e.Op(kOpCompositeConstruct, PointeeOf(m_, m_.TypeOf(lss[0])), {c.p, c.q}));
            if (lss[1]) e.Store(lss[1], e.Op(kOpCompositeConstruct, PointeeOf(m_, m_.TypeOf(lss[1])), {c.rp, c.rq}));
            if (lss[2]) e.Store(lss[2], c.lss);
            for (const auto& r : redirected)
            {
                const Word original = e.Load(r.pointee, r.original);
                Word value = 0;
                if (r.kind == Redirected::Primitive) value = e.Sel(r.pointee, c.lss, r.pointee == ty.u ? c.seg : e.Op(kOpBitcast, r.pointee, {c.seg}), original);
                else if (r.kind == Redirected::Distance) value = e.Sel(ty.f, c.lss, c.t, original);
                else value = PutU(e, m_, ty, r.pointee, original, c);
                e.Store(r.copy, value);
            }
            auto& body = m_.functions[k].body;
            const size_t at = PrologueIndex(m_.functions[k]);
            if (!at) return Fail(error, "a hit entry point without an entry block");
            body.insert(body.begin() + static_cast<ptrdiff_t>(at), pro.begin(), pro.end());
            for (const Word var : {tri.var, prim.var, kind.var, origin.var, direction.var, tmax.var, tmin.var}) AddInterface(m_, entry, var);
            for (const Word var : lss)
                if (var) AddInterface(m_, entry, var);
            for (const auto& r : redirected)
            {
                AddInterface(m_, entry, r.original);
                AddInterface(m_, entry, r.copy);
            }
            ++report_.hitEntryPoints;
        }
        m_.Reindex();
        return true;
    }

    // An array of `length` `element`s, under any (undecorated or decorated) type id.
    bool SameArray(Word type, Word element, uint32_t length) const
    {
        const Inst* t = m_.Global(type);
        uint32_t n = 0;
        return t && t->Op() == kOpTypeArray && t->w.size() == 4 && t->w[2] == element && ConstantU32(m_, t->w[3], n) && n == length;
    }

    bool Queries(std::string& error)
    {
        const Types& ty = *ty_;
        for (auto& f : m_.functions)
        {
            bool lss = false;
            for (const auto& inst : f.body) lss = lss || LssQueryOp(inst.Op());
            if (!lss) continue;
            std::vector<Inst> out;
            out.reserve(f.body.size() * 2);
            Emitter e(m_, ty, out);
            std::map<std::pair<Word, Word>, Context> live; // (query, intersection) -> its decode, valid in this block
            std::string contextError;
            const auto context = [&](Word query, Word intersection) -> Context {
                const auto key = std::make_pair(query, intersection);
                if (const auto it = live.find(key); it != live.end()) return it->second;
                uint32_t committed = 0;
                if (!ConstantU32(m_, intersection, committed) || committed > 1) contextError = "a ray-query intersection operand that is not the constant 0 or 1";
                const Word vertices = e.Op(kOpRayQueryGetIntersectionTriangleVertexPositions, ty.arr3v3, {query, intersection});
                Hit h{};
                h.v0 = e.Extract(ty.v3, vertices, 0);
                h.v1 = e.Extract(ty.v3, vertices, 1);
                h.v2 = e.Extract(ty.v3, vertices, 2);
                h.prim = e.Op(kOpRayQueryGetIntersectionPrimitiveIndex, ty.u, {query, intersection});
                // RayQueryCommittedIntersectionTriangleKHR = 1, RayQueryCandidateIntersectionTriangleKHR = 0.
                h.valid = e.Cmp(kOpIEqual, e.Op(kOpRayQueryGetIntersectionType, ty.u, {query, intersection}), e.KU(committed ? 1 : 0));
                h.o = e.Op(kOpRayQueryGetIntersectionObjectRayOrigin, ty.v3, {query, intersection});
                h.d = e.Op(kOpRayQueryGetIntersectionObjectRayDirection, ty.v3, {query, intersection});
                h.t = e.Op(kOpRayQueryGetIntersectionT, ty.f, {query, intersection});
                h.tmin = e.Op(kOpRayQueryGetRayTMin, ty.f, {query});
                const Context c = Decode(e, ty, h);
                ++report_.queryContexts;
                return live.emplace(key, c).first->second;
            };
            for (auto& inst : f.body)
            {
                const uint16_t op = inst.Op();
                if (op == kOpLabel) live.clear();
                // Anything that moves a ray query (or might, through a call) ends the decodes of this block.
                if (op == kOpRayQueryInitialize || op == kOpRayQueryTerminate || op == kOpRayQueryGenerateIntersection || op == kOpRayQueryConfirmIntersection
                    || op == kOpRayQueryProceed || op == kOpFunctionCall)
                    live.clear();
                if (LssQueryOp(op) && inst.w.size() == 5)
                {
                    const Word type = inst.w[1], result = inst.w[2];
                    const Context c = context(inst.w[3], inst.w[4]);
                    if (op == kOpRayQueryGetIntersectionLssPositions) e.OpTo(kOpCompositeConstruct, type, result, {c.p, c.q});
                    else if (op == kOpRayQueryGetIntersectionLssRadii) e.OpTo(kOpCompositeConstruct, type, result, {c.rp, c.rq});
                    else if (op == kOpRayQueryGetIntersectionLssHitValue) e.OpTo(kOpCopyObject, type, result, {c.u});
                    else e.OpTo(kOpCopyObject, type, result, {c.lss});
                    ++report_.ops;
                    continue;
                }
                if ((op == kOpRayQueryGetIntersectionPrimitiveIndex || op == kOpRayQueryGetIntersectionT || op == kOpRayQueryGetIntersectionBarycentrics)
                    && inst.w.size() == 5)
                {
                    // The read keeps its operands under a new id; its old id becomes the redirection.
                    const Word type = inst.w[1], result = inst.w[2], query = inst.w[3], intersection = inst.w[4];
                    const Word raw = m_.NewId();
                    inst.w[2] = raw;
                    out.push_back(inst);
                    const Context c = context(query, intersection);
                    if (op == kOpRayQueryGetIntersectionPrimitiveIndex)
                        e.OpTo(kOpSelect, type, result, {c.lss, type == ty.u ? c.seg : e.Op(kOpBitcast, type, {c.seg}), raw});
                    else if (op == kOpRayQueryGetIntersectionT) e.OpTo(kOpSelect, type, result, {c.lss, c.t, raw});
                    else e.OpTo(kOpSelect, type, result, {c.lss, e.Op(kOpCompositeConstruct, type, {c.u, e.K(0.0f)}), raw});
                    ++report_.reads;
                    continue;
                }
                out.push_back(inst);
            }
            if (!contextError.empty()) return Fail(error, contextError);
            f.body = std::move(out);
        }
        m_.Reindex();
        return true;
    }

    Module& m_;
    SpirvRewriteReport& report_;
    const Types* ty_{};
    std::vector<Entry> entries_;
    std::vector<std::vector<uint32_t>> reachedBy_; // function index -> execution models of the entry points reaching it
    std::map<size_t, size_t> entryFunction_;       // hit entry point index -> its function index
};

bool Neutralise(Module& m, SpirvRewriteReport& report, std::string& error)
{
    uint32_t b = 0;
    std::vector<Word> vars;
    for (const auto& [id, storage] : GlobalVariables(m))
        if (m.BuiltInOf(id, b) && (LssBuiltIn(b) || SphereBuiltIn(b))) vars.push_back(id);
    for (const Word id : vars)
    {
        const Word pointee = PointeeOf(m, m.TypeOf(id));
        if (!pointee || !ToPrivate(m, id, m.Null(pointee), error)) return false;
        ++report.neutralised;
    }
    for (auto& f : m.functions)
        for (auto& inst : f.body)
            if ((LssQueryOp(inst.Op()) || OtherPrimitiveOp(inst.Op())) && inst.w.size() >= 3)
            {
                const Word type = inst.w[1], result = inst.w[2];
                inst = Make(kOpCopyObject, {type, result, m.Null(type)});
                ++report.neutralised;
            }
    RemoveLssDeclarations(m);
    return true;
}
}

bool SpirvUsesLss(std::span<const uint32_t> words)
{
    if (words.size() < 5 || words[0] != spirv::kMagic) return false;
    for (size_t i = 5; i < words.size();)
    {
        const uint32_t count = words[i] >> 16;
        if (count == 0 || i + count > words.size() || (words[i] & 0xFFFFu) != kOpCapability) return false;
        if (count >= 2 && (words[i + 1] == kCapLss || words[i + 1] == kCapSpheres)) return true;
        i += count;
    }
    return false;
}

SpirvRewrite NeutraliseLssSpirv(std::span<const uint32_t> words, std::vector<uint32_t>& out, SpirvRewriteReport& report, std::string& error)
{
    out.clear();
    if (!SpirvUsesLss(words)) return SpirvRewrite::Unchanged;
    Module m;
    if (!m.Parse(words, error)) return SpirvRewrite::Failed;
    std::string why;
    if (!Neutralise(m, report, why))
    {
        error = why.empty() ? "the neutral rewrite failed" : why;
        return SpirvRewrite::Failed;
    }
    out = m.Emit();
    return SpirvRewrite::Neutral;
}

SpirvRewrite RewriteLssSpirv(std::span<const uint32_t> words, std::vector<uint32_t>& out, SpirvRewriteReport& report, std::string& error,
    const LssDecodeLayout& layout)
{
    out.clear();
    report = {};
    error.clear();
    if (!SpirvUsesLss(words)) return SpirvRewrite::Unchanged;
    struct Scoped
    {
        LssDecodeLayout saved = tLayout;
        explicit Scoped(const LssDecodeLayout& l) { tLayout = l; }
        ~Scoped() { tLayout = saved; }
    } scoped(layout);
    Module m;
    if (!m.Parse(words, error)) return SpirvRewrite::Failed;
    std::string why;
    Rewriter rewriter(m, report);
    if (rewriter.Run(why))
    {
        out = m.Emit();
        return SpirvRewrite::Rewritten;
    }
    // From the original words again: the failed attempt may have edited the module.
    SpirvRewriteReport neutral;
    neutral.note = "full rewrite: " + why;
    std::string neutralError;
    const SpirvRewrite result = NeutraliseLssSpirv(words, out, neutral, neutralError);
    report = neutral;
    error = result == SpirvRewrite::Neutral ? why : neutralError;
    return result;
}
}
