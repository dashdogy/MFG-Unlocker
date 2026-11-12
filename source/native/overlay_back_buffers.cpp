// xemfglive.3: the menu proxy's own back buffers (overlay_back_buffers.h).
#include "overlay_back_buffers.h"
#include "overlay_native.h"
#include "overlay_platform.h"
#include "single_module.h"
#include "ui_host.h"
#include <cstdio>

namespace single_overlay::proxy
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr const wchar_t* kSection = L"XeMFG";
constexpr const wchar_t* kKey = L"ProxyBackBuffers";
enum class Setting { eOn, eOff, eInvalid, eMissing };

Setting ReadSetting(const std::wstring& path) noexcept
{
    return standalone_ui::ReadOnOff(kSection, kKey, path.c_str(), Setting::eMissing);
}

// Games whose renderer ignores window size changes. Cyberpunk 2077 in Borderless Windowed gives its renderer the
// configured resolution on WM_SIZE, so the nudge never reaches ResizeBuffers.
bool KnownGame() noexcept
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* leaf = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!leaf) return false;
    for (const wchar_t* game : {L"Cyberpunk2077.exe"})
        if (!_wcsicmp(leaf + 1, game)) return true;
    return false;
}
}

BackBufferChoice ChooseBackBuffers() noexcept
{
    static const BackBufferChoice choice = [] {
        try {
            switch (ReadSetting(standalone_ui::SettingsPath("RTX40MFG-UI.ini"))) {
            case Setting::eOn: return BackBufferChoice::eSettingOn;
            case Setting::eOff: return BackBufferChoice::eSettingOff;
            case Setting::eInvalid:
                single_module::Log(L"MFG_PROXY_UI XeMFG live switching: [XeMFG] ProxyBackBuffers is not 1 or 0; it is ignored");
                break;
            case Setting::eMissing: break;
            }
        } catch (...) {}
        return KnownGame() ? BackBufferChoice::eKnownGame : BackBufferChoice::eNotNeeded;
    }();
    return choice;
}

bool BackBuffersChosen(BackBufferChoice choice) noexcept
{
    return choice == BackBufferChoice::eSettingOn || choice == BackBufferChoice::eKnownGame;
}

const wchar_t* ChoiceText(BackBufferChoice choice) noexcept
{
    switch (choice) {
    case BackBufferChoice::eSettingOff: return L"[XeMFG] ProxyBackBuffers=0";
    case BackBufferChoice::eSettingOn: return L"[XeMFG] ProxyBackBuffers=1";
    case BackBufferChoice::eKnownGame: return L"a game known to ignore window size changes";
    case BackBufferChoice::eNotNeeded: break;
    }
    return L"the game resizes with its window";
}

bool RememberIgnoredResize() noexcept
{
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return false;
    try {
        const std::wstring path = standalone_ui::SettingsPath("RTX40MFG-UI.ini");
        if (path.empty() || ReadSetting(path) != Setting::eMissing) return false;   // the player's choice stands
        const bool written = WritePrivateProfileStringW(kSection, kKey, L"1", path.c_str()) && ReadSetting(path) == Setting::eOn;
        single_module::Log(written
            ? L"MFG_PROXY_UI XeMFG live switching: the game ignored the window nudge; [XeMFG] ProxyBackBuffers=1 written, so from its next launch a switch needs no resize"
            : L"MFG_PROXY_UI XeMFG live switching: the game ignored the window nudge; [XeMFG] ProxyBackBuffers=1 could not be written");
        return written;
    } catch (...) {
        return false;
    }
}

