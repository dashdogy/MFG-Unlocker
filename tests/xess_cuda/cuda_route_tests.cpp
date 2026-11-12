// XeFG tensor-core route (source/native/xess/cuda): the CUDA kernels against Intel's DXIL programs,
// through the production route code, on an NVIDIA RTX 20/30/40/50 adapter.
//
// For every layer program in the route's table and every case, the same recording runs twice on
// identical inputs: once with a pipeline state the route does not recognize (Intel's program runs),
// once with one it recognizes (the Dispatch becomes the CUDA launch). Bindings follow the provider:
// shader-visible descriptor tables at non-zero heap offsets (0 = t0,[t4,]u0; 1 = t1,t2,t3), root
// constants b0. Outputs are padded past the dispatch's footprint and poisoned, and the whole padded
// texture must be byte-identical. Also checked: the route launched exactly once per replaced
// Dispatch; incomplete recording state on the Dispatch thread keeps Intel's program (fallback).
// A layer with RTXMFG's own DXIL (LayerInfo::ownSha256, which ShaderPort swaps in for Intel's program) also
// runs that program on every case: its root signature part must equal Intel's, its output must equal Intel's, and
// a pipeline created from it after the route is installed must be recognized and launch the same kernel.
//
// The recording list is created after the route is installed, as the provider's are, with
// CreateCommandList (--list create) or CreateCommandList1 (--list create1). The Agility build
// (XEFG_CUDA_AGILITY_SDK_VERSION, D3D12Core.dll in .\D3D12\) runs on a runtime that gives each list its
// own table on its first Reset; --expect-agility requires that runtime and that the list gets its own
// table. A list with its own table must carry the hooks there, the table every list starts on must never
// be written, and ForgetLayers must put the list table's originals back.
//
//   XefgCudaRouteTests --fatbin FILE --programs DIR [--programs DIR ...]
//       [--list create|create1] [--expect-agility] [--wrapped-device] [--expect-wrapper] [--dred] [--expect-class-table]
//       [--recorder [--stream] [--strict]] [--wrapper-stream-subobject]
// --dred: DRED auto-breadcrumbs and page-fault reporting forced on before the device is created, as Wuthering Waves
// runs. An Agility runtime then keeps each list on its class table in the runtime's image; --expect-class-table
// requires that runtime beside the executable, the recording list on that image table, and the route's
// "runtime_class" adoption of it.
//   XefgCudaRouteTests --architecture-gate   (no GPU: which compute capabilities and device names the route admits)
// The m8n8k16 kernels (sm_75's mma shape) run on any later GPU when compiled with XEFG_CUDA_MMA_M8N8K16: give such a
// fatbin to --fatbin to check the RTX 20 kernels on an RTX 30/40/50 adapter.
// --wrapped-device: Install with a device whose vtable is in this executable and that cannot be seen through
// must be refused, for the whole process. --expect-wrapper: ReShade's dxgi.dll beside the executable wraps the
// device, lists and heaps (as in a game with ReShade); the route must resolve the runtime's device and hook the
// runtime's lists under the wrappers, and every case must still pass.
// --recorder: the routed pipelines are created on a device wrapper (XefgPsoWrapper.dll's, or ReShade's with
// --expect-wrapper) whose pipeline methods XefgPsoRecorder.dll hooks, as the SCSKiller shader recorder's d3d12.dll
// hooks ReShade's: provider -> recorder -> wrapper -> runtime, with the lists created as before. Every layer must be
// recognized above the recorder (cuda_layer origin "stack") and every case must pass. --stream: the routed pipelines
// are CreatePipelineState streams. --strict: the route's stack recognition off (the caller rule alone): no layer may
// be recognized, the one cuda_pso_refused line must name the recorder as the first frame after the wrapper's, and a
// routed Dispatch must keep Intel's program. --wrapper-stream-subobject: XefgPsoWrapper.dll
// passes compute pipelines on as streams with a default SAMPLE_MASK subobject: not recognized, logged once with its type.
// DIR: DXIL programs (any file names); each layer's program (and own program) is found by its SHA-256.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "xess/cuda/cuda_route.h"
#include "sha256_util.h"
#include "cuda_cases.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

#ifdef XEFG_CUDA_AGILITY_SDK_VERSION
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = XEFG_CUDA_AGILITY_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif

namespace
{
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) { char b[256]; snprintf(b, sizeof b, "%s failed 0x%08lx", what, (unsigned long)hr); throw std::runtime_error(b); }
}
std::vector<uint8_t> ReadFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string Sha256(const std::vector<uint8_t>& bytes)
{
    sha256_util::Digest d{};
    if (!sha256_util::Compute(bytes.data(), bytes.size(), d)) throw std::runtime_error("sha256");
    return sha256_util::LowerHex(d);
}

using namespace xefg_cuda_cases;

