#include "lss_ir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>

namespace rtx_hair {
namespace {
using std::string;

// NVAPI extension opcodes (nvShaderExtnEnums.h, R610).
constexpr int kOpTrianglePositions = 86;
constexpr int kOpCommittedTrianglePositions = 98;
constexpr int kOpLssPositions = 104;
constexpr int kOpCommittedLssPositions = 106;
constexpr int kOpIsLssHit = 112;
constexpr int kOpCommittedIsLss = 114;
constexpr int kOpCommittedLssHitParameter = 117;
bool IsLssOp(long op) { return (op >= 104 && op <= 107) || (op >= 112 && op <= 119); }
bool HitOp(int op) { return op == kOpLssPositions || op == kOpIsLssHit; }
bool CommittedOp(int op) { return op == kOpCommittedLssPositions || op == kOpCommittedIsLss || op == kOpCommittedLssHitParameter; }
int Outputs(int op) { return op == kOpLssPositions || op == kOpCommittedLssPositions ? 8 : 1; }
// NvShaderExtnStruct::src0u.x: the ray query's flags for ray-query ops.
constexpr long kSrc0Offset = 76;

struct Failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const string& what) { throw Failure(what); }

const std::regex& Rx(const char* pattern)
{
    // Compiled once per thread and call site (each call site passes one literal);
    // pipelines are created on many threads.
    thread_local std::map<const char*, std::regex> cache;
    auto it = cache.find(pattern);
    if (it == cache.end()) it = cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript | std::regex::optimize)).first;
    return it->second;
}

#define NAME R"((%[-\w.$]+))"
#define ANY R"((%[-\w.$]+|-?\d+|undef))"

bool Match(const string& line, const char* needle, const char* pattern, std::smatch& m)
{
    return line.find(needle) != string::npos && std::regex_search(line, m, Rx(pattern));
}

// The value a line defines ("  %12 = ..."), or "".
string Def(const string& line)
{
    size_t at = line.find_first_not_of(' ');
    if (at == string::npos || line[at] != '%') return {};
    const size_t eq = line.find(" = ", at);
    if (eq == string::npos) return {};
    const string name = line.substr(at, eq - at);
    return name.find(' ') == string::npos ? name : string{};
}

// Whether `line` mentions value `name` as a whole token.
bool UsesToken(const string& line, const string& name)
{
    for (size_t at = line.find(name); at != string::npos; at = line.find(name, at + 1))
    {
        const size_t after = at + name.size();
        if (after == line.size() || !(isalnum(static_cast<unsigned char>(line[after])) || line[after] == '.' || line[after] == '_'
                || line[after] == '$' || line[after] == '-'))
            return true;
    }
    return false;
}

bool Label(const string& line) { return line.rfind("; <label>:", 0) == 0 || (!line.empty() && line[0] != ' ' && line.back() == ':'); }

// A float constant in LLVM IR's exact hexadecimal (double) form.
string Hex(float value)
{
    const double d = value;
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(bits));
    return text;
}

// Branch-free IR under one name prefix.
class Emitter
{
public:
    Emitter(string prefix, std::vector<string>& out, std::set<string>& used) : prefix_(std::move(prefix)), out_(out), used_(used) {}
    string Op(const string& rhs)
    {
        const string name = prefix_ + std::to_string(next_++);
        out_.push_back("  " + name + " = " + rhs);
        return name;
    }
    void Raw(const string& line) { out_.push_back("  " + line); }
    void Use(const char* fn) { used_.insert(fn); }
    string Call(const char* fn, const char* ret, const string& args)
    {
        used_.insert(fn);
        return Op(string("call ") + ret + " @" + fn + "(" + args + ")");
    }
    string FAdd(const string& a, const string& b) { return Op("fadd float " + a + ", " + b); }
    string FSub(const string& a, const string& b) { return Op("fsub float " + a + ", " + b); }
    string FMul(const string& a, const string& b) { return Op("fmul float " + a + ", " + b); }
    string FDiv(const string& a, const string& b) { return Op("fdiv float " + a + ", " + b); }
    string Sqrt(const string& a) { return Call("dx.op.unary.f32", "float", "i32 24, float " + a); }
    string Abs(const string& a) { return Call("dx.op.unary.f32", "float", "i32 6, float " + a); }
    string Max(const string& a, const string& b) { return Call("dx.op.binary.f32", "float", "i32 35, float " + a + ", float " + b); }
    string Min(const string& a, const string& b) { return Call("dx.op.binary.f32", "float", "i32 36, float " + a + ", float " + b); }
    string FCmp(const char* predicate, const string& a, const string& b) { return Op(string("fcmp ") + predicate + " float " + a + ", " + b); }
    string And(const string& a, const string& b) { return Op("and i1 " + a + ", " + b); }
    string Or(const string& a, const string& b) { return Op("or i1 " + a + ", " + b); }
    string Sel(const string& c, const string& a, const string& b, const char* type = "float")
    {
        return Op("select i1 " + c + ", " + type + " " + a + ", " + type + " " + b);
    }
    string Finite(const string& a) { return FCmp("olt", Abs(a), Hex(1.0e30f)); }

private:
    string prefix_;
    std::vector<string>& out_;
    std::set<string>& used_;
    int next_{};
};

