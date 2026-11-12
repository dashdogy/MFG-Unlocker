#include "provider_list.h"
#include <cstring>
#include <cstdio>

namespace xefg_cuda
{
bool ProviderList::Bind(ID3D12GraphicsCommandList* real) noexcept
{
    Unbind(); state_ = {}; escaped_ = false;
    std::string reason;
    if (disabled_ || !CanRecord(real, reason))
    {
        try {
            if (disabled_) reason = "disabled";
            if (lastRefusal_ != reason) {
                lastRefusal_ = reason;
                Report("cuda_provider_list", "\"state\":\"refused\",\"reason\":\"" + reason + "\"");
            }
        } catch (...) {}
        return false;
    }
    real_ = real; real_->AddRef();
    real_->QueryInterface(IID_PPV_ARGS(&list2_));
    if (!loggedBind_) {
        loggedBind_ = true;
        try { Report("cuda_provider_list", "\"state\":\"bound\",\"table\":\"" + reason + "\""); } catch (...) {}
    }
    return true;
}
void ProviderList::Unbind() noexcept
{
    list2_.Reset();
    if (real_) real_->Release();
    real_ = nullptr; state_ = {};
}
void ProviderList::Escape(const char* reason) noexcept
{
    if (escaped_) return;
    escaped_ = true;
    try { Report("cuda_provider_list", std::string("\"state\":\"escaped\",\"reason\":\"") + reason + "\""); } catch (...) {}
}
void ProviderList::Disable(int32_t result) noexcept
{
    disabled_ = true;
    try { Report("cuda_provider_list", "\"state\":\"disabled\",\"result\":" + std::to_string(result)); } catch (...) {}
}
HRESULT STDMETHODCALLTYPE ProviderList::QueryInterface(REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild)
        || iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList)
        || (list2_ && (iid == __uuidof(ID3D12GraphicsCommandList1) || iid == __uuidof(ID3D12GraphicsCommandList2))))
    { *out = static_cast<ID3D12GraphicsCommandList2*>(this); AddRef(); return S_OK; }
    char text[80]; std::snprintf(text, sizeof(text), "iid_%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    Escape(text);
    return real_ ? real_->QueryInterface(iid, out) : E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE ProviderList::GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData)
{
    if (!real_) return E_UNEXPECTED;
    return real_->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE ProviderList::SetPrivateData(REFGUID guid, UINT DataSize, const void *pData)
{
    if (!real_) return E_UNEXPECTED;
    return real_->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE ProviderList::SetPrivateDataInterface(REFGUID guid, const IUnknown *pData)
{
    if (!real_) return E_UNEXPECTED;
    return real_->SetPrivateDataInterface(guid, pData);
}

HRESULT STDMETHODCALLTYPE ProviderList::SetName(LPCWSTR Name)
{
    if (!real_) return E_UNEXPECTED;
    return real_->SetName(Name);
}

HRESULT STDMETHODCALLTYPE ProviderList::GetDevice(REFIID riid, void **ppvDevice)
{
    if (!real_) return E_UNEXPECTED;
    return real_->GetDevice(riid, ppvDevice);
}

D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE ProviderList::GetType()
{
    if (!real_) return D3D12_COMMAND_LIST_TYPE{};
    return real_->GetType();
}

HRESULT STDMETHODCALLTYPE ProviderList::Close()
{
    if (!real_) return E_UNEXPECTED;
    Escape("close"); return real_->Close();
}

HRESULT STDMETHODCALLTYPE ProviderList::Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState)
{
    if (!real_) return E_UNEXPECTED;
    Escape("reset"); return real_->Reset(pAllocator, pInitialState);
}

void STDMETHODCALLTYPE ProviderList::ClearState(ID3D12PipelineState *pPipelineState)
{
    if (!real_) return;
    state_ = {}; state_.layer = LayerOf(pPipelineState);
        real_->ClearState(pPipelineState);
}

void STDMETHODCALLTYPE ProviderList::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    if (!real_) return;
    real_->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ProviderList::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (!real_) return;
    real_->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ProviderList::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    if (!real_) return;
    if (state_.layer && !escaped_ && LaunchOn(real_, state_, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ)) return;
        real_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE ProviderList::CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)
{
    if (!real_) return;
    real_->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
}

