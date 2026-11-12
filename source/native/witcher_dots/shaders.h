#pragma once
#include <Windows.h>
#include <objbase.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace witcher_dots {
// PathTracer: a library translated only for the opt-in hair translucency (see
// SetHairTranslucency): the game's path tracer and its sun and local-light
// shadow passes.
enum class ShaderKind { ClosestHit, Prepass, PathTracer };
// Value names (named form: %rN, see TranslateIr) the translation edits in one
// build of the game's hair shaders. ClosestHitHair: LSS hit attributes and the
// NVIDIA-extension endpoint block; PTHairPrepassRGS: the ray query's LSS hit
// test, the two strand-u sequences and the endpoint block. Both: the object-
// space normal (three values) and the instruction the rounded normal precedes.
struct ClosestAnchors {
    const char *prim,*attrLoad,*attrU,*endpointsFrom,*endpointsTo,*index;
    std::array<int,8> extracts; std::array<int,6> ray; // ray: origin xyz, direction xyz
    const char *nonuniform,*noalias;
    std::array<int,3> normals; const char* normalMarker;
};
struct PrepassAnchors {
    const char *query,*candidateType,*hitFrom,*hitBranch,*alphaPrim,*alphaUFrom,*alphaUTo,*alphaU;
    const char *mainUFrom,*mainUTo,*mainPrim,*mainU,*instance,*index,*endpointsFrom,*endpointsTo;
    std::array<int,8> extracts; std::array<int,6> ray;
    const char *nonuniform,*noalias;
    std::array<int,3> normals; const char* normalMarker;
};
// Where a shader reads the hair hit distance, for the round-strand distance
// (see SetRoundHitDistance). ClosestHitHair stores RayTCurrent in its payload;
// PTHairPrepassRGS reads CommittedRayT once and derives the world offset
// (direction * t) and position (camera + offset), the object-space hit
// (ray origin + ray direction * t) and its offset from the segment start.
struct ClosestShellAnchors { const char* payloadT; };
struct PrepassShellAnchors {
    const char* hitT; std::array<int,3> worldDir,camera,worldOffset,worldPos,objectHit,objectBase,objectOffset;
};
// Hair translucency (issue 70, beyond the game's own look; ini default 65): a ray that
// starts on hair skips hair instances (instance mask bit 0x80, hair only) with
// probability HairTranslucency() %. A site is one TraceRay of a known library
// (exact handle, flags and mask, named form) and how its origin is known to be
// hair: always (the hair prepass), the path tracer's loop flag for the current
// vertex, or bit 17 of the bounce hit's payload word (set by ClosestHitHair)
// for the new vertex, or the G-buffer material word of the pixel (hair: class 3
// with bit 8, word & 15 == 11). `random` is the shader's own random state; when
// that does not dominate the TraceRay, `seed` (IR, `$` = the site's value
// prefix) computes one from values that do, and `random` names it (`$.name`).
enum class HairOrigin { Always, LoopFlag, PayloadBit, MaterialWord };
// Colour-aware translucency (HairTranslucencyByColour): the coin's probability
// is scaled by min(1, Y / white point), Y the linear luminance of the hair the
// ray starts on. `colourValue`: a float luminance already computed (Luminance,
// e.g. the loop phi), the hair material colour as three values (Material,
// "r,g,b"; linear = m * 0.0466), a GBuffer2 load (GBufferLoad; its .xyz are
// gamma encoded, linear ~ c * c) or three linear values (Linear, "r,g,b").
// Material and GBufferLoad are the hair's specular colour (about
// 0.047 for every hair), so the anchored sites use its albedo (Linear and the
// loop phi); the shadow passes bind no albedo and stay colour-blind (None).
enum class HairColour { None, Luminance, Material, GBufferLoad, Linear };
struct TranslucencySite {
    const char *handle,*flags,*mask,*masked; HairOrigin origin; const char *hair,*random,*seed{};
    HairColour colour{}; const char* colourValue{};
};
// A path tracer loop (colour-aware): in function `function`, the loop header,
// preheader and latch blocks, the hair pixel's linear colour ("r,g,b") and the
// bounce payload (<4 x i32>). New named values: %dots.hp (pixel luminance),
// %dots.hh (the bounce hit's luminance, payload word 0 .x RGB8 gamma encoded)
// and the header phi %dots.hl (the current vertex: pixel, then the last hit).
struct TranslucencyLoop { const char *function,*header,*preheader,*latch,*pixel,*payload; };
struct TranslucencyAnchors { const TranslucencySite* sites; size_t count; const TranslucencyLoop* loops{}; size_t loopCount{}; };
// HairBounces: the path tracer's loop trip count for hair pixels (the game's
// select(hair pixel, 4, 2)), one exact line per function (original numbering).
struct BounceSite { const char *function,*line; };
struct BounceAnchors { const BounceSite* sites; size_t count; };
// HairAlbedoLinear: the hair colour interpolated from HairAlbedoBuffer (named
// form, red, green, blue) in the prepass and in ClosestHitHair.
struct AlbedoAnchors { std::array<const char*,3> values; };
// HairIgnoreReflectionMask: a load of the path tracer's ReflectionsTexture at
// the pixel (the RT-reflection mask). `line`: the exact start of the line that
// first uses `operand` (the loaded value); `hair`: the function's hair flag.
struct MaskSite { const char *function,*line,*operand,*hair; };
struct MaskAnchors { const MaskSite* sites; size_t count; };
// HairDebugLighting (diagnostic): exact prepass lines (named form) replaced in
// a mode, and the path tracer functions' hair flags for mode 3.
struct DebugLine { uint32_t mode; const char *from,*to; };
struct DebugAnchors { const DebugLine* lines; size_t count; };
struct HairFlagSite { const char *function,*hair; };
struct HairFlagAnchors { const HairFlagSite* sites; size_t count; };
// HairStencilCharacter: the path tracer's StencilMapTexture word at the pixel.
// `line`: the exact start of its bit-2 test; `word`: the loaded word; `hair`:
// the function's hair flag.
struct StencilSite { const char *function,*line,*word,*hair; };
struct StencilAnchors { const StencilSite* sites; size_t count; };
// A known game hair shader: exact kind, size and SHA-256, and its anchors.
// Builds without hit-distance anchors keep the ribbon hit distance.
struct ShaderIdentity {
    ShaderKind kind; uint32_t size; const char* sha256; const ClosestAnchors* closest; const PrepassAnchors* prepass;
    const ClosestShellAnchors* closestShell{}; const PrepassShellAnchors* prepassShell{};
    const TranslucencyAnchors* translucency{}; const char* label{}; const BounceAnchors* bounces{};
    const AlbedoAnchors* albedo{}; const MaskAnchors* masks{};
    const DebugAnchors* debug{}; const HairFlagAnchors* hairFlags{}; const StencilAnchors* stencil{};
};
std::span<const ShaderIdentity> KnownShaders();
const ShaderIdentity* FindShader(std::span<const std::byte> bytes, ShaderKind kind);
bool TranslateIr(std::string_view original, const ShaderIdentity& shader, std::string& translated, std::string& error);
// rtxhair.1 universal hair in Witcher 3: the look fixes of this file (HairAlbedoLinear,
// HairDebugLighting, hair translucency in the prepass) on a hair shader whose NVAPI LSS
// ops rtx_hair already rewrote, keeping the original value numbering so the anchors
// hold. `rewritten`: that shader's disassembly; `translated`: the named form with the
// fixes; `applied`: which ones ("" when none is on). False with `error` on any anchor
// mismatch (the caller keeps the rewrite without them). The path tracer libraries take
// TranslateIr unchanged (they have no LSS op).
bool ApplyHairLook(std::string_view rewritten, const ShaderIdentity& shader, std::string& translated, std::string& applied, std::string& error);
// GitHub issue 70: report hair hits on a round strand 1.12x the LSS radius,
// which encloses the crossed-ribbon proxy, instead of on a ribbon. Shading
// points and ray origins on a ribbon lie inside the strand, and its own
// ribbons then shadow them (hair far too dark in shade). On by default;
// RoundHitDistance=0 in RTXMFG.WitcherDOTS.ini keeps the ribbon distance.
// Read by TranslateIr: set before ShaderCache::Prepare.
void SetRoundHitDistance(bool enabled) noexcept;
bool RoundHitDistance() noexcept;
bool RoundHitDistanceSupported(const ShaderIdentity& shader) noexcept;
// HairTranslucency=0..100 in RTXMFG.WitcherDOTS.ini (ini default 65; 0: nothing
// is rewritten). Read by TranslateIr: set before ShaderCache::Prepare.
void SetHairTranslucency(uint32_t percent) noexcept;
uint32_t HairTranslucency() noexcept;
// HairTranslucencyByColour (default 1) and HairTranslucencyWhitePoint (5-100 %,
// default 35): read by TranslateIr like HairTranslucency.
void SetHairTranslucencyColour(bool byColour, uint32_t whitePoint) noexcept;
bool HairTranslucencyByColour() noexcept;
uint32_t HairTranslucencyWhitePoint() noexcept;
// HairBounces=1..32 (default 4: the game's path length for hair pixels).
void SetHairBounces(uint32_t bounces) noexcept;
uint32_t HairBounces() noexcept;
// A translucency-only library has work: translucency on and it has sites, or
// HairBounces changed and it has bounce anchors.
bool TranslationWanted(const ShaderIdentity& shader) noexcept;
// HairAlbedoLinear=1 in RTXMFG.WitcherDOTS.ini (the ini default): treat
// HairAlbedoBuffer as linear colour. The game decodes it with pow 2.2 wherever
// it is used, so the prepass and ClosestHitHair gamma-encode it once where it is formed (anchored
// builds only). Read by TranslateIr: set before ShaderCache::Prepare.
void SetHairAlbedoLinear(bool linear) noexcept;
bool HairAlbedoLinear() noexcept;
// HairIgnoreReflectionMask=1 in RTXMFG.WitcherDOTS.ini: hair pixels read the
// path tracer's RT-reflection mask as 0. The hair prepass does not write
// ReflectionsTexture, so hair pixels otherwise inherit the mask of the surface
// behind them. Read by TranslateIr: set before ShaderCache::Prepare.
void SetHairIgnoreReflectionMask(bool ignore) noexcept;
bool HairIgnoreReflectionMask() noexcept;
// HairDebugLighting=0..5 in RTXMFG.WitcherDOTS.ini (diagnostic, default 0):
// 1 prepass local-light term off, 2 prepass sun term off, 3 path tracer output
// zeroed on hair pixels, 4 prepass all-lights loop, 5 prepass one random light.
void SetHairDebugLighting(uint32_t mode) noexcept;
uint32_t HairDebugLighting() noexcept;
// HairStencilCharacter=1 in RTXMFG.WitcherDOTS.ini: the path tracer sees stencil
// bit 2 on hair pixels. The hair prepass does not write the stencil, so hair
// pixels otherwise inherit the bits of the surface behind them. Read by
// TranslateIr: set before ShaderCache::Prepare.
void SetHairStencilCharacter(bool enabled) noexcept;
bool HairStencilCharacter() noexcept;
std::array<uint8_t,32> Sha256(std::span<const std::byte> bytes);
bool HashEquals(std::span<const std::byte> bytes, const char* expected);
class ShaderCache {
public:
    // Explicit game-side DXC; never distribute or overwrite its compiler.
    bool Initialize(const std::wstring& directory, std::string& error);
    bool Translate(std::span<const std::byte> source, ShaderKind kind, std::vector<std::byte>& output, std::string& error);
    bool CompileConverter(std::vector<std::byte>& output, std::string& error);
    bool CompileProgram(std::string_view source, const wchar_t* target, std::vector<std::byte>& output, std::string& error);
    // `rvas`: the build's ClosestHitHair copies, then its PTHairPrepassRGS copies.
    bool Prepare(HMODULE game, const std::array<uint32_t,4>& rvas, const std::wstring& directory, std::string& error);
    std::span<const std::byte> Replacement(const void* data, size_t size) const noexcept;
    // Both prepared shaders were translated with the round-strand hit distance.
    bool RoundHitApplied() const noexcept {
        return closestApplied_&&prepassApplied_;
    }
    // The prepass was translated with hair translucency on (its build anchored).
    bool TranslucencyApplied() const noexcept { return prepassTranslucent_; }
    // Both prepared shaders were translated with the linear hair albedo.
    bool AlbedoApplied() const noexcept { return closestAlbedo_&&prepassAlbedo_; }
    // The translucency-only libraries (path tracer, shadow passes) translated
    // for hair translucency when it is on (PathTracerState says which).
    bool PathTracerReady() const noexcept { return !translucent_.empty(); }
    size_t TranslucentLibraries() const noexcept { return translucent_.size(); }
    const std::string& PathTracerState() const noexcept { return pathTracerState_; }
    ShaderKind ReplacementKind(std::span<const std::byte> replacement) const noexcept {
        if(replacement.data()==closest_.data())return ShaderKind::ClosestHit;
        for(const auto& library:translucent_)if(replacement.data()==library.bytes.data())return ShaderKind::PathTracer;
        return ShaderKind::Prepass;
    }
    const std::vector<std::byte>& Converter() const noexcept { return converter_; }
    bool Ready() const noexcept { return !closest_.empty()&&!prepass_.empty()&&!converter_.empty(); }
private:
    bool Disassemble(std::span<const std::byte> source, std::string& text, std::string& error);
    bool Validate(IDxcBlob* blob, std::vector<std::byte>& output, std::string& error);
    void PreparePathTracer(HMODULE game);
    std::string PrepareTranslucent(HMODULE game, const ShaderIdentity& shader);
    HMODULE compilerModule_{}, validatorModule_{};
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler_;
    Microsoft::WRL::ComPtr<IDxcLibrary> library_;
    Microsoft::WRL::ComPtr<IDxcAssembler> assembler_;
    Microsoft::WRL::ComPtr<IDxcOptimizer> optimizer_;
    Microsoft::WRL::ComPtr<IDxcValidator> validator_;
    std::array<const void*,4> originals_{};
    const ShaderIdentity *closestIdentity_{},*prepassIdentity_{};
    std::vector<std::byte> closest_,prepass_,converter_;
    struct Translated { const ShaderIdentity* identity; std::vector<std::byte> bytes; };
    std::vector<Translated> translucent_;
    bool closestApplied_{},prepassApplied_{},prepassTranslucent_{},closestAlbedo_{},prepassAlbedo_{};
    std::string pathTracerState_="off";
};
}
