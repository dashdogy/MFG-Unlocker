#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>
#include <xefg_swapchain_d3d12.h>

namespace xess_hudless_match
{
// XeFG's UI composition (Witcher: ui_mode 4, interpolate the HUD-less colour and extract the UI
// from the back buffer) compares the HUD-less input with the back buffer. Witcher's HUD-less input is
// RGBA16F before its final tone/colour transform while the back buffer is HDR10 (R10G10B10A2, PQ), so
// every pixel differs, XeFG takes the whole screen as UI and presents the real frame (copies).
// On an exactly aligned pair the back buffer is a tight per-channel function of the
// HUD-less value (1D LUT residual p90 0.002-0.010 in code units).
//
// The matcher rebuilds that function every frame from the frame's own pair and gives XeFG a HUD-less
// texture in the back buffer's format and encoding:
//  - HUD-less tag (game thread): consumed; the game's resource stays valid until the next Present.
//  - Right before the game's Present, on our own command list on the presentation queue:
//    1. a 2D histogram per channel: HUD-less value (log2 bins) x back-buffer code (1024 bins), every
//       fourth pixel in both directions;
//    2. per input bin the median back-buffer code (the UI is a minority, so it does not move the median);
//       a bin further than 32 codes from the 5-bin running median is replaced by it (a bin taken over by
//       a HUD panel over a narrow range of scene values); empty bins are interpolated from their neighbours;
//    3. every HUD-less pixel through that curve into our texture (back buffer format).
//  The caller tags our texture UNTIL_NEXT_PRESENT. The mapping is colour-space-agnostic: it maps HUD-less
//  values straight to the back buffer's code values (UNORM back buffers only: 10-bit or 8-bit, not sRGB
//  views or float/scRGB; any other shape fails closed and the caller keeps composition off).
enum class Mode : uint32_t { eGame = 0, eMatch = 1 };
const char* ModeName(Mode mode) noexcept;

constexpr uint32_t kBins = 256, kCodes = 1024;
constexpr float kLogMin = -16.0f, kBinsPerOctave = 12.0f; // 2^-16 .. 2^5.33

class Matcher
{
public:
    Matcher(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept;
    ~Matcher(); // waits (bounded) for its own GPU work
    Matcher(const Matcher&) = delete;
    Matcher& operator=(const Matcher&) = delete;

    // The game's HUD-less tag. True when consumed (Submit tags our texture); false with a reason when the
    // caller must forward the game's tag (mode game, validity, shape...).
    bool OnHudless(uint32_t frame, const xefg_swapchain_d3d12_resource_data_t& hudless, Mode mode, const char*& reason) noexcept;
    bool Pending() const noexcept { return pending_; }
    // Right before the game's Present: backBuffer is the swap chain's current buffer (state PRESENT).
    // False: nothing was tagged for the frame (the caller turns composition off for it).
    bool Submit(ID3D12Resource* backBuffer, uint32_t& frame, xefg_swapchain_d3d12_resource_data_t& tag,
        const char*& reason) noexcept;
    bool Broken() const noexcept { return broken_; }
    uint64_t Submitted() const noexcept { return submitted_; }

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> output;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t fence{};
    };
    bool Pipeline(const char*& reason) noexcept;
    bool Prepare(const D3D12_RESOURCE_DESC& backBuffer, const char*& reason) noexcept;
    bool WaitSlot(const Slot& slot, DWORD milliseconds) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_{};
    uint64_t fenceValue_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 5> pipelines_; // clear, histogram, lut, fill, apply
    bool pipelineTried_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptorSize_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> histogram_, lut_;
    std::array<Slot, 4> slots_;
    std::vector<std::pair<uint64_t, std::array<Slot, 4>>> retired_;
    D3D12_RESOURCE_DESC outputDesc_{};
    bool prepared_{};
    uint32_t next_{};
    // The consumed HUD-less input of the pending frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> hudless_;
    D3D12_RESOURCE_STATES hudlessState_{};
    xefg_swapchain_2d_t base_{}, size_{};
    uint32_t frame_{};
    bool pending_{}, broken_{};
    uint64_t submitted_{};
};
}
