#include "universal_hair.h"

#include "dots_builder.h"
#include "instance_patch.h"
#include "lss_dxil.h"
#include "nvapi_lss.h"
#include "rtxhair_converter_source.h"      // generated: kConverterHlsl
#include "rtxhair_instance_patch_source.h" // generated: kInstancePatchHlsl
#include "vk_hair_status.h"                // vkhair.4: the Vulkan side

#include "../dlss5.h"
#include "../overlay_graphics_api.h"
#include "../overlay_native.h"
#include "../single_module.h"
#include "../witcher_dots/gpu_runtime.h"
#include "../witcher_dots/shaders.h"
#include "../witcher_dots/witcher_dots.h"

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rtx_hair::universal {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t kInitialize = 0x0150e828;
constexpr uint32_t kIsNvShaderExtnOpCodeSupported = 0x3dfacec8;
constexpr uint32_t kOpTrianglePositions = 86, kOpCommittedTrianglePositions = 98; // what the rewritten shaders call
// The NVAPI LSS ops lss_ir.cpp rewrites. The candidate, HitObject and built-in ray T ops
// are not, so they keep the driver's answer (unsupported on this GPU).
constexpr std::array<uint32_t, 5> kRewrittenOps{104, 106, 112, 114, 117};
constexpr uint64_t kRequiresRayQuery = 0x00100000; // SFI0: D3D_SHADER_REQUIRES_RAYTRACING_TIER_1_1

using QueryFn = void*(__cdecl*)(uint32_t);
using InitializeFn = nvapi::Status(__cdecl*)();
using CapsFn = nvapi::Status(__cdecl*)(ID3D12Device*, uint32_t, void*, size_t);
using OpSupportedFn = nvapi::Status(__cdecl*)(ID3D12Device*, uint32_t, bool*);
struct PipelineOptions // NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_V1
{
    uint32_t version, flags;
};
using OptionsFn = nvapi::Status(__cdecl*)(ID3D12Device5*, const PipelineOptions*);

// The driver's functions, resolved through the original nvapi_QueryInterface before it
// is hooked; the hooks below forward everything that is not LSS to them.
std::atomic<QueryFn> realQuery{};
std::atomic<CapsFn> realCaps{};
std::atomic<OpSupportedFn> realOp{};
std::atomic<OptionsFn> realOptions{};
std::atomic<nvapi::PrebuildFn> realPrebuild{};
std::atomic<nvapi::BuildFn> realBuild{};
std::atomic<bool> admitted{};
std::atomic<bool> trackingAttempted{};
std::mutex prepareLock;

DotsBuilder& Builder()
{
    static auto* const builder = new DotsBuilder;
    return *builder;
}

InstancePatcher& Patcher()
{
    static auto* const patcher = new InstancePatcher;
    return *patcher;
}

// gpu_runtime's TopLevelPatch (instance_patch.h).
bool PatchInstances(ID3D12GraphicsCommandList4* native, D3D12_GPU_VIRTUAL_ADDRESS input, uint32_t count, D3D12_GPU_VIRTUAL_ADDRESS output) noexcept
{
    try
    {
        return Patcher().Record(native, input, count, output);
    }
    catch (...)
    {
        return false;
    }
}

std::atomic<bool> patchPublished{};
// A DOTS BLAS (built or inactive). The patch is published with the first one, so scenes
// without hair are left alone.
void HairBlas(D3D12_GPU_VIRTUAL_ADDRESS blas)
{
    Patcher().Add(blas);
    if (!patchPublished.exchange(true, std::memory_order_acq_rel))
    {
        witcher_dots::SetInstancePatch(&PatchInstances);
        single_module::Log(L"RTX_HAIR hair instance culling disabled in top-level builds from now on (DOTS ribbons are single-sided)");
    }
}

// DXC's interfaces are not thread-safe: every use is under shaderLock.
std::mutex shaderLock;
DxcTools& Tools()
{
    static auto* const tools = new DxcTools;
    return *tools;
}
struct Rewritten
{
    std::vector<std::byte> code; // empty: the game's shader is kept
};
std::unordered_map<uint64_t, std::shared_ptr<const Rewritten>>& ShaderResults()
{
    static auto* const results = new std::unordered_map<uint64_t, std::shared_ptr<const Rewritten>>;
    return *results;
}

std::atomic<uint64_t> capsAnswered{}, opsAnswered{}, optionsStripped{}, prebuilds{}, builds{}, inactiveBuilds{}, shadersRewritten{},
    shadersRefused{}, lookShaders{}, lookFailures{};

// hairauto.1: outside Witcher 3 the shader pass waits for the game's LSS capability query or
// ENABLE_LSS_SUPPORT option (NVAPI requires both before any LSS pipeline), so ray-traced games
// without RTX hair never pay for DXC's disassembly of their shaders.
std::atomic<bool> lssWanted{};
void WantLss(const wchar_t* why) noexcept
{
    if (lssWanted.exchange(true, std::memory_order_acq_rel)) return;
    wchar_t line[160]{};
    swprintf_s(line, L"RTX_HAIR the game asked for LSS (%s): its shaders are checked for LSS ops from now on", why);
    single_module::Log(line);
}
bool ShadersWanted() noexcept
{
    static const bool witcher = witcher_dots::GameProcess();
    return witcher || lssWanted.load(std::memory_order_acquire);
}

// Witcher 3's hair look fixes (witcher_dots/shaders.h, the RTXMFG.WitcherDOTS.ini settings)
// on the shaders universal hair creates there: its two hair shaders after the LSS rewrite,
// and its path tracer and shadow libraries. Off for the rest of the process once the
// driver refuses a state object that carries them.
std::atomic<bool> witcherLook{true};
bool WitcherLook() noexcept
{
    return witcherLook.load(std::memory_order_acquire) && witcher_dots::GameProcess();
}
// gpu_runtime's RewriteRefused.
bool LookRefused() noexcept
{
    if (!witcher_dots::GameProcess() || !witcherLook.exchange(false, std::memory_order_acq_rel)) return false;
    single_module::Log(L"RTX_HAIR the driver refused a state object with Witcher 3's look fixes: universal hair goes on without them");
    return true;
}
const witcher_dots::ShaderIdentity* WitcherShader(std::span<const std::byte> bytes) noexcept
{
    for (const auto kind : {witcher_dots::ShaderKind::ClosestHit, witcher_dots::ShaderKind::Prepass, witcher_dots::ShaderKind::PathTracer})
        if (const auto* shader = witcher_dots::FindShader(bytes, kind)) return shader;
    return nullptr;
}
std::atomic<uint32_t> buildLogs{}, shaderLogs{}, deviceLogs{};
std::atomic<uint64_t> lastBuildTick{};
std::atomic<bool> failed{};
std::mutex reasonLock;
std::string reasonText; // the latest not-admitted or failure reason (reasonLock)

void Log(const std::wstring& text) noexcept
{
    single_module::Log(text.c_str());
}

std::wstring Wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

// hairauto.1: on by default (universal_hair.h Requested).
bool ReadRequested() noexcept
{
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(L"RTXMFG_RTX_HAIR", value, static_cast<DWORD>(std::size(value)));
    if (length == 1 && (value[0] == L'0' || value[0] == L'1')) return value[0] == L'1';
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return true;
    path.resize(n);
    const auto ini = (std::filesystem::path(path).parent_path() / L"RTXMFG.RtxHair.ini").wstring();
    return GetPrivateProfileIntW(L"RtxHair", L"Universal", 1, ini.c_str()) != 0;
}

uint64_t Fnv(const std::byte* data, size_t size) noexcept
{
    uint64_t hash = 0xcbf29ce484222325ull ^ size;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

bool Contains(const std::byte* data, size_t size, std::string_view text) noexcept
{
    const auto* begin = reinterpret_cast<const char*>(data);
    return std::search(begin, begin + size, text.begin(), text.end()) != begin + size;
}

// A DXIL container worth disassembling: it names the NVAPI extension buffer (libraries
// keep resource names in RDAT), or it declares ray queries (non-library shaders keep no
// names).
bool Candidate(const std::byte* data, size_t size) noexcept
{
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0) return false;
    if (Contains(data, size, "g_NvidiaExt") || Contains(data, size, "NvShaderExtn")) return true;
    uint32_t parts{};
    std::memcpy(&parts, data + 28, 4);
    if (parts > 64 || 32 + static_cast<size_t>(parts) * 4 > size) return false;
    for (uint32_t i = 0; i < parts; ++i)
    {
        uint32_t offset{};
        std::memcpy(&offset, data + 32 + i * 4, 4);
        if (offset > size || size - offset < 16 || std::memcmp(data + offset, "SFI0", 4) != 0) continue;
        uint64_t flags{};
        std::memcpy(&flags, data + offset + 8, 8);
        return (flags & kRequiresRayQuery) != 0;
    }
    return false;
}

// gpu_runtime's ShaderRewrite: runs inside the game's CreateStateObject,
// AddToStateObject and CreateComputePipelineState. Results are cached by content.
bool RewriteShader(const void* code, size_t size, std::vector<std::byte>& output) noexcept
{
    try
    {
        const auto* data = static_cast<const std::byte*>(code);
        if (!data || !ShadersWanted() || !Candidate(data, size)) return false;
        const bool look = WitcherLook();
        const uint64_t key = Fnv(data, size) ^ (look ? 0x9E3779B97F4A7C15ull : 0);
        std::lock_guard lock(shaderLock);
        auto& results = ShaderResults();
        if (const auto found = results.find(key); found != results.end())
        {
            if (found->second->code.empty()) return false;
            output = found->second->code;
            return true;
        }
        auto result = std::make_shared<Rewritten>();
        IrRewriteReport report{};
        std::string error;
        // Witcher 3: its own look fixes on top (hair shaders) or alone (path tracer libraries).
        const witcher_dots::ShaderIdentity* witcher = look ? WitcherShader({data, size}) : nullptr;
        const bool library = witcher && witcher->kind == witcher_dots::ShaderKind::PathTracer;
        std::string applied, lookError;
        IrTransform transform;
        if (witcher && !library)
            transform = [&](std::string_view, std::string& ir, std::string& why) {
                std::string out;
                if (!witcher_dots::ApplyHairLook(ir, *witcher, out, applied, why)) return false;
                ir = std::move(out);
                return true;
            };
        else if (library && witcher_dots::TranslationWanted(*witcher))
            transform = [&](std::string_view original, std::string& ir, std::string& why) {
                std::string out;
                if (!witcher_dots::TranslateIr(original, *witcher, out, why)) return false;
                ir = std::move(out);
                applied = witcher->label ? witcher->label : "path tracer";
                return true;
            };
        bool rewritten = RewriteDxil(Tools(), {data, size}, result->code, report, error, nullptr, transform ? &transform : nullptr, !library);
        if (transform && !rewritten)
        {
            lookError = error;
            applied.clear();
            lookFailures.fetch_add(1, std::memory_order_relaxed);
            // The hair shader still gets the LSS rewrite; a library stays the game's.
            if (!library) rewritten = RewriteDxil(Tools(), {data, size}, result->code, report, error);
            else error = "no NVAPI LSS op";
        }
        if (rewritten && !applied.empty()) lookShaders.fetch_add(1, std::memory_order_relaxed);
        if (!lookError.empty() || !applied.empty())
        {
            wchar_t line[640]{};
            if (applied.empty())
                swprintf_s(line, L"RTX_HAIR Witcher 3 look fixes NOT applied to a %S (%zu bytes): %.400S", library ? "path tracer library" : "hair shader",
                    size, lookError.c_str());
            else
                swprintf_s(line, L"RTX_HAIR Witcher 3 look fixes on a %S (%zu -> %zu bytes): %S", library ? "path tracer library" : "hair shader", size,
                    result->code.size(), applied.c_str());
            single_module::Log(line);
        }
        if (!rewritten) result->code.clear();
        const bool lss = !library && (rewritten || error != "no NVAPI LSS op");
        if (rewritten) shadersRewritten.fetch_add(1, std::memory_order_relaxed);
        else if (lss) shadersRefused.fetch_add(1, std::memory_order_relaxed);
        if (lss && shaderLogs.fetch_add(1, std::memory_order_relaxed) < 32)
        {
            wchar_t line[640]{};
            if (rewritten)
                swprintf_s(line, L"RTX_HAIR shader rewritten: %zu -> %zu bytes, functions=%u hitContexts=%u queryContexts=%u lssOps=%u reads=%u",
                    size, result->code.size(), report.functions, report.hitContexts, report.queryContexts, report.ops, report.reads);
            else
                swprintf_s(line, L"RTX_HAIR shader with NVAPI LSS ops NOT rewritten (%zu bytes; the game's own is kept): %.400S", size,
                    error.c_str());
            single_module::Log(line);
        }
        if (results.size() >= 8192) results.clear();
        results.emplace(key, result);
        if (!rewritten) return false;
        output = result->code;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool RewrittenOp(uint32_t op) noexcept
{
    return std::find(kRewrittenOps.begin(), kRewrittenOps.end(), op) != kRewrittenOps.end();
}

nvapi::Status __cdecl Caps(ID3D12Device* device, uint32_t type, void* data, size_t size)
{
    if (type == nvapi::kCapsLinearSweptSpheres && data && size >= sizeof(uint32_t) && admitted.load(std::memory_order_acquire))
    {
        if (witcher_dots::TrackedDevice(device))
        {
            const uint32_t standard = nvapi::kLssCapStandard;
            WantLss(L"capability query");   // before the answer: the game's LSS pipelines come after it
            std::memcpy(data, &standard, sizeof(standard));
            if (capsAnswered.fetch_add(1, std::memory_order_relaxed) == 0)
                single_module::Log(L"RTX_HAIR LSS capability reported (STANDARD): the game sees RTX hair support");
            return nvapi::kOk;
        }
        if (deviceLogs.fetch_add(1, std::memory_order_relaxed) < 4)
            single_module::Log(L"RTX_HAIR LSS capability asked for a device other than the prepared one: the driver's answer is kept");
    }
    const auto real = realCaps.load(std::memory_order_acquire);
    return real ? real(device, type, data, size) : nvapi::kError;
}

nvapi::Status __cdecl OpSupported(ID3D12Device* device, uint32_t op, bool* supported)
{
    if (supported && RewrittenOp(op) && admitted.load(std::memory_order_acquire) && witcher_dots::TrackedDevice(device))
    {
        *supported = true;
        opsAnswered.fetch_add(1, std::memory_order_relaxed);
        return nvapi::kOk;
    }
    const auto real = realOp.load(std::memory_order_acquire);
    return real ? real(device, op, supported) : nvapi::kError;
}

nvapi::Status __cdecl Options(ID3D12Device5* device, const PipelineOptions* state)
{
    const auto real = realOptions.load(std::memory_order_acquire);
    if (!real) return nvapi::kError;
    if (state && (state->flags & nvapi::kPipelineEnableLss) && admitted.load(std::memory_order_acquire) && witcher_dots::TrackedDevice(device))
    {
        WantLss(L"ENABLE_LSS_SUPPORT");   // before forwarding: the pipelines it covers come after it
        PipelineOptions copy = *state;
        copy.flags &= ~nvapi::kPipelineEnableLss;
        if (optionsStripped.fetch_add(1, std::memory_order_relaxed) == 0)
            single_module::Log(L"RTX_HAIR pipeline option ENABLE_LSS_SUPPORT left out (hair shaders are rewritten to triangle position fetch)");
        return real(device, &copy);
    }
    return real(device, state);
}

nvapi::Status __cdecl Prebuild(ID3D12Device5* device, nvapi::PrebuildParams* params)
{
    const auto real = realPrebuild.load(std::memory_order_acquire);
    if (!real) return nvapi::kError;
    if (!params || !params->pDesc || !params->pInfo || (params->version & 0xFFFF) < sizeof(nvapi::PrebuildParams)
        || !admitted.load(std::memory_order_acquire) || !DotsBuilder::HasLss(*params->pDesc) || !witcher_dots::TrackedDevice(device))
        return real(device, params);
    prebuilds.fetch_add(1, std::memory_order_relaxed);
    return Builder().Prebuild(*params->pDesc, *params->pInfo);
}

nvapi::Status __cdecl Build(ID3D12GraphicsCommandList4* list, const nvapi::BuildParams* params)
{
    const auto real = realBuild.load(std::memory_order_acquire);
    if (!real) return nvapi::kError;
    if (!params || !params->pDesc || (params->version & 0xFFFF) < sizeof(nvapi::BuildParams) || !admitted.load(std::memory_order_acquire))
        return real(list, params);
    const nvapi::BuildInputsEx& inputs = params->pDesc->inputs;
    if (inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
    {
        // A top-level build through NVAPI: hair instances lose their culling, as through D3D12.
        if (inputs.descsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY || !inputs.numDescs || !inputs.instanceDescs || !patchPublished.load(std::memory_order_acquire))
            return real(list, params);
        std::string error;
        const D3D12_GPU_VIRTUAL_ADDRESS patched = witcher_dots::PatchTopLevel(list, inputs.instanceDescs, inputs.numDescs, error);
        if (!patched) return real(list, params);
        nvapi::BuildDescEx desc = *params->pDesc;
        desc.inputs.instanceDescs = patched;
        nvapi::BuildParams forwarded = *params;
        forwarded.pDesc = &desc;
        return real(list, &forwarded);
    }
    if (!DotsBuilder::HasLss(inputs)) return real(list, params);
    const D3D12_GPU_VIRTUAL_ADDRESS destination = params->pDesc->destAccelerationStructureData;
    auto& builder = Builder();
    nvapi::Status status = nvapi::kError;
    std::string error;
    const bool injected = witcher_dots::InjectCompute(list,
        [&](ID3D12GraphicsCommandList4* native, const std::function<void()>& restore) {
            status = builder.Build(native, *params, [&](ID3D12GraphicsCommandList4*) { restore(); });
        },
        error);
    if (injected)
    {
        if (status == nvapi::kOk)
        {
            lastBuildTick.store(GetTickCount64(), std::memory_order_relaxed);
            HairBlas(destination);
        }
        const uint64_t count = builds.fetch_add(1, std::memory_order_relaxed);
        if (count == 0 && status == nvapi::kOk)
            single_module::Log(L"RTX_HAIR first hair BLAS recorded as DOTS triangles (GPU completion and game image not yet verified)");
        if (status != nvapi::kOk && buildLogs.fetch_add(1, std::memory_order_relaxed) < 24)
            Log(L"RTX_HAIR hair BLAS build failed: " + Wide(builder.LastError()));
        return status;
    }
    // BuildInactive reads a buffer of the prepared device: only on that device's lists.
    ComPtr<ID3D12Device> owner;
    if (!list || FAILED(list->GetDevice(IID_PPV_ARGS(&owner))) || !witcher_dots::TrackedDevice(owner.Get()))
    {
        if (buildLogs.fetch_add(1, std::memory_order_relaxed) < 24)
            Log(L"RTX_HAIR hair BLAS on a list of another device: passed to the driver unchanged (" + Wide(error) + L")");
        return real(list, params);
    }
    inactiveBuilds.fetch_add(1, std::memory_order_relaxed);
    if (buildLogs.fetch_add(1, std::memory_order_relaxed) < 24)
        Log(L"RTX_HAIR hair BLAS built without hair (the converter could not run on this list: " + Wide(error) + L")");
    const nvapi::Status inactive = builder.BuildInactive(list, *params);
    if (inactive == nvapi::kOk) HairBlas(destination);
    return inactive;
}

void* __cdecl Query(uint32_t id)
{
    const auto original = realQuery.load(std::memory_order_acquire);
    void* const resolved = original ? original(id) : nullptr;
    if (!resolved || !admitted.load(std::memory_order_acquire)) return resolved;
    switch (id)
    {
    case nvapi::kGetRaytracingCaps: return reinterpret_cast<void*>(&Caps);
    case kIsNvShaderExtnOpCodeSupported: return reinterpret_cast<void*>(&OpSupported);
    case nvapi::kSetCreatePipelineStateOptions: return reinterpret_cast<void*>(&Options);
    case nvapi::kPrebuildInfoEx: return reinterpret_cast<void*>(&Prebuild);
    case nvapi::kBuildEx: return reinterpret_cast<void*>(&Build);
    default: return resolved;
    }
}

// RTX 40 series is Ada (CUDA compute capability 8.9), RTX 30 series Ampere (8.6); the
// CUDA device must match the game's D3D12 adapter LUID exactly once (as witcher_dots).
bool SupportedAdapter(const LUID& luid, int& major, int& minor) noexcept
{
    using InitFn = int(WINAPI*)(unsigned);
    using CountFn = int(WINAPI*)(int*);
    using DeviceFn = int(WINAPI*)(int*, int);
    using CapabilityFn = int(WINAPI*)(int*, int*, int);
    using LuidFn = int(WINAPI*)(char*, unsigned*, int);
    const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return false;
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(cuda, "cuInit"));
    const auto getCount = reinterpret_cast<CountFn>(GetProcAddress(cuda, "cuDeviceGetCount"));
    const auto getDevice = reinterpret_cast<DeviceFn>(GetProcAddress(cuda, "cuDeviceGet"));
    const auto getCapability = reinterpret_cast<CapabilityFn>(GetProcAddress(cuda, "cuDeviceComputeCapability"));
    const auto getLuid = reinterpret_cast<LuidFn>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    int count = 0;
    uint32_t matches = 0;
    bool complete = init && getCount && getDevice && getCapability && getLuid && init(0) == 0 && getCount(&count) == 0 && count > 0;
    for (int ordinal = 0; complete && ordinal < count; ++ordinal)
    {
        int device = 0, candidateMajor = 0, candidateMinor = 0;
        unsigned nodeMask = 0;
        std::array<char, sizeof(LUID)> raw{};
        if (getDevice(&device, ordinal) != 0 || getCapability(&candidateMajor, &candidateMinor, device) != 0
            || getLuid(raw.data(), &nodeMask, device) != 0)
        {
            complete = false;
            break;
        }
        LUID candidate{};
        std::memcpy(&candidate, raw.data(), sizeof(candidate));
        if (candidate.LowPart == luid.LowPart && candidate.HighPart == luid.HighPart)
        {
            ++matches;
            major = candidateMajor;
            minor = candidateMinor;
        }
    }
    FreeLibrary(cuda);
    return complete && matches == 1 && major == 8 && (minor == 9 || minor == 6);
}

bool AdapterDescription(ID3D12Device5* device, DXGI_ADAPTER_DESC1& desc) noexcept
{
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<FactoryFn>(GetProcAddress(single_module::LoadSystemModule(L"dxgi.dll"), "CreateDXGIFactory1"));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    return create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))
        && SUCCEEDED(adapter->GetDesc1(&desc));
}

constexpr char kNoTools[] = "dxcompiler.dll and dxil.dll not found beside the game or this module";

// The folders with both dxcompiler.dll and dxil.dll: the game executable's, then this module's.
std::vector<std::filesystem::path> ToolFolders()
{
    std::vector<std::filesystem::path> folders;
    for (const HMODULE module : {static_cast<HMODULE>(nullptr), single_module::Self()})
    {
        std::wstring path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!n || n >= path.size()) continue;
        path.resize(n);
        const auto folder = std::filesystem::path(path).parent_path();
        if (std::find(folders.begin(), folders.end(), folder) != folders.end()) continue;
        std::error_code missing;
        if (std::filesystem::exists(folder / L"dxcompiler.dll", missing) && std::filesystem::exists(folder / L"dxil.dll", missing))
            folders.push_back(folder);
    }
    return folders;
}

