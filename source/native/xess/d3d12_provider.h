#pragma once
// ngxprov.1: XeMFG as a D3D12 DLSS-G provider (vulkan/mfg_xemfg_provider.h), the Vulkan provider's twin
// (vulkan/vk_provider.h); NVSDK_NGX_D3D12_* calls reach it through vulkan/xemfg_provider_inprocess.cpp. XeFG's internal
// frame generator records straight into the evaluation's command list on the game's device; Streamline's DLSS-G
// presents and paces the frames.
//
// Per evaluation (DLSSG.MultiFrameIndex 1..DLSSG.MultiFrameCount, one generated frame each):
//  index 1  the frame's back buffer, depth, motion vectors and HUD-less color are copied into one of two input slots
//           (the other holds the previous frame), then XeFG's Execute records every interpolated frame of the batch
//           into the provider's outputs;
//  index k  output k is copied into DLSSG.OutputInterpolated.
// The back buffer is also copied into DLSSG.OutputReal, which Streamline presents after the generated frames. Nothing
// is submitted here. XeFG reads depth and motion vectors over the game's DLSSG.DepthSubrect* and DLSSG.MVecsSubrect*
// rectangles: a texture can be larger than the picture it holds (ngx_provider::ChooseDepthMotionRects).
//
// Resource states: Streamline leaves the back buffer in NON_PIXEL_SHADER_RESOURCE, depth, motion vectors and HUD-less
// color in COPY_DEST, and OutputInterpolated and OutputReal in UNORDERED_ACCESS (as OptiScaler's own FFX provider and
// dlssg-to-fsr3 take them); every one goes back to that state after its copy. XeFG's Execute takes its inputs in
// NON_PIXEL_SHADER_RESOURCE and its outputs in UNORDERED_ACCESS (its own swap chain's barriers around the call:
// libxess_fg 1.3.1.78, 0x20B290 -> 0x21A0F0, outputs COMMON -> UNORDERED_ACCESS -> COMMON).
//
// GPU lifetime: a released feature, or one re-created for new inputs, keeps its textures and XeFG's context for 2 s
// before they are released (OptiScaler's own providers wait the same, Util::DelayedDestroy): nothing here knows the
// queue the evaluation runs on.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

namespace xess_d3d12_provider
{
// The API's D3D12 entries; every call returns an NVSDK_NGX_Result.
int32_t Init(ID3D12Device* device);
int32_t CreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, void** feature);
int32_t Evaluate(ID3D12GraphicsCommandList* list, void* feature, NVSDK_NGX_Parameter* parameters);
int32_t ReleaseFeature(void* feature);
int32_t Shutdown();
}