void STDMETHODCALLTYPE ProviderList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox)
{
    if (!real_) return;
    real_->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

void STDMETHODCALLTYPE ProviderList::CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource)
{
    if (!real_) return;
    real_->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE ProviderList::CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags)
{
    if (!real_) return;
    real_->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
}

void STDMETHODCALLTYPE ProviderList::ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    if (!real_) return;
    real_->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

void STDMETHODCALLTYPE ProviderList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
    if (!real_) return;
    real_->IASetPrimitiveTopology(PrimitiveTopology);
}

void STDMETHODCALLTYPE ProviderList::RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports)
{
    if (!real_) return;
    real_->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE ProviderList::RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects)
{
    if (!real_) return;
    real_->RSSetScissorRects(NumRects, pRects);
}

void STDMETHODCALLTYPE ProviderList::OMSetBlendFactor(const FLOAT BlendFactor[ 4 ])
{
    if (!real_) return;
    real_->OMSetBlendFactor(BlendFactor);
}

void STDMETHODCALLTYPE ProviderList::OMSetStencilRef(UINT StencilRef)
{
    if (!real_) return;
    real_->OMSetStencilRef(StencilRef);
}

void STDMETHODCALLTYPE ProviderList::SetPipelineState(ID3D12PipelineState *pPipelineState)
{
    if (!real_) return;
    state_.layer = LayerOf(pPipelineState);
        real_->SetPipelineState(pPipelineState);
}

void STDMETHODCALLTYPE ProviderList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers)
{
    if (!real_) return;
    real_->ResourceBarrier(NumBarriers, pBarriers);
}

void STDMETHODCALLTYPE ProviderList::ExecuteBundle(ID3D12GraphicsCommandList *pCommandList)
{
    if (!real_) return;
    Escape("bundle"); real_->ExecuteBundle(pCommandList);
}

void STDMETHODCALLTYPE ProviderList::SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps)
{
    if (!real_) return;
    state_.heap = nullptr; state_.tableMask = 0;
        if (ppDescriptorHeaps) for (UINT i = 0; i < NumDescriptorHeaps && i < 2; ++i)
            if (ppDescriptorHeaps[i] && ppDescriptorHeaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) state_.heap = ppDescriptorHeaps[i];
        real_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRootSignature(ID3D12RootSignature *pRootSignature)
{
    if (!real_) return;
    state_.tableMask = state_.constantMask = 0;
        real_->SetComputeRootSignature(pRootSignature);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature)
{
    if (!real_) return;
    real_->SetGraphicsRootSignature(pRootSignature);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    if (!real_) return;
    if (RootParameterIndex < 2) { state_.tables[RootParameterIndex] = BaseDescriptor; state_.tableMask |= 1u << RootParameterIndex; }
        real_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    if (!real_) return;
    real_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    if (!real_) return;
    if (RootParameterIndex == 2 && DestOffsetIn32BitValues < 12) { state_.constants[DestOffsetIn32BitValues] = SrcData; state_.constantMask |= 1u << DestOffsetIn32BitValues; }
        real_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
    if (!real_) return;
    real_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues)
{
    if (!real_) return;
    if (RootParameterIndex == 2 && pSrcData && DestOffsetIn32BitValues < 12 && Num32BitValuesToSet <= 12 - DestOffsetIn32BitValues)
        { std::memcpy(state_.constants + DestOffsetIn32BitValues, pSrcData, size_t(Num32BitValuesToSet) * 4); state_.constantMask |= ((1u << Num32BitValuesToSet) - 1) << DestOffsetIn32BitValues; }
        real_->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues)
{
    if (!real_) return;
    real_->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    Escape("root_cbv"); real_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    real_->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    Escape("root_srv"); real_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    real_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    Escape("root_uav"); real_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
    if (!real_) return;
    real_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE ProviderList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView)
{
    if (!real_) return;
    real_->IASetIndexBuffer(pView);
}

void STDMETHODCALLTYPE ProviderList::IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews)
{
    if (!real_) return;
    real_->IASetVertexBuffers(StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE ProviderList::SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews)
{
    if (!real_) return;
    real_->SOSetTargets(StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE ProviderList::OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor)
{
    if (!real_) return;
    real_->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE ProviderList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects)
{
    if (!real_) return;
    real_->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE ProviderList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[ 4 ], UINT NumRects, const D3D12_RECT *pRects)
{
    if (!real_) return;
    real_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
}

void STDMETHODCALLTYPE ProviderList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects)
{
    if (!real_) return;
    real_->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
}

void STDMETHODCALLTYPE ProviderList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects)
{
    if (!real_) return;
    real_->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
}

