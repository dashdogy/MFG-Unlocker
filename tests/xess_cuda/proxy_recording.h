#pragma once
std::vector<uint32_t> RunProxy(Gpu& g, const Spec& s, const Case& c, Program& program, ID3D12PipelineState* pso, xefg_cuda::ProviderList* proxy, int mode)
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

    auto* record = proxy ? proxy->Interface() : g.list.Get();
    auto* bindings = mode == 1 ? g.list.Get() : record;
    auto bind = [&] {
        ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
        bindings->SetDescriptorHeaps(1, heaps);
        bindings->SetComputeRootSignature(program.root.Get());
        bindings->SetComputeRootDescriptorTable(0, g.GpuHandle(t0));
        bindings->SetComputeRootDescriptorTable(1, g.GpuHandle(t1));
        bindings->SetComputeRoot32BitConstants(2, UINT(c.constants.size()), c.constants.data(), 0);
    };
    bind();
    record->SetPipelineState(pso);
    record->Dispatch(c.groupsX, c.groupsY, c.groupsZ);
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