// ---- D3D12 ------------------------------------------------------------------------------------------------
struct Gpu
{
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; uint64_t value{}; HANDLE event{};
    ComPtr<ID3D12DescriptorHeap> heap; UINT inc{};
    Gpu()
    {
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (d.VendorId == 0x10DE && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
            adapter.Reset();
        }
        if (!device) throw std::runtime_error("no NVIDIA D3D12 adapter");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 64;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "heap");
        inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // The recording list, open on the allocator (created as the provider creates its lists).
    void MakeList(bool create1)
    {
        if (!create1)
        {
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
            return;
        }
        ComPtr<ID3D12Device4> device4; Check(device.As(&device4), "device4");
        Check(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list)), "list1");
        Check(list->Reset(allocator.Get(), nullptr), "list1 reset");
    }
    void Submit()
    {
        Check(list->Close(), "close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        Check(fence->SetEventOnCompletion(value, event), "event");
        WaitForSingleObject(event, INFINITE);
        Check(device->GetDeviceRemovedReason(), "device removed");
        Check(allocator->Reset(), "allocator reset"); Check(list->Reset(allocator.Get(), nullptr), "list reset");
    }
    ComPtr<ID3D12Resource> Buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
        ComPtr<ID3D12Resource> r; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
        return r;
    }
    static DXGI_FORMAT Format(uint32_t components) { return components == 2 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT; }
    ComPtr<ID3D12Resource> TextureArray(uint32_t w, uint32_t h, uint32_t slices, uint32_t components = 4)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
        rd.DepthOrArraySize = UINT16(slices); rd.MipLevels = 1; rd.Format = Format(components); rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)), "texture");
        return r;
    }
    // One upload buffer for all slices, recorded into the open list (not submitted).
    std::vector<ComPtr<ID3D12Resource>> staging;
    // words: [slice][h][w] x 4; a 2-component texture takes words 0, 1 of each texel.
    void UploadTexture(ID3D12Resource* dst, const uint32_t* words, uint32_t w, uint32_t h, uint32_t slices, uint32_t components = 4)
    {
        const uint32_t pitch = (w*components*4 + 255) & ~255u; const uint64_t slice = (uint64_t(pitch)*h + 511) & ~511ull;
        auto up = Buffer(slice*slices, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* p{}; Check(up->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        for (uint32_t l = 0; l < slices; ++l)
            for (uint32_t r = 0; r < h; ++r)
                for (uint32_t x = 0; x < w; ++x)
                    memcpy(p + slice*l + size_t(r)*pitch + size_t(x)*components*4, words + ((size_t(l)*h + r)*w + x)*4, components*4);
        up->Unmap(0, nullptr);
        for (uint32_t l = 0; l < slices; ++l)
        {
            D3D12_TEXTURE_COPY_LOCATION s{}, d{};
            s.pResource = up.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint.Offset = slice*l;
            s.PlacedFootprint.Footprint = {Format(components), w, h, 1, pitch};
            d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = l;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
        staging.push_back(up);
    }
    void UploadBuffer(ID3D12Resource* dst, const void* data, uint64_t bytes)
    {
        auto up = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p{}; Check(up->Map(0, nullptr, &p), "map"); memcpy(p, data, bytes); up->Unmap(0, nullptr);
        list->CopyBufferRegion(dst, 0, up.Get(), 0, bytes);
        staging.push_back(up);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i)*inc; return h; }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT i) { auto h = heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(i)*inc; return h; }
};
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
}

struct Program
{
    std::vector<uint8_t> dxil; ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12PipelineState> reference, routed;
    std::vector<uint8_t> own; ComPtr<ID3D12PipelineState> ownReference, ownRouted;   // RTXMFG's program
};
// A DXBC container's part (empty when absent).
std::vector<uint8_t> Part(const std::vector<uint8_t>& container, const char* fourcc)
{
    if (container.size() < 32 || memcmp(container.data(), "DXBC", 4)) return {};
    uint32_t count{}; memcpy(&count, container.data() + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4*i + 4 <= container.size(); ++i)
    {
        uint32_t offset{}, size{}; memcpy(&offset, container.data() + 32 + 4*i, 4);
        if (offset + 8 > container.size()) break;
        memcpy(&size, container.data() + offset + 4, 4);
        if (!memcmp(container.data() + offset, fourcc, 4) && offset + 8 + size <= container.size())
            return {container.begin() + offset + 8, container.begin() + offset + 8 + size};
    }
    return {};
}

