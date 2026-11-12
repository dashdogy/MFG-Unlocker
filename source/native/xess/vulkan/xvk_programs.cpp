// Root signature (RTS0) parsing and the embedded SPIR-V program set (generate_xvk_programs.py).
#include "xvk_internal.h"
#include "embedded_xvk_programs.generated.h"
#include "xvk_programs.packed.h"
#include <set>

namespace xvk {

namespace {

template <class T>
bool Read(const uint8_t* data, size_t size, size_t offset, T& out)
{
    if (offset > size || size - offset < sizeof(T)) return false;
    memcpy(&out, data + offset, sizeof(T));
    return true;
}

bool FindPart(const uint8_t* data, size_t size, uint32_t fourcc, const uint8_t*& part, size_t& partSize)
{
    uint32_t magic = 0, count = 0;
    if (!Read(data, size, 0, magic) || magic != 0x43425844 /* DXBC */) return false;
    if (!Read(data, size, 28, count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset = 0, code = 0, length = 0;
        if (!Read(data, size, 32 + 4 * i, offset) || !Read(data, size, offset, code) || !Read(data, size, offset + 4, length)) return false;
        if (code != fourcc) continue;
        if (offset + 8 > size || size - offset - 8 < length) return false;
        part = data + offset + 8;
        partSize = length;
        return true;
    }
    return false;
}

} // namespace

bool ParseRootSignature(const void* blob, size_t size, RootLayout& out, std::string& error)
{
    const auto* data = static_cast<const uint8_t*>(blob);
    const uint8_t* rts = data;
    size_t rtsSize = size;
    if (size >= 4 && memcmp(data, "DXBC", 4) == 0 && !FindPart(data, size, 0x30535452 /* RTS0 */, rts, rtsSize)) {
        error = "container has no RTS0 part";
        return false;
    }
    uint32_t version = 0, parameterCount = 0, parameterOffset = 0, samplerCount = 0, samplerOffset = 0, flags = 0;
    if (!Read(rts, rtsSize, 0, version) || !Read(rts, rtsSize, 4, parameterCount) || !Read(rts, rtsSize, 8, parameterOffset) ||
        !Read(rts, rtsSize, 12, samplerCount) || !Read(rts, rtsSize, 16, samplerOffset) || !Read(rts, rtsSize, 20, flags)) {
        error = "truncated RTS0 header";
        return false;
    }
    if (version < 1 || version > 3) { error = "unsupported RTS0 version " + std::to_string(version); return false; }
    out = {};
    for (uint32_t i = 0; i < parameterCount; ++i) {
        uint32_t type = 0, visibility = 0, payload = 0;
        if (!Read(rts, rtsSize, parameterOffset + 12 * i, type) || !Read(rts, rtsSize, parameterOffset + 12 * i + 4, visibility) ||
            !Read(rts, rtsSize, parameterOffset + 12 * i + 8, payload)) { error = "truncated parameter"; return false; }
        RootParameter parameter;
        parameter.type = D3D12_ROOT_PARAMETER_TYPE(type);
        if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            uint32_t rangeCount = 0, rangeOffset = 0;
            if (!Read(rts, rtsSize, payload, rangeCount) || !Read(rts, rtsSize, payload + 4, rangeOffset)) { error = "truncated table"; return false; }
            const uint32_t stride = version == 1 ? 20 : 24;
            uint32_t running = 0;
            for (uint32_t r = 0; r < rangeCount; ++r) {
                const size_t at = rangeOffset + size_t(stride) * r;
                uint32_t rangeType = 0, count = 0, base = 0, space = 0, offset = 0;
                if (!Read(rts, rtsSize, at, rangeType) || !Read(rts, rtsSize, at + 4, count) || !Read(rts, rtsSize, at + 8, base) ||
                    !Read(rts, rtsSize, at + 12, space) || !Read(rts, rtsSize, at + (version == 1 ? 16 : 20), offset)) {
                    error = "truncated range";
                    return false;
                }
                if (offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) offset = running;
                if (count == UINT_MAX) { error = "unbounded range"; return false; }
                parameter.ranges.push_back({D3D12_DESCRIPTOR_RANGE_TYPE(rangeType), base, count, space, offset});
                running = offset + count;
            }
        } else if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (!Read(rts, rtsSize, payload, parameter.shaderRegister) || !Read(rts, rtsSize, payload + 4, parameter.space) ||
                !Read(rts, rtsSize, payload + 8, parameter.num32)) { error = "truncated constants"; return false; }
        } else {
            if (!Read(rts, rtsSize, payload, parameter.shaderRegister) || !Read(rts, rtsSize, payload + 4, parameter.space)) {
                error = "truncated root descriptor";
                return false;
            }
        }
        out.parameters.push_back(std::move(parameter));
    }
    const uint32_t samplerStride = version == 3 ? 56 : 52;
    for (uint32_t i = 0; i < samplerCount; ++i) {
        StaticSampler sampler{};
        if (samplerOffset + size_t(samplerStride) * (i + 1) > rtsSize) { error = "truncated static sampler"; return false; }
        memcpy(&sampler.desc, rts + samplerOffset + size_t(samplerStride) * i, sizeof(D3D12_STATIC_SAMPLER_DESC));
        out.samplers.push_back(sampler);
    }
    return true;
}

