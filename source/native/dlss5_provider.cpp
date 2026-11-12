#include "dlss5_provider.h"
#include "protected_pointer.h"
#include <atomic>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <mutex>
namespace dlss5 {
namespace {
using PcToHeaderFn = PVOID (NTAPI*)(PVOID, PVOID*);
PcToHeaderFn gPcToHeader = nullptr;
using HandleExAFn=BOOL(WINAPI*)(DWORD,LPCSTR,HMODULE*);
using HandleExWFn=BOOL(WINAPI*)(DWORD,LPCWSTR,HMODULE*);
HandleExAFn gHandleExA=nullptr;HandleExWFn gHandleExW=nullptr;
std::mutex gImports;
HMODULE gSelf = nullptr;
thread_local HMODULE gDirectRuntime = nullptr;
struct Scope {
    HMODULE previous;
    explicit Scope(HMODULE runtime) : previous(gDirectRuntime) { gDirectRuntime = runtime; }
    ~Scope() { gDirectRuntime = previous; }
};
PVOID NTAPI DirectCallerHeader(PVOID address, PVOID* base) {
    PVOID actual = nullptr;
    PVOID result = gPcToHeader(address, &actual);
    // Only a synchronous call made by our module through DirectProvider is
    // represented as its exact retained NGX core. All other callers pass through.
    if (gDirectRuntime && actual == gSelf) actual = gDirectRuntime;
    if (base) *base = actual;
    return result ? actual : result;
}
BOOL WINAPI DirectHandleExA(DWORD flags,LPCSTR name,HMODULE* module) {
    const BOOL result=gHandleExA(flags,name,module);
    if(result && module && gDirectRuntime && *module==gSelf
        && flags==(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))*module=gDirectRuntime;
    return result;
}
BOOL WINAPI DirectHandleExW(DWORD flags,LPCWSTR name,HMODULE* module) {
    const BOOL result=gHandleExW(flags,name,module);
    if(result && module && gDirectRuntime && *module==gSelf
        && flags==(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))*module=gDirectRuntime;
    return result;
}
bool ExecutableOwner(void* function, HMODULE module) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return function && VirtualQuery(function, &info, sizeof(info)) == sizeof(info)
        && info.AllocationBase == module && info.State == MEM_COMMIT
        && !(info.Protect & PAGE_GUARD)
        && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
bool SameFile(const wchar_t* left,const wchar_t* right) noexcept {
    const DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
    HANDLE a=CreateFileW(left,FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE b=CreateFileW(right,FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,0,nullptr);
    BY_HANDLE_FILE_INFORMATION x{},y{};
    const bool same=a!=INVALID_HANDLE_VALUE && b!=INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(a,&x) && GetFileInformationByHandle(b,&y)
        && x.dwVolumeSerialNumber==y.dwVolumeSerialNumber && x.nFileIndexHigh==y.nFileIndexHigh && x.nFileIndexLow==y.nFileIndexLow;
    if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);
    return same;
}
// Loaded images only; all PE offsets and strings are bounded before inspection.
bool InstallCallerImport(HMODULE module,const char* symbol,void* expected,void* replacement,bool optional) noexcept {
    __try {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const size_t size = nt->OptionalHeader.SizeOfImage;
        const auto fits = [=](size_t at, size_t bytes) { return at < size && bytes <= size - at; };
        // DLSSNR feature identity comes from its resource contract, not filename/version.
        bool color = false, output = false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const size_t sectionOffset = reinterpret_cast<const unsigned char*>(sections) - base;
        if (nt->FileHeader.NumberOfSections > 96 || !fits(sectionOffset, nt->FileHeader.NumberOfSections * sizeof(*sections))) return false;
        for (unsigned i=0; i<nt->FileHeader.NumberOfSections; ++i) {
            const auto& section=sections[i];
            if (!(section.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)
                || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                || !fits(section.VirtualAddress, section.Misc.VirtualSize)) continue;
            for (size_t at=section.VirtualAddress, end=at+section.Misc.VirtualSize; at+15<end; ++at) {
                if (!memcmp(base+at, "DLSSNR.Color", 13)) color=true;
                if (!memcmp(base+at, "DLSSNR.Output", 14)) output=true;
                if (color && output) break;
            }
        }
        if (!color || !output) return false;
        const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !fits(dir.VirtualAddress,dir.Size) || dir.Size > 4096*sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
        void** found = nullptr;
        const auto* imports=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
        for(size_t i=0; i<dir.Size/sizeof(*imports) && imports[i].Name; ++i) {
            const auto& imp=imports[i];
            if (!imp.FirstThunk || !imp.OriginalFirstThunk) continue;
            for (size_t j=0; j<16384; ++j) {
                const size_t nameAt=size_t(imp.OriginalFirstThunk)+j*8, slotAt=size_t(imp.FirstThunk)+j*8;
                if (!fits(nameAt,8) || !fits(slotAt,8)) return false;
                const auto value=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+nameAt)->u1.AddressOfData;
                if (!value) break;
                if (IMAGE_SNAP_BY_ORDINAL64(value) || !fits(size_t(value),2+strlen(symbol)+1)) continue;
                if (memcmp(base+value+2,symbol,strlen(symbol)+1)) continue;
                auto** slot=reinterpret_cast<void**>(base+slotAt);
                bool data=false;
                for(unsigned s=0;s<nt->FileHeader.NumberOfSections;++s) {
                    const auto& sec=sections[s];
                    if(slotAt>=sec.VirtualAddress && sec.Misc.VirtualSize>=8 && slotAt-sec.VirtualAddress<=sec.Misc.VirtualSize-8)
                        data=!(sec.Characteristics & (IMAGE_SCN_CNT_CODE|IMAGE_SCN_MEM_EXECUTE));
                }
                if (!data || found) return false;
                found=slot;
            }
        }
        if (!found)return optional;
        if(reinterpret_cast<uintptr_t>(found)&7)return false;
        if (*found == replacement) return true;
        if (*found != expected) return false;
        const auto result=protected_pointer::ReplaceProtectedPointer(reinterpret_cast<uintptr_t>(found),
            reinterpret_cast<uintptr_t>(expected),reinterpret_cast<uintptr_t>(replacement));
        return result.disposition==protected_pointer::PublishDisposition::ePublishedRestored;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
NVSDK_NGX_Result FaultResult() noexcept { return static_cast<NVSDK_NGX_Result>(kProviderFault); }
// Keep SEH in a helper without C++ destructors and retain a real return address.
template <class Fn, class... Args>
__declspec(noinline) NVSDK_NGX_Result CallProvider(Fn fn, Args... args) noexcept {
    __try { volatile auto result=fn(args...); return result; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return FaultResult(); }
}
// One graphics API's entries: the runtime's parameter allocation pair and the
// provider's Init, Create, Evaluate and Release.
struct Entries { const char *allocate, *destroy, *init, *create, *evaluate, *release; };
// The admission of DirectProvider::Open and VulkanDirectProvider::Open: the
// provider's members (held*) are set once, and only when every check passed.
template <class Init, class Create, class Evaluate>
bool AdmitProvider(const wchar_t* path, HMODULE runtime, const Entries& names, HMODULE& heldRuntime, HMODULE& heldModule,
    Init& heldInit, Create& heldCreate, Evaluate& heldEvaluate, ReleaseFn& heldRelease) noexcept {
    std::lock_guard lock(gImports);
    if (heldModule) return heldRuntime==runtime && heldInit && heldCreate && heldEvaluate && heldRelease;
    if (!path || !runtime || !GetProcAddress(runtime,names.allocate)
        || !GetProcAddress(runtime,names.destroy)) return false;
    // Absolute paths prevent DLL search-path substitution.
    if (!(path[0] && path[1]==L':' && (path[2]==L'\\' || path[2]==L'/'))) return false;
    if (!gPcToHeader) gPcToHeader=reinterpret_cast<PcToHeaderFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"RtlPcToFileHeader"));
    if(!gHandleExA)gHandleExA=reinterpret_cast<HandleExAFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetModuleHandleExA"));
    if(!gHandleExW)gHandleExW=reinterpret_cast<HandleExWFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetModuleHandleExW"));
    if (!gPcToHeader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&DirectCallerHeader), &gSelf)) return false;
    HMODULE loaded=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loaded) return false;
    wchar_t actual[32768]{};
    const DWORD actualLength=GetModuleFileNameW(loaded,actual,32768);
    if(!actualLength || actualLength>=32768 || !SameFile(actual,path)){FreeLibrary(loaded);return false;}
    const auto init=reinterpret_cast<Init>(GetProcAddress(loaded,names.init));
    const auto create=reinterpret_cast<Create>(GetProcAddress(loaded,names.create));
    const auto evaluate=reinterpret_cast<Evaluate>(GetProcAddress(loaded,names.evaluate));
    const auto release=reinterpret_cast<ReleaseFn>(GetProcAddress(loaded,names.release));
    if (!ExecutableOwner(reinterpret_cast<void*>(init),loaded) || !ExecutableOwner(reinterpret_cast<void*>(create),loaded)
        || !ExecutableOwner(reinterpret_cast<void*>(evaluate),loaded) || !ExecutableOwner(reinterpret_cast<void*>(release),loaded)
        || !GetProcAddress(loaded,"NVSDK_NGX_GetSnippetVersion")
        || !InstallCallerImport(loaded,"RtlPcToFileHeader",reinterpret_cast<void*>(gPcToHeader),reinterpret_cast<void*>(&DirectCallerHeader),false)
        || !InstallCallerImport(loaded,"GetModuleHandleExA",reinterpret_cast<void*>(gHandleExA),reinterpret_cast<void*>(&DirectHandleExA),true)
        || !InstallCallerImport(loaded,"GetModuleHandleExW",reinterpret_cast<void*>(gHandleExW),reinterpret_cast<void*>(&DirectHandleExW),true)) {
        // An indeterminate import publication may retain our hook: keep the image
        // and our pinned module alive even when admission failed.
        return false;
    }
    HMODULE retained=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(runtime),&retained) || retained!=runtime) return false;
    heldRuntime=runtime; heldModule=loaded; heldInit=init; heldCreate=create; heldEvaluate=evaluate; heldRelease=release;
    return true;
}
}
bool DirectProvider::Open(const wchar_t* path, HMODULE runtime) noexcept {
    return AdmitProvider(path,runtime,{"NVSDK_NGX_D3D12_AllocateParameters","NVSDK_NGX_D3D12_DestroyParameters",
        "NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature","NVSDK_NGX_D3D12_EvaluateFeature",
        "NVSDK_NGX_D3D12_ReleaseFeature"},runtime_,module_,init_,create_,evaluate_,release_);
}
NVSDK_NGX_Result DirectProvider::Initialize(ID3D12Device* device,NVSDK_NGX_Parameter* params) noexcept {
    if (!module_ || !device || faulted_ || (device_ && device_!=device)) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (initialized_) return NVSDK_NGX_Result_Success;
    Scope scope(runtime_);
    const auto result=CallProvider(init_,0x24480451ull,L"",device,NVSDK_NGX_Version_API,params);
    faulted_=static_cast<uint32_t>(result)==kProviderFault;
    if (result==NVSDK_NGX_Result_Success) { initialized_=true; device_=device; device_->AddRef(); }
    return result;
}
NVSDK_NGX_Result DirectProvider::Create(ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out) noexcept {
    if (!initialized_ || faulted_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(create_,cmd,static_cast<NVSDK_NGX_Feature>(kFeature),params,out);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
NVSDK_NGX_Result DirectProvider::Evaluate(ID3D12GraphicsCommandList* cmd,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params) noexcept {
    if (!initialized_ || faulted_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(evaluate_,cmd,handle,params,nullptr);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
NVSDK_NGX_Result DirectProvider::Release(NVSDK_NGX_Handle* handle) noexcept {
    if (!initialized_ || faulted_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(release_,handle);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
// The Vulkan entries, admitted exactly as DirectProvider::Open admits the D3D12 ones.
bool VulkanDirectProvider::Open(const wchar_t* path, HMODULE runtime) noexcept {
    return AdmitProvider(path,runtime,{"NVSDK_NGX_VULKAN_AllocateParameters","NVSDK_NGX_VULKAN_DestroyParameters",
        "NVSDK_NGX_VULKAN_Init_Ext2","NVSDK_NGX_VULKAN_CreateFeature1","NVSDK_NGX_VULKAN_EvaluateFeature",
        "NVSDK_NGX_VULKAN_ReleaseFeature"},runtime_,module_,init_,create_,evaluate_,release_);
}
NVSDK_NGX_Result VulkanDirectProvider::Initialize(void* instance,void* physical,void* device,void* gipa,void* gdpa,
    NVSDK_NGX_Parameter* params) noexcept {
    if (!module_ || !instance || !physical || !device || faulted_ || (device_ && device_!=device)) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (initialized_) return NVSDK_NGX_Result_Success;
    Scope scope(runtime_);
    const auto result=CallProvider(init_,0x24480451ull,L"",instance,physical,device,gipa,gdpa,NVSDK_NGX_Version_API,params);
    faulted_=static_cast<uint32_t>(result)==kProviderFault;
    if (result==NVSDK_NGX_Result_Success) { initialized_=true; device_=device; }
    return result;
}
NVSDK_NGX_Result VulkanDirectProvider::Create(void* device,void* cmd,const NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** out) noexcept {
    if (!initialized_ || faulted_ || device!=device_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(create_,device,cmd,static_cast<NVSDK_NGX_Feature>(kFeature),params,out);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
NVSDK_NGX_Result VulkanDirectProvider::Evaluate(void* cmd,const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params) noexcept {
    if (!initialized_ || faulted_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(evaluate_,cmd,handle,params,nullptr);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
NVSDK_NGX_Result VulkanDirectProvider::Release(NVSDK_NGX_Handle* handle) noexcept {
    if (!initialized_ || faulted_) return NVSDK_NGX_Result_FAIL_NotInitialized;
    Scope scope(runtime_); const auto result=CallProvider(release_,handle);
    faulted_=static_cast<uint32_t>(result)==kProviderFault; return result;
}
}