struct V3
{
    string x, y, z;
};
V3 Sub(Emitter& e, const V3& a, const V3& b) { return {e.FSub(a.x, b.x), e.FSub(a.y, b.y), e.FSub(a.z, b.z)}; }
V3 Add(Emitter& e, const V3& a, const V3& b) { return {e.FAdd(a.x, b.x), e.FAdd(a.y, b.y), e.FAdd(a.z, b.z)}; }
V3 Scale(Emitter& e, const V3& a, const string& s) { return {e.FMul(a.x, s), e.FMul(a.y, s), e.FMul(a.z, s)}; }
string Dot(Emitter& e, const V3& a, const V3& b) { return e.FAdd(e.FAdd(e.FMul(a.x, b.x), e.FMul(a.y, b.y)), e.FMul(a.z, b.z)); }
V3 Sel(Emitter& e, const string& c, const V3& a, const V3& b) { return {e.Sel(c, a.x, b.x), e.Sel(c, a.y, b.y), e.Sel(c, a.z, b.z)}; }

// How a function reaches the NVAPI extension UAV, copied from one of its LSS sites so
// the emitted sequences have the shape the driver recognizes.
struct ExtAccess
{
    string global;       // library: the handle global (load + createHandleForLib per access)
    string root;         // otherwise: the entry-block handle (createHandleFromBinding / createHandle)
    string counterProps; // annotateHandle properties of the counter access ("" = no annotateHandle)
    string storeProps;   // and of the opcode store
    bool rawStore{true}; // rawBufferStore (op 140) or bufferStore (op 69)
};

string ExtHandle(Emitter& e, const ExtAccess& x, const string& props)
{
    string handle = x.root;
    if (!x.global.empty())
    {
        const string loaded = e.Op("load %dx.types.Handle, %dx.types.Handle* " + x.global + ", align 4");
        handle = e.Call("dx.op.createHandleForLib.dx.types.Handle", "%dx.types.Handle", "i32 160, %dx.types.Handle " + loaded);
    }
    if (!props.empty())
        handle = e.Call("dx.op.annotateHandle", "%dx.types.Handle", "i32 216, %dx.types.Handle " + handle + ", %dx.types.ResourceProperties " + props);
    return handle;
}

void ExtStore(Emitter& e, const ExtAccess& x, const string& index, long offset, const string& value)
{
    const string handle = ExtHandle(e, x, x.storeProps);
    const char* fn = x.rawStore ? "dx.op.rawBufferStore.i32" : "dx.op.bufferStore.i32";
    e.Use(fn);
    const string tail = x.rawStore ? ", i8 1, i32 4)" : ", i8 1)";
    e.Raw(string("call void @") + fn + "(i32 " + (x.rawStore ? "140" : "69") + ", %dx.types.Handle " + handle + ", i32 " + index + ", i32 "
        + std::to_string(offset) + ", i32 " + value + ", i32 undef, i32 undef, i32 undef" + tail);
}

// The decoded LSS hit, for one hit shader or one ray-query state.
struct Context
{
    string lss;    // i1: a valid triangle hit carrying the DOTS marker
    string seg;    // i32: the segment (LSS primitive) index
    string p[4];   // endpoint 0: x, y, z, radius
    string q[4];   // endpoint 1
    string t;      // the hit distance the shader should see
    string u;      // the hit parameter along the segment, 0..1
};

