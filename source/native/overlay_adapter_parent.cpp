#include "overlay_adapter_parent.h"
#include "process_mitigations.h"
#include "provider_image.h"
#include "overlay_dxgi_proxy.h"
#include "overlay_native.h"
#include "overlay_platform.h"
#include "overlay_graphics_api.h"
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace single_overlay::adapter_parent {
namespace {
using Microsoft::WRL::ComPtr;
using Parent=proxy::ParentFn;
struct Binding {
    void* target=nullptr;
    std::atomic<Parent> original{nullptr};
    std::atomic<bool> active{false};
    std::array<unsigned char,5> published{};
    const unsigned char* relay=nullptr;
    std::array<unsigned char,14> relayBytes{};
};
// Pinned public method owners and their trampolines have process lifetime.
std::array<Binding,4> bindings{};
std::mutex publication;
std::atomic<uint64_t> observed{0}, parentCalls{0};

bool SystemDxgi(HMODULE& image) noexcept {
    wchar_t expected[MAX_PATH]{},actual[MAX_PATH]{};
    image=GetModuleHandleW(L"dxgi.dll");
    return image && GetSystemDirectoryW(expected,MAX_PATH)
        && !wcscat_s(expected,L"\\dxgi.dll")
        && GetModuleFileNameW(image,actual,MAX_PATH) && !_wcsicmp(expected,actual);
}
bool Current(const Binding& b) noexcept {
    return b.target && b.relay && !memcmp(b.target,b.published.data(),b.published.size())
        && !memcmp(b.relay,b.relayBytes.data(),b.relayBytes.size());
}
bool PublishedTo(Binding& b,void* trampoline,Parent replacement) noexcept {
    // The pinned x64 MinHook implementation puts its FF25 relay in the same
    // 64-byte allocation as this hook's trampoline. Validate the decoded route
    // before activation instead of accepting arbitrary post-publication bytes.
    const auto* relay=provider_image::MinHookRelay(b.target,reinterpret_cast<const void*>(replacement));
    const auto address=reinterpret_cast<uintptr_t>(relay),begin=reinterpret_cast<uintptr_t>(trampoline);
    if(!relay||address<begin||address-begin>64-b.relayBytes.size())return false;
    memcpy(b.published.data(),b.target,b.published.size());
    memcpy(b.relayBytes.data(),relay,b.relayBytes.size());b.relay=relay;
    return Current(b);
}
template<size_t I> HRESULT STDMETHODCALLTYPE GetParent(IDXGIObject* object,REFIID iid,void** output) {
    auto& binding=bindings[I];
    const auto original=binding.original.load(std::memory_order_acquire);
    if (!original) return E_UNEXPECTED; // Never published before its trampoline.
    const auto caller=_ReturnAddress();
    if (!binding.active.load(std::memory_order_acquire)||gInsideOverlay||native::InsideLoader()
        ||!slots::ImageEntry(GetModuleHandleW(nullptr),caller)||!Current(binding))
        return original(object,iid,output);
    // The callable is learned only from QI(IDXGIAdapter). Do not route another
    // object's shared method through the overlay, or infer a private layout.
    HMODULE dxgi=nullptr;
    if (!SystemDxgi(dxgi)||!native::SystemDxgiObject(object,9)
        ||(*reinterpret_cast<void***>(object))[6]!=binding.target)
        return original(object,iid,output);
    ++parentCalls;
    return proxy::ParentFactoryCall(original,object,iid,output);
}
template<size_t... I> auto Entries(std::index_sequence<I...>) {
    return std::array<Parent,sizeof...(I)>{&GetParent<I>...};
}
const auto entries=Entries(std::make_index_sequence<4>{});

void ObserveAdapter(IUnknown* object,const wchar_t* armed) noexcept {
    if (!object||gInsideOverlay||native::InsideLoader()||!single_module::OwnsBackend()) return;
    InternalScope internal;
    // No probing, adapter enumeration, replacement adapter, or native vtable
    // writes. Inspect only the caller's actual, successfully queried adapter.
    if (!native::PinInterface(object,2)) return;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&adapter)))||!adapter) return;
    HMODULE dxgi=nullptr;
    if (!SystemDxgi(dxgi)||!native::SystemDxgiObject(adapter.Get(),9)||!HookMitigationsSupported()) return;
    void* target=(*reinterpret_cast<void***>(adapter.Get()))[6];
    if (!slots::ImageEntry(dxgi,target)) return;
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target),&pinned)||pinned!=dxgi) return;
    ++observed;
    std::lock_guard lock(publication);
    size_t index=bindings.size();
    for(size_t i=0;i<bindings.size();++i) {
        if(bindings[i].target==target)return;
        if(!bindings[i].target&&index==bindings.size())index=i;
    }
    if(index==bindings.size())return;
    auto& binding=bindings[index];
    DWORD protection=0;
    if(!protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(target),protection,8))return;
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return;
    std::array<unsigned char,16> before{};memcpy(before.data(),target,before.size());
    void* trampoline=nullptr;
    const auto created=MH_CreateHook(target,reinterpret_cast<void*>(entries[index]),&trampoline);
    if(created!=MH_OK||!trampoline) {
        single_module::Log(L"MFG_PROXY_UI adapter-parent entry unavailable: public method could not be prepared");return;
    }
    if(memcmp(target,before.data(),before.size())) { MH_RemoveHook(target);return; }
    binding.target=target;
    binding.original.store(reinterpret_cast<Parent>(trampoline),std::memory_order_release);
    // Once enabled, retain the inactive trampoline even on verification failure.
    // Concurrent callers always have a valid original and never a freed relay.
    const auto enabled=MH_EnableHook(target);
    if(enabled!=MH_OK) {
        single_module::Log(L"MFG_PROXY_UI adapter-parent entry unavailable: publication rejected");return;
    }
    if(!protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(target),protection,8)
        ||!PublishedTo(binding,trampoline,entries[index]))return;
    binding.active.store(true,std::memory_order_release);
    single_module::Log(armed);
}
}

void Observe(IUnknown* object) noexcept {
    ObserveAdapter(object,L"MFG_PROXY_UI adapter-parent entry armed from application D3D12 adapter; caller=main-executable adapterReplacement=0 nativeTableWrites=0");
}

void ObserveD3D11(IUnknown* device) noexcept {
    if (!device||gInsideOverlay||native::InsideLoader()||!single_module::OwnsBackend()) return;
    // Never in a D3D12 or Vulkan application, whichever API came first: its adapter-parent route stays as it was.
    if (!graphics_api::D3D11Application()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            wchar_t line[200]{};
            swprintf_s(line,L"MFG_PROXY_UI adapter-parent entry not armed from a D3D11 device: D3D12 or Vulkan application (step=application_api seen=0x%X first=0x%X)",
                graphics_api::Seen(),graphics_api::First());
            single_module::Log(line);
        }
        return;
    }
    ComPtr<IDXGIAdapter> adapter;
    {
        // The device's own adapter, through its public IDXGIDevice: the one GetParent gives the game's factory.
        InternalScope internal;
        ComPtr<IDXGIDevice> dxgi;
        if (!native::PinInterface(device,2)||FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||!dxgi
            ||!native::PinInterface(dxgi.Get(),11)||FAILED(dxgi->GetAdapter(&adapter))||!adapter) return;
    }
    ObserveAdapter(adapter.Get(),L"MFG_PROXY_UI adapter-parent entry armed from application D3D11 device adapter; caller=main-executable adapterReplacement=0 nativeTableWrites=0");
}
}