bool LoadTools(std::wstring& where, std::string& error)
{
    std::string reasons;
    for (const auto& folder : ToolFolders())
    {
        std::string why;
        if (LoadDxcTools(folder, Tools(), why))
        {
            where = folder.wstring();
            return true;
        }
        reasons += (reasons.empty() ? "" : "; ") + why;
    }
    error = reasons.empty() ? std::string(kNoTools) : "DXC unusable: " + reasons;
    return false;
}

enum class Outcome
{
    Prepared,
    NotAdmitted, // this device's GPU or driver: a later device may still be admitted
    Failed,      // tracking was attempted: never again in this process
};

Outcome Prepare(IUnknown* object, std::string& error)
{
    // hairauto.1: without DXC it can never be admitted, so turn the game away before the GPU
    // checks below (cuInit, NVAPI) run inside its device creation.
    if (ToolFolders().empty())
    {
        error = kNoTools;
        return Outcome::NotAdmitted;
    }
    ComPtr<ID3D12Device5> device;
    if (!witcher_dots::ResolveNativeDevice(object, device) || !device)
    {
        error = "native D3D12 device (ID3D12Device5) unavailable";
        return Outcome::NotAdmitted;
    }
    DXGI_ADAPTER_DESC1 adapter{};
    if (!AdapterDescription(device.Get(), adapter))
    {
        error = "authoritative DXGI adapter lookup failed";
        return Outcome::NotAdmitted;
    }
    int major = 0, minor = 0;
    if (adapter.VendorId != 0x10de || !SupportedAdapter(device->GetAdapterLuid(), major, minor))
    {
        char text[160]{};
        _snprintf_s(text, _TRUNCATE, "requires an RTX 40 or RTX 30 series GPU (vendor %04x, compute capability %d.%d)", adapter.VendorId, major, minor);
        error = text;
        return Outcome::NotAdmitted;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))
        || options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        error = "requires DXR 1.1";
        return Outcome::NotAdmitted;
    }
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (!nvapi) nvapi = single_module::LoadSystemModule(L"nvapi64.dll");
    const auto query = nvapi ? reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    const auto initialize = query ? reinterpret_cast<InitializeFn>(query(kInitialize)) : nullptr;
    if (!initialize || initialize() != nvapi::kOk)
    {
        error = "NVAPI unavailable";
        return Outcome::NotAdmitted;
    }
    const auto caps = reinterpret_cast<CapsFn>(query(nvapi::kGetRaytracingCaps));
    const auto op = reinterpret_cast<OpSupportedFn>(query(kIsNvShaderExtnOpCodeSupported));
    const auto pipelineOptions = reinterpret_cast<OptionsFn>(query(nvapi::kSetCreatePipelineStateOptions));
    const auto prebuild = reinterpret_cast<nvapi::PrebuildFn>(query(nvapi::kPrebuildInfoEx));
    const auto build = reinterpret_cast<nvapi::BuildFn>(query(nvapi::kBuildEx));
    if (!caps || !op || !pipelineOptions || !prebuild || !build)
    {
        error = "NVAPI driver lacks the raytracing caps, shader-op, pipeline-option or extended build interfaces";
        return Outcome::NotAdmitted;
    }
    uint32_t lss = 0;
    nvapi::Status status = caps(device.Get(), nvapi::kCapsLinearSweptSpheres, &lss, sizeof(lss));
    if (status == nvapi::kNotSupported) lss = 0, status = nvapi::kOk;
    if (status != nvapi::kOk)
    {
        error = "native LSS capability query failed";
        return Outcome::NotAdmitted;
    }
    if (lss & 1)
    {
        error = "native LSS supported: the game's own RTX hair path is kept";
        return Outcome::NotAdmitted;
    }
    bool positions = false, committed = false;
    if (op(device.Get(), kOpTrianglePositions, &positions) != nvapi::kOk || op(device.Get(), kOpCommittedTrianglePositions, &committed) != nvapi::kOk
        || !positions || !committed)
    {
        error = "the driver lacks NVAPI triangle position fetch (shader ops 86 and 98)";
        return Outcome::NotAdmitted;
    }
    std::wstring dxcFolder;
    std::vector<std::byte> converter, instancePatch;
    {
        std::lock_guard lock(shaderLock);
        if (!LoadTools(dxcFolder, error)) return Outcome::NotAdmitted;
        std::string why;
        if (!CompileHlsl(Tools(), kConverterHlsl, L"cs_6_0", converter, why))
        {
            error = "converter compile failed: " + why.substr(0, 400);
            return Outcome::NotAdmitted;
        }
        if (!CompileHlsl(Tools(), kInstancePatchHlsl, L"cs_6_0", instancePatch, why))
        {
            error = "instance patch compile failed: " + why.substr(0, 400);
            return Outcome::NotAdmitted;
        }
    }
    if (!Builder().Initialize(device.Get(), prebuild, build, converter, error)) return Outcome::NotAdmitted;
    if (!Patcher().Initialize(device.Get(), instancePatch, error)) return Outcome::NotAdmitted;
    realCaps.store(caps, std::memory_order_release);
    realOp.store(op, std::memory_order_release);
    realOptions.store(pipelineOptions, std::memory_order_release);
    realPrebuild.store(prebuild, std::memory_order_release);
    realBuild.store(build, std::memory_order_release);

    // From here on, a failure leaves this process without universal hair.
    trackingAttempted.store(true, std::memory_order_release);
    witcher_dots::SetRewriteRefused(&LookRefused);
    // Before any queue table hook: DLSS 5 hooks the runtime method behind it.
    dlss5::SetQueueForward(&witcher_dots::QueueExecuteForward);
    if (!witcher_dots::InitializeTracking(device.Get(), &RewriteShader, error))
    {
        if (!witcher_dots::AbortGpuPreparation()) single_module::Log(L"RTX_HAIR tracking rollback indeterminate; forwarding bindings retained");
        return Outcome::Failed;
    }
    const auto initialized = MH_Initialize();
    void* trampoline = nullptr;
    if ((initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        || MH_CreateHook(reinterpret_cast<void*>(query), reinterpret_cast<void*>(&Query), &trampoline) != MH_OK || !trampoline)
    {
        error = "nvapi_QueryInterface hook unavailable";
        witcher_dots::AbortGpuPreparation();
        return Outcome::Failed;
    }
    realQuery.store(reinterpret_cast<QueryFn>(trampoline), std::memory_order_release);
    // Admitted before the hook is live, so the game's first query after it is answered.
    admitted.store(true, std::memory_order_release);
    if (MH_EnableHook(reinterpret_cast<void*>(query)) != MH_OK)
    {
        admitted.store(false, std::memory_order_release);
        error = "nvapi_QueryInterface hook not enabled";
        witcher_dots::AbortGpuPreparation();
        return Outcome::Failed;
    }
    const LUID luid = device->GetAdapterLuid();
    wchar_t line[700]{};
    swprintf_s(line,
        L"RTX_HAIR universal hair prepared pid=%lu gpu=%s device=%04x capability=%d.%d luid=%08x:%08x dxc=%s "
        L"(LSS reported to the game; LSS builds become DOTS triangles, three ribbons and 6 triangles per segment with hair instance culling disabled; LSS shader ops rewritten to triangle position fetch)",
        GetCurrentProcessId(), adapter.Description, adapter.DeviceId, major, minor, static_cast<uint32_t>(luid.HighPart), luid.LowPart,
        dxcFolder.c_str());
    single_module::Log(line);
    if (witcher_dots::GameProcess())
    {
        wchar_t look[400]{};
        swprintf_s(look,
            L"RTX_HAIR Witcher 3 look fixes (RTXMFG.WitcherDOTS.ini): translucency %u%% (%s, white point %u%%), albedo %s, bounces %u, "
            L"reflection mask %s, stencil %s, debug lighting %u; hit distance on the strand enclosing the ribbons",
            witcher_dots::HairTranslucency(), witcher_dots::HairTranslucencyByColour() ? L"colour-aware" : L"colour-blind",
            witcher_dots::HairTranslucencyWhitePoint(), witcher_dots::HairAlbedoLinear() ? L"linear" : L"game's gamma",
            witcher_dots::HairBounces(), witcher_dots::HairIgnoreReflectionMask() ? L"ignored on hair" : L"game's",
            witcher_dots::HairStencilCharacter() ? L"character bit on hair" : L"game's", witcher_dots::HairDebugLighting());
        single_module::Log(look);
    }
    return Outcome::Prepared;
}
} // namespace