// Emits one vertex fetch and everything decoded from it. `query` empty: hit shader
// (op 86, the current hit); else a ray query's committed hit (op 98).
Context EmitContext(Emitter& e, const ExtAccess& x, const string& query)
{
    const bool hit = query.empty();
    // NVAPI triangle position fetch: index, opcode (+ ray flags), nine outputs.
    string flags;
    if (!hit) flags = e.Call("dx.op.rayQuery_StateScalar.i32", "i32", "i32 195, i32 " + query);
    const string index = e.Call("dx.op.bufferUpdateCounter", "i32",
        "i32 70, %dx.types.Handle " + ExtHandle(e, x, x.counterProps) + ", i8 1");
    ExtStore(e, x, index, 0, std::to_string(hit ? kOpTrianglePositions : kOpCommittedTrianglePositions));
    if (!hit) ExtStore(e, x, index, kSrc0Offset, flags);
    string v[9];
    for (auto& value : v)
    {
        const string word = e.Call("dx.op.bufferUpdateCounter", "i32", "i32 70, %dx.types.Handle " + ExtHandle(e, x, x.counterProps) + ", i8 1");
        value = e.Op("bitcast i32 " + word + " to float");
    }
    // The hit: primitive, validity, object-space ray, hardware distance.
    string prim, valid, t, tmin;
    V3 o, d;
    const char* axes[3] = {"0", "1", "2"};
    string* oc[3] = {&o.x, &o.y, &o.z};
    string* dc[3] = {&d.x, &d.y, &d.z};
    if (hit)
    {
        prim = e.Call("dx.op.primitiveIndex.i32", "i32", "i32 161");
        const string kind = e.Call("dx.op.hitKind.i32", "i32", "i32 143");
        valid = e.Op("icmp uge i32 " + kind + ", 254"); // triangle front / back face
        for (int c = 0; c < 3; ++c)
        {
            *oc[c] = e.Call("dx.op.objectRayOrigin.f32", "float", string("i32 149, i8 ") + axes[c]);
            *dc[c] = e.Call("dx.op.objectRayDirection.f32", "float", string("i32 150, i8 ") + axes[c]);
        }
        t = e.Call("dx.op.rayTCurrent.f32", "float", "i32 154");
        tmin = e.Call("dx.op.rayTMin.f32", "float", "i32 153");
    }
    else
    {
        prim = e.Call("dx.op.rayQuery_StateScalar.i32", "i32", "i32 210, i32 " + query);
        const string status = e.Call("dx.op.rayQuery_StateScalar.i32", "i32", "i32 184, i32 " + query);
        valid = e.Op("icmp eq i32 " + status + ", 1"); // COMMITTED_TRIANGLE_HIT
        for (int c = 0; c < 3; ++c)
        {
            *oc[c] = e.Call("dx.op.rayQuery_StateVector.f32", "float", "i32 211, i32 " + query + ", i8 " + axes[c]);
            *dc[c] = e.Call("dx.op.rayQuery_StateVector.f32", "float", "i32 212, i32 " + query + ", i8 " + axes[c]);
        }
        t = e.Call("dx.op.rayQuery_StateScalar.f32", "float", "i32 200, i32 " + query);
        tmin = e.Call("dx.op.rayQuery_StateScalar.f32", "float", "i32 198, i32 " + query);
    }
    // Marker: all nine coordinates.
    string tag;
    for (const auto& value : v)
    {
        const string bits = e.Op("bitcast float " + value + " to i32");
        const string low = e.Op("and i32 " + bits + ", " + std::to_string(kTagMask));
        const string ok = e.Op("icmp eq i32 " + low + ", " + std::to_string(kTagValue));
        tag = tag.empty() ? ok : e.And(tag, ok);
    }
    Context c;
    c.lss = e.And(valid, tag);
    // kTrianglesPerSegment per segment (dots_converter.hlsl): triangle t = prim % that,
    // t & 1 odd; with two windings, the second half reversed.
    c.seg = e.Op("udiv i32 " + prim + ", " + std::to_string(kTrianglesPerSegment));
    const string local = e.Op("urem i32 " + prim + ", " + std::to_string(kTrianglesPerSegment));
    // Decode. Even triangle (A, B, C): M = q = (B+C)/2, X0 = A; odd (A, D, B): M = p =
    // (A+D)/2, X0 = B. The ribbon axis a = (C-B) or (A-D), normalized; the other
    // endpoint's radius follows from (q - p) . a = 0. A reversed triangle (A, C, B) or
    // (D, A, B) has the same M and X0 and the opposite axis, which the sign undoes.
    const string parity = e.Op("and i32 " + local + ", 1");
    const string even = e.Op("icmp eq i32 " + parity + ", 0");
    string forward = even;
    if constexpr (kWindings == 2)
    {
        const string reversed = e.Op("zext i1 " + e.Op("icmp uge i32 " + local + ", " + std::to_string(kTrianglesPerSegment / 2)) + " to i32");
        forward = e.Op("icmp eq i32 " + e.Op("xor i32 " + parity + ", " + reversed) + ", 0");
    }
    const V3 v0{v[0], v[1], v[2]}, v1{v[3], v[4], v[5]}, v2{v[6], v[7], v[8]};
    const string half = Hex(0.5f);
    const V3 x0 = Sel(e, even, v0, v2);
    const V3 m = Scale(e, Add(e, Sel(e, even, v1, v0), Sel(e, even, v2, v1)), half);
    const V3 dv = Sub(e, Sel(e, even, v2, v0), v1);
    const string len = e.Sqrt(Dot(e, dv, dv));
    const string positive = e.FCmp("ogt", len, Hex(0.0f));
    const string inv = e.Sel(positive, e.FDiv(Hex(1.0f), len), Hex(0.0f));
    const V3 a = Scale(e, dv, inv);
    const string rm = e.FMul(len, Hex(0.5f / kRibbon));
    const string sign = e.Sel(forward, Hex(1.0f), Hex(-1.0f));
    const string rx = e.FMul(e.FMul(sign, Dot(e, Sub(e, x0, m), a)), Hex(1.0f / kRibbon));
    const V3 xe = Sub(e, x0, Scale(e, a, e.FMul(e.FMul(sign, rx), Hex(kRibbon))));
    const V3 p = Sel(e, even, xe, m), q = Sel(e, even, m, xe);
    const string rp = e.Sel(even, rx, rm), rq = e.Sel(even, rm, rx);
    c.p[0] = p.x; c.p[1] = p.y; c.p[2] = p.z; c.p[3] = rp;
    c.q[0] = q.x; c.q[1] = q.y; c.q[2] = q.z; c.q[3] = rq;
    // Distance to the round strand (a cone body tangent to two end spheres), branch-free,
    // on the normalized object-space ray, solved in the segment's own frame from the ray's
    // closest approach to the axis (and each sphere around its own) so the quantities stay
    // at the strand's scale: about the ray origin, float32 cancellation loses thin strands.
    const string zero = Hex(0.0f), big = Hex(1.0e20f);
    const string dl = e.Sqrt(Dot(e, d, d));
    const string dinv = e.FDiv(Hex(1.0f), dl);
    const V3 rd = Scale(e, d, dinv);
    const V3 ba = Sub(e, q, p);
    const string m0 = Dot(e, ba, ba);
    const string length = e.Sqrt(m0);
    const string linv = e.Sel(e.FCmp("ogt", length, zero), e.FDiv(Hex(1.0f), length), zero);
    const V3 axis = Scale(e, ba, linv);
    const V3 oa = Sub(e, o, p);
    const string z0 = Dot(e, oa, axis);
    const V3 p0 = Sub(e, oa, Scale(e, axis, z0));      // origin: perpendicular to the axis
    const string dz = Dot(e, rd, axis);
    const V3 dp = Sub(e, rd, Scale(e, axis, dz));      // direction: perpendicular part
    // The strand the hit is reported on encloses the ribbons (lss_ir.h kHitRadius).
    const string hp = e.FMul(rp, Hex(kHitRadius)), hq = e.FMul(rq, Hex(kHitRadius));
    const string slope = e.FMul(e.FSub(hp, hq), linv); // sine of the cone's half-angle
    const string cos2 = e.FSub(Hex(1.0f), e.FMul(slope, slope));
    const string dd = Dot(e, dp, dp);
    const string t0 = e.FDiv(e.FSub(zero, Dot(e, p0, dp)), dd); // closest approach to the axis
    const V3 p1 = Add(e, p0, Scale(e, dp, t0));
    const string z1 = e.FAdd(z0, e.FMul(dz, t0));
    const string c1 = e.FSub(hp, e.FMul(slope, z1));
    const string qa = e.FSub(e.FMul(cos2, dd), e.FMul(e.FMul(slope, slope), e.FMul(dz, dz)));
    const string qb = e.FMul(e.FMul(slope, dz), c1);
    const string qc = e.FSub(e.FMul(cos2, Dot(e, p1, p1)), e.FMul(c1, c1));
    const string disc = e.FSub(e.FMul(qb, qb), e.FMul(qa, qc));
    const string tb = e.FAdd(t0, e.FDiv(e.FSub(e.FSub(zero, qb), e.Sqrt(e.Max(disc, zero))), qa));
    const string zh = e.FAdd(z0, e.FMul(dz, tb));
    const string rh = e.FSub(hp, e.FMul(slope, zh));    // body radius there (the near nappe: >= 0)
    const string foot = e.FSub(zh, e.FDiv(e.FMul(rh, slope), cos2)); // the touching sphere's centre on the axis
    const string body = e.And(e.And(e.And(e.FCmp("oge", disc, zero), e.FCmp("ogt", qa, zero)), e.And(e.Finite(tb), e.FCmp("ogt", cos2, zero))),
        e.And(e.And(e.FCmp("oge", foot, zero), e.FCmp("ole", foot, length)), e.FCmp("oge", rh, zero)));
    auto cap = [&](const V3& centre, const string& radius) {
        const string tc = Dot(e, Sub(e, centre, o), rd);
        const V3 w = Sub(e, Add(e, o, Scale(e, rd, tc)), centre);
        const string hh = e.FSub(e.FMul(radius, radius), Dot(e, w, w));
        return e.Sel(e.FCmp("oge", hh, zero), e.FSub(tc, e.Sqrt(e.Max(hh, zero))), big);
    };
    const string caps = e.Min(cap(p, hp), cap(q, hq));
    const string tn = e.Sel(body, e.Min(tb, caps), caps);
    const string ta = e.FMul(tn, dinv);
    const string ok = e.And(e.Or(body, e.FCmp("olt", caps, Hex(1.0e19f))), e.And(e.Finite(ta), e.FCmp("oge", ta, tmin)));
    const string use = e.And(c.lss, ok);
    c.t = e.Sel(use, ta, t);
    // u: the hit point projected on the segment.
    const V3 at = Add(e, o, Scale(e, d, c.t));
    const string along = e.FDiv(Dot(e, Sub(e, at, p), ba), m0);
    const string uRaw = e.Sel(e.FCmp("ogt", m0, Hex(0.0f)), along, Hex(0.0f));
    c.u = e.Max(e.Min(uRaw, Hex(1.0f)), Hex(0.0f));
    return c;
}

