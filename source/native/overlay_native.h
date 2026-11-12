#pragma once
#include "overlay_slots.h"
#include <intrin.h>
#include <wrl/client.h>
#include <array>
namespace single_overlay::native {
using LoaderCallout = BOOLEAN (NTAPI*)();
// The loader lock (PEB+0x110) as the session diagnostics read it.
inline const RTL_CRITICAL_SECTION* LoaderLock() noexcept {
    const auto* peb = reinterpret_cast<const unsigned char*>(__readgsqword(0x60));
    return peb ? *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + 0x110) : nullptr;
}
// Wine and Proton do not export RtlIsThreadWithinLoaderCallout (GitHub #131); their loader holds
// PEB->LoaderLock through DllMain and TLS callbacks, so its owner answers instead. Without either, fail closed.
inline bool InsideLoader(LoaderCallout callout, const RTL_CRITICAL_SECTION* lock) noexcept {
    if (callout) return callout();
    return !lock || reinterpret_cast<DWORD_PTR>(lock->OwningThread) == GetCurrentThreadId();
}
inline bool InsideLoader() noexcept {
    static auto fn = reinterpret_cast<LoaderCallout>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsThreadWithinLoaderCallout"));
    return InsideLoader(fn, LoaderLock());
}
inline bool PinInterface(IUnknown* object, size_t last) noexcept {
    if (!object || last > 64) return false;
    auto** table = *reinterpret_cast<void***>(object);
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(table), &owner)
        || !slots::ImageSlot(owner, table) || !slots::ImageSlot(owner, table + last)) return false;
    for (size_t i = 0; i <= last; ++i) {
        HMODULE callable = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(table[i]), &callable) || !slots::ImageEntry(callable, table[i])) return false;
    }
    return true;
}
inline bool SystemDxgiObject(IUnknown* object, size_t last) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!object || !GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll")) return false;
    const HMODULE dxgi = GetModuleHandleW(path);
    auto** table = *reinterpret_cast<void***>(object);
    return slots::ImageSlot(dxgi, table) && slots::ImageSlot(dxgi, table + last) && PinInterface(object, last);
}
// Streamline's StreamlineRetrieveBaseInterface: its proxies answer with the object they wrap.
inline constexpr GUID kStreamlineBase = {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
// NVIDIA, ReShade and Special K's public COM unwrapping contracts. No private
// object offsets, no replacement of an application's interface, and bounded
// cycle detection.
template<class T> bool Unwrap(IUnknown* input, Microsoft::WRL::ComPtr<T>& output) noexcept {
    using Microsoft::WRL::ComPtr;
    constexpr GUID baseGuid = kStreamlineBase;
    // ReShade 6.8.0 source/com_utils.hpp: IID_UnwrappedObject. The returned
    // original owns an AddRef, as with any successful QueryInterface call.
    constexpr GUID reshadeGuid = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
    // Special K 26.9.25 include/SpecialK/render/dxgi/dxgi_swapchain.h:
    // IID_IUnwrappedDXGISwapChain. Only its swapchain wrapper answers, with its
    // AddRef'd real swapchain; it refuses Streamline's base IID by design and
    // passes other IIDs to that real swapchain.
    constexpr GUID specialKGuid = {0xe8a33b4a,0x1405,0x424c,{0xae,0x88,0x0d,0x3e,0x9d,0x46,0xc9,0x14}};
    output.Reset();
    ComPtr<IUnknown> current = input;
    std::array<ComPtr<IUnknown>, 4> visited;
    for (size_t i = 0; current && i < visited.size(); ++i) {
        if (!PinInterface(current.Get(), 2) || FAILED(current.As(&visited[i]))) return false;
        for (size_t j = 0; j < i; ++j) if (visited[j].Get() == visited[i].Get()) return false;
        ComPtr<IUnknown> base;
        HRESULT hr = current->QueryInterface(baseGuid, reinterpret_cast<void**>(base.GetAddressOf()));
        for (const GUID& next : {reshadeGuid, specialKGuid}) {
            if (hr != E_NOINTERFACE) break;
            if (base) return false;
            hr = current->QueryInterface(next, reinterpret_cast<void**>(base.GetAddressOf()));
        }
        if (hr == E_NOINTERFACE && base) return false;
        if (hr == E_NOINTERFACE) return SUCCEEDED(current.As(&output)) && output;
        if (hr != S_OK || !base) return false;
        current = base;
    }
    return false;
}
}