// Records the layer's Dispatch the way the provider binds it; returns the whole output texture.
// split: heaps, root signature, tables and constants are recorded on another thread (incomplete state
// on the Dispatch thread: the route must keep Intel's program).
std::vector<uint32_t> Run(Gpu& g, const Spec& s, const Case& c, Program& program, ID3D12PipelineState* pso, bool split)
{
    std::vector<ComPtr<ID3D12Resource>> keep;
    std::map<uint32_t, ComPtr<ID3D12Resource>> textures, buffers;
    for (const auto& [reg, t] : c.textures)
    {
        const uint32_t components = reg == 0 ? c.components : 4;
        auto r = g.TextureArray(t.w, t.h, t.slices, components); g.UploadTexture(r.Get(), t.words.data(), t.w, t.h, t.slices, components);
        Barrier(g.list.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        textures[reg] = r;
    }
    for (const auto& [reg, b] : c.buffers)
    {
        auto r = g.Buffer(b.size()*4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST); g.UploadBuffer(r.Get(), b.data(), b.size()*4);
        Barrier(g.list.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        buffers[reg] = r;
    }
    auto output = g.TextureArray(c.outW, c.outH, c.outSlices, c.components);
    {
        std::vector<uint32_t> poison(size_t(c.outW)*c.outH*c.outSlices*4, 0xdeadbeefu);
        g.UploadTexture(output.Get(), poison.data(), c.outW, c.outH, c.outSlices, c.components);
    }
    Barrier(g.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // table 0 at heap index 7: t0, [t4,] u0; table 1 at index 21: t1, t2, t3.
    const UINT t0 = 7, t1 = 21;
    auto srvTexture = [&](uint32_t reg, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Format = Gpu::Format(reg == 0 ? c.components : 4); v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.MipLevels = 1;
        const auto it = textures.find(reg);
        v.Texture2DArray.ArraySize = it == textures.end() ? 1 : c.textures.at(reg).slices;
        g.device->CreateShaderResourceView(it == textures.end() ? nullptr : it->second.Get(), &v, h);
    };
    auto srvBuffer = [&](uint32_t reg, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Format = DXGI_FORMAT_UNKNOWN; v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = UINT(c.buffers.at(reg).size()/4); v.Buffer.StructureByteStride = 16;
        g.device->CreateShaderResourceView(buffers.at(reg).Get(), &v, h);
    };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = Gpu::Format(c.components); uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.ArraySize = c.outSlices;
    srvTexture(0, g.Cpu(t0));
    if (s.family == Family::Flow) g.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, g.Cpu(t0 + 1));
    else { srvTexture(4, g.Cpu(t0 + 1)); g.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, g.Cpu(t0 + 2)); }
    for (uint32_t i = 0; i < 3; ++i) srvBuffer(1 + i, g.Cpu(t1 + i));

    auto bind = [&] {
        ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
        g.list->SetDescriptorHeaps(1, heaps);
        g.list->SetComputeRootSignature(program.root.Get());
        g.list->SetComputeRootDescriptorTable(0, g.GpuHandle(t0));
        g.list->SetComputeRootDescriptorTable(1, g.GpuHandle(t1));
        g.list->SetComputeRoot32BitConstants(2, UINT(c.constants.size()), c.constants.data(), 0);
    };
    if (split) { std::thread other(bind); other.join(); } else bind();
    g.list->SetPipelineState(pso);
    g.list->Dispatch(c.groupsX, c.groupsY, c.groupsZ);
    Barrier(g.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t texel = c.components*4;
    const uint32_t pitch = (c.outW*texel + 255) & ~255u; const uint64_t slice = (uint64_t(pitch)*c.outH + 511) & ~511ull;
    auto readback = g.Buffer(slice*c.outSlices, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (uint32_t l = 0; l < c.outSlices; ++l)
    {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = l;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint.Offset = slice*l;
        dst.PlacedFootprint.Footprint = {Gpu::Format(c.components), c.outW, c.outH, 1, pitch};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    g.Submit();
    g.staging.clear();
    std::vector<uint32_t> out(size_t(c.outW)*c.outH*c.outSlices*4);
    uint8_t* m{}; Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map readback");
    for (uint32_t l = 0; l < c.outSlices; ++l)
        for (uint32_t r = 0; r < c.outH; ++r)
            for (uint32_t x = 0; x < c.outW; ++x)
                memcpy(out.data() + ((size_t(l)*c.outH + r)*c.outW + x)*4, m + slice*l + size_t(r)*pitch + size_t(x)*texel, texel);
    readback->Unmap(0, nullptr);
    return out;
}

size_t Mismatches(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, size_t& poisonA)
{
    size_t n = 0; poisonA = 0;
    for (size_t i = 0; i < a.size(); ++i) { n += a[i] != b[i]; poisonA += a[i] == 0xdeadbeefu; }
    return n;
}

// ---- command-list tables (the route's hooks are in this executable) --------------------------------------
// ID3D12GraphicsCommandList slots the route hooks: Reset, Dispatch, SetPipelineState, SetDescriptorHeaps,
// SetComputeRootSignature, SetComputeRootDescriptorTable, SetComputeRoot32BitConstant(s).
constexpr int kHookedSlots[] = {10, 14, 25, 28, 29, 31, 33, 35};
constexpr int kTableSnapshot = 64;
void** TableOf(void* object) { return *static_cast<void***>(object); }
std::vector<void*> Snapshot(void** table) { return std::vector<void*>(table, table + kTableSnapshot); }
bool InThisExecutable(const void* address)
{
    HMODULE owner{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(address), &owner) && owner == GetModuleHandleW(nullptr);
}
size_t HookedSlots(void** table)
{
    size_t n = 0;
    for (int slot : kHookedSlots) n += InThisExecutable(table[slot]);
    return n;
}

// --wrapped-device: a device whose vtable is in this executable, as a wrapper's is in its own image, with no
// unwrap contract and no fence to reach the runtime's device through (it answers QueryInterface for itself only
// and fails every other method). Install must refuse it and stay unavailable for the process: the real device's
// tables are never patched.
HRESULT STDMETHODCALLTYPE WrapperQuery(void* self, REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Device) ? self : nullptr;
    return *out ? S_OK : E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE WrapperReference(void*) { return 1; }
HRESULT STDMETHODCALLTYPE WrapperMethod() { return E_NOTIMPL; }
void* gWrapperTable[64]{};
struct WrappedDevice { void** table; } gWrappedDevice{gWrapperTable};

// The image a vtable (or code) lies in, by file name; empty if none.
std::wstring ImageName(const void* address)
{
    HMODULE owner{};
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &owner) || !GetModuleFileNameW(owner, path, MAX_PATH)) return {};
    return fs::path(path).filename().wstring();
}
// --expect-wrapper: ReShade (its dxgi.dll beside this executable) wraps the device, its lists and heaps, as in a
// game. The route works on the runtime objects under them; ReShade 6.7+ answers IID_UnwrappedObject with those.
ComPtr<IUnknown> NativeOf(IUnknown* object)
{
    constexpr GUID unwrapped = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
    ComPtr<IUnknown> native;
    if (FAILED(object->QueryInterface(unwrapped, reinterpret_cast<void**>(native.GetAddressOf())))) native = object;
    return native;
}
std::string gRouteLog;

int WrappedDeviceTest(ID3D12Device* real, std::span<const unsigned char> fatbin)
{
    for (auto& slot : gWrapperTable) slot = reinterpret_cast<void*>(&WrapperMethod);
    gWrapperTable[0] = reinterpret_cast<void*>(&WrapperQuery);
    gWrapperTable[1] = gWrapperTable[2] = reinterpret_cast<void*>(&WrapperReference);
    constexpr int kDeviceSlots[] = {11, 12, 47, 51}; // CreateComputePipelineState, CreateCommandList, CreatePipelineState, CreateCommandList1
    void** const realTable = TableOf(real);
    std::vector<void*> before;
    for (int slot : kDeviceSlots) before.push_back(realTable[slot]);
    std::string reason, again;
    const bool installed = xefg_cuda::Install(reinterpret_cast<ID3D12Device*>(&gWrappedDevice), GetModuleHandleW(nullptr), fatbin, reason);
    const auto status = xefg_cuda::Snapshot();
    const bool later = xefg_cuda::Install(real, GetModuleHandleW(nullptr), fatbin, again);
    size_t changed = 0;
    for (size_t i = 0; i < std::size(kDeviceSlots); ++i) changed += realTable[kDeviceSlots[i]] != before[i];
    const bool ok = !installed && reason == "wrapped_device" && status.state == xefg_cuda::State::Unavailable
        && !later && again == "wrapped_device" && changed == 0;
    printf("%s wrapped device: installed %d reason '%s', state %u; real device afterwards installed %d reason '%s', %zu device slots changed\n",
        ok ? "PASS" : "FAIL", installed, reason.c_str(), static_cast<unsigned>(status.state), later, again.c_str(), changed);
    return ok ? 0 : 1;
}
const char* MemoryKind(const void* address)
{
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(address, &m, sizeof m) != sizeof m) return "unknown";
    return m.Type == MEM_IMAGE ? "image" : m.Type == MEM_PRIVATE ? "private" : "mapped";
}
std::wstring LoadedRuntime()
{
    wchar_t path[MAX_PATH]{};
    const HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core || !GetModuleFileNameW(core, path, MAX_PATH)) return L"(no D3D12Core.dll)";
    return path;
}

