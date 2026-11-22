#include "caller_scoped_import.h"
#include "overlay_slots.h"
#include "process_mitigations.h"
#include "single_module.h"
#include "protected_pointer.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>

namespace caller_scoped_import {
namespace {
struct Entry {
    void* target = nullptr;
    void* replacement = nullptr;
    alignas(8) std::atomic<void*> active{nullptr};
    void* relay = nullptr;
    void* trampoline = nullptr;
    bool installed = false;
    std::array<unsigned char,5> published{};
};
std::array<Entry, 15> entries; // Resolver, five input APIs, and three factories each for DXGI, ReShade and Streamline.
std::mutex mutex;
// Second caller range read by a relay at each call. Empty (begin above end)
// until published; every partially published state is empty or exact.
struct HostScope {
    alignas(8) std::atomic<uintptr_t> begin{UINTPTR_MAX};
    alignas(8) std::atomic<uintptr_t> end{0};
};
HostScope interposerHost; // Shared by the three interposer factory relays.
HostScope noHost;         // Never published: main-executable callers only.

const wchar_t* OwnerName(const char* name) noexcept {
    if (!name) return nullptr;
    if (!strcmp(name,"GetProcAddress")) return L"kernel32.dll";
    for (const char* factory : {"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"})
        if (!strcmp(name,factory)) return L"dxgi.dll";
    for (const char* input : {"GetAsyncKeyState","GetKeyState","GetKeyboardState","SetCursorPos","ClipCursor"})
        if (!strcmp(name,input)) return L"user32.dll";
    return nullptr;
}
bool ModuleRange(HMODULE image, uintptr_t& begin, uintptr_t& end) noexcept {
    if (!image) return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(image)+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    begin=reinterpret_cast<uintptr_t>(image);
    const auto size=nt->OptionalHeader.SizeOfImage;
    if (!size || begin>UINTPTR_MAX-size) return false;
    end=begin+size;
    return true;
}
bool ImageRange(HMODULE image, uintptr_t& begin, uintptr_t& end) noexcept {
    return image && image==GetModuleHandleW(nullptr) && ModuleRange(image,begin,end);
}
// Some loaders make an entire .idata page executable after startup. We never
// write these slots, so that page flag need not veto a public-entry fallback.
// Require a declared non-code PE data section and the exact named import first;
// arbitrary executable storage must not become eligible.
bool DeclaredDataImport(HMODULE image, void** slot, const char* name, size_t size) noexcept {
    if (!name) return false;
    const auto* base=reinterpret_cast<const unsigned char*>(image);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto rva=reinterpret_cast<uintptr_t>(slot)-reinterpret_cast<uintptr_t>(image);
    auto valid=[&](size_t at,size_t bytes){return at<size&&bytes<=size-at;};
    const size_t sectionOffset=size_t(dos->e_lfanew)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt->FileHeader.SizeOfOptionalHeader;
    if (!nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections>96
        || !valid(sectionOffset,size_t(nt->FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER))) return false;
    bool dataSection=false;
    const auto* sections=reinterpret_cast<const IMAGE_SECTION_HEADER*>(base+sectionOffset);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        if (rva<section.VirtualAddress || section.Misc.VirtualSize<sizeof(void*)
            || rva-section.VirtualAddress>section.Misc.VirtualSize-sizeof(void*)) continue;
        if (section.Characteristics&(IMAGE_SCN_CNT_CODE|IMAGE_SCN_MEM_EXECUTE)) return false;
        dataSection=(section.Characteristics&(IMAGE_SCN_CNT_INITIALIZED_DATA|IMAGE_SCN_CNT_UNINITIALIZED_DATA))!=0;
        break;
    }
    if (!dataSection) return false;
    const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !valid(dir.VirtualAddress,dir.Size)
        || dir.Size>1024*sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    const auto* imports=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for(size_t i=0;i<dir.Size/sizeof(*imports);++i) {
        const auto& imp=imports[i];
        if (!imp.Name) break;
        if (!imp.OriginalFirstThunk || !imp.FirstThunk || rva<imp.FirstThunk
            || (rva-imp.FirstThunk)%sizeof(void*)) continue;
        const auto index=(rva-imp.FirstThunk)/sizeof(void*);
        if (index>=4096) continue;
        for(size_t j=0;j<=index;++j) {
            const size_t lookup=size_t(imp.OriginalFirstThunk)+j*sizeof(IMAGE_THUNK_DATA64);
            if (!valid(lookup,sizeof(IMAGE_THUNK_DATA64))) return false;
            const auto value=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+lookup)->u1.AddressOfData;
            if (!value) break;
            if (j!=index) continue;
            if (IMAGE_SNAP_BY_ORDINAL64(value) || !valid(size_t(value),3)) return false;
            const auto* symbol=reinterpret_cast<const char*>(base+value+2);
            const size_t available=size-size_t(value)-2;
            if (!memchr(symbol,0,available<4096?available:4096)) return false;
            return !strcmp(symbol,name);
        }
    }
    return false;
}
bool PublicTarget(void* target, const char* name, HMODULE& owner) noexcept {
    const wchar_t* leaf=OwnerName(name);
    if (!leaf || !target
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&owner)) return false;
    wchar_t actual[MAX_PATH]{},system[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(owner,actual,MAX_PATH);
    const DWORD systemLength=GetSystemDirectoryW(system,MAX_PATH);
    if (!length || length>=MAX_PATH || !systemLength || systemLength>=MAX_PATH
        || wcscat_s(system,L"\\") || wcscat_s(system,leaf)
        || reinterpret_cast<void*>(GetProcAddress(owner,name))!=target
        || !single_overlay::slots::ImageEntry(owner,target)) return false;
    if (!_wcsicmp(actual,system)) return true;
    // MSFS 2024 has four-byte-aligned factory imports. Keep those imports
    // intact and relay only the main executable's call to its local ReShade.
    if (wcscmp(leaf,L"dxgi.dll")) return false;
    wchar_t local[MAX_PATH]{};
    const DWORD count=GetModuleFileNameW(nullptr,local,MAX_PATH);
    auto* slash=count && count<MAX_PATH?wcsrchr(local,L'\\'):nullptr;
    return slash && !wcscpy_s(slash+1,MAX_PATH-static_cast<size_t>(slash+1-local),leaf)
        && !_wcsicmp(actual,local) && single_overlay::slots::ReShadeFactoryExports(owner);
}
// A loaded Streamline interposer's own exported factory entry, identified by
// the module that owns the code, its leaf name, the export table and the
// public interposer entries beside it. Its file location is the game's choice.
bool InterposerTarget(void* target, const char* name, HMODULE& owner) noexcept {
    owner=nullptr;
    if (!target || !name || (strcmp(name,"CreateDXGIFactory") && strcmp(name,"CreateDXGIFactory1")
        && strcmp(name,"CreateDXGIFactory2"))) return false;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&owner) || !owner || owner==GetModuleHandleW(nullptr)) return false;
    std::unique_ptr<wchar_t[]> path(new(std::nothrow) wchar_t[32768]);
    const DWORD count=path?GetModuleFileNameW(owner,path.get(),32768):0;
    if (!count || count>=32768) return false;
    const wchar_t* leaf=wcsrchr(path.get(),L'\\'); leaf=leaf?leaf+1:path.get();
    if (_wcsicmp(leaf,L"sl.interposer.dll") || reinterpret_cast<void*>(GetProcAddress(owner,name))!=target) return false;
    for (const char* identity : {"slInit","slSetD3DDevice"}) {
        void* entry=reinterpret_cast<void*>(GetProcAddress(owner,identity));
        HMODULE entryOwner=nullptr;
        if (!entry || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(entry),&entryOwner) || entryOwner!=owner) return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(target,&memory,sizeof(memory))==sizeof(memory)
        && memory.AllocationBase==owner && memory.Type==MEM_IMAGE && memory.State==MEM_COMMIT
        && !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        && (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool Current(const Entry& e) noexcept {
    return e.installed && !memcmp(e.target,e.published.data(),e.published.size());
}
// Tail dispatch preserves RCX/RDX/R8/R9, all stack arguments and the original
// caller return address. RAX/R10 and flags are volatile in the Windows x64 ABI.
// Each relay, its aligned activation pointer and host scope have process lifetime.
bool MakeRelay(Entry& entry, uintptr_t begin, uintptr_t end, const HostScope& host) noexcept {
    auto* code=static_cast<unsigned char*>(entry.relay);
    if (!code) return false;
    size_t at=0;
    auto bytes=[&](std::initializer_list<unsigned char> data){for(auto b:data)code[at++]=b;};
    auto qword=[&](uintptr_t value){memcpy(code+at,&value,8);at+=8;};
    auto bind=[&](size_t displacement){code[displacement]=static_cast<unsigned char>(at-displacement-1);};
    bytes({0xf3,0x0f,0x1e,0xfa,0x48,0x8b,0x04,0x24}); // endbr64; mov rax,[rsp]
    bytes({0x49,0xba});qword(begin);bytes({0x4c,0x39,0xd0,0x72});const auto belowExecutable=at++; // jb host
    bytes({0x49,0xba});qword(end);bytes({0x4c,0x39,0xd0,0x72});const auto insideExecutable=at++;  // jb scoped
    bind(belowExecutable);
    // cmp rax,[r10] against the published host scope; empty fails both ways.
    bytes({0x49,0xba});qword(reinterpret_cast<uintptr_t>(&host.begin));bytes({0x49,0x3b,0x02,0x72});const auto belowHost=at++;
    bytes({0x49,0xba});qword(reinterpret_cast<uintptr_t>(&host.end));bytes({0x49,0x3b,0x02,0x73});const auto aboveHost=at++;
    bind(insideExecutable);
    bytes({0x49,0xba});qword(reinterpret_cast<uintptr_t>(&entry.active));
    bytes({0x49,0x8b,0x02,0x48,0x85,0xc0,0x74});const auto inactive=at++;
    bytes({0xff,0xe0});
    bind(belowHost);bind(aboveHost);bind(inactive);
    bytes({0x48,0xb8});qword(reinterpret_cast<uintptr_t>(entry.trampoline));bytes({0xff,0xe0});
    DWORD previous=0,observed=0;
    return VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous)
        && protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(code),observed,at)
        && observed==PAGE_EXECUTE_READ && FlushInstructionCache(GetCurrentProcess(),code,at);
}
// Caller holds the mutex; the target's owner is pinned and the caller range
// is the main executable plus the given host scope. Returns the failed stage,
// or nullptr when the entry is published, inactive, with its original protection.
const wchar_t* Install(Entry& entry, void* expected, void* replacement, uintptr_t begin, uintptr_t end,
                       const HostScope& host, unsigned& code) noexcept {
    const auto init=MH_Initialize();
    if (init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) { code=init; return L"initialize"; }
    entry.target=expected;
    entry.replacement=replacement;
    // MinHook requires an executable relay at creation time. Prepare a valid
    // inactive relay, then finalize its trampoline while the hook is disabled.
    entry.relay=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!entry.relay) { code=GetLastError(); return L"relay-allocation"; }
    entry.trampoline=expected;
    if (!MakeRelay(entry,begin,end,host)) { code=GetLastError(); return L"relay-preparation"; }
    const auto create=MH_CreateHook(expected,entry.relay,&entry.trampoline);
    if (create!=MH_OK || !entry.trampoline) { code=create; return L"decode"; }
    // Publish the complete pass-through trampoline before enabling the entry.
    DWORD relayProtection=0;
    if (!VirtualProtect(entry.relay,4096,PAGE_READWRITE,&relayProtection)) {
        code=GetLastError(); MH_RemoveHook(expected); return L"relay-finalization";
    }
    if (!MakeRelay(entry,begin,end,host)) { code=GetLastError(); MH_RemoveHook(expected); return L"relay-publication"; }
    DWORD originalProtection=0;
    if (!protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(expected),originalProtection,8)) {
        MH_RemoveHook(expected); return L"protection";
    }
    const auto enabled=MH_EnableHook(expected);
    if (enabled!=MH_OK) { code=enabled; MH_RemoveHook(expected); return L"entry-publication"; }
    memcpy(entry.published.data(),expected,entry.published.size());
    entry.installed=true;
    if (!protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(expected),originalProtection,8)
        || !Current(entry)) return L"verification";
    return nullptr;
}
}

