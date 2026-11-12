#pragma once
#include "dlss5.h"
#include "dlss_sr_policy.h"
#include <Windows.h>
#include <nvsdk_ngx.h>

// dx11nr.1: DLSS 5 (DLSSNR) on DirectX 11 games, through a bridge to a DirectX 12 device of ours.
//
// The game's DLSS SR/DLAA/RR features on the NGX runtime's D3D11 exports are tracked as kD3D11Route slots of
// dlss5.cpp. On an evaluation DLSS 5 owns, the bridge copies the game's color (After upscaling: the DLSS output;
// Before upscaling: its input), depth (as R32_FLOAT, by a compute pass), motion vectors and exposure into textures
// shared with its own D3D12 device on the same adapter, runs the D3D12 stage on them (with the game's evaluation keys)
// on its own queue between fences shared with the game's immediate context, and copies the result back (After) or
// substitutes it as the game's DLSS input for that call (Before). The device, its queue and its NGX initialization
// are made on the first owned evaluation, never under the loader lock, and kept for the process; a failed NGX
// initialization keeps the bridge off. Present is not available on DirectX 11 (it releases as Off does).
// Each tracked game feature has its own shared textures, retired with its release once their work completed.
// Nothing crosses the APIs while the D3D12 stage would refuse the evaluation (dlss5::shared::CanEvaluate).
//
// Every D3D11 compute state the depth pass touches (shader, shader resource 0, unordered access view 0) is restored,
// and the game's predicate is suspended around the bridge's D3D11 work and put back.
namespace dlss5::d3d11 {
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
    NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
// The runtime's NVSDK_NGX_D3D12_Init_Ext.
using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
    const NVSDK_NGX_Parameter*);
using InitResolver = InitFn (*)(HMODULE runtime) noexcept;
struct Api {
    HMODULE runtime = nullptr;
    // The game's D3D11 Evaluate (the original behind our hook).
    EvaluateFn evaluate = nullptr;
    // The same runtime's D3D12 entries for DLSS 5's own feature on the bridge device (trampolines, so that feature
    // never enters our runtime hooks).
    dlss5::Api d3d12{};
    // Its D3D12 Init_Ext for the bridge device, resolved once, when the device is made.
    InitResolver init = nullptr;
};
// A game SR/RR Create on the D3D11 route succeeded (its keys read before the call).
void GameCreated(const NVSDK_NGX_Handle* handle, const dlss_sr::CreateKeys& keys) noexcept;
// A game SR/RR Create on the D3D11 route while not all its entries were ours (another hook over them): DLSS 5 does
// not take the handle; the reason is published (Refusal::eUntracked).
void UntrackedCreate() noexcept;
// The game's D3D11 evaluation: `api.evaluate` exactly once, with the bridge around it when DLSS 5 owns `handle`.
// Returns the game's result.
NVSDK_NGX_Result Evaluate(const Api& api, ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) noexcept;
// Around the game's D3D11 Release. BeforeRelease returns whether DLSS 5 owned the handle; pass it to AfterRelease.
bool BeforeRelease(const NVSDK_NGX_Handle* handle) noexcept;
void AfterRelease(const NVSDK_NGX_Handle* handle, NVSDK_NGX_Result result, bool owned) noexcept;
#if defined(MFG_DLSS5_TESTING)
namespace testing {
// The depth pass alone: `source` (a game depth texture, mip 0) as R32_FLOAT in `target` (a UAV-capable texture of
// the same size) on `context`, whose compute state and predicate come back as they were. False when refused.
bool ConvertDepth(ID3D11DeviceContext* context, ID3D11Resource* source, ID3D11Resource* target) noexcept;
// Evaluations the bridge copied back into the game's output (After) or substituted as its input (Before).
uint64_t Processed() noexcept;
// The current refusal (dlss5_policy.h d3d11::Refusal).
uint32_t Refused() noexcept;
// Tracked game features whose shared textures are held.
size_t Sets() noexcept;
}
#endif
}
