#include "shaders.h"
#include "rounded_normal_ir.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
namespace witcher_dots {
namespace {
using RX=std::regex;
RX Pattern(const std::string& p) { return RX(p,std::regex_constants::ECMAScript); }
struct Located { size_t at{}, length{}; };
std::vector<Located> Lines(const std::string& s,const RX& pattern) {
    std::vector<Located> matches;
    size_t at=0;
    while(at<s.size()) {
        const auto end=s.find('\n',at);
        const auto n=(end==std::string::npos?s.size():end)-at;
        const auto line=s.substr(at,n);std::smatch m;
        if(std::regex_search(line,m,pattern)) matches.push_back({at+static_cast<size_t>(m.position()),static_cast<size_t>(m.length())});
        if(end==std::string::npos)break;at=end+1;
    }
    return matches;
}
std::string One(std::string s,const std::string& pattern,std::string_view replacement) {
    const auto span=pattern.find("[\\s\\S]*?");
    auto matches=Lines(s,Pattern(pattern.substr(0,span)));
    if(matches.size()!=1)throw std::runtime_error("shader profile marker mismatch: "+pattern);
    auto m=matches.front();
    if(span!=std::string::npos) {
        const auto tail=pattern.find("(?=",span);
        if(tail==std::string::npos||pattern.back()!=')')throw std::runtime_error("invalid shader range");
        const auto ends=Lines(s,Pattern(pattern.substr(tail+3,pattern.size()-tail-4)));
        if(ends.size()!=1||ends[0].at<=m.at)throw std::runtime_error("shader range mismatch");
        m.length=ends[0].at-m.at;
    }
    s.replace(m.at,m.length,replacement);
    return s;
}
std::string Match(const std::string& s,const std::string& pattern) {
    const auto matches=Lines(s,Pattern(pattern));
    if(matches.size()!=1)throw std::runtime_error("missing or ambiguous shader profile marker");
    return s.substr(matches[0].at,matches[0].length);
}
void ReplaceLiteral(std::string& s,std::string_view from,std::string_view to) {
    size_t at=0;
    while ((at=s.find(from,at))!=std::string::npos) { s.replace(at,from.size(),to);at+=to.size(); }
}
std::string Named(std::string s) {
    std::set<std::string> blocks{"0"};
    RX labels(R"(; <label>:(\d+))");
    for(auto it=std::sregex_iterator(s.begin(),s.end(),labels);it!=std::sregex_iterator();++it) blocks.insert((*it)[1]);
    RX refs(R"(%(\d+)\b)");
    std::string out;size_t pos=0;
    for(auto it=std::sregex_iterator(s.begin(),s.end(),refs);it!=std::sregex_iterator();++it) {
        const auto m=*it;out.append(s,pos,static_cast<size_t>(m.position())-pos);
        out+=(blocks.contains(m[1])?"%bb":"%r")+m[1].str();pos=static_cast<size_t>(m.position()+m.length());
    }
    out.append(s,pos,std::string::npos);
    std::istringstream lines(out);std::string line,result;
    while(std::getline(lines,line)) {
        line=std::regex_replace(line,Pattern(R"(^; <label>:(\d+)(.*)$)"),"bb$1:$2");
        line=std::regex_replace(line,Pattern(R"(^(define .* \{)$)"),"$1\nbb0:");
        result+=line+"\n";
    }
    return result;
}
void Prune(std::string& s) {
    std::istringstream input(s);std::string line,out;
    const RX symbol(R"((@[^ (]+)\()");
    while(std::getline(input,line)) {
        if (line.starts_with("declare ")) {
            std::smatch m;
            if (std::regex_search(line,m,symbol)) {
                const std::string needle=m[1].str()+"(";
                const auto at=s.find(needle);
                if (at!=std::string::npos&&s.find(needle,at+needle.size())==std::string::npos) continue;
            }
        }
        out+=line+"\n";
    }
    s=std::move(out);
}
// The anchors of every known build. Build 25575366 (5.0.0.1041720): the
// original LSS shaders. Build 25646871 (5.0.0.1044392): the prepass reads the
// strand u from committed barycentrics x instead of NVIDIA-extension calls,
// and both shaders compute the object-space normal without the old fallbacks.
constexpr ClosestAnchors kClosest25575366{"r9","r11","r12","r113","r150","r21",{121,125,129,133,137,141,145,149},{150,151,152,154,155,156},
    "27","28",{283,284,285},"r286"};
constexpr PrepassAnchors kPrepass25575366{"r211","r217","r220","r231","r238","r239","r251","r250",
    "r302","r314","r314","r313","r315","r348","r440","r480",{451,455,459,463,467,471,475,479},{501,502,503,504,505,506},
    "53","54",{632,633,634},"r635"};
constexpr ClosestAnchors kClosest25646871{"r9","r11","r12","r113","r150","r21",{121,125,129,133,137,141,145,149},{150,151,152,154,155,156},
    "27","28",{249,250,251},"r252"};
constexpr PrepassAnchors kPrepass25646871{"r211","r217","r220","r231","r238","r239","r240","r239",
    "r289","r290","r290","r289","r291","r324","r416","r456",{427,431,435,439,443,447,451,455},{468,469,470,471,472,473},
    "53","54",{562,563,564},"r565"};
// Issue 70 hit-distance anchors (build 25646871 only).
constexpr ClosestShellAnchors kClosestShell25646871{"r401"};
constexpr PrepassShellAnchors kPrepassShell25646871{"r282",{198,199,200},{133,134,135},{283,284,285},{286,287,288},
    {477,478,479},{537,538,539},{540,541,542}};
// Hair translucency sites (issue 70). Path tracer library (original value
// numbering: its two functions number their values separately, so the named
// form, which collects block labels over the whole library, is not used). Library
// 63828ae5... (PTRGSWithSERFinal + PTRGSFinal; builds 25646871 and 25773555):
// in each function the bounce leaves the current vertex (loop flag %r944 /
// %r942), the other rays leave the bounce hit (payload word %r5893 / %r5844);
// %r911 / %r909 is the loop's random state. The SER bounce is the NVIDIA
// HitObject TraceRay (mask -1). Left alone: the pre-loop rays (stencil 131
// only) and the HitObject invoke marker (flags 0, mask 0).
constexpr std::array<TranslucencySite,12> kPathTracerSites{{
    {"5857","528","-1","-129",HairOrigin::LoopFlag,"944","911",nullptr,HairColour::Luminance,"dots.hl"},
    {"7759","%7757","255","127",HairOrigin::PayloadBit,"5893","911",nullptr,HairColour::Luminance,"dots.hh"},
    {"10451","12","255","127",HairOrigin::PayloadBit,"5893","911",nullptr,HairColour::Luminance,"dots.hh"},
    {"12672","28","255","127",HairOrigin::PayloadBit,"5893","911",nullptr,HairColour::Luminance,"dots.hh"},
    {"13233","28","255","127",HairOrigin::PayloadBit,"5893","911",nullptr,HairColour::Luminance,"dots.hh"},
    {"15183","28","255","127",HairOrigin::PayloadBit,"5893","911",nullptr,HairColour::Luminance,"dots.hh"},
    {"5840","528","255","127",HairOrigin::LoopFlag,"942","909",nullptr,HairColour::Luminance,"dots.hl"},
    {"7710","%7708","255","127",HairOrigin::PayloadBit,"5844","909",nullptr,HairColour::Luminance,"dots.hh"},
    {"10402","12","255","127",HairOrigin::PayloadBit,"5844","909",nullptr,HairColour::Luminance,"dots.hh"},
    {"12623","28","255","127",HairOrigin::PayloadBit,"5844","909",nullptr,HairColour::Luminance,"dots.hh"},
    {"13184","28","255","127",HairOrigin::PayloadBit,"5844","909",nullptr,HairColour::Luminance,"dots.hh"},
    {"15116","28","255","127",HairOrigin::PayloadBit,"5844","909",nullptr,HairColour::Luminance,"dots.hh"},
}};
// Colour-aware loops (dominator-checked on the game DXC disassembly): the pixel
// colour dominates the preheader's branch, the payload the latch's branch. The
// pixel colour is the hair albedo, GBuffer0 after the game's pow 2.2 decode
// (not GBuffer2, the specular colour).
constexpr std::array<TranslucencyLoop,2> kPathTracerLoops{{
    {"PTRGSWithSERFinal","910","902","15468","662,663,664","5892"},
    {"PTRGSFinal","908","900","15401","660,661,662","5843"},
}};
constexpr TranslucencyAnchors kPathTracerTranslucency{kPathTracerSites.data(),kPathTracerSites.size(),kPathTracerLoops.data(),kPathTracerLoops.size()};
// The hair prepass (build 25646871 shader): its three rays all leave the hair
// hit; %r77 is its per-pixel, per-frame seed. Colour: the hair albedo after
// the prepass's own pow 2.2 decode, %r635-%r637 (dominates the three rays).
constexpr std::array<TranslucencySite,3> kPrepassSites25646871{{
    {"r964","%r962","255","127",HairOrigin::Always,nullptr,"r77",nullptr,HairColour::Linear,"r635,r636,r637"},
    {"r3619","12","255","127",HairOrigin::Always,nullptr,"r77",nullptr,HairColour::Linear,"r635,r636,r637"},
    {"r5945","12","255","127",HairOrigin::Always,nullptr,"r77",nullptr,HairColour::Linear,"r635,r636,r637"},
}};
constexpr TranslucencyAnchors kPrepassTranslucency25646871{kPrepassSites25646871.data(),kPrepassSites25646871.size()};
// The sun shadow pass (ShadowsRGS d321458d...) and the local-light shadow pass
// (LocalShadowsRGS da2e0e8f...), original numbering: one TraceRay each from
// the pixel, hair by its material word (%478 / %139). Seeds: the frame value
// CameraCB[13].w and the pixel (Morton code %88; dispatch index %14/%15; the
// local pass loads CameraCB itself: its own load does not dominate the ray).
// Neither binds the hair albedo (GBuffer2 holds the specular colour), so their
// coins are colour-blind.
constexpr std::array<TranslucencySite,1> kShadowSites{{
    {"615","528","255","127",HairOrigin::MaterialWord,"478","$.seed","  $.fz = mul i32 %62, 19349663\n  $.seed = xor i32 %88, $.fz\n",
        HairColour::None,nullptr},
}};
constexpr TranslucencyAnchors kShadowTranslucency{kShadowSites.data(),kShadowSites.size()};
constexpr std::array<TranslucencySite,1> kLocalShadowSites{{
    {"806","%804","255","127",HairOrigin::MaterialWord,"139","$.seed",
        "  $.cb = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %13, i32 13)\n"
        "  $.frame = extractvalue %dx.types.CBufRet.i32 $.cb, 3\n  $.fy = mul i32 %15, 73856093\n  $.fx = xor i32 %14, $.fy\n"
        "  $.fz = mul i32 $.frame, 19349663\n  $.seed = xor i32 $.fx, $.fz\n",HairColour::None,nullptr},
}};
constexpr TranslucencyAnchors kLocalShadowTranslucency{kLocalShadowSites.data(),kLocalShadowSites.size()};
// HairBounces (issue 70): the loop trip count, select(hair pixel, 4, 2), in
// both path tracer functions (game-DXC text, original numbering).
constexpr std::array<BounceSite,2> kPathTracerBounceSites{{
    {"PTRGSWithSERFinal","  %909 = select i1 %139, i32 4, i32 2"},
    {"PTRGSFinal","  %907 = select i1 %137, i32 4, i32 2"},
}};
constexpr BounceAnchors kPathTracerBounces{kPathTracerBounceSites.data(),kPathTracerBounceSites.size()};
// HairAlbedoLinear (issue 70): the colour interpolated from HairAlbedoBuffer
// (build 25646871 shaders, named form).
constexpr AlbedoAnchors kClosestAlbedo25646871{{"r56","r57","r58"}};
constexpr AlbedoAnchors kPrepassAlbedo25646871{{"r359","r360","r361"}};
// HairIgnoreReflectionMask (issue 70): the path tracer's ReflectionsTexture
// loads at the pixel, in both functions (original numbering): the reflection
// dispatch's gate and the main path's mask (the bounce-hit load is left alone).
constexpr std::array<MaskSite,4> kPathTracerMaskSites{{
    {"PTRGSWithSERFinal","  %191 = call float @dx.op.unary.f32(i32 7, float %190)","%190","%139"},
    {"PTRGSWithSERFinal","  %592 = call float @dx.op.unary.f32(i32 7, float %591)","%591","%139"},
    {"PTRGSFinal","  %189 = call float @dx.op.unary.f32(i32 7, float %188)","%188","%137"},
    {"PTRGSFinal","  %590 = call float @dx.op.unary.f32(i32 7, float %589)","%589","%137"},
}};
constexpr MaskAnchors kPathTracerMasks{kPathTracerMaskSites.data(),kPathTracerMaskSites.size()};
// HairDebugLighting (issue 70, diagnostic), prepass build 25646871 (named form):
// 1 local-light term off, 2 sun term off, 4 all-lights loop forced, 5 one random
// light (RIS) forced. Mode 3 zeroes the path tracer's stores on hair pixels.
constexpr std::array<DebugLine,9> kPrepassDebug25646871{{
    {1,"  %r7755 = fmul fast float %r7754, %r7752","  %r7755 = fmul fast float %r7754, 0.000000e+00"},
    {2,"  %r7759 = fadd fast float %r7756, %r2945","  %r7759 = fadd fast float %r7756, 0.000000e+00"},
    {2,"  %r7760 = fadd fast float %r7757, %r2946","  %r7760 = fadd fast float %r7757, 0.000000e+00"},
    {2,"  %r7761 = fadd fast float %r7758, %r2947","  %r7761 = fadd fast float %r7758, 0.000000e+00"},
    {2,"  %r7765 = fadd fast float %r7762, %r2948","  %r7765 = fadd fast float %r7762, 0.000000e+00"},
    {2,"  %r7766 = fadd fast float %r7763, %r2949","  %r7766 = fadd fast float %r7763, 0.000000e+00"},
    {2,"  %r7767 = fadd fast float %r7764, %r2950","  %r7767 = fadd fast float %r7764, 0.000000e+00"},
    {4,"  %r2957 = icmp eq i32 %r2956, 0","  %r2957 = icmp eq i32 %r2956, %r2956"},
    {5,"  %r2957 = icmp eq i32 %r2956, 0","  %r2957 = icmp ne i32 %r2956, %r2956"},
}};
constexpr DebugAnchors kPrepassDebug{kPrepassDebug25646871.data(),kPrepassDebug25646871.size()};
constexpr std::array<HairFlagSite,2> kPathTracerHairFlagSites{{{"PTRGSWithSERFinal","%139"},{"PTRGSFinal","%137"}}};
constexpr HairFlagAnchors kPathTracerHairFlags{kPathTracerHairFlagSites.data(),kPathTracerHairFlagSites.size()};
// HairStencilCharacter (issue 70): the bit-2 test of the path tracer's
// StencilMapTexture word at the pixel, in both functions (original numbering).
// Bit 2 picks the per-light scale and the interior-weighted multiplier.
constexpr std::array<StencilSite,2> kPathTracerStencilSites{{
    {"PTRGSWithSERFinal","  %678 = and i32 %675, 2","%675","%139"},
    {"PTRGSFinal","  %676 = and i32 %673, 2","%673","%137"},
}};
constexpr StencilAnchors kPathTracerStencil{kPathTracerStencilSites.data(),kPathTracerStencilSites.size()};
constexpr std::array<ShaderIdentity,7> kShaders{{
    {ShaderKind::ClosestHit,8240,"4f2063aca18fdac330cdf5d54d52dfd22b620fc50f84184ef3adaefbcc206e3f",&kClosest25575366,nullptr},
    {ShaderKind::Prepass,63072,"3406beddeabcebcc372e10df365eac5ee03b7aa6e99aa4b1f5d4d252aae7ea6e",nullptr,&kPrepass25575366},
    {ShaderKind::ClosestHit,7804,"d6bdd62b710a5566db95e4277a7ba94a454ba04f15c65bcbff1c8c81974b4436",&kClosest25646871,nullptr,&kClosestShell25646871,nullptr,
        nullptr,nullptr,nullptr,&kClosestAlbedo25646871},
    {ShaderKind::Prepass,62232,"736d1986356e2bf49c38fea948bbc38101a6fc67cd9c96d9c9380fd51de30acf",nullptr,&kPrepass25646871,nullptr,&kPrepassShell25646871,
        &kPrepassTranslucency25646871,nullptr,nullptr,&kPrepassAlbedo25646871,nullptr,&kPrepassDebug},
    {ShaderKind::PathTracer,231144,"63828ae55a44ce1ab041065ba4e3169e0b33c277943b19545be7a810ae048121",nullptr,nullptr,nullptr,nullptr,
        &kPathTracerTranslucency,"path tracer",&kPathTracerBounces,nullptr,&kPathTracerMasks,nullptr,&kPathTracerHairFlags,&kPathTracerStencil},
    {ShaderKind::PathTracer,6960,"d321458d078bb91475fd5cba7d570f537096a0a411ca8ec9e44f4659d9a8184c",nullptr,nullptr,nullptr,nullptr,
        &kShadowTranslucency,"sun shadows"},
    {ShaderKind::PathTracer,10616,"da2e0e8f5c5ffcdc89dab5aaa45040bb754f36d02a05604e1a066840a75fcced",nullptr,nullptr,nullptr,nullptr,
        &kLocalShadowTranslucency,"local-light shadows"},
}};
// Renames %rN and %dots.flat.rN tokens of an inserted snippet (written in
// build 25575366 names) in one pass, so mapped names never chain.
std::string Renamed(std::string_view ir,const std::vector<std::pair<int,int>>& names) {
    const RX token(R"(%(dots\.flat\.)?r(\d+)\b)");
    std::string text(ir),out;size_t pos=0;
    for(auto it=std::sregex_iterator(text.begin(),text.end(),token);it!=std::sregex_iterator();++it) {
        const auto& m=*it;out.append(text,pos,static_cast<size_t>(m.position())-pos);
        int id=std::stoi(m[2].str());
        for(const auto& [from,to]:names)if(from==id) {id=to;break;}
        out+="%"+m[1].str()+"r"+std::to_string(id);pos=static_cast<size_t>(m.position()+m.length());
    }
    out.append(text,pos,std::string::npos);
    return out;
}
std::string Endpoints(bool pre,std::string global,const std::string& inst,const std::string& index,const std::string& non,const std::string& alias,
    const std::array<int,8>& ids) {
    std::string code=R"(  %dots.ptr = getelementptr inbounds [32 x %dx.types.Handle], [32 x %dx.types.Handle]* GLOBAL, i32 0, i32 INST, !dx.nonuniform !NON
  %dots.load = load %dx.types.Handle, %dx.types.Handle* %dots.ptr, align 4, !noalias !ALIAS
  %dots.handle0 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %dots.load)
  %dots.handle = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %dots.handle0, %dx.types.ResourceProperties { i32 12, i32 16 })
  %dots.p0 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %dots.handle, i32 INDEX, i32 0, i8 15, i32 4)
  %dots.next = add i32 INDEX, 1
  %dots.p1 = call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %dots.handle, i32 %dots.next, i32 0, i8 15, i32 4)
)";
    ReplaceLiteral(code,"GLOBAL",global);ReplaceLiteral(code,"INST",inst);
    ReplaceLiteral(code,"INDEX",index);ReplaceLiteral(code,"NON",non);
    ReplaceLiteral(code,"ALIAS",alias);
    if(!pre) code="  %dots.inst = call i32 @dx.op.instanceID.i32(i32 141)\n"+code;
    for(size_t i=0;i<ids.size();++i)
        code+="  %r"+std::to_string(ids[i])+" = extractvalue %dx.types.ResRet.f32 %dots.p"+(i<4?"0":"1")+", "+std::to_string(i%4)+"\n";
    return code;
}
std::string QueryU(const char* query,const char* prim,const char* out,const char* tag) {
    std::string code=R"(  %TAG.bx = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %QUERY, i8 0)
  %TAG.by = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %QUERY, i8 1)
  %TAG.sum = fadd float %TAG.bx, %TAG.by
  %TAG.parity = and i32 %PRIM, 1
  %TAG.even = icmp eq i32 %TAG.parity, 0
  %OUT = select i1 %TAG.even, float %TAG.sum, float %TAG.by
)";
    ReplaceLiteral(code,"TAG",tag);ReplaceLiteral(code,"PRIM",prim);ReplaceLiteral(code,"OUT",out);ReplaceLiteral(code,"QUERY",query);
    return code;
}
void Normals(std::string& s,const std::array<int,3>& outputs,const char* marker,std::string_view rounded) {
    for(int id:outputs) {
        const auto name="%r"+std::to_string(id);
        auto phi=Match(s,"^  "+name+" = phi float.*$");
        ReplaceLiteral(phi,name+" =","%dots.flat.r"+std::to_string(id)+" =");
        s=One(std::move(s),"^  "+name+" = phi float.*$",phi);
    }
    const std::string pattern="^  %"+std::string(marker)+" =.*$";
    const std::string original=Match(s,pattern);
    s=One(std::move(s),pattern,std::string(rounded)+original);
}
std::vector<std::pair<int,int>> NameMap(const std::array<int,8>& fromExtracts,const std::array<int,8>& toExtracts,
    const std::array<int,6>& fromRay,const std::array<int,6>& toRay,const std::array<int,3>& fromNormals,const std::array<int,3>& toNormals) {
    std::vector<std::pair<int,int>> names;
    for(size_t i=0;i<8;++i)names.push_back({fromExtracts[i],toExtracts[i]});
    for(size_t i=0;i<6;++i)names.push_back({fromRay[i],toRay[i]});
    for(size_t i=0;i<3;++i)names.push_back({fromNormals[i],toNormals[i]});
    return names;
}
std::string N(const char* name) {return std::string("%")+name;}
// Issue 70: 1.12f, the round strand that encloses both crossed ribbons
// (half-width pi/(2*sqrt(2)) r = 1.1107 r) for the round-strand hit distance.
constexpr const char* kShellScale="0x3FF1EB8520000000";
std::atomic<bool> roundHitDistance{true};
// The rounded-normal snippet (build 25575366 names) on the enclosing round
// strand: radii scaled first, so %dots.n82 (the ray parameter of the hit, the
// same in object and world space) and the normal belong to that strand.
std::string Shell(std::string_view snippet,const char* ra,const char* rb) {
    std::string s(snippet);
    s=std::regex_replace(s,Pattern(std::string(ra)+"\\b"),"%dots.ra");
    s=std::regex_replace(s,Pattern(std::string(rb)+"\\b"),"%dots.rb");
    return "  %dots.ra = fmul float "+std::string(ra)+", "+kShellScale+"\n  %dots.rb = fmul float "+std::string(rb)+", "+kShellScale+"\n"+s;
}
std::string Rn(int id) {return "%r"+std::to_string(id);}
std::string Fix(int id) {return "%dots.fix.r"+std::to_string(id);}
// After the prepass rounded normal (build names): the corrected hit distance
// and every value the game derived from CommittedRayT before that point.
std::string PrepassShellTail(const PrepassShellAnchors& a,const std::array<int,6>& ray) {
    std::string code="  %dots.t = select i1 %dots.n88, float %dots.n82, float %"+std::string(a.hitT)+"\n";
    for(size_t i=0;i<3;++i) {
        const auto c=std::to_string(i);
        code+="  "+Fix(a.worldOffset[i])+" = fmul float "+Rn(a.worldDir[i])+", %dots.t\n";
        code+="  "+Fix(a.worldPos[i])+" = fadd float "+Rn(a.camera[i])+", "+Fix(a.worldOffset[i])+"\n";
        code+="  %dots.oh"+c+" = fmul float %dots.t, "+Rn(ray[3+i])+"\n";
        code+="  "+Fix(a.objectHit[i])+" = fadd float "+Rn(ray[i])+", %dots.oh"+c+"\n";
        code+="  "+Fix(a.objectOffset[i])+" = fsub float "+Fix(a.objectHit[i])+", "+Rn(a.objectBase[i])+"\n";
    }
    return code;
}
std::vector<std::pair<std::string,std::string>> PrepassShellNames(const PrepassShellAnchors& a) {
    std::vector<std::pair<std::string,std::string>> names{{"%"+std::string(a.hitT),"%dots.t"}};
    for(const auto* group:{&a.worldOffset,&a.worldPos,&a.objectHit,&a.objectOffset})
        for(int id:*group)names.push_back({Rn(id),Fix(id)});
    return names;
}
// Renames whole %rN tokens after the marker line. Every later use of these
// values is dominated by the marker block (checked per anchored build).
void RenameAfter(std::string& s,const std::string& marker,const std::vector<std::pair<std::string,std::string>>& names) {
    const auto line=Lines(s,Pattern(marker));
    if(line.size()!=1)throw std::runtime_error("hit distance marker mismatch");
    const auto from=s.find('\n',line[0].at);
    if(from==std::string::npos)throw std::runtime_error("hit distance marker at end");
    std::string tail=s.substr(from);
    const RX token(R"(%r\d+\b)");
    std::string out;size_t pos=0;size_t renamed=0;
    for(auto it=std::sregex_iterator(tail.begin(),tail.end(),token);it!=std::sregex_iterator();++it) {
        const auto& m=*it;out.append(tail,pos,static_cast<size_t>(m.position())-pos);
        std::string text=m.str();
        for(const auto& [old,now]:names)if(text==old) {text=now;++renamed;break;}
        out+=text;pos=static_cast<size_t>(m.position()+m.length());
    }
    out.append(tail,pos,std::string::npos);
    if(!renamed)throw std::runtime_error("hit distance uses missing");
    s.replace(from,std::string::npos,out);
}
std::atomic<uint32_t> hairTranslucency{0};
std::atomic<bool> hairByColour{true};
std::atomic<uint32_t> hairWhitePoint{35};
std::atomic<uint32_t> hairBounces{4};
std::atomic<bool> hairAlbedoLinear{false};
std::atomic<bool> hairIgnoreReflectionMask{false};
std::atomic<uint32_t> hairDebugLighting{0};
std::atomic<bool> hairStencilCharacter{false};
// A float constant in LLVM IR's exact hexadecimal (double) form.
std::string F(float value) {
    const double d=value;uint64_t bits{};std::memcpy(&bits,&d,sizeof(bits));
    char text[24]{};sprintf_s(text,"0x%016llX",static_cast<unsigned long long>(bits));return text;
}
// "a,b,c" -> {"a","b","c"}.
std::array<std::string,3> Three(const char* list) {
    std::array<std::string,3> out;std::string s(list?list:"");size_t at=0;
    for(auto& part:out) {const size_t comma=s.find(',',at);part=s.substr(at,comma==std::string::npos?std::string::npos:comma-at);at=comma==std::string::npos?s.size():comma+1;}
    if(out[0].empty()||out[1].empty()||out[2].empty())throw std::runtime_error("hair colour value list");
    return out;
}
// Rec. 709 luminance of three linear values into `name` (IR lines).
std::string Luminance(const std::string& name,const std::array<std::string,3>& linear) {
    return "  "+name+".lr = fmul float "+linear[0]+", "+F(0.2126f)+"\n  "+name+".lg = fmul float "+linear[1]+", "+F(0.7152f)+"\n"
        "  "+name+".lb = fmul float "+linear[2]+", "+F(0.0722f)+"\n  "+name+".lrg = fadd float "+name+".lr, "+name+".lg\n"
        "  "+name+" = fadd float "+name+".lrg, "+name+".lb\n";
}
// Colour-aware translucency in one path tracer function: %dots.hp after the
// pixel colour, %dots.hh after the bounce payload load and the header phi
// %dots.hl. Anchors are searched inside the function only (value numbers
// repeat between functions) and must each match once; inserted from the last
// position back, so earlier positions stay valid.
void Loop(std::string& s,const TranslucencyLoop& loop) {
    const std::string define=std::string("define void @\"\\01?")+loop.function+"@@YAXXZ\"()";
    const size_t begin=s.find(define);
    if(begin==std::string::npos||s.find(define,begin+1)!=std::string::npos)throw std::runtime_error("hair colour function mismatch");
    const size_t end=s.find("\n}\n",begin);
    if(end==std::string::npos)throw std::runtime_error("hair colour function end");
    const auto once=[&](const std::string& needle) {
        const size_t at=s.find(needle,begin);
        if(at==std::string::npos||at>=end)throw std::runtime_error("hair colour anchor missing: "+needle);
        const size_t next=s.find(needle,at+1);
        if(next!=std::string::npos&&next<end)throw std::runtime_error("hair colour anchor ambiguous: "+needle);
        return at;
    };
    const auto lineEnd=[&](size_t at) {const size_t e=s.find('\n',at);if(e==std::string::npos)throw std::runtime_error("hair colour line");return e+1;};
    const auto pixel=Three(loop.pixel);
    size_t pixelAt=0;
    for(const auto& value:pixel)pixelAt=std::max(pixelAt,lineEnd(once("\n  %"+value+" = ")+1));
    const size_t headerAt=lineEnd(once("\n; <label>:"+std::string(loop.header)+" ")+1);
    const size_t payloadAt=lineEnd(once("\n  %"+std::string(loop.payload)+" = load <4 x i32>, ")+1);
    if(!(pixelAt<headerAt&&headerAt<payloadAt))throw std::runtime_error("hair colour anchor order");
    const std::string w="%dots.hh";
    std::string hit="  "+w+".w = extractelement <4 x i32> %"+std::string(loop.payload)+", i32 0\n";
    std::array<std::string,3> linear;
    for(int c=0;c<3;++c) {
        const std::string ch=w+".c"+std::to_string(c);
        if(c)hit+="  "+ch+".s = lshr i32 "+w+".w, "+std::to_string(8*c)+"\n";
        hit+="  "+ch+".i = and i32 "+(c?ch+".s":w+".w")+", 255\n  "+ch+".f = uitofp i32 "+ch+".i to float\n"
            "  "+ch+".n = fmul float "+ch+".f, "+F(1.0f/255.0f)+"\n  "+ch+".l = fmul float "+ch+".n, "+ch+".n\n";
        linear[c]=ch+".l";
    }
    hit+=Luminance(w,linear);
    s.insert(payloadAt,hit);
    s.insert(headerAt,"  %dots.hl = phi float [ %dots.hp, %"+std::string(loop.preheader)+" ], [ %dots.hh, %"+std::string(loop.latch)+" ]\n");
    s.insert(pixelAt,Luminance("%dots.hp",{"%"+pixel[0],"%"+pixel[1],"%"+pixel[2]}));
}
// HairBounces: each path tracer function's loop trip count line, searched
// inside that function only (value numbers repeat between functions).
void Bounces(std::string& s,const BounceAnchors& anchors,uint32_t bounces) {
    for(size_t i=0;i<anchors.count;++i) {
        const auto& site=anchors.sites[i];
        const std::string define=std::string("define void @\"\\01?")+site.function+"@@YAXXZ\"()";
        const size_t begin=s.find(define);
        if(begin==std::string::npos||s.find(define,begin+1)!=std::string::npos)throw std::runtime_error("hair bounces function mismatch");
        const size_t end=s.find("\n}\n",begin);
        const std::string line=std::string("\n")+site.line+"\n";
        const size_t at=s.find(line,begin);
        if(end==std::string::npos||at==std::string::npos||at>=end)throw std::runtime_error("hair bounces anchor missing");
        const size_t next=s.find(line,at+1);
        if(next!=std::string::npos&&next<end)throw std::runtime_error("hair bounces anchor ambiguous");
        std::string replaced(site.line);
        const size_t count=replaced.find("i32 4, i32 2");
        if(count==std::string::npos)throw std::runtime_error("hair bounces anchor form");
        replaced.replace(count,12,"i32 "+std::to_string(bounces)+", i32 2");
        s.replace(at+1,line.size()-2,replaced);
    }
}
// HairDebugLighting (diagnostic): each prepass line of the current mode is
// replaced exactly once.
void DebugLines(std::string& s,const DebugAnchors& anchors,uint32_t mode) {
    for(size_t i=0;i<anchors.count;++i) {
        const auto& line=anchors.lines[i];
        if(line.mode!=mode)continue;
        const std::string from=std::string("\n")+line.from+"\n";
        const size_t at=s.find(from);
        if(at==std::string::npos||s.find(from,at+1)!=std::string::npos)throw std::runtime_error("hair debug lighting anchor");
        s.replace(at+1,from.size()-2,line.to);
    }
}
// HairDebugLighting=3 (diagnostic): every RTGBufferA/B store after the hair
// flag (it dominates them all, switch edges included) writes a zero colour for
// hair pixels; .w (the hit distance) is kept.
void ZeroHairStores(std::string& s,const HairFlagAnchors& anchors) {
    const RX store(R"(^(  call void @dx\.op\.textureStore\.f32\(i32 67, %dx\.types\.Handle %[\w.]+, i32 [^,]+, i32 [^,]+, i32 undef, )float ([^,]+), float ([^,]+), float ([^,]+), (float [^,]+, i8 15\).*)$)");
    size_t total=0;
    for(size_t f=0;f<anchors.count;++f) {
        const auto& site=anchors.sites[f];
        const std::string define=std::string("define void @\"\\01?")+site.function+"@@YAXXZ\"()";
        const size_t begin=s.find(define);
        if(begin==std::string::npos||s.find(define,begin+1)!=std::string::npos)throw std::runtime_error("hair debug store function mismatch");
        const size_t flag=s.find(std::string("\n  ")+site.hair+" = ",begin);
        size_t end=s.find("\n}\n",begin);
        if(flag==std::string::npos||end==std::string::npos||flag>end)throw std::runtime_error("hair debug store flag");
        size_t at=s.find('\n',flag+1),count=0;
        while(at!=std::string::npos&&at<end) {
            const size_t lineEnd=s.find('\n',at+1);
            if(lineEnd==std::string::npos)break;
            const std::string line=s.substr(at+1,lineEnd-at-1);
            std::smatch m;
            if(std::regex_match(line,m,store)) {
                const std::string v="%dots.pz"+std::to_string(total++);
                std::string code;
                for(int c=0;c<3;++c)code+="  "+v+"c"+std::to_string(c)+" = select i1 "+site.hair+", float 0.000000e+00, float "+m[2+c].str()+"\n";
                code+=m[1].str()+"float "+v+"c0, float "+v+"c1, float "+v+"c2, "+m[5].str();
                s.replace(at+1,line.size(),code);
                end+=code.size()-line.size();
                at+=1+code.size();
                ++count;
                continue;
            }
            at=lineEnd;
        }
        if(!count)throw std::runtime_error("hair debug store missing");
    }
}
// HairStencilCharacter: bit 2 is ORed into the stencil word for hair pixels
// right before the bit-2 test; searched inside each function (value numbers
// repeat). Later uses of the word (bit 64, the exact compare) are unchanged.
void StencilCharacter(std::string& s,const StencilAnchors& anchors) {
    for(size_t i=0;i<anchors.count;++i) {
        const auto& site=anchors.sites[i];
        const std::string define=std::string("define void @\"\\01?")+site.function+"@@YAXXZ\"()";
        const size_t begin=s.find(define);
        if(begin==std::string::npos||s.find(define,begin+1)!=std::string::npos)throw std::runtime_error("hair stencil function mismatch");
        const size_t end=s.find("\n}\n",begin);
        const std::string line=std::string("\n")+site.line;
        const size_t at=s.find(line,begin);
        if(end==std::string::npos||at==std::string::npos||at>=end)throw std::runtime_error("hair stencil anchor missing");
        const size_t next=s.find(line,at+1);
        if(next!=std::string::npos&&next<end)throw std::runtime_error("hair stencil anchor ambiguous");
        const std::string name="%dots.sc"+std::to_string(i),operand=std::string("i32 ")+site.word+",";
        const size_t use=s.find(operand,at+1);
        if(use==std::string::npos||use>s.find('\n',at+1))throw std::runtime_error("hair stencil operand");
        s.replace(use,operand.size(),"i32 "+name+",");
        s.insert(at+1,"  "+name+"b = select i1 "+site.hair+", i32 2, i32 0\n  "+name+" = or i32 "+site.word+", "+name+"b\n");
    }
}
// HairIgnoreReflectionMask: the loaded mask becomes select(hair, 0, mask) right
// before its first use; searched inside each function (value numbers repeat).
void Masks(std::string& s,const MaskAnchors& anchors) {
    for(size_t i=0;i<anchors.count;++i) {
        const auto& site=anchors.sites[i];
        const std::string define=std::string("define void @\"\\01?")+site.function+"@@YAXXZ\"()";
        const size_t begin=s.find(define);
        if(begin==std::string::npos||s.find(define,begin+1)!=std::string::npos)throw std::runtime_error("hair reflection mask function mismatch");
        const size_t end=s.find("\n}\n",begin);
        const std::string line=std::string("\n")+site.line;
        const size_t at=s.find(line,begin);
        if(end==std::string::npos||at==std::string::npos||at>=end)throw std::runtime_error("hair reflection mask anchor missing");
        const size_t next=s.find(line,at+1);
        if(next!=std::string::npos&&next<end)throw std::runtime_error("hair reflection mask anchor ambiguous");
        const std::string name="%dots.rm"+std::to_string(i),operand=std::string("float ")+site.operand+")";
        const size_t use=s.find(operand,at+1);
        if(use==std::string::npos||use>s.find('\n',at+1))throw std::runtime_error("hair reflection mask operand");
        s.replace(use,operand.size(),"float "+name+")");
        s.insert(at+1,"  "+name+" = select i1 "+site.hair+", float 0.000000e+00, float "+site.operand+"\n");
    }
}
// HairAlbedoLinear: pow(saturate(c), 1/2.2) of each channel right after the
// colour is formed; every later use of the channel takes the encoded value, so
// the game's own pow 2.2 decodes (prepass lighting, GBuffer0 in the path
// tracer, the hit payload) return the linear colour.
void AlbedoGamma(std::string& s,const AlbedoAnchors& a) {
    std::vector<std::pair<std::string,std::string>> names;std::string code;
    for(int c=0;c<3;++c) {
        const std::string v=N(a.values[c]),g="%dots.ga"+std::to_string(c);
        code+="\n  "+g+".sat = call float @dx.op.unary.f32(i32 7, float "+v+")\n"
            "  "+g+".log = call float @dx.op.unary.f32(i32 23, float "+g+".sat)\n"
            "  "+g+".exp = fmul float "+g+".log, "+F(1.0f/2.2f)+"\n"
            "  "+g+" = call float @dx.op.unary.f32(i32 21, float "+g+".exp)";
        names.push_back({v,g});
    }
    const std::string marker="^  "+N(a.values[2])+" = .*$";
    RenameAfter(s,marker,names);
    s=One(std::move(s),marker,Match(s,marker)+code);
}
// Issue 70 hair translucency: before each anchored TraceRay, a coin from the
// shader's own random state (hashed with the site) and the site's hair origin
// select the instance mask without bit 0x80 (hair) with probability percent.
// The anchor is the TraceRay's handle, flags and mask operands (any payload).
void Translucent(std::string& s,const TranslucencyAnchors& anchors,uint32_t percent) {
    const uint32_t threshold=static_cast<uint32_t>((static_cast<uint64_t>(std::min(percent,100u))<<24)/100);
    const bool byColour=HairTranslucencyByColour();
    const float inverseWhite=100.0f/static_cast<float>(std::clamp(HairTranslucencyWhitePoint(),5u,100u));
    for(size_t l=0;byColour&&l<anchors.loopCount;++l)Loop(s,anchors.loops[l]);
    for(size_t i=0;i<anchors.count;++i) {
        const auto& site=anchors.sites[i];
        const std::string v="%dots.ht"+std::to_string(i);
        const auto value=[&](const char* name) {return name[0]=='$'?v+(name+1):N(name);};
        const std::string head="i32 157, %dx.types.Handle "+N(site.handle)+", i32 "+site.flags+", i32 ";
        const std::string call=head+site.mask+", ";
        const size_t at=s.find(call);
        if(at==std::string::npos||s.find(call,at+call.size())!=std::string::npos)throw std::runtime_error("hair translucency site mismatch");
        const size_t line=s.rfind('\n',at)+1;
        if(s.compare(line,30,"  call void @dx.op.traceRay.st")!=0)throw std::runtime_error("hair translucency site is not a TraceRay");
        const int32_t salt=static_cast<int32_t>(0x68E31DA4u+static_cast<uint32_t>(i)*0x9E3779B9u);
        std::string code;
        if(site.seed) {std::string seed(site.seed);ReplaceLiteral(seed,"$",v);code+=seed;}
        code+="  "+v+".s = xor i32 "+value(site.random)+", "+std::to_string(salt)+"\n"
            "  "+v+".a = mul i32 "+v+".s, -1640531535\n  "+v+".b = lshr i32 "+v+".a, 15\n  "+v+".c = xor i32 "+v+".a, "+v+".b\n"
            "  "+v+".d = mul i32 "+v+".c, -2073254261\n  "+v+".e = lshr i32 "+v+".d, 16\n  "+v+".f = xor i32 "+v+".d, "+v+".e\n"
            "  "+v+".u = lshr i32 "+v+".f, 8\n";
        if(byColour&&site.colour!=HairColour::None) {
            // w = min(1, Y / white point); the coin passes when u < threshold * w.
            std::string y;
            if(site.colour==HairColour::Luminance)y=N(site.colourValue);
            else {
                const bool material=site.colour==HairColour::Material;
                const bool listed=material||site.colour==HairColour::Linear;
                const auto values=listed?Three(site.colourValue)
                    :std::array<std::string,3>{site.colourValue,site.colourValue,site.colourValue};
                std::array<std::string,3> linear;
                for(int c=0;c<3;++c) {
                    if(site.colour==HairColour::Linear) {linear[c]=N(values[c].c_str());continue;}
                    const std::string ch=v+".c"+std::to_string(c);
                    if(material) {
                        code+="  "+ch+".pos = fcmp ogt float "+N(values[c].c_str())+", 0.000000e+00\n"
                            "  "+ch+".m = select i1 "+ch+".pos, float "+N(values[c].c_str())+", float 0.000000e+00\n"
                            "  "+ch+".l = fmul float "+ch+".m, "+F(0.0466f)+"\n";
                    } else {
                        code+="  "+ch+".e = extractvalue %dx.types.ResRet.f32 "+N(values[0].c_str())+", "+std::to_string(c)+"\n"
                            "  "+ch+".l = fmul float "+ch+".e, "+ch+".e\n";
                    }
                    linear[c]=ch+".l";
                }
                code+=Luminance(v+".y",linear);y=v+".y";
            }
            code+="  "+v+".t = fmul float "+y+", "+F(inverseWhite)+"\n  "+v+".lt = fcmp olt float "+v+".t, 1.000000e+00\n"
                "  "+v+".w = select i1 "+v+".lt, float "+v+".t, float 1.000000e+00\n  "+v+".uf = uitofp i32 "+v+".u to float\n"
                "  "+v+".thr = fmul float "+v+".w, "+F(static_cast<float>(threshold))+"\n  "+v+".pass = fcmp olt float "+v+".uf, "+v+".thr\n";
        } else {
            code+="  "+v+".pass = icmp ult i32 "+v+".u, "+std::to_string(threshold)+"\n";
        }
        std::string skip=v+".pass";
        if(site.origin==HairOrigin::LoopFlag) {
            code+="  "+v+".hair = icmp ne i32 "+N(site.hair)+", 0\n  "+v+".skip = and i1 "+v+".hair, "+v+".pass\n";skip=v+".skip";
        } else if(site.origin==HairOrigin::PayloadBit) {
            code+="  "+v+".bit = and i32 "+N(site.hair)+", 131072\n  "+v+".hair = icmp ne i32 "+v+".bit, 0\n"
                "  "+v+".skip = and i1 "+v+".hair, "+v+".pass\n";skip=v+".skip";
        } else if(site.origin==HairOrigin::MaterialWord) {
            code+="  "+v+".class = and i32 "+N(site.hair)+", 15\n  "+v+".hair = icmp eq i32 "+v+".class, 11\n"
                "  "+v+".skip = and i1 "+v+".hair, "+v+".pass\n";skip=v+".skip";
        }
        code+="  "+v+".mask = select i1 "+skip+", i32 "+site.masked+", i32 "+site.mask+"\n";
        s.replace(at,call.size(),head+v+".mask, ");
        s.insert(line,code);
    }
}
}
std::span<const ShaderIdentity> KnownShaders() {return kShaders;}
void SetRoundHitDistance(bool enabled) noexcept {roundHitDistance.store(enabled,std::memory_order_release);}
bool RoundHitDistance() noexcept {return roundHitDistance.load(std::memory_order_acquire);}
bool RoundHitDistanceSupported(const ShaderIdentity& shader) noexcept {
    return shader.kind==ShaderKind::ClosestHit?shader.closestShell!=nullptr:shader.prepassShell!=nullptr;
}
void SetHairTranslucency(uint32_t percent) noexcept {hairTranslucency.store(std::min(percent,100u),std::memory_order_release);}
uint32_t HairTranslucency() noexcept {return hairTranslucency.load(std::memory_order_acquire);}
void SetHairTranslucencyColour(bool byColour,uint32_t whitePoint) noexcept {
    hairByColour.store(byColour,std::memory_order_release);hairWhitePoint.store(std::clamp(whitePoint,5u,100u),std::memory_order_release);
}
bool HairTranslucencyByColour() noexcept {return hairByColour.load(std::memory_order_acquire);}
uint32_t HairTranslucencyWhitePoint() noexcept {return hairWhitePoint.load(std::memory_order_acquire);}
void SetHairBounces(uint32_t bounces) noexcept {hairBounces.store(std::clamp(bounces,1u,32u),std::memory_order_release);}
uint32_t HairBounces() noexcept {return hairBounces.load(std::memory_order_acquire);}
void SetHairAlbedoLinear(bool linear) noexcept {hairAlbedoLinear.store(linear,std::memory_order_release);}
bool HairAlbedoLinear() noexcept {return hairAlbedoLinear.load(std::memory_order_acquire);}
void SetHairIgnoreReflectionMask(bool ignore) noexcept {hairIgnoreReflectionMask.store(ignore,std::memory_order_release);}
bool HairIgnoreReflectionMask() noexcept {return hairIgnoreReflectionMask.load(std::memory_order_acquire);}
void SetHairDebugLighting(uint32_t mode) noexcept {hairDebugLighting.store(mode>5?0:mode,std::memory_order_release);}
uint32_t HairDebugLighting() noexcept {return hairDebugLighting.load(std::memory_order_acquire);}
void SetHairStencilCharacter(bool enabled) noexcept {hairStencilCharacter.store(enabled,std::memory_order_release);}
bool HairStencilCharacter() noexcept {return hairStencilCharacter.load(std::memory_order_acquire);}
bool TranslationWanted(const ShaderIdentity& shader) noexcept {
    return (HairTranslucency()&&shader.translucency)||(HairBounces()!=4&&shader.bounces)||(HairIgnoreReflectionMask()&&shader.masks)
        ||(HairDebugLighting()==3&&shader.hairFlags)||(HairStencilCharacter()&&shader.stencil);
}
const ShaderIdentity* FindShader(std::span<const std::byte> bytes,ShaderKind kind) {
    for(const auto& shader:kShaders)if(shader.kind==kind&&bytes.size()==shader.size&&HashEquals(bytes,shader.sha256))return &shader;
    return nullptr;
}
bool ApplyHairLook(std::string_view rewritten,const ShaderIdentity& shader,std::string& translated,std::string& applied,std::string& error) {
    try {
        translated.clear();applied.clear();
        if(shader.kind==ShaderKind::PathTracer)throw std::runtime_error("not a hair shader");
        std::string s=Named(std::string(rewritten));
        const auto note=[&](const char* fix) {applied+=applied.empty()?fix:std::string(", ")+fix;};
        if(HairAlbedoLinear()&&shader.albedo) {AlbedoGamma(s,*shader.albedo);note("linear albedo");}
        if(shader.kind==ShaderKind::Prepass) {
            if(HairDebugLighting()&&shader.debug) {DebugLines(s,*shader.debug,HairDebugLighting());note("debug lighting");}
            if(HairTranslucency()&&shader.translucency) {Translucent(s,*shader.translucency,HairTranslucency());note("translucency");}
        }
        Prune(s);
        translated=std::move(s);return true;
    } catch(const std::exception& e) {translated.clear();applied.clear();error=e.what();return false;}
}
bool TranslateIr(std::string_view original,const ShaderIdentity& shader,std::string& translated,std::string& error) {
    try {
        if(shader.kind==ShaderKind::PathTracer) {
            // Hair translucency and HairBounces only: the library is otherwise unchanged.
            if(!TranslationWanted(shader))throw std::runtime_error("nothing to translate");
            std::string s(original);
            if(HairBounces()!=4&&shader.bounces)Bounces(s,*shader.bounces,HairBounces());
            if(HairIgnoreReflectionMask()&&shader.masks)Masks(s,*shader.masks);
            if(HairStencilCharacter()&&shader.stencil)StencilCharacter(s,*shader.stencil);
            if(HairDebugLighting()==3&&shader.hairFlags)ZeroHairStores(s,*shader.hairFlags);
            if(HairTranslucency()&&shader.translucency)Translucent(s,*shader.translucency,HairTranslucency());
            translated=std::move(s);return true;
        }
        const bool pre=shader.kind==ShaderKind::Prepass;
        if(pre?!shader.prepass:!shader.closest)throw std::runtime_error("shader anchors missing");
        std::string s=Named(std::string(original));
        auto global=Match(s,R"(^(@".*HairVertexBuffers[^"]*") =)");global.resize(global.size()-2);
        if (!pre) {
            const auto& a=*shader.closest;
            s=One(std::move(s),"^  "+N(a.prim)+" = call i32 @dx.op.primitiveIndex.i32.*$","  %dots.prim = call i32 @dx.op.primitiveIndex.i32(i32 161)\n  "
                +N(a.prim)+" = lshr i32 %dots.prim, 2");
            s=One(std::move(s),"^  "+N(a.attrU)+" = extractelement.*$","  %dots.bx = extractelement <2 x float> "+N(a.attrLoad)+", i32 0\n"
                "  %dots.by = extractelement <2 x float> "+N(a.attrLoad)+", i32 1\n"
                "  %dots.sum = fadd float %dots.bx, %dots.by\n  %dots.parity = and i32 %dots.prim, 1\n  %dots.even = icmp eq i32 %dots.parity, 0\n"
                "  "+N(a.attrU)+" = select i1 %dots.even, float %dots.sum, float %dots.by");
            s=One(std::move(s),"^  "+N(a.endpointsFrom)+" = [\\s\\S]*?(?=^  "+N(a.endpointsTo)+" =)",
                Endpoints(false,global,"%dots.inst",N(a.index),a.nonuniform,a.noalias,a.extracts));
            const bool shell=RoundHitDistance()&&shader.closestShell;
            Normals(s,a.normals,a.normalMarker,Renamed(shell?Shell(kClosestRoundedNormal,"%r133","%r149"):std::string(kClosestRoundedNormal),
                NameMap(kClosest25575366.extracts,a.extracts,kClosest25575366.ray,a.ray,kClosest25575366.normals,a.normals)));
            // The payload hit distance, read after the rounded normal (its block
            // dominates the payload store in every anchored build).
            if(shell) {
                const std::string t=N(shader.closestShell->payloadT);
                s=One(std::move(s),"^  "+t+" = call float @dx\\.op\\.rayTCurrent\\.f32.*$",
                    "  %dots.tcur = call float @dx.op.rayTCurrent.f32(i32 154)\n  "+t+" = select i1 %dots.n88, float %dots.n82, float %dots.tcur");
            }
            if(HairAlbedoLinear()&&shader.albedo)AlbedoGamma(s,*shader.albedo);
        } else {
            const auto& a=*shader.prepass;
            // The game bridge must establish exclusive ownership of hair mask
            // 0x80 and the 32 descriptor slots before admitting this program.
            s=One(std::move(s),"^  "+N(a.hitFrom)+" = [\\s\\S]*?(?=^  br i1 "+N(a.hitBranch)+",)",
                "  %dots.id = call i32 @dx.op.rayQuery_StateScalar.i32(i32 208, i32 "+N(a.query)+")\n"
                "  %dots.triangle = icmp eq i32 "+N(a.candidateType)+", 1\n  %dots.slot = icmp ult i32 %dots.id, 32\n"
                "  %dots.valid = and i1 %dots.triangle, %dots.slot\n  "+N(a.hitBranch)+" = xor i1 %dots.valid, true\n");
            s=One(std::move(s),"^  "+N(a.alphaPrim)+" = call i32 @dx.op.rayQuery_StateScalar.i32.*$",
                "  %dots.palpha = call i32 @dx.op.rayQuery_StateScalar.i32(i32 210, i32 "+N(a.query)+")\n  "+N(a.alphaPrim)+" = lshr i32 %dots.palpha, 2");
            s=One(std::move(s),"^  "+N(a.alphaUFrom)+" = [\\s\\S]*?(?=^  "+N(a.alphaUTo)+" =)",QueryU(a.query,"dots.palpha",a.alphaU,"dots.ualpha"));
            s=One(std::move(s),"^  "+N(a.mainUFrom)+" = [\\s\\S]*?(?=^  "+N(a.mainUTo)+" =)","");
            s=One(std::move(s),"^  "+N(a.mainPrim)+" = call i32 @dx.op.rayQuery_StateScalar.i32.*$",
                "  %dots.pmain = call i32 @dx.op.rayQuery_StateScalar.i32(i32 210, i32 "+N(a.query)+")\n  "+N(a.mainPrim)+" = lshr i32 %dots.pmain, 2\n"
                    +QueryU(a.query,"dots.pmain",a.mainU,"dots.umain"));
            s=One(std::move(s),"^  "+N(a.endpointsFrom)+" = [\\s\\S]*?(?=^  "+N(a.endpointsTo)+" =)",
                Endpoints(true,global,N(a.instance),N(a.index),a.nonuniform,a.noalias,a.extracts));
            const bool shell=RoundHitDistance()&&shader.prepassShell;
            std::string rounded=Renamed(shell?Shell(kPrepassRoundedNormal,"%r463","%r479"):std::string(kPrepassRoundedNormal),
                NameMap(kPrepass25575366.extracts,a.extracts,kPrepass25575366.ray,a.ray,kPrepass25575366.normals,a.normals));
            if(shell)rounded+=PrepassShellTail(*shader.prepassShell,a.ray);
            Normals(s,a.normals,a.normalMarker,rounded);
            if(shell)RenameAfter(s,"^  "+N(a.normalMarker)+" =.*$",PrepassShellNames(*shader.prepassShell));
            if(HairAlbedoLinear()&&shader.albedo)AlbedoGamma(s,*shader.albedo);
            if(HairDebugLighting()&&shader.debug)DebugLines(s,*shader.debug,HairDebugLighting());
            if(HairTranslucency()&&shader.translucency)Translucent(s,*shader.translucency,HairTranslucency());
        }
        Prune(s);
        if(s.find("call i32 @dx.op.bufferUpdateCounter")!=std::string::npos)
            throw std::runtime_error("untranslated NVIDIA extension sequence");
        translated=std::move(s);return true;
    } catch(const std::exception& e) { translated.clear();error=e.what();return false; }
}
}
