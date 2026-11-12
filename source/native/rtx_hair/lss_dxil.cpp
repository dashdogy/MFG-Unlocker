#include "lss_dxil.h"

#include <sstream>

namespace rtx_hair {
using Microsoft::WRL::ComPtr;
namespace {
bool Result(IDxcOperationResult* result, ComPtr<IDxcBlob>& blob, std::string& error)
{
    HRESULT status = E_FAIL;
    if (!result || FAILED(result->GetStatus(&status)))
    {
        error = "DXC returned no operation status";
        return false;
    }
    if (FAILED(status))
    {
        ComPtr<IDxcBlobEncoding> messages;
        result->GetErrorBuffer(&messages);
        error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "DXC operation failed";
        if (error.size() > 4096) error.resize(4096);
        return false;
    }
    return SUCCEEDED(result->GetResult(&blob)) && blob;
}

// The disassembly's resource-binding table without its name column. The names come from
// the container's reflection part, which a container assembled from IR does not have
// (non-library shaders keep no names in the module), so only the binding itself counts.
std::string Bindings(const std::string& text)
{
    const auto begin = text.find("; Resource Bindings:"), end = text.find("target datalayout", begin);
    if (begin == std::string::npos || end == std::string::npos) return {};
    std::istringstream in(text.substr(begin, end - begin));
    std::string line, out;
    while (std::getline(in, line))
    {
        std::istringstream split(line);
        std::vector<std::string> words;
        for (std::string word; split >> word;) words.push_back(word);
        // "; [<name>] <type> <format> <dim> <id> <bind> <count>": drop the ";" and the
        // name when present (8 words with it, 7 without).
        const bool comment = !words.empty() && words[0] == ";";
        const size_t first = comment ? (words.size() == 8 ? 2 : 1) : 0;
        for (size_t i = first; i < words.size(); ++i) out += words[i] + ' ';
        out += '\n';
    }
    return out;
}
} // namespace

bool LoadDxcTools(const std::filesystem::path& directory, DxcTools& tools, std::string& error)
{
    tools = {};
    const auto root = std::filesystem::path(directory).make_preferred();
    if (!root.is_absolute())
    {
        error = "DXC directory must be absolute";
        return false;
    }
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    tools.validatorModule = LoadLibraryExW((root / L"dxil.dll").c_str(), nullptr, flags);
    tools.compilerModule = LoadLibraryExW((root / L"dxcompiler.dll").c_str(), nullptr, flags);
    if (!tools.compilerModule || !tools.validatorModule)
    {
        error = "dxcompiler.dll or dxil.dll not loadable";
        return false;
    }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(tools.compilerModule, "DxcCreateInstance"));
    if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&tools.compiler))) || FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&tools.library)))
        || FAILED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&tools.assembler))) || FAILED(create(CLSID_DxcOptimizer, IID_PPV_ARGS(&tools.optimizer)))
        || FAILED(create(CLSID_DxcValidator, IID_PPV_ARGS(&tools.validator))))
    {
        error = "DXC interface unavailable";
        return false;
    }
    return true;
}

bool DisassembleDxil(DxcTools& tools, std::span<const std::byte> container, std::string& text, std::string& error)
{
    if (!tools.compiler || container.empty() || container.size() > 64u * 1024 * 1024)
    {
        error = "invalid shader input";
        return false;
    }
    const DxcBuffer input{container.data(), container.size(), 0};
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlob> blob;
    if (FAILED(tools.compiler->Disassemble(&input, IID_PPV_ARGS(&result))) || !Result(result.Get(), blob, error)) return false;
    text.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return true;
}