namespace
{
// A copy waits this long for its slot's previous copy, a drain for the queue (a GPU that stalls this long has hung).
constexpr DWORD kSlotWaitMs = 1000;
constexpr DWORD kDrainWaitMs = 2000;

// The 8-bit formats a game may view as sRGB: DXGI allows that on a chain's own buffers, D3D12 on a resource of the
// typed format only with fully typed format casting.
bool HasSrgbView(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
}

void BackBuffers::Once(uint32_t bit, const wchar_t* text) noexcept
{
    if (logged_ & bit) return;
    logged_ |= bit;
    single_module::Log(text);
}

bool BackBuffers::Create(IDXGISwapChain1* chain, IUnknown* queue, std::string& reason) noexcept
{
    std::lock_guard lock(mutex_);
    if (active_.load()) return true;
    try {
        if (CreateLocked(chain, queue, reason)) {
            active_ = true;
            return true;
        }
    } catch (...) {
        reason = "exception";
    }
    ResetLocked();   // nothing is kept from a refused start (no copy was ever submitted)
    return false;
}

bool BackBuffers::CreateLocked(IDXGISwapChain1* chain, IUnknown* queue, std::string& reason)
{
    if (!chain || !queue) { reason = "no_chain"; return false; }
    InternalScope internal;
    if (!native::Unwrap(queue, queue_) || !queue_) { reason = "queue"; return false; }
    if (queue_->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { reason = "queue_type"; return false; }
    if (FAILED(queue_->GetDevice(IID_PPV_ARGS(&device_))) || !device_) { reason = "device"; return false; }
    if (device_->GetNodeCount() != 1) { reason = "multi_node"; return false; }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(chain->GetDesc1(&desc))) { reason = "chain_desc"; return false; }
    for (auto& slot : slots_)
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) {
            reason = "allocator";
            return false;
        }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].allocator.Get(), nullptr,
            IID_PPV_ARGS(&list_))) || FAILED(list_->Close())) { reason = "command_list"; return false; }
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { reason = "fence"; return false; }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { reason = "event"; return false; }
    return CreateBuffers(desc, reason);
}

void BackBuffers::ResetLocked() noexcept
{
    active_ = false;
    chainBuffers_.clear();
    buffers_.clear();
    list_.Reset();
    for (auto& slot : slots_) slot = Slot{};
    fence_.Reset();
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    fenceValue_ = 0;
    device_.Reset();
    queue_.Reset();
}

// Committed resources as the chain's buffers are: its size, format and count, render-target (and unordered-access
// when the chain allows it) in the present state.
bool BackBuffers::CreateBuffers(const DXGI_SWAP_CHAIN_DESC1& desc, std::string& reason) noexcept
{
    if (desc.SampleDesc.Count != 1 || desc.Stereo) { reason = "chain_samples"; return false; }
    if (desc.BufferCount < 2 || desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS) { reason = "buffer_count"; return false; }
    if (!desc.Width || !desc.Height || desc.Format == DXGI_FORMAT_UNKNOWN) { reason = "chain_size"; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    if (HasSrgbView(desc.Format) && (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof options))
            || !options.CastingFullyTypedFormatSupported)) {
        reason = "format_casting";
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.Width;
    resource.Height = desc.Height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = desc.Format;
    resource.SampleDesc.Count = 1;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    std::vector<ComPtr<ID3D12Resource>> buffers(desc.BufferCount);
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_PRESENT,
                nullptr, IID_PPV_ARGS(&buffers[i]))) || !buffers[i]) {
            reason = "buffer_memory";
            return false;
        }
        wchar_t name[48]{};
        swprintf_s(name, L"MFG Unlocker back buffer %u", i);
        buffers[i]->SetName(name);
    }
    buffers_ = std::move(buffers);
    desc_ = desc;
    index_ = 0;
    return true;
}

HRESULT BackBuffers::Get(UINT index, REFIID riid, void** output) noexcept
{
    if (!output) return E_POINTER;
    *output = nullptr;
    std::lock_guard lock(mutex_);
    if (index >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
    return buffers_[index]->QueryInterface(riid, output);
}

bool BackBuffers::WaitFence(uint64_t value, DWORD milliseconds) noexcept
{
    if (!value || fence_->GetCompletedValue() >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, event_))) return false;
    return WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0 || fence_->GetCompletedValue() >= value;
}

// The current chain's buffers, as native resources on our device at our size and format.
bool BackBuffers::ChainBuffers(IDXGISwapChain3* chain) noexcept
{
    if (!chainBuffers_.empty()) return true;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(chain->GetDesc1(&desc)) || !desc.BufferCount || desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS) return false;
    std::vector<ComPtr<ID3D12Resource>> buffers(desc.BufferCount);
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ComPtr<ID3D12Resource> exposed;
        if (FAILED(chain->GetBuffer(i, IID_PPV_ARGS(&exposed))) || !native::Unwrap(exposed.Get(), buffers[i]) || !buffers[i])
            return false;
        const auto resource = buffers[i]->GetDesc();
        if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resource.Width != desc_.Width
            || resource.Height != desc_.Height || resource.Format != desc_.Format || resource.SampleDesc.Count != 1) {
            Once(1u << 4, L"MFG_PROXY_UI XeMFG back buffers: the swap chain's buffers differ from the game's (size or format); frames are not copied");
            return false;
        }
    }
    chainBuffers_ = std::move(buffers);
    return true;
}

