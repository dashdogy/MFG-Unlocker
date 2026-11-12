#pragma once
#include "dlss5.h"
#include <dxgiformat.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// dx11nr.1: what the DirectX 11 bridge (dlss5_d3d11.cpp) decides without a device. No D3D11 or D3D12 calls.
namespace dlss5::d3d11 {
// Refusal and Blocking (status "dlss5D3D11Refusal") are in dlss5_policy.h, which the menu reads.

// The typed member of a DLSS color/output format's family, so the copy between the game's texture and the shared one
// is legal and the D3D12 stage can view and write it; UNKNOWN for formats the bridge refuses (sRGB views included).
constexpr DXGI_FORMAT ColorFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// A game depth buffer: `copy`, the typeless family of the bridge's own copy of it (never bound as a depth target),
// and `view`, the shader view of its depth the conversion to the shared R32_FLOAT texture reads.
struct DepthFormats { DXGI_FORMAT copy = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN; };
constexpr DepthFormats DepthFormatsFor(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
        return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
    case DXGI_FORMAT_R16_FLOAT: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT};
    default: return {};
    }
}
// Motion vectors the D3D12 stage reads (dlss5.cpp MotionCaptureFormat): two- or four-channel 16/32-bit float, as
// the shared texture's typed format; UNKNOWN refuses.
constexpr DXGI_FORMAT MotionFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
// A 1x1 exposure copy the D3D12 stage can view; UNKNOWN leaves the exposure out (as an unreadable one on D3D12).
constexpr DXGI_FORMAT ExposureFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// Every evaluation key the D3D12 stage reads besides its resources (dlss5.cpp ReadFrame), read as ReadFrame does. The
// shared textures hold whole game textures (mip 0), so every subrect stays valid as the game set it.
enum class KeyKind : uint8_t { eUnsigned, eFloat };
struct ScalarKey { const char* name; KeyKind kind; };
inline constexpr ScalarKey kScalarKeys[] = {
    {"DLSS.Output.Subrect.Base.X", KeyKind::eUnsigned}, {"DLSS.Output.Subrect.Base.Y", KeyKind::eUnsigned},
    {"DLSS.Input.Color.Subrect.Base.X", KeyKind::eUnsigned}, {"DLSS.Input.Color.Subrect.Base.Y", KeyKind::eUnsigned},
    {"DLSS.Input.Depth.Subrect.Base.X", KeyKind::eUnsigned}, {"DLSS.Input.Depth.Subrect.Base.Y", KeyKind::eUnsigned},
    {"DLSS.Input.MV.Subrect.Base.X", KeyKind::eUnsigned}, {"DLSS.Input.MV.Subrect.Base.Y", KeyKind::eUnsigned},
    {"DLSS.Render.Subrect.Dimensions.Width", KeyKind::eUnsigned}, {"DLSS.Render.Subrect.Dimensions.Height", KeyKind::eUnsigned},
    {"Reset", KeyKind::eUnsigned},
    {"MV.Scale.X", KeyKind::eFloat}, {"MV.Scale.Y", KeyKind::eFloat},
    {"DLSS.Exposure.Scale", KeyKind::eFloat}, {"DLSS.Pre.Exposure", KeyKind::eFloat},
};
// The resources it reads; the bridge sets the shared D3D12 textures under these names.
inline constexpr const char* kResourceKeys[] = {"Color", "Output", "Depth", "MotionVectors", "ExposureTexture"};

// The bridge never tries the direct provider once native NGX reported an outdated driver (0xBAD0000C): a direct
// create after that hung an RTX 2080 Ti on driver 576.88. Every other route and result keeps dlss5.cpp's fallback.
constexpr bool DirectFallbackAllowed(size_t route, uint32_t nativeResult) noexcept {
    return !(route == kD3D11Route && nativeResult == kResultOutOfDate);
}
// The bridge device's own NGX initialization: any failure stops the bridge for the process, since both the native
// create and the direct provider need NGX on that device.
// NGX success is NVSDK_NGX_Result_Success (0x1), not 0; anything else, an unknown value included, blocks.
constexpr bool InitBlocks(uint32_t initResult) noexcept
{
    return initResult != static_cast<uint32_t>(NVSDK_NGX_Result_Success);
}

