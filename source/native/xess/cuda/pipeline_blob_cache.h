#pragma once

#include "../../sha256_util.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xefg_cuda
{
// Private data follows the root signature's lifetime. Hash the serialized input so a recreated root
// signature can match while a reused COM address cannot.
inline constexpr GUID kPipelineCacheRootSignature =
    {0xe8a622f0, 0xf77a, 0x431f, {0x9d, 0x71, 0x7e, 0x43, 0x1b, 0xd8, 0x90, 0xa2}};
inline constexpr size_t kMaxPipelineInputBytes = 1 << 20;
struct PipelineRootSignature
{
    sha256_util::Digest digest{};
    UINT nodeMask{};
    bool operator==(const PipelineRootSignature&) const = default;
};
static_assert(sizeof(PipelineRootSignature) == 36);

inline void RecordPipelineRootSignature(ID3D12RootSignature* root, UINT nodeMask, const void* bytes, SIZE_T size) noexcept
{
    if (!bytes || !size || size > kMaxPipelineInputBytes) return;
    PipelineRootSignature signature{};
    signature.nodeMask = nodeMask;
    if (sha256_util::Compute(bytes, size, signature.digest))
        root->SetPrivateData(kPipelineCacheRootSignature, sizeof(signature), &signature);
}

class PipelineBlobCache
{
public:
    // A newly created context can use blobs from builds completed before teardown. Cold calls
    // always pass the original descriptor through, even if the same pipeline is made twice.
    void ContextRetired() noexcept
    {
        std::lock_guard lock(mutex_);
        ++generation_;
    }

    template <class Create>
    HRESULT CreateCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out, Create create) noexcept
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cachedDesc{};
        Key key{};
        if (!desc || !out || iid != __uuidof(ID3D12PipelineState) || !Describe(desc, cachedDesc, key))
            return create(desc, out);

        BlobPtr blob;
        bool remember = false;
        uint64_t generation;
        {
            std::lock_guard lock(mutex_);
            generation = generation_;
            const Entry* entry = Find(key);
            if (entry && entry->generation < generation) blob = entry->blob;
            remember = !entry && count_ < entries_.size() && bytes_ < kMaxBytes;
        }

        if (blob)
        {
            cachedDesc.CachedPSO = {blob->data(), blob->size()};
            // Keep the blob alive across the call, including if a concurrent call rejects it.
            void* created = nullptr;
            const HRESULT result = create(&cachedDesc, &created);
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
            pipeline.Attach(static_cast<ID3D12PipelineState*>(created));
            if (SUCCEEDED(result) && pipeline)
            {
                *out = pipeline.Detach();
                return result;
            }
            Reject(key, blob);
        }

        const HRESULT result = create(desc, out);
        if (remember && SUCCEEDED(result) && *out)
            Remember(key, generation, static_cast<ID3D12PipelineState*>(*out));
        return result;
    }

private:
    using Blob = std::vector<unsigned char>;
    using BlobPtr = std::shared_ptr<const Blob>;
    struct Key
    {
        sha256_util::Digest shader{};
        PipelineRootSignature root{};
        bool hasExplicitRoot{};
        SIZE_T shaderSize{};
        UINT nodeMask{};
        D3D12_PIPELINE_STATE_FLAGS flags{};
        bool operator==(const Key&) const = default;
    };
    struct Entry
    {
        Key key{};
        BlobPtr blob; // Empty after rejection. Do not send another blob for this key to the same device.
        uint64_t generation{};
    };
    static constexpr size_t kMaxBlobBytes = 2 << 20;
    static constexpr size_t kMaxBytes = 32 << 20;

    static bool Describe(const D3D12_COMPUTE_PIPELINE_STATE_DESC* input, D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                         Key& key) noexcept
    {
        __try
        {
            desc = *input;
            if (desc.CachedPSO.pCachedBlob || desc.CachedPSO.CachedBlobSizeInBytes || !desc.CS.pShaderBytecode
                || !desc.CS.BytecodeLength || desc.CS.BytecodeLength > kMaxPipelineInputBytes)
                return false;
            key.hasExplicitRoot = desc.pRootSignature != nullptr;
            if (key.hasExplicitRoot)
            {
                UINT bytes = sizeof(key.root);
                if (FAILED(desc.pRootSignature->GetPrivateData(kPipelineCacheRootSignature, &bytes, &key.root))
                    || bytes != sizeof(key.root))
                    return false;
            }
            key.shaderSize = desc.CS.BytecodeLength;
            key.nodeMask = desc.NodeMask;
            key.flags = desc.Flags;
            return sha256_util::Compute(desc.CS.pShaderBytecode, key.shaderSize, key.shader);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    Entry* Find(const Key& key) noexcept // mutex_ held
    {
        for (size_t i = 0; i < count_; ++i)
            if (entries_[i].key == key) return &entries_[i];
        return nullptr;
    }

    void Remember(const Key& key, uint64_t generation, ID3D12PipelineState* pipeline) noexcept
    {
        Microsoft::WRL::ComPtr<ID3DBlob> driverBlob;
        if (FAILED(pipeline->GetCachedBlob(&driverBlob)) || !driverBlob) return;
        const size_t size = driverBlob->GetBufferSize();
        const auto* data = static_cast<const unsigned char*>(driverBlob->GetBufferPointer());
        if (!data || !size || size > kMaxBlobBytes) return;
        try
        {
            auto copy = std::make_shared<const Blob>(data, data + size);
            std::lock_guard lock(mutex_);
            if (generation_ != generation || count_ == entries_.size() || size > kMaxBytes - bytes_ || Find(key)) return;
            entries_[count_++] = {key, std::move(copy), generation};
            bytes_ += size;
        }
        catch (...) {} // Allocation failure keeps the original PSO and result.
    }

    void Reject(const Key& key, const BlobPtr& attempted) noexcept
    {
        std::lock_guard lock(mutex_);
        Entry* entry = Find(key);
        if (entry && entry->blob == attempted)
        {
            bytes_ -= attempted->size();
            entry->blob.reset();
        }
    }

    std::mutex mutex_;
    std::array<Entry, 128> entries_{};
    size_t count_{};
    size_t bytes_{};
    uint64_t generation_{};
};
}
