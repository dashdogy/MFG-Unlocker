// RtxHairIrTests: CPU checks of source/native/rtx_hair/lss_ir.cpp (rtxhair.1 phase 1a).
//
// RtxHairIrTests --fixtures <dir>                 the committed fixtures (DXC output of probe_lss.hlsl)
//                [--ir <file.ll> ...]             any other DXIL text (rewrite + structure checks only)
//                [--dxc <dir> --container <file> ...]  full round trip of DXIL containers through
//                                                 DXC (rewrite, finalize, assemble, validate)
//                [--witcher-look]                 containers of Witcher 3 (known by size): also its
//                                                 look fixes (witcher_dots ApplyHairLook on the hair
//                                                 shaders, TranslateIr on the path tracer libraries),
//                                                 as universal hair applies them in the game
// Exit codes: 0 pass, 1 a check failed, 2 setup failure.
#include "rtx_hair/lss_dxil.h"
#include "rtx_hair/lss_ir.h"
#include "witcher_dots/shaders.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// shader_ir.cpp's FindShader hashes through shader_cache.cpp, which this test does not link
// (it picks Witcher 3's shaders by size): never called here.
namespace witcher_dots
{
std::array<uint8_t, 32> Sha256(std::span<const std::byte>)
{
    return {};
}
bool HashEquals(std::span<const std::byte>, const char*)
{
    return false;
}
}

namespace
{
int failures = 0;

void Check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot read %ls\n", path.c_str());
        std::exit(2);
    }
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// Structure of rewritten IR: per function, numbered values and labels stay consecutive
// (LLVM's rule), every %rtxhair value is defined once and before any use.
std::string Structure(const std::string& ir)
{
    std::istringstream in(ir);
    std::string line, problem;
    bool inside = false;
    long last = -1;
    std::set<std::string> defined;
    static const std::regex number(R"(^  %(\d+) = |^; <label>:(\d+))");
    static const std::regex ours(R"(%rtxhair\.[\w.]+)");
    while (std::getline(in, line))
    {
        if (line.rfind("define ", 0) == 0)
        {
            inside = true;
            last = -1;
            defined.clear();
            continue;
        }
        if (!inside) continue;
        if (line == "}")
        {
            inside = false;
            continue;
        }
        std::smatch m;
        if (std::regex_search(line, m, number))
        {
            const long n = std::stol(m[1].matched ? m[1].str() : m[2].str());
            if (last >= 0 && n != last + 1 && problem.empty()) problem = "numbering jumps from %" + std::to_string(last) + " to %" + std::to_string(n);
            last = n;
        }
        const size_t eq = line.find(" = ");
        const std::string def = line.rfind("  %rtxhair.", 0) == 0 && eq != std::string::npos ? line.substr(2, eq - 2) : std::string{};
        for (auto it = std::sregex_iterator(line.begin(), line.end(), ours); it != std::sregex_iterator(); ++it)
        {
            const std::string name = it->str();
            if (name == def) continue;
            if (!defined.count(name) && problem.empty()) problem = name + " used before its definition";
        }
        if (!def.empty() && !defined.insert(def).second && problem.empty()) problem = def + " defined twice";
    }
    return problem;
}

