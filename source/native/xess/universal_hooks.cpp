#include "xess_mod.h"
#include "xess_runtime.h"
#include "runtime_layout.h"
#include "xess_api.h"
#include "game_profile.h"
#include "../optiscaler_host.h"
#include "standalone_policy.h"
#include "sdk_pacing.h"
#include "module_list.h"
#include <string>
#include <MinHook.h>
#include <Psapi.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>
#include <algorithm>
#include <type_traits>
#include "third_party/minhook/src/hde/hde64.h"

namespace xess_mod {
Api original;
namespace {
HMODULE selfModule{}, fgOwner{}, llOwner{};
// OptiScaler took libxell's create and destroy entries (immutable after active's publication).
bool hostLatency{};
std::atomic<bool> active{}, prepared{};
std::atomic<bool> selfReady{};
std::mutex discoveryMutex;
bool attempted{};
// The pair the Vulkan host loaded (discoveryMutex), and its calls in progress on this thread.
HMODULE vulkanFg{}, vulkanLl{};
std::atomic<bool> vulkanHost{};
thread_local bool tVulkanHostCall{};
// optisleep.1: Hook_xellSleep's call into libxell in progress on this thread (GatewaySleepScope).
thread_local bool tGatewaySleep{};
// sdkscan.1: discovery runs inside a first Create call on this thread; that pass adds Toolhelp's list only when psapi
// fails, which keeps the call short.
thread_local bool tCreateDiscovery{};
// sdkscan.1: attempted and the Vulkan host's pair, published for the listing read before discoveryMutex.
std::atomic<bool> discoveryDone{}, vulkanPairKnown{};
std::atomic<decltype(&GetProcAddress)> rawGetProc{&GetProcAddress};
FARPROC RawProc(HMODULE module, LPCSTR name) noexcept { return rawGetProc.load(std::memory_order_acquire)(module, name); }
struct CreateSlot { std::atomic<HMODULE> owner{}; std::atomic<FARPROC> target{}; };
std::array<CreateSlot, 8> fgCreateSlots, llCreateSlots;
SRWLOCK slotLock = SRWLOCK_INIT;
bool resolverAttempted{};
bool HostCaller(void* address) noexcept;

template<bool Fg, size_t Index> struct FirstCreate {
    using Result = std::conditional_t<Fg, xefg_swapchain_result_t, xell_result_t>;
    using Handle = std::conditional_t<Fg, xefg_swapchain_handle_t, xell_context_handle_t>;
    using Function = Result(*)(ID3D12Device*, Handle*);
    static __declspec(noinline) Result Call(ID3D12Device* device, Handle* output)
    {
        auto& slot = (Fg ? fgCreateSlots : llCreateSlots)[Index];
        auto native = reinterpret_cast<Function>(slot.target.load(std::memory_order_acquire));
        if (!native) return static_cast<Result>(-1);
        // Discovery/hook activation runs at the API call, never inside resolver
        // callbacks or DLL load notifications. Private programs are published by
        // the backend before it invokes the private provider's first Create.
        tCreateDiscovery = true;
        DiscoverSdkModules();
        tCreateDiscovery = false;
        if (active.load(std::memory_order_acquire) && slot.owner.load(std::memory_order_acquire) == (Fg ? fgOwner : llOwner)) {
            HostCallScope host(HostCaller(_ReturnAddress()));
            if constexpr (Fg) return Hook_xefgSwapChainD3D12CreateContext(device, output);
            else return Hook_xellD3D12CreateContext(device, output);
        }
        return native(device, output);
    }
};
template<bool Fg, size_t... Index> auto CreateGateways(std::index_sequence<Index...>)
{ return std::array<FARPROC, sizeof...(Index)>{reinterpret_cast<FARPROC>(&FirstCreate<Fg, Index>::Call)...}; }
const auto fgCreateGateways = CreateGateways<true>(std::make_index_sequence<8>{});
const auto llCreateGateways = CreateGateways<false>(std::make_index_sequence<8>{});
bool OwnCode(HMODULE module, const void* code) noexcept;
bool PrivateModule(HMODULE module) noexcept;
bool SdkCaller(void* address) noexcept;

FARPROC WINAPI ResolveCreate(HMODULE module, LPCSTR name)
{
    const auto result = RawProc(module, name);
    const DWORD error = GetLastError();
    FARPROC replacement = result;
    if (result && uintptr_t(name) > 0xffff) {
        const bool fg = !strcmp(name, "xefgSwapChainD3D12CreateContext");
        const bool ll = !strcmp(name, "xellD3D12CreateContext");
        // The game's verified SDK pair may resolve another native provider.
        // Preserve these cached internal exports as well as same-module calls;
        // returning our gateway would change the forwarding adapter's owner.
        if ((fg || ll) && !(active.load(std::memory_order_acquire) && SdkCaller(_ReturnAddress()))
            && !OwnCode(module, _ReturnAddress()) && !PrivateModule(module)
            && OwnCode(module, reinterpret_cast<void*>(result))) {
            auto& slots = fg ? fgCreateSlots : llCreateSlots;
            const auto& gateways = fg ? fgCreateGateways : llCreateGateways;
            AcquireSRWLockExclusive(&slotLock);
            for (size_t i=0; i<slots.size(); ++i) {
                auto owner = slots[i].owner.load(std::memory_order_acquire);
                if (!owner) {
                    HMODULE pinned{};
                    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) break;
                    slots[i].owner.store(module, std::memory_order_release);
                    slots[i].target.store(result, std::memory_order_release);
                    owner = module;
                }
                if (owner == module) { replacement = gateways[i]; break; }
            }
            ReleaseSRWLockExclusive(&slotLock);
        }
    }
    // UI gateways stay with RTXMFG's executable-scoped import hooks; this
    // resolver only routes the two XeSS SDK creation entry points.
    SetLastError(error);
    return replacement;
}
void InstallResolver() noexcept
{
    if (resolverAttempted || !selfReady.load(std::memory_order_acquire)) return;
    resolverAttempted = true;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto target = reinterpret_cast<void*>(RawProc(kernel, "GetProcAddress"));
    // The resolved system entry point may belong to kernelbase on modern Windows.
    HMODULE owner{};
    if (!target || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target), &owner) || !OwnCode(owner, target)) return;
    void* trampoline{};
    if (MH_CreateHook(target, reinterpret_cast<void*>(&ResolveCreate), &trampoline) != MH_OK) return;
    rawGetProc.store(reinterpret_cast<decltype(&GetProcAddress)>(trampoline), std::memory_order_release);
    const bool enabled = MH_EnableHook(target) == MH_OK;
    WriteLog("sdk_dynamic_resolver", std::string("\"enabled\":")+(enabled ? "true" : "false"));
}