bool Eligible(HMODULE importer, void** slot, void* expected, const char* name) noexcept {
    uintptr_t begin=0,end=0;
    const auto address=reinterpret_cast<uintptr_t>(slot);
    if (!expected || (address%8 && address%8!=4) || !ImageRange(importer,begin,end)
        || address<begin || address>end-sizeof(void*)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(slot,&memory,sizeof(memory))!=sizeof(memory) || memory.AllocationBase!=importer
        || memory.Type!=MEM_IMAGE || memory.State!=MEM_COMMIT
        || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        || memory.RegionSize<sizeof(void*)
        || address-reinterpret_cast<uintptr_t>(memory.BaseAddress)>memory.RegionSize-sizeof(void*)) return false;
    const auto* ownerName=OwnerName(name);
    const bool factory=ownerName && !wcscmp(ownerName,L"dxgi.dll");
    const bool executablePage=(memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
    // Aligned slots in data pages publish through the IAT itself. An aligned
    // slot is eligible only once its page became executable (the Microsoft
    // Store MSFS 2024 input imports turn RWX after startup), never otherwise.
    if (!(address%8) && !executablePage) return false;
    if ((factory || executablePage) && !DeclaredDataImport(importer,slot,name,end-begin)) return false;
    void* current=nullptr;
    memcpy(&current,slot,sizeof(current));
    HMODULE owner=nullptr;
    return current==expected && PublicTarget(expected,name,owner);
}

bool Prepare(HMODULE importer, void** slot, void* expected, void* replacement, const char* name, void*& original) noexcept {
    original=nullptr;
    auto fail=[&](const wchar_t* stage,unsigned code=0){
        wchar_t line[256]{};swprintf_s(line,L"MFG_UNALIGNED_IMPORT unavailable symbol=%hs stage=%s code=%u",name?name:"(null)",stage,code);
        single_module::Log(line);return false;
    };
    if (!replacement || replacement==expected || !Eligible(importer,slot,expected,name)) return fail(L"eligibility");
    if (!HookMitigationsSupported()) return fail(L"mitigation");
    uintptr_t begin=0,end=0;
    if (!ImageRange(importer,begin,end)) return false;
    std::lock_guard lock(mutex);
    Entry* entry=nullptr;
    for (auto& e:entries) {
        if (e.target==expected) {
            if (e.replacement!=replacement || !Current(e)) return false;
            original=e.trampoline;
            return true;
        }
        if (!e.target && !entry) entry=&e;
    }
    if (!entry) return false;
    HMODULE owner=nullptr,pinned=nullptr;
    if (!PublicTarget(expected,name,owner)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(expected),&pinned) || pinned!=owner) return false;
    unsigned code=0;
    if (const auto stage=Install(*entry,expected,replacement,begin,end,noHost,code)) return fail(stage,code);
    if (!Eligible(importer,slot,expected,name)) return fail(L"verification");
    original=entry->trampoline;
    wchar_t line[256]{};
    swprintf_s(line,L"MFG_UNALIGNED_IMPORT prepared symbol=%hs caller=main-executable slotRva=0x%llX slotWrites=0",name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(slot)-begin));
    single_module::Log(line);
    return true;
}
bool PrepareInterposerEntry(void* expected, void* replacement, const char* name, void*& original) noexcept {
    original=nullptr;
    auto fail=[&](const wchar_t* stage,unsigned code=0){
        wchar_t line[256]{};swprintf_s(line,L"MFG_PROXY_UI interposer-entry unavailable symbol=%hs stage=%s code=%u",name?name:"(null)",stage,code);
        single_module::Log(line);return false;
    };
    HMODULE owner=nullptr;
    if (!replacement || replacement==expected || !InterposerTarget(expected,name,owner)) return fail(L"eligibility");
    if (!HookMitigationsSupported()) return fail(L"mitigation");
    uintptr_t begin=0,end=0;
    if (!ImageRange(GetModuleHandleW(nullptr),begin,end)) return fail(L"caller-range");
    std::lock_guard lock(mutex);
    Entry* entry=nullptr;
    for (auto& e:entries) {
        if (e.target==expected) {
            // Repeated slInit calls reuse the retained relay; never stack another.
            if (e.replacement!=replacement || !Current(e)) return false;
            original=e.trampoline;
            return true;
        }
        if (!e.target && !entry) entry=&e;
    }
    if (!entry) return fail(L"capacity");
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(expected),&pinned) || pinned!=owner) return fail(L"pin");
    unsigned code=0;
    if (const auto stage=Install(*entry,expected,replacement,begin,end,interposerHost,code)) return fail(stage,code);
    if (!InterposerTarget(expected,name,owner) || owner!=pinned) return fail(L"verification");
    original=entry->trampoline;
    wchar_t line[256]{};
    swprintf_s(line,L"MFG_PROXY_UI interposer-entry prepared symbol=%hs caller=main-executable entryRva=0x%llX exportWrites=0 nativeTableWrites=0",name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(expected)-reinterpret_cast<uintptr_t>(owner)));
    single_module::Log(line);
    return true;
}
bool SetInterposerHost(HMODULE host) noexcept {
    uintptr_t begin=0,end=0;
    if (!host || host==GetModuleHandleW(nullptr) || !ModuleRange(host,begin,end)) return false;
    std::lock_guard lock(mutex);
    const uintptr_t published=interposerHost.end.load(std::memory_order_acquire);
    if (published) return published==end && interposerHost.begin.load(std::memory_order_acquire)==begin;
    interposerHost.begin.store(begin,std::memory_order_release);
    interposerHost.end.store(end,std::memory_order_release);
    return true;
}
bool Activate(void* expected, void* replacement) noexcept {
    std::lock_guard lock(mutex);
    for(auto& e:entries)if(e.target==expected&&e.replacement==replacement&&Current(e)) {
        e.active.store(replacement,std::memory_order_release);
        return true;
    }
    return false;
}
bool Deactivate(void* expected, void* replacement) noexcept {
    std::lock_guard lock(mutex);
    for (auto& e:entries) if (e.target==expected && e.replacement==replacement) {
        void* active=replacement;
        e.active.compare_exchange_strong(active,nullptr,std::memory_order_acq_rel);
        return e.active.load(std::memory_order_acquire)==nullptr;
    }
    return false;
}
}
