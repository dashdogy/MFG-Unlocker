// RtxHairSpirvTests (CTest rtxhair-spirv, CPU): vkhair.1, source/native/rtx_hair/lss_spirv and
// spirv_module on SPIR-V compiled at build time by the pinned Vulkan SDK's glslangValidator.
//
// RtxHairSpirvTests --fixtures <dir> --probe <dir> --spirv-tools <SPIRV-Tools-shared.dll> [--dump <dir>]
//
// Every module: parse + emit gives back the same words, and every instruction's operands decode against
// the grammar table. Every case: the expected result (Rewritten / Neutral / Unchanged) and report counts;
// a rewritten or neutral module validates (spirv-val's library, Vulkan 1.3 rules), declares no LSS or
// sphere capability, carries the position-fetch capability it needs, and comes back Unchanged when
// rewritten again; the neutral form of every LSS module validates too. SPIRV-Tools is a test-only input:
// the DLL never loads it. Exit 0 pass, 1 a check failed, 2 setup failure.
#include "rtx_hair/lss_spirv.h"
#include "rtx_hair/spirv_module.h"

#include <Windows.h>
#include <spirv-tools/libspirv.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using CreateFn = spv_context (*)(spv_target_env);
using DestroyFn = void (*)(spv_context);
using ValidateFn = spv_result_t (*)(spv_const_context, const uint32_t*, size_t, spv_diagnostic*);
using DiagnosticDestroyFn = void (*)(spv_diagnostic);
using ToTextFn = spv_result_t (*)(spv_const_context, const uint32_t*, size_t, uint32_t, spv_text*, spv_diagnostic*);
using TextDestroyFn = void (*)(spv_text);

struct Tools
{
    CreateFn create{};
    DestroyFn destroy{};
    ValidateFn validate{};
    DiagnosticDestroyFn diagnosticDestroy{};
    ToTextFn toText{};
    TextDestroyFn textDestroy{};
    spv_context context{};

    bool Load(const std::filesystem::path& dll)
    {
        HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) return false;
        create = reinterpret_cast<CreateFn>(GetProcAddress(module, "spvContextCreate"));
        destroy = reinterpret_cast<DestroyFn>(GetProcAddress(module, "spvContextDestroy"));
        validate = reinterpret_cast<ValidateFn>(GetProcAddress(module, "spvValidateBinary"));
        diagnosticDestroy = reinterpret_cast<DiagnosticDestroyFn>(GetProcAddress(module, "spvDiagnosticDestroy"));
        toText = reinterpret_cast<ToTextFn>(GetProcAddress(module, "spvBinaryToText"));
        textDestroy = reinterpret_cast<TextDestroyFn>(GetProcAddress(module, "spvTextDestroy"));
        if (!create || !destroy || !validate || !diagnosticDestroy || !toText || !textDestroy) return false;
        context = create(SPV_ENV_VULKAN_1_3);
        return context != nullptr;
    }

    bool Validate(const std::vector<uint32_t>& words, std::string& message) const
    {
        spv_diagnostic diagnostic = nullptr;
        const spv_result_t result = validate(context, words.data(), words.size(), &diagnostic);
        if (diagnostic && diagnostic->error) message = diagnostic->error;
        diagnosticDestroy(diagnostic);
        return result == SPV_SUCCESS;
    }

    std::string Disassemble(const std::vector<uint32_t>& words) const
    {
        spv_text text = nullptr;
        spv_diagnostic diagnostic = nullptr;
        std::string out;
        if (toText(context, words.data(), words.size(), SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES, &text, &diagnostic) == SPV_SUCCESS && text)
            out.assign(text->str, text->length);
        else if (diagnostic && diagnostic->error)
            out = std::string("; disassembly failed: ") + diagnostic->error;
        textDestroy(text);
        diagnosticDestroy(diagnostic);
        return out;
    }
};

bool ReadWords(const std::filesystem::path& path, std::vector<uint32_t>& words)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0 || bytes.empty()) return false;
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return true;
}

bool HasCapability(const std::vector<uint32_t>& words, uint32_t capability)
{
    rtx_hair::spirv::Module m;
    std::string error;
    return m.Parse(words, error) && m.HasCapability(capability);
}

struct Case
{
    const char* file;
    rtx_hair::SpirvRewrite expect;
    uint32_t hitEntryPoints, builtIns, ops, reads;
    bool hitFetch, queryFetch; // RayTracingPositionFetchKHR / RayQueryPositionFetchKHR expected
};