bool SdkCaller(void* address) noexcept
{
    HMODULE owner{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner) && (owner == fgOwner || owner == llOwner);
}
// Calls from OptiScaler's own code (its frame generation output). The contexts they
// create stay untracked (xess_mod HostCallScope) and later calls on them go straight to the
// provider.
bool HostCaller(void* address) noexcept
{
    if (tVulkanHostCall && vulkanHost.load(std::memory_order_acquire)) return true;
    return optiscaler_host::Detected() && optiscaler_host::IsHostAddress(address);
}
// Every hooked XeFG entry point except creation takes its context first, as does every XeLL one.
template<class First, class... Rest> bool HostOwnedFirst(First first, Rest...) noexcept
{
    if constexpr (std::is_same_v<First, xefg_swapchain_handle_t>) return HostOwnedHandle(first, true);
    else if constexpr (std::is_same_v<First, xell_context_handle_t>) return HostOwnedHandle(first, false);
    else return false;
}
inline bool HostOwnedFirst() noexcept { return false; }
// hosthandle.1: an XeFG call on a context OptiScaler created, whatever module it returns into. XeLL calls keep the
// caller rule (OptiScaler's XeLL wrapper handles and our sleep hooks are not part of this change).
template<class First, class... Rest> bool HostOwnedFgFirst(First first, Rest...) noexcept
{
    if constexpr (std::is_same_v<First, xefg_swapchain_handle_t>) return HostOwnedHandle(first, true);
    else return false;
}
inline bool HostOwnedFgFirst() noexcept { return false; }
// A host context's initialization and destruction still reach the backend, which
// registers and removes its plain pacing owner around the provider call.
template<auto H> constexpr bool kHostLifecycle = false;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainDestroy> = true;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainD3D12InitFromSwapChain> = true;
template<> constexpr bool kHostLifecycle<&Hook_xefgSwapChainD3D12InitFromSwapChainDesc> = true;
// Names the gateway's entry point once per process; defined after `entries`.
void LogFirstCall(void* gateway, void* caller) noexcept;
// hosthandle.1: names the entry point and the module a host context's call came from, once per entry point.
void LogHostHandleCall(void* gateway, void* caller) noexcept;
// All originals exist before hook publication. A partial cohort remains native.
// Native SDK self-calls must not turn its internal contexts into private ones.
template<auto Member, auto Hook, class Signature = decltype(Hook)> struct Gateway;
template<auto Member, auto Hook, class Result, class... Args>
struct Gateway<Member, Hook, Result(*)(Args...)> {
    static __declspec(noinline) Result Call(Args... args)
    {
        // Which SDK entry points a game actually uses (first call each).
        static std::atomic<bool> seen{};
        if (!seen.exchange(true, std::memory_order_relaxed)) LogFirstCall(reinterpret_cast<void*>(&Call), _ReturnAddress());
        if (!prepared.load(std::memory_order_acquire)) return static_cast<Result>(-1);
        if (!active.load(std::memory_order_acquire)) return (original.*Member)(args...);
        if (SdkCaller(_ReturnAddress())) return (original.*Member)(args...);
        const bool hostCaller = HostCaller(_ReturnAddress());
        // hosthandle.1: a context OptiScaler created (hostFg) is the host's whoever calls (standalone_policy.h HostRoutedCall).
        const bool hostHandle = !hostCaller && optiscaler_host::Detected() && HostOwnedFgFirst(args...);
        if (hostHandle)
        {
            static std::atomic<bool> logged{};
            if (!logged.exchange(true, std::memory_order_relaxed)) LogHostHandleCall(reinterpret_cast<void*>(&Call), _ReturnAddress());
        }
        const bool host = xemfg_standalone::HostRoutedCall(hostCaller, hostHandle);
        if constexpr (!kHostLifecycle<Hook>)
            if (host && HostOwnedFirst(args...)) return (original.*Member)(args...);
        HostCallScope scope(host);
        return Hook(args...);
    }
};
struct Entry { const char* name; bool fg; void* hook; void** original; void* target{}; };
std::array entries{
#define FG_ENTRY(name) Entry{#name, true, reinterpret_cast<void*>(&Gateway<&Api::name, &Hook_##name>::Call), reinterpret_cast<void**>(&original.name)},
    XESS_FG_IMPORTS(FG_ENTRY)
#undef FG_ENTRY
#define LL_ENTRY(name) Entry{#name, false, reinterpret_cast<void*>(&Gateway<&Api::name, &Hook_##name>::Call), reinterpret_cast<void**>(&original.name)},
    XESS_LL_IMPORTS(LL_ENTRY)
#undef LL_ENTRY
};
const char* GatewayName(void* gateway) noexcept
{
    for (const auto& entry : entries) if (entry.hook == gateway) return entry.name;
    return "unknown";
}
// The file name (leaf, UTF-8) of the module `caller` lies in, and that module (null when none).
std::string CallerModule(void* caller, HMODULE& owner)
{
    owner = nullptr;
    wchar_t path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(caller), &owner))
        GetModuleFileNameW(owner, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    char module[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, leaf ? leaf+1 : path, -1, module, MAX_PATH, nullptr, nullptr);
    return module;
}
void LogFirstCall(void* gateway, void* caller) noexcept
{
    try {
        HMODULE owner{};
        const std::string module = CallerModule(caller, owner);
        WriteLog("sdk_first_call", std::string("\"api\":\"")+GatewayName(gateway)+"\",\"caller\":\""+module+
            "\",\"sdk_internal\":"+(owner && (owner == fgOwner || owner == llOwner) ? "true" : "false"));
    } catch (...) {}
}
void LogHostHandleCall(void* gateway, void* caller) noexcept
{
    try {
        HMODULE owner{};
        const std::string module = CallerModule(caller, owner);
        WriteLog("host_handle_call", std::string("\"api\":\"")+GatewayName(gateway)+"\",\"caller\":\""+module+"\"");
    } catch (...) {}
}
// These SDK-private context entry points are present in shipping Intel DLLs,
// but absent from the public headers. Native calls keep their full ABI through
// opaque tail forwarding. A tracked/retired private token gets UNSUPPORTED;
// the mod does not forward it across providers or implement private interfaces.
#define INTERNAL_API_LIST(X) \
    X(0, true, xefgSwapChainD3D12GetProfilingData) \
    X(1, true, xefgSwapChainD3D12SetDiagnosticCallbacks) \
    X(2, true, xefgSwapChainGetParameterP) \
    X(3, true, xefgSwapChainSetParameterP) \
    X(4, false, xellD3D12SetAppQueue) \
    X(5, false, xellGetContextParameterP) \
    X(6, false, xellGetLastPresentStartFrameId) \
    X(7, false, xellQueryInterface) \
    X(8, false, xellSetContextParameterP) \
    X(9, false, xellSetDisplayInfo) \
    X(10, false, xellSetFgEnabled) \
    X(11, false, xellSetGeneratedFramesCount)
