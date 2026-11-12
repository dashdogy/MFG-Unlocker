// A device wrapper in its own image, the shape of ReShade's for the route. Its table is in
// this image, ReShade's IID_UnwrappedObject gives the runtime's device under it (the route resolves it by unwrap), and
// CreateComputePipelineState / ID3D12Device2::CreatePipelineState are passed to that device from this image. Every
// other method fails: the tests call only these on it.
// extraSubobject: a compute pipeline is passed on as a CreatePipelineState stream that also carries a default
// SAMPLE_MASK subobject (a wrapper that adds benign state); the route does not parse such a stream and logs it.
#include <windows.h>
#include <d3d12.h>
#include <new>

namespace
{
constexpr GUID kUnwrapped = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
struct Wrapper { void** table; ID3D12Device* native; bool extraSubobject; LONG references; };

HRESULT STDMETHODCALLTYPE Query(Wrapper* self, REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == kUnwrapped) { self->native->AddRef(); *out = self->native; return S_OK; }
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12Device)
        || iid == __uuidof(ID3D12Device1) || iid == __uuidof(ID3D12Device2))
    { InterlockedIncrement(&self->references); *out = self; return S_OK; }
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE AddRef(Wrapper* self) { return ULONG(InterlockedIncrement(&self->references)); }
ULONG STDMETHODCALLTYPE Release(Wrapper* self) { return ULONG(InterlockedDecrement(&self->references)); } // kept for the process
HRESULT STDMETHODCALLTYPE Unsupported() { return E_NOTIMPL; }

HRESULT Stream(ID3D12Device* native, const D3D12_PIPELINE_STATE_STREAM_DESC& desc, REFIID iid, void** out)
{
    ID3D12Device2* device2{};
    HRESULT result = native->QueryInterface(IID_PPV_ARGS(&device2));
    if (SUCCEEDED(result)) { result = device2->CreatePipelineState(&desc, iid, out); device2->Release(); }
    return result;
}
// The work after each call keeps this image's frame on the stack (no tail call), as a wrapper's own bookkeeping does.
volatile LONG gForwarded{};
HRESULT STDMETHODCALLTYPE CreateComputePipelineState(Wrapper* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out)
{
    HRESULT result{};
    if (!self->extraSubobject || !desc) result = self->native->CreateComputePipelineState(desc, iid, out);
    else
    {
        struct alignas(void*) Root { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; ID3D12RootSignature* root; };
        struct alignas(void*) Shader { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; D3D12_SHADER_BYTECODE cs; };
        struct alignas(void*) Mask { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; UINT mask; };
        struct { Root root; Shader cs; Mask mask; } stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, desc->pRootSignature},
            {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, desc->CS}, {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT_MAX}};
        result = Stream(self->native, {sizeof(stream), &stream}, iid, out);
    }
    InterlockedIncrement(&gForwarded);
    return result;
}
HRESULT STDMETHODCALLTYPE CreatePipelineState(Wrapper* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out)
{
    const HRESULT result = desc ? Stream(self->native, *desc, iid, out) : E_INVALIDARG;
    InterlockedIncrement(&gForwarded);
    return result;
}

void* gTable[64]{};
}

// The wrapper of native (held for the process); its table: IUnknown, CreateComputePipelineState (11), CreatePipelineState (47).
extern "C" __declspec(dllexport) HRESULT XefgWrapDevice(ID3D12Device* native, BOOL extraSubobject, ID3D12Device** out)
{
    if (!native || !out) return E_POINTER;
    for (auto& slot : gTable) slot = reinterpret_cast<void*>(&Unsupported);
    gTable[0] = reinterpret_cast<void*>(&Query);
    gTable[1] = reinterpret_cast<void*>(&AddRef);
    gTable[2] = reinterpret_cast<void*>(&Release);
    gTable[11] = reinterpret_cast<void*>(&CreateComputePipelineState);
    gTable[47] = reinterpret_cast<void*>(&CreatePipelineState);
    native->AddRef();
    auto* wrapper = new (std::nothrow) Wrapper{gTable, native, extraSubobject != FALSE, 1};
    if (!wrapper) return E_OUTOFMEMORY;
    *out = reinterpret_cast<ID3D12Device*>(wrapper);
    return S_OK;
}