void RewriteText(const std::string& name, const std::string& ir, const rtx_hair::IrRewriteReport* expected)
{
    std::string out, error;
    rtx_hair::IrRewriteReport report;
    Check(rtx_hair::UsesLss(ir), name + ": input uses LSS");
    const bool ok = rtx_hair::RewriteLssIr(ir, out, report, error);
    Check(ok, name + ": rewrite" + (ok ? "" : " (" + error + ")"));
    if (!ok) return;
    std::printf("  functions %u hitContexts %u queryContexts %u ops %u reads %u\n", report.functions, report.hitContexts, report.queryContexts,
        report.ops, report.reads);
    Check(!rtx_hair::UsesLss(out), name + ": no LSS op left");
    const std::string problem = Structure(out);
    Check(problem.empty(), name + ": structure" + (problem.empty() ? "" : " (" + problem + ")"));
    std::string again;
    rtx_hair::IrRewriteReport second;
    Check(rtx_hair::RewriteLssIr(ir, again, second, error) && again == out, name + ": deterministic");
    if (expected)
        Check(report.functions == expected->functions && report.hitContexts == expected->hitContexts
                && report.queryContexts == expected->queryContexts && report.ops == expected->ops && report.reads == expected->reads,
            name + ": expected counts");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::filesystem::path fixtures, dxc;
    std::vector<std::filesystem::path> irs, containers;
    bool witcherLook = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--witcher-look")
        {
            witcherLook = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "incomplete argument %ls\n", a.c_str());
            return 2;
        }
        if (a == L"--fixtures") fixtures = argv[++i];
        else if (a == L"--ir") irs.push_back(argv[++i]);
        else if (a == L"--dxc") dxc = argv[++i];
        else if (a == L"--container") containers.push_back(argv[++i]);
        else
        {
            std::fprintf(stderr, "unknown argument %ls\n", a.c_str());
            return 2;
        }
    }
    if (fixtures.empty() && irs.empty() && containers.empty())
    {
        std::fprintf(stderr, "usage: RtxHairIrTests --fixtures <dir> [--ir <file.ll>] [--dxc <dir> --container <file>]\n");
        return 2;
    }
    if (!fixtures.empty())
    {
        // Closest hit: NvRtIsLssHit, NvRtLssObjectPositionsAndRadii; reads: PrimitiveIndex,
        // the barycentrics (one float2 load), RayTCurrent.
        const std::string lib = Read(fixtures / L"lss_lib.ll");
        rtx_hair::IrRewriteReport libCounts{1, 1, 0, 2, 3};
        RewriteText("lss_lib.ll", lib, &libCounts);
        // Ray query: CommittedIsLss in one block with the reads (primitive, barycentrics
        // x and y, RayT); CommittedLssHitParameter and the positions in the next block.
        const std::string cs = Read(fixtures / L"lss_cs.ll");
        rtx_hair::IrRewriteReport csCounts{1, 0, 2, 3, 4};
        RewriteText("lss_cs.ll", cs, &csCounts);
        // Refusals: an unsupported LSS op, and no extension UAV.
        std::string out, error;
        rtx_hair::IrRewriteReport report;
        // (Each result is taken before its message: argument evaluation order is unspecified.)
        std::string candidate = std::regex_replace(cs, std::regex(R"(i32 0, i32 106,)"), "i32 0, i32 105,");
        bool refused = !rtx_hair::RewriteLssIr(candidate, out, report, error);
        Check(refused && error.find("unsupported NVAPI LSS op 105") != std::string::npos && out.empty(), "unsupported op refused (" + error + ")");
        const std::string noExt = std::regex_replace(lib, std::regex(R"(g_NvidiaExt)"), "g_Other");
        refused = !rtx_hair::RewriteLssIr(noExt, out, report, error);
        Check(refused && error.find("no NVAPI extension UAV") != std::string::npos && out.empty(), "no extension UAV refused (" + error + ")");
        Check(!rtx_hair::UsesLss("define void @f() {\n  ret void\n}\n"), "plain shader has no LSS");
    }
    for (const auto& path : irs) RewriteText(path.filename().string(), Read(path), nullptr);
    if (!containers.empty())
    {
        rtx_hair::DxcTools tools;
        std::string error;
        if (dxc.empty() || !rtx_hair::LoadDxcTools(std::filesystem::absolute(dxc), tools, error))
        {
            std::fprintf(stderr, "DXC: %s\n", dxc.empty() ? "--dxc missing" : error.c_str());
            return 2;
        }
        if (witcherLook)
        {
            // RTXMFG.WitcherDOTS.ini defaults.
            witcher_dots::SetHairAlbedoLinear(true);
            witcher_dots::SetHairTranslucency(65);
            witcher_dots::SetHairTranslucencyColour(true, 35);
        }
        for (const auto& path : containers)
        {
            const std::string bytes = Read(path);
            std::vector<std::byte> output;
            rtx_hair::IrRewriteReport report;
            const auto* first = reinterpret_cast<const std::byte*>(bytes.data());
            const witcher_dots::ShaderIdentity* witcher = nullptr;
            if (witcherLook)
                for (const auto& known : witcher_dots::KnownShaders())
                    if (known.size == bytes.size()) witcher = &known;
            if (!witcher)
            {
                const bool ok = rtx_hair::RewriteDxil(tools, {first, first + bytes.size()}, output, report, error);
                Check(ok, path.filename().string() + ": DXIL round trip" + (ok ? "" : " (" + error + ")"));
                if (ok)
                    std::printf("  %zu -> %zu bytes, functions %u hitContexts %u queryContexts %u ops %u reads %u\n", bytes.size(), output.size(),
                        report.functions, report.hitContexts, report.queryContexts, report.ops, report.reads);
                continue;
            }
            const bool library = witcher->kind == witcher_dots::ShaderKind::PathTracer;
            std::string applied;
            const rtx_hair::IrTransform transform = [&](std::string_view original, std::string& ir, std::string& why) {
                std::string out;
                const bool done = library ? witcher_dots::TranslateIr(original, *witcher, out, why)
                                          : witcher_dots::ApplyHairLook(ir, *witcher, out, applied, why);
                if (done) ir = std::move(out);
                if (done && library) applied = witcher->label ? witcher->label : "path tracer";
                return done;
            };
            const bool ok = rtx_hair::RewriteDxil(tools, {first, first + bytes.size()}, output, report, error, nullptr, &transform, !library);
            const char* kind = library ? "path tracer library" : witcher->kind == witcher_dots::ShaderKind::Prepass ? "hair prepass" : "hair closest hit";
            Check(ok, path.filename().string() + ": Witcher 3 look fixes on the " + kind + (ok ? " (" + applied + ")" : " (" + error + ")"));
            if (ok && !library)
            {
                const bool prepass = witcher->kind == witcher_dots::ShaderKind::Prepass;
                Check(applied.find("linear albedo") != std::string::npos && (!prepass || applied.find("translucency") != std::string::npos),
                    path.filename().string() + ": the expected look fixes applied");
            }
            if (ok)
                std::printf("  %zu -> %zu bytes, lss ops %u reads %u\n", bytes.size(), output.size(), report.ops, report.reads);
        }
    }
    std::printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