#define DECLARE_GUARD(index, fg, name) extern "C" void XessInternalGuard##index();
INTERNAL_API_LIST(DECLARE_GUARD)
#undef DECLARE_GUARD
struct InternalEntry { const char* name; bool fg; void* hook; void* target{}; void* original{}; };
std::array internalEntries{
#define INTERNAL_ENTRY(index, fg, name) InternalEntry{#name, fg, reinterpret_cast<void*>(&XessInternalGuard##index)},
    INTERNAL_API_LIST(INTERNAL_ENTRY)
#undef INTERNAL_ENTRY
};
extern "C" int XessUnsupportedInternalApi() noexcept { return -10; }
extern "C" void* XessInternalApiTarget(unsigned index, void* handle, void* caller) noexcept
{
    if (index >= internalEntries.size()) return reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    const bool published = prepared.load(std::memory_order_acquire);
    auto& entry=internalEntries[index];
    if (!published || !entry.original) return reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    const DWORD error=GetLastError();
    void* target=entry.original;
    if (active.load(std::memory_order_acquire) && !SdkCaller(caller) && PrivateApiHandle(entry.fg,handle))
        target=reinterpret_cast<void*>(&XessUnsupportedInternalApi);
    SetLastError(error);
    return target;
}
bool OwnCode(HMODULE module, const void* code) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!code || VirtualQuery(code, &info, sizeof(info)) != sizeof(info)) return false;
    const DWORD access = info.Protect & 0xff;
    return info.AllocationBase == module && info.Type == MEM_IMAGE && info.State == MEM_COMMIT
        && !(info.Protect & PAGE_GUARD) && (access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ
            || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY);
}
// An entry OptiScaler redirected into itself. Upstream master 9171bf45 detours its own libxell's exports
// to its XeLL input: a 5-byte jump to a relay (jmp [rip+disp]) whose target is OptiScaler's image. Followed only
// while OptiScaler is identified; any other jump at an entry stays foreign.
bool HostRedirectedEntry(const void* entry) noexcept
{
    if (!entry || !optiscaler_host::Detected()) return false;
    const auto* code = static_cast<const unsigned char*>(entry);
    for (unsigned hop = 0; hop < 4; ++hop) {
        unsigned char bytes[6]{};
        SIZE_T copied{};
        if (!ReadProcessMemory(GetCurrentProcess(), code, bytes, sizeof(bytes), &copied) || copied != sizeof(bytes))
            return false;
        int32_t displacement{};
        if (bytes[0] == 0xe9) {
            memcpy(&displacement, bytes + 1, sizeof(displacement));
            code += 5 + displacement;
        } else if (hop && bytes[0] == 0xff && bytes[1] == 0x25) {
            memcpy(&displacement, bytes + 2, sizeof(displacement));
            uintptr_t target{};
            if (!ReadProcessMemory(GetCurrentProcess(), code + 6 + displacement, &target, sizeof(target), &copied)
                || copied != sizeof(target)) return false;
            code = reinterpret_cast<const unsigned char*>(target);
        } else return false;
        if (optiscaler_host::IsHostAddress(code)) return true;
    }
    return false;
}
bool Decode(HMODULE module, const unsigned char* code, hde64s& instruction, std::array<unsigned char,16>& bytes) noexcept
{
    if (!OwnCode(module,code)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(code,&memory,sizeof(memory))!=sizeof(memory)) return false;
    const auto available=(std::min)(size_t(15),uintptr_t(memory.BaseAddress)+memory.RegionSize-uintptr_t(code));
    SIZE_T copied{}; bytes={};
    if (!ReadProcessMemory(GetCurrentProcess(),code,bytes.data(),available,&copied) || copied!=available) return false;
    hde64_disasm(bytes.data(),&instruction);
    return instruction.len && instruction.len<=available && !(instruction.flags & F_ERROR);
}
bool PrivateContextAbi(HMODULE module, const void* entry) noexcept
{
    // Private APIs have no published signature. Admit only an owned, decoded
    // wrapper proving RCX is the context and its null case returns SDK -8 in
    // EAX. This establishes the bounded UNSUPPORTED guard's input/result ABI;
    // no layout, argument count or implementation RVA is transferred.
    const auto* code=static_cast<const unsigned char*>(entry);
    for (unsigned hop=0; hop<4; ++hop) {
        hde64s instruction{}; std::array<unsigned char,16> bytes{};
        if (!Decode(module,code,instruction,bytes)) return false;
        if (instruction.opcode==0xe9 && instruction.len==5) code+=5+int32_t(instruction.imm.imm32);
        else if (instruction.opcode==0xeb && instruction.len==2) code+=2+int8_t(instruction.imm.imm8);
        else break;
    }
    for (unsigned count=0; count<32; ++count) {
        hde64s instruction{}; std::array<unsigned char,16> bytes{};
        if (!Decode(module,code,instruction,bytes)) return false;
        if (instruction.len==3 && bytes[0]==0x48 && bytes[1]==0x85 && bytes[2]==0xc9) {
            code+=3;
            if (!Decode(module,code,instruction,bytes)) return false;
            const unsigned char* nullCase{};
            if ((instruction.opcode==0x75 && instruction.len==2) || (instruction.opcode==0x0f && instruction.opcode2==0x85))
                nullCase=code+instruction.len;
            else if (instruction.opcode==0x74 && instruction.len==2) nullCase=code+2+int8_t(instruction.imm.imm8);
            else if (instruction.opcode==0x0f && instruction.opcode2==0x84) nullCase=code+instruction.len+int32_t(instruction.imm.imm32);
            if (!nullCase || !Decode(module,nullCase,instruction,bytes)) return false;
            return (instruction.len==5 && bytes[0]==0xb8 && instruction.imm.imm32==0xfffffff8u)
                || (instruction.len==3 && bytes[0]==0x8d && bytes[1]==0x41 && bytes[2]==0xf8);
        }
        // Only a prologue that preserves incoming RCX may precede the check.
        // In particular, a moved second argument must not masquerade as context.
        const unsigned reg=instruction.modrm_reg+(instruction.rex_r?8:0);
        const unsigned rm=instruction.modrm_rm+(instruction.rex_b?8:0);
        const bool push=instruction.opcode>=0x50 && instruction.opcode<=0x57;
        const bool store=instruction.opcode==0x89 && (instruction.modrm_mod!=3 || rm!=1);
        const bool load=(instruction.opcode==0x8b || instruction.opcode==0x8d
            || (instruction.opcode==0x0f && (instruction.opcode2==0xb6 || instruction.opcode2==0xb7
                || instruction.opcode2==0xbe || instruction.opcode2==0xbf))) && reg!=1;
        const bool immediate=instruction.opcode>=0xb8 && instruction.opcode<=0xbf
            && (instruction.opcode-0xb8+(instruction.rex_b?8:0))!=1;
        const bool stack=instruction.opcode==0x83 && instruction.modrm_mod==3 && rm==4;
        const bool nop=instruction.opcode==0x90 || (instruction.opcode==0x0f && instruction.opcode2==0x1f);
        if (!push && !store && !load && !immediate && !stack && !nop) return false;
        code+=instruction.len;
    }
    return false;
}
bool PrivateModule(HMODULE module) noexcept
{
    // The mod itself is never the game's SDK.
    return module == selfModule;
}
// rejectlog.1: why discovery did not admit a module that exports the role's version entry, so a missing pair and a pair
// found and rejected read differently in the log. reason nullptr: admitted.
struct CandidateCheck
{
    const char* reason = nullptr;
    bool versioned = false;    // the role's version export exists (only these modules are logged)
    bool hasVersion = false;   // the version call succeeded
    int32_t status = 0;        // the version call's result when it failed
    uint16_t major = 0, minor = 0, patch = 0;
    char entry[64]{};          // the export the reason names, when there is one (identifier characters only)
    void Name(const char* name) noexcept
    {
        size_t i = 0;
        for (; name && name[i] && i+1 < sizeof(entry); ++i)
        {
            const char c = name[i];
            entry[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ? c : '?';
        }
        entry[i] = 0;
    }
};
bool KnownExports(HMODULE module, bool fg, CandidateCheck& check) noexcept
{
    check.reason = "export_table_unreadable";
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
    auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto bytes = size_t(info.SizeOfImage);
    auto fits = [&](size_t rva, size_t n) {
        if (rva >= bytes || n > bytes-rva) return false;
        MEMORY_BASIC_INFORMATION memory{};
        return VirtualQuery(base+rva,&memory,sizeof(memory))==sizeof(memory) && memory.AllocationBase==module
            && memory.Type==MEM_IMAGE && memory.State==MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS|PAGE_GUARD))
            && uintptr_t(base+rva)+n <= uintptr_t(memory.BaseAddress)+memory.RegionSize;
    };
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!fits(0, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || !fits(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!fits(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return false;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+dir.VirtualAddress);
    if (ex->NumberOfNames > 4096 || !fits(ex->AddressOfNames, size_t(ex->NumberOfNames)*4)) return false;
    auto* names = reinterpret_cast<const DWORD*>(base+ex->AddressOfNames);
    const char* prefix = fg ? "xefgSwapChain" : "xell";
    for (DWORD i=0; i<ex->NumberOfNames; ++i) {
        if (!fits(names[i], 1)) return false;
        const char* name = reinterpret_cast<const char*>(base+names[i]);
        const auto limit = (std::min)(size_t(256), bytes-names[i]);
        if (!fits(names[i],limit) || !memchr(name, 0, limit)) return false;
        if (strncmp(name, prefix, strlen(prefix))) continue;
        if (!strcmp(name, fg ? "xefgSwapChainGetVersion" : "xellGetVersion")) continue;
        if (!fg && (!strcmp(name,"xellAILGetDecision") || !strcmp(name,"xellAILGetVersion")
            || !strcmp(name,"xellAILIsSupportedDevice"))) continue;
        bool known{};
        for (const auto& entry : entries) if (entry.fg == fg && !strcmp(name, entry.name)) known = true;
        for (const auto& entry : internalEntries) if (entry.fg == fg && !strcmp(name, entry.name)) known = true;
        if (!known) { check.reason = "unknown_export"; check.Name(name); return false; }
    }
    check.reason = nullptr;
    return true;
}
bool Candidate(HMODULE module, bool fg, CandidateCheck& check) noexcept
{
    check = {};
    const auto version = RawProc(module, fg ? "xefgSwapChainGetVersion" : "xellGetVersion");
    if (!version) { check.reason = "no_version_export"; return false; }
    check.versioned = true;
    if (!OwnCode(module, reinterpret_cast<void*>(version))) { check.reason = "version_outside_code"; return false; }
    if (fg) {
        xefg_swapchain_version_t v{};
        const auto result = reinterpret_cast<decltype(&xefgSwapChainGetVersion)>(version)(&v);
        if (result != 0) { check.reason = "version_call_failed"; check.status = static_cast<int32_t>(result); return false; }
        check.major=v.major; check.minor=v.minor; check.patch=v.patch;
    } else {
        xell_version_t v{};
        const auto result = reinterpret_cast<decltype(&xellGetVersion)>(version)(&v);
        if (result != 0) { check.reason = "version_call_failed"; check.status = static_cast<int32_t>(result); return false; }
        check.major=v.major; check.minor=v.minor; check.patch=v.patch;
    }
    check.hasVersion = true;
    if (check.major != 1) { check.reason = "api_major"; return false; }
    if (!KnownExports(module, fg, check)) return false;
    size_t index{};
    for (const auto& entry : entries) {
        if (entry.fg != fg) continue;
        const auto address = RawProc(module, entry.name);
        if (!address && index < (fg ? 11u : 6u)) { check.reason = "required_export_missing"; check.Name(entry.name); return false; }
        if (address && !OwnCode(module, reinterpret_cast<void*>(address)))
        { check.reason = "export_outside_code"; check.Name(entry.name); return false; }
        ++index;
    }
    for (const auto& entry : internalEntries) {
        if (entry.fg != fg) continue;
        const auto address=RawProc(module,entry.name);
        // OptiScaler's own entry jump hides the wrapper; that entry is its, not ours to guard
        if (address && !HostRedirectedEntry(reinterpret_cast<void*>(address))
            && !PrivateContextAbi(module,reinterpret_cast<void*>(address)))
        { check.reason = "private_entry_layout"; check.Name(entry.name); return false; }
    }
    return true;
}
// rejectlog.1: one sdk_candidate_rejected line per module and role per process, at most 16 (discovery runs every 5 s
// until it admits a pair, and on every first Create). Under discoveryMutex. The path and file hash are
// game_profile.cpp's (LogSdkCandidateRejected), as for native_sdk_module.
// rejectlog.2: not for the identified OptiScaler image, which exports xellGetVersion beside its own XeLL wrapper entries
// and fails the private-entry check, but is never the SDK pair.
std::vector<std::pair<HMODULE, bool>> rejectedLogged;
void LogRejectedCandidate(HMODULE module, bool fg, const CandidateCheck& check) noexcept
{
    if (!check.versioned || !check.reason || optiscaler_host::IsHostModule(module)) return;
    try {
        const std::pair<HMODULE, bool> key{module, fg};
        if (rejectedLogged.size() >= 16 || std::find(rejectedLogged.begin(), rejectedLogged.end(), key) != rejectedLogged.end())
            return;
        rejectedLogged.push_back(key);
        std::string fields = std::string("\"role\":\"")+(fg ? "fg" : "ll")+"\",\"reason\":\""+check.reason+"\"";
        if (check.entry[0]) fields += ",\"export\":\""+std::string(check.entry)+"\"";
        if (check.hasVersion)
            fields += ",\"api_version\":\""+std::to_string(check.major)+"."+std::to_string(check.minor)+"."+
                std::to_string(check.patch)+"\"";
        else if (check.status) fields += ",\"status\":"+std::to_string(check.status);
        LogSdkCandidateRejected(module, fields);
    } catch (...) {}
}
// sdkscan.1: what discovery listed and what it made of every libxess_fg* / libxell* file, for sessions where it never
// saw a candidate. Under discoveryMutex.
const char* SdkFileKind(HMODULE module) noexcept
{
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) return nullptr;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf+1 : path;
    if (!_wcsnicmp(leaf, L"libxess_fg", 10)) return "fg";
    if (!_wcsnicmp(leaf, L"libxell", 7)) return "ll";
    return nullptr;
}
// Whether the module's own export table names `wanted` (1), doesn't (0) or can't be read (-1): a missing version export
// reads differently from a GetProcAddress that returned nothing.
int ExportTableHas(HMODULE module, const char* wanted) noexcept
{
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return -1;
    auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto bytes = size_t(info.SizeOfImage);
    auto fits = [&](size_t rva, size_t n) {
        if (rva >= bytes || n > bytes-rva) return false;
        MEMORY_BASIC_INFORMATION memory{};
        return VirtualQuery(base+rva,&memory,sizeof(memory))==sizeof(memory) && memory.AllocationBase==module
            && memory.Type==MEM_IMAGE && memory.State==MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS|PAGE_GUARD))
            && uintptr_t(base+rva)+n <= uintptr_t(memory.BaseAddress)+memory.RegionSize;
    };
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!fits(0, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
        || !fits(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return -1;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return -1;
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    if (!fits(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return -1;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+dir.VirtualAddress);
    if (ex->NumberOfNames > 65536 || !fits(ex->AddressOfNames, size_t(ex->NumberOfNames)*4)) return -1;
    auto* names = reinterpret_cast<const DWORD*>(base+ex->AddressOfNames);
    const size_t length = strlen(wanted)+1;
    for (DWORD i=0; i<ex->NumberOfNames; ++i)
        if (fits(names[i], length) && !memcmp(base+names[i], wanted, length)) return 1;
    return 0;
}
// One sdk_module_scan line whenever the listing's outcome changes (at most 8 per process): both lists' counts and
// errors, the modules only Toolhelp listed, and how many libxess_fg* / libxell* files the list holds.
void LogModuleScan(const ModuleListStats& stats, size_t modules, size_t named) noexcept
{
    static unsigned lines = 0;
    static DWORD psapiError = MAXDWORD, toolhelpError = MAXDWORD;
    static bool toolhelpOnly = false;
    static size_t sdkNamed = SIZE_MAX;
    const size_t onlyToolhelp = modules > stats.psapi ? modules-stats.psapi : 0;
    if (lines >= 8 || (stats.psapiError == psapiError && stats.toolhelpError == toolhelpError
        && (onlyToolhelp > 0) == toolhelpOnly && named == sdkNamed)) return;
    ++lines;
    psapiError = stats.psapiError;
    toolhelpError = stats.toolhelpError;
    toolhelpOnly = onlyToolhelp > 0;
    sdkNamed = named;
    WriteLog("sdk_module_scan", "\"modules\":"+std::to_string(modules)+",\"psapi\":"+std::to_string(stats.psapi)+
        ",\"psapi_error\":"+std::to_string(stats.psapiError)+",\"toolhelp\":"+std::to_string(stats.toolhelp)+
        ",\"toolhelp_error\":"+std::to_string(stats.toolhelpError)+",\"toolhelp_only\":"+std::to_string(onlyToolhelp)+
        ",\"sdk_named\":"+std::to_string(named));
}
// One sdk_module_seen line per libxess_fg* / libxell* module per process (at most 16). They are written when discovery
// returns, after any activation: the file hash (only for a module no role saw a version export in; the others already
// get one from native_sdk_module or sdk_candidate_rejected) must never delay the hooks behind OptiScaler's first Create.
std::vector<HMODULE> seenLogged;
std::string CandidateOutcome(bool admitted, const CandidateCheck& check)
{
    std::string text = admitted ? "admitted" : check.reason ? check.reason : "rejected";
    if (!admitted && check.entry[0]) text += std::string(":")+check.entry;
    return text;
}
struct SeenModules
{
    struct Line { HMODULE module; std::string fields; bool withHash; };
    std::vector<Line> pending;
    // The checks run here, while the pass still holds its reference; only the write (path, hash) waits.
    // handle: GetModuleHandleExW returned this module (no candidate checks ran otherwise).
    void Add(HMODULE module, const char* kind, bool handle, const CandidateCheck* fg = nullptr, bool isFg = false,
        const CandidateCheck* ll = nullptr, bool isLl = false) noexcept
    {
        try {
            if (seenLogged.size() >= 16 || std::find(seenLogged.begin(), seenLogged.end(), module) != seenLogged.end())
                return;
            seenLogged.push_back(module);
            std::string fields = std::string("\"name_kind\":\"")+kind+"\",\"handle\":"+(handle ? "true" : "false");
            bool unversioned = false;
            if (handle && fg && ll) {
                const bool fgName = !strcmp(kind, "fg");
                const char* version = fgName ? "xefgSwapChainGetVersion" : "xellGetVersion";
                const int table = ExportTableHas(module, version);
                fields += std::string(",\"version_export\":\"")+(table > 0 ? "in_table" : table == 0 ? "absent" : "unreadable")+
                    "\",\"version_resolved\":"+(RawProc(module, version) ? "true" : "false")+
                    ",\"fg\":\""+CandidateOutcome(isFg, *fg)+"\",\"ll\":\""+CandidateOutcome(isLl, *ll)+"\"";
                const auto& role = fgName ? *fg : *ll;
                if (role.hasVersion)
                    fields += ",\"api_version\":\""+std::to_string(role.major)+"."+std::to_string(role.minor)+"."+
                        std::to_string(role.patch)+"\"";
                // The admitted and the rejected-with-a-version ones get their hash from native_sdk_module or
                // sdk_candidate_rejected.
                unversioned = !isFg && !isLl && !fg->versioned && !ll->versioned;
            }
            pending.push_back({module, std::move(fields), unversioned});
        } catch (...) {}
    }
    ~SeenModules()
    {
        for (const auto& line : pending) LogSdkModuleSeen(line.module, line.fields, line.withHash);
    }
};
}
HMODULE Self() noexcept { return selfReady.load(std::memory_order_acquire) ? selfModule : nullptr; }
VulkanHostCallScope::VulkanHostCallScope() noexcept : previous(tVulkanHostCall) { tVulkanHostCall = true; }
VulkanHostCallScope::~VulkanHostCallScope() { tVulkanHostCall = previous; }
GatewaySleepScope::GatewaySleepScope() noexcept : previous(tGatewaySleep) { tGatewaySleep = true; }
GatewaySleepScope::~GatewaySleepScope() { tGatewaySleep = previous; }
HMODULE NativeFgModule() noexcept { return Active() ? fgOwner : nullptr; }
HMODULE NativeLlModule() noexcept { return Active() ? llOwner : nullptr; }
bool HostOwnsLatency() noexcept { return Active() && hostLatency; }

bool ResolveNativeSdkCreate(HMODULE importer, const void* caller, HMODULE module,
    LPCSTR name, FARPROC& result) noexcept
{
    const DWORD incomingError = GetLastError();
    const bool createName = name && reinterpret_cast<uintptr_t>(name) > 0xffff
        && (!strcmp(name, "xefgSwapChainD3D12CreateContext") || !strcmp(name, "xellD3D12CreateContext"));
    // The selected owner pair is immutable after active's release publication.
    // Do not inspect it before acquire, or trust a stolen wrapper's importer.
    const bool handled = createName && module && importer && active.load(std::memory_order_acquire)
        && (importer == fgOwner || importer == llOwner)
        && OwnCode(importer, caller);
    SetLastError(incomingError);
    if (!handled) return false;
    result = RawProc(module, name);
    return true;
}
bool Active() noexcept { return active.load(std::memory_order_acquire); }
bool AdoptVulkanHostPair(HMODULE fg, HMODULE ll) noexcept
{
    {
        std::lock_guard lock(discoveryMutex);
        if (!selfReady.load(std::memory_order_acquire) || !fg || !ll || fg == ll) return false;
        if (vulkanFg && (vulkanFg != fg || vulkanLl != ll)) return false;   // one pair per process
        vulkanFg = fg;
        vulkanLl = ll;
        vulkanPairKnown.store(true, std::memory_order_release);
        // Under OptiScaler the route is already active on the pair OptiScaler loaded; the host drives
        // exactly that pair (owners are immutable after active's publication).
        if (attempted && Active() && fgOwner == fg && llOwner == ll) vulkanHost.store(true, std::memory_order_release);
    }
    DiscoverSdkModules();
    return Active() && vulkanHost.load(std::memory_order_acquire) && fgOwner == fg && llOwner == ll;
}
bool ActiveSdkPair(HMODULE& fg, HMODULE& ll) noexcept
{
    if (!Active()) return false;
    fg = fgOwner;
    ll = llOwner;
    return fg && ll && fg != ll;
}
bool HostRedirectedExport(HMODULE module, const char* name) noexcept
{
    return module && name && HostRedirectedEntry(reinterpret_cast<const void*>(RawProc(module, name)));
}

namespace
{
// sleepsight.1: libxell's sleep, timed below every route into it. OptiScaler's XeLL input detours libxell's exports into
// its own code and calls the originals through its trampolines, so neither our export hook nor Hook_xellSleep sees the
// sleep that holds the game. In libxell 1.3.2.10 (D2030DCD) the export xellSleep is `jmp` to its body (RVA 0xCBB0:
// handle -> implementation, vtable +0x28); every route, OptiScaler's trampoline included, reaches that body. The target
// is decoded from the module's own file (the export's first instruction there, E9 rel32, into an executable section),
// the loaded image must be that file (time stamp and image size) and the target's first 16 bytes in memory must equal
// the file's (nothing else patched it); otherwise the probe is not installed (logged). The hook only times the call.
// optisleep.1: on OptiScaler's calls (HostProbeActs) the low-latency pacing's sleep follows it, and the marker body's
// PRESENT_START / PRESENT_END time the game's whole Present (sdk_pacing HostPresentMarker); calls pass on unchanged.
constexpr size_t kXellProbeBytes = 16;
constexpr size_t kXellProbeFileLimit = 64u << 20;
using XellSleepFunction = xell_result_t (*)(xell_context_handle_t, uint32_t);
std::atomic<XellSleepFunction> xellSleepOriginal{};
std::atomic<bool> xellSleepProbeTried{};
// optisleep.1: libxell's marker body (1.3.2.10: xellAddMarkerData `jmp` to RVA 0xC450), probed the same way.
using XellMarkerFunction = xell_result_t (*)(xell_context_handle_t, uint32_t, xell_latency_marker_type_t);
std::atomic<XellMarkerFunction> xellMarkerOriginal{};
// standalone_policy.h PresentMarkerEdge names the Present markers by value.
static_assert(XELL_PRESENT_START == 4 && XELL_PRESENT_END == 5, "XeLL's Present marker values changed");

// optisleep.1: OptiScaler's call (standalone_policy.h HostProbeActs): never our standalone hosts' own calls, which run
// inside a VulkanHostCallScope and sleep and time the Present themselves, nor a call through our own Hook_xellSleep
// (GatewaySleepScope: a game's own XeLL context, where OptiScaler left the export to our gateway), which sleeps after
// releasing the context's lease.
bool HostProbeActs() noexcept
{
    return xemfg_standalone::HostProbeActs(optiscaler_host::Detected(), tVulkanHostCall || tGatewaySleep);
}

xell_result_t XellSleepBody(xell_context_handle_t handle, uint32_t frame)
{
    xell_result_t result;
    {
        sdk_pacing::HostSleepScope scope(sdk_pacing::HostSleepSource::kXell);
        const auto original = xellSleepOriginal.load(std::memory_order_acquire);
        result = original ? original(handle, frame) : XELL_RESULT_ERROR_INVALID_CONTEXT;
    }
    // optisleep.1: OptiScaler's XeLL sleep is the game's sleep point before its input (latency.1's sleep follows it).
    if (HostProbeActs()) sdk_pacing::HostSleepPoint();
    return result;
}

xell_result_t XellMarkerBody(xell_context_handle_t handle, uint32_t frame, xell_latency_marker_type_t marker)
{
    // optisleep.1: OptiScaler's PRESENT_START / PRESENT_END bracket the game's Present (its whole wait there).
    const int edge = xemfg_standalone::PresentMarkerEdge(uint32_t(marker));
    const bool acts = edge && HostProbeActs();
    if (acts && edge > 0) sdk_pacing::HostPresentMarker(true, frame);
    const auto original = xellMarkerOriginal.load(std::memory_order_acquire);
    const xell_result_t result = original ? original(handle, frame, marker) : XELL_RESULT_ERROR_INVALID_CONTEXT;
    if (acts && edge < 0) sdk_pacing::HostPresentMarker(false, frame);
    return result;
}

bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>& data) noexcept
{
    try
    {
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= LONGLONG(kXellProbeFileLimit);
        if (ok)
        {
            data.resize(size_t(size.QuadPart));
            DWORD got{};
            ok = ReadFile(file, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
        }
        CloseHandle(file);
        return ok;
    }
    catch (...) { return false; }
}

// The file offset of `size` bytes at `rva` (inside one section's raw data), or 0; `executable`: that section's.
size_t FileOffset(const std::vector<uint8_t>& file, const IMAGE_NT_HEADERS64& nt, size_t sections, uint32_t rva,
    size_t size, bool* executable = nullptr) noexcept
{
    const auto* section = reinterpret_cast<const IMAGE_SECTION_HEADER*>(reinterpret_cast<const uint8_t*>(&nt.OptionalHeader)
        + nt.FileHeader.SizeOfOptionalHeader);
    for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];
        if (rva < s.VirtualAddress || uint64_t(rva)-s.VirtualAddress+size > s.SizeOfRawData) continue;
        const uint64_t offset = uint64_t(s.PointerToRawData)+(rva-s.VirtualAddress);
        if (offset+size > file.size() || sections+i*sizeof(IMAGE_SECTION_HEADER) > file.size()) return 0;
        if (executable) *executable = (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        return size_t(offset);
    }
    return 0;
}

// The body the file's export `name` jumps to (E9 rel32), and its first kXellProbeBytes bytes in the file.
const char* DecodeExportJump(HMODULE module, const char* name, uint32_t& exportRva, uint32_t& targetRva,
    std::array<uint8_t, kXellProbeBytes>& bytes) noexcept
{
    constexpr DWORD kPathChars = 32768;
    static thread_local wchar_t path[kPathChars];
    const DWORD length = GetModuleFileNameW(module, path, kPathChars);
    if (!length || length >= kPathChars) return "module_path";
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, file)) return "file_read";
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return "file_short";
    IMAGE_DOS_HEADER dos{};
    memcpy(&dos, file.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0
        || size_t(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64) > file.size()) return "file_headers";
    const auto& nt = *reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data()+dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return "file_headers";
    const size_t sections = size_t(dos.e_lfanew)+offsetof(IMAGE_NT_HEADERS64, OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    if (sections+size_t(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER) > file.size()) return "file_sections";
    // The loaded image is this file.
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    IMAGE_DOS_HEADER loadedDos{};
    IMAGE_NT_HEADERS64 loaded{};
    SIZE_T got{};
    if (!ReadProcessMemory(GetCurrentProcess(), base, &loadedDos, sizeof(loadedDos), &got) || got != sizeof(loadedDos)
        || loadedDos.e_lfanew != dos.e_lfanew
        || !ReadProcessMemory(GetCurrentProcess(), base+loadedDos.e_lfanew, &loaded, sizeof(loaded), &got)
        || got != sizeof(loaded)
        || loaded.FileHeader.TimeDateStamp != nt.FileHeader.TimeDateStamp
        || loaded.OptionalHeader.SizeOfImage != nt.OptionalHeader.SizeOfImage) return "image_not_file";
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const size_t exports = FileOffset(file, nt, sections, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!directory.VirtualAddress || !exports) return "export_directory";
    IMAGE_EXPORT_DIRECTORY table{};
    memcpy(&table, file.data()+exports, sizeof(table));
    const size_t names = FileOffset(file, nt, sections, table.AddressOfNames, size_t(table.NumberOfNames)*4);
    const size_t ordinals = FileOffset(file, nt, sections, table.AddressOfNameOrdinals, size_t(table.NumberOfNames)*2);
    const size_t functions = FileOffset(file, nt, sections, table.AddressOfFunctions, size_t(table.NumberOfFunctions)*4);
    if (!names || !ordinals || !functions) return "export_tables";
    const size_t nameLength = strlen(name);
    exportRva = 0;
    for (DWORD i = 0; i < table.NumberOfNames && !exportRva; ++i)
    {
        uint32_t nameRva{};
        memcpy(&nameRva, file.data()+names+size_t(i)*4, 4);
        const size_t text = FileOffset(file, nt, sections, nameRva, nameLength+1);
        if (!text || memcmp(file.data()+text, name, nameLength+1) != 0) continue;
        uint16_t ordinal{};
        memcpy(&ordinal, file.data()+ordinals+size_t(i)*2, 2);
        if (ordinal >= table.NumberOfFunctions) return "export_ordinal";
        memcpy(&exportRva, file.data()+functions+size_t(ordinal)*4, 4);
    }
    if (!exportRva) return "export_missing";
    if (exportRva >= directory.VirtualAddress && exportRva < directory.VirtualAddress+directory.Size) return "export_forwarded";
    const size_t entry = FileOffset(file, nt, sections, exportRva, 5);
    if (!entry) return "export_offset";
    if (file[entry] != 0xE9) return "export_not_jump";
    int32_t displacement{};
    memcpy(&displacement, file.data()+entry+1, 4);
    const int64_t target = int64_t(exportRva)+5+displacement;
    if (target <= 0 || target >= int64_t(nt.OptionalHeader.SizeOfImage)) return "target_bounds";
    targetRva = uint32_t(target);
    bool executable{};
    const size_t body = FileOffset(file, nt, sections, targetRva, kXellProbeBytes, &executable);
    if (!body || !executable) return "target_section";
    memcpy(bytes.data(), file.data()+body, kXellProbeBytes);
    return nullptr;
}