bool CompileHlsl(DxcTools& tools, std::string_view source, const wchar_t* target, std::vector<std::byte>& output, std::string& error)
{
    output.clear();
    if (!tools.compiler || !tools.validator || source.empty() || source.size() > 1024 * 1024 || !target)
    {
        error = "invalid compiler input";
        return false;
    }
    const DxcBuffer input{source.data(), source.size(), CP_UTF8};
    const wchar_t* args[] = {L"-E", L"main", L"-T", target, L"-O3", L"-HV", L"2021"};
    ComPtr<IDxcResult> result;
    ComPtr<IDxcBlob> compiled;
    if (FAILED(tools.compiler->Compile(&input, args, static_cast<UINT32>(std::size(args)), nullptr, IID_PPV_ARGS(&result)))
        || !Result(result.Get(), compiled, error))
        return false;
    ComPtr<IDxcOperationResult> validation;
    ComPtr<IDxcBlob> validated;
    if (FAILED(tools.validator->Validate(compiled.Get(), 0, &validation)) || !Result(validation.Get(), validated, error)) return false;
    const auto* first = static_cast<const std::byte*>(validated->GetBufferPointer());
    output.assign(first, first + validated->GetBufferSize());
    return true;
}

bool RewriteDxil(DxcTools& tools, std::span<const std::byte> container, std::vector<std::byte>& output, IrRewriteReport& report,
    std::string& error, std::string* rewrittenIr, const IrTransform* transform, bool lssRequired)
{
    output.clear();
    report = {};
    std::string original, ir;
    if (!DisassembleDxil(tools, container, original, error)) return false;
    const bool lss = UsesLss(original);
    if (!lss && (lssRequired || !transform))
    {
        error = "no NVAPI LSS op";
        return false;
    }
    if (lss && !RewriteLssIr(original, ir, report, error)) return false;
    if (!lss) ir = original;
    if (transform && !(*transform)(original, ir, error)) return false;
    if (rewrittenIr) *rewrittenIr = ir;
    // Assembling IR alone does not finalize a library; run DXC's standard DXIL passes
    // first (they also drop the dead handle loads left by the removed sequences).
    ComPtr<IDxcBlobEncoding> input, messages;
    ComPtr<IDxcBlob> optimized, assembled;
    ComPtr<IDxcOperationResult> result;
    const wchar_t* passes[] = {L"-hlsl-dxilload", L"-instcombine", L"-simplifycfg", L"-dce", L"-hlsl-dxilfinalize"};
    if (FAILED(tools.library->CreateBlobWithEncodingFromPinned(ir.data(), static_cast<UINT32>(ir.size()), CP_UTF8, &input)))
    {
        error = "IR blob creation failed";
        return false;
    }
    if (FAILED(tools.optimizer->RunOptimizer(input.Get(), passes, static_cast<UINT32>(std::size(passes)), &optimized, &messages)) || !optimized)
    {
        error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize()) : "DXIL finalization failed";
        if (error.size() > 4096) error.resize(4096);
        return false;
    }
    if (FAILED(tools.assembler->AssembleToContainer(optimized.Get(), &result)) || !Result(result.Get(), assembled, error)) return false;
    ComPtr<IDxcOperationResult> validation;
    ComPtr<IDxcBlob> validated;
    if (FAILED(tools.validator->Validate(assembled.Get(), 0, &validation)) || !Result(validation.Get(), validated, error)) return false;
    const auto* first = static_cast<const std::byte*>(validated->GetBufferPointer());
    output.assign(first, first + validated->GetBufferSize());
    // The result must keep the original's resource bindings and have no LSS op left.
    std::string final;
    if (!DisassembleDxil(tools, output, final, error))
    {
        output.clear();
        return false;
    }
    if (Bindings(original) != Bindings(final))
    {
        output.clear();
        error = "rewritten shader changed its resource bindings";
        return false;
    }
    if (UsesLss(final))
    {
        output.clear();
        error = "rewritten shader kept an LSS op";
        return false;
    }
    if (!lss)
    {
        // Transform only: the shader keeps every NVIDIA-extension sequence and TraceRay.
        const auto count = [](const std::string& s, std::string_view needle) {
            size_t n = 0;
            for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
            return n;
        };
        if (count(final, "call i32 @dx.op.bufferUpdateCounter") != count(original, "call i32 @dx.op.bufferUpdateCounter")
            || count(final, "call void @dx.op.traceRay") != count(original, "call void @dx.op.traceRay"))
        {
            output.clear();
            error = "transformed shader lost an extension sequence or a TraceRay";
            return false;
        }
    }
    return true;
}
} // namespace rtx_hair