void ProgramSet::Load(HMODULE module)
{
    module_ = module;
    for (size_t i = 0; i < std::size(embedded::programs); ++i) index_.emplace(embedded::programs[i].dxilSha256, i);
    Log("program set: %zu embedded programs, manifest %.16s", index_.size(), embedded::manifestSha256);
}

namespace {
// RTXMFG's own XeFG shaders (xess/generate_xess_shaders.py, the pinned SHA-256 of each compiled container) replace
// exactly one Intel program each (xess/runtime_payload.cpp LoadReplacements) and were proven to produce
// byte-identical results with the same root signature, so each runs as the SPIR-V of the program it replaces.
constexpr std::pair<const char*, const char*> kReplacements[] = {
    {"43ccfe670409fd807b604b870674012958fbb4067305a449304ee91ab6154f3f", "3f3f91d2ecd4345042f0519f7455776314be9e7730453a3fcb42c57f0acb49b5"},
    {"6c9bf4b59956c7fec3d764ffe6520e7aec3e6137553b9fca0ba65ecdd7642ce8", "c3649191906bdd300462dbafe6add8f82b09e8e10c41e92784a209d99513224b"},
    {"9900c30febf95bfa81b4da1178e5773863116beba4d3f032897da4254b6d8926", "01c8beac27d3f3f82ecba8f3dc2d9d8b60d4c175a41df3c24a24e8643f5578f5"},
    {"e78063f97ad2c0913412f07589cd17d0f2a6ebeeb6edd70647a69cca6b6a02bf", "8291bbc36b1100b07d0788f35a3a9fadfbfcc2267ff0ccb7afb114d76f5d69fe"},
    {"7a29fbf311cd8dee5b8c1cfdadf31ee983092a512cf9c10845a079b457eb62a9", "a60f7488872a675d66afd2bec02b0b89e3cc38c1c4ed1d25d745a7b2fe677412"},
    {"ce847b58086ce7e9d25f7d8b90e989247cd4582226506d9552112f1ef4130643", "8c2efb9e955f39379a91887ed3d90412c4b7ae2c142a874e3da5f699de197f7c"},
    {"efa8da5ef5f90c62b789b0fed8e77e2ce043e558d164cc552ca7fb9127cedb00", "40e159414d5b839ba7e0e07cbf8ff4ef425ee026c216b2928ef3bb6dd0e20d40"}};
}

const Program* ProgramSet::Find(const std::string& requested)
{
    std::string sha256 = requested;
    if (index_.find(sha256) == index_.end())
        for (const auto& [ours, intel] : kReplacements)
            if (sha256 == ours) { sha256 = intel; break; }
    std::lock_guard lock(mutex_);
    if (auto it = loaded_.find(sha256); it != loaded_.end()) return it->second.get();
    auto row = index_.find(sha256);
    if (row == index_.end()) return nullptr;
    const embedded::ProgramAsset& asset = embedded::programs[row->second];
    std::unique_ptr<Program> program;
    const auto payload = embedded_payload::Find(module_, embedded_payload::packed::xvk_programs,
        embedded_payload::packed::xvk_programs_items, asset.resource);
    const auto* bytes = payload.data();
    const DWORD size = static_cast<DWORD>(payload.size());
    if (!bytes || size != asset.spirvBytes || size % 4 || Sha256(bytes, size) != asset.spirvSha256) {
        Log("program set: embedded program %.16s failed verification (resource %u, %lu bytes)", asset.dxilSha256, asset.resource, size);
    } else {
        program = std::make_unique<Program>();
        program->sha256 = asset.dxilSha256;
        program->spirv.resize(size / 4);
        memcpy(program->spirv.data(), bytes, size);
        program->pushConstantBytes = asset.pushConstantBytes;
        static constexpr VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
        for (uint32_t i = 0; i < asset.bindingCount; ++i) {
            const embedded::BindingRow& b = embedded::bindings[asset.firstBinding + i];
            program->bindings.push_back({b.binding, types[b.type]});
        }
    }
    return (loaded_[sha256] = std::move(program)).get();
}

namespace {
std::atomic<uint64_t> gMissing{0};
}

void NoteMissingProgram(const std::string& sha256, const void* dxil, size_t bytes)
{
    ++gMissing;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        std::lock_guard lock(mutex);
        if (seen.size() > 256 || !seen.insert(sha256).second) return;
    }
    Log("CreateComputePipelineState: no SPIR-V for program %s (%zu bytes); failing closed", sha256.c_str(), bytes);
    (void)dxil;
}

size_t EmbeddedProgramCount(HMODULE) noexcept { return std::size(embedded::programs); }
uint64_t MissingProgramCount() noexcept { return gMissing.load(); }

} // namespace xvk
