// RtxHairVkProbe: universal RTX hair on Vulkan, phases 0, 1a and 1b.
//
// The Vulkan twin of tests/rtxhair_probe. Universal hair rewrites a game's LSS shaders (SPV_NV_linear_swept_spheres)
// to read the hit triangle's own vertices (VK_KHR_ray_tracing_position_fetch) and decode the segment from them, with no
// game buffer, descriptor or per-game anchor. Phase 0 asks whether position fetch works here through Vulkan, and
// whether a DOTS segment (rtx_hair/dots_converter.hlsl) decodes back to its endpoints and radii from that triangle.
//
// RtxHairVkProbe [--out <report.json>] [--dump <dir>] [--adapter <index>] [--seed <n>] [--validation]
//
// Scene (the D3D12 probe's, same seed): synthetic LSS strands (tapered to a zero tip, constant, thinning,
// growing) converted to DOTS on the CPU, and a game-like indexed grid. Five BLASes in one TLAS, each
// reached by its own instance mask:
//   0x01 DOTS, tagged           ALLOW_DATA_ACCESS; 3 low mantissa bits of every coordinate set to the tag
//                               (the "is LSS" marker, rtx_hair/lss_ir.h kTagMask / kTagValue)
//   0x08 DOTS, plain            ALLOW_DATA_ACCESS, untagged (decode error without the tag)
//   0x04 mesh, data access      the grid, ALLOW_DATA_ACCESS
//   0x10 DOTS, tagged, updated  ALLOW_DATA_ACCESS | ALLOW_UPDATE: built from the strands, then updated in
//                               place to moved strands (games refit hair every frame)
//   0x02 mesh, plain            the grid without ALLOW_DATA_ACCESS (a game's own BLAS). Vulkan leaves
//                               position fetch undefined there; traced last, in dispatches of its own,
//                               and reported only (universal hair adds the flag to every BLAS instead)
// Rays are traced twice: a compute shader's ray query (OpRayQueryGetIntersectionTriangleVertexPositionsKHR,
// committed) and a ray tracing pipeline's closest hit (BuiltIn HitTriangleVertexPositionsKHR).
// Also reported: whether the driver lists VK_NV_ray_tracing_linear_swept_spheres, what it writes into a
// chained VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV, and each BLAS's size with and without
// ALLOW_DATA_ACCESS.
//
// Phase 1a (vkhair.1): probe_lss.comp and probe_lss.rchit, hair shaders written for RTX 50 (GL_NV_linear_swept_spheres),
// rewritten by source/native/rtx_hair/lss_spirv and traced on the same scene both ways. On tagged DOTS (also after the
// update) they must report LSS hits on the segment; everywhere else plain triangle hits. --dump writes the SPIR-V.
//
// Phase 1b (vkhair.3): the strands as Vulkan LSS geometry in four layouts, built through
// source/native/rtx_hair/vk_dots_builder under a TLAS with culling left on that the builder's instance patch must fix,
// traced again by the rewritten shaders; the probe's compute pipeline must be bound after each build.
//
// --game (vkhair.4): the probe acts as an RTX 50 game. It needs the LSS extension and feature reported,
// creates its device with them (not with position fetch), creates phase 1a's LSS shaders as written and builds phase
// 1b's LSS geometry with plain Vulkan calls: MFGUnlocker.dll, beside the probe as winmm.dll, must do every conversion
// through its hooks. Without the DLL it exits 3. The builder's own counters and instance copy are not checked then.
//
// --feature-only (vkhairchain.1): as --game, but the device chains the LSS feature and lists neither its extension nor
// position fetch, as Indiana Jones does. The DLL must still add position fetch and do every conversion.
//
// Exit codes: 0 pass; 1 a check failed (the report says which); 2 setup failure;
// 3 position fetch unavailable (extension or feature missing, or neither pipeline could be created).
#include <cstdint>

#include "rtx_hair/lss_ir.h"
#include "rtx_hair/lss_spirv.h"

#include "probe_comp.h"
#include "probe_lss_comp.h"
#include "probe_lss_rchit.h"
#include "probe_lss_rgen.h"
#include "probe_lss_rmiss.h"
#include "probe_rchit.h"
#include "probe_rgen.h"
#include "probe_rmiss.h"

#include <Windows.h>
#include <mmsystem.h>
#include <vulkan/vulkan.h>

#include "rtx_hair/vk_dots_builder.h" // after vulkan.h: it asks for VK_NO_PROTOTYPES, which this probe does not use

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kTagMask = rtx_hair::kTagMask, kTagValue = rtx_hair::kTagValue;
constexpr float kRibbon = rtx_hair::kRibbon;                     // pi/3: three ribbons per segment
constexpr uint32_t kTriangles = rtx_hair::kTrianglesPerSegment;  // 6: three ribbons, one winding
constexpr uint32_t kVertices = kTriangles * 3;
constexpr uint32_t kStrands = 64, kSegmentsPerStrand = 8, kGrid = 32;
static_assert(kTriangles == 6, "probe_common.glsl's DotsDecode assumes one winding (even/odd triangles)");

[[noreturn]] void Fail(int code, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "RtxHairVkProbe: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::exit(code);
}

#define CHECK_VK(expr) \
    do { const VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) Fail(2, "%s failed: %d (line %d)", #expr, static_cast<int>(vr_), __LINE__); } while (0)

struct F3 { float x{}, y{}, z{}; };
F3 operator+(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
F3 operator-(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
F3 operator*(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
F3 Cross(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(F3 a) { return std::sqrt(Dot(a, a)); }
F3 Normalize(F3 a) { const float l = Length(a); return l > 0 ? a * (1.0f / l) : F3{}; }
struct F4 { F3 xyz; float w{}; };

// probe_common.glsl's RayIn and HitOut (std430).
struct RayIn { F3 origin; float tmax{}; F3 direction; uint32_t mask{}; };
static_assert(sizeof(RayIn) == 32);
struct HitOut
{
    uint32_t status, instanceId, primitive, geometry;
    float bary[2], t;
    uint32_t hitKind;
    F3 v0; uint32_t tagged;
    F3 v1; uint32_t pad1;
    F3 v2; uint32_t pad2;
    float p[4], q[4];
};
static_assert(sizeof(HitOut) == 112);
constexpr uint32_t kUnwritten = 0xFFFFFFFFu; // results are filled with 0xFF before each pass

// The same tessellation as rtx_hair/dots_converter.hlsl (18 vertices per segment: three ribbons 60
// degrees apart, one winding), on the CPU.
void Tessellate(F4 p, F4 q, F3* out)
{
    const F3 d = q.xyz - p.xyz;
    const F3 t = d * (1.0f / std::sqrt(Dot(d, d)));
    const F3 a{std::fabs(t.x), std::fabs(t.y), std::fabs(t.z)};
    const F3 reference = (a.x <= a.y && a.x <= a.z) ? F3{1, 0, 0} : ((a.y <= a.z) ? F3{0, 1, 0} : F3{0, 0, 1});
    const F3 s = Normalize(Cross(t, reference));
    const F3 v = Cross(t, s);
    for (int face = 0; face < 3; ++face)
    {
        const F3 axis = face == 0 ? s : (face == 1 ? s * 0.5f + v * 0.86602540378f : s * -0.5f + v * 0.86602540378f);
        const F3 A = p.xyz + axis * (p.w * kRibbon), B = q.xyz - axis * (q.w * kRibbon);
        const F3 C = q.xyz + axis * (q.w * kRibbon), D = p.xyz - axis * (p.w * kRibbon);
        F3* f = out + face * 6;
        f[0] = A; f[1] = B; f[2] = C; f[3] = A; f[4] = D; f[5] = B;
    }
}

float Tag(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits = (bits & ~kTagMask) | kTagValue;
    std::memcpy(&value, &bits, 4);
    return value;
}

// probe_common.glsl's DotsDecode, on the CPU.
void DotsDecode(F3 v0, F3 v1, F3 v2, uint32_t primitive, F4& p, F4& q)
{
    if ((primitive & 1) == 0)
    {
        const F3 qc = (v1 + v2) * 0.5f, d = v2 - v1;
        const float len = Length(d);
        const F3 a = len > 0 ? d * (1.0f / len) : F3{};
        const float rq = len / (2 * kRibbon), rp = Dot(v0 - qc, a) / kRibbon;
        p = {v0 - a * (rp * kRibbon), rp};
        q = {qc, rq};
    }
    else
    {
        const F3 pc = (v0 + v1) * 0.5f, d = v0 - v1;
        const float len = Length(d);
        const F3 a = len > 0 ? d * (1.0f / len) : F3{};
        const float rp = len / (2 * kRibbon), rq = -Dot(v2 - pc, a) / kRibbon;
        p = {pc, rp};
        q = {v2 + a * (rq * kRibbon), rq};
    }
}

bool SameBits(F3 a, F3 b) { return std::memcmp(&a, &b, sizeof(F3)) == 0; }

// Where the updated BLAS's strands move to: a function of the position alone, so segments that share a
// vertex still share it.
F3 MovedPosition(F3 v) { return {v.x + 0.02f * std::sin(7.0f * v.z), v.y + 0.03f, v.z + 0.02f * std::cos(5.0f * v.x)}; }

// IEEE half precision, round to nearest even (what an engine writes), and back (what the converter's
// unpackHalf2x16 reads). The D3D12 probe's.
uint16_t ToHalf(float value)
{
    uint32_t x;
    std::memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00 | (mantissa ? 0x200 : 0));
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    if (exponent <= 0)
    {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t h = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (h & 1))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;
    return static_cast<uint16_t>(sign | h);
}

float FromHalf(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    if (exponent == 0)
    {
        const float v = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -v : v;
    }
    const uint32_t bits = exponent == 31 ? sign | 0x7F800000 | (mantissa << 13) : sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

// Phase 1b: the builder's compute binds go through this, so the probe sees what it leaves bound.
PFN_vkCmdBindPipeline gRealBind = nullptr;
VkPipeline gLastCompute = VK_NULL_HANDLE;
VKAPI_ATTR void VKAPI_CALL SpyBindPipeline(VkCommandBuffer list, VkPipelineBindPoint point, VkPipeline pipeline)
{
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) gLastCompute = pipeline;
    gRealBind(list, point, pipeline);
}

std::string Json(const std::string& s)
{
    std::string out;
    for (const char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char e[8]; std::snprintf(e, sizeof(e), "\\u%04x", c); out += e; }
        else out += c;
    }
    return out;
}

struct Options
{
    std::filesystem::path out, dump;
    int adapter = -1;
    uint32_t seed = 1;
    bool validation = false;
    bool game = false; // --game: act as an RTX 50 game; MFGUnlocker.dll (as winmm.dll) must do every conversion
    bool featureOnly = false; // --feature-only: as --game, chaining the LSS feature with neither its extension nor position fetch listed
};

Options Parse(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--out" && more) o.out = argv[++i];
        else if (a == L"--dump" && more) o.dump = argv[++i];
        else if (a == L"--adapter" && more) o.adapter = _wtoi(argv[++i]);
        else if (a == L"--seed" && more) o.seed = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (a == L"--validation") o.validation = true;
        else if (a == L"--game") o.game = true;
        else if (a == L"--feature-only") o.game = o.featureOnly = true;
        else Fail(2, "unknown argument %ls (use --out, --dump, --adapter, --seed, --validation, --game, --feature-only)", a.c_str());
    }
    return o;
}

// Validation messages (--validation): counted, the first few kept for the report.
struct Messages
{
    uint32_t errors{}, warnings{};
    std::vector<std::string> samples;
} gMessages;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;
    const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ++(error ? gMessages.errors : gMessages.warnings);
    if (gMessages.samples.size() < 8 && data && data->pMessage)
        gMessages.samples.push_back(std::string(error ? "error: " : "warning: ") + std::string(data->pMessage).substr(0, 400));
    return VK_FALSE;
}

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
};