// --architecture-gate (no GPU): the architectures with SASS in the fatbin, and Turing (7.5) only with tensor cores.
// Names as nvcuda's cuDeviceGetName reports them.
int ArchitectureGate()
{
    struct Case { int major, minor; const char* name; bool admitted; };
    const Case cases[] = {
        {7, 5, "NVIDIA GeForce RTX 2060", true}, {7, 5, "NVIDIA GeForce RTX 2080 Ti", true},
        {7, 5, "NVIDIA GeForce RTX 2070 with Max-Q Design", true}, {7, 5, "NVIDIA TITAN RTX", true},
        {7, 5, "Quadro RTX 4000", true}, {7, 5, "Tesla T4", true},
        {7, 5, "NVIDIA GeForce GTX 1660 SUPER", false}, {7, 5, "NVIDIA GeForce GTX 1650", false},
        {7, 5, "NVIDIA GeForce MX450", false}, {7, 5, "NVIDIA T1000", false}, {7, 5, "Quadro T2000", false},
        {7, 5, "NVIDIA T400 4GB", false}, {7, 5, "", false},
        {8, 6, "NVIDIA GeForce RTX 3080", true}, {8, 9, "NVIDIA GeForce RTX 4080", true},
        {8, 0, "NVIDIA A100-SXM4-40GB", false}, {8, 7, "Orin", false}, {7, 0, "Tesla V100-SXM2-16GB", false},
        {7, 2, "Xavier", false}, {6, 1, "NVIDIA GeForce GTX 1080 Ti", false},
        {12, 0, "NVIDIA GeForce RTX 5090", true}, {12, 0, "NVIDIA GeForce RTX 5060 Laptop GPU", true},
        {12, 0, "NVIDIA RTX PRO 6000 Blackwell Workstation Edition", true}, {12, 1, "NVIDIA GB10", false},
        {10, 0, "NVIDIA B200", false},
    };
    size_t failures = 0;
    for (const auto& c : cases)
    {
        const bool admitted = xefg_cuda::SupportedArchitecture(c.major, c.minor, c.name);
        if (admitted != c.admitted) ++failures;
        printf("%s %d.%d '%s': %s\n", admitted == c.admitted ? "PASS" : "FAIL", c.major, c.minor, c.name,
            admitted ? "tensor-core route" : "DXIL port");
    }
    printf("%zu cases, %zu failures\n", std::size(cases), failures);
    return failures ? 1 : 0;
}