// One libxell body probe: export `name`'s jump target decoded from the file, unchanged in memory, hooked with `detour`
// (the trampoline goes to `original` before the hook is enabled). Logged as `stage`.
template<class Function>
void InstallXellBodyProbe(HMODULE module, const char* name, const char* stage, void* detour,
    std::atomic<Function>& original) noexcept
{
    uint32_t exportRva{}, targetRva{};
    std::array<uint8_t, kXellProbeBytes> expected{}, actual{};
    const char* reason = DecodeExportJump(module, name, exportRva, targetRva, expected);
    auto* target = reinterpret_cast<uint8_t*>(module)+targetRva;
    SIZE_T got{};
    if (!reason && !OwnCode(module, target)) reason = "target_not_module_code";
    if (!reason && (!ReadProcessMemory(GetCurrentProcess(), target, actual.data(), actual.size(), &got)
        || got != actual.size() || actual != expected)) reason = "target_changed";
    void* trampoline{};
    if (!reason && MH_CreateHook(target, detour, &trampoline) != MH_OK)
        reason = "hook_plan_failed";
    if (!reason)
    {
        original.store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
        if (MH_EnableHook(target) != MH_OK) reason = "hook_enable_failed";
    }
    const bool redirected = HostRedirectedEntry(reinterpret_cast<const void*>(RawProc(module, name)));
    try
    {
        WriteLog(stage, std::string("\"installed\":")+(reason ? "false" : "true")+",\"reason\":\""+
            (reason ? reason : "")+"\",\"export_rva\":"+std::to_string(exportRva)+",\"target_rva\":"+
            std::to_string(targetRva)+",\"export_redirected\":"+(redirected ? "true" : "false"));
    }
    catch (...) {}
}

