#include "overlay_application_imports.h"
#include "single_overlay.h"
#include "overlay_build.h"
#include "overlay_slots.h"
#include "overlay_platform.h"
#include "xess/xess_mod.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace single_overlay::application_imports {
namespace {
using Resolver = FARPROC (WINAPI*)(HMODULE,LPCSTR);
struct Binding {
    std::atomic<HMODULE> importer{nullptr}; // Pinned before IAT publication; never reused.
    std::atomic<Resolver> original{nullptr};
    bool complete = false;
};
std::array<Binding,32> bindings{};
std::atomic_flag publishing = ATOMIC_FLAG_INIT;

bool FactoryName(const char* name) noexcept {
    return !strcmp(name,"CreateDXGIFactory") || !strcmp(name,"CreateDXGIFactory1")
        || !strcmp(name,"CreateDXGIFactory2");
}
template<size_t I> __declspec(noinline) FARPROC WINAPI Resolve(HMODULE module,LPCSTR name) {
    const auto original=bindings[I].original.load(std::memory_order_acquire);
    if (!original) return nullptr;
    FARPROC result=nullptr;
    if (!xess_mod::ResolveNativeSdkCreate(bindings[I].importer.load(std::memory_order_acquire),
            _ReturnAddress(),module,name,result)) result=original(module,name);
    const DWORD error=GetLastError();
    if (result && name && reinterpret_cast<uintptr_t>(name)>0xffff) {
        // Resolve a renderer's dependencies before returning their exports to
        // the caller. Only the three factory names are redirected here.
        ArmModule(module);
        if (FactoryName(name)) result=single_overlay::ResolveProc(module,name,result);
    }
    SetLastError(error);
    return result;
}
template<size_t... I> auto Entries(std::index_sequence<I...>) {
    return std::array<Resolver,sizeof...(I)>{&Resolve<I>...};
}
const auto resolvers=Entries(std::make_index_sequence<32>{});

struct Reference {
    HMODULE module=nullptr;
    explicit Reference(HMODULE candidate) noexcept {
        if (!candidate || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(candidate),&module) || module!=candidate) {
            if (module) FreeLibrary(module);
            module=nullptr;
        }
    }
    ~Reference() { if (module) FreeLibrary(module); }
};
// Resolver interception runs on game-owned worker threads, including threads
// with small stack reservations. Long-path storage must not live on that stack.
struct ModulePaths { wchar_t module[32768]; wchar_t application[32768]; };
bool LocalApplicationDll(HMODULE module,ModulePaths& paths) noexcept {
    if (!module || module==GetModuleHandleW(nullptr)) return false;
    auto& path=paths.module;
    auto& application=paths.application;
    const DWORD n=GetModuleFileNameW(module,path,_countof(path));
    const DWORD m=GetModuleFileNameW(nullptr,application,_countof(application));
    if (!n || n>=_countof(path) || !m || m>=_countof(application)) return false;
    auto* separator=wcsrchr(application,L'\\');
    if (!separator) return false;
    const size_t root=separator-application+1;
    if (n<=root || _wcsnicmp(application,path,root)) return false;
    const auto* leaf=wcsrchr(path,L'\\'); leaf=leaf?leaf+1:path;
    const size_t length=wcslen(leaf);
    if (length<5 || _wcsicmp(leaf+length-4,L".dll")) return false;
    // These modules own middleware/native implementation paths, not application
    // factory calls. Export checks also reject renamed providers and overlays.
    if (!_wcsnicmp(leaf,L"sl.",3) || !_wcsnicmp(leaf,L"nvngx",5)
        || !_wcsnicmp(leaf,L"_nvngx",6) || !_wcsnicmp(leaf,L"nvapi",5)
        || !_wcsicmp(leaf,L"dxgi.dll") || !_wcsicmp(leaf,L"d3d12.dll")
        || !_wcsicmp(leaf,L"d3d12core.dll")) return false;
    for (const char* name : {"slInit","slOnPluginStartup","NVSDK_NGX_D3D12_CreateFeature",
            "NVSDK_NGX_D3D12_EvaluateFeature","ReShadeRegisterAddon","MfgUnlockSingleModuleQuery"})
        if (GetProcAddress(module,name)) return false;
    return true;
}
bool SystemExport(void* entry,const wchar_t* leaf,const char* symbol) noexcept {
    const HMODULE module=GetModuleHandleW(leaf);
    wchar_t actual[MAX_PATH]{},expected[MAX_PATH]{};
    const DWORD count=module?GetModuleFileNameW(module,actual,_countof(actual)):0;
    return count && count<_countof(actual) && GetSystemDirectoryW(expected,_countof(expected))
        && !wcscat_s(expected,L"\\") && !wcscat_s(expected,leaf)
        && !_wcsicmp(actual,expected)
        && reinterpret_cast<void*>(GetProcAddress(module,symbol))==entry;
}
FARPROC FactoryReplacement(const char* name,void* value) noexcept {
    if (!value || !FactoryName(name)) return nullptr;
    HMODULE owner=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(value),&owner)) return nullptr;
    FARPROC replacement=nullptr;
    if (slots::ImageEntry(owner,value) && reinterpret_cast<void*>(GetProcAddress(owner,name))==value) {
        const auto result=single_overlay::ResolveProc(owner,name,reinterpret_cast<FARPROC>(value));
        if (result!=reinterpret_cast<FARPROC>(value)) replacement=result;
    }
    FreeLibrary(owner);
    return replacement;
}
bool Inspect(HMODULE module,size_t index,slots::Batch& batch,bool& graphics) noexcept {
    return slots::VisitImports(module,[&](const char* name,void** slot) {
        void* value=protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot));
        if (!strcmp(name,"D3D12CreateDevice") && SystemExport(value,L"d3d12.dll",name)) graphics=true;
        if (FactoryName(name)) {
            if (const auto replacement=FactoryReplacement(name,value)) {
                graphics=true;
                return batch.Add(module,slot,value,reinterpret_cast<void*>(replacement));
            }
        } else if (!strcmp(name,"GetProcAddress") && SystemExport(value,L"kernel32.dll",name)) {
            const auto previous=bindings[index].original.load(std::memory_order_acquire);
            if (previous && previous!=reinterpret_cast<Resolver>(value)) return false;
            bindings[index].original.store(reinterpret_cast<Resolver>(value),std::memory_order_release);
            return batch.Add(module,slot,value,reinterpret_cast<void*>(resolvers[index]));
        }
        return true;
    });
}
bool SafeInspect(HMODULE module,size_t index,slots::Batch& batch,bool& graphics) noexcept {
    __try { return Inspect(module,index,batch,graphics); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// A declared slInit import whose bound slot holds the interposer's own entry.
bool ImportsInit(HMODULE module,void* entry,bool& imported) noexcept {
    return slots::VisitImports(module,[&](const char* name,void** slot) {
        if (!strcmp(name,"slInit")
            && protected_pointer::ReadPointer(reinterpret_cast<uintptr_t>(slot))==entry) imported=true;
        return true;
    });
}
bool SafeImportsInit(HMODULE module,void* entry,bool& imported) noexcept {
    __try { return ImportsInit(module,entry,imported); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Log(const wchar_t* result,const wchar_t* path,const slots::Batch& batch) noexcept {
    std::unique_ptr<wchar_t[]> line(new(std::nothrow) wchar_t[33000]);
    if (!line) return;
    swprintf_s(line.get(),33000,L"MFG_PROXY_UI renderer-imports result=%s imports=%zu published=%zu rollbackVerified=%d path=%s",
        result,batch.count,batch.published,batch.rollbackVerified,path);
    single_module::Log(line.get());
}

// Enumerate only declared, already loaded dependencies; never enumerate the
// process or load a module from DllMain. A bounded breadth-first walk covers
// renderer DLLs called through a statically imported game/engine entry.
bool Dependencies(HMODULE module,std::array<HMODULE,64>& queue,size_t& count,ModulePaths& paths) noexcept {
    __try {
        const auto* base=reinterpret_cast<const unsigned char*>(module);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const size_t size=nt->OptionalHeader.SizeOfImage;
        const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return true;
        if (dir.VirtualAddress>=size || dir.Size>size-dir.VirtualAddress || dir.Size>1024*sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
        const auto* imports=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
        for(size_t i=0;i<dir.Size/sizeof(*imports);++i) {
            const auto rva=imports[i].Name;
            if (!rva) return true;
            if (rva>=size || !memchr(base+rva,0,(std::min)(size_t(512),size-rva))) return false;
            const auto child=GetModuleHandleA(reinterpret_cast<const char*>(base+rva));
            if (!child) continue;
            if (!LocalApplicationDll(child,paths)) continue;
            bool seen=false;
            for(size_t j=0;j<count;++j) seen|=queue[j]==child;
            if (!seen && count<queue.size()) queue[count++]=child;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}
}

void ArmModule(HMODULE module) noexcept {
#if !MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    if (gInsideOverlay || !single_module::OwnsBackend()) return;
    Reference retained(module);
    if (!retained.module) return;
    // Nonblocking even when startup and the existing inventory worker meet.
    if (publishing.test_and_set(std::memory_order_acquire)) return;
    struct Unlock { ~Unlock(){publishing.clear(std::memory_order_release);} } unlock;
    for(const auto& binding:bindings) if(binding.importer.load(std::memory_order_acquire)==module && binding.complete) return;
    std::unique_ptr<ModulePaths> paths(new(std::nothrow) ModulePaths);
    if (!paths || !LocalApplicationDll(module,*paths)) return;
    size_t index=bindings.size();
    for(size_t i=0;i<bindings.size();++i) {
        const auto importer=bindings[i].importer.load(std::memory_order_acquire);
        if (importer==module) { index=i; break; }
        if (!importer && index==bindings.size()) index=i;
    }
    if (index==bindings.size()) return;
    slots::Batch batch;
    bool graphics=false;
    const bool inspected=SafeInspect(module,index,batch,graphics);
    if (!graphics) return; // A generic plugin's resolver is not eligible.
    if (!inspected) { Log(L"inspection-rejected",paths->module,batch); return; }
    if (!batch.count) return;
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) return;
    bindings[index].importer.store(module,std::memory_order_release);
    // Failed slots retain their original target; this binding is never recycled.
    bindings[index].complete=batch.Publish();
    Log(bindings[index].complete?L"armed":L"publication-rejected",paths->module,batch);
#endif
}
HMODULE StreamlineHost(const void* caller,HMODULE interposer) noexcept {
#if MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    (void)caller;(void)interposer;
    return nullptr;
#else
    if (!caller || !interposer || !single_module::OwnsBackend()) return nullptr;
    void* const entry=reinterpret_cast<void*>(GetProcAddress(interposer,"slInit"));
    if (!entry || !slots::ImageEntry(interposer,entry)) return nullptr;
    HMODULE candidate=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(caller),&candidate)) return nullptr;
    Reference retained(candidate);
    if (!retained.module || retained.module==interposer) return nullptr;
    std::unique_ptr<ModulePaths> paths(new(std::nothrow) ModulePaths);
    if (!paths || !LocalApplicationDll(retained.module,*paths)) return nullptr;
    bool imported=false;
    if (!SafeImportsInit(retained.module,entry,imported) || !imported) return nullptr;
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(retained.module),&pinned) || pinned!=retained.module) return nullptr;
    return pinned;
#endif
}
void ArmStartupDependencies() noexcept {
#if !MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
    if (!single_module::OwnsBackend()) return;
    std::unique_ptr<ModulePaths> paths(new(std::nothrow) ModulePaths);
    if (!paths) return;
    std::array<HMODULE,64> queue{};
    size_t count=1;
    queue[0]=GetModuleHandleW(nullptr);
    for(size_t i=0;i<count;++i) {
        Reference retained(queue[i]);
        if (!retained.module) continue;
        if (i && !LocalApplicationDll(retained.module,*paths)) continue;
        if (i) ArmModule(retained.module);
        Dependencies(retained.module,queue,count,*paths);
    }
#endif
}
}