// ---- Pipelines through a recorder module and a device wrapper (tests/xess_cuda/pso_*.cpp) --------------------------------
struct PsoModules
{
    HRESULT (*wrap)(ID3D12Device*, BOOL, ID3D12Device**){};
    BOOL (*record)(ID3D12Device*){};
    LONG (*recorded)(){};
};
PsoModules LoadPsoModules()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto directory = fs::path(exe).parent_path();
    const HMODULE wrapper = LoadLibraryW((directory / L"XefgPsoWrapper.dll").c_str());
    const HMODULE recorder = LoadLibraryW((directory / L"XefgPsoRecorder.dll").c_str());
    if (!wrapper || !recorder) throw std::runtime_error("XefgPsoWrapper.dll and XefgPsoRecorder.dll must be beside the executable");
    PsoModules modules;
    modules.wrap = reinterpret_cast<decltype(modules.wrap)>(GetProcAddress(wrapper, "XefgWrapDevice"));
    modules.record = reinterpret_cast<decltype(modules.record)>(GetProcAddress(recorder, "XefgRecordPipelines"));
    modules.recorded = reinterpret_cast<decltype(modules.recorded)>(GetProcAddress(recorder, "XefgRecordedPipelines"));
    if (!modules.wrap || !modules.record || !modules.recorded) throw std::runtime_error("XefgPso module exports");
    return modules;
}
// A routed pipeline as the provider creates it on the device it holds: CreateComputePipelineState, or (--stream) a
// CreatePipelineState stream of its root signature and compute shader.
ComPtr<ID3D12PipelineState> CreateRouted(ID3D12Device* device, ID3D12RootSignature* root, const std::vector<uint8_t>& dxil, bool stream)
{
    ComPtr<ID3D12PipelineState> pso;
    if (!stream)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = root; d.CS = {dxil.data(), dxil.size()};
        Check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), "routed pso");
        return pso;
    }
    struct alignas(void*) Root { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; ID3D12RootSignature* root; };
    struct alignas(void*) Shader { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_SHADER_BYTECODE cs; };
    struct { Root root; Shader cs; } subobjects{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, {dxil.data(), dxil.size()}}};
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(subobjects), &subobjects};
    ComPtr<ID3D12Device2> device2; Check(device->QueryInterface(IID_PPV_ARGS(&device2)), "device2");
    Check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)), "routed stream pso");
    return pso;
}
// The route's log lines of one stage.
std::vector<std::string> RouteLines(const char* stage)
{
    std::vector<std::string> lines;
    const std::string prefix = std::string(stage) + " {";
    for (size_t at = 0; at < gRouteLog.size();)
    {
        const size_t end = gRouteLog.find('\n', at);
        const std::string line = gRouteLog.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.compare(0, prefix.size(), prefix)) lines.push_back(line);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return lines;
}
// --strict / --wrapper-stream-subobject: no routed pipeline is recognized, one cuda_pso_refused line carries expected,
// and a routed Dispatch keeps Intel's program (no launch).
int RefusedTest(Gpu& g, Program& program, bool create1, const std::vector<std::string>& expected)
{
    const auto status = xefg_cuda::Snapshot();
    const auto refusals = RouteLines("cuda_pso_refused");
    bool named = refusals.size() == 1;
    for (const auto& text : expected) named = named && refusals[0].find(text) != std::string::npos;
    const bool unrecognized = status.layers == 0 && RouteLines("cuda_layer").empty();
    printf("%s refused: %u recognized pipeline states, %zu cuda_pso_refused line(s)%s\n", unrecognized && named ? "PASS" : "FAIL",
        status.layers, refusals.size(), named ? " naming what is in the way" : "");
    g.MakeList(create1);
    const Spec& s = kSpecs[0];
    const Case c = Make(s, 37, 29, 5, true, 0);
    const auto reference = Run(g, s, c, program, program.reference.Get(), false);
    const auto before = xefg_cuda::Snapshot();
    const auto routed = Run(g, s, c, program, program.routed.Get(), false);
    const auto after = xefg_cuda::Snapshot();
    size_t poison{};
    const size_t bad = Mismatches(reference, routed, poison);
    const bool kept = bad == 0 && after.launches == before.launches;
    printf("%s %s routed Dispatch keeps Intel's program: %zu mismatching words, launches +%llu\n", kept ? "PASS" : "FAIL", s.name, bad,
        (unsigned long long)(after.launches - before.launches));
    return unrecognized && named && kept ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && !strcmp(argv[1], "--architecture-gate")) return ArchitectureGate();
        fs::path fatbinPath; std::vector<fs::path> programDirs;
        bool create1 = false, expectAgility = false, wrappedDevice = false, expectWrapper = false, dred = false, expectClassTable = false;
        bool recorder = false, stream = false, strict = false, wrapperStreamSubobject = false;
        for (int i = 1; i < argc; ++i)
        {
            if (!strcmp(argv[i], "--fatbin") && i + 1 < argc) fatbinPath = argv[++i];
            else if (!strcmp(argv[i], "--programs") && i + 1 < argc) programDirs.push_back(argv[++i]);
            else if (!strcmp(argv[i], "--list") && i + 1 < argc)
            {
                const std::string mode = argv[++i];
                if (mode != "create" && mode != "create1") throw std::runtime_error("--list create|create1");
                create1 = mode == "create1";
            }
            else if (!strcmp(argv[i], "--expect-agility")) expectAgility = true;
            else if (!strcmp(argv[i], "--wrapped-device")) wrappedDevice = true;
            else if (!strcmp(argv[i], "--expect-wrapper")) expectWrapper = true;
            else if (!strcmp(argv[i], "--dred")) dred = true;
            else if (!strcmp(argv[i], "--expect-class-table")) expectClassTable = true;
            else if (!strcmp(argv[i], "--recorder")) recorder = true;
            else if (!strcmp(argv[i], "--stream")) stream = true;
            else if (!strcmp(argv[i], "--strict")) strict = true;
            else if (!strcmp(argv[i], "--wrapper-stream-subobject")) wrapperStreamSubobject = true;
            else throw std::runtime_error(std::string("unknown argument ") + argv[i]);
        }
        if (fatbinPath.empty() || programDirs.empty()) throw std::runtime_error("usage: --fatbin FILE --programs DIR [...]");
        if ((stream || strict) && !recorder) throw std::runtime_error("--stream and --strict need --recorder");
        if (wrapperStreamSubobject && (recorder || expectWrapper)) throw std::runtime_error("--wrapper-stream-subobject runs alone");
        const auto fatbin = ReadFile(fatbinPath);
        std::map<std::string, std::vector<uint8_t>> bySha;
        for (const auto& dir : programDirs)
            for (const auto& e : fs::directory_iterator(dir))
                if (e.is_regular_file() && e.file_size() < 1000000) { auto b = ReadFile(e.path()); bySha.emplace(Sha256(b), std::move(b)); }
        xefg_cuda::SetLog([](const char* stage, const std::string& fields) noexcept {
            printf("  log %s {%s}\n", stage, fields.c_str());
            try { gRouteLog += std::string(stage) + " {" + fields + "}\n"; } catch (...) {}
        });

        if (dred)
        {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
            Check(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)), "DRED settings");
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            printf("DRED auto-breadcrumbs and page faults forced on\n");
        }
        Gpu g;
        const std::wstring runtime = LoadedRuntime();
        printf("D3D12 runtime: %ls\n", runtime.c_str());
        if (wrappedDevice) return WrappedDeviceTest(g.device.Get(), {fatbin.data(), fatbin.size()});
        const std::wstring deviceImage = ImageName(TableOf(g.device.Get()));
        const bool wrapped = _wcsicmp(deviceImage.c_str(), L"D3D12Core.dll") && _wcsicmp(deviceImage.c_str(), L"d3d12.dll");
        printf("device vtable in %ls (%s)\n", deviceImage.c_str(), wrapped ? "a wrapper" : "the runtime");
        if (expectWrapper != wrapped)
            throw std::runtime_error(expectWrapper ? "--expect-wrapper: the device is not wrapped (ReShade not loaded?)" : "the device is wrapped");
        if (expectAgility || expectClassTable)
        {
            wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const auto expected = fs::path(exe).parent_path() / L"D3D12" / L"D3D12Core.dll";
            if (_wcsicmp(runtime.c_str(), expected.c_str()))
                throw std::runtime_error("--expect-agility: the Agility runtime beside the executable is not the one loaded");
        }
        // A list that never had a Reset (CreateCommandList1: created closed) is on the table the runtime's
        // lists share: the system runtime's class table, or the table Agility copies for each list on its
        // first Reset.
        ComPtr<ID3D12Device4> device4; Check(g.device.As(&device4), "device4");
        ComPtr<ID3D12GraphicsCommandList> probe;
        Check(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&probe)), "probe list");
        const auto nativeProbe = NativeOf(probe.Get());
        void** const sharedTable = TableOf(nativeProbe.Get());
        const auto sharedBefore = Snapshot(sharedTable);
        printf("shared table: %p (%s memory)\n", static_cast<void*>(sharedTable), MemoryKind(sharedTable));
        std::map<std::string, Program> programs;
        size_t ownPrograms = 0;
        for (const auto& info : xefg_cuda::Layers())
        {
            auto it = bySha.find(info.sha256);
            if (it == bySha.end()) throw std::runtime_error(std::string("program fixture missing for ") + info.program + " " + info.sha256);
            Program p; p.dxil = it->second;
            Check(g.device->CreateRootSignature(0, p.dxil.data(), p.dxil.size(), IID_PPV_ARGS(&p.root)), "root signature");
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = p.root.Get(); d.CS = {p.dxil.data(), p.dxil.size()};
            Check(g.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&p.reference)), "reference pso"); // before the route: Intel's program
            if (info.ownSha256)
            {
                auto own = bySha.find(info.ownSha256);
                if (own == bySha.end()) throw std::runtime_error(std::string("own program fixture missing for ") + info.program + " " + info.ownSha256);
                p.own = own->second;
                const auto rts = Part(p.own, "RTS0");
                if (rts.empty() || rts != Part(p.dxil, "RTS0"))
                    throw std::runtime_error(std::string("own program root signature differs from Intel's for ") + info.program);
                D3D12_COMPUTE_PIPELINE_STATE_DESC o{}; o.pRootSignature = p.root.Get(); o.CS = {p.own.data(), p.own.size()};
                Check(g.device->CreateComputePipelineState(&o, IID_PPV_ARGS(&p.ownReference)), "own reference pso");
                ++ownPrograms;
            }
            programs[info.program] = std::move(p);
        }
        // The device the provider holds and creates its pipelines on. --recorder / --wrapper-stream-subobject:
        // XefgPsoWrapper.dll's wrapper of the runtime's device (ReShade's own device with --expect-wrapper), and under
        // --recorder XefgPsoRecorder.dll hooks its pipeline methods. The lists are created on g.device as before.
        ComPtr<ID3D12Device> provided = g.device;
        PsoModules modules{};
        const bool psoModules = recorder || wrapperStreamSubobject;
        if (psoModules)
        {
            modules = LoadPsoModules();
            if (!expectWrapper)
            {
                ID3D12Device* wrapper{};
                Check(modules.wrap(g.device.Get(), wrapperStreamSubobject, &wrapper), "XefgWrapDevice");
                provided.Attach(wrapper);
            }
            if (recorder && !modules.record(provided.Get())) throw std::runtime_error("XefgRecordPipelines");
            printf("pipelines: %s%ls\n", recorder ? "XefgPsoRecorder.dll -> " : "", ImageName(TableOf(provided.Get())).c_str());
        }
        if (strict) xefg_cuda::SetStackRecognition(false);
        std::string reason;
        if (!xefg_cuda::Install(provided.Get(), GetModuleHandleW(nullptr), {fatbin.data(), fatbin.size()}, reason))
            throw std::runtime_error("route unavailable: " + reason);
        if ((expectWrapper || psoModules) && gRouteLog.find("\"native\":\"unwrap\"") == std::string::npos
            && gRouteLog.find("\"native\":\"fence\"") == std::string::npos)
            throw std::runtime_error("the route did not resolve the runtime's device under the wrapper");
        for (auto& [name, p] : programs)
        {
            p.routed = CreateRouted(provided.Get(), p.root.Get(), p.dxil, stream);
            if (!p.own.empty()) p.ownRouted = CreateRouted(provided.Get(), p.root.Get(), p.own, stream);
        }
        const auto status0 = xefg_cuda::Snapshot();
        printf("route active, compute capability %d.%d, %u recognized pipeline states (%zu own programs)\n", status0.major, status0.minor,
            status0.layers, ownPrograms);
        if (recorder)
        {
            const LONG seen = modules.recorded();
            printf("recorder: %ld pipelines created through XefgPsoRecorder.dll%s\n", seen, stream ? " (streams)" : "");
            if (size_t(seen) != programs.size() + ownPrograms) throw std::runtime_error("the recorder did not see every routed pipeline");
        }
        const std::string wrapperImage = expectWrapper ? "dxgi.dll+" : "XefgPsoWrapper.dll+";
        if (strict)
            return RefusedTest(g, programs.at(kSpecs[0].name), create1, {"\"reason\":\"caller\"", "\"caller\":\"" + wrapperImage,
                "\"first\":\"XefgPsoRecorder.dll+", "\"provider_frame\":1,"});
        if (wrapperStreamSubobject)
            return RefusedTest(g, programs.at(kSpecs[0].name), create1, {"\"reason\":\"stream_subobject\",\"subobject_type\":9,\"offset\":40,",
                "\"caller\":\"XefgPsoWrapper.dll+", "\"provider_frame\":0,"});
        if (status0.layers != programs.size() + ownPrograms) throw std::runtime_error("not every layer program was recognized");
        if (recorder)
        {
            // Every layer recognized above the recorder: its line names XefgPsoRecorder.dll as the first frame after the wrapper's.
            const auto layers = RouteLines("cuda_layer");
            size_t stack = 0;
            for (const auto& line : layers) stack += line.find("\"origin\":\"stack\",\"first\":\"XefgPsoRecorder.dll+") != std::string::npos;
            const bool ok = layers.size() == xefg_cuda::Layers().size() && stack == layers.size() && RouteLines("cuda_pso_refused").empty();
            printf("%s recorder: %zu cuda_layer lines, %zu recognized above XefgPsoRecorder.dll, %zu cuda_pso_refused\n", ok ? "PASS" : "FAIL",
                layers.size(), stack, RouteLines("cuda_pso_refused").size());
            if (!ok) throw std::runtime_error("--recorder: the layers were not recognized through the stack walk");
        }
        g.MakeList(create1);
        const auto nativeList = NativeOf(g.list.Get()); // the list the route hooks (the runtime's, under a wrapper)
        if (expectWrapper && nativeList.Get() == static_cast<IUnknown*>(g.list.Get()))
            throw std::runtime_error("--expect-wrapper: the recording list is not wrapped");
        void** const listTable = TableOf(nativeList.Get());
        const bool ownTable = listTable != sharedTable;
        printf("recording list (%s): %s table %p (%s memory), %zu of 8 hooks\n", create1 ? "CreateCommandList1" : "CreateCommandList",
            ownTable ? "its own" : "the shared", static_cast<void*>(listTable), MemoryKind(listTable), HookedSlots(listTable));
        if (HookedSlots(listTable) != std::size(kHookedSlots)) throw std::runtime_error("the recording list's table does not carry the route's hooks");
        if (expectAgility && (!ownTable || strcmp(MemoryKind(listTable), "private")))
            throw std::runtime_error("--expect-agility: the recording list did not get its own table");
        if (expectClassTable && (ownTable || strcmp(MemoryKind(listTable), "image")
                || gRouteLog.find("\"table\":\"runtime_class\"") == std::string::npos))
            throw std::runtime_error("--expect-class-table: the recording list is not on the runtime's class table adopted as runtime_class");

        size_t failures = 0, cases = 0;
        struct Size { uint32_t w, h; };
        for (const auto& s : kSpecs)
        {
            auto& program = programs.at(s.name);
            for (const auto& size : {Size{37, 29}, Size{101, 67}, Size{16, 8}})
                for (int mode = 0; mode < 5; ++mode)
                    for (int realistic = 0; realistic < 2; ++realistic)
                    {
                        if ((s.family == Family::Pool && mode == 2)) continue; // no constants to shrink
                        if (mode == 4 && !(s.family == Family::Flow && s.nout == 8)) continue; // 2-component tensors: 8-channel flow only
                        const Case c = Make(s, size.w, size.h, uint32_t(1 + mode*7 + realistic), realistic != 0, mode);
                        const auto before = xefg_cuda::Snapshot();
                        const auto reference = Run(g, s, c, program, program.reference.Get(), false);
                        const auto mid = xefg_cuda::Snapshot();
                        const auto routed = Run(g, s, c, program, program.routed.Get(), false);
                        const auto after = xefg_cuda::Snapshot();
                        size_t poison{}; const size_t bad = Mismatches(reference, routed, poison);
                        const bool launchedOnce = mid.launches == before.launches && after.launches == mid.launches + 1;
                        ++cases;
                        const bool ok = bad == 0 && launchedOnce;
                        failures += !ok;
                        if (!ok || getenv("XEFG_CUDA_VERBOSE"))
                            printf("%s %s: %zu mismatching words (Intel left %zu poisoned), launches %llu -> %llu\n", ok ? "PASS" : "FAIL",
                                c.label.c_str(), bad, poison, (unsigned long long)mid.launches, (unsigned long long)after.launches);
                        if (program.ownReference)
                        {
                            // RTXMFG's program as DXIL (not recognized: created before the route) and as the provider
                            // creates it after the swap (recognized: the same kernel launches).
                            const auto ownBefore = xefg_cuda::Snapshot();
                            const auto ownDxil = Run(g, s, c, program, program.ownReference.Get(), false);
                            const auto ownMid = xefg_cuda::Snapshot();
                            const auto ownRouted = Run(g, s, c, program, program.ownRouted.Get(), false);
                            const auto ownAfter = xefg_cuda::Snapshot();
                            size_t ownPoison{};
                            const size_t badDxil = Mismatches(reference, ownDxil, ownPoison), badRouted = Mismatches(reference, ownRouted, ownPoison);
                            const bool ownOk = badDxil == 0 && badRouted == 0 && ownMid.launches == ownBefore.launches
                                && ownAfter.launches == ownMid.launches + 1;
                            ++cases; failures += !ownOk;
                            if (!ownOk || getenv("XEFG_CUDA_VERBOSE"))
                                printf("%s %s own program: %zu mismatching words as DXIL, %zu routed, launches %llu -> %llu\n",
                                    ownOk ? "PASS" : "FAIL", c.label.c_str(), badDxil, badRouted, (unsigned long long)ownMid.launches,
                                    (unsigned long long)ownAfter.launches);
                        }
                    }
            // Incomplete recording state on the Dispatch thread: Intel's program must run.
            const Case c = Make(s, 37, 29, 99, true, 0);
            const auto reference = Run(g, s, c, program, program.reference.Get(), false);
            const auto before = xefg_cuda::Snapshot();
            const auto split = Run(g, s, c, program, program.routed.Get(), true);
            const auto after = xefg_cuda::Snapshot();
            size_t poison{}; const size_t bad = Mismatches(reference, split, poison);
            const bool ok = bad == 0 && after.launches == before.launches && after.fallbacks == before.fallbacks + 1;
            ++cases; failures += !ok;
            if (!ok || getenv("XEFG_CUDA_VERBOSE"))
                printf("%s %s fallback (state recorded on another thread): %zu mismatching words, launches +%llu, fallbacks +%llu\n",
                    ok ? "PASS" : "FAIL", s.name, bad, (unsigned long long)(after.launches - before.launches),
                    (unsigned long long)(after.fallbacks - before.fallbacks));
            if (program.ownRouted)
            {
                // The own program's fallback runs the own program as DXIL.
                const auto ownBefore = xefg_cuda::Snapshot();
                const auto ownSplit = Run(g, s, c, program, program.ownRouted.Get(), true);
                const auto ownAfter = xefg_cuda::Snapshot();
                size_t ownPoison{}; const size_t ownBad = Mismatches(reference, ownSplit, ownPoison);
                const bool ownOk = ownBad == 0 && ownAfter.launches == ownBefore.launches && ownAfter.fallbacks == ownBefore.fallbacks + 1;
                ++cases; failures += !ownOk;
                if (!ownOk || getenv("XEFG_CUDA_VERBOSE"))
                    printf("%s %s own program fallback: %zu mismatching words, launches +%llu, fallbacks +%llu\n", ownOk ? "PASS" : "FAIL",
                        s.name, ownBad, (unsigned long long)(ownAfter.launches - ownBefore.launches),
                        (unsigned long long)(ownAfter.fallbacks - ownBefore.fallbacks));
            }
        }
        if (TableOf(nativeList.Get()) != listTable) { ++failures; printf("FAIL the recording list changed tables during the cases\n"); }
        if (ownTable)
        {
            // Every other list starts on (and Agility copies) the shared table: it never carries the route's pointers.
            const bool untouched = Snapshot(sharedTable) == sharedBefore && HookedSlots(sharedTable) == 0;
            ++cases; failures += !untouched;
            printf("%s shared table untouched\n", untouched ? "PASS" : "FAIL");
        }
        const auto status = xefg_cuda::Snapshot();
        {
            // No provider context remains: a list's own table gets its originals back, and the list keeps working.
            xefg_cuda::ForgetLayers();
            const bool restored = ownTable ? HookedSlots(listTable) == 0 : true;
            const Spec& s = kSpecs[0];
            auto& program = programs.at(s.name);
            const Case c = Make(s, 37, 29, 5, true, 0);
            const auto reference = Run(g, s, c, program, program.reference.Get(), false);
            const auto routed = Run(g, s, c, program, program.routed.Get(), false);
            const auto after = xefg_cuda::Snapshot();
            size_t poison{}; const size_t bad = Mismatches(reference, routed, poison);
            const bool ok = restored && bad == 0 && after.launches == status.launches;
            ++cases; failures += !ok;
            printf("%s after ForgetLayers: list table %s (%zu hooks), %zu mismatching words, launches +%llu\n", ok ? "PASS" : "FAIL",
                MemoryKind(listTable), HookedSlots(listTable), bad, (unsigned long long)(after.launches - status.launches));
        }
        printf("%zu cases, %zu failures; route launches %llu, fallbacks %llu\n", cases, failures,
            (unsigned long long)status.launches, (unsigned long long)status.fallbacks);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e) { fprintf(stderr, "error: %s\n", e.what()); return 2; }
}