struct Store
{
    size_t line;
    string handle, index, value;
    long offset;
    bool raw;
};
struct Counter
{
    size_t line;
    string name, handle;
};
struct Site
{
    int op;
    size_t store;               // opcode store line
    string index;               // index counter value
    size_t indexLine;
    std::vector<size_t> stores; // every store at the index (deleted)
    string query;               // committed ops
    std::vector<string> outputs;
    std::vector<size_t> outputLines;
};
enum class Read { Primitive, BaryVector, BaryX, BaryY, RayT };
struct ReadSite
{
    size_t line;
    string value, query; // query empty: hit shader
    Read kind;
};

struct Module
{
    string global;                // library: g_NvidiaExt's handle global
    long rangeId{-1}, space{-1}, reg{-1}; // non-library: its UAV binding
    std::set<string> declared;    // dx.op functions declared
};

// One function: returns its rewritten lines (or the original ones when it has no LSS op).
std::vector<string> RewriteFunction(const std::vector<string>& lines, size_t begin, size_t end, const Module& module, int& contextId,
    std::set<string>& used, IrRewriteReport& report)
{
    std::map<string, string> rootOf, propsOf; // ext handles: value -> root, annotate properties
    std::set<string> extLoads;
    std::map<string, size_t> defLine;
    std::map<string, string> flagsQuery;      // RayFlags value -> query
    std::map<string, Counter> counters;
    std::vector<Store> stores;
    std::vector<size_t> blockOf(end, 0);
    size_t block = 0;
    std::smatch m;
    for (size_t i = begin + 1; i < end; ++i)
    {
        const string& line = lines[i];
        if (Label(line)) ++block;
        blockOf[i] = block;
        const string def = Def(line);
        if (!def.empty()) defLine[def] = i;
        if (!module.global.empty() && Match(line, "load %dx.types.Handle", R"(^\s*)" NAME R"( = load %dx\.types\.Handle, %dx\.types\.Handle\* (\S+), align)", m)
            && m[2] == module.global)
            extLoads.insert(m[1]);
        else if (Match(line, "createHandleForLib", R"(^\s*)" NAME R"( = call %dx\.types\.Handle @dx\.op\.createHandleForLib\.dx\.types\.Handle\(i32 160, %dx\.types\.Handle )" NAME R"(\))", m)
            && extLoads.count(m[2]))
            rootOf[m[1]] = m[1];
        else if (module.reg >= 0 && Match(line, "createHandleFromBinding", R"(^\s*)" NAME R"( = call %dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, %dx\.types\.ResBind \{ i32 (\d+), i32 (\d+), i32 (\d+), i8 1 \}, i32 (\d+), i1 false\))", m)
            && std::stol(m[2]) == module.reg && std::stol(m[4]) == module.space && std::stol(m[5]) == module.reg)
            rootOf[m[1]] = m[1];
        else if (module.rangeId >= 0 && Match(line, "createHandle(", R"(^\s*)" NAME R"( = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 1, i32 (\d+), i32 (\d+), i1 false\))", m)
            && std::stol(m[2]) == module.rangeId)
            rootOf[m[1]] = m[1];
        else if (Match(line, "annotateHandle", R"(^\s*)" NAME R"( = call %dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle )" NAME R"(, %dx\.types\.ResourceProperties (\{[^}]*\}|zeroinitializer)\))", m)
            && rootOf.count(m[2]))
        {
            rootOf[m[1]] = rootOf[m[2]];
            propsOf[m[1]] = m[3];
        }
        else if (Match(line, "bufferUpdateCounter", R"(^\s*)" NAME R"( = call i32 @dx\.op\.bufferUpdateCounter\(i32 70, %dx\.types\.Handle )" NAME R"(, i8 1\))", m)
            && rootOf.count(m[2]))
            counters[m[1]] = {i, m[1], m[2]};
        else if (Match(line, "ufferStore.i32", R"(call void @dx\.op\.(raw)?[bB]ufferStore\.i32\(i32 (140|69), %dx\.types\.Handle )" NAME R"(, i32 )" NAME R"(, i32 (\d+), i32 )" ANY R"(,)", m)
            && rootOf.count(m[3]))
            stores.push_back({i, m[3], m[4], m[6], std::stol(m[5]), m[1].matched});
        else if (Match(line, "rayQuery_StateScalar.i32(i32 195", R"(^\s*)" NAME R"( = call i32 @dx\.op\.rayQuery_StateScalar\.i32\(i32 195, i32 )" NAME R"(\))", m))
            flagsQuery[m[1]] = m[2];
    }
    // LSS sites.
    std::vector<Site> sites;
    std::set<string> indexValues;
    for (const auto& s : stores)
        if (s.offset == 0) indexValues.insert(s.index);
    for (const auto& s : stores)
    {
        if (s.offset != 0 || s.value.empty() || (s.value[0] != '-' && !isdigit(static_cast<unsigned char>(s.value[0])))) continue;
        const long op = std::stol(s.value);
        if (!IsLssOp(op)) continue;
        if (!HitOp(static_cast<int>(op)) && !CommittedOp(static_cast<int>(op)))
            Fail("unsupported NVAPI LSS op " + std::to_string(op));
        Site site{static_cast<int>(op), s.line, s.index};
        const auto counter = counters.find(s.index);
        if (counter == counters.end()) Fail("LSS op " + s.value + ": index is not an extension counter");
        site.indexLine = counter->second.line;
        size_t last = s.line;
        string flags;
        for (const auto& other : stores)
        {
            if (other.index != s.index) continue;
            site.stores.push_back(other.line);
            last = std::max(last, other.line);
            if (other.offset == kSrc0Offset) flags = other.value;
            else if (other.offset != 0) Fail("LSS op " + s.value + ": unexpected operand at offset " + std::to_string(other.offset));
        }
        if (CommittedOp(site.op))
        {
            const auto query = flagsQuery.find(flags);
            if (query == flagsQuery.end()) Fail("LSS op " + s.value + ": ray flags are not a ray query's RayFlags()");
            site.query = query->second;
        }
        else if (!flags.empty()) Fail("LSS op " + s.value + ": unexpected ray flags");
        // The outputs: the next extension counters in the same block.
        std::vector<const Counter*> after;
        for (const auto& [name, c] : counters)
            if (c.line > last) after.push_back(&c);
        std::sort(after.begin(), after.end(), [](const Counter* a, const Counter* b) { return a->line < b->line; });
        if (after.size() < static_cast<size_t>(Outputs(site.op))) Fail("LSS op " + s.value + ": outputs missing");
        for (int k = 0; k < Outputs(site.op); ++k)
        {
            const Counter* c = after[k];
            if (blockOf[c->line] != blockOf[s.line] || indexValues.count(c->name)) Fail("LSS op " + s.value + ": outputs not found");
            site.outputs.push_back(c->name);
            site.outputLines.push_back(c->line);
        }
        for (const auto& other : stores)
            if (other.offset == 0 && other.line > s.line && other.line < site.outputLines.back()) Fail("LSS op " + s.value + ": interleaved extension op");
        sites.push_back(std::move(site));
    }
    if (sites.empty()) return {lines.begin() + begin, lines.begin() + end + 1};
    ++report.functions;

    // Extension access shape, from the first site.
    ExtAccess x;
    {
        const Site& s = sites.front();
        const string& counterHandle = counters.at(s.index).handle;
        const Store* opcode = nullptr;
        for (const auto& st : stores)
            if (st.line == s.store) opcode = &st;
        x.counterProps = propsOf.count(counterHandle) ? propsOf[counterHandle] : string{};
        x.storeProps = propsOf.count(opcode->handle) ? propsOf[opcode->handle] : string{};
        x.rawStore = opcode->raw;
        if (!module.global.empty()) x.global = module.global;
        else
        {
            x.root = rootOf.at(counterHandle);
            if (blockOf[defLine.at(x.root)] != 0) Fail("extension handle not created in the entry block");
        }
    }
    bool hitShader = false;
    std::set<string> queries;
    for (const auto& s : sites)
    {
        if (HitOp(s.op)) hitShader = true;
        else queries.insert(s.query);
    }
    if (hitShader && x.global.empty()) Fail("hit-shader LSS op outside a library");

    // Reads redirected to the LSS values.
    std::map<size_t, ReadSite> reads;
    string attributes;
    if (hitShader && Match(lines[begin], "BuiltInTriangleIntersectionAttributes", R"(%struct\.BuiltInTriangleIntersectionAttributes\*[^,)]*? )" NAME R"(\))", m))
        attributes = m[1];
    std::map<string, int> attributeElement; // GEP into the attributes -> element (-1: the whole float2)
    for (size_t i = begin + 1; i < end; ++i)
    {
        const string& line = lines[i];
        if (hitShader)
        {
            if (Match(line, "primitiveIndex", R"(^\s*)" NAME R"( = call i32 @dx\.op\.primitiveIndex\.i32\(i32 161\))", m)) reads[i] = {i, m[1], "", Read::Primitive};
            else if (Match(line, "rayTCurrent", R"(^\s*)" NAME R"( = call float @dx\.op\.rayTCurrent\.f32\(i32 154\))", m)) reads[i] = {i, m[1], "", Read::RayT};
            else if (!attributes.empty() && Match(line, "getelementptr", R"(^\s*)" NAME R"( = getelementptr inbounds %struct\.BuiltInTriangleIntersectionAttributes, %struct\.BuiltInTriangleIntersectionAttributes\* )" NAME R"(, i32 0, i32 0(?:, i32 (\d))?)", m)
                && m[2] == attributes)
                attributeElement[m[1]] = m[3].matched ? std::stoi(m[3]) : -1;
            else if (!attributes.empty() && Match(line, "load ", R"(^\s*)" NAME R"( = load (<2 x float>|float), (?:<2 x float>|float)\* )" NAME R"(, align)", m)
                && attributeElement.count(m[3]))
            {
                const int element = attributeElement[m[3]];
                const bool vector = m[2] == "<2 x float>";
                if (vector != (element < 0) || element > 1) Fail("unexpected barycentric load");
                reads[i] = {i, m[1], "", vector ? Read::BaryVector : (element == 0 ? Read::BaryX : Read::BaryY)};
            }
            else if (!attributes.empty() && UsesToken(line, attributes))
                Fail("unexpected use of the hit attributes");
            else
                for (const auto& [gep, element] : attributeElement)
                    if (UsesToken(line, gep)) Fail("unexpected use of the barycentrics");
        }
        if (Match(line, "rayQuery_State", R"(^\s*)" NAME R"( = call (?:i32|float) @dx\.op\.rayQuery_State(Scalar|Vector)\.(?:i32|f32)\(i32 (\d+), i32 )" NAME R"((?:, i8 (\d))?\))", m)
            && queries.count(m[4]))
        {
            const long op = std::stol(m[3]);
            if (op == 210) reads[i] = {i, m[1], m[4], Read::Primitive};
            else if (op == 200) reads[i] = {i, m[1], m[4], Read::RayT};
            else if (op == 194) reads[i] = {i, m[1], m[4], m[5] == "0" ? Read::BaryX : Read::BaryY};
        }
    }

    // Emit.
    std::vector<string> out{lines[begin]};
    std::map<string, string> rename;          // original value -> redirected value
    std::set<size_t> keep;                     // output lines exempt from renaming
    const string prefix = "%rtxhair." + std::to_string(contextId++) + ".";
    Emitter e(prefix, out, used);
    Context hitContext;
    if (hitShader)
    {
        hitContext = EmitContext(e, x, "");
        ++report.hitContexts;
    }
    std::map<string, Context> live; // ray query -> context valid in the current block
    std::map<size_t, const Site*> byIndexLine;
    std::map<size_t, std::pair<const Site*, int>> byOutput;
    std::set<size_t> deleted;
    for (const auto& s : sites)
    {
        byIndexLine[s.indexLine] = &s;
        for (size_t l : s.stores) deleted.insert(l);
        for (size_t k = 0; k < s.outputLines.size(); ++k) byOutput[s.outputLines[k]] = {&s, static_cast<int>(k)};
    }
    auto contextFor = [&](const string& query) -> Context& {
        auto it = live.find(query);
        if (it == live.end())
        {
            it = live.emplace(query, EmitContext(e, x, query)).first;
            ++report.queryContexts;
        }
        return it->second;
    };
    for (size_t i = begin + 1; i < end; ++i)
    {
        const string& line = lines[i];
        if (Label(line)) live.clear();
        // Anything that moves a ray query invalidates its context.
        for (const char* op : {"rayQuery_Proceed", "rayQuery_TraceRayInline", "rayQuery_Abort", "rayQuery_CommitNonOpaqueTriangleHit",
                 "rayQuery_CommitProceduralPrimitiveHit"})
            if (line.find(op) != string::npos)
                for (auto it = live.begin(); it != live.end();)
                    it = line.find("i32 " + it->first + ")") != string::npos || line.find("i32 " + it->first + ",") != string::npos ? live.erase(it) : std::next(it);
        if (deleted.count(i)) continue;
        if (const auto s = byIndexLine.find(i); s != byIndexLine.end())
        {
            if (!s->second->query.empty()) contextFor(s->second->query);
            out.push_back("  " + s->second->index + " = add i32 0, 0");
            ++report.ops;
            continue;
        }
        if (const auto o = byOutput.find(i); o != byOutput.end())
        {
            const Site& s = *o->second.first;
            const int k = o->second.second;
            if (!s.query.empty() && !live.count(s.query)) Fail("ray query moved inside an LSS op");
            const Context& c = s.query.empty() ? hitContext : live.at(s.query);
            const string& name = s.outputs[k];
            if (s.op == kOpLssPositions || s.op == kOpCommittedLssPositions)
                out.push_back("  " + name + " = bitcast float " + (k < 4 ? c.p[k] : c.q[k - 4]) + " to i32");
            else if (s.op == kOpIsLssHit || s.op == kOpCommittedIsLss)
                out.push_back("  " + name + " = zext i1 " + c.lss + " to i32");
            else
                out.push_back("  " + name + " = bitcast float " + c.u + " to i32");
            continue;
        }
        if (const auto r = reads.find(i); r != reads.end())
        {
            const ReadSite& rs = r->second;
            const Context& c = rs.query.empty() ? hitContext : contextFor(rs.query);
            out.push_back(line);
            const size_t first = out.size() - 1; // the read and its redirection keep their operands
            string redirected;
            switch (rs.kind)
            {
            case Read::Primitive: redirected = e.Sel(c.lss, c.seg, rs.value, "i32"); break;
            case Read::RayT: redirected = e.Sel(c.lss, c.t, rs.value); break;
            case Read::BaryX: redirected = e.Sel(c.lss, c.u, rs.value); break;
            case Read::BaryY: redirected = e.Sel(c.lss, Hex(0.0f), rs.value); break;
            case Read::BaryVector:
            {
                // Scalar selects (DXIL has no vector select); instcombine folds the lanes.
                const string x0 = e.Op("extractelement <2 x float> " + rs.value + ", i32 0");
                const string x1 = e.Op("extractelement <2 x float> " + rs.value + ", i32 1");
                const string lane0 = e.Op("insertelement <2 x float> undef, float " + e.Sel(c.lss, c.u, x0) + ", i32 0");
                redirected = e.Op("insertelement <2 x float> " + lane0 + ", float " + e.Sel(c.lss, Hex(0.0f), x1) + ", i32 1");
                break;
            }
            }
            for (size_t k = first; k < out.size(); ++k) keep.insert(k);
            rename[rs.value] = redirected;
            ++report.reads;
            continue;
        }
        out.push_back(line);
    }
    out.push_back(lines[end]);
    // Point every other use of a redirected value at its redirection.
    if (!rename.empty())
    {
        static const std::regex token(R"(%[-\w.$]+)");
        for (size_t i = 1; i + 1 < out.size(); ++i)
        {
            if (keep.count(i) || out[i].find('%') == string::npos) continue;
            string& line = out[i];
            const string def = Def(line);
            string rewritten;
            size_t pos = 0;
            for (auto it = std::sregex_iterator(line.begin(), line.end(), token); it != std::sregex_iterator(); ++it)
            {
                const auto& t = *it;
                const size_t at = static_cast<size_t>(t.position());
                rewritten.append(line, pos, at - pos);
                const auto found = rename.find(t.str());
                const bool isDef = !def.empty() && at == line.find_first_not_of(' ') && t.str() == def;
                rewritten += found != rename.end() && !isDef ? found->second : t.str();
                pos = at + t.length();
            }
            rewritten.append(line, pos, string::npos);
            line = std::move(rewritten);
        }
    }
    return out;
}