struct Gpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer list = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<Buffer> owned;

    PFN_vkCreateAccelerationStructureKHR createAs = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAs = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAs = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRtPipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR groupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR traceRays = nullptr;

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return UINT32_MAX;
    }

    // Every buffer has a device address; host-visible ones stay mapped.
    Buffer Make(VkDeviceSize size, VkBufferUsageFlags usage, bool host)
    {
        Buffer b;
        b.size = std::max<VkDeviceSize>(size, 16);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = b.size;
        info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK_VK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
        const VkMemoryPropertyFlags wanted = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                  : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const uint32_t type = MemoryType(requirements.memoryTypeBits, wanted);
        if (type == UINT32_MAX) Fail(2, "no memory type for a %s buffer", host ? "host-visible" : "device-local");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        CHECK_VK(vkAllocateMemory(device, &allocate, nullptr, &b.memory));
        CHECK_VK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if (host) CHECK_VK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &addressInfo);
        owned.push_back(b);
        return b;
    }

    Buffer Upload(const void* data, size_t bytes, VkBufferUsageFlags usage)
    {
        Buffer b = Make(bytes, usage, true);
        std::memcpy(b.mapped, data, bytes);
        return b;
    }

    void Begin()
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK_VK(vkBeginCommandBuffer(list, &begin));
    }

    // Ends, submits and waits; begins the next recording. Returns the submission's result (a lost
    // device is reported, not fatal, so the plain-BLAS passes cannot take the report with them).
    VkResult Submit()
    {
        CHECK_VK(vkEndCommandBuffer(list));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &list;
        VkResult result = vkQueueSubmit(queue, 1, &submit, fence);
        if (result == VK_SUCCESS) result = vkWaitForFences(device, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000);
        if (result != VK_SUCCESS) return result;
        CHECK_VK(vkResetFences(device, 1, &fence));
        CHECK_VK(vkResetCommandBuffer(list, 0));
        Begin();
        return VK_SUCCESS;
    }

    void Barrier(VkPipelineStageFlags from, VkAccessFlags fromAccess, VkPipelineStageFlags to, VkAccessFlags toAccess)
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = fromAccess;
        barrier.dstAccessMask = toAccess;
        vkCmdPipelineBarrier(list, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
};

VkDeviceAddress AlignUp(VkDeviceAddress value, VkDeviceAddress alignment)
{
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

struct Blas
{
    const char* name;
    uint32_t mask, instanceId;
    float offsetX;
    bool dots, tagged, dataAccess, update, informational;
    VkDeviceSize sizeWith{}, sizeWithout{}, scratchWith{}, scratchWithout{}, updateScratch{};
    Buffer storage{}, scratch{};
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkDeviceAddress address{};
};

struct Group
{
    uint32_t rays{}, hits{}, unwritten{}, wrongInstance{}, exact{}, sameSetOtherOrder{}, zero{}, tagged{};
    double posError{}, radiusError{}, shaderPosError{}, shaderRadiusError{};
    std::vector<std::string> samples;
};

struct Pass
{
    const char* name;
    bool created{}, mainRan{}, plainRan{};
    std::string error;
    VkResult mainResult = VK_SUCCESS, plainResult = VK_SUCCESS;
    std::vector<HitOut> results;
    Group groups[5];
};

// SPIR-V words and their size in bytes.
struct Code
{
    const uint32_t* words;
    size_t bytes;
};
struct RtTables
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkStridedDeviceAddressRegionKHR raygen{}, miss{}, hit{}, callable{};
};

// probe_lss_common.glsl's LssOut (std430).
struct LssOut
{
    uint32_t status, isLss, primitive, instanceId;
    float bary[2], t, hitParam;
    float p[4], q[4];
};
static_assert(sizeof(LssOut) == 64);

struct LssGroup
{
    uint32_t rays{}, hits{}, unwritten{}, lss{}, wrong{}, segmentOk{}, expectSurface{}, surfaceOk{}, fallback{}, inside{}, fallbackOk{}, triangleOk{};
    double decodeError{}, surfaceError{}, uError{}, triangleError{};
    std::vector<std::string> samples;
};
struct LssPass
{
    const char* name;
    bool rewritten{}, created{}, mainRan{}, plainRan{};
    std::string error;
    VkResult mainResult = VK_SUCCESS, plainResult = VK_SUCCESS;
    rtx_hair::SpirvRewrite result{};
    rtx_hair::SpirvRewriteReport report{};
    size_t originalWords{};
    std::vector<uint32_t> spirv; // the rewritten module
    std::vector<LssOut> results;
    LssGroup groups[5];
};

const char* RewriteName(rtx_hair::SpirvRewrite r)
{
    switch (r)
    {
    case rtx_hair::SpirvRewrite::Unchanged: return "Unchanged";
    case rtx_hair::SpirvRewrite::Rewritten: return "Rewritten";
    case rtx_hair::SpirvRewrite::Neutral: return "Neutral";
    default: return "Failed";
    }
}

// Distance from x to the swept sphere of (p, rp)-(q, rq): the minimum over s of |x - c(s)| - r(s), which is
// convex in s. Independent of the shader's rounded-cone code (the D3D12 probe's).
double SweptDistance(const double x[3], const F4& p, const F4& q)
{
    auto f = [&](double s) {
        const double c[3] = {p.xyz.x + s * (q.xyz.x - p.xyz.x), p.xyz.y + s * (q.xyz.y - p.xyz.y), p.xyz.z + s * (q.xyz.z - p.xyz.z)};
        const double d[3] = {x[0] - c[0], x[1] - c[1], x[2] - c[2]};
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - (p.w + s * (q.w - p.w));
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 100; ++i)
    {
        const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
        if (f(a) < f(b)) hi = b;
        else lo = a;
    }
    return std::min({f(0), f(1), f(0.5 * (lo + hi))});
}

// Moller-Trumbore in double; b1, b2 are the barycentrics (weights of vertices 1 and 2).
bool RayTriangle(const double o[3], const double d[3], F3 a, F3 b, F3 c, double& t, double& b1, double& b2)
{
    const double e1[3] = {double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z}, e2[3] = {double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z};
    const double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-18) return false;
    const double tv[3] = {o[0] - a.x, o[1] - a.y, o[2] - a.z};
    b1 = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
    const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
    b2 = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
    t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    return b1 >= -1e-6 && b2 >= -1e-6 && b1 + b2 <= 1 + 1e-6;
}

// The round strand the rewritten shaders report hits on: kHitRadius LSS radii (rtx_hair/lss_ir.h).
F4 Shell(F4 v)
{
    v.w *= rtx_hair::kHitRadius;
    return v;
}

std::string Hex(const uint8_t* data, size_t size)
{
    std::string out;
    char b[4];
    for (size_t i = 0; i < size; ++i) { std::snprintf(b, sizeof(b), "%02x", data[i]); out += b; }
    return out;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name, uint32_t* version = nullptr)
{
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) { if (version) *version = e.specVersion; return true; }
    return false;
}
}