// Installs the libxell probes on `module` once per process: the sleep (xell_sleep_probe) and, optisleep.1, the markers
// (xell_marker_probe). Each fails closed on its own.
void InstallXellSleepProbe(HMODULE module) noexcept
{
    if (!module || xellSleepProbeTried.exchange(true, std::memory_order_acq_rel)) return;
    InstallXellBodyProbe(module, "xellSleep", "xell_sleep_probe", reinterpret_cast<void*>(&XellSleepBody),
        xellSleepOriginal);
    InstallXellBodyProbe(module, "xellAddMarkerData", "xell_marker_probe", reinterpret_cast<void*>(&XellMarkerBody),
        xellMarkerOriginal);
}
}
bool InstallImports(HMODULE module) noexcept
{
    std::lock_guard lock(discoveryMutex);
    if (selfReady.load(std::memory_order_acquire))
        return module == selfModule;
    HMODULE pinned{};
    if (!module || (selfModule && selfModule != module)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return false;
    selfModule = module;
    selfReady.store(true, std::memory_order_release);
    return true;
}
// OptiScaler loads its own XeSS FG/XeLL pair (v10: <host dir>\OptiScaler\,
// 0.9.4: beside the host) while the game may load its own copies. When a role
// has several candidates, the copy in the detected host's library folder is
// the one OptiScaler drives (it also redirects the game's XeLL exports to its
// own copy). Identity is still established later by the exact provider
// profile; the folder only selects between otherwise-admitted candidates.
bool InDirectory(HMODULE module, const std::wstring& directory) noexcept
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH || directory.empty() || length <= directory.size()) return false;
    if (_wcsnicmp(path, directory.c_str(), directory.size()) != 0) return false;
    return !wcschr(path + directory.size(), L'\\');
}
HMODULE PreferHostCopy(const std::vector<HMODULE>& candidates) noexcept
{
    if (!optiscaler_host::Detected()) return nullptr;
    const auto host = optiscaler_host::Read();
    std::wstring directory(host.path);
    const size_t slash = directory.find_last_of(L'\\');
    if (slash == std::wstring::npos) return nullptr;
    directory.resize(slash + 1);
    for (const std::wstring& folder : {directory + L"OptiScaler\\", directory}) {
        HMODULE chosen{};
        size_t matches{};
        for (HMODULE module : candidates) if (InDirectory(module, folder)) { chosen = module; ++matches; }
        if (matches == 1) return chosen;
        if (matches > 1) return nullptr;
    }
    return nullptr;
}
void DiscoverSdkModules() noexcept
{
    // sdkscan.1: the module list is read before discoveryMutex. Toolhelp takes the loader lock, and no thread may wait
    // for it while holding discoveryMutex (a Create call can reach discovery from any thread).
    std::vector<HMODULE> modules;
    ModuleListStats listing;
    bool listed = false;
    // Only where discovery lists anything: under OptiScaler or for the Vulkan host's pair, until a pair is attempted.
    const bool scanned = selfReady.load(std::memory_order_acquire) && !discoveryDone.load(std::memory_order_acquire)
        && (optiscaler_host::Detected() || vulkanPairKnown.load(std::memory_order_acquire));
    if (scanned)
        listed = LoadedModules(modules, listing, tCreateDiscovery ? ToolhelpList::eFallback : ToolhelpList::eAlways);
    std::lock_guard lock(discoveryMutex);
    if (!selfReady.load(std::memory_order_acquire)) return;
    // XeSS MFG runs under OptiScaler, or under RTXMFG's own Vulkan host on exactly the pair it loaded.
    // Without either a game's own XeSS-FG stays untouched: no resolver detour, no candidate pins, no
    // SDK hooks. Nothing latches, so OptiScaler loaded later still activates the route (contexts
    // created before then stay native).
    const bool vulkanPair = vulkanFg && vulkanLl;
    if (!optiscaler_host::Detected() && !vulkanPair)
    {
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_relaxed))
            WriteLog("sdk_route_requires_optiscaler", "\"optiscaler\":false,\"native_xefg\":\"untouched\"");
        return;
    }
    if (!vulkanPair) InstallResolver();
    if (attempted || !scanned) return;
    try {
        // sdkscan.1: the whole list (psapi's without a cap, plus Toolhelp's) and a line when listing fails or changes.
        // This runs only until a pair is admitted.
        std::vector<const char*> kinds(modules.size());
        size_t named{};
        for (size_t i=0; i<modules.size(); ++i)
            if (!PrivateModule(modules[i]) && (kinds[i] = SdkFileKind(modules[i])) != nullptr) ++named;
        if (!tCreateDiscovery) LogModuleScan(listing, modules.size(), named);   // the worker's passes report the listing
        if (!listed) return;
        SeenModules seen;   // written when this pass returns, after any activation
        std::vector<HMODULE> fgs, lls;
        for (size_t i=0; i<modules.size(); ++i) {
            const auto module = modules[i];
            if (PrivateModule(module)) continue;
            HMODULE retained{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(module), &retained) || retained != module) {
                if (kinds[i]) seen.Add(module, kinds[i], false);
                continue;
            }
            CandidateCheck fgCheck, llCheck;
            const bool isFg = Candidate(module, true, fgCheck), isLl = Candidate(module, false, llCheck);
            LogRejectedCandidate(module, true, fgCheck);
            LogRejectedCandidate(module, false, llCheck);
            if (kinds[i]) seen.Add(module, kinds[i], true, &fgCheck, isFg, &llCheck, isLl);
            if (isFg || isLl) {
                HMODULE pinned{};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { FreeLibrary(retained); return; }
            }
            if (isFg && std::find(fgs.begin(), fgs.end(), module) == fgs.end()) fgs.push_back(module);
            if (isLl && std::find(lls.begin(), lls.end(), module) == lls.end()) lls.push_back(module);
            FreeLibrary(retained);
        }
        const auto choose = [](const std::vector<HMODULE>& list, const char* role) -> HMODULE {
            if (list.size() == 1) return list.front();
            if (list.size() < 2) return nullptr;
            HMODULE preferred = PreferHostCopy(list);
            WriteLog("sdk_candidates_ambiguous", std::string("\"role\":\"") + role + "\",\"count\":"
                + std::to_string(list.size()) + ",\"optiscaler_copy\":" + (preferred ? "true" : "false"));
            return preferred;
        };
        HMODULE fg = choose(fgs, "fg"), ll = choose(lls, "ll");
        if (vulkanPair)
        {
            // The Vulkan host's own pair, and only when discovery admits both of its modules.
            const bool fgFound = std::find(fgs.begin(), fgs.end(), vulkanFg) != fgs.end();
            const bool llFound = std::find(lls.begin(), lls.end(), vulkanLl) != lls.end();
            WriteLog("sdk_vulkan_host_pair", std::string("\"fg_admitted\":")+(fgFound ? "true" : "false")+
                ",\"ll_admitted\":"+(llFound ? "true" : "false")+",\"fg_candidates\":"+std::to_string(fgs.size())+
                ",\"ll_candidates\":"+std::to_string(lls.size()));
            if (!fgFound || !llFound) return;
            fg = vulkanFg;
            ll = vulkanLl;
        }
        // Activate one complete pair. Additional providers retain native contexts.
        if (!fg || !ll || fg == ll)
        {
            // rejectlog.1: what discovery admitted while no pair is complete, each time the counts change (at least one
            // admitted module; the rejected ones have their own sdk_candidate_rejected line).
            static size_t loggedFg = 0, loggedLl = 0;
            if (!vulkanPair && fgs.size()+lls.size() > 0 && (fgs.size() != loggedFg || lls.size() != loggedLl))
            {
                loggedFg = fgs.size();
                loggedLl = lls.size();
                WriteLog("sdk_pair_incomplete", "\"fg_candidates\":"+std::to_string(fgs.size())+",\"ll_candidates\":"+
                    std::to_string(lls.size())+",\"same_module\":"+(fg && fg == ll ? "true" : "false"));
            }
            return;
        }
        attempted = true;
        discoveryDone.store(true, std::memory_order_release);
        // RTXMFG's DllMain already owns the per-process module claim; DLSS-G
        // and XeSS-FG are two backends of the same owner.
        auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
        // Witness the pristine SDK before our public-entry detours change its thunks.
        // A private-layout rejection does not disable the native public API route.
        // Entry jumps OptiScaler placed into itself don't hide the layout behind them.
        xess_layout::entryRedirectFilter.store(&HostRedirectedEntry, std::memory_order_release);
        const bool privateLayout = xess_layout::Pair(fg, ll);
        WriteLog("sdk_private_layout", std::string("\"eligible\":") + (privateLayout ? "true" : "false"));
        fgOwner = fg; llOwner = ll;
        std::vector<void*> created;
        size_t hostRedirected{}, hostLifetime{};
        for (auto& entry : entries) {
            entry.target = reinterpret_cast<void*>(RawProc(entry.fg ? fg : ll, entry.name));
            if (!entry.target) continue;
            // OptiScaler routes this export itself (its XeLL input). Calls keep taking its jump, which
            // gets no second hook on top.
            if (HostRedirectedEntry(entry.target)) {
                *entry.original = entry.target; ++hostRedirected;
                if (!entry.fg && (!strcmp(entry.name, "xellD3D12CreateContext") || !strcmp(entry.name, "xellDestroyContext")))
                    ++hostLifetime;
                continue;
            }
            void* trampoline{};
            if (MH_CreateHook(entry.target, entry.hook, &trampoline) != MH_OK) {
                for (void* target : created) MH_RemoveHook(target);
                WriteLog("sdk_route_rejected", "\"reason\":\"api_hook_plan_failed\""); return;
            }
            *entry.original = trampoline;
            created.push_back(entry.target);
        }
        for (auto& entry : internalEntries) {
            entry.target = reinterpret_cast<void*>(RawProc(entry.fg ? fg : ll,entry.name));
            if (!entry.target) continue;
            if (HostRedirectedEntry(entry.target)) { entry.original = entry.target; ++hostRedirected; continue; }
            if (MH_CreateHook(entry.target,entry.hook,&entry.original) != MH_OK) {
                for (void* target : created) MH_RemoveHook(target);
                WriteLog("sdk_route_rejected","\"reason\":\"private_api_guard_plan_failed\""); return;
            }
            created.push_back(entry.target);
        }
        hostLatency = hostLifetime == 2;
        prepared.store(true, std::memory_order_release);
        bool queued = true;
        for (void* target : created) if (MH_QueueEnableHook(target) != MH_OK) queued = false;
        const bool enabled = queued && MH_ApplyQueued() == MH_OK;
        // Never remove trampolines after publication, even on partial enablement.
        if (!enabled) { WriteLog("sdk_route_rejected", "\"reason\":\"api_hook_commit_failed_native_only\""); return; }
        active.store(true, std::memory_order_release);
        if (vulkanPair) vulkanHost.store(true, std::memory_order_release);
        LogSdkModule(fg,"fg"); LogSdkModule(ll,"ll");
        WriteLog("sdk_route_ready", "\"entrypoints\":"+std::to_string(created.size())+
            ",\"fg_module\":"+std::to_string(uintptr_t(fg))+",\"ll_module\":"+std::to_string(uintptr_t(ll))+
            ",\"host_redirected\":"+std::to_string(hostRedirected)+",\"host_xell\":"+(hostLatency ? "true" : "false"));
        // sleepsight.1: the game's XeLL sleep, whichever route reaches libxell.
        InstallXellSleepProbe(ll);
    } catch (...) { WriteLog("sdk_route_rejected", "\"reason\":\"discovery_exception\""); }
}
}