struct Declaration
{
    const char* name;
    const char* text;
    int attributes; // 0 nounwind, 1 readnone, 2 readonly
};
constexpr Declaration kDeclarations[] = {
    {"dx.op.unary.f32", "declare float @dx.op.unary.f32(i32, float)", 1},
    {"dx.op.binary.f32", "declare float @dx.op.binary.f32(i32, float, float)", 1},
    {"dx.op.primitiveIndex.i32", "declare i32 @dx.op.primitiveIndex.i32(i32)", 1},
    {"dx.op.hitKind.i32", "declare i32 @dx.op.hitKind.i32(i32)", 1},
    {"dx.op.objectRayOrigin.f32", "declare float @dx.op.objectRayOrigin.f32(i32, i8)", 1},
    {"dx.op.objectRayDirection.f32", "declare float @dx.op.objectRayDirection.f32(i32, i8)", 1},
    {"dx.op.rayTCurrent.f32", "declare float @dx.op.rayTCurrent.f32(i32)", 2},
    {"dx.op.rayTMin.f32", "declare float @dx.op.rayTMin.f32(i32)", 1},
    {"dx.op.rayQuery_StateScalar.i32", "declare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32)", 2},
    {"dx.op.rayQuery_StateScalar.f32", "declare float @dx.op.rayQuery_StateScalar.f32(i32, i32)", 2},
    {"dx.op.rayQuery_StateVector.f32", "declare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8)", 2},
    {"dx.op.bufferUpdateCounter", "declare i32 @dx.op.bufferUpdateCounter(i32, %dx.types.Handle, i8)", 0},
    {"dx.op.rawBufferStore.i32", "declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32)", 0},
    {"dx.op.bufferStore.i32", "declare void @dx.op.bufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8)", 0},
    {"dx.op.createHandleForLib.dx.types.Handle", "declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle)", 2},
    {"dx.op.annotateHandle", "declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties)", 1},
};
} // namespace