int wmain(int argc, wchar_t** argv)
{
    const Options options = Parse(argc, argv);
    timeGetTime(); // winmm: in game mode MFGUnlocker.dll sits beside the probe as winmm.dll
    std::mt19937 random(options.seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto range = [&](float a, float b) { return a + (b - a) * unit(random); };
    std::vector<std::string> failures;

    // Instance (Vulkan 1.3), with the validation layer if asked for and installed. Its messages go to
    // OnMessage from vkCreateInstance on (the chained messenger info), never to stdout.
    Gpu gpu;
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool validation = false;
    if (options.validation)
        for (const auto& l : layers)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    const char* debugUtils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "RtxHairVkProbe";
    app.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = OnMessage;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    if (validation)
    {
        instanceInfo.pNext = &messengerInfo;
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = &debugUtils;
    }
    CHECK_VK(vkCreateInstance(&instanceInfo, nullptr, &gpu.instance));
    if (validation)
    {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(gpu.instance, &messengerInfo, nullptr, &gpu.messenger);
    }

    // Physical device: the first NVIDIA one (or --adapter).
    uint32_t physicalCount = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(gpu.instance, &physicalCount, nullptr));
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    CHECK_VK(vkEnumeratePhysicalDevices(gpu.instance, &physicalCount, physicals.data()));
    VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties driverProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &idProperties};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR, &driverProperties};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, &asProperties};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rtProperties};
    for (uint32_t i = 0; i < physicalCount; ++i)
    {
        VkPhysicalDeviceProperties basic{};
        vkGetPhysicalDeviceProperties(physicals[i], &basic);
        if (options.adapter >= 0 ? static_cast<int>(i) == options.adapter : basic.vendorID == 0x10de)
        {
            gpu.physical = physicals[i];
            break;
        }
    }
    if (!gpu.physical) Fail(2, "no NVIDIA Vulkan device (use --adapter)");
    vkGetPhysicalDeviceProperties2(gpu.physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);
    const VkPhysicalDeviceProperties& basic = properties.properties;
    const uint32_t driver = basic.driverVersion;
    char driverText[32]{};
    if (basic.vendorID == 0x10de)
        std::snprintf(driverText, sizeof(driverText), "%u.%02u", (driver >> 22) & 0x3FF, (driver >> 14) & 0xFF);
    else
        std::snprintf(driverText, sizeof(driverText), "0x%08x", driver);

    uint32_t extensionCount = 0;
    CHECK_VK(vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> extensions(extensionCount);
    CHECK_VK(vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &extensionCount, extensions.data()));
    uint32_t fetchVersion = 0, lssVersion = 0;
    const bool hasFetch = HasExtension(extensions, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, &fetchVersion);
    const bool hasLss = HasExtension(extensions, VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME, &lssVersion);
    const bool hasAs = HasExtension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool hasDeferred = HasExtension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool hasQuery = HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool hasPipeline = HasExtension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);

    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR fetchFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR queryFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, hasFetch ? &fetchFeatures : nullptr};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipelineFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &queryFeatures};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &pipelineFeatures};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asFeatures};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12};
    vkGetPhysicalDeviceFeatures2(gpu.physical, &features);

    // What the driver does with a chained LSS feature struct on a GPU that may not list the extension: sentinels it
    // leaves alone mean it ignores the struct; values it writes are its answer (universal hair reports the feature
    // itself, after the driver). Validation messages from this query alone are reported apart, not as failures.
    const uint32_t errorsBefore = gMessages.errors, messagesBefore = gMessages.errors + gMessages.warnings;
    constexpr VkBool32 kSentinel = 0xCDCDCDCDu;
    VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV lssFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV};
    lssFeatures.spheres = kSentinel;
    lssFeatures.linearSweptSpheres = kSentinel;
    VkPhysicalDeviceFeatures2 lssQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &lssFeatures};
    vkGetPhysicalDeviceFeatures2(gpu.physical, &lssQuery);
    const uint32_t lssQueryErrors = gMessages.errors - errorsBefore, lssQueryMessages = gMessages.errors + gMessages.warnings - messagesBefore;
    const auto lssValue = [&](VkBool32 v) { return v == kSentinel ? std::string("\"untouched\"") : std::to_string(v); };

    const bool fetchAvailable = hasFetch && fetchFeatures.rayTracingPositionFetch;
    // Report and exit early when this GPU cannot run the probe at all.
    auto earlyReport = [&](int code, const char* why) {
        std::ostringstream json;
        json << "{\n  \"adapter\": \"" << Json(basic.deviceName) << "\", \"vendorId\": " << basic.vendorID << ", \"deviceId\": " << basic.deviceID
             << ", \"driver\": \"" << driverText << "\", \"positionFetchExtension\": " << (hasFetch ? "true" : "false")
             << ", \"positionFetchFeature\": " << (fetchFeatures.rayTracingPositionFetch ? "true" : "false")
             << ", \"lssExtension\": " << (hasLss ? "true" : "false") << ",\n  \"failures\": [\"" << Json(why) << "\"],\n  \"exitCode\": " << code << "\n}\n";
        const std::string report = json.str();
        std::fwrite(report.data(), 1, report.size(), stdout);
        if (!options.out.empty()) std::ofstream(options.out, std::ios::binary) << report;
        std::fprintf(stderr, "RtxHairVkProbe: %s (exit %d)\n", why, code);
        std::exit(code);
    };
    if (!hasAs || !hasDeferred || !hasQuery || !hasPipeline || !asFeatures.accelerationStructure || !queryFeatures.rayQuery
        || !pipelineFeatures.rayTracingPipeline || !features12.bufferDeviceAddress)
        earlyReport(2, "needs VK_KHR_acceleration_structure, VK_KHR_ray_query, VK_KHR_ray_tracing_pipeline and bufferDeviceAddress");
    if (!fetchAvailable) earlyReport(3, "VK_KHR_ray_tracing_position_fetch or its rayTracingPositionFetch feature is missing");
    if (options.game && (!hasLss || lssFeatures.linearSweptSpheres != VK_TRUE))
        earlyReport(3, "game mode: VK_NV_ray_tracing_linear_swept_spheres or its feature is not reported (is MFGUnlocker.dll beside the probe as winmm.dll?)");

    // Device: one queue family with compute; only the features the probe uses.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount && gpu.family == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) gpu.family = i;
    if (gpu.family == UINT32_MAX) Fail(2, "no compute queue family");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = gpu.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR enableFetch{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    enableFetch.rayTracingPositionFetch = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR enableQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &enableFetch};
    enableQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enablePipeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &enableQuery};
    enablePipeline.rayTracingPipeline = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enableAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &enablePipeline};
    enableAs.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &enableAs};
    enable12.bufferDeviceAddress = VK_TRUE;
    // Game mode: like an RTX 50 game, the LSS extension and feature instead of position fetch (the DLL swaps them).
    VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV enableLss{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV};
    enableLss.linearSweptSpheres = VK_TRUE;
    if (options.game)
    {
        enableLss.pNext = enableQuery.pNext == &enableFetch ? enableFetch.pNext : enableQuery.pNext;
        enableQuery.pNext = &enableLss;
    }
    std::vector<const char*> deviceExtensions{VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME};
    if (!options.game) deviceExtensions.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
    else if (!options.featureOnly) deviceExtensions.push_back(VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME);
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable12};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    CHECK_VK(vkCreateDevice(gpu.physical, &deviceInfo, nullptr, &gpu.device));
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    const auto load = [&](const char* name) {
        const auto f = vkGetDeviceProcAddr(gpu.device, name);
        if (!f) Fail(2, "vkGetDeviceProcAddr(%s) returned null", name);
        return f;
    };
    gpu.createAs = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
    gpu.destroyAs = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
    gpu.buildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(load("vkGetAccelerationStructureBuildSizesKHR"));
    gpu.buildAs = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
    gpu.asAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(load("vkGetAccelerationStructureDeviceAddressKHR"));
    gpu.createRtPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(load("vkCreateRayTracingPipelinesKHR"));
    gpu.groupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(load("vkGetRayTracingShaderGroupHandlesKHR"));
    gpu.traceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(load("vkCmdTraceRaysKHR"));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = gpu.family;
    CHECK_VK(vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &gpu.pool));
    VkCommandBufferAllocateInfo listInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    listInfo.commandPool = gpu.pool;
    listInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    listInfo.commandBufferCount = 1;
    CHECK_VK(vkAllocateCommandBuffers(gpu.device, &listInfo, &gpu.list));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK_VK(vkCreateFence(gpu.device, &fenceInfo, nullptr, &gpu.fence));
    gpu.Begin();

    // Synthetic LSS strands (the D3D12 probe's, same random sequence): kSegmentsPerStrand + 1 vertices
    // each; segment j = (v[j], v[j+1]).
    std::vector<F4> truthP, truthQ; // per segment, object space
    for (uint32_t s = 0; s < kStrands; ++s)
    {
        F3 at{range(-0.5f, 0.5f), range(-0.5f, 0.5f), range(-0.5f, 0.5f)};
        F3 dir = Normalize({range(-1, 1), range(-1, 1), range(-1, 1)});
        const float r0 = range(0.004f, 0.02f);
        std::vector<F4> vertices;
        for (uint32_t j = 0; j <= kSegmentsPerStrand; ++j)
        {
            const float u = static_cast<float>(j) / kSegmentsPerStrand;
            float r = r0;
            switch (s % 4)
            {
            case 0: r = r0 * (1.0f - u); break;          // tapered to a zero tip
            case 1: break;                               // constant
            case 2: r = r0 * (1.0f - 0.75f * u); break;  // thinning
            default: r = 0.002f + 0.013f * u; break;     // growing
            }
            vertices.push_back({at, r});
            dir = Normalize(dir + F3{range(-0.3f, 0.3f), range(-0.3f, 0.3f), range(-0.3f, 0.3f)});
            at = at + dir * range(0.05f, 0.12f);
        }
        for (uint32_t j = 0; j < kSegmentsPerStrand; ++j) { truthP.push_back(vertices[j]); truthQ.push_back(vertices[j + 1]); }
    }
    const uint32_t segments = static_cast<uint32_t>(truthP.size());
    std::vector<F4> movedP(segments), movedQ(segments);
    for (uint32_t s = 0; s < segments; ++s)
    {
        movedP[s] = {MovedPosition(truthP[s].xyz), truthP[s].w};
        movedQ[s] = {MovedPosition(truthQ[s].xyz), truthQ[s].w};
    }
    std::vector<F3> dotsPlain(segments * kVertices), dotsMoved(segments * kVertices), dotsTagged;
    for (uint32_t s = 0; s < segments; ++s)
    {
        Tessellate(truthP[s], truthQ[s], &dotsPlain[s * kVertices]);
        Tessellate(movedP[s], movedQ[s], &dotsMoved[s * kVertices]);
    }
    dotsTagged = dotsPlain;
    for (auto& v : dotsTagged) { v.x = Tag(v.x); v.y = Tag(v.y); v.z = Tag(v.z); }
    for (auto& v : dotsMoved) { v.x = Tag(v.x); v.y = Tag(v.y); v.z = Tag(v.z); }

    // Game-like mesh: an indexed, jittered height-field grid.
    std::vector<F3> meshVertices;
    std::vector<uint32_t> meshIndices;
    for (uint32_t y = 0; y <= kGrid; ++y)
        for (uint32_t x = 0; x <= kGrid; ++x)
        {
            const float fx = static_cast<float>(x) / kGrid - 0.5f, fy = static_cast<float>(y) / kGrid - 0.5f;
            meshVertices.push_back({fx + range(-0.004f, 0.004f), fy + range(-0.004f, 0.004f),
                0.1f * std::sin(3 * fx) * std::cos(2 * fy) + range(-0.003f, 0.003f)});
        }
    for (uint32_t y = 0; y < kGrid; ++y)
        for (uint32_t x = 0; x < kGrid; ++x)
        {
            const uint32_t a = y * (kGrid + 1) + x, b = a + 1, c = a + kGrid + 1, d = c + 1;
            meshIndices.insert(meshIndices.end(), {a, b, c, b, d, c});
        }
    const uint32_t meshTriangles = static_cast<uint32_t>(meshIndices.size() / 3);

    constexpr VkBufferUsageFlags kInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const Buffer dotsTaggedBuffer = gpu.Upload(dotsTagged.data(), dotsTagged.size() * sizeof(F3), kInput);
    const Buffer dotsPlainBuffer = gpu.Upload(dotsPlain.data(), dotsPlain.size() * sizeof(F3), kInput);
    const Buffer dotsMovedBuffer = gpu.Upload(dotsMoved.data(), dotsMoved.size() * sizeof(F3), kInput);
    const Buffer meshVertexBuffer = gpu.Upload(meshVertices.data(), meshVertices.size() * sizeof(F3), kInput);
    const Buffer meshIndexBuffer = gpu.Upload(meshIndices.data(), meshIndices.size() * sizeof(uint32_t), kInput);

    // BLASes. Index order follows the D3D12 probe (0-3), then the updated one (4).
    Blas blases[] = {
        {"dotsTagged", 0x01, 10, 0.0f, true, true, true, false, false},
        {"dotsPlain", 0x08, 11, 4.0f, true, false, true, false, false},
        {"meshDataAccess", 0x04, 21, 12.0f, false, false, true, false, false},
        {"meshPlain", 0x02, 20, 8.0f, false, false, false, false, true},
        {"dotsUpdated", 0x10, 12, 16.0f, true, true, true, true, false},
    };
    constexpr uint32_t kBlasCount = static_cast<uint32_t>(std::size(blases));
    const VkDeviceSize scratchAlign = asProperties.minAccelerationStructureScratchOffsetAlignment;
    auto geometryOf = [&](const Blas& blas, const Buffer& dotsSource) {
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexStride = sizeof(F3);
        if (blas.dots)
        {
            tri.vertexData.deviceAddress = dotsSource.address;
            tri.maxVertex = segments * kVertices - 1;
            tri.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        else
        {
            tri.vertexData.deviceAddress = meshVertexBuffer.address;
            tri.maxVertex = static_cast<uint32_t>(meshVertices.size()) - 1;
            tri.indexType = VK_INDEX_TYPE_UINT32;
            tri.indexData.deviceAddress = meshIndexBuffer.address;
        }
        return geometry;
    };
    auto flagsOf = [](const Blas& blas, bool dataAccess) {
        return static_cast<VkBuildAccelerationStructureFlagsKHR>(VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
            | (dataAccess ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR : 0)
            | (blas.update ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0));
    };
    for (auto& blas : blases)
    {
        const uint32_t primitives = blas.dots ? segments * kTriangles : meshTriangles;
        const Buffer& source = blas.tagged ? dotsTaggedBuffer : dotsPlainBuffer;
        VkAccelerationStructureGeometryKHR geometry = geometryOf(blas, source);
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR with{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        VkAccelerationStructureBuildSizesInfoKHR without{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        info.flags = flagsOf(blas, true);
        gpu.buildSizes(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &with);
        info.flags = flagsOf(blas, false);
        gpu.buildSizes(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &without);
        blas.sizeWith = with.accelerationStructureSize;
        blas.sizeWithout = without.accelerationStructureSize;
        blas.scratchWith = with.buildScratchSize;
        blas.scratchWithout = without.buildScratchSize;
        const auto& sizes = blas.dataAccess ? with : without;
        blas.updateScratch = sizes.updateScratchSize;
        blas.storage = gpu.Make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        blas.scratch = gpu.Make(std::max(sizes.buildScratchSize, sizes.updateScratchSize) + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = blas.storage.buffer;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        CHECK_VK(gpu.createAs(gpu.device, &create, nullptr, &blas.as));
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = blas.as;
        blas.address = gpu.asAddress(gpu.device, &addressInfo);
        info.flags = flagsOf(blas, blas.dataAccess);
        info.dstAccelerationStructure = blas.as;
        info.scratchData.deviceAddress = AlignUp(blas.scratch.address, scratchAlign);
        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{primitives, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &rangeInfo;
        gpu.buildAs(gpu.list, 1, &info, &ranges);
    }
    gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    // The updated BLAS: refit in place to the moved strands (same counts, tagged as a converter writes them).
    {
        Blas& blas = blases[4];
        VkAccelerationStructureGeometryKHR geometry = geometryOf(blas, dotsMovedBuffer);
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        info.flags = flagsOf(blas, true);
        info.srcAccelerationStructure = blas.as;
        info.dstAccelerationStructure = blas.as;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        info.scratchData.deviceAddress = AlignUp(blas.scratch.address, scratchAlign);
        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{segments * kTriangles, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &rangeInfo;
        gpu.buildAs(gpu.list, 1, &info, &ranges);
    }
    gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // TLAS: one instance per BLAS, translated apart along x; culling off, as universal hair sets it on DOTS instances.
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    for (const auto& blas : blases)
    {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
        instance.transform.matrix[0][3] = blas.offsetX;
        instance.instanceCustomIndex = blas.instanceId;
        instance.mask = blas.mask;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blas.address;
        instances.push_back(instance);
    }
    const Buffer instanceBuffer = gpu.Upload(instances.data(), instances.size() * sizeof(instances[0]), kInput);
    VkAccelerationStructureGeometryKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    top.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    top.geometry.instances.arrayOfPointers = VK_FALSE;
    top.geometry.instances.data.deviceAddress = instanceBuffer.address;
    VkAccelerationStructureBuildGeometryInfoKHR topInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    topInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    topInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    topInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    topInfo.geometryCount = 1;
    topInfo.pGeometries = &top;
    VkAccelerationStructureBuildSizesInfoKHR topSizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    gpu.buildSizes(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topInfo, &kBlasCount, &topSizes);
    const Buffer tlasStorage = gpu.Make(topSizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    const Buffer tlasScratch = gpu.Make(topSizes.buildScratchSize + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    VkAccelerationStructureCreateInfoKHR tlasCreate{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tlasCreate.buffer = tlasStorage.buffer;
    tlasCreate.size = topSizes.accelerationStructureSize;
    tlasCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    CHECK_VK(gpu.createAs(gpu.device, &tlasCreate, nullptr, &tlas));
    topInfo.dstAccelerationStructure = tlas;
    topInfo.scratchData.deviceAddress = AlignUp(tlasScratch.address, scratchAlign);
    {
        VkAccelerationStructureBuildRangeInfoKHR rangeInfo{kBlasCount, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &rangeInfo;
        gpu.buildAs(gpu.list, 1, &topInfo, &ranges);
    }
    gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    if (const VkResult r = gpu.Submit(); r != VK_SUCCESS) Fail(2, "acceleration structure builds failed: %d", static_cast<int>(r));

    // Rays. DOTS: across each segment's axis at a random point and angle (the hit may be a neighbouring
    // segment; checks use the segment that was hit). Mesh: down onto a triangle. The updated BLAS's rays aim
    // at the moved strands. Main rays first (BLAS 0, 1, 2, 4), then the plain mesh's (3).
    std::vector<RayIn> rays;
    std::vector<uint32_t> rayBlas;
    for (const uint32_t b : {0u, 1u, 2u, 4u, 3u})
    {
        const auto& blas = blases[b];
        const auto& P = blas.update ? movedP : truthP;
        const auto& Q = blas.update ? movedQ : truthQ;
        const uint32_t count = blas.dots ? segments : std::min(meshTriangles, segments);
        for (uint32_t i = 0; i < count; ++i)
        {
            RayIn ray{};
            ray.mask = blas.mask;
            if (blas.dots)
            {
                const F3 p = P[i].xyz, q = Q[i].xyz;
                const F3 m = p + (q - p) * range(0.25f, 0.75f);
                const F3 n = Normalize(Cross(Normalize(q - p), Normalize({range(-1, 1), range(-1, 1), range(-1, 1)})));
                ray.origin = m + n * 0.25f;
                ray.direction = n * -1.0f;
                ray.tmax = 0.5f;
            }
            else
            {
                const F3 a = meshVertices[meshIndices[i * 3]], c1 = meshVertices[meshIndices[i * 3 + 1]], c2 = meshVertices[meshIndices[i * 3 + 2]];
                const F3 centroid = (a + c1 + c2) * (1.0f / 3);
                ray.origin = centroid + F3{0, 0, 1};
                ray.direction = {0, 0, -1};
                ray.tmax = 2.0f;
            }
            ray.origin.x += blas.offsetX;
            rays.push_back(ray);
            rayBlas.push_back(b);
        }
    }
    const uint32_t rayCount = static_cast<uint32_t>(rays.size());
    const uint32_t plainCount = std::min(meshTriangles, segments), mainCount = rayCount - plainCount;
    const Buffer rayBuffer = gpu.Upload(rays.data(), rays.size() * sizeof(RayIn), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Descriptors: TLAS, rays, results; push constants (rayOffset, rayCount, tagMask, tagValue).
    constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR
        | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, kStages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
    setLayoutInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDescriptorSetLayout(gpu.device, &setLayoutInfo, nullptr, &setLayout));
    const VkPushConstantRange pushRange{kStages, 0, 16};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    CHECK_VK(vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &layout));
    const VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    VkDescriptorPoolCreateInfo descriptorPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorPoolInfo.maxSets = 8;
    descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    descriptorPoolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    CHECK_VK(vkCreateDescriptorPool(gpu.device, &descriptorPoolInfo, nullptr, &descriptorPool));
    // One set per pass (each pass has its own result buffer) and scene (its TLAS and rays).
    auto makeSet = [&](const Buffer& results, VkAccelerationStructureKHR sceneTlas, const Buffer& sceneRays) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        CHECK_VK(vkAllocateDescriptorSets(gpu.device, &allocate, &set));
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &sceneTlas;
        const VkDescriptorBufferInfo rayInfo{sceneRays.buffer, 0, VK_WHOLE_SIZE}, resultInfo{results.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        for (auto& w : writes) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.descriptorCount = 1; }
        writes[0].pNext = &asWrite;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &rayInfo;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &resultInfo;
        vkUpdateDescriptorSets(gpu.device, 3, writes, 0, nullptr);
        return set;
    };
    auto resultText = [](const char* what, VkResult r) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s %d", what, static_cast<int>(r));
        return std::string(text);
    };
    // A shader module; VK_NULL_HANDLE with `error` when the driver refuses the SPIR-V.
    auto makeModule = [&](const Code& code, std::string& error) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.bytes;
        info.pCode = code.words;
        VkShaderModule m = VK_NULL_HANDLE;
        const VkResult r = vkCreateShaderModule(gpu.device, &info, nullptr, &m);
        if (r != VK_SUCCESS) error = resultText("vkCreateShaderModule", r);
        return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
    };
    std::vector<VkPipeline> pipelines; // for teardown
    // A compute pipeline; false with `error` when the driver refuses it.
    auto makeCompute = [&](const Code& code, VkPipeline& pipeline, std::string& error) {
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = makeModule(code, error);
        info.stage.pName = "main";
        info.layout = layout;
        if (!info.stage.module) return false;
        const VkResult r = vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (r != VK_SUCCESS)
        {
            error = resultText("vkCreateComputePipelines", r);
            return false;
        }
        pipelines.push_back(pipeline);
        return true;
    };
    // A ray tracing pipeline (raygen, miss, closest-hit triangle group) and its binding table, each at a base-aligned offset.
    auto makeRt = [&](const Code& rgen, const Code& rmiss, const Code& rchit, RtTables& out, std::string& error) {
        VkPipelineShaderStageCreateInfo stages[3]{};
        const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        const Code* codes[3] = {&rgen, &rmiss, &rchit};
        for (int i = 0; i < 3; ++i)
        {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = kinds[i];
            stages[i].module = makeModule(*codes[i], error);
            stages[i].pName = "main";
            if (!stages[i].module) return false;
        }
        VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
        for (auto& g : groups)
        {
            g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            g.generalShader = g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[2].closestHitShader = 2;
        VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        info.stageCount = 3;
        info.pStages = stages;
        info.groupCount = 3;
        info.pGroups = groups;
        info.maxPipelineRayRecursionDepth = 1;
        info.layout = layout;
        const VkResult r = gpu.createRtPipelines(gpu.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
        if (r != VK_SUCCESS)
        {
            error = resultText("vkCreateRayTracingPipelinesKHR", r);
            return false;
        }
        pipelines.push_back(out.pipeline);
        const uint32_t handleSize = rtProperties.shaderGroupHandleSize;
        const VkDeviceSize stride = AlignUp(handleSize, rtProperties.shaderGroupHandleAlignment);
        const VkDeviceSize base = AlignUp(stride, rtProperties.shaderGroupBaseAlignment);
        std::vector<uint8_t> handles(3 * handleSize);
        CHECK_VK(gpu.groupHandles(gpu.device, out.pipeline, 0, 3, handles.size(), handles.data()));
        const Buffer table = gpu.Make(3 * base + rtProperties.shaderGroupBaseAlignment, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true);
        const VkDeviceAddress start = AlignUp(table.address, rtProperties.shaderGroupBaseAlignment);
        auto* bytes = static_cast<uint8_t*>(table.mapped) + (start - table.address);
        for (int i = 0; i < 3; ++i) std::memcpy(bytes + i * base, handles.data() + i * handleSize, handleSize);
        out.raygen = {start, stride, stride};
        out.miss = {start + base, stride, stride};
        out.hit = {start + 2 * base, stride, stride};
        return true;
    };
    // Both passes' main rays first, then (separate submissions) the plain mesh's rays, so an undefined fetch on
    // the plain BLAS cannot take the main results with it. A failed submission ends the GPU work. Push constants:
    // ray offset, ray count, then phase 0's tag mask and value, or the LSS shaders' cull mask.
    struct SceneRun
    {
        VkAccelerationStructureKHR tlas;
        const Buffer* rays;
        uint32_t count, main, plain, a, b;
    };
    const SceneRun scene0{tlas, &rayBuffer, rayCount, mainCount, plainCount, kTagMask, kTagValue};
    const SceneRun scene1a{tlas, &rayBuffer, rayCount, mainCount, plainCount, 0x11u, 0u}; // hair: tagged DOTS 0x01, 0x10
    bool gpuFailed = false;
    auto trace = [&](auto& passes, VkPipeline compute, const RtTables& rt, const Buffer* results, size_t stride, const SceneRun& run) {
        VkDescriptorSet sets[2]{};
        for (int k = 0; k < 2; ++k)
            if (passes[k].created) sets[k] = makeSet(results[k], run.tlas, *run.rays);
        const uint32_t constantsMain[4] = {0, run.main, run.a, run.b};
        const uint32_t constantsPlain[4] = {run.main, run.plain, run.a, run.b};
        for (int part = 0; part < 2 && !gpuFailed; ++part)
            for (int k = 0; k < 2 && !gpuFailed; ++k)
            {
                auto& pass = passes[k];
                if (!pass.created || (part == 1 && pass.mainResult != VK_SUCCESS)) continue;
                const uint32_t* constants = part == 0 ? constantsMain : constantsPlain;
                if (constants[1] == 0) continue;
                const VkPipelineBindPoint point = k == 0 ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
                const VkPipelineStageFlags stage = k == 0 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
                vkCmdBindPipeline(gpu.list, point, k == 0 ? compute : rt.pipeline);
                vkCmdBindDescriptorSets(gpu.list, point, layout, 0, 1, &sets[k], 0, nullptr);
                vkCmdPushConstants(gpu.list, layout, kStages, 0, 16, constants);
                if (k == 0) vkCmdDispatch(gpu.list, (constants[1] + 63) / 64, 1, 1);
                else gpu.traceRays(gpu.list, &rt.raygen, &rt.miss, &rt.hit, &rt.callable, constants[1], 1, 1);
                gpu.Barrier(stage, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                const VkResult r = gpu.Submit();
                (part == 0 ? pass.mainRan : pass.plainRan) = true;
                (part == 0 ? pass.mainResult : pass.plainResult) = r;
                gpuFailed = r != VK_SUCCESS;
            }
        for (int k = 0; k < 2; ++k)
        {
            if (!passes[k].mainRan) continue;
            passes[k].results.resize(run.count);
            std::memcpy(passes[k].results.data(), results[k].mapped, static_cast<size_t>(run.count) * stride);
        }
    };
    auto resultBuffers = [&](size_t stride, Buffer* out, uint32_t count) {
        for (int k = 0; k < 2; ++k)
        {
            out[k] = gpu.Make(static_cast<VkDeviceSize>(count) * stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            std::memset(out[k].mapped, 0xFF, static_cast<size_t>(count) * stride);
        }
    };

    // Phase 0: probe.comp (ray query) and probe.rchit (closest hit), position fetch as written.
    Pass passes[2] = {{"rayQuery"}, {"closestHit"}};
    Buffer phase0Results[2];
    resultBuffers(sizeof(HitOut), phase0Results, rayCount);
    VkPipeline computePipeline = VK_NULL_HANDLE;
    RtTables rt0;
    passes[0].created = makeCompute({kProbeComp, sizeof(kProbeComp)}, computePipeline, passes[0].error);
    passes[1].created = makeRt({kProbeRgen, sizeof(kProbeRgen)}, {kProbeRmiss, sizeof(kProbeRmiss)}, {kProbeRchit, sizeof(kProbeRchit)}, rt0, passes[1].error);
    trace(passes, computePipeline, rt0, phase0Results, sizeof(HitOut), scene0);

    // Phase 1a: probe_lss.comp and probe_lss.rchit, rewritten by rtx_hair/lss_spirv, on the same scene.
    LssPass lssPasses[2] = {{"lssRayQuery"}, {"lssClosestHit"}};
    {
        const Code sources[2] = {{kProbeLssComp, sizeof(kProbeLssComp)}, {kProbeLssRchit, sizeof(kProbeLssRchit)}};
        for (int k = 0; k < 2; ++k)
        {
            LssPass& p = lssPasses[k];
            p.originalWords = sources[k].bytes / 4;
            std::string error;
            if (options.game)
            {
                p.spirv.assign(sources[k].words, sources[k].words + p.originalWords);
                p.result = rtx_hair::SpirvRewrite::Unchanged; // as written for RTX 50: MFGUnlocker.dll rewrites it
                p.rewritten = true;
                continue;
            }
            p.result = rtx_hair::RewriteLssSpirv(std::span<const uint32_t>(sources[k].words, p.originalWords), p.spirv, p.report, error);
            p.rewritten = p.result == rtx_hair::SpirvRewrite::Rewritten;
            if (!p.rewritten) p.error = std::string("not rewritten (") + RewriteName(p.result) + "): " + error;
            if (!options.dump.empty() && !p.spirv.empty())
            {
                std::filesystem::create_directories(options.dump);
                std::ofstream(options.dump / (std::string(p.name) + ".rewritten.spv"), std::ios::binary)
                    .write(reinterpret_cast<const char*>(p.spirv.data()), static_cast<std::streamsize>(p.spirv.size() * 4));
            }
        }
    }
    Buffer lssResults[2];
    resultBuffers(sizeof(LssOut), lssResults, rayCount);
    VkPipeline lssCompute = VK_NULL_HANDLE;
    RtTables rt1;
    if (lssPasses[0].rewritten)
        lssPasses[0].created = makeCompute({lssPasses[0].spirv.data(), lssPasses[0].spirv.size() * 4}, lssCompute, lssPasses[0].error);
    if (lssPasses[1].rewritten)
        lssPasses[1].created = makeRt({kProbeLssRgen, sizeof(kProbeLssRgen)}, {kProbeLssRmiss, sizeof(kProbeLssRmiss)},
            {lssPasses[1].spirv.data(), lssPasses[1].spirv.size() * 4}, rt1, lssPasses[1].error);
    trace(lssPasses, lssCompute, rt1, lssResults, sizeof(LssOut), scene1a);

    // Phase 1b: layouts A-C below, and D: an unsupported position format, every primitive inactive, never hit. Their
    // TLAS keeps culling on: the builder's instance patch must turn it off on the four hair instances and leave the
    // plain mesh instance beside them as it is.
    struct LayoutCase
    {
        const char* name;
        uint32_t mask, instanceId;
        float offsetX;
        std::vector<F4> p, q; // per LSS primitive, as the GPU reads them (after the last build)
        std::vector<F3> dots; // CPU DOTS of p, q (tagged), for ribbon fallbacks
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Buffer storage{}, scratch{};
        VkResult result = VK_SUCCESS;
        LssGroup groups[2];
    };
    LayoutCase layouts[4] = {{"successive32", 0x01, 30, 0.0f}, {"list16", 0x02, 31, 4.0f}, {"successiveConstant", 0x04, 32, 8.0f},
        {"unsupported", 0x08, 33, 12.0f}};
    rtx_hair::vk::DeviceFunctions functions{};
    rtx_hair::vk::DotsBuilderVk builder;
    std::string builderError;
    bool builderReady = rtx_hair::vk::LoadDeviceFunctions(gpu.device, vkGetDeviceProcAddr, functions);
    if (!builderReady) builderError = "a device function the builder needs is missing";
    gRealBind = functions.cmdBindPipeline;
    functions.cmdBindPipeline = SpyBindPipeline;
    if (builderReady && !options.game) builderReady = builder.Initialize(gpu.device, functions, gpu.memory, builderError);
    if (builderReady && !options.game) builder.SetRebuildPolicy(2, 1000000, 0);
    uint32_t builderCalls = 0, restores = 0;
    bool patchRead = false;
    uint32_t patchedOk = 0, patchedChecked = 0;
    VkAccelerationStructureKHR tlasB = VK_NULL_HANDLE;
    Buffer tlasBStorage{}, tlasBScratch{}, raysBBuffer{};
    std::vector<RayIn> raysB;
    std::vector<uint32_t> rayLayout;
    LssPass buildPasses[2] = {{"lssRayQuery"}, {"lssClosestHit"}};
    rtx_hair::vk::DotsStats builderStats{};
    // A build through the builder, with the probe's compute pipeline bound before it.
    auto buildThrough = [&](const VkAccelerationStructureBuildGeometryInfoKHR& info, const VkAccelerationStructureBuildRangeInfoKHR* ranges) {
        vkCmdBindPipeline(gpu.list, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        gLastCompute = VK_NULL_HANDLE;
        if (options.game) gpu.buildAs(gpu.list, 1, &info, &ranges);
        else builder.CmdBuild(gpu.buildAs, gpu.list, 1, &info, &ranges, computePipeline);
        ++builderCalls;
        if (gLastCompute == computePipeline) ++restores;
    };
    if (builderReady && !gpuFailed && passes[0].created)
    {
        std::vector<F4> strandVertices, movedVertices;
        std::vector<uint32_t> segmentStart;
        for (uint32_t s = 0; s < kStrands; ++s)
        {
            for (uint32_t j = 0; j < kSegmentsPerStrand; ++j)
            {
                strandVertices.push_back(truthP[s * kSegmentsPerStrand + j]);
                segmentStart.push_back(s * (kSegmentsPerStrand + 1) + j);
            }
            strandVertices.push_back(truthQ[s * kSegmentsPerStrand + kSegmentsPerStrand - 1]);
        }
        for (const F4& v : strandVertices) movedVertices.push_back({MovedPosition(v.xyz), v.w});
        constexpr VkBufferUsageFlags kLssInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        // A: float4 (position, radius), successive with UINT32 segment starts.
        const Buffer a0 = gpu.Upload(strandVertices.data(), strandVertices.size() * sizeof(F4), kLssInput);
        const Buffer a1 = gpu.Upload(movedVertices.data(), movedVertices.size() * sizeof(F4), kLssInput);
        const Buffer aIndex = gpu.Upload(segmentStart.data(), segmentStart.size() * sizeof(uint32_t), kLssInput);
        // B: a list of (p, q) pairs as halves, behind primitiveOffset 16 and firstVertex 2 (both buffers: positions
        // start at 16 + 8 * 2 bytes, radii at 16 + 2 * 2).
        constexpr uint32_t kOffsetB = 16, kFirstB = 2;
        std::vector<uint8_t> bPositions(kOffsetB + 8 * kFirstB, 0x7F), bRadii(kOffsetB + 2 * kFirstB, 0x7F);
        for (uint32_t s = 0; s < segments; ++s)
            for (const F4& v : {truthP[s], truthQ[s]})
            {
                const uint16_t h[4] = {ToHalf(v.xyz.x), ToHalf(v.xyz.y), ToHalf(v.xyz.z), 0};
                const uint16_t r = ToHalf(v.w);
                bPositions.insert(bPositions.end(), reinterpret_cast<const uint8_t*>(h), reinterpret_cast<const uint8_t*>(h) + 8);
                bRadii.insert(bRadii.end(), reinterpret_cast<const uint8_t*>(&r), reinterpret_cast<const uint8_t*>(&r) + 2);
            }
        const Buffer bPos = gpu.Upload(bPositions.data(), bPositions.size(), kLssInput);
        const Buffer bRad = gpu.Upload(bRadii.data(), bRadii.size(), kLssInput);
        // C: packed float3 behind 3 unused vertices (firstVertex 3), one radius, UINT16 indices behind 8 bytes.
        constexpr uint32_t kFirstC = 3, kOffsetC = 8;
        constexpr float kRadiusC = 0.008f;
        std::vector<F3> cPositions(kFirstC, F3{9.0f, 9.0f, 9.0f});
        for (const F4& v : strandVertices) cPositions.push_back(v.xyz);
        std::vector<uint16_t> cIndices(kOffsetC / 2, 0xFFFF);
        for (const uint32_t start : segmentStart) cIndices.push_back(static_cast<uint16_t>(start));
        const Buffer cPos = gpu.Upload(cPositions.data(), cPositions.size() * sizeof(F3), kLssInput);
        const Buffer cRad = gpu.Upload(&kRadiusC, sizeof(kRadiusC), kLssInput);
        const Buffer cIndex = gpu.Upload(cIndices.data(), cIndices.size() * sizeof(uint16_t), kLssInput);
        // Truth (as the GPU reads it) and the CPU DOTS of it.
        for (uint32_t s = 0; s < segments; ++s)
        {
            const F4 mp = {MovedPosition(truthP[s].xyz), truthP[s].w}, mq = {MovedPosition(truthQ[s].xyz), truthQ[s].w};
            layouts[0].p.push_back(mp);
            layouts[0].q.push_back(mq);
            auto half = [](F4 v) { return F4{{FromHalf(ToHalf(v.xyz.x)), FromHalf(ToHalf(v.xyz.y)), FromHalf(ToHalf(v.xyz.z))}, FromHalf(ToHalf(v.w))}; };
            layouts[1].p.push_back(half(truthP[s]));
            layouts[1].q.push_back(half(truthQ[s]));
            layouts[2].p.push_back({truthP[s].xyz, kRadiusC});
            layouts[2].q.push_back({truthQ[s].xyz, kRadiusC});
            layouts[3].p.push_back(truthP[s]);
            layouts[3].q.push_back(truthQ[s]);
        }
        for (auto& L : layouts)
        {
            L.dots.resize(segments * kVertices);
            for (uint32_t s = 0; s < segments; ++s) Tessellate(L.p[s], L.q[s], &L.dots[s * kVertices]);
            for (auto& v : L.dots) { v.x = Tag(v.x); v.y = Tag(v.y); v.z = Tag(v.z); }
        }
        VkAccelerationStructureGeometryLinearSweptSpheresDataNV data[4]{};
        VkAccelerationStructureGeometryKHR geometry[4]{};
        VkAccelerationStructureBuildRangeInfoKHR range[4]{};
        for (int k = 0; k < 4; ++k)
        {
            auto& d = data[k];
            d.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_LINEAR_SWEPT_SPHERES_DATA_NV;
            d.indexingMode = VK_RAY_TRACING_LSS_INDEXING_MODE_SUCCESSIVE_NV;
            d.endCapsMode = VK_RAY_TRACING_LSS_PRIMITIVE_END_CAPS_MODE_CHAINED_NV;
            d.indexType = VK_INDEX_TYPE_NONE_KHR;
            geometry[k].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry[k].pNext = &d;
            geometry[k].geometryType = VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV;
            geometry[k].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            range[k].primitiveCount = segments;
        }
        data[0].vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data[0].vertexData.deviceAddress = a0.address;
        data[0].vertexStride = sizeof(F4);
        data[0].radiusFormat = VK_FORMAT_R32_SFLOAT;
        data[0].radiusData.deviceAddress = a0.address + 12;
        data[0].radiusStride = sizeof(F4);
        data[0].indexType = VK_INDEX_TYPE_UINT32;
        data[0].indexData.deviceAddress = aIndex.address;
        data[0].indexStride = 4;
        data[1].vertexFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        data[1].vertexData.deviceAddress = bPos.address;
        data[1].vertexStride = 8;
        data[1].radiusFormat = VK_FORMAT_R16_SFLOAT;
        data[1].radiusData.deviceAddress = bRad.address;
        data[1].radiusStride = 2;
        data[1].indexingMode = VK_RAY_TRACING_LSS_INDEXING_MODE_LIST_NV;
        range[1].primitiveOffset = kOffsetB;
        range[1].firstVertex = kFirstB;
        data[2].vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data[2].vertexData.deviceAddress = cPos.address;
        data[2].vertexStride = sizeof(F3);
        data[2].radiusFormat = VK_FORMAT_R32_SFLOAT;
        data[2].radiusData.deviceAddress = cRad.address;
        data[2].radiusStride = 0;
        data[2].indexType = VK_INDEX_TYPE_UINT16;
        data[2].indexData.deviceAddress = cIndex.address;
        data[2].indexStride = 2;
        range[2].primitiveOffset = kOffsetC;
        range[2].firstVertex = kFirstC;
        data[3] = data[0];
        data[3].vertexFormat = VK_FORMAT_R8G8B8A8_UNORM; // unsupported: inactive
        const VkDeviceSize scratchAlignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
        VkAccelerationStructureBuildGeometryInfoKHR infos[4]{};
        for (int k = 0; k < 4; ++k)
        {
            auto& L = layouts[k];
            auto& info = infos[k];
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | (k == 0 ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geometry[k];
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            if (options.game) gpu.buildSizes(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &range[k].primitiveCount, &sizes);
            else builder.BuildSizes(gpu.buildSizes, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &range[k].primitiveCount, &sizes);
            if (!sizes.accelerationStructureSize)
            {
                L.result = VK_ERROR_UNKNOWN;
                continue;
            }
            L.storage = gpu.Make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
            L.scratch = gpu.Make(std::max(sizes.buildScratchSize, sizes.updateScratchSize) + scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
            VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            create.buffer = L.storage.buffer;
            create.size = sizes.accelerationStructureSize;
            create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            L.result = gpu.createAs(gpu.device, &create, nullptr, &L.as);
            if (L.result != VK_SUCCESS) continue;
            info.dstAccelerationStructure = L.as;
            info.scratchData.deviceAddress = AlignUp(L.scratch.address, scratchAlignment);
            buildThrough(info, &range[k]);
        }
        gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        // A, updated in place twice to the moved strands (the second becomes a full build: rebuild policy 2).
        if (layouts[0].as)
            for (int u = 0; u < 2; ++u)
            {
                VkAccelerationStructureBuildGeometryInfoKHR update = infos[0];
                data[0].vertexData.deviceAddress = a1.address;
                data[0].radiusData.deviceAddress = a1.address + 12;
                update.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
                update.srcAccelerationStructure = layouts[0].as;
                buildThrough(update, &range[0]);
                gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            }
        // TLAS: the four layouts with culling ON (the patch turns it off) and the plain-data-access mesh (left alone).
        std::vector<VkAccelerationStructureInstanceKHR> instancesB;
        for (const auto& L : layouts)
        {
            if (!L.as) continue;
            VkAccelerationStructureInstanceKHR instance{};
            instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
            instance.transform.matrix[0][3] = L.offsetX;
            instance.instanceCustomIndex = L.instanceId;
            instance.mask = L.mask;
            instance.flags = 0;
            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            addressInfo.accelerationStructure = L.as;
            instance.accelerationStructureReference = gpu.asAddress(gpu.device, &addressInfo);
            instancesB.push_back(instance);
        }
        {
            VkAccelerationStructureInstanceKHR mesh{};
            mesh.transform.matrix[0][0] = mesh.transform.matrix[1][1] = mesh.transform.matrix[2][2] = 1.0f;
            mesh.transform.matrix[0][3] = 16.0f;
            mesh.instanceCustomIndex = blases[2].instanceId;
            mesh.mask = 0x10;
            mesh.flags = 0;
            mesh.accelerationStructureReference = blases[2].address;
            instancesB.push_back(mesh);
        }
        const Buffer instanceBufferB = gpu.Upload(instancesB.data(), instancesB.size() * sizeof(instancesB[0]), kInput);
        VkAccelerationStructureGeometryKHR topB{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        topB.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        topB.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        topB.geometry.instances.data.deviceAddress = instanceBufferB.address;
        VkAccelerationStructureBuildGeometryInfoKHR topInfoB{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        topInfoB.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        topInfoB.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        topInfoB.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        topInfoB.geometryCount = 1;
        topInfoB.pGeometries = &topB;
        const uint32_t instanceCountB = static_cast<uint32_t>(instancesB.size());
        VkAccelerationStructureBuildSizesInfoKHR topSizesB{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        if (options.game) gpu.buildSizes(gpu.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topInfoB, &instanceCountB, &topSizesB);
        else builder.BuildSizes(gpu.buildSizes, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topInfoB, &instanceCountB, &topSizesB);
        tlasBStorage = gpu.Make(topSizesB.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        tlasBScratch = gpu.Make(topSizesB.buildScratchSize + scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR tlasBCreate{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        tlasBCreate.buffer = tlasBStorage.buffer;
        tlasBCreate.size = topSizesB.accelerationStructureSize;
        tlasBCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        CHECK_VK(gpu.createAs(gpu.device, &tlasBCreate, nullptr, &tlasB));
        topInfoB.dstAccelerationStructure = tlasB;
        topInfoB.scratchData.deviceAddress = AlignUp(tlasBScratch.address, scratchAlignment);
        const VkAccelerationStructureBuildRangeInfoKHR topRangeB{instanceCountB, 0, 0, 0};
        buildThrough(topInfoB, &topRangeB);
        gpu.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_TRANSFER_READ_BIT);
        // The patched instance copy, read back.
        VkDeviceSize copyBytes = 0;
        const VkBuffer copy = options.game ? VK_NULL_HANDLE : builder.InstanceCopy(tlasB, copyBytes);
        Buffer readback{};
        if (copy && copyBytes >= instancesB.size() * sizeof(instancesB[0]))
        {
            readback = gpu.Make(instancesB.size() * sizeof(instancesB[0]), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            const VkBufferCopy region{0, 0, instancesB.size() * sizeof(instancesB[0])};
            vkCmdCopyBuffer(gpu.list, copy, readback.buffer, 1, &region);
            gpu.Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        }
        const VkResult built = gpu.Submit();
        gpuFailed = built != VK_SUCCESS;
        if (!gpuFailed && readback.mapped)
        {
            patchRead = true;
            const auto* got = static_cast<const VkAccelerationStructureInstanceKHR*>(readback.mapped);
            for (size_t k = 0; k < instancesB.size(); ++k)
            {
                ++patchedChecked;
                VkAccelerationStructureInstanceKHR want = instancesB[k];
                if (k + 1 < instancesB.size()) want.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; // hair; the mesh is last
                if (std::memcmp(&got[k], &want, sizeof(want)) == 0) ++patchedOk;
            }
        }
        if (!options.game) builderStats = builder.Stats();
        // Rays at each layout's segments, as phase 0's (their own random sequence).
        std::mt19937 randomB(options.seed + 1000);
        auto rangeB = [&](float lo, float hi) { return lo + (hi - lo) * unit(randomB); };
        for (uint32_t k = 0; k < 4; ++k)
        {
            const auto& L = layouts[k];
            if (!L.as) continue;
            for (uint32_t i = 0; i < segments; ++i)
            {
                RayIn ray{};
                ray.mask = L.mask;
                const F3 p = L.p[i].xyz, q = L.q[i].xyz;
                const F3 m = p + (q - p) * rangeB(0.25f, 0.75f);
                const F3 n = Normalize(Cross(Normalize(q - p), Normalize({rangeB(-1, 1), rangeB(-1, 1), rangeB(-1, 1)})));
                ray.origin = m + n * 0.25f;
                ray.origin.x += L.offsetX;
                ray.direction = n * -1.0f;
                ray.tmax = 0.5f;
                raysB.push_back(ray);
                rayLayout.push_back(k);
            }
        }
        if (!gpuFailed && !raysB.empty())
        {
            raysBBuffer = gpu.Upload(raysB.data(), raysB.size() * sizeof(RayIn), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            const uint32_t countB = static_cast<uint32_t>(raysB.size());
            Buffer resultsB[2];
            resultBuffers(sizeof(LssOut), resultsB, countB);
            for (int k = 0; k < 2; ++k) buildPasses[k].created = lssPasses[k].created;
            const SceneRun sceneB{tlasB, &raysBBuffer, countB, countB, 0, 0x0Fu, 0u}; // every layout is hair
            trace(buildPasses, lssCompute, rt1, resultsB, sizeof(LssOut), sceneB);
        }
    }

    // Checks. Expected vertices of a hit primitive, from the CPU buffers each BLAS was built from.
    auto expected = [&](uint32_t b, uint32_t primitive, F3* out) {
        const auto& blas = blases[b];
        if (blas.dots)
        {
            const auto& source = blas.update ? dotsMoved : (blas.tagged ? dotsTagged : dotsPlain);
            for (int k = 0; k < 3; ++k) out[k] = source[primitive * 3 + k];
        }
        else
            for (int k = 0; k < 3; ++k) out[k] = meshVertices[meshIndices[primitive * 3 + k]];
    };
    for (auto& pass : passes)
    {
        if (pass.results.empty()) continue;
        for (uint32_t i = 0; i < rayCount; ++i)
        {
            const uint32_t b = rayBlas[i];
            const auto& blas = blases[b];
            if (b == 3 ? (!pass.plainRan || pass.plainResult != VK_SUCCESS) : pass.mainResult != VK_SUCCESS) continue;
            const HitOut& hit = pass.results[i];
            Group& g = pass.groups[b];
            ++g.rays;
            if (hit.status == kUnwritten) { ++g.unwritten; continue; }
            if (hit.status != 1) continue;
            ++g.hits;
            if (hit.instanceId != blas.instanceId) { ++g.wrongInstance; continue; }
            const uint32_t primitives = blas.dots ? segments * kTriangles : meshTriangles;
            if (hit.primitive >= primitives || hit.geometry != 0) { ++g.wrongInstance; continue; }
            F3 want[3];
            expected(b, hit.primitive, want);
            const F3 got[3] = {hit.v0, hit.v1, hit.v2};
            const bool exact = SameBits(got[0], want[0]) && SameBits(got[1], want[1]) && SameBits(got[2], want[2]);
            if (exact) ++g.exact;
            else
            {
                bool permuted = true;
                for (int k = 0; k < 3 && permuted; ++k)
                    permuted = SameBits(got[k], want[0]) || SameBits(got[k], want[1]) || SameBits(got[k], want[2]);
                if (permuted) ++g.sameSetOtherOrder;
                if (g.samples.size() < 4)
                {
                    char text[256];
                    std::snprintf(text, sizeof(text), "prim %u got (%g %g %g) (%g %g %g) (%g %g %g) want (%g %g %g)", hit.primitive,
                        got[0].x, got[0].y, got[0].z, got[1].x, got[1].y, got[1].z, got[2].x, got[2].y, got[2].z, want[0].x, want[0].y, want[0].z);
                    g.samples.push_back(text);
                }
            }
            if (Dot(got[0], got[0]) == 0 && Dot(got[1], got[1]) == 0 && Dot(got[2], got[2]) == 0) ++g.zero;
            if (hit.tagged) ++g.tagged;
            if (blas.dots)
            {
                const uint32_t segment = hit.primitive / kTriangles;
                F4 p, q;
                DotsDecode(got[0], got[1], got[2], hit.primitive, p, q);
                const F4 tp = blas.update ? movedP[segment] : truthP[segment], tq = blas.update ? movedQ[segment] : truthQ[segment];
                auto posErr = [](F3 a, F3 b) { return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}); };
                g.posError = std::max<double>({g.posError, posErr(p.xyz, tp.xyz), posErr(q.xyz, tq.xyz)});
                g.radiusError = std::max<double>({g.radiusError, std::fabs(p.w - tp.w), std::fabs(q.w - tq.w)});
                const F3 sp{hit.p[0], hit.p[1], hit.p[2]}, sq{hit.q[0], hit.q[1], hit.q[2]};
                g.shaderPosError = std::max<double>({g.shaderPosError, posErr(sp, tp.xyz), posErr(sq, tq.xyz)});
                g.shaderRadiusError = std::max<double>({g.shaderRadiusError, std::fabs(hit.p[3] - tp.w), std::fabs(hit.q[3] - tq.w)});
            }
        }
    }

    // A hit on hair (tagged DOTS, or an LSS layout of phase 1b): the rewritten shader must see an LSS hit on the
    // segment it hit, with that segment's endpoints, a hit distance on the round strand (first entry, by the CPU's
    // independent swept-sphere distance) or, where the ray only grazes a ribbon outside the strand, the ribbon's own
    // distance, and u on the segment.
    auto checkHair = [&](const LssOut& hit, const RayIn& ray, float offsetX, const std::vector<F4>& P, const std::vector<F4>& Q,
                         const std::vector<F3>& dots, LssGroup& g, const auto& sample) {
        const double o[3] = {double(ray.origin.x) - offsetX, ray.origin.y, ray.origin.z};
        const double d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
        if (!hit.isLss || hit.primitive >= P.size())
        {
            ++g.wrong;
            sample("not an LSS hit on a segment");
            return;
        }
        const F4 tp = P[hit.primitive], tq = Q[hit.primitive];
        const F4 sp = Shell(tp), sq = Shell(tq); // the strand hits are reported on
        const double decode = std::max({std::fabs(hit.p[0] - tp.xyz.x), std::fabs(hit.p[1] - tp.xyz.y), std::fabs(hit.p[2] - tp.xyz.z),
            std::fabs(hit.q[0] - tq.xyz.x), std::fabs(hit.q[1] - tq.xyz.y), std::fabs(hit.q[2] - tq.xyz.z), std::fabs(hit.p[3] - tp.w),
            std::fabs(hit.q[3] - tq.w)});
        g.decodeError = std::max(g.decodeError, decode);
        if (decode < 2e-4) ++g.segmentOk;
        else sample("endpoints differ from the segment");
        auto at = [&](double t, double x[3]) { for (int c = 0; c < 3; ++c) x[c] = o[c] + d[c] * t; };
        // Every probe ray has TMin 0. A ray that starts inside this segment's strand (it passes close to
        // a neighbouring strand) has no entry to find.
        double start[3];
        at(0.0, start);
        const bool inside = SweptDistance(start, sp, sq) <= 0;
        bool enters = false;
        for (int s = 1; s <= 4000 && !enters && !inside; ++s)
        {
            double x[3];
            at(ray.tmax * s / 4000.0, x);
            enters = SweptDistance(x, sp, sq) < 0;
        }
        double x[3];
        at(hit.t, x);
        if (!enters)
        {
            // No entry from outside (a ribbon grazed outside the strand, or a start inside it): the shader
            // must keep the ribbon's own distance, that of one of the segment's DOTS triangles.
            ++g.fallback;
            if (inside) ++g.inside;
            bool ribbon = false;
            for (uint32_t tri = 0; tri < kTriangles && !ribbon; ++tri)
            {
                const F3* v = &dots[(static_cast<size_t>(hit.primitive) * kTriangles + tri) * 3];
                double tt, b1, b2;
                ribbon = RayTriangle(o, d, v[0], v[1], v[2], tt, b1, b2) && std::fabs(tt - hit.t) < 1e-4;
            }
            if (ribbon) ++g.fallbackOk;
            else sample(inside ? "starts inside the strand, distance is not a ribbon's" : "fallback distance is not a ribbon's");
        }
        else
        {
            ++g.expectSurface;
            const double surface = std::fabs(SweptDistance(x, sp, sq));
            g.surfaceError = std::max(g.surfaceError, surface);
            bool earlier = false;
            for (int s = 0; s < 400 && hit.t > 1e-4; ++s)
            {
                double y[3];
                at((hit.t - 1e-4) * s / 400.0, y);
                earlier = earlier || SweptDistance(y, sp, sq) < -1e-6;
            }
            if (surface < 5e-5 && !earlier) ++g.surfaceOk;
            else sample(earlier ? "an earlier entry into the strand" : "hit distance off the strand surface");
        }
        const double axis[3] = {double(tq.xyz.x) - tp.xyz.x, double(tq.xyz.y) - tp.xyz.y, double(tq.xyz.z) - tp.xyz.z};
        const double len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
        const double along = ((x[0] - tp.xyz.x) * axis[0] + (x[1] - tp.xyz.y) * axis[1] + (x[2] - tp.xyz.z) * axis[2]) / len2;
        const double u = std::clamp(along, 0.0, 1.0);
        g.uError = std::max({g.uError, std::fabs(hit.bary[0] - u), std::fabs(hit.hitParam - u), std::fabs(double(hit.bary[1]))});
    };

    // Phase 1a checks (the D3D12 probe's): tagged DOTS by checkHair; everything else a plain triangle hit, exactly as
    // without the rewrite.
    for (auto& pass : lssPasses)
    {
        if (pass.results.empty()) continue;
        for (uint32_t i = 0; i < rayCount; ++i)
        {
            const uint32_t b = rayBlas[i];
            const auto& blas = blases[b];
            if (b == 3 ? (!pass.plainRan || pass.plainResult != VK_SUCCESS) : pass.mainResult != VK_SUCCESS) continue;
            const LssOut& hit = pass.results[i];
            LssGroup& g = pass.groups[b];
            ++g.rays;
            if (hit.status == kUnwritten) { ++g.unwritten; continue; }
            if (hit.status != 1) continue;
            ++g.hits;
            auto sample = [&](const char* what) {
                if (g.samples.size() >= 4) return;
                char text[256];
                std::snprintf(text, sizeof(text), "ray %u: %s (isLss %u prim %u t %g bary %g %g hitParam %g)", i, what, hit.isLss, hit.primitive, hit.t,
                    hit.bary[0], hit.bary[1], hit.hitParam);
                g.samples.push_back(text);
            };
            if (hit.instanceId != blas.instanceId) { ++g.wrong; sample("wrong instance"); continue; }
            if (hit.isLss) ++g.lss;
            const RayIn& ray = rays[i];
            const double o[3] = {double(ray.origin.x) - blas.offsetX, ray.origin.y, ray.origin.z};
            const double d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
            if (blas.tagged)
                checkHair(hit, ray, blas.offsetX, blas.update ? movedP : truthP, blas.update ? movedQ : truthQ, blas.update ? dotsMoved : dotsTagged, g,
                    sample);
            else
            {
                if (hit.isLss) { ++g.wrong; sample("LSS hit on plain triangles"); continue; }
                const uint32_t primitives = blas.dots ? segments * kTriangles : meshTriangles;
                if (hit.primitive >= primitives) { ++g.wrong; sample("primitive out of range"); continue; }
                F3 v[3];
                expected(b, hit.primitive, v);
                double t, b1, b2;
                if (!RayTriangle(o, d, v[0], v[1], v[2], t, b1, b2)) { sample("ray misses the reported triangle"); continue; }
                // The hit point the shader's barycentrics describe, against the exact one (scene units): a ribbon
                // triangle near a tapered tip is micro-units wide, so its barycentrics alone amplify float32
                // rounding by the inverse of that width.
                const auto point = [&](double w1, double w2, int c) {
                    const double a = c == 0 ? v[0].x : c == 1 ? v[0].y : v[0].z, e1 = (c == 0 ? v[1].x : c == 1 ? v[1].y : v[1].z) - a,
                                 e2 = (c == 0 ? v[2].x : c == 1 ? v[2].y : v[2].z) - a;
                    return a + w1 * e1 + w2 * e2;
                };
                double error = std::fabs(hit.t - t);
                for (int c = 0; c < 3; ++c) error = std::max(error, std::fabs(point(hit.bary[0], hit.bary[1], c) - point(b1, b2, c)));
                g.triangleError = std::max(g.triangleError, error);
                if (error < 1e-4) ++g.triangleOk;
                else sample("triangle hit changed");
            }
        }
    }

    // Phase 1b checks: every layout but the unsupported one is hair; the unsupported one must never be hit.
    for (int k = 0; k < 2; ++k)
    {
        const LssPass& pass = buildPasses[k];
        if (pass.results.empty() || pass.mainResult != VK_SUCCESS) continue;
        for (uint32_t i = 0; i < raysB.size(); ++i)
        {
            LayoutCase& L = layouts[rayLayout[i]];
            const LssOut& hit = pass.results[i];
            LssGroup& g = L.groups[k];
            ++g.rays;
            if (hit.status == kUnwritten) { ++g.unwritten; continue; }
            if (hit.status != 1) continue;
            ++g.hits;
            auto sample = [&](const char* what) {
                if (g.samples.size() >= 4) return;
                char text[256];
                std::snprintf(text, sizeof(text), "ray %u: %s (isLss %u prim %u t %g bary %g %g hitParam %g)", i, what, hit.isLss, hit.primitive, hit.t,
                    hit.bary[0], hit.bary[1], hit.hitParam);
                g.samples.push_back(text);
            };
            if (hit.instanceId != L.instanceId) { ++g.wrong; sample("wrong instance"); continue; }
            if (hit.isLss) ++g.lss;
            if (rayLayout[i] == 3) { ++g.wrong; sample("an unsupported layout was hit (it must be inactive)"); continue; }
            checkHair(hit, raysB[i], L.offsetX, L.p, L.q, L.dots, g, sample);
        }
    }

    // Verdict.
    bool anyCreated = false;
    for (const auto& pass : passes)
    {
        if (!pass.created) { failures.push_back(std::string(pass.name) + ": not created: " + pass.error); continue; }
        anyCreated = true;
        if (!pass.mainRan) { failures.push_back(std::string(pass.name) + ": not traced (an earlier submission failed)"); continue; }
        if (pass.mainResult != VK_SUCCESS) { failures.push_back(std::string(pass.name) + ": " + resultText("trace failed", pass.mainResult)); continue; }
        for (uint32_t b = 0; b < kBlasCount; ++b)
        {
            const auto& g = pass.groups[b];
            const auto& blas = blases[b];
            if (blas.informational) continue; // what fetch returns without ALLOW_DATA_ACCESS: reported only
            const std::string where = std::string(pass.name) + "/" + blas.name + ": ";
            if (g.unwritten) failures.push_back(where + "results never written");
            if (g.hits * 10 < g.rays * 9) failures.push_back(where + "fewer than 90% of rays hit");
            if (g.wrongInstance) failures.push_back(where + "hits on the wrong instance, geometry or primitive");
            if (g.exact != g.hits - g.wrongInstance) failures.push_back(where + "fetched positions not bit-exact");
            if (blas.dots && (g.shaderPosError > 2e-4 || g.shaderRadiusError > 1e-4)) failures.push_back(where + "decode error over tolerance");
            if (blas.tagged ? g.tagged != g.hits - g.wrongInstance : g.tagged != 0) failures.push_back(where + "tag detection wrong");
        }
    }
    // Phase 1a verdict.
    for (const auto& pass : lssPasses)
    {
        if (!pass.rewritten) { failures.push_back(std::string(pass.name) + ": " + pass.error); continue; }
        if (!pass.created) { failures.push_back(std::string(pass.name) + ": rewritten shader refused: " + pass.error); continue; }
        if (!pass.mainRan) { failures.push_back(std::string(pass.name) + ": not traced (an earlier submission failed)"); continue; }
        if (pass.mainResult != VK_SUCCESS) { failures.push_back(std::string(pass.name) + ": " + resultText("trace failed", pass.mainResult)); continue; }
        for (uint32_t b = 0; b < kBlasCount; ++b)
        {
            const auto& g = pass.groups[b];
            const auto& blas = blases[b];
            if (blas.informational) continue;
            const std::string where = std::string(pass.name) + "/" + blas.name + ": ";
            if (g.unwritten) failures.push_back(where + "results never written");
            if (g.hits * 10 < g.rays * 9) failures.push_back(where + "fewer than 90% of rays hit");
            if (g.wrong) failures.push_back(where + "wrong hits (see samples)");
            if (blas.tagged)
            {
                if (g.lss != g.hits) failures.push_back(where + "not every hit reported as LSS");
                if (g.segmentOk != g.hits) failures.push_back(where + "endpoints not decoded within 2e-4");
                if (g.surfaceOk != g.expectSurface) failures.push_back(where + "hit distance not on the strand surface");
                if (g.fallback * 100 > g.hits * 15) failures.push_back(where + "more than 15% ribbon-only hits");
                if (g.fallbackOk != g.fallback) failures.push_back(where + "a ribbon-only hit without the ribbon's distance");
                if (g.uError > 2e-3) failures.push_back(where + "u / barycentrics off");
            }
            else
            {
                if (g.lss) failures.push_back(where + "plain triangles reported as LSS");
                if (g.triangleOk != g.hits - g.wrong) failures.push_back(where + "triangle hits changed by the rewrite");
            }
        }
    }
    // Phase 1b verdict.
    if (!builderReady) failures.push_back("build: DOTS builder not ready: " + builderError);
    else if (lssPasses[0].created || lssPasses[1].created)
    {
        if (!options.game && restores != builderCalls) failures.push_back("build: the probe's compute pipeline was not bound again after every build");
        for (int k = 0; k < 4; ++k)
        {
            const auto& L = layouts[k];
            const std::string where = std::string("build/") + L.name + ": ";
            if (L.result != VK_SUCCESS || !L.as) { failures.push_back(where + "acceleration structure not created (" + std::to_string(L.result) + ")"); continue; }
            for (int pIndex = 0; pIndex < 2; ++pIndex)
            {
                if (!buildPasses[pIndex].created) continue;
                const auto& g = L.groups[pIndex];
                const std::string at = where + buildPasses[pIndex].name + ": ";
                if (!buildPasses[pIndex].mainRan || buildPasses[pIndex].mainResult != VK_SUCCESS) { failures.push_back(at + "not traced"); continue; }
                if (g.unwritten) failures.push_back(at + "results never written");
                if (k == 3)
                {
                    if (g.hits) failures.push_back(at + "an unsupported layout was hit (it must be inactive)");
                    continue;
                }
                if (g.hits * 10 < g.rays * 9) failures.push_back(at + "fewer than 90% of rays hit");
                if (g.wrong) failures.push_back(at + "wrong hits (see samples)");
                if (g.lss != g.hits) failures.push_back(at + "not every hit reported as LSS");
                if (g.segmentOk != g.hits) failures.push_back(at + "endpoints not decoded within 2e-4");
                if (g.surfaceOk != g.expectSurface) failures.push_back(at + "hit distance not on the strand surface");
                if (g.fallbackOk != g.fallback) failures.push_back(at + "a ribbon-only hit without the ribbon's distance");
                if (g.fallback * 100 > g.hits * 15) failures.push_back(at + "more than 15% ribbon-only hits");
                if (g.uError > 2e-3) failures.push_back(at + "u / barycentrics off");
            }
        }
        if (!options.game)
        {
            if (builderStats.invalidGeometries != 1) failures.push_back("build: expected exactly one rejected (inactive) geometry");
            if (builderStats.rebuilds != 1 || builderStats.updates != 1)
                failures.push_back("build: layout A's first update was not a refit, or its second not a full rebuild in place");
            if (builderStats.topLevelPatched != 1) failures.push_back("build: the top-level build was not patched exactly once");
            if (!patchRead) failures.push_back("patch: the instance copy could not be read back");
            else if (patchedChecked == 0 || patchedOk != patchedChecked)
                failures.push_back("patch: hair instances' culling not disabled exactly (or another byte changed)");
            if (builderStats.failures) failures.push_back("build: builder failures: " + builder.LastError());
        }
    }
    const uint32_t otherErrors = gMessages.errors - lssQueryErrors;
    if (validation && otherErrors) failures.push_back("validation: " + std::to_string(otherErrors) + " error message(s) (see validation.samples)");
    const int exitCode = !anyCreated ? 3 : (failures.empty() ? 0 : 1);

    // Report.
    std::ostringstream json;
    json << "{\n  \"adapter\": \"" << Json(basic.deviceName) << "\", \"vendorId\": " << basic.vendorID << ", \"deviceId\": " << basic.deviceID
         << ", \"driver\": \"" << driverText << "\", \"driverName\": \"" << Json(driverProperties.driverName) << "\", \"driverInfo\": \""
         << Json(driverProperties.driverInfo) << "\", \"apiVersion\": \"" << VK_API_VERSION_MAJOR(basic.apiVersion) << "."
         << VK_API_VERSION_MINOR(basic.apiVersion) << "." << VK_API_VERSION_PATCH(basic.apiVersion) << "\", \"luid\": \""
         << (idProperties.deviceLUIDValid ? Hex(idProperties.deviceLUID, VK_LUID_SIZE) : std::string()) << "\", \"seed\": " << options.seed << ", \"game\": " << (options.game ? "true" : "false")
         << ", \"featureOnly\": " << (options.featureOnly ? "true" : "false")
         << ",\n  \"segments\": " << segments << ", \"meshTriangles\": " << meshTriangles << ", \"rays\": " << rayCount
         << ",\n  \"extensions\": {\"positionFetch\": " << (hasFetch ? "true" : "false") << ", \"positionFetchVersion\": " << fetchVersion
         << ", \"linearSweptSpheres\": " << (hasLss ? "true" : "false") << ", \"linearSweptSpheresVersion\": " << lssVersion
         << "},\n  \"features\": {\"rayTracingPositionFetch\": " << (fetchFeatures.rayTracingPositionFetch ? "true" : "false")
         << ", \"lssStructSpheres\": " << lssValue(lssFeatures.spheres) << ", \"lssStructLinearSweptSpheres\": " << lssValue(lssFeatures.linearSweptSpheres)
         << ", \"lssQueryMessages\": " << lssQueryMessages << "},\n  \"blas\": [";
    for (uint32_t i = 0; i < kBlasCount; ++i)
        json << (i ? ", " : "") << "\n    {\"name\": \"" << blases[i].name << "\", \"mask\": " << blases[i].mask << ", \"dataAccess\": "
             << (blases[i].dataAccess ? "true" : "false") << ", \"updated\": " << (blases[i].update ? "true" : "false")
             << ", \"bytesWithDataAccess\": " << blases[i].sizeWith << ", \"bytesWithout\": " << blases[i].sizeWithout
             << ", \"scratchWithDataAccess\": " << blases[i].scratchWith << ", \"scratchWithout\": " << blases[i].scratchWithout
             << ", \"updateScratch\": " << blases[i].updateScratch << "}";
    json << "\n  ],\n  \"passes\": [";
    for (size_t p = 0; p < 2; ++p)
    {
        const auto& pass = passes[p];
        json << (p ? ",\n" : "\n") << "    {\"name\": \"" << pass.name << "\", \"created\": " << (pass.created ? "true" : "false")
             << ", \"error\": \"" << Json(pass.error) << "\", \"mainRan\": " << (pass.mainRan ? "true" : "false") << ", \"mainResult\": " << pass.mainResult
             << ", \"plainRan\": " << (pass.plainRan ? "true" : "false") << ", \"plainResult\": " << pass.plainResult
             << ", \"groups\": [";
        for (uint32_t b = 0; b < kBlasCount; ++b)
        {
            const auto& g = pass.groups[b];
            json << (b ? ", " : "") << "\n      {\"blas\": \"" << blases[b].name << "\", \"informational\": " << (blases[b].informational ? "true" : "false")
                 << ", \"rays\": " << g.rays << ", \"hits\": " << g.hits << ", \"unwritten\": " << g.unwritten << ", \"wrongInstance\": " << g.wrongInstance
                 << ", \"exact\": " << g.exact << ", \"sameSetOtherOrder\": " << g.sameSetOtherOrder << ", \"zero\": " << g.zero << ", \"tagged\": " << g.tagged
                 << ", \"posError\": " << g.posError << ", \"radiusError\": " << g.radiusError << ", \"shaderPosError\": " << g.shaderPosError
                 << ", \"shaderRadiusError\": " << g.shaderRadiusError << ", \"samples\": [";
            for (size_t s = 0; s < g.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(g.samples[s]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ],\n  \"lss\": [";
    for (size_t p = 0; p < 2; ++p)
    {
        const auto& pass = lssPasses[p];
        const auto& r = pass.report;
        json << (p ? ",\n" : "\n") << "    {\"name\": \"" << pass.name << "\", \"rewrite\": \"" << RewriteName(pass.result) << "\", \"words\": ["
             << pass.originalWords << ", " << pass.spirv.size() << "], \"report\": {\"hitEntryPoints\": " << r.hitEntryPoints << ", \"builtIns\": "
             << r.builtIns << ", \"ops\": " << r.ops << ", \"reads\": " << r.reads << ", \"queryContexts\": " << r.queryContexts << ", \"note\": \""
             << Json(r.note) << "\"}, \"created\": " << (pass.created ? "true" : "false") << ", \"error\": \"" << Json(pass.error) << "\", \"mainRan\": "
             << (pass.mainRan ? "true" : "false") << ", \"mainResult\": " << pass.mainResult << ", \"plainRan\": " << (pass.plainRan ? "true" : "false")
             << ", \"plainResult\": " << pass.plainResult << ", \"groups\": [";
        for (uint32_t b = 0; b < kBlasCount; ++b)
        {
            const auto& g = pass.groups[b];
            json << (b ? ", " : "") << "\n      {\"blas\": \"" << blases[b].name << "\", \"informational\": " << (blases[b].informational ? "true" : "false")
                 << ", \"rays\": " << g.rays << ", \"hits\": " << g.hits << ", \"unwritten\": " << g.unwritten << ", \"lss\": " << g.lss << ", \"wrong\": " << g.wrong
                 << ", \"segmentOk\": " << g.segmentOk << ", \"expectSurface\": " << g.expectSurface << ", \"surfaceOk\": " << g.surfaceOk << ", \"fallback\": "
                 << g.fallback << ", \"inside\": " << g.inside << ", \"fallbackOk\": " << g.fallbackOk << ", \"triangleOk\": " << g.triangleOk
                 << ", \"decodeError\": " << g.decodeError << ", \"surfaceError\": " << g.surfaceError << ", \"uError\": " << g.uError << ", \"triangleError\": "
                 << g.triangleError << ", \"samples\": [";
            for (size_t s = 0; s < g.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(g.samples[s]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ],\n  \"build\": {\"ready\": " << (builderReady ? "true" : "false") << ", \"error\": \"" << Json(builderError) << "\", \"lastError\": \""
         << Json(builderReady ? builder.LastError() : std::string()) << "\", \"calls\": " << builderCalls << ", \"restores\": " << restores
         << ", \"stats\": {\"sizeQueries\": " << builderStats.sizeQueries << ", \"builds\": " << builderStats.builds << ", \"updates\": " << builderStats.updates
         << ", \"rebuilds\": " << builderStats.rebuilds << ", \"geometries\": " << builderStats.geometries << ", \"primitives\": " << builderStats.primitives
         << ", \"invalidGeometries\": " << builderStats.invalidGeometries << ", \"failures\": " << builderStats.failures << ", \"topLevelPatched\": "
         << builderStats.topLevelPatched << ", \"hairBlas\": " << builderStats.hairBlas << ", \"poolBuffers\": " << builderStats.poolBuffers
         << ", \"poolBytes\": " << builderStats.poolBytes << "}, \"patch\": {\"read\": " << (patchRead ? "true" : "false") << ", \"instancesOk\": " << patchedOk
         << ", \"instances\": " << patchedChecked << "}, \"layouts\": [";
    for (int k = 0; k < 4; ++k)
    {
        const auto& L = layouts[k];
        json << (k ? ",\n" : "\n") << "    {\"name\": \"" << L.name << "\", \"result\": " << L.result << ", \"passes\": [";
        for (int pIndex = 0; pIndex < 2; ++pIndex)
        {
            const auto& g = L.groups[pIndex];
            json << (pIndex ? ", " : "") << "\n      {\"pass\": \"" << buildPasses[pIndex].name << "\", \"rays\": " << g.rays << ", \"hits\": " << g.hits
                 << ", \"unwritten\": " << g.unwritten << ", \"lss\": " << g.lss << ", \"wrong\": " << g.wrong << ", \"segmentOk\": " << g.segmentOk
                 << ", \"expectSurface\": " << g.expectSurface << ", \"surfaceOk\": " << g.surfaceOk << ", \"fallback\": " << g.fallback << ", \"inside\": "
                 << g.inside << ", \"fallbackOk\": " << g.fallbackOk << ", \"decodeError\": " << g.decodeError << ", \"surfaceError\": " << g.surfaceError
                 << ", \"uError\": " << g.uError << ", \"samples\": [";
            for (size_t sIndex = 0; sIndex < g.samples.size(); ++sIndex) json << (sIndex ? ", " : "") << "\"" << Json(g.samples[sIndex]) << "\"";
            json << "]}";
        }
        json << "]}";
    }
    json << "\n  ]},\n  \"validation\": {\"requested\": " << (options.validation ? "true" : "false") << ", \"enabled\": " << (validation ? "true" : "false")
         << ", \"errors\": " << gMessages.errors << ", \"warnings\": " << gMessages.warnings << ", \"lssQueryErrors\": " << lssQueryErrors << ", \"samples\": [";
    for (size_t s = 0; s < gMessages.samples.size(); ++s) json << (s ? ", " : "") << "\"" << Json(gMessages.samples[s]) << "\"";
    json << "]},\n  \"failures\": [";
    for (size_t i = 0; i < failures.size(); ++i) json << (i ? ", " : "") << "\"" << Json(failures[i]) << "\"";
    json << "],\n  \"exitCode\": " << exitCode << "\n}\n";
    const std::string report = json.str();
    std::fwrite(report.data(), 1, report.size(), stdout);
    if (!options.out.empty())
    {
        std::ofstream file(options.out, std::ios::binary);
        file << report;
        if (!file) Fail(2, "cannot write %ls", options.out.c_str());
    }
    std::fprintf(stderr, "RtxHairVkProbe: %s (exit %d)\n", exitCode == 0 ? "PASS" : (exitCode == 3 ? "position fetch unavailable" : "FAIL"), exitCode);

    // Teardown (skipped after a failed submission: the driver owns what is left).
    if (!gpuFailed)
    {
        vkDeviceWaitIdle(gpu.device);
        for (const VkPipeline p : pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        builder.Shutdown();
        for (const auto& L : layouts)
            if (L.as) gpu.destroyAs(gpu.device, L.as, nullptr);
        if (tlasB) gpu.destroyAs(gpu.device, tlasB, nullptr);
        gpu.destroyAs(gpu.device, tlas, nullptr);
        for (auto& blas : blases) gpu.destroyAs(gpu.device, blas.as, nullptr);
        for (auto& b : gpu.owned)
        {
            vkDestroyBuffer(gpu.device, b.buffer, nullptr);
            vkFreeMemory(gpu.device, b.memory, nullptr);
        }
    }
    return exitCode;
}
