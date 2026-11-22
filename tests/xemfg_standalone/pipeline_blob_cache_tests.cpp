#include "xess/cuda/pipeline_blob_cache.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
using Microsoft::WRL::ComPtr;
using xefg_cuda::PipelineBlobCache;

constexpr unsigned char kShader[] = {1, 2, 3};
constexpr unsigned char kOtherShader[] = {1, 2, 4};
constexpr unsigned char kRoot[] = {5, 6, 7};
constexpr unsigned char kOtherRoot[] = {5, 6, 8};
constexpr unsigned char kDriverBlob[] = {10, 20, 30, 40};

void Check(bool passed, const char* message)
{
    if (passed) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

template <class Interface>
class ComObject : public Interface
{
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        *out = nullptr;
        if (iid != __uuidof(Interface) && iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }
protected:
    virtual ~ComObject() = default;
private:
    ULONG refs_{1};
};

template <class Interface>
class DeviceChild : public ComObject<Interface>
{
public:
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        if (guid != xefg_cuda::kPipelineCacheRootSignature || size != sizeof(root_)) return E_INVALIDARG;
        std::memcpy(&root_, data, size);
        tagged_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        if (guid != xefg_cuda::kPipelineCacheRootSignature || !tagged_) return DXGI_ERROR_NOT_FOUND;
        if (*size < sizeof(root_)) return DXGI_ERROR_MORE_DATA;
        *size = sizeof(root_);
        std::memcpy(data, &root_, *size);
        return S_OK;
    }
private:
    xefg_cuda::PipelineRootSignature root_{};
    bool tagged_{};
};

struct RefCounts
{
    unsigned pipelines{};
    unsigned blobs{};
    unsigned extracts{};
};

class DriverBlob : public ComObject<ID3DBlob>
{
public:
    explicit DriverBlob(RefCounts& counts) : counts_(counts)
    {
        ++counts_.blobs;
        std::memcpy(data_, kDriverBlob, sizeof(data_));
    }
    ~DriverBlob() override { --counts_.blobs; }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data_; }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return sizeof(data_); }
private:
    RefCounts& counts_;
    unsigned char data_[sizeof(kDriverBlob)]{};
};

class Pipeline : public DeviceChild<ID3D12PipelineState>
{
public:
    explicit Pipeline(RefCounts& counts) : counts_(counts) { ++counts_.pipelines; }
    ~Pipeline() override { --counts_.pipelines; }
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** out) override
    {
        ++counts_.extracts;
        *out = new DriverBlob(counts_);
        return S_OK;
    }
private:
    RefCounts& counts_;
};

struct Fixture
{
    PipelineBlobCache cache;
    RefCounts counts;

    template <class Inspect>
    void Create(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, Inspect inspect,
                HRESULT expected = S_OK, REFIID iid = __uuidof(ID3D12PipelineState))
    {
        void* created = nullptr;
        const HRESULT result = cache.CreateCompute(&desc, iid, &created,
            [&](const D3D12_COMPUTE_PIPELINE_STATE_DESC* input, void** out)
            {
                const HRESULT status = inspect(input);
                *out = new Pipeline(counts);
                return status;
            });
        ComPtr<ID3D12PipelineState> pipeline;
        pipeline.Attach(static_cast<ID3D12PipelineState*>(created));
        Check(result == expected && pipeline, "Create must preserve the final call's result and output");
        Check(counts.blobs == 0, "Cache must release the driver blob before returning");
    }

    void Expect(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, bool hit)
    {
        unsigned calls = 0;
        Create(desc, [&](const D3D12_COMPUTE_PIPELINE_STATE_DESC* input)
        {
            ++calls;
            Check((input != &desc) == hit, "Unexpected cache hit or miss");
            if (hit) CheckBlob(*input);
            return S_OK;
        });
        Check(calls == 1, "A successful create should call the driver once");
        Check(counts.pipelines == 0, "Cache must not retain any pipeline state");
    }

    static void CheckBlob(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
    {
        Check(desc.CachedPSO.pCachedBlob && desc.CachedPSO.CachedBlobSizeInBytes == sizeof(kDriverBlob)
            && std::memcmp(desc.CachedPSO.pCachedBlob, kDriverBlob, sizeof(kDriverBlob)) == 0,
            "CachedPSO must reference a copy of the expected driver bytes");
    }
};

D3D12_COMPUTE_PIPELINE_STATE_DESC Description(ID3D12RootSignature* root = nullptr)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {kShader, sizeof(kShader)};
    return desc;
}

