// A module that hooks pipeline creation only, in its own image: the shape of the SCSKiller
// shader recorder's d3d12.dll, which hooks the device a layer (ReShade) returns. Slots 11 (CreateComputePipelineState)
// and 47 (ID3D12Device2::CreatePipelineState) of that device's table point here and pass each call on to the table's
// original from this image; list creation is not hooked. The provider's pipeline calls then reach the wrapper from here.
#include <windows.h>
#include <d3d12.h>

namespace
{
using CreateComputeFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStreamFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
CreateComputeFn gCompute{};
CreateStreamFn gStream{};
volatile LONG gRecorded{};

// Counted after the call: this image's frame stays on the stack (no tail call).
HRESULT STDMETHODCALLTYPE RecordCompute(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out)
{
    const HRESULT result = gCompute(self, desc, iid, out);
    if (SUCCEEDED(result)) InterlockedIncrement(&gRecorded);
    return result;
}
HRESULT STDMETHODCALLTYPE RecordStream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out)
{
    const HRESULT result = gStream(self, desc, iid, out);
    if (SUCCEEDED(result)) InterlockedIncrement(&gRecorded);
    return result;
}

bool Patch(void** slot, void* hook)
{
    DWORD old{}, unused{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &unused);
    return true;
}
}

// Hooks the two slots of device's table (once per process).
extern "C" __declspec(dllexport) BOOL XefgRecordPipelines(ID3D12Device* device)
{
    if (!device || gCompute) return FALSE;
    void** table = *reinterpret_cast<void***>(device);
    gCompute = reinterpret_cast<CreateComputeFn>(table[11]);
    gStream = reinterpret_cast<CreateStreamFn>(table[47]);
    return Patch(&table[11], reinterpret_cast<void*>(&RecordCompute)) && Patch(&table[47], reinterpret_cast<void*>(&RecordStream));
}

// Pipelines created through the hooks.
extern "C" __declspec(dllexport) LONG XefgRecordedPipelines() { return gRecorded; }