bool BackBuffers::Copy(IDXGISwapChain3* chain) noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (!active_.load() || !chain) return false;
        InternalScope internal;
        if (!ChainBuffers(chain)) {
            Once(1u << 0, L"MFG_PROXY_UI XeMFG back buffers: the swap chain's buffers could not be taken; frames are not copied");
            return false;
        }
        const UINT target = chain->GetCurrentBackBufferIndex();
        const UINT source = index_.load();
        if (target >= chainBuffers_.size() || source >= buffers_.size()) return false;
        Slot& slot = slots_[copies_ % slots_.size()];
        if (!WaitFence(slot.fence, kSlotWaitMs)) {
            Once(1u << 1, L"MFG_PROXY_UI XeMFG back buffers: an earlier copy did not finish within a second; this frame is not copied");
            return false;
        }
        if (FAILED(slot.allocator->Reset()) || FAILED(list_->Reset(slot.allocator.Get(), nullptr))) {
            Once(1u << 2, L"MFG_PROXY_UI XeMFG back buffers: the copy's command list could not be reset; frames are not copied");
            return false;
        }
        ID3D12Resource* const from = buffers_[source].Get();
        ID3D12Resource* const to = chainBuffers_[target].Get();
        const D3D12_RESOURCE_BARRIER before[2] = {
            Transition(from, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(to, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
        const D3D12_RESOURCE_BARRIER after[2] = {
            Transition(to, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
            Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT)};
        list_->ResourceBarrier(2, before);
        list_->CopyResource(to, from);
        list_->ResourceBarrier(2, after);
        if (FAILED(list_->Close())) {
            Once(1u << 3, L"MFG_PROXY_UI XeMFG back buffers: the copy's command list did not close; frames are not copied");
            return false;
        }
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        slot.fence = ++fenceValue_;
        queue_->Signal(fence_.Get(), slot.fence);
        ++copies_;
        return true;
    } catch (...) {
        return false;
    }
}

void BackBuffers::Presented(UINT chainBefore, UINT chainAfter) noexcept
{
    std::lock_guard lock(mutex_);
    if (chainBefore != chainAfter && !buffers_.empty()) index_ = (index_.load() + 1) % UINT(buffers_.size());
}

bool BackBuffers::Released() const noexcept
{
    std::lock_guard lock(mutex_);
    for (const auto& buffer : buffers_) {
        ID3D12Resource* const resource = buffer.Get();
        resource->AddRef();
        if (resource->Release() != 1) return false;
    }
    return true;
}

bool BackBuffers::Drain() noexcept
{
    std::lock_guard lock(mutex_);
    if (!queue_ || !fence_) return true;
    const uint64_t value = ++fenceValue_;
    if (FAILED(queue_->Signal(fence_.Get(), value))) return false;
    return WaitFence(value, kDrainWaitMs);
}

void BackBuffers::ForgetChain() noexcept
{
    std::lock_guard lock(mutex_);
    chainBuffers_.clear();
}

bool BackBuffers::Recreate(IDXGISwapChain1* chain, std::string& reason) noexcept
{
    std::lock_guard lock(mutex_);
    chainBuffers_.clear();
    buffers_.clear();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    InternalScope internal;
    if (!chain || FAILED(chain->GetDesc1(&desc))) reason = "chain_desc";
    else if (CreateBuffers(desc, reason)) return true;
    active_ = false;
    return false;
}

void BackBuffers::Destroy() noexcept
{
    if (queue_ && fence_ && !Drain())
        single_module::Log(L"MFG_PROXY_UI XeMFG back buffers: the GPU did not finish within two seconds while they were released");
    std::lock_guard lock(mutex_);
    ResetLocked();
}
}