bool Requested() noexcept
{
    static const bool requested = ReadRequested();
    return requested;
}

void ObserveDevice(IUnknown* device) noexcept
{
    try
    {
        if (!device || !Requested() || admitted.load(std::memory_order_acquire) || trackingAttempted.load(std::memory_order_acquire)
            || single_overlay::native::InsideLoader())
            return;
        std::lock_guard lock(prepareLock);
        if (admitted.load(std::memory_order_acquire) || trackingAttempted.load(std::memory_order_acquire)) return;
        std::string error;
        const Outcome outcome = Prepare(device, error);
        if (outcome == Outcome::Prepared) return;
        if (outcome == Outcome::Failed) failed.store(true, std::memory_order_release);
        bool changed = false;
        {
            std::lock_guard reason(reasonLock);
            changed = reasonText != error;
            reasonText = error;
        }
        // A reason repeated by a later device is not logged again.
        static std::atomic<uint32_t> logs{};
        if ((changed || outcome == Outcome::Failed) && logs.fetch_add(1, std::memory_order_relaxed) < 8)
            Log(std::wstring(outcome == Outcome::Failed ? L"RTX_HAIR universal hair failed (off for this process): " : L"RTX_HAIR universal hair not admitted: ")
                + Wide(error));
    }
    catch (...)
    {
    }
}

