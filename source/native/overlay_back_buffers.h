#pragma once
// xemfglive.3: back buffers of the menu proxy's own for a D3D12 swap chain that may move live (XeMFG standalone).
//
// A game holds its swap chain's back buffers until its next ResizeBuffers, and DXGI frees a chain only once nobody
// holds them, so a chain can otherwise move only inside the game's resize; games that ignore window size changes never
// resize. With these buffers the game renders into the proxy's own resources (GetBuffer and GetCurrentBackBufferIndex
// answer from them) and every Present first copies the frame into the chain's current buffer, so the proxy alone holds
// the chain's buffers and can replace the chain at any Present.
//
// Costs, paid even with XeMFG off: one frame copy per Present and one more set of buffers in video memory. So only
// while XeMFG's pair is installed (xess_d3d12::Ready), and only where needed:
//   - [XeMFG] ProxyBackBuffers=1 or 0 in RTX40MFG-UI.ini beside the game decides when set;
//   - otherwise the games known to ignore window size changes;
//   - and a game whose window nudge went unanswered once (RememberIgnoredResize).
// Every other game keeps the chain's own buffers, and a switch waits for its resize (xemfglive.1).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace single_overlay::proxy
{
// Whether this game's movable chains take the proxy's back buffers (read once per process).
enum class BackBufferChoice { eNotNeeded, eSettingOff, eSettingOn, eKnownGame };
BackBufferChoice ChooseBackBuffers() noexcept;
bool BackBuffersChosen(BackBufferChoice choice) noexcept;
const wchar_t* ChoiceText(BackBufferChoice choice) noexcept;
// A switch's window nudge went unanswered in this game: [XeMFG] ProxyBackBuffers=1 is written beside it (unless the
// key is already set), so the game's chains take the buffers from its next launch. True when it is set to 1 now.
bool RememberIgnoredResize() noexcept;

class BackBuffers
{
public:
    BackBuffers() = default;
    BackBuffers(const BackBuffers&) = delete;
    BackBuffers& operator=(const BackBuffers&) = delete;
    ~BackBuffers() { Destroy(); }

    // Buffers matching `chain`'s description on `queue`'s device. False (with why): the game gets the chain's own.
    bool Create(IDXGISwapChain1* chain, IUnknown* queue, std::string& reason) noexcept;
    bool Active() const noexcept { return active_.load(std::memory_order_acquire); }
    // The game's view: buffer `index` (DXGI_ERROR_INVALID_CALL past the count) and the one it renders next.
    HRESULT Get(UINT index, REFIID riid, void** output) noexcept;
    UINT Index() const noexcept { return index_.load(std::memory_order_acquire); }
    // Before the chain's Present: the game's current buffer into the chain's current one. False when nothing was
    // copied (logged once per cause); the Present goes ahead either way.
    bool Copy(IDXGISwapChain3* chain) noexcept;
    // After it: the game's index moves on when the chain's did (a Present that did not take the frame moves neither).
    void Presented(UINT chainBefore, UINT chainAfter) noexcept;
    // The game holds none of them (DXGI's condition for ResizeBuffers).
    bool Released() const noexcept;
    // The queue's work so far is done on the GPU, every copy included (bounded wait). False on a timeout.
    bool Drain() noexcept;
    // The chain's buffers let go (before the chain resizes, moves or goes); the next Copy takes the current chain's.
    void ForgetChain() noexcept;
    // After the chain's ResizeBuffers: new buffers at its description, index 0. False (with why): off for good.
    bool Recreate(IDXGISwapChain1* chain, std::string& reason) noexcept;
    void Destroy() noexcept;
    DXGI_SWAP_CHAIN_DESC1 Desc() const noexcept { return desc_; }

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fence = 0;
    };
    bool CreateLocked(IDXGISwapChain1* chain, IUnknown* queue, std::string& reason);
    void ResetLocked() noexcept;
    bool CreateBuffers(const DXGI_SWAP_CHAIN_DESC1& desc, std::string& reason) noexcept;
    bool WaitFence(uint64_t value, DWORD milliseconds) noexcept;
    bool ChainBuffers(IDXGISwapChain3* chain) noexcept;
    void Once(uint32_t bit, const wchar_t* text) noexcept;

    mutable std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<UINT> index_{0};
    DXGI_SWAP_CHAIN_DESC1 desc_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fenceValue_ = 0;
    std::array<Slot, 4> slots_{};
    uint64_t copies_ = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> buffers_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> chainBuffers_;
    uint32_t logged_ = 0;
};
}