void Hits()
{
    Fixture f;
    ComPtr<ID3D12RootSignature> root, recreated;
    root.Attach(new DeviceChild<ID3D12RootSignature>);
    recreated.Attach(new DeviceChild<ID3D12RootSignature>);
    xefg_cuda::RecordPipelineRootSignature(root.Get(), 0, kRoot, sizeof(kRoot));
    xefg_cuda::RecordPipelineRootSignature(recreated.Get(), 0, kRoot, sizeof(kRoot));
    const auto cold = Description(root.Get());
    f.Expect(cold, false);
    f.Expect(cold, false); // Duplicate creation before teardown still uses the original path.
    Check(f.counts.extracts == 1, "Duplicate cold creation should not extract another blob");
    Check(root.Reset() == 0, "Cache must not retain the original root signature");
    f.cache.ContextRetired();
    f.Expect(Description(recreated.Get()), true);
    Check(f.counts.extracts == 1, "Cache hit should not extract another blob");
}

void Misses()
{
    Fixture f;
    ComPtr<ID3D12RootSignature> root;
    root.Attach(new DeviceChild<ID3D12RootSignature>);
    const auto cold = Description();
    f.Expect(cold, false);
    f.cache.ContextRetired();
    auto changed = cold;
    changed.CS = {kOtherShader, sizeof(kOtherShader)};
    f.Expect(changed, false); // Equal byte counts do not establish a match.
    changed = cold;
    changed.NodeMask = 1;
    f.Expect(changed, false);
    changed = cold;
    changed.Flags = D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG;
    f.Expect(changed, false);
    f.Expect(Description(root.Get()), false); // Unknown explicit root.
    xefg_cuda::RecordPipelineRootSignature(root.Get(), 0, kRoot, sizeof(kRoot));
    f.Expect(Description(root.Get()), false); // Explicit and implicit roots are separate.
    f.cache.ContextRetired();
    xefg_cuda::RecordPipelineRootSignature(root.Get(), 1, kRoot, sizeof(kRoot));
    f.Expect(Description(root.Get()), false); // Different root signature node mask.
    xefg_cuda::RecordPipelineRootSignature(root.Get(), 0, kOtherRoot, sizeof(kOtherRoot));
    f.Expect(Description(root.Get()), false); // Changed serialized root at the same address.
    changed = cold;
    changed.CachedPSO = {kOtherShader, sizeof(kOtherShader)};
    f.Expect(changed, false);
    f.Create(cold, [&](const auto* input)
    {
        Check(input == &cold, "Requests for another interface must pass through");
        return S_OK;
    }, S_OK, __uuidof(IUnknown));
    Check(f.cache.CreateCompute(&cold, __uuidof(ID3D12PipelineState), nullptr, [&](const auto* input, void** output)
    {
        Check(input == &cold && !output, "Validation-only requests must pass through");
        return S_FALSE;
    }) == S_FALSE, "Validation-only requests must preserve the result");
}

void Lifetime()
{
    Fixture f;
    const auto desc = Description();
    f.Create(desc, [&](const auto* input)
    {
        Check(input == &desc, "Late publication setup must be a cold create");
        f.cache.ContextRetired();
        return S_OK;
    });
    f.Expect(desc, false); // The create completed after its generation retired.
    f.cache.ContextRetired();
    f.Create(desc, [&](const auto* outer)
    {
        Fixture::CheckBlob(*outer);
        unsigned calls = 0;
        f.Create(desc, [&](const auto* inner)
        {
            ++calls;
            if (inner == &desc) return S_FALSE;
            Fixture::CheckBlob(*inner);
            return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
        }, S_FALSE);
        Check(calls == 2, "Driver rejection must retry exactly once with the original descriptor");
        Check(f.counts.pipelines == 0, "Rejected and retried outputs must be released");
        Fixture::CheckBlob(*outer); // The nested call evicted the blob while the outer call still owned it.
        return S_OK;
    });
    f.cache.ContextRetired();
    f.Expect(desc, false); // A rejected key stays on the original path.
    Check(f.counts.pipelines == 0 && f.counts.blobs == 0, "Cache must retain only its own blob bytes");
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view selected = argv[1];
    if (selected == "hits") Hits();
    else if (selected == "misses") Misses();
    else if (selected == "lifetime") Lifetime();
    else return 2;
    return 0;
}