// vkhair.4: Vulkan games are converted by vk_hair.h; ReadSnapshot shows their status.
void ObserveVulkanDevice() noexcept
{
}

bool CudaAdmits(const LUID& luid, int& major, int& minor) noexcept
{
    return SupportedAdapter(luid, major, minor);
}

Snapshot ReadSnapshot() noexcept
{
    Snapshot s;
    try
    {
        if (!Requested()) return s;
        // vkhair.4: a Vulkan game's conversion, while no D3D12 device was seen. Prepared once the LSS
        // extension is reported on an admitted GPU (as D3D12's caps answer), Active once its hair is built.
        namespace api = single_overlay::graphics_api;
        if (!admitted.load(std::memory_order_acquire) && !(api::Seen() & api::kD3D12))
        {
            const vk::Status v = vk::ReadStatus();
            if (v.prepared || v.failed || v.admitted || (v.seen && v.reason[0]))
            {
                s.state = v.failed ? State::Failed : (v.prepared || v.admitted) ? State::Prepared : State::NotAdmitted;
                if (s.state != State::Prepared) strncpy_s(s.reason, v.reason, _TRUNCATE);
                s.capsAnswered = v.extensionReports + v.featureReports;
                s.optionsStripped = v.pipelineFlagsStripped;
                s.shadersRewritten = v.shadersRewritten;
                s.shadersRefused = v.shadersNeutral + v.shadersFailed;
                s.prebuilds = v.sizeQueries;
                s.builds = v.builds;
                s.updates = v.updates;
                s.inactiveBuilds = v.invalidGeometries + v.indirectBuilds;
                s.failures = v.failures + v.hostBuildsRefused;
                s.poolBuffers = v.poolBuffers;
                s.poolBytes = v.poolBytes;
                s.evictions = v.evictions;
                s.rebuilds = v.rebuilds;
                s.hairBlas = v.hairBlas;
                s.topLevelPatched = v.topLevelPatched;
                s.instanceCopyBytes = v.topLevelCopyBytes; // hairmem.1: held now (it was a running total)
                s.vulkan = true;
                s.ribbons = v.ribbons;
                s.spareBytes = v.spareBytes;
                s.retiredBytes = v.retiredBytes;
                s.reuses = v.reuses;
                s.gameHairBlasBytes = v.gameHairBlasBytes;
                if (v.lastBuildTick) s.lastBuildAgeMs = GetTickCount64() - v.lastBuildTick;
                if (v.lastError[0]) strncpy_s(s.lastError, v.lastError, _TRUNCATE);
                else if (s.state == State::Prepared && !v.requested)
                    strncpy_s(s.lastError, "Vulkan: LSS is reported; the game has not enabled it (yet)", _TRUNCATE);
                return s;
            }
        }
        s.state = admitted.load(std::memory_order_acquire) ? State::Prepared
            : failed.load(std::memory_order_acquire)       ? State::Failed
                                                           : State::Waiting;
        {
            std::lock_guard lock(reasonLock);
            if (s.state == State::Waiting && !reasonText.empty()) s.state = State::NotAdmitted;
            if (s.state != State::Prepared) strncpy_s(s.reason, reasonText.c_str(), _TRUNCATE);
        }
        s.capsAnswered = capsAnswered.load(std::memory_order_relaxed);
        s.optionsStripped = optionsStripped.load(std::memory_order_relaxed);
        s.shadersRewritten = shadersRewritten.load(std::memory_order_relaxed);
        s.shadersRefused = shadersRefused.load(std::memory_order_relaxed);
        s.witcherLook = witcher_dots::GameProcess();
        s.lookShaders = lookShaders.load(std::memory_order_relaxed);
        s.lookFailures = lookFailures.load(std::memory_order_relaxed);
        s.lookRefused = s.witcherLook && !witcherLook.load(std::memory_order_acquire);
        if (s.state != State::Prepared) return s;
        const BuilderStats stats = Builder().Stats();
        s.prebuilds = stats.prebuilds;
        s.builds = stats.builds;
        s.updates = stats.updates;
        s.inactiveBuilds = stats.inactiveBuilds;
        s.skippedUpdates = stats.skippedUpdates;
        s.failures = stats.failures;
        s.poolBuffers = stats.poolBuffers;
        s.poolBytes = stats.poolBytes;
        s.evictions = stats.evictions;
        s.rebuilds = stats.rebuilds;
        s.hairBlas = Patcher().Size();
        const auto topLevel = witcher_dots::ReadTopLevelStats();
        s.topLevelPatched = topLevel.patched;
        s.topLevelUnpatched = topLevel.unpatched;
        s.instanceCopyBytes = topLevel.leaseBytes;
        if (const uint64_t tick = lastBuildTick.load(std::memory_order_relaxed)) s.lastBuildAgeMs = GetTickCount64() - tick;
        strncpy_s(s.lastError, Builder().LastError().c_str(), _TRUNCATE);
    }
    catch (...)
    {
    }
    return s;
}
} // namespace rtx_hair::universal
