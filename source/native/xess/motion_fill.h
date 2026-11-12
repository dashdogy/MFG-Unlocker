#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <xefg_swapchain_d3d12.h>

namespace xess_motion_fill
{
// Witcher's own XeFG motion vectors (RGBA16F, render resolution) hold a (2,2,0,0) marker
// wherever the game wrote no velocity: all sky and some near geometry (35-57% of the screen in
// captures). XeFG reads it as two screen widths of motion and presents copies of the
// nearest real frame. Its valid vectors are pure camera motion (capture fit, corr 1.0000):
//   MV.x = 0.5 * (cur_ndc.x - prev_ndc.x),  MV.y = 0.5 * (prev_ndc.y - cur_ndc.y),  w = 1.
// The filler gives marker pixels that camera motion, from the depth and the view/projection
// matrices XeFG receives (row-vector convention: clip = [x y z 1] * View * Projection; depth 0 =
// infinitely far, a direction).
//
// Flow, per frame:
//  - depth and motion tags (game thread, the game's command list): copies into our buffers only
//    (resource barriers on the game's resource and copies; no pipeline or descriptor state);
//  - right before the game's Present: our own command list on the presentation queue fills the
//    marker, writes our motion texture, and the caller tags that texture UNTIL_NEXT_PRESENT.
// The motion tag is consumed only when everything for the frame is ready; otherwise the caller
// forwards the game's own tag unchanged.
enum class Mode : uint32_t { eGame = 0, eFill = 1, eZero = 2 };
const char* ModeName(Mode mode) noexcept;

// CPU reference of the fill (tests, and the reprojection the shader receives).
// Row-major 4x4, row-vector convention. False when a matrix is singular.
bool Invert(const float (&m)[16], double (&out)[16]) noexcept;
// cur NDC + depth -> previous clip: inverse(P) * inverse(V) * prevV * prevP.
bool Reprojection(const float (&view)[16], const float (&projection)[16], const float (&prevView)[16],
    const float (&prevProjection)[16], float (&out)[16]) noexcept;
// Witcher's matrix slots: a rigid view and a perspective projection.
bool UsableMatrices(const float (&view)[16], const float (&projection)[16]) noexcept;

class Filler
{
public:
    Filler(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept;
    ~Filler(); // waits (bounded) for its own GPU work before releasing resources
    Filler(const Filler&) = delete;
    Filler& operator=(const Filler&) = delete;

    // The constants XeFG received for `frame` (after the slot fix).
    void OnConstants(uint32_t frame, const float (&view)[16], const float (&projection)[16]) noexcept;
    // Depth tag (forwarded by the caller): records a copy of plane 0 when `mode` fills.
    void OnDepth(ID3D12GraphicsCommandList* list, uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& depth,
        Mode mode) noexcept;
    // Motion tag: true when consumed (copy recorded, Submit tags our texture). False with a reason
    // when the caller must forward the game's tag (mode game, not ready, shape, matrices...).
    bool OnMotion(ID3D12GraphicsCommandList* list, uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& motion,
        Mode mode, const char*& reason) noexcept;
    bool Pending() const noexcept { return pending_; }
    // Before the game's Present: fills on our own list, executes it on the presentation queue and
    // returns the tag (our texture, UNTIL_NEXT_PRESENT) for `frame`. False: nothing was submitted
    // for the frame.
    bool Submit(uint32_t& frame, xefg_swapchain_d3d12_resource_data_t& tag, const char*& reason) noexcept;
    // A failure that disables the filler for the rest of the context (logged once by the caller).
    bool Broken() const noexcept { return broken_; }
    uint64_t Submitted() const noexcept { return submitted_; }

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> depth, motionIn, motionOut, texture;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t fence{};
        uint32_t frame{};
        bool depthCopied{};
        Mode mode{Mode::eGame};
        float reprojection[16]{};
        bool reprojected{};
        xefg_swapchain_2d_t base{}, size{};
    };
    struct Constants { uint32_t frame{}; float view[16]{}, projection[16]{}; bool valid{}; };
    bool Prepare(const D3D12_RESOURCE_DESC& motion, const D3D12_RESOURCE_DESC& depth, const char*& reason) noexcept;
    bool Pipeline(const char*& reason) noexcept;
    bool WaitSlot(const Slot& slot, DWORD milliseconds) noexcept;
    void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES state,
        ID3D12Resource* buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_{};
    uint64_t fenceValue_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    bool pipelineTried_{};
    std::array<Slot, 4> slots_;
    // Resource sets replaced by a size change; released once their fence has passed.
    std::vector<std::pair<uint64_t, std::array<Slot, 4>>> retired_;
    D3D12_RESOURCE_DESC motionDesc_{}, depthDesc_{}; // the shapes the slots were allocated for
    D3D12_RESOURCE_DESC seenDepth_{}; // the depth shape of the last depth tag
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint_{}, depthFootprint_{};
    bool prepared_{};
    uint32_t next_{};
    Slot* current_{}; // the slot of the frame whose depth/motion are being tagged
    bool pending_{}, broken_{};
    uint64_t submitted_{};
    Constants constants_[2]{}; // [frame % 2]
};
}