const char* Name(rtx_hair::SpirvRewrite r)
{
    switch (r)
    {
    case rtx_hair::SpirvRewrite::Unchanged: return "Unchanged";
    case rtx_hair::SpirvRewrite::Rewritten: return "Rewritten";
    case rtx_hair::SpirvRewrite::Neutral: return "Neutral";
    default: return "Failed";
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    std::filesystem::path fixtures, probe, toolsDll, dump;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::wstring a = argv[i];
        if (a == L"--fixtures") fixtures = argv[i + 1];
        else if (a == L"--probe") probe = argv[i + 1];
        else if (a == L"--spirv-tools") toolsDll = argv[i + 1];
        else if (a == L"--dump") dump = argv[i + 1];
        else
        {
            std::fprintf(stderr, "RtxHairSpirvTests: unknown argument %ls\n", a.c_str());
            return 2;
        }
    }
    Tools tools;
    if (fixtures.empty() || probe.empty() || !tools.Load(toolsDll))
    {
        std::fprintf(stderr, "RtxHairSpirvTests: needs --fixtures, --probe and a loadable --spirv-tools DLL (%ls)\n", toolsDll.c_str());
        return 2;
    }
    if (!dump.empty()) std::filesystem::create_directories(dump);

    using R = rtx_hair::SpirvRewrite;
    // hit entry points, LSS built-ins, LSS ray-query ops, redirected reads (variables in hit shaders, read
    // sites in ray-query functions).
    const Case cases[] = {
        {"lss_closest.rchit", R::Rewritten, 1, 3, 0, 3, true, false},
        {"lss_anyhit.rahit", R::Rewritten, 1, 2, 0, 0, true, false},
        {"lss_query.comp", R::Rewritten, 0, 0, 4, 3, false, true},
        {"lss_candidate.comp", R::Rewritten, 0, 0, 2, 0, false, true},
        {"lss_query.frag", R::Rewritten, 0, 0, 2, 0, false, true},
        {"probe_lss.comp", R::Rewritten, 0, 0, 4, 3, false, true},
        {"probe_lss.rchit", R::Rewritten, 1, 3, 0, 3, true, false},
        {"sphere.rchit", R::Neutral, 0, 0, 0, 0, false, false},
        {"ser_lss.rgen", R::Neutral, 0, 0, 0, 0, false, false},
        {"plain.rchit", R::Unchanged, 0, 0, 0, 0, false, false},
        {"probe_lss.rgen", R::Unchanged, 0, 0, 0, 0, false, false},
        {"probe.comp", R::Unchanged, 0, 0, 0, 0, false, false},
    };
    int failures = 0, checks = 0;
    auto fail = [&](const char* file, const std::string& what) {
        ++failures;
        std::printf("FAIL %s: %s\n", file, what.c_str());
    };
    for (const Case& c : cases)
    {
        // Fixtures, else the probe's own shaders (<file>.spv in either folder).
        std::filesystem::path path = fixtures / (std::string(c.file) + ".spv");
        if (!std::filesystem::exists(path)) path = probe / (std::string(c.file) + ".spv");
        std::vector<uint32_t> words;
        if (!ReadWords(path, words))
        {
            fail(c.file, "cannot read " + path.string());
            continue;
        }
        std::string error;
        // The editor alone: parse + emit is the identity, every operand list decodes.
        {
            ++checks;
            rtx_hair::spirv::Module m;
            if (!m.Parse(words, error)) fail(c.file, "parse: " + error);
            else
            {
                if (m.Emit() != words) fail(c.file, "parse + emit changed the words");
                std::vector<rtx_hair::spirv::IdOperand> ids;
                size_t bad = 0;
                for (const auto* section : {&m.capabilities, &m.extensions, &m.imports, &m.memoryModel, &m.entryPoints, &m.executionModes, &m.debug, &m.annotations, &m.globals})
                    for (const auto& inst : *section) bad += m.IdOperands(inst, ids) ? 0 : 1;
                for (const auto& f : m.functions)
                    for (const auto& inst : f.body) bad += m.IdOperands(inst, ids) ? 0 : 1;
                if (bad) fail(c.file, std::to_string(bad) + " instruction(s) whose operands do not decode");
            }
        }
        ++checks;
        std::vector<uint32_t> out;
        rtx_hair::SpirvRewriteReport report;
        const R result = rtx_hair::RewriteLssSpirv(words, out, report, error);
        std::printf("%-20s %-9s hit=%u builtIns=%u ops=%u reads=%u contexts=%u neutralised=%u words %zu -> %zu%s%s\n", c.file, Name(result),
            report.hitEntryPoints, report.builtIns, report.ops, report.reads, report.queryContexts, report.neutralised, words.size(), out.size(),
            report.note.empty() ? "" : " note: ", report.note.c_str());
        if (result != c.expect)
        {
            fail(c.file, std::string("expected ") + Name(c.expect) + ", got " + Name(result) + (error.empty() ? "" : " (" + error + ")"));
            continue;
        }
        if (result == R::Unchanged)
        {
            if (!out.empty()) fail(c.file, "Unchanged with output");
            continue;
        }
        if (result == R::Rewritten
            && (report.hitEntryPoints != c.hitEntryPoints || report.builtIns != c.builtIns || report.ops != c.ops || report.reads != c.reads))
            fail(c.file, "report counts differ from the expected " + std::to_string(c.hitEntryPoints) + "/" + std::to_string(c.builtIns) + "/"
                + std::to_string(c.ops) + "/" + std::to_string(c.reads));
        std::string message;
        if (!tools.Validate(out, message)) fail(c.file, "rewritten module does not validate: " + message);
        if (rtx_hair::SpirvUsesLss(out)) fail(c.file, "an LSS or sphere capability is still declared");
        if (HasCapability(out, 5336) != c.hitFetch) fail(c.file, "RayTracingPositionFetchKHR capability not as expected");
        if (HasCapability(out, 5391) != c.queryFetch) fail(c.file, "RayQueryPositionFetchKHR capability not as expected");
        // hairmem.1: the two-ribbon decode ([RtxHair] Ribbons=2: 4 triangles per segment, ribbon pi/(2 sqrt 2)) is the same
        // rewrite with other constants: it validates, reports the same counts and is not the three-ribbon module.
        if (result == R::Rewritten)
        {
            ++checks;
            rtx_hair::LssDecodeLayout two;
            two.trianglesPerSegment = 4;
            two.ribbon = 1.11072073454f;
            std::vector<uint32_t> twoOut;
            rtx_hair::SpirvRewriteReport twoReport;
            if (rtx_hair::RewriteLssSpirv(words, twoOut, twoReport, error, two) != R::Rewritten) fail(c.file, "two-ribbon rewrite: " + error);
            else if (twoReport.hitEntryPoints != report.hitEntryPoints || twoReport.builtIns != report.builtIns || twoReport.ops != report.ops
                || twoReport.reads != report.reads)
                fail(c.file, "the two-ribbon rewrite reports other counts");
            else if (!tools.Validate(twoOut, message)) fail(c.file, "two-ribbon module does not validate: " + message);
            else if (twoOut == out) fail(c.file, "the two-ribbon decode is the three-ribbon one");
            std::vector<uint32_t> defaultOut;
            rtx_hair::SpirvRewriteReport defaultReport;
            if (rtx_hair::RewriteLssSpirv(words, defaultOut, defaultReport, error) != R::Rewritten || defaultOut != out)
                fail(c.file, "a rewrite after the two-ribbon one is not the default module again");
        }
        std::vector<uint32_t> again;
        rtx_hair::SpirvRewriteReport againReport;
        if (rtx_hair::RewriteLssSpirv(out, again, againReport, error) != R::Unchanged) fail(c.file, "a rewritten module is rewritten again");
        // The neutral form of every LSS module validates too.
        std::vector<uint32_t> neutral;
        rtx_hair::SpirvRewriteReport neutralReport;
        if (rtx_hair::NeutraliseLssSpirv(words, neutral, neutralReport, error) != R::Neutral) fail(c.file, "no neutral form: " + error);
        else if (!tools.Validate(neutral, message)) fail(c.file, "neutral module does not validate: " + message);
        else if (rtx_hair::SpirvUsesLss(neutral)) fail(c.file, "the neutral module still declares LSS");
        if (!dump.empty())
        {
            const std::string base = (dump / c.file).string();
            std::ofstream(base + ".rewritten.spvasm", std::ios::binary) << tools.Disassemble(out);
            std::ofstream(base + ".neutral.spvasm", std::ios::binary) << tools.Disassemble(neutral);
            std::ofstream(base + ".rewritten.spv", std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * 4));
        }
    }
    std::printf("RtxHairSpirvTests: %s (%d of %d checks failed)\n", failures ? "FAIL" : "PASS", failures, checks);
    tools.destroy(tools.context);
    return failures ? 1 : 0;
}