// The parameter object the bridge hands the D3D12 stage: the game's D3D11 block holds ID3D11Resource*, which the
// D3D12 stage must never see. A key keeps each scalar type it was Set with (a mirror of what the game's block
// returned for that type) and one resource pointer: Get(ID3D12Resource**) and Get(ID3D11Resource**) return only
// pointers Set with that type, Get(void**) any. Fixed storage, no allocation; names longer than 63 bytes and keys
// beyond kEntries are dropped.
class Block final : public NVSDK_NGX_Parameter {
public:
    static constexpr size_t kEntries = 32, kName = 64;
    void Set(const char* name, unsigned long long value) override { if (Entry* e = Find(name, true)) { e->u64 = value; e->kinds |= kU64; } }
    void Set(const char* name, float value) override { if (Entry* e = Find(name, true)) { e->f32 = value; e->kinds |= kF32; } }
    void Set(const char* name, double value) override { if (Entry* e = Find(name, true)) { e->f64 = value; e->kinds |= kF64; } }
    void Set(const char* name, unsigned int value) override { if (Entry* e = Find(name, true)) { e->u32 = value; e->kinds |= kU32; } }
    void Set(const char* name, int value) override { if (Entry* e = Find(name, true)) { e->i32 = value; e->kinds |= kI32; } }
    void Set(const char* name, ID3D11Resource* value) override { Point(name, value, kD3D11); }
    void Set(const char* name, ID3D12Resource* value) override { Point(name, value, kD3D12); }
    void Set(const char* name, void* value) override { Point(name, value, kPointer); }
    NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return Read(name, kU64, out, &Entry::u64); }
    NVSDK_NGX_Result Get(const char* name, float* out) const override { return Read(name, kF32, out, &Entry::f32); }
    NVSDK_NGX_Result Get(const char* name, double* out) const override { return Read(name, kF64, out, &Entry::f64); }
    NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override { return Read(name, kU32, out, &Entry::u32); }
    NVSDK_NGX_Result Get(const char* name, int* out) const override { return Read(name, kI32, out, &Entry::i32); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return Pointer(name, kD3D11, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return Pointer(name, kD3D12, out); }
    NVSDK_NGX_Result Get(const char* name, void** out) const override { return Pointer(name, kD3D11 | kD3D12 | kPointer, out); }
    void Reset() override { entries_ = {}; count_ = 0; }
    // The D3D12 resource Set under `name` (null when none).
    ID3D12Resource* Resource12(const char* name) const noexcept {
        ID3D12Resource* value = nullptr;
        return Get(name, &value) == NVSDK_NGX_Result_Success ? value : nullptr;
    }
    size_t Count() const noexcept { return count_; }
private:
    enum : uint8_t { kU64 = 1, kF32 = 2, kF64 = 4, kU32 = 8, kI32 = 16, kD3D11 = 32, kD3D12 = 64, kPointer = 128 };
    struct Entry {
        char name[kName]{};
        uint8_t kinds = 0;
        unsigned long long u64 = 0; float f32 = 0.0f; double f64 = 0.0; unsigned int u32 = 0; int i32 = 0;
        void* pointer = nullptr;
    };
    Entry* Find(const char* name, bool add) noexcept {
        if (!name) return nullptr;
        const size_t length = strnlen(name, kName);
        if (length == kName) return nullptr;
        for (size_t n = 0; n != count_; ++n) if (!std::strcmp(entries_[n].name, name)) return &entries_[n];
        if (!add || count_ == kEntries) return nullptr;
        Entry& entry = entries_[count_++];
        std::memcpy(entry.name, name, length + 1);
        return &entry;
    }
    const Entry* Find(const char* name) const noexcept { return const_cast<Block*>(this)->Find(name, false); }
    void Point(const char* name, void* value, uint8_t kind) noexcept {
        if (Entry* e = Find(name, true)) { e->pointer = value; e->kinds = static_cast<uint8_t>((e->kinds & ~(kD3D11 | kD3D12 | kPointer)) | kind); }
    }
    template <class T> NVSDK_NGX_Result Read(const char* name, uint8_t kind, T* out, T Entry::* member) const noexcept {
        const Entry* e = Find(name);
        if (!out || !e || !(e->kinds & kind)) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = e->*member;
        return NVSDK_NGX_Result_Success;
    }
    template <class T> NVSDK_NGX_Result Pointer(const char* name, uint8_t kinds, T** out) const noexcept {
        const Entry* e = Find(name);
        if (!out || !e || !(e->kinds & kinds)) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = static_cast<T*>(e->pointer);
        return NVSDK_NGX_Result_Success;
    }
    std::array<Entry, kEntries> entries_{};
    size_t count_ = 0;
};

// kScalarKeys from the game's block into `block`, each with the types ReadFrame reads it with (absent keys stay
// absent, so the D3D12 stage applies the same SDK defaults). A faulting game block leaves the key out.
inline void CopyScalars(const NVSDK_NGX_Parameter* game, Block& block) noexcept {
    if (!game) return;
    for (const ScalarKey& key : kScalarKeys) {
        unsigned int unsignedValue = 0;
        int signedValue = 0;
        float realValue = 0.0f;
        bool hasUnsigned = false, hasSigned = false, hasReal = false;
        __try {
            if (key.kind == KeyKind::eUnsigned) {
                hasUnsigned = game->Get(key.name, &unsignedValue) == NVSDK_NGX_Result_Success;
                hasSigned = game->Get(key.name, &signedValue) == NVSDK_NGX_Result_Success;
            } else hasReal = game->Get(key.name, &realValue) == NVSDK_NGX_Result_Success;
        } __except (EXCEPTION_EXECUTE_HANDLER) { hasUnsigned = hasSigned = hasReal = false; }
        if (hasUnsigned) block.Set(key.name, unsignedValue);
        if (hasSigned) block.Set(key.name, signedValue);
        if (hasReal) block.Set(key.name, realValue);
    }
}
}