bool UsesLss(std::string_view ir)
{
    static const std::regex store(R"(@dx\.op\.(?:raw)?[bB]ufferStore\.i32\(i32 (?:140|69), %dx\.types\.Handle %[-\w.$]+, i32 %[-\w.$]+, i32 0, i32 (\d+),)");
    for (auto it = std::cregex_iterator(ir.data(), ir.data() + ir.size(), store); it != std::cregex_iterator(); ++it)
        if (IsLssOp(std::stol((*it)[1].str()))) return true;
    return false;
}

bool RewriteLssIr(std::string_view ir, std::string& out, IrRewriteReport& report, std::string& error)
{
    out.clear();
    report = {};
    error.clear();
    try
    {
        std::vector<string> lines;
        for (size_t at = 0; at <= ir.size();)
        {
            size_t end = ir.find('\n', at);
            if (end == std::string_view::npos) end = ir.size();
            string line(ir.substr(at, end - at));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            at = end + 1;
        }
        Module module;
        std::smatch m;
        for (const auto& line : lines)
        {
            if (line.find("g_NvidiaExt") != string::npos && line.find("= external constant %dx.types.Handle") != string::npos)
                module.global = line.substr(0, line.find(" = "));
            // Its UAV record in !dx.resources: by name (libraries), or by the
            // NvShaderExtnStruct element type (other shaders: DXC strips the name).
            else if (Match(line, "!\"g_NvidiaExt\"", R"(^!\d+ = !\{i32 (\d+), .*!"g_NvidiaExt", i32 (\d+), i32 (\d+),)", m)
                || Match(line, "NvShaderExtnStruct>\"* undef", R"(^!\d+ = !\{i32 (\d+), %"class\.RWStructuredBuffer<NvShaderExtnStruct>"\* undef, !"[^"]*", i32 (\d+), i32 (\d+),)", m))
            {
                module.rangeId = std::stol(m[1]);
                module.space = std::stol(m[2]);
                module.reg = std::stol(m[3]);
            }
            else if (Match(line, "declare ", R"(^declare [^@]*@(dx\.op\.[\w.]+)\()", m))
                module.declared.insert(m[1]);
        }
        if (module.global.empty() && module.reg < 0) Fail("no NVAPI extension UAV (g_NvidiaExt)");
        std::vector<string> result;
        std::set<string> used;
        int contextId = 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].rfind("define ", 0) != 0)
            {
                result.push_back(lines[i]);
                continue;
            }
            size_t end = i + 1;
            while (end < lines.size() && lines[end] != "}") ++end;
            if (end == lines.size()) Fail("unterminated function");
            auto rewritten = RewriteFunction(lines, i, end, module, contextId, used, report);
            result.insert(result.end(), rewritten.begin(), rewritten.end());
            i = end;
        }
        if (report.ops == 0) Fail("no NVAPI LSS op");
        bool groups[3]{};
        for (const auto& d : kDeclarations)
            if (used.count(d.name) && !module.declared.count(d.name))
            {
                result.push_back(string(d.text) + " #" + std::to_string(990 + d.attributes));
                groups[d.attributes] = true;
            }
        const char* groupText[3] = {"{ nounwind }", "{ nounwind readnone }", "{ nounwind readonly }"};
        for (int g = 0; g < 3; ++g)
            if (groups[g]) result.push_back("attributes #" + std::to_string(990 + g) + " = " + groupText[g]);
        for (const auto& line : result)
        {
            out += line;
            out += '\n';
        }
        return true;
    }
    catch (const std::exception& failure)
    {
        out.clear();
        error = failure.what();
        return false;
    }
}
} // namespace rtx_hair