void STDMETHODCALLTYPE ProviderList::DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion)
{
    if (!real_) return;
    real_->DiscardResource(pResource, pRegion);
}

void STDMETHODCALLTYPE ProviderList::BeginQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    if (!real_) return;
    real_->BeginQuery(pQueryHeap, Type, Index);
}

void STDMETHODCALLTYPE ProviderList::EndQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    if (!real_) return;
    real_->EndQuery(pQueryHeap, Type, Index);
}

void STDMETHODCALLTYPE ProviderList::ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource *pDestinationBuffer, UINT64 AlignedDestinationBufferOffset)
{
    if (!real_) return;
    real_->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset);
}

void STDMETHODCALLTYPE ProviderList::SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation)
{
    if (!real_) return;
    real_->SetPredication(pBuffer, AlignedBufferOffset, Operation);
}

void STDMETHODCALLTYPE ProviderList::SetMarker(UINT Metadata, const void *pData, UINT Size)
{
    if (!real_) return;
    real_->SetMarker(Metadata, pData, Size);
}

void STDMETHODCALLTYPE ProviderList::BeginEvent(UINT Metadata, const void *pData, UINT Size)
{
    if (!real_) return;
    real_->BeginEvent(Metadata, pData, Size);
}

void STDMETHODCALLTYPE ProviderList::EndEvent()
{
    if (!real_) return;
    real_->EndEvent();
}

void STDMETHODCALLTYPE ProviderList::ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset)
{
    if (!real_) return;
    Escape("indirect"); real_->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
}

void STDMETHODCALLTYPE ProviderList::AtomicCopyBufferUINT(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges)
{
    if (!list2_.Get()) return;
    list2_.Get()->AtomicCopyBufferUINT(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}

void STDMETHODCALLTYPE ProviderList::AtomicCopyBufferUINT64(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges)
{
    if (!list2_.Get()) return;
    list2_.Get()->AtomicCopyBufferUINT64(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}

void STDMETHODCALLTYPE ProviderList::OMSetDepthBounds(FLOAT Min, FLOAT Max)
{
    if (!list2_.Get()) return;
    list2_.Get()->OMSetDepthBounds(Min, Max);
}

void STDMETHODCALLTYPE ProviderList::SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions)
{
    if (!list2_.Get()) return;
    list2_.Get()->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
}

void STDMETHODCALLTYPE ProviderList::ResolveSubresourceRegion(ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource, UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode)
{
    if (!list2_.Get()) return;
    list2_.Get()->ResolveSubresourceRegion(pDstResource, DstSubresource, DstX, DstY, pSrcResource, SrcSubresource, pSrcRect, Format, ResolveMode);
}

void STDMETHODCALLTYPE ProviderList::SetViewInstanceMask(UINT Mask)
{
    if (!list2_.Get()) return;
    list2_.Get()->SetViewInstanceMask(Mask);
}

void STDMETHODCALLTYPE ProviderList::WriteBufferImmediate(UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes)
{
    if (!list2_.Get()) return;
    list2_.Get()->WriteBufferImmediate(Count, pParams, pModes);
}
}
