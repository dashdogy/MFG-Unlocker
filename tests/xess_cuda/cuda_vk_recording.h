#pragma once
// Same D3D12 resource/binding recordings as the D3D12 test; submission is a real
// Vulkan primary command buffer on an xvk recorded device.
using Microsoft::WRL::ComPtr;
using namespace xefg_cuda_cases;
void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + ": " + std::to_string(hr));
}
struct Gpu
{
    Vulkan& vk;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT inc{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkQueue queue{};
    VkFence fence{};
    template<class T> T Proc(const char* name) { return reinterpret_cast<T>(vk.gdpa(vk.device, name)); }
    void Begin()
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(Proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "begin command buffer");
        if (!xvk::BeginRecording(device.Get(), command)) throw std::runtime_error("xvk BeginRecording");
    }
    Gpu(Vulkan& v, HMODULE programs) : vk(v)
    {
        auto desc = vk.desc; desc.programModule = programs;
        Check(xvk::CreateDevice(desc, IID_PPV_ARGS(&device)), "xvk CreateDevice");
        Proc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(vk.device, desc.recordedFamily, 0, &queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = desc.recordedFamily;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(Proc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(vk.device, &pi, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        Check(Proc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(vk.device, &ai, &command), "allocate buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(Proc<PFN_vkCreateFence>("vkCreateFence")(vk.device, &fi, nullptr, &fence), "fence");
        Begin();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 64;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "heap");
        inc = device->GetDescriptorHandleIncrementSize(hd.Type);
    }
    void Submit(bool restart = true)
    {
        Check(list->Close(), "close"); xvk::EndRecording(device.Get());
        Check(Proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "end buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        Check(Proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence), "queue submit");
        Check(Proc<PFN_vkWaitForFences>("vkWaitForFences")(vk.device, 1, &fence, VK_TRUE, 5000000000ull), "fence wait");
        if (!xvk::WaitRecordedIdle(device.Get(), 1000000000ull)) throw std::runtime_error("xvk recorded idle timeout");
        Check(Proc<PFN_vkResetFences>("vkResetFences")(vk.device, 1, &fence), "reset fence");
        Check(device->GetDeviceRemovedReason(), "device removed");
        if (restart) {
            Check(Proc<PFN_vkResetCommandPool>("vkResetCommandPool")(vk.device, pool, 0), "reset pool");
            Begin(); Check(allocator->Reset(), "allocator reset"); Check(list->Reset(allocator.Get(), nullptr), "list reset");
        }
    }
    ~Gpu()
    {
        try { if (list) Submit(false); } catch (...) {}
        staging.clear(); heap.Reset(); list.Reset(); allocator.Reset(); device.Reset();
        if (fence) Proc<PFN_vkDestroyFence>("vkDestroyFence")(vk.device, fence, nullptr);
        if (pool) Proc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(vk.device, pool, nullptr);
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
std::vector<uint32_t> Run(Gpu& g, const Spec& s, const Case& c, Program& program, ID3D12PipelineState* pso, bool split, bool refresh = false, int refusal = 0)
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
        const bool upload = refusal == 4 && reg == 1;
        auto r = g.Buffer(b.size()*4, upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST);
        if (upload) { void* p{}; Check(r->Map(0, nullptr, &p), "map upload weights"); std::memcpy(p, b.data(), b.size()*4); r->Unmap(0, nullptr); }
        else g.UploadBuffer(r.Get(), b.data(), b.size()*4);
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
        v.Format = Gpu::Format(reg == 0 ? c.components : 4);
        const auto it = textures.find(reg);
        const uint32_t slices = it == textures.end() ? 1 : c.textures.at(reg).slices;
        if (slices == 1) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.MipLevels = 1; v.Texture2DArray.ArraySize = slices;
        }
        g.device->CreateShaderResourceView(it == textures.end() ? nullptr : it->second.Get(), &v, h);
    };
    auto srvBuffer = [&](uint32_t reg, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Format = DXGI_FORMAT_UNKNOWN; v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = UINT(c.buffers.at(reg).size()/4); v.Buffer.StructureByteStride = 16;
        g.device->CreateShaderResourceView(buffers.at(reg).Get(), &v, h);
    };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = Gpu::Format(c.components);
    if (c.outSlices == 1) uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    else { uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; uav.Texture2DArray.ArraySize = c.outSlices; }
    srvTexture(0, g.Cpu(t0));
    if (s.family == Family::Flow) g.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, g.Cpu(t0 + 1));
    else { srvTexture(4, g.Cpu(t0 + 1)); g.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, g.Cpu(t0 + 2)); }
    for (uint32_t i = 0; i < 3; ++i) srvBuffer(1 + i, g.Cpu(t1 + i));
    // Keep the SPIR-V view valid while testing that a 2D view without its CUDA twin is refused.
    if (refusal == 1) reinterpret_cast<xvk::Descriptor*>(g.Cpu(t0).ptr)->cudaArrayView = VK_NULL_HANDLE;
    if (refusal == 3) reinterpret_cast<xvk::Descriptor*>(g.Cpu(t1).ptr)->stride = 32;

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
    if (refresh) {
        auto changed = c.buffers.at(1);
        for (auto& word : changed) word ^= 0x03030303u;
        Barrier(g.list.Get(), buffers.at(1).Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g.UploadBuffer(buffers.at(1).Get(), changed.data(), changed.size()*4);
        Barrier(g.list.Get(), buffers.at(1).Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = output.Get();
        g.list->ResourceBarrier(1, &barrier);
        g.list->Dispatch(c.groupsX, c.groupsY, c.groupsZ);
    }
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
