#pragma once
// NVAPI x64 ABI for linear swept spheres and the extended acceleration-structure
// builds (R610 nvapi.h), declared here so the DLL needs no NVAPI header. Layouts are
// checked against nvapi.h by tests/rtxhair_probe (static_asserts).
#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace rtx_hair::nvapi {
using Status = int32_t;
inline constexpr Status kOk = 0, kError = -1, kInvalidArgument = -5, kNotSupported = -104;

// nvapi_interface.h ids.
inline constexpr uint32_t kGetRaytracingCaps = 0x85a6c2a0;
inline constexpr uint32_t kSetCreatePipelineStateOptions = 0x5c607a27;
inline constexpr uint32_t kPrebuildInfoEx = 0x8d025b77;
inline constexpr uint32_t kBuildEx = 0xe24ead45;

inline constexpr uint32_t kCapsLinearSweptSpheres = 6; // NVAPI_D3D12_RAYTRACING_CAPS_TYPE
inline constexpr uint32_t kLssCapStandard = 1;         // NVAPI_D3D12_RAYTRACING_LINEAR_SWEPT_SPHERES_CAP_STANDARD
inline constexpr uint32_t kPipelineEnableLss = 1u << 4; // NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_LSS_SUPPORT

// NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX
inline constexpr uint32_t kBuildPerformUpdate = 1u << 5;
inline constexpr uint32_t kBuildAllowDataAccess = 1u << 9;

// NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_EX
inline constexpr uint32_t kGeometryTriangles = 0;
inline constexpr uint32_t kGeometryLss = 5;

// NVAPI_D3D12_RAYTRACING_LSS_PRIMITIVE_FORMAT
inline constexpr uint32_t kLssList = 0;
inline constexpr uint32_t kLssSuccessiveImplicit = 1;

// NVAPI_D3D12_RAYTRACING_GEOMETRY_LSS_DESC
struct LssDesc
{
    uint32_t vertexCount, indexCount, primitiveCount;
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE vertexPositionBuffer;
    DXGI_FORMAT vertexPositionFormat;
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE vertexRadiusBuffer;
    DXGI_FORMAT vertexRadiusFormat;
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE indexBuffer;
    DXGI_FORMAT indexFormat;
    uint32_t endcapMode, primitiveFormat;
};
static_assert(sizeof(LssDesc) == 96 && offsetof(LssDesc, vertexPositionBuffer) == 16 && offsetof(LssDesc, vertexRadiusBuffer) == 40
    && offsetof(LssDesc, indexBuffer) == 64 && offsetof(LssDesc, primitiveFormat) == 88);

// NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX: the union's largest member (DMM triangles) is
// 160 bytes.
inline constexpr size_t kGeometryDescExBytes = 168;
struct GeometryDescEx
{
    uint32_t type;
    D3D12_RAYTRACING_GEOMETRY_FLAGS flags;
    union
    {
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
        LssDesc lss;
        uint8_t bytes[160];
    };
};
static_assert(sizeof(GeometryDescEx) == kGeometryDescExBytes && offsetof(GeometryDescEx, triangles) == 8);

// NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX
struct BuildInputsEx
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type;
    uint32_t flags;
    uint32_t numDescs;
    D3D12_ELEMENTS_LAYOUT descsLayout;
    uint32_t geometryDescStrideInBytes;
    union
    {
        D3D12_GPU_VIRTUAL_ADDRESS instanceDescs;
        const GeometryDescEx* pGeometryDescs;
        const GeometryDescEx* const* ppGeometryDescs;
    };
};
static_assert(sizeof(BuildInputsEx) == 32 && offsetof(BuildInputsEx, pGeometryDescs) == 24);

// NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX
struct BuildDescEx
{
    D3D12_GPU_VIRTUAL_ADDRESS destAccelerationStructureData;
    BuildInputsEx inputs;
    D3D12_GPU_VIRTUAL_ADDRESS sourceAccelerationStructureData;
    D3D12_GPU_VIRTUAL_ADDRESS scratchAccelerationStructureData;
};
static_assert(sizeof(BuildDescEx) == 56);

// NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_V1
struct PrebuildParams
{
    uint32_t version;
    const BuildInputsEx* pDesc;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo;
};
static_assert(sizeof(PrebuildParams) == 24);
inline constexpr uint32_t kPrebuildParamsVersion = sizeof(PrebuildParams) | (1u << 16);

// NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_V1
struct BuildParams
{
    uint32_t version;
    const BuildDescEx* pDesc;
    uint32_t numPostbuildInfoDescs;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs;
};
static_assert(sizeof(BuildParams) == 32);
inline constexpr uint32_t kBuildParamsVersion = sizeof(BuildParams) | (1u << 16);

using PrebuildFn = Status(__cdecl*)(ID3D12Device5*, PrebuildParams*);
using BuildFn = Status(__cdecl*)(ID3D12GraphicsCommandList4*, const BuildParams*);
}
