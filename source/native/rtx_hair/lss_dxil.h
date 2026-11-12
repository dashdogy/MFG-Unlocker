#pragma once
// DXIL container in, DXIL container out: lss_ir.h's rewrite between DXC's disassembler
// and its finalize passes, assembler and validator (which signs the result). The same
// round trip as witcher_dots/shader_cache.cpp, for any shader.
#include "lss_ir.h"

#include <Windows.h>
// dxcapi.h needs IUnknown, IStream and BSTR, which Windows.h leaves out under
// WIN32_LEAN_AND_MEAN.
#include <objbase.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtx_hair {
struct DxcTools
{
    HMODULE compilerModule{}, validatorModule{};
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    Microsoft::WRL::ComPtr<IDxcLibrary> library;
    Microsoft::WRL::ComPtr<IDxcAssembler> assembler;
    Microsoft::WRL::ComPtr<IDxcOptimizer> optimizer;
    Microsoft::WRL::ComPtr<IDxcValidator> validator;
};

// dxcompiler.dll and dxil.dll from one absolute directory (never the search path).
bool LoadDxcTools(const std::filesystem::path& directory, DxcTools& tools, std::string& error);

bool DisassembleDxil(DxcTools& tools, std::span<const std::byte> container, std::string& text, std::string& error);

// HLSL source (entry point "main") to validated, signed DXIL, as witcher_dots's
// ShaderCache::CompileProgram does for its converter.
bool CompileHlsl(DxcTools& tools, std::string_view source, const wchar_t* target, std::vector<std::byte>& output, std::string& error);

// A further edit of the disassembly, after the LSS rewrite (which keeps the original value
// numbering, so anchors on it still hold): `original` is the container's disassembly, `ir`
// the text to edit in place. False with `error`: nothing is produced.
using IrTransform = std::function<bool(std::string_view original, std::string& ir, std::string& error)>;

// Rewrites a container that uses NVAPI LSS ops. False with `error` (and `output` empty)
// when the shader has none or cannot be rewritten: the caller keeps the original.
// `rewrittenIr`, if given, receives the IR handed to the optimizer (diagnostics).
// `transform` runs after the LSS rewrite; with `lssRequired` false, a shader without LSS
// ops gets the transform alone, and must then keep its NVIDIA-extension sequences and
// TraceRay calls.
bool RewriteDxil(DxcTools& tools, std::span<const std::byte> container, std::vector<std::byte>& output, IrRewriteReport& report,
    std::string& error, std::string* rewrittenIr = nullptr, const IrTransform* transform = nullptr, bool lssRequired = true);
}
