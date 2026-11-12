#include "gpu_runtime.h"
#include "checked_memory.h"
#include "../protected_pointer.h"
#include "../overlay_native.h"
#include "../single_module.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <psapi.h>
#include <dxgi1_4.h>
namespace witcher_dots {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t kMaxLists=1024,kMaxQueues=32,kMaxLeases=128,kMaxOwners=32,kMaxRoots=1024;
// Lists and root signatures the game destroyed leave tracking (TakeReleased*):
// save loads and settings changes recreate renderer and frame-generation
// objects. A list created past capacity would stay untracked, and a hair build
// on it falls back to raster until the hair is recreated.
constexpr size_t kListSweepAt=128;constexpr uint64_t kListSweepMs=1000;
// Removed keys of the lock-free indexes below. A removed key whose successor
// slot is empty ends no probe path and is cleared (Vacate), so lookups stay
// short however many objects come and go.
constexpr uintptr_t kRemovedKey=1;
struct Published { std::atomic<uintptr_t> slot{};std::atomic<void*> original{};void* hook{};DWORD protection{};size_t index{}; };
// Open addressing keyed by vtable slot address. Every hooked call resolves its
// forwarding binding here without a lock, syscall or scan; each Agility
// command list embeds its own table, so entries come and go with lists
// (removed only when the list is destroyed, see UnpublishTable).
constexpr size_t kPublicationSlots=32768;
std::array<Published,kPublicationSlots> publications{};
std::mutex publicationLock;
size_t SlotHash(uintptr_t slot) noexcept {return static_cast<size_t>(((slot>>3)*0x9E3779B97F4A7C15ull)>>49);}
Published* FindPublication(uintptr_t slot) noexcept {
    for(size_t n=0,i=SlotHash(slot);n<kPublicationSlots;++n,i=(i+1)&(kPublicationSlots-1)) {
        const auto observed=publications[i].slot.load(std::memory_order_acquire);
        if(observed==slot)return &publications[i];
        if(!observed)return nullptr;
    }
    return nullptr;
}
// Caller holds publicationLock. Fields are complete before the slot is visible.
Published* NewPublication(uintptr_t slot) noexcept {
    for(size_t n=0,i=SlotHash(slot);n<kPublicationSlots;++n,i=(i+1)&(kPublicationSlots-1)) {
        const auto key=publications[i].slot.load(std::memory_order_relaxed);
        if(!key||key==kRemovedKey)return &publications[i];
    }
    return nullptr;
}
// Caller holds publicationLock. Only for a table inside a destroyed list.
void Unpublish(uintptr_t slot) noexcept {
    for(size_t n=0,i=SlotHash(slot);n<kPublicationSlots;++n,i=(i+1)&(kPublicationSlots-1)) {
        const auto key=publications[i].slot.load(std::memory_order_relaxed);
        if(!key)return;
        if(key!=slot)continue;
        publications[i].slot.store(kRemovedKey,std::memory_order_release);
        if(publications[(i+1)&(kPublicationSlots-1)].slot.load(std::memory_order_relaxed))return;
        for(size_t j=i;publications[j].slot.load(std::memory_order_relaxed)==kRemovedKey;j=(j-1)&(kPublicationSlots-1))
            publications[j].slot.store(0,std::memory_order_release);
        return;
    }
}
// Agility builds each list's private table by copying the device's (hooked)
// image table, then overriding the UMD fast-path slots. Lists this runtime
// never tracked therefore reach the image-table hooks through tables with no
// publication. Their forward is the image table's original for that index,
// the same D3D12Core entry every table of the device uses. Never null once the
// matching image slot is published; a hook cannot exist before that.
enum class Kind : size_t { List,Queue,Device,Count };
constexpr size_t kCanonicalSlots=80;
std::array<std::array<std::atomic<void*>,kCanonicalSlots>,static_cast<size_t>(Kind::Count)> canonical{};
bool ExchangeMethod(void** slot,void* before,void* after,DWORD& protection) noexcept {
    // D3D12Core's optimized command-list table is already writable image data.
    // Changing its RW page protection needlessly can turn it into WRITECOPY.
    // Preserve the existing protection and perform only the expected-value CAS.
    if(protection==PAGE_READWRITE||protection==PAGE_WRITECOPY) {
        const auto address=reinterpret_cast<uintptr_t>(slot);
        if(!protected_pointer::ProtectionMatches(address,protection,sizeof(void*)))return false;
        const auto observed=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),after,before);
        if(observed!=before||protected_pointer::ReadPointer(address)!=after)return false;
        DWORD effective{};if(!protected_pointer::QueryProtection(address,effective,sizeof(void*)))return false;
        if(protection==PAGE_WRITECOPY&&effective==PAGE_READWRITE) {
            // A write to an image's COW data page naturally becomes private RW.
            // Establish that physical sharing ended before accepting this
            // equivalent protection; the original mapped file is untouched.
            PSAPI_WORKING_SET_EX_INFORMATION page{};page.VirtualAddress=slot;
            if(!QueryWorkingSetEx(GetCurrentProcess(),&page,sizeof(page))
                ||!page.VirtualAttributes.Valid||page.VirtualAttributes.Shared)return false;
            protection=effective;
        }
        return effective==protection;
    }
    return single_overlay::slots::Exchange(slot,before,after,protection);
}
struct RuntimeFile {
    HANDLE file{INVALID_HANDLE_VALUE},mapping{};
    const void* view{};
    ~RuntimeFile() {
        if(view)UnmapViewOfFile(view);
        if(mapping)CloseHandle(mapping);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    }
};
// Publication holds publicationLock. ImageMethod pins each image before this
// check, so cached approvals cannot be reused for an unloaded/replaced module.
std::array<HMODULE,2> verifiedAgility{};
bool SameRuntimeHeaders(const IMAGE_NT_HEADERS64& disk,IMAGE_NT_HEADERS64 loaded,HMODULE module) {
    // Agility's independent loader records the relocated base in the header.
    // Accept only the file's preferred base or this image's actual allocation.
    if(loaded.OptionalHeader.ImageBase!=disk.OptionalHeader.ImageBase
        &&loaded.OptionalHeader.ImageBase!=reinterpret_cast<uintptr_t>(module))return false;
    loaded.OptionalHeader.ImageBase=disk.OptionalHeader.ImageBase;
    return !std::memcmp(&disk,&loaded,sizeof(disk));
}
bool VerifiedRuntimeImage(HMODULE module,const wchar_t* path,const char* hash,size_t limit,UINT expectedSdk) {
    RuntimeFile image;
    image.file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    LARGE_INTEGER size{};
    if(image.file==INVALID_HANDLE_VALUE||!GetFileSizeEx(image.file,&size)
        ||size.QuadPart<sizeof(IMAGE_NT_HEADERS64)||size.QuadPart>static_cast<LONGLONG>(limit))return false;
    image.mapping=CreateFileMappingW(image.file,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!image.mapping)return false;
    image.view=MapViewOfFile(image.mapping,FILE_MAP_READ,0,0,0);
    // A null hash admits any content; the loaded image must still match the
    // file's PE headers (only the header pages of the mapping are read).
    if(!image.view||(hash&&!HashEquals({static_cast<const std::byte*>(image.view),static_cast<size_t>(size.QuadPart)},hash)))return false;
    IMAGE_DOS_HEADER diskDos{},loadedDos{};IMAGE_NT_HEADERS64 diskNt{},loadedNt{};
    if(!Read(image.view,0,diskDos)||diskDos.e_magic!=IMAGE_DOS_SIGNATURE||diskDos.e_lfanew<0
        ||static_cast<size_t>(diskDos.e_lfanew)>static_cast<size_t>(size.QuadPart)-sizeof(diskNt)
        ||!Read(image.view,static_cast<size_t>(diskDos.e_lfanew),diskNt)
        ||diskNt.Signature!=IMAGE_NT_SIGNATURE||diskNt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64
        ||diskNt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC
        ||!Read(module,0,loadedDos)||loadedDos.e_magic!=diskDos.e_magic||loadedDos.e_lfanew!=diskDos.e_lfanew
        ||!Read(module,static_cast<size_t>(diskDos.e_lfanew),loadedNt)
        ||!SameRuntimeHeaders(diskNt,loadedNt,module))return false;
    if(!expectedSdk)return true;
    const auto version=GetProcAddress(module,"D3D12SDKVersion");
    const auto versionAddress=reinterpret_cast<uintptr_t>(version),base=reinterpret_cast<uintptr_t>(module);
    MEMORY_BASIC_INFORMATION memory{};UINT sdkVersion{};
    if(!version||versionAddress<base||diskNt.OptionalHeader.SizeOfImage<sizeof(sdkVersion)
        ||versionAddress-base>diskNt.OptionalHeader.SizeOfImage-sizeof(sdkVersion)
        ||VirtualQuery(version,&memory,sizeof(memory))!=sizeof(memory)||memory.AllocationBase!=module
        ||memory.Type!=MEM_IMAGE||memory.State!=MEM_COMMIT
        ||(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS|PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
        ||!Read(version,0,sdkVersion)||sdkVersion!=expectedSdk)return false;
    return true;
}
bool VerifiedAgilityRuntime(HMODULE module,const wchar_t* path,const wchar_t* leaf) {
    size_t profile{};const char* hash{};
    if(!_wcsicmp(leaf,L"d3d12core.dll")) {
        profile=0;hash="681C72042ED1691A326C2AADFF47A1957ED998F649158312C155C1B2AB71F44C";
    } else if(!_wcsicmp(leaf,L"d3d12sdklayers.dll")) {
        profile=1;hash="591183CDF1EB175F2342B890CDDC651DBF08A8811DB0C1A84AB6EC4E94AB71BD";
    } else return false;
    if(verifiedAgility[profile]==module)return true;
    if(verifiedAgility[profile])return false;
    // The first DOTS profile is the game's verified Microsoft Agility SDK 619.
    // A directory/name/version alone does not admit another DLL or a proxy.
    if(!VerifiedRuntimeImage(module,path,hash,16*1024*1024,619))return false;
    verifiedAgility[profile]=module;
    wchar_t message[512]{};swprintf_s(message,L"WITCHER_DOTS native_runtime=verified_agility sdk=619 module=%s",path);
    single_module::Log(message);
    return true;
}
bool NativeRuntime(HMODULE module) {
    wchar_t path[MAX_PATH]{},system[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(module,path,_countof(path));
    const DWORD systemSize=GetSystemDirectoryW(system,_countof(system));
    if(!n||n>=_countof(path)||!systemSize||systemSize>=_countof(system))return false;
    const auto* leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
    if(n>systemSize&&!_wcsnicmp(path,system,systemSize)&&path[systemSize]==L'\\'
        &&(!_wcsicmp(leaf,L"d3d12.dll")||!_wcsicmp(leaf,L"d3d12core.dll")||!_wcsicmp(leaf,L"d3d12sdklayers.dll")))return true;
    try {return VerifiedAgilityRuntime(module,path,leaf);} catch(...) {return false;}
}
HMODULE verifiedListDriver{};
bool DeviceLuid(LUID& luid); // The prepared device's adapter; set before any list is validated.
// NVIDIA display drivers carry file version 32.0.1X.YYYY for release XYY.YY
// (617.14 = 32.0.16.1714). DOTS is validated from 617.14 onward.
constexpr std::array<WORD,4> kMinimumDriverFile{32,0,16,1714};
bool DriverFileVersion(const wchar_t* path,std::array<WORD,4>& version) {
    using SizeFn=DWORD(WINAPI*)(LPCWSTR,LPDWORD);using InfoFn=BOOL(WINAPI*)(LPCWSTR,DWORD,DWORD,LPVOID);
    using QueryFn=BOOL(WINAPI*)(LPCVOID,LPCWSTR,LPVOID*,PUINT);
    const HMODULE module=single_module::LoadSystemModule(L"version.dll");
    const auto size=reinterpret_cast<SizeFn>(GetProcAddress(module,"GetFileVersionInfoSizeW"));
    const auto info=reinterpret_cast<InfoFn>(GetProcAddress(module,"GetFileVersionInfoW"));
    const auto query=reinterpret_cast<QueryFn>(GetProcAddress(module,"VerQueryValueW"));
    DWORD ignored{};const DWORD bytes=size&&info&&query?size(path,&ignored):0;
    if(!bytes||bytes>1024*1024)return false;
    std::vector<std::byte> data(bytes);VS_FIXEDFILEINFO* fixed{};UINT length{};
    if(!info(path,0,bytes,data.data())||!query(data.data(),L"\\",reinterpret_cast<void**>(&fixed),&length)
        ||!fixed||length<sizeof(VS_FIXEDFILEINFO)||fixed->dwSignature!=0xFEEF04BD)return false;
    version={HIWORD(fixed->dwFileVersionMS),LOWORD(fixed->dwFileVersionMS),HIWORD(fixed->dwFileVersionLS),LOWORD(fixed->dwFileVersionLS)};
    return true;
}
// Directory (with trailing separator) of the D3D12 user-mode driver the kernel
// registered for this adapter: NVIDIA's nvldumdx.dll loader, which loads
// nvwgf2umx.dll from the same driver package.
bool AdapterDriverDirectory(const LUID& luid,std::wstring& directory) {
    struct OpenFromLuid {LUID luid;UINT adapter;};                       // D3DKMT_OPENADAPTERFROMLUID
    struct Query {UINT adapter;UINT type;void* data;UINT size;};          // D3DKMT_QUERYADAPTERINFO
    struct UmdName {UINT version;wchar_t name[MAX_PATH];};                // D3DKMT_UMDFILENAMEINFO
    struct Close {UINT adapter;};                                         // D3DKMT_CLOSEADAPTER
    static_assert(sizeof(OpenFromLuid)==12&&sizeof(Query)==24&&sizeof(UmdName)==524);
    using OpenFn=LONG(APIENTRY*)(OpenFromLuid*);using QueryFn=LONG(APIENTRY*)(const Query*);using CloseFn=LONG(APIENTRY*)(const Close*);
    const HMODULE gdi=single_module::LoadSystemModule(L"gdi32.dll");
    const auto open=reinterpret_cast<OpenFn>(GetProcAddress(gdi,"D3DKMTOpenAdapterFromLuid"));
    const auto query=reinterpret_cast<QueryFn>(GetProcAddress(gdi,"D3DKMTQueryAdapterInfo"));
    const auto close=reinterpret_cast<CloseFn>(GetProcAddress(gdi,"D3DKMTCloseAdapter"));
    OpenFromLuid opened{luid,0};
    if(!open||!query||!close||open(&opened)<0||!opened.adapter)return false;
    UmdName name{3,{}}; // KMTUMDVERSION_DX12
    const Query request{opened.adapter,1,&name,sizeof(name)}; // KMTQAITYPE_UMDRIVERNAME
    const LONG status=query(&request);
    const Close closing{opened.adapter};close(&closing);
    name.name[MAX_PATH-1]=0;
    const wchar_t* leaf=wcsrchr(name.name,L'\\');
    if(status<0||!leaf)return false;
    directory.assign(name.name,static_cast<size_t>(leaf-name.name)+1);
    return true;
}
bool PrivateListCallable(HMODULE owner) {
    if(NativeRuntime(owner)&&(owner==verifiedAgility[0]||owner==verifiedAgility[1]))return true;
    if(verifiedListDriver==owner)return true;
    if(verifiedListDriver)return false;
    // Agility's optimized per-device list table forwards some public methods
    // straight to the UMD; they are ordinary D3D12 command-list methods that
    // DOTS only forwards to. Admit NVIDIA's D3D12 UMD independent of release:
    // the image from the driver package the kernel registered for this
    // device's adapter, matching its file, at release 617.14 or later. Native
    // factory/Reset origin and exact D3D12 device identity are proven first.
    // This does not admit driver-owned device/queue tables or modify its code.
    wchar_t path[MAX_PATH]{},system[MAX_PATH]{};
    constexpr wchar_t driverPrefix[]=L"\\DriverStore\\FileRepository\\";
    const auto n=GetModuleFileNameW(owner,path,_countof(path));
    const auto systemSize=GetSystemDirectoryW(system,_countof(system));
    if(!n||n>=_countof(path)||!systemSize||systemSize>=_countof(system)||n<=systemSize
        ||n-systemSize<=_countof(driverPrefix)-1
        ||_wcsnicmp(path,system,systemSize)||_wcsnicmp(path+systemSize,driverPrefix,_countof(driverPrefix)-1))return false;
    const auto* leaf=wcsrchr(path,L'\\');
    if(!leaf||_wcsicmp(leaf+1,L"nvwgf2umx.dll"))return false;
    std::wstring directory;std::array<WORD,4> version{};const char* failure=nullptr;
    try {
        LUID luid{};
        if(!DeviceLuid(luid)||!AdapterDriverDirectory(luid,directory))failure="adapter driver registration unavailable";
        else if(_wcsnicmp(path,directory.c_str(),directory.size())||wcschr(path+directory.size(),L'\\'))failure="not in the adapter's registered driver package";
        else if(!DriverFileVersion(path,version))failure="file version unavailable";
        else if(version<kMinimumDriverFile)failure="older than 617.14";
        else if(!VerifiedRuntimeImage(owner,path,nullptr,512*1024*1024,0))failure="loaded image differs from its file";
    } catch(...) {failure="identity check failed";}
    if(failure) {
        static std::atomic_flag logged=ATOMIC_FLAG_INIT;
        if(!logged.test_and_set()) {
            wchar_t message[768]{};
            swprintf_s(message,L"WITCHER_DOTS native_list_driver=rejected reason=%S module=%s registered=%s",failure,path,directory.c_str());
            single_module::Log(message);
        }
        return false;
    }
    verifiedListDriver=owner;
    const unsigned release=(version[2]%10)*10000u+version[3];
    wchar_t message[640]{};
    swprintf_s(message,L"WITCHER_DOTS native_list_driver=verified release=%u.%02u file=%u.%u.%u.%u module=%s",
        release/100,release%100,version[0],version[1],version[2],version[3],path);
    single_module::Log(message);return true;
}
bool ImageMethod(void* value,HMODULE& owner,bool data=false) {
    MEMORY_BASIC_INFORMATION m{};
    return value&&VirtualQuery(value,&m,sizeof(m))==sizeof(m)&&m.State==MEM_COMMIT&&m.Type==MEM_IMAGE
        &&!(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        &&(data?!(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
            :!!(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        &&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(value),&owner)
        &&owner==m.AllocationBase;
}
void** Table(void* object) {
    void** table{};return Read(object,0,table)?table:nullptr;
}
std::atomic<uint32_t> ambiguousCanonical{};
// Hot path of every instrumented call: the runtime is invoking a method on a
// live COM object, so its table pointer is read directly.
template<class Fn> Fn Original(void* object,size_t index,Kind kind=Kind::List) noexcept {
    void** const table=*static_cast<void***>(object);
    if(const auto* entry=FindPublication(reinterpret_cast<uintptr_t>(table+index)))
        if(void* original=entry->original.load(std::memory_order_acquire))return reinterpret_cast<Fn>(original);
    return reinterpret_cast<Fn>(canonical[static_cast<size_t>(kind)][index].load(std::memory_order_acquire));
}
// Cost diagnostics for the instrumented list methods: every call is counted on
// its own thread, and one call in 32 per thread is timed, splitting DOTS's own
// work (lookup and bookkeeping) from the runtime's call. Per-thread totals
// reach the shared counters every 1024 calls, so the hot path adds no shared
// cache-line traffic. QPC is 10 MHz: single samples quantize to 100 ns, the
// sums over many samples are unbiased.
// A thread preempted inside a sampled call would add milliseconds to one
// sample: each sample's DOTS share is capped at 20 us and such samples are
// counted (a list's first-Reset table validation is reported separately).
std::atomic<uint64_t> listCalls{},listSamples{},listOverheadTicks{},listOriginalTicks{},listOutliers{};
struct ListCounter { uint64_t calls{},samples{},overhead{},original{},outliers{};uint32_t tick{}; };
thread_local ListCounter listCounter{};
int64_t Qpc() noexcept {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
int64_t SampleCapTicks() noexcept {
    static const int64_t cap=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return std::max<int64_t>(1,f.QuadPart/50000);}();
    return cap;
}
class ListProbe {
    int64_t start{},originalStart{},originalTicks{};bool sampled{};
public:
    ListProbe() noexcept {auto& c=listCounter;++c.calls;if((++c.tick&31)==0) {sampled=true;start=Qpc();}}
    void Before() noexcept {if(sampled)originalStart=Qpc();}
    void After() noexcept {if(sampled)originalTicks=Qpc()-originalStart;}
    ~ListProbe() {
        auto& c=listCounter;
        if(sampled) {
            const int64_t total=Qpc()-start,cap=SampleCapTicks();
            const int64_t own=total>originalTicks?total-originalTicks:0;
            ++c.samples;c.original+=static_cast<uint64_t>(std::min(originalTicks,cap));c.overhead+=static_cast<uint64_t>(std::min(own,cap));
            if(own>cap||originalTicks>cap)++c.outliers;
        }
        if(c.calls>=1024) {
            listCalls.fetch_add(c.calls,std::memory_order_relaxed);listSamples.fetch_add(c.samples,std::memory_order_relaxed);
            listOverheadTicks.fetch_add(c.overhead,std::memory_order_relaxed);listOriginalTicks.fetch_add(c.original,std::memory_order_relaxed);
            listOutliers.fetch_add(c.outliers,std::memory_order_relaxed);
            c.calls=c.samples=c.overhead=c.original=c.outliers=0;
        }
    }
    ListProbe(const ListProbe&)=delete;ListProbe& operator=(const ListProbe&)=delete;
};
// Cost of one QueryPerformanceCounter call in ticks, measured once: a sampled
// call's overhead includes two of them (start/Before and After/end).
double QpcCostTicks() noexcept {
    static const double cost=[] {
        const int64_t start=Qpc();int64_t last=start;
        for(int i=0;i<4096;++i)last=Qpc();
        return static_cast<double>(last-start)/4096.0;
    }();
    return cost;
}
// ExecuteCommandLists on tracked queues: calls, lists, submissions carrying
// converted hair, and DOTS's own part of the hook (lease fences and Signal).
std::atomic<uint64_t> executeCalls{},executeLists{},executeHair{},executeOverheadTicks{};
constexpr size_t kList4Methods=77; // IUnknown through DispatchRays (ID3D12GraphicsCommandList4).
constexpr std::array<size_t,14> kTrackedListMethods{9,10,11,25,26,28,29,31,33,35,37,39,41,75};
// Opt-in GPU crash diagnostics (crash report requested before the device is
// created): Dispatch is also instrumented, so a device-removal report names
// the pipeline, thread groups and calling module of the dispatch that hung.
constexpr std::array<size_t,15> kDiagnosticListMethods{9,10,11,14,25,26,28,29,31,33,35,37,39,41,75};
// rtxhair.1 universal tracking: BuildRaytracingAccelerationStructure (72) as well, for the
// hair instances' culling flag (SetInstancePatch).
constexpr std::array<size_t,15> kUniversalListMethods{9,10,11,25,26,28,29,31,33,35,37,39,41,72,75};
std::atomic<bool> dispatchDiagnostics{};
std::atomic<bool> universalTracking{};
std::span<const size_t> TrackedListMethods() noexcept {
    if(universalTracking.load(std::memory_order_relaxed))return kUniversalListMethods;
    if(dispatchDiagnostics.load(std::memory_order_relaxed))return kDiagnosticListMethods;
    return kTrackedListMethods;
}
const std::array<void*,kList4Methods>& ListHooks();
struct PrivateListTable { void** table{};void* allocation{};DWORD protection{};std::array<void*,kList4Methods> native{}; };
// Callables proven to belong to the verified Agility runtime or the adapter's
// registered NVIDIA UMD. ImageMethod pins the owning module, so the proof holds
// for the process; every Agility list table carries the same driver entries,
// so validating a new list's table is lookups, not address-space queries
// (about 2.4 ms per new list before). Caller holds publicationLock.
constexpr size_t kCallableSlots=1024;
std::array<std::atomic<const void*>,kCallableSlots> callableKeys{};
size_t callableCount{};
size_t CallableHash(const void* key) noexcept {return static_cast<size_t>(((reinterpret_cast<uintptr_t>(key)>>4)*0x9E3779B97F4A7C15ull)>>54);}
bool PrivateCallable(void* method,HMODULE& owner) {
    owner=nullptr;
    if(!method)return false;
    size_t free=kCallableSlots;
    for(size_t n=0,i=CallableHash(method);n<kCallableSlots;++n,i=(i+1)&(kCallableSlots-1)) {
        const void* key=callableKeys[i].load(std::memory_order_acquire);
        if(key==method)return true;
        if(!key) {free=i;break;}
    }
    if(!ImageMethod(method,owner)||!PrivateListCallable(owner))return false;
    if(free<kCallableSlots&&callableCount<kCallableSlots/2) {callableKeys[free].store(method,std::memory_order_release);++callableCount;}
    return true;
}
bool PrivateTableMatches(const PrivateListTable& permit,void* object) {
    MEMORY_BASIC_INFORMATION memory{};
    return permit.table&&Table(object)==permit.table
        &&VirtualQuery(permit.table,&memory,sizeof(memory))==sizeof(memory)
        &&memory.State==MEM_COMMIT&&memory.Type==MEM_PRIVATE&&memory.AllocationBase==permit.allocation
        &&memory.Protect==permit.protection&&memory.Protect==PAGE_READWRITE
        &&reinterpret_cast<uintptr_t>(permit.table)>=reinterpret_cast<uintptr_t>(memory.BaseAddress)
        &&memory.RegionSize>=kList4Methods*sizeof(void*)
        &&reinterpret_cast<uintptr_t>(permit.table)-reinterpret_cast<uintptr_t>(memory.BaseAddress)
            <=memory.RegionSize-kList4Methods*sizeof(void*);
}
bool PrivateNativeList(ID3D12GraphicsCommandList4* list,PrivateListTable& permit,void** before) {
    // Only called for a retained interface returned by our native CreateList
    // hooks, or immediately after its original native Reset returns. The exact
    // device identity is checked by the caller. This is not proxy discovery.
    permit.table=Table(list);MEMORY_BASIC_INFORMATION memory{};
    if(!permit.table||reinterpret_cast<uintptr_t>(permit.table)%alignof(void*)
        ||VirtualQuery(permit.table,&memory,sizeof(memory))!=sizeof(memory))return false;
    permit.allocation=memory.AllocationBase;permit.protection=memory.Protect;
    if(!PrivateTableMatches(permit,list))return false;
    std::lock_guard guard(publicationLock);
    const auto& hooks=ListHooks();
    for(size_t index=0;index<kList4Methods;++index) {
        void* method{};HMODULE owner{};
        // PrivateTableMatches proved the whole table committed private RW.
        if(!ReadFast(permit.table,index*sizeof(void*),method))return false;
        // An already instrumented table resolves its own publication. A table
        // Agility copied from the hooked image table resolves the image
        // original (optionally the exact previous table's binding).
        if(hooks[index]&&method==hooks[index]) {
            const auto* own=FindPublication(reinterpret_cast<uintptr_t>(permit.table+index));
            const auto* donor=before?FindPublication(reinterpret_cast<uintptr_t>(before+index)):nullptr;
            void* forward=own?own->original.load(std::memory_order_acquire)
                :donor?donor->original.load(std::memory_order_acquire)
                :canonical[static_cast<size_t>(Kind::List)][index].load(std::memory_order_acquire);
            if(!forward)return false;
            method=forward;
        }
        if(!PrivateCallable(method,owner)) {
            wchar_t path[MAX_PATH]{};GetModuleFileNameW(owner,path,_countof(path));
            return false;
        }
        permit.native[index]=method;
    }
    return PrivateTableMatches(permit,list);
}
// With a permit (a private list table), the caller proves the whole table's
// region (PrivateTableMatches) before and after publishing all of its slots.
bool Publish(void* object,size_t index,void* hook,Kind kind,const PrivateListTable* permit=nullptr) {
    void** table{};
    if(permit) {if(!ReadFast(object,0,table))return false;}
    else table=Table(object);
    if(!table||index>=kCanonicalSlots)return false;
    const auto slot=reinterpret_cast<uintptr_t>(table+index);
    if(slot%sizeof(void*))return false;
    std::lock_guard guard(publicationLock);
    if(permit&&(index>=kList4Methods||table!=permit->table))return false;
    if(const auto* existing=FindPublication(slot))
        return existing->hook==hook&&protected_pointer::ReadPointer(slot)==hook;
    HMODULE dataOwner{},callOwner{};void* previous{};
    if(!(permit?ReadFast(table,index*sizeof(void*),previous):Read(table,index*sizeof(void*),previous))
        ||(!permit&&!ImageMethod(table+index,dataOwner,true)))return false;
    void* forward=permit?permit->native[index]:previous;
    if((permit&&previous!=forward&&previous!=hook)||(permit?!PrivateCallable(forward,callOwner):!ImageMethod(forward,callOwner)))return false;
    // Reject addon/proxy-owned tables. Public unwrapping establishes the native
    // object first; only system D3D12 or the exact verified Agility runtime's
    // methods are instrumented. Foreign callable/table owners remain rejected.
    if(!permit&&(!NativeRuntime(dataOwner)||!NativeRuntime(callOwner)))return false;
    // A private table's protection (committed private PAGE_READWRITE covering
    // every slot) was proven by PrivateTableMatches above and is re-proven
    // after the exchange; an image table slot is queried individually.
    DWORD protection{};
    if(permit)protection=permit->protection;
    else if(!protected_pointer::QueryProtection(slot,protection,sizeof(void*)))return false;
    auto* entry=NewPublication(slot);if(!entry)return false;
    if(!permit) {
        // Image tables seed the fallback for copies of this table (see canonical).
        auto& fallback=canonical[static_cast<size_t>(kind)][index];void* expected{};
        if(!fallback.compare_exchange_strong(expected,forward,std::memory_order_acq_rel)&&expected!=forward)
            ambiguousCanonical.fetch_add(1,std::memory_order_relaxed);
    }
    entry->hook=hook;entry->protection=protection;entry->index=index;entry->original.store(forward,std::memory_order_release);
    entry->slot.store(slot,std::memory_order_release);
    bool published=false;
    if(permit) {
        // Expected-value exchange with readback; the table stays private RW.
        const auto observed=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(table+index),hook,previous);
        published=observed==previous&&protected_pointer::ReadPointer(slot)==hook;
    } else {
        published=ExchangeMethod(table+index,previous,hook,protection);
    }
    entry->protection=protection;
    // Never discard a forwarding binding after even ambiguous publication.
    return published&&(permit||protected_pointer::ProtectionMatches(slot,protection,sizeof(void*)));
}
struct RootInfo { uint32_t count{};std::array<D3D12_ROOT_PARAMETER_TYPE,64> type{};std::array<uint32_t,64> constants{};ComPtr<ID3D12RootSignature> keep; };
enum class ValueKind { None,Table,Constants,Cbv,Srv,Uav };
struct RootValue { ValueKind kind{};uint64_t value{};uint64_t wordsKnown{};std::array<uint32_t,64> words{}; };
struct Bindings {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12StateObject> rayPipeline;
    std::array<RootValue,64> values{};
    uint64_t used{}; // Root indices with a recorded value; keeps Reset cheap.
    std::array<ComPtr<ID3D12DescriptorHeap>,2> heaps{};
    uint32_t heapCount{};
    bool pipelineKnown{},rootKnown{},valid{true};
    void ClearValues() noexcept {
        for(uint64_t mask=used;mask;mask&=mask-1)values[static_cast<size_t>(std::countr_zero(mask))]={};
        used=0;
    }
    void Clear() noexcept {
        ClearValues();root.Reset();pipeline.Reset();rayPipeline.Reset();
        for(auto& heap:heaps)heap.Reset();
        heapCount=0;pipelineKnown=rootKnown=false;valid=true;
    }
};
// Identity only. D3D12 lists hold no resource references; an owning reference
// here kept swapchain buffers alive, failing ResizeBuffers (then removing the
// device with DXGI_ERROR_ACCESS_DENIED on the next stale back-buffer write).
struct ResourceState {ID3D12Resource* resource{};D3D12_RESOURCE_STATES state{};bool known{};};
// Dispatch diagnostics: one recording's dispatches in order (DRED counts each
// as a Dispatch breadcrumb, including the DOTS converter's own).
struct DispatchRecord { const void* pso{};const void* root{};uint32_t x{},y{},z{},injected{}; };
struct DispatchLog { std::atomic<uint32_t> count{};uint64_t generation{};std::array<DispatchRecord,2048> records{}; };
struct ListState {
    ComPtr<ID3D12GraphicsCommandList4> keep;
    std::atomic<bool> reclaimed{}; // A buffer of the current recording was returned (C().lock).
    void** table{};
    std::array<void*,4> identityMethods{};
    std::unique_ptr<Bindings> bindings=std::make_unique<Bindings>();
    std::array<ResourceState,512> barriers{};
    size_t barrierCursor{};
    std::vector<size_t> leases; // C().lock
    std::atomic<bool> hasLeases{};
    uint64_t generation{1};
    std::atomic<bool> open{};
    std::atomic<uint32_t> instrumentFailures{};
    std::atomic<bool> tableVerified{}; // VerifyListTable passed for the current table.
    std::vector<void**> privateTables; // Agility tables embedded in this list (publications leave with it).
    // Dispatch diagnostics: the latest three recordings (a submitted list may
    // be reset and recorded again while its previous recording still runs).
    std::array<std::atomic<DispatchLog*>,3> dispatchLogs{};
    std::atomic<uint32_t> dispatchCursor{};
    ListState()=default;
    ListState(const ListState&)=delete;ListState& operator=(const ListState&)=delete;
    ~ListState() {for(auto& log:dispatchLogs)delete log.load(std::memory_order_relaxed);}
};
struct QueueState { ComPtr<ID3D12CommandQueue> keep;ComPtr<ID3D12Fence> fence;uint64_t next{}; };
struct Lease {
    ComPtr<ID3D12Resource> vertices,positions,indices,blas,scratch;
    uint64_t capacity{},fenceValue{},lastUsed{};
    ID3D12CommandQueue* queue{};
    ID3D12GraphicsCommandList4* list{};
    uint64_t generation{};
    bool poisoned{};
};
struct Association {
    void* owner{};
    ComPtr<ID3D12Resource> blas,positions,indices;
    uint64_t address{},bytes{},scratchBytes{};
    uint32_t segments{},vertices{},flags{},refits{};
};
struct Context {
    std::mutex lock;
    ComPtr<ID3D12Device5> device;
    ComPtr<IUnknown> identity;
    // Successful device-proxy proofs are immutable and retain both identities.
    // The key is published last, preventing pointer reuse or partial reads.
    struct DeviceAlias {
        ComPtr<IUnknown> proxy,native;
        std::atomic<IUnknown*> key{};
    };
    std::array<DeviceAlias,8> deviceAliases{};
    std::mutex deviceAliasLock;
    ComPtr<ID3D12RootSignature> converterRoot;
    ComPtr<ID3D12PipelineState> converterPso;
    std::atomic<ShaderCache*> shaders{};
    std::atomic<ShaderRewrite> rewrite{}; // rtxhair.1 universal hair (InitializeTracking); never with shaders
    std::unordered_map<ID3D12GraphicsCommandList4*,std::unique_ptr<ListState>> lists;
    std::unordered_map<ID3D12CommandQueue*,QueueState> queues;
    std::unordered_map<ID3D12RootSignature*,RootInfo> roots;
    std::array<Lease,kMaxLeases> leases{};
    std::array<Association,kMaxOwners> associations{};
    RuntimeStats stats{};
    uint64_t lastSweep{},lastRebuild{},lastListSweep{};
    // Converted-vertex pool limits (BuildTriangles); only the harness lowers them.
    uint64_t poolBudget{kGeometryBudget},poolCeiling{kGeometryCeiling},vramReserve{kVramReserve};
    ComPtr<IDXGIAdapter3> memory; // The prepared device's adapter (OS video-memory budget).
};
Context& C() { static auto* const value=new Context;return *value; }
// Hair-build cost split for the overlay: waiting for the runtime lock versus
// work done while holding it (conversion recording, budgets, eviction).
std::atomic<uint64_t> buildLockWaitTicks{},buildLockHeldTicks{};
// List table changes seen at Reset (each re-validates the table) and their cost.
std::atomic<uint64_t> tableChanges{},tableFailures{},tableChangeTicks{};
struct BuildLock {
    std::unique_lock<std::mutex> lock;LARGE_INTEGER acquired{};
    explicit BuildLock(std::mutex& mutex) {
        LARGE_INTEGER start{};QueryPerformanceCounter(&start);
        lock=std::unique_lock(mutex);QueryPerformanceCounter(&acquired);
        buildLockWaitTicks.fetch_add(static_cast<uint64_t>(acquired.QuadPart-start.QuadPart),std::memory_order_relaxed);
    }
    ~BuildLock() {
        LARGE_INTEGER end{};QueryPerformanceCounter(&end);
        buildLockHeldTicks.fetch_add(static_cast<uint64_t>(end.QuadPart-acquired.QuadPart),std::memory_order_relaxed);
    }
};
bool DeviceLuid(LUID& luid) {if(!C().device)return false;luid=C().device->GetAdapterLuid();return true;}
thread_local bool injecting{};
std::atomic<bool> removalReportArmed{};
std::atomic<uint64_t> settingChangeTick{};std::atomic<bool> settingChangeOn{};
bool OwnedDevice(ID3D12Device* device) {
    if(!device)return false;ComPtr<IUnknown> id;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&id)))||!id)return false;
    auto& ctx=C();
    if(id.Get()==ctx.identity.Get())return true;
    const auto known=[&] {
        for(const auto& alias:ctx.deviceAliases)
            if(alias.key.load(std::memory_order_acquire)==id.Get()&&alias.native.Get()==ctx.identity.Get())return true;
        return false;
    };
    if(known())return true;
    // ReShade leaves resources native but hooks GetDevice to return its proxy.
    // Compare the proven native COM identity, never just the adapter/LUID.
    // Resolution is outside the alias lock: wrapper calls can take their own
    // locks, and the fallback creates a native fence on older wrappers.
    ComPtr<ID3D12Device5> native;ComPtr<IUnknown> nativeId;
    if(!ResolveNativeDevice(device,native)||FAILED(native.As(&nativeId))
        ||!nativeId||nativeId.Get()!=ctx.identity.Get())return false;
    // Retain once rather than repeating unwrap/fence discovery per hair update.
    // No COM calls under this lock; empty entries receive already-retained refs.
    std::lock_guard lock(ctx.deviceAliasLock);
    if(known())return true;
    for(auto& alias:ctx.deviceAliases)if(!alias.key.load(std::memory_order_relaxed)) {
        auto* key=id.Get();alias.proxy=std::move(id);alias.native=std::move(nativeId);
        alias.key.store(key,std::memory_order_release);break;
    }
    // A full cache changes only cost: this call still proved exact ownership.
    return true;
}
template<class T> bool Child(T* child) {
    ComPtr<ID3D12Device> device;
    return child&&SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&device)))&&OwnedDevice(device.Get());
}
template<class T> bool Unwrap(IUnknown* input,ComPtr<T>& output) {
    auto** const table=Table(input);
    return input&&Readable(input,sizeof(void*))&&Readable(table,3*sizeof(void*))
        &&single_overlay::native::PinInterface(input,2)&&single_overlay::native::Unwrap(input,output);
}
// Lock-free index of retained lists (removed only once the game destroyed
// the list). Recording hooks run on the list's own recording thread, which
// D3D12 requires to be exclusive, so per-list bindings/barriers need no global
// lock; leases still use C().lock.
constexpr size_t kListSlots=4096;
std::array<std::atomic<const void*>,kListSlots> listKeys{};
std::array<ListState*,kListSlots> listValues{};
size_t ListHash(const void* list) noexcept {return static_cast<size_t>(((reinterpret_cast<uintptr_t>(list)>>4)*0x9E3779B97F4A7C15ull)>>52);}
bool Removed(const void* key) noexcept {return reinterpret_cast<uintptr_t>(key)==kRemovedKey;}
// Caller holds the index's writer lock. Marks `key` removed and clears the
// removed run that now ends at an empty slot (no probe path crosses it).
template<size_t N> void RemoveKey(std::array<std::atomic<const void*>,N>& keys,size_t start,const void* key) noexcept {
    for(size_t n=0,i=start;n<N;++n,i=(i+1)&(N-1)) {
        const void* observed=keys[i].load(std::memory_order_relaxed);
        if(!observed)return;
        if(observed!=key)continue;
        keys[i].store(reinterpret_cast<const void*>(kRemovedKey),std::memory_order_release);
        if(keys[(i+1)&(N-1)].load(std::memory_order_relaxed))return;
        for(size_t j=i;Removed(keys[j].load(std::memory_order_relaxed));j=(j-1)&(N-1))keys[j].store(nullptr,std::memory_order_release);
        return;
    }
}
ListState* List(const void* list) noexcept {
    for(size_t n=0,i=ListHash(list);n<kListSlots;++n,i=(i+1)&(kListSlots-1)) {
        const void* key=listKeys[i].load(std::memory_order_acquire);
        if(key==list)return listValues[i];
        if(!key)return nullptr;
    }
    return nullptr;
}
bool IndexList(const void* list,ListState* state) noexcept { // Caller holds C().lock.
    for(size_t n=0,i=ListHash(list);n<kListSlots;++n,i=(i+1)&(kListSlots-1)) {
        const void* key=listKeys[i].load(std::memory_order_relaxed);
        if(!key||Removed(key)) {listValues[i]=state;listKeys[i].store(list,std::memory_order_release);return true;}
    }
    return false;
}
// Dispatch diagnostics: the code that issued each compute pipeline's first
// dispatch (insert-only; a destroyed pipeline's address may be reused).
constexpr size_t kCallerSlots=8192;
std::array<std::atomic<const void*>,kCallerSlots> callerKeys{};
std::array<const void*,kCallerSlots> callerValues{};
std::atomic<uint32_t> callerCount{};
std::mutex callerLock;
size_t CallerHash(const void* key) noexcept {return static_cast<size_t>(((reinterpret_cast<uintptr_t>(key)>>4)*0x9E3779B97F4A7C15ull)>>51);}
const void* FindCaller(const void* pso) noexcept {
    for(size_t n=0,i=CallerHash(pso);n<kCallerSlots;++n,i=(i+1)&(kCallerSlots-1)) {
        const void* key=callerKeys[i].load(std::memory_order_acquire);
        if(key==pso)return callerValues[i];
        if(!key)return nullptr;
    }
    return nullptr;
}
struct ImageRange {uintptr_t begin{},end{};};
ImageRange RangeOf(HMODULE module) noexcept {
    if(!module)return {};
    const auto base=reinterpret_cast<uintptr_t>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    return {base,base+nt->OptionalHeader.SizeOfImage};
}
// First stack frame in the game executable, else the first outside this
// module (a wrapper or NVIDIA component that recorded the dispatch).
const void* IssuingCode() noexcept {
    static const ImageRange game=RangeOf(GetModuleHandleW(nullptr));
    static const ImageRange own=[] {
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&callerCount),&module);
        return RangeOf(module);
    }();
    void* frames[32]{};const USHORT count=RtlCaptureStackBackTrace(1,32,frames,nullptr);
    const void* outside=nullptr;
    for(USHORT i=0;i<count;++i) {
        const auto at=reinterpret_cast<uintptr_t>(frames[i]);
        if(at>=game.begin&&at<game.end&&game.begin!=own.begin)return frames[i];
        if(!outside&&(at<own.begin||at>=own.end))outside=frames[i];
    }
    return outside?outside:(count?frames[0]:nullptr);
}
void RememberCaller(const void* pso) noexcept {
    if(!pso||FindCaller(pso)||callerCount.load(std::memory_order_relaxed)>=kCallerSlots*3/4)return;
    try {
        std::lock_guard guard(callerLock);
        for(size_t n=0,i=CallerHash(pso);n<kCallerSlots;++n,i=(i+1)&(kCallerSlots-1)) {
            const void* key=callerKeys[i].load(std::memory_order_relaxed);
            if(key==pso)return;
            if(key)continue;
            callerValues[i]=IssuingCode();callerKeys[i].store(pso,std::memory_order_release);
            callerCount.fetch_add(1,std::memory_order_relaxed);return;
        }
    } catch(...) {}
}
// Recording thread (exclusive per list). Logs are fixed arrays, never moved
// while the list is tracked, so the removal report can read them.
void RotateDispatchLog(ListState& s) noexcept {
    const uint32_t next=(s.dispatchCursor.load(std::memory_order_relaxed)+1)%3;
    if(auto* log=s.dispatchLogs[next].load(std::memory_order_relaxed)) {
        log->count.store(0,std::memory_order_relaxed);log->generation=s.generation;
    }
    s.dispatchCursor.store(next,std::memory_order_release);
}
void LogDispatch(ListState& s,UINT x,UINT y,UINT z) noexcept {
    auto& slot=s.dispatchLogs[s.dispatchCursor.load(std::memory_order_relaxed)];
    auto* log=slot.load(std::memory_order_relaxed);
    if(!log) {
        log=new(std::nothrow) DispatchLog;if(!log)return;
        log->generation=s.generation;slot.store(log,std::memory_order_release);
    }
    const uint32_t n=log->count.load(std::memory_order_relaxed);
    if(n<log->records.size()) {
        const void* pso=injecting?static_cast<const void*>(C().converterPso.Get()):static_cast<const void*>(s.bindings->pipeline.Get());
        const void* root=injecting?static_cast<const void*>(C().converterRoot.Get()):static_cast<const void*>(s.bindings->root.Get());
        log->records[n]={pso,root,x,y,z,injecting?1u:0u};
        if(!injecting)RememberCaller(pso);
    }
    log->count.store(n+1,std::memory_order_release);
}
// Root signatures created on this device (retained until the game destroyed
// them): read on every SetComputeRootSignature without the global lock.
constexpr size_t kRootSlots=2048;
std::array<std::atomic<const void*>,kRootSlots> rootKeys{};
std::array<const RootInfo*,kRootSlots> rootValues{};
size_t RootHash(const void* root) noexcept {return static_cast<size_t>(((reinterpret_cast<uintptr_t>(root)>>4)*0x9E3779B97F4A7C15ull)>>53);}
const RootInfo* FindRoot(const void* root) noexcept {
    if(!root)return nullptr;
    for(size_t n=0,i=RootHash(root);n<kRootSlots;++n,i=(i+1)&(kRootSlots-1)) {
        const void* key=rootKeys[i].load(std::memory_order_acquire);
        if(key==root)return rootValues[i];
        if(!key)return nullptr;
    }
    return nullptr;
}
bool IndexRoot(const void* root,const RootInfo* info) noexcept { // Caller holds C().lock.
    for(size_t n=0,i=RootHash(root);n<kRootSlots;++n,i=(i+1)&(kRootSlots-1)) {
        const void* key=rootKeys[i].load(std::memory_order_relaxed);
        if(!key||Removed(key)) {rootValues[i]=info;rootKeys[i].store(root,std::memory_order_release);return true;}
    }
    return false;
}
// After InstrumentList: the retained list's table is image data or a private
// region it proved, so plain guarded reads suffice.
bool RememberListTable(ListState& list) {
    if(!ReadFast(list.keep.Get(),0,list.table)||!list.table)return false;
    constexpr std::array<size_t,4> indices{0,1,2,7};
    for(size_t i=0;i<indices.size();++i)if(!ReadFast(list.table,indices[i]*sizeof(void*),list.identityMethods[i]))return false;
    return true;
}
// Full check, run when a list's table is (re)published: identity methods,
// every tracked slot holding its hook and the slot protection recorded at
// publication. Its result gates the cheap per-build check below.
bool VerifyListTable(ListState& list) {
    list.tableVerified.store(false,std::memory_order_release);
    void** current{};
    if(!list.table||!ReadFast(list.keep.Get(),0,current)||current!=list.table)return false;
    constexpr std::array<size_t,4> indices{0,1,2,7};
    for(size_t i=0;i<indices.size();++i) {
        void* method{};if(!ReadFast(list.table,indices[i]*sizeof(void*),method)||method!=list.identityMethods[i])return false;
    }
    // An embedded (private) table lies in one committed private region: a
    // single query proves the protection of every slot in it.
    MEMORY_BASIC_INFORMATION region{};
    const bool privateRegion=VirtualQuery(list.table,&region,sizeof(region))==sizeof(region)
        &&region.State==MEM_COMMIT&&region.Type==MEM_PRIVATE;
    const auto regionBegin=reinterpret_cast<uintptr_t>(region.BaseAddress),regionEnd=regionBegin+region.RegionSize;
    std::lock_guard guard(publicationLock);
    for(const auto index:TrackedListMethods()) {
        const auto slot=reinterpret_cast<uintptr_t>(list.table+index);
        const auto* entry=FindPublication(slot);
        if(!entry||protected_pointer::ReadPointer(slot)!=entry->hook)return false;
        if(privateRegion&&slot>=regionBegin&&slot+sizeof(void*)<=regionEnd) {
            if(region.Protect!=entry->protection)return false;
        } else if(!protected_pointer::ProtectionMatches(slot,entry->protection,sizeof(void*)))return false;
    }
    list.tableVerified.store(true,std::memory_order_release);
    return true;
}
// Per hair build: the list still uses its verified table, which still holds
// the identity methods and every hook. Plain reads of this retained list's own
// table and the lock-free publication index; no address-space query or lock.
bool CurrentListTable(const ListState& list) {
    if(!list.tableVerified.load(std::memory_order_acquire)||!list.table||Table(list.keep.Get())!=list.table)return false;
    constexpr std::array<size_t,4> indices{0,1,2,7};
    for(size_t i=0;i<indices.size();++i)if(list.table[indices[i]]!=list.identityMethods[i])return false;
    for(const auto index:TrackedListMethods()) {
        const auto* entry=FindPublication(reinterpret_cast<uintptr_t>(list.table+index));
        if(!entry||list.table[index]!=entry->hook)return false;
    }
    return true;
}
// Each tracked native list stores its own address as private data. Wrappers
// without a public unwrap (ReShade before 6.7) forward GetPrivateData to the
// list they wrap, which identifies it; only a tracked list of this device counts.
constexpr GUID kNativeListTag={0x3b0e6f52,0x9d17,0x4c3a,{0x8e,0x61,0x2f,0x74,0xa9,0xc5,0x10,0xd8}};
ListState* TaggedList(IUnknown* wrapper) {
    ComPtr<ID3D12Object> object;void* tagged{};UINT size=sizeof(tagged);
    if(!wrapper||FAILED(wrapper->QueryInterface(IID_PPV_ARGS(&object)))
        ||FAILED(object->GetPrivateData(kNativeListTag,&size,&tagged))||size!=sizeof(tagged))return nullptr;
    auto* known=List(tagged);
    return known&&CurrentListTable(*known)&&known->bindings->valid&&Child(known->keep.Get())?known:nullptr;
}
bool UnwrapList(IUnknown* input,ComPtr<ID3D12GraphicsCommandList4>& output) {
    // Generic public unwrapping deliberately rejects arbitrary heap vtables.
    // A native list already retained by our device hooks has stronger origin
    // and lifetime proof; accept only its current published table generation.
    if(!input)return false;
    std::array<ComPtr<IUnknown>,4> visited;
    ComPtr<IUnknown> current;
    for(size_t depth=0;depth<visited.size();++depth) {
        if(auto* known=List(input)) {
            if(!CurrentListTable(*known)||!known->bindings->valid||!Child(known->keep.Get()))return false;
            output=known->keep;return true;
        }
        // A forwarding wrapper (Streamline, ReShade) of a tracked list: two
        // calls instead of the full public unwrapping walk on every build.
        if(depth==0)if(auto* tagged=TaggedList(input)) {output=tagged->keep;return true;}
        if(!Readable(input,sizeof(void*))||!Readable(Table(input),3*sizeof(void*))
            ||!single_overlay::native::PinInterface(input,2))return false;
        current=input;
        if(FAILED(current.As(&visited[depth])))return false;
        for(size_t prior=0;prior<depth;++prior)if(visited[prior].Get()==visited[depth].Get())return false;
        constexpr GUID base={0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
        constexpr GUID reshade={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
        ComPtr<IUnknown> native;auto result=current->QueryInterface(base,reinterpret_cast<void**>(native.GetAddressOf()));
        if(result==E_NOINTERFACE&&!native)result=current->QueryInterface(reshade,reinterpret_cast<void**>(native.GetAddressOf()));
        if(result==E_NOINTERFACE) {
            if(native)return false;
            if(auto* tagged=TaggedList(current.Get())) {output=tagged->keep;return true;}
            return SUCCEEDED(current.As(&output))&&output;
        }
        if(result!=S_OK||!native)return false;
        current=std::move(native);input=current.Get();
    }
    return false;
}
void Reject() { ++C().stats.rejected; }
// Caller holds C().lock. Stops new conversions for the rest of the process;
// the reason that first stopped them is logged so the log names it.
std::atomic<uint32_t> lostLogs{};
void Lost(const char* reason) {
    if(!C().stats.lost&&lostLogs.fetch_add(1,std::memory_order_relaxed)<8) {
        wchar_t line[256]{};
        _snwprintf_s(line,_TRUNCATE,L"WITCHER_DOTS tracking lost: %S; new conversions stopped",reason);single_module::Log(line);
    }
    C().stats.lost=true;
}
bool Available(const Lease& lease) {
    if(lease.list||lease.poisoned)return false;
    if(!lease.queue)return true;
    const auto q=C().queues.find(lease.queue);
    const uint64_t complete=q==C().queues.end()?UINT64_MAX:q->second.fence->GetCompletedValue();
    return q!=C().queues.end()&&complete!=UINT64_MAX&&complete>=lease.fenceValue;
}
// Caller holds C().lock. Local video memory the pool may still take: the OS
// budget less current use and the reserve; 0 when unknown.
uint64_t VramHeadroom() {
    auto& ctx=C();DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if(!ctx.memory||FAILED(ctx.memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)))return 0;
    const uint64_t used=local.CurrentUsage+ctx.vramReserve;
    return local.Budget>used?local.Budget-used:0;
}
// Caller holds C().lock. Converted-vertex buffers held, and those in flight.
struct PoolCount { uint32_t buffers{},busy{}; };
PoolCount PoolUse() {
    PoolCount count;
    for(const auto& lease:C().leases)if(lease.vertices) {++count.buffers;if(!Available(lease))++count.busy;}
    return count;
}
std::atomic<uint32_t> poolGrowthLogs{};
// The game allocates every hair BLAS itself from the triangle prebuild sizes;
// this bound only stops runaway accounting. A save load briefly holds the old
// and the new hair together (about 2x the live set), which must not refuse
// conversion and leave hair in the game's raster fallback.
constexpr uint64_t kAsBudget=4096ull*1024*1024;
// Occasional full rebuild of a refitted hair BLAS (see BuildTriangles). On an
// RTX 4080 a full build costs ~4.5x a refit (73k segments 0.60 vs 0.13 ms,
// 197k 1.62 vs 0.28 ms, 932k 6.68 vs 1.51 ms): only hair up to ~200k segments
// is rebuilt, at most one per 250 ms, so a rebuild never becomes a visible spike.
constexpr uint32_t kRebuildAfterRefits=300,kRebuildMaxSegments=200000;
constexpr uint64_t kRebuildSpacingMs=250;
// Caller holds C().lock. Idle leases (list reset, submission complete) no
// longer need the game's hair buffers. An association whose BLAS only this
// runtime still references belongs to hair the game destroyed (save load,
// streaming): drop it so its memory is freed and its slot/budget reused.
// Caller holds C().lock. True while the game still references this
// association's BLAS (a count above this runtime's own references).
bool GameHolds(const Association& entry) {
    if(!entry.blas)return false;
    ULONG ours=0;
    for(const auto& other:C().associations)if(other.blas.Get()==entry.blas.Get())++ours;
    for(const auto& lease:C().leases)if(lease.blas.Get()==entry.blas.Get())++ours;
    entry.blas->AddRef();
    return entry.blas->Release()>ours;
}
std::atomic<uint32_t> evictionLogs{};
// Converted-vertex buffers unused this long are returned, so the pool shrinks
// while hair is off or out of view instead of holding its peak size.
constexpr uint64_t kIdleReleaseMs=5000,kReclaimMs=1000;
// A finished recording on a list the game does not reset (its hair-build list
// while Path Traced Hair is off) would hold its buffer indefinitely. Once that
// recording is closed and its submission complete, the buffer is returned; a
// later re-submission of the same recording stops conversions (Execute).
void ReclaimFinished(uint64_t now) {
    auto& ctx=C();
    for(auto& lease:ctx.leases) {
        if(!lease.list||lease.poisoned||!lease.queue||now-lease.lastUsed<kReclaimMs)continue;
        auto* s=List(lease.list);
        if(!s||s->open.load(std::memory_order_acquire)||lease.generation!=s->generation)continue;
        const auto q=ctx.queues.find(lease.queue);
        if(q==ctx.queues.end())continue;
        const uint64_t complete=q->second.fence->GetCompletedValue();
        if(complete==UINT64_MAX||complete<lease.fenceValue)continue;
        s->reclaimed.store(true,std::memory_order_release);lease.list=nullptr;++ctx.stats.reclaims;
    }
}
void EvictReleased() {
    auto& ctx=C();const uint64_t now=GetTickCount64();
    ReclaimFinished(now);
    for(auto& lease:ctx.leases) {
        if((!lease.vertices&&!lease.blas)||!Available(lease))continue;
        if(lease.blas) {lease.positions.Reset();lease.indices.Reset();lease.blas.Reset();lease.scratch.Reset();}
        if(lease.vertices&&now-lease.lastUsed>=kIdleReleaseMs) {
            lease.vertices.Reset();ctx.stats.geometryBytes-=lease.capacity;lease.capacity=0;++ctx.stats.poolReleases;
        }
    }
    uint32_t evicted=0,live=0;
    for(auto& entry:ctx.associations) {
        if(!entry.owner)continue;
        if(GameHolds(entry)) {++live;continue;}
        entry=Association{};++evicted;
    }
    if(!evicted)return;
    ctx.stats.evictions+=evicted;
    // Hair toggles and save loads release hair in bursts: keep a bounded trace.
    if(evictionLogs.fetch_add(1,std::memory_order_relaxed)<16) {
        wchar_t line[160]{};
        swprintf_s(line,L"WITCHER_DOTS released hair evicted=%u live=%u",evicted,live);single_module::Log(line);
    }
}
bool Buffer(ID3D12Device* device,uint64_t bytes,D3D12_RESOURCE_STATES initial,ComPtr<ID3D12Resource>& resource) {
    if(!bytes||bytes>kGeometryBudget)return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&resource)));
}
// Returns the first unsupported LSS descriptor field, or nullptr. The game's
// hair builder (witcher3+0x280f2f0) records endcap mode NONE (0); the strip
// conversion has no caps, so NONE and CHAINED (1) convert identically.
const char* InputsInvalid(const HairInput& hair) {
    const auto& g=hair.geometry;
    if(g.type!=5)return "geometry type";
    if(g.flags!=1)return "geometry flags";
    if(g.vertexCount<2||g.indexCount!=g.primitiveCount)return "vertex/index counts";
    if(g.positions.stride!=16||g.positionFormat!=DXGI_FORMAT_R32G32B32_FLOAT)return "position layout";
    if(g.radii.stride!=16||g.radiusFormat!=DXGI_FORMAT_R32_FLOAT||g.radii.address!=g.positions.address+12)return "radius layout";
    if(g.indices.stride!=4||g.indexFormat!=DXGI_FORMAT_R32_UINT)return "index layout";
    if(g.endcaps>1)return "endcap mode";
    if(g.primitiveFormat!=1)return "primitive format";
    return nullptr;
}
D3D12_RAYTRACING_GEOMETRY_DESC Triangles(const HairInput& hair,uint64_t address) {
    D3D12_RAYTRACING_GEOMETRY_DESC g{};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexCount=hair.plan.vertices;g.Triangles.VertexBuffer={address,12};
    g.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;return g;
}
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs(uint32_t flags,const D3D12_RAYTRACING_GEOMETRY_DESC* geometry) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.Flags=static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(flags);input.NumDescs=1;
    input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=geometry;return input;
}
// The converter reads the hair inputs exactly where the game's own LSS BLAS
// build reads them, and AS build inputs must already be NON_PIXEL_SHADER_RESOURCE
// (or COMMON, implicitly promoted: buffers decay to COMMON after every
// ExecuteCommandLists). The game's builder never transitions these buffers in
// this list; they arrive readable from earlier submissions. Reject only a
// contradicting or partial transition recorded in this list since Reset.
// Returns nullptr when readable, else the reason.
const char* SourceUnreadable(ListState& list,ID3D12Resource* resource) {
    const size_t size=list.barriers.size(),n=std::min(list.barrierCursor,size);
    for(size_t j=0;j<n;++j) {
        const auto& event=list.barriers[(list.barrierCursor-1-j)%size];
        if(event.resource!=resource)continue;
        if(!event.known)return "partial or split transition recorded in this list";
        return event.state==D3D12_RESOURCE_STATE_COMMON||(event.state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            ?nullptr:"transitioned to a non-readable state in this list";
    }
    return nullptr;
}
using ResetFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
bool InstrumentList(ListState& state,void** before=nullptr);
HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* self,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    ListProbe probe;const auto original=Original<ResetFn>(self,10);
    probe.Before();const auto hr=original(self,allocator,pipeline);probe.After();
    if(FAILED(hr)||injecting)return hr;
    auto* s=List(self);if(!s)return hr;
    if(s->hasLeases.load(std::memory_order_acquire)) {
        std::lock_guard lock(C().lock);
        for(size_t id:s->leases) {
            auto& lease=C().leases[id];
            if(lease.list==s->keep.Get()&&lease.generation==s->generation) {
                // Unsubmitted recordings can be discarded. Submitted buffers
                // remain protected by their exact queue/fence completion.
                lease.list=nullptr;
                // Resource references are retained until this lease's
                // submission fence completes, even if Reset occurs first.
                if(Available(lease)) {
                    lease.positions.Reset();lease.indices.Reset();lease.blas.Reset();lease.scratch.Reset();
                }
            }
        }
        s->leases.clear();s->hasLeases.store(false,std::memory_order_release);s->reclaimed.store(false,std::memory_order_release);
    }
    s->barrierCursor=0;++s->generation;s->open.store(true,std::memory_order_release);
    if(dispatchDiagnostics.load(std::memory_order_relaxed))RotateDispatchLog(*s);
    s->bindings->Clear();s->bindings->pipeline=pipeline;s->bindings->pipelineKnown=true;
    // Agility moves a list into its own embedded table on the first Reset and
    // keeps it afterwards. Validate and publish only when the table changes.
    // A failure leaves the recorded table stale, so CurrentListTable rejects
    // conversions on this list alone.
    if(*reinterpret_cast<void***>(self)!=s->table) {
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
        if(!InstrumentList(*s,s->table)||!RememberListTable(*s)||!VerifyListTable(*s)) {
            s->instrumentFailures.fetch_add(1,std::memory_order_relaxed);tableFailures.fetch_add(1,std::memory_order_relaxed);
        }
        QueryPerformanceCounter(&end);
        tableChanges.fetch_add(1,std::memory_order_relaxed);
        tableChangeTicks.fetch_add(static_cast<uint64_t>(end.QuadPart-start.QuadPart),std::memory_order_relaxed);
    }
    return hr;
}
using CloseFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* self) {
    ListProbe probe;const auto original=Original<CloseFn>(self,9);
    probe.Before();const auto hr=original(self);probe.After();
    if(!injecting)if(auto* s=List(self)) {s->open.store(false,std::memory_order_release);if(FAILED(hr))s->bindings->valid=false;}
    return hr;
}
using PsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
void STDMETHODCALLTYPE Pso(ID3D12GraphicsCommandList* self,ID3D12PipelineState* pipeline) {
    ListProbe probe;const auto original=Original<PsoFn>(self,25);
    probe.Before();original(self,pipeline);probe.After();
    if(!injecting)if(auto* s=List(self)) {s->bindings->pipeline=pipeline;s->bindings->rayPipeline.Reset();s->bindings->pipelineKnown=true;}
}
void STDMETHODCALLTYPE Clear(ID3D12GraphicsCommandList* self,ID3D12PipelineState* pipeline) {
    ListProbe probe;const auto original=Original<PsoFn>(self,11);
    probe.Before();original(self,pipeline);probe.After();
    if(!injecting)if(auto* s=List(self)) {s->bindings->Clear();s->bindings->pipeline=pipeline;s->bindings->pipelineKnown=true;}
}
using RayPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,ID3D12StateObject*);
void STDMETHODCALLTYPE RayPso(ID3D12GraphicsCommandList4* self,ID3D12StateObject* pipeline) {
    ListProbe probe;const auto original=Original<RayPsoFn>(self,75);
    probe.Before();original(self,pipeline);probe.After();
    if(!injecting)if(auto* s=List(self)) {s->bindings->rayPipeline=pipeline;s->bindings->pipeline.Reset();s->bindings->pipelineKnown=true;}
}
using RootFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
void STDMETHODCALLTYPE Root(ID3D12GraphicsCommandList* self,ID3D12RootSignature* root) {
    ListProbe probe;const auto original=Original<RootFn>(self,29);
    probe.Before();original(self,root);probe.After();
    if(!injecting)if(auto* s=List(self)) {
        auto& b=*s->bindings;if(b.root.Get()!=root)b.ClearValues();b.root=root;b.rootKnown=FindRoot(root)!=nullptr;
    }
}
void Binding(ID3D12GraphicsCommandList* self,UINT index,ValueKind kind,uint64_t value) {
    if(auto* s=List(self)) {
        auto& b=*s->bindings;
        if(index>=64) {b.valid=false;return;}
        b.values[index].kind=kind;b.values[index].value=value;b.used|=1ull<<index;
    }
}
using TableFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
void STDMETHODCALLTYPE Descriptor(ID3D12GraphicsCommandList* self,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE value) {
    ListProbe probe;const auto original=Original<TableFn>(self,31);
    probe.Before();original(self,index,value);probe.After();
    if(!injecting)Binding(self,index,ValueKind::Table,value.ptr);
}
using VaFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
template<size_t Index,ValueKind Kind> void STDMETHODCALLTYPE Address(ID3D12GraphicsCommandList* self,UINT index,D3D12_GPU_VIRTUAL_ADDRESS value) {
    ListProbe probe;const auto original=Original<VaFn>(self,Index);
    probe.Before();original(self,index,value);probe.After();
    if(!injecting)Binding(self,index,Kind,value);
}
// The runtime consumed the same argument pointers in the original call.
void Words(ID3D12GraphicsCommandList* self,UINT index,UINT count,const void* data,UINT offset) {
    if(auto* s=List(self)) {
        auto& b=*s->bindings;
        if(index>=64||offset>64||count>64-offset||(count&&!data)) {b.valid=false;return;}
        auto& v=b.values[index];v.kind=ValueKind::Constants;b.used|=1ull<<index;
        memcpy(v.words.data()+offset,data,size_t(count)*4);
        for(UINT i=offset;i<offset+count;++i)v.wordsKnown|=1ull<<i;
    }
}
using WordFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
void STDMETHODCALLTYPE Word(ID3D12GraphicsCommandList* self,UINT index,UINT word,UINT offset) {
    ListProbe probe;const auto original=Original<WordFn>(self,33);
    probe.Before();original(self,index,word,offset);probe.After();
    if(!injecting)Words(self,index,1,&word,offset);
}
using WordsFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT);
void STDMETHODCALLTYPE Constants(ID3D12GraphicsCommandList* self,UINT index,UINT count,const void* data,UINT offset) {
    ListProbe probe;const auto original=Original<WordsFn>(self,35);
    probe.Before();original(self,index,count,data,offset);probe.After();
    if(!injecting)Words(self,index,count,data,offset);
}
using HeapsFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*);
void STDMETHODCALLTYPE Heaps(ID3D12GraphicsCommandList* self,UINT count,ID3D12DescriptorHeap* const* heaps) {
    ListProbe probe;const auto original=Original<HeapsFn>(self,28);
    probe.Before();original(self,count,heaps);probe.After();
    if(injecting)return;
    auto* s=List(self);if(!s)return;
    auto& b=*s->bindings;
    if(count>2||(count&&!heaps)) {b.valid=false;return;}
    bool changed=b.heapCount!=count;
    for(UINT i=0;i<count;++i)changed=changed||b.heaps[i].Get()!=heaps[i];
    if(changed)for(uint64_t mask=b.used;mask;mask&=mask-1) {
        const auto i=static_cast<size_t>(std::countr_zero(mask));
        if(b.values[i].kind==ValueKind::Table) {b.values[i]={};b.used&=~(1ull<<i);}
    }
    b.heapCount=count;
    for(UINT i=0;i<2;++i)b.heaps[i]=i<count?heaps[i]:nullptr;
}
using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void STDMETHODCALLTYPE Barriers(ID3D12GraphicsCommandList* self,UINT count,const D3D12_RESOURCE_BARRIER* barriers) {
    ListProbe probe;const auto original=Original<BarrierFn>(self,26);
    probe.Before();original(self,count,barriers);probe.After();
    if(injecting)return;
    auto* s=List(self);if(!s)return;
    if(count>4096||(count&&!barriers)) {s->bindings->valid=false;return;}
    for(UINT i=0;i<count;++i) {
        const auto& b=barriers[i];
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&b.Transition.pResource) {
            auto& event=s->barriers[(s->barrierCursor++)%s->barriers.size()];
            event.resource=b.Transition.pResource;event.state=b.Transition.StateAfter;
            event.known=b.Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE
                &&b.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        } else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            s->barrierCursor=0;
        }
    }
}
using DispatchFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT);
// Published only with dispatch diagnostics (TrackedListMethods).
void STDMETHODCALLTYPE DispatchHook(ID3D12GraphicsCommandList* self,UINT x,UINT y,UINT z) {
    ListProbe probe;const auto original=Original<DispatchFn>(self,14);
    probe.Before();original(self,x,y,z);probe.After();
    if(auto* s=List(self))LogDispatch(*s,x,y,z);
}
void STDMETHODCALLTYPE BuildAs(ID3D12GraphicsCommandList4* self,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc,
    UINT count,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* post);
const std::array<void*,kList4Methods>& ListHooks() {
    static const auto hooks=[] {
        std::array<void*,kList4Methods> h{};h[14]=reinterpret_cast<void*>(&DispatchHook);h[72]=reinterpret_cast<void*>(&BuildAs);
        h[9]=reinterpret_cast<void*>(&Close);h[10]=reinterpret_cast<void*>(&Reset);h[11]=reinterpret_cast<void*>(&Clear);
        h[25]=reinterpret_cast<void*>(&Pso);h[26]=reinterpret_cast<void*>(&Barriers);h[28]=reinterpret_cast<void*>(&Heaps);
        h[29]=reinterpret_cast<void*>(&Root);h[31]=reinterpret_cast<void*>(&Descriptor);h[33]=reinterpret_cast<void*>(&Word);
        h[35]=reinterpret_cast<void*>(&Constants);h[37]=reinterpret_cast<void*>(&Address<37,ValueKind::Cbv>);
        h[39]=reinterpret_cast<void*>(&Address<39,ValueKind::Srv>);h[41]=reinterpret_cast<void*>(&Address<41,ValueKind::Uav>);
        h[75]=reinterpret_cast<void*>(&RayPso);
        return h;
    }();
    return hooks;
}
bool InstrumentList(ListState& state,void** before) {
    auto* const list=state.keep.Get();
    PrivateListTable permit{};HMODULE owner{};
    const auto table=Table(list);if(!table)return false;
    const bool image=ImageMethod(table,owner,true);
    // Recorded before publication, so even a partial one leaves with the list.
    if(!image&&std::find(state.privateTables.begin(),state.privateTables.end(),table)==state.privateTables.end())
        state.privateTables.push_back(table);
    if(!image&&!PrivateNativeList(list,permit,before))return false;
    const auto& hooks=ListHooks();
    for(const auto index:TrackedListMethods())if(!Publish(list,index,hooks[index],Kind::List,image?nullptr:&permit))return false;
    // PrivateNativeList proved the private table's region before publication.
    return Table(list)==table&&(image||PrivateTableMatches(permit,list));
}
std::atomic<bool> listCapacityLogged{};
std::atomic<uint32_t> listReleaseLogs{};
// Caller holds C().lock. A tracked list the game destroyed (this runtime holds
// its only reference) can no longer be recorded or submitted: its leases are
// detached as on Reset (a submitted recording keeps its buffers until its
// fence completes), and its index entry and the publications of its embedded
// tables are removed before the list is released. Released states are
// destroyed by the caller after the lock is dropped.
void TakeReleasedLists(std::vector<std::unique_ptr<ListState>>& released) {
    auto& ctx=C();
    for(auto it=ctx.lists.begin();it!=ctx.lists.end();) {
        auto& s=*it->second;
        s.keep->AddRef();
        if(s.keep->Release()!=1) {++it;continue;}
        for(auto& lease:ctx.leases)if(lease.list==s.keep.Get()) {
            lease.list=nullptr;
            if(Available(lease)) {lease.positions.Reset();lease.indices.Reset();lease.blas.Reset();lease.scratch.Reset();}
        }
        RemoveKey(listKeys,ListHash(it->first),it->first);
        {
            std::lock_guard guard(publicationLock);
            for(auto** table:s.privateTables) {
                // A table another tracked list still uses is not this list's own.
                bool shared=false;
                for(const auto& entry:ctx.lists) {
                    const auto& other=*entry.second;
                    if(&other!=&s&&(other.table==table||std::find(other.privateTables.begin(),other.privateTables.end(),table)!=other.privateTables.end())) {shared=true;break;}
                }
                if(!shared)for(const auto index:TrackedListMethods())Unpublish(reinterpret_cast<uintptr_t>(table+index));
            }
        }
        released.push_back(std::move(it->second));it=ctx.lists.erase(it);
    }
    ctx.stats.releasedLists+=released.size();
}
bool TrackList(ID3D12GraphicsCommandList4* list,bool open,ID3D12PipelineState* pso) noexcept try {
    if(!Child(list))return false;
    const auto type=list->GetType();
    if(type!=D3D12_COMMAND_LIST_TYPE_DIRECT&&type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return true;
    auto& ctx=C();
    std::vector<std::unique_ptr<ListState>> released; // Destroyed after the lock is dropped.
    std::lock_guard lock(ctx.lock);
    if(ctx.lists.contains(list))return true;
    const uint64_t now=GetTickCount64();
    if(ctx.lists.size()>=kMaxLists||(ctx.lists.size()>=kListSweepAt&&now-ctx.lastListSweep>=kListSweepMs)) {
        ctx.lastListSweep=now;TakeReleasedLists(released);
        if(!released.empty()&&listReleaseLogs.fetch_add(1,std::memory_order_relaxed)<32) {
            wchar_t line[160]{};
            swprintf_s(line,L"WITCHER_DOTS released destroyed command lists=%zu tracked=%zu",released.size(),ctx.lists.size());
            single_module::Log(line);
        }
    }
    // Beyond capacity a list simply stays untracked: its copied hooks forward
    // through the canonical originals, and hair builds on it are rejected.
    if(ctx.lists.size()>=kMaxLists) {
        if(!listCapacityLogged.exchange(true))single_module::Log(L"WITCHER_DOTS list tracking capacity reached; further lists stay untracked");
        return true;
    }
    auto state=std::make_unique<ListState>();state->keep=list;state->open=open;
    state->bindings->pipeline=pso;state->bindings->pipelineKnown=true;state->leases.reserve(kMaxLeases);
    // Retain the native object before any private-table publication, including
    // partial failures. Forwarding slots outlive the application reference.
    auto* tracked=state.get();ctx.lists.emplace(list,std::move(state));
    if(!InstrumentList(*tracked)) {tracked->bindings->valid=false;return false;}
    if(!RememberListTable(*tracked)||!VerifyListTable(*tracked)||!IndexList(list,tracked))return false;
    void* self=list;list->SetPrivateData(kNativeListTag,sizeof(self),&self);
    return true;
} catch(...) {return false;}
using ExecuteFn=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* self,UINT count,ID3D12CommandList* const* lists) {
    const int64_t start=Qpc();
    executeCalls.fetch_add(1,std::memory_order_relaxed);executeLists.fetch_add(count,std::memory_order_relaxed);
    const auto original=Original<ExecuteFn>(self,10,Kind::Queue);
    bool have=false;
    if(lists&&count<=256)for(UINT i=0;i<count&&!have;++i)
        if(const auto* s=List(lists[i]))have=s->hasLeases.load(std::memory_order_acquire);
    if(!have) {
        executeOverheadTicks.fetch_add(static_cast<uint64_t>(Qpc()-start),std::memory_order_relaxed);
        original(self,count,lists);return;
    }
    executeHair.fetch_add(1,std::memory_order_relaxed);
    // Assign each lease's exact submission fence before the lists can execute.
    // A racing Reset then sees an incomplete fence and keeps the resources.
    // The lock is not held across the runtime call, whose driver may record
    // through instrumented lists of its own.
    auto& ctx=C();uint64_t value{};ID3D12Fence* fence{};
    {
        std::lock_guard lock(ctx.lock);
        const auto queue=ctx.queues.find(self);
        if(queue!=ctx.queues.end()) {value=++queue->second.next;fence=queue->second.fence.Get();}
        for(UINT i=0;i<count;++i) {
            auto* s=List(lists[i]);if(!s||!s->hasLeases.load(std::memory_order_acquire))continue;
            if(s->open.load(std::memory_order_acquire))Lost("hair recording submitted while its list was still open");
            if(s->reclaimed.load(std::memory_order_acquire)) {
                // Its converted vertices may already serve another recording.
                static std::atomic_flag logged=ATOMIC_FLAG_INIT;
                if(!logged.test_and_set())single_module::Log(L"WITCHER_DOTS finished hair recording submitted again after its buffer was returned; conversions stopped");
                Lost("finished hair recording submitted again after its buffer was returned");continue;
            }
            for(size_t id:s->leases) {
                auto& lease=ctx.leases[id];
                if(lease.list!=s->keep.Get()||lease.generation!=s->generation)continue;
                if(!fence||(lease.queue&&lease.queue!=self)) {
                    lease.poisoned=true;Lost(fence?"hair recording submitted on a second queue":"hair recording submitted on an untracked queue");
                }
                else {lease.queue=self;lease.fenceValue=value;}
            }
        }
    }
    const int64_t submit=Qpc();
    original(self,count,lists);
    const int64_t submitted=Qpc();
    if(fence&&FAILED(self->Signal(fence,value))) {
        std::lock_guard lock(ctx.lock);
        for(auto& lease:ctx.leases)if(lease.queue==self&&lease.fenceValue==value)lease.poisoned=true;
        Lost("queue fence signal failed");
    }
    executeOverheadTicks.fetch_add(static_cast<uint64_t>((submit-start)+(Qpc()-submitted)),std::memory_order_relaxed);
}
std::atomic<bool> queueCapacityLogged{};std::atomic<uint32_t> queueLogs{};
bool TrackQueue(ID3D12CommandQueue* queue) noexcept try {
    if(!Child(queue))return false;
    const auto type=queue->GetDesc().Type;
    if(type!=D3D12_COMMAND_LIST_TYPE_DIRECT&&type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return true;
    auto& ctx=C();std::lock_guard lock(ctx.lock);
    if(ctx.queues.contains(queue))return true;
    // Overlays (Steam, ReShade add-ons) create queues of their own on the
    // game's device. Beyond capacity a queue stays untracked, like a list:
    // only hair submitted on it stops conversions (Execute), not one more
    // queue such as the Steam overlay's on its first notification (a
    // controller connecting, a screenshot).
    if(ctx.queues.size()>=kMaxQueues) {
        if(!queueCapacityLogged.exchange(true))single_module::Log(L"WITCHER_DOTS queue tracking capacity reached; further queues stay untracked");
        // Its submissions must still reach Execute (normally the runtime table
        // already published), where hair on an untracked queue stops
        // conversions instead of leaving a lease without a fence.
        return Publish(queue,10,reinterpret_cast<void*>(&Execute),Kind::Queue);
    }
    QueueState state;state.keep=queue;
    if(FAILED(ctx.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state.fence)))||!Publish(queue,10,reinterpret_cast<void*>(&Execute),Kind::Queue))return false;
    ctx.queues.emplace(queue,std::move(state));
    if(queueLogs.fetch_add(1,std::memory_order_relaxed)<kMaxQueues) {
        wchar_t line[128]{};
        _snwprintf_s(line,_TRUNCATE,L"WITCHER_DOTS tracked queue %zu type=%s",ctx.queues.size(),type==D3D12_COMMAND_LIST_TYPE_DIRECT?L"direct":L"compute");
        single_module::Log(line);
    }
    return true;
} catch(...) {return false;}
std::atomic<uint32_t> listTrackLogs{};
using QueueFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* self,const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** output) {
    const auto hr=Original<QueueFn>(self,8,Kind::Device)(self,desc,iid,output);
    if(SUCCEEDED(hr)&&output&&*output&&OwnedDevice(self)) {
        ComPtr<ID3D12CommandQueue> queue;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue)))&&!TrackQueue(queue.Get())) {
            std::lock_guard lock(C().lock);Lost("command queue tracking failed (device, fence or publication)");
        }
    }
    return hr;
}
using ListFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
HRESULT STDMETHODCALLTYPE CreateList(ID3D12Device* self,UINT node,D3D12_COMMAND_LIST_TYPE type,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso,REFIID iid,void** output) {
    const auto hr=Original<ListFn>(self,12,Kind::Device)(self,node,type,allocator,pso,iid,output);
    if(SUCCEEDED(hr)&&output&&*output&&OwnedDevice(self)) {
        ComPtr<ID3D12GraphicsCommandList4> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))&&!TrackList(list.Get(),true,pso)) {
            // The list stays uninstrumented: hair recorded on it is refused
            // (BuildTriangles: untracked list / table not instrumented); other
            // lists keep converting. Overlays create lists of their own.
            if(listTrackLogs.fetch_add(1,std::memory_order_relaxed)<8)
                single_module::Log(L"WITCHER_DOTS command list tracking failed; hair builds on that list are refused");
        }
    }
    return hr;
}
using List1Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**);
HRESULT STDMETHODCALLTYPE CreateList1(ID3D12Device4* self,UINT node,D3D12_COMMAND_LIST_TYPE type,D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** output) {
    const auto hr=Original<List1Fn>(self,51,Kind::Device)(self,node,type,flags,iid,output);
    if(SUCCEEDED(hr)&&output&&*output&&OwnedDevice(self)) {
        ComPtr<ID3D12GraphicsCommandList4> list;
        if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))&&!TrackList(list.Get(),false,nullptr)) {
            // The list stays uninstrumented: hair recorded on it is refused
            // (BuildTriangles: untracked list / table not instrumented); other
            // lists keep converting. Overlays create lists of their own.
            if(listTrackLogs.fetch_add(1,std::memory_order_relaxed)<8)
                single_module::Log(L"WITCHER_DOTS command list tracking failed; hair builds on that list are refused");
        }
    }
    return hr;
}
using RootCreateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,const void*,SIZE_T,REFIID,void**);
HRESULT STDMETHODCALLTYPE CreateRoot(ID3D12Device* self,UINT node,const void* data,SIZE_T size,REFIID iid,void** output) {
    const auto hr=Original<RootCreateFn>(self,16,Kind::Device)(self,node,data,size,iid,output);
    if(FAILED(hr)||!output||!*output||!OwnedDevice(self)||size>1024*1024)return hr;
    try {
    using DeserializeFn=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    const auto module=single_module::LoadSystemModule(L"d3d12.dll");
    const auto deserialize=reinterpret_cast<DeserializeFn>(GetProcAddress(module,"D3D12CreateVersionedRootSignatureDeserializer"));
    ComPtr<ID3D12VersionedRootSignatureDeserializer> decoder;ComPtr<ID3D12RootSignature> root;
    if(!deserialize||FAILED(deserialize(data,size,IID_PPV_ARGS(&decoder)))
        ||FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&root))))return hr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
    if(FAILED(decoder->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0,&desc))||!desc||desc->Desc_1_0.NumParameters>64)return hr;
    RootInfo info;info.count=desc->Desc_1_0.NumParameters;info.keep=root;
    for(UINT i=0;i<info.count;++i) {
        const auto& p=desc->Desc_1_0.pParameters[i];info.type[i]=p.ParameterType;
        if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if(p.Constants.Num32BitValues>64)return hr;info.constants[i]=p.Constants.Num32BitValues;
        }
    }
    std::vector<ComPtr<ID3D12RootSignature>> released; // Released after the lock is dropped.
    std::lock_guard lock(C().lock);
    if(C().roots.size()>=kMaxRoots) {
        // Root signatures the game destroyed: only this runtime's reference
        // remains (a list binding holds its own), so nothing can bind them.
        for(auto it=C().roots.begin();it!=C().roots.end();) {
            auto& keep=it->second.keep;keep->AddRef();
            if(keep->Release()!=1) {++it;continue;}
            RemoveKey(rootKeys,RootHash(it->first),it->first);
            released.push_back(std::move(keep));it=C().roots.erase(it);
        }
        C().stats.releasedRoots+=released.size();
    }
    if(C().roots.size()<kMaxRoots&&!C().roots.contains(root.Get()))
        IndexRoot(root.Get(),&C().roots.emplace(root.Get(),std::move(info)).first->second);
    return hr;
    } catch(...) {StopConversions();return hr;}
}
// rtxhair.1 universal hair: a state object description with its DXIL libraries
// through the universal rewrite. Associations into the copied subobject array are
// re-pointed at the copy; one outside it keeps the game's pointer.
struct LibraryCopy {
    D3D12_STATE_OBJECT_DESC desc{};
    std::vector<D3D12_STATE_SUBOBJECT> objects;
    std::vector<D3D12_DXIL_LIBRARY_DESC> libraries;
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations;
    std::vector<std::vector<std::byte>> code;
    uint32_t replaced{};
};
bool RewriteLibraries(ShaderRewrite rewrite,const D3D12_STATE_OBJECT_DESC* original,LibraryCopy& c) {
    if(!CopyChecked(&c.desc,original,sizeof(c.desc))||!c.desc.NumSubobjects||c.desc.NumSubobjects>4096
        ||!Readable(c.desc.pSubobjects,size_t(c.desc.NumSubobjects)*sizeof(D3D12_STATE_SUBOBJECT)))return false;
    const UINT n=c.desc.NumSubobjects;
    c.objects.assign(c.desc.pSubobjects,c.desc.pSubobjects+n);
    c.libraries.resize(n);c.associations.resize(n);c.code.resize(n);
    for(UINT i=0;i<n;++i)if(c.objects[i].Type==D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
        if(!CopyChecked(&c.libraries[i],c.objects[i].pDesc,sizeof(c.libraries[i])))return false;
        const auto code=c.libraries[i].DXILLibrary;
        if(!code.pShaderBytecode||!code.BytecodeLength||code.BytecodeLength>64ull*1024*1024
            ||!Readable(code.pShaderBytecode,code.BytecodeLength))continue;
        if(!rewrite(code.pShaderBytecode,code.BytecodeLength,c.code[i])||c.code[i].empty())continue;
        c.libraries[i].DXILLibrary={c.code[i].data(),c.code[i].size()};c.objects[i].pDesc=&c.libraries[i];++c.replaced;
    }
    if(!c.replaced)return true;
    const uintptr_t begin=reinterpret_cast<uintptr_t>(c.desc.pSubobjects),end=begin+size_t(n)*sizeof(D3D12_STATE_SUBOBJECT);
    for(UINT i=0;i<n;++i)if(c.objects[i].Type==D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        if(!CopyChecked(&c.associations[i],c.objects[i].pDesc,sizeof(c.associations[i])))return false;
        const uintptr_t p=reinterpret_cast<uintptr_t>(c.associations[i].pSubobjectToAssociate);
        if(p<begin||p>=end)continue;
        if((p-begin)%sizeof(D3D12_STATE_SUBOBJECT))return false;
        c.associations[i].pSubobjectToAssociate=&c.objects[(p-begin)/sizeof(D3D12_STATE_SUBOBJECT)];
        c.objects[i].pDesc=&c.associations[i];
    }
    c.desc.pSubobjects=c.objects.data();
    return true;
}
std::atomic<uint32_t> universalStateLogs{};
// Created with the rewritten libraries; a driver refusal is created again with the
// game's own, which is what this GPU would have been given without universal hair.
std::atomic<RewriteRefused> rewriteRefused{};
template<class Create> HRESULT UniversalState(ShaderRewrite rewrite,const D3D12_STATE_OBJECT_DESC* original,Create&& create,void** output,const wchar_t* what) {
    LibraryCopy copy;
    if(!RewriteLibraries(rewrite,original,copy)||!copy.replaced)return create(original);
    HRESULT hr=create(&copy.desc);const HRESULT translated=hr;
    bool retried=false;
    if(FAILED(hr)) {
        if(output)*output=nullptr;
        // Once more with what the rewrite produces now (Witcher 3's look fixes off), if it
        // says that changed; then the game's own libraries.
        if(const auto refused=rewriteRefused.load(std::memory_order_acquire);refused&&refused()) {
            LibraryCopy retry;
            if(RewriteLibraries(rewrite,original,retry)&&retry.replaced) {
                retried=true;hr=create(&retry.desc);
                if(FAILED(hr)&&output)*output=nullptr;
            }
        }
        if(FAILED(hr))hr=create(original);
    }
    if(SUCCEEDED(translated)) {std::lock_guard lock(C().lock);C().stats.shaderLibraries+=copy.replaced;}
    if(universalStateLogs.fetch_add(1,std::memory_order_relaxed)<32) {
        wchar_t line[300]{};
        swprintf_s(line,L"RTX_HAIR %s: %u of %u subobjects rewritten, result 0x%08X%s",
            what,copy.replaced,copy.desc.NumSubobjects,static_cast<unsigned>(translated),
            SUCCEEDED(translated)?L"":retried&&SUCCEEDED(hr)?L"; created again without the Witcher 3 look fixes"
                :L"; created again with the game's own libraries");
        single_module::Log(line);
    }
    return hr;
}
using StateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*,const D3D12_STATE_OBJECT_DESC*,REFIID,void**);
HRESULT STDMETHODCALLTYPE CreateState(ID3D12Device5* self,const D3D12_STATE_OBJECT_DESC* original,REFIID iid,void** output) try {
    const auto call=Original<StateFn>(self,62,Kind::Device);
    if(const auto rewrite=C().rewrite.load(std::memory_order_acquire)) {
        if(!original||!OwnedDevice(self))return call(self,original,iid,output);
        return UniversalState(rewrite,original,[&](const D3D12_STATE_OBJECT_DESC* d) {return call(self,d,iid,output);},output,L"state object");
    }
    auto* const shaders=C().shaders.load(std::memory_order_acquire);
    if(!OwnedDevice(self)||!shaders||!original)return call(self,original,iid,output);
    D3D12_STATE_OBJECT_DESC desc{};
    if(!CopyChecked(&desc,original,sizeof(desc))||!desc.NumSubobjects||desc.NumSubobjects>1024||!Readable(desc.pSubobjects,size_t(desc.NumSubobjects)*sizeof(D3D12_STATE_SUBOBJECT)))
        return call(self,original,iid,output);
    std::vector<D3D12_STATE_SUBOBJECT> objects(desc.pSubobjects,desc.pSubobjects+desc.NumSubobjects);
    std::vector<D3D12_DXIL_LIBRARY_DESC> libraries(desc.NumSubobjects);
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations(desc.NumSubobjects);
    uint32_t replacements=0;bool closest=false,prepass=false;uint32_t path=0;
    std::vector<std::pair<size_t,D3D12_SHADER_BYTECODE>> pathOriginals; // translucency-only libraries replaced
    for(UINT i=0;i<desc.NumSubobjects;++i) if(objects[i].Type==D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
        if(!CopyChecked(&libraries[i],objects[i].pDesc,sizeof(libraries[i])))return call(self,original,iid,output);
        const auto replacement=shaders->Replacement(libraries[i].DXILLibrary.pShaderBytecode,libraries[i].DXILLibrary.BytecodeLength);
        if(!replacement.empty()) {
            const auto kind=shaders->ReplacementKind(replacement);
            if(kind==ShaderKind::PathTracer) {pathOriginals.emplace_back(i,libraries[i].DXILLibrary);++path;}
            else (kind==ShaderKind::ClosestHit?closest:prepass)=true;
            libraries[i].DXILLibrary={replacement.data(),replacement.size()};objects[i].pDesc=&libraries[i];++replacements;
        }
    }
    if(!replacements)return call(self,original,iid,output);
    const uintptr_t begin=reinterpret_cast<uintptr_t>(desc.pSubobjects),end=begin+objects.size()*sizeof(D3D12_STATE_SUBOBJECT);
    for(UINT i=0;i<desc.NumSubobjects;++i) if(objects[i].Type==D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
        if(!CopyChecked(&associations[i],objects[i].pDesc,sizeof(associations[i])))return E_INVALIDARG;
        const uintptr_t p=reinterpret_cast<uintptr_t>(associations[i].pSubobjectToAssociate);
        if(p<begin||p>=end||(p-begin)%sizeof(D3D12_STATE_SUBOBJECT))return E_INVALIDARG;
        associations[i].pSubobjectToAssociate=&objects[(p-begin)/sizeof(D3D12_STATE_SUBOBJECT)];
        objects[i].pDesc=&associations[i];
    }
    desc.pSubobjects=objects.data();auto hr=call(self,&desc,iid,output);
    // Hair translucency fails closed: if the driver refuses a state object with
    // translucency libraries, it is created again with the game's own ones.
    HRESULT refused=S_OK;
    if(FAILED(hr)&&path) {
        refused=hr;
        for(const auto& [index,code]:pathOriginals) {libraries[index].DXILLibrary=code;--replacements;}
        path=0;
        if(output)*output=nullptr;
        hr=replacements?call(self,&desc,iid,output):call(self,original,iid,output);
    }
    if(SUCCEEDED(hr)&&output&&*output) {std::lock_guard lock(C().lock);C().stats.shaderLibraries+=replacements;}
    static std::atomic<uint32_t> logged{};
    if(logged.fetch_add(1,std::memory_order_relaxed)<8) {
        wchar_t line[256]{};
        swprintf_s(line,L"WITCHER_DOTS state object translated closestHit=%d prepass=%d translucencyLibraries=%u subobjects=%u result=0x%08X",
            closest,prepass,path,desc.NumSubobjects,static_cast<unsigned>(hr));
        single_module::Log(line);
    }
    if(FAILED(refused)) {
        wchar_t line[200]{};
        swprintf_s(line,L"WITCHER_DOTS hair translucency: the driver refused the translucency libraries (0x%08X); the game's own are used",
            static_cast<unsigned>(refused));
        single_module::Log(line);
    }
    return hr;
} catch(...) {
    if(output)*output=nullptr;
    StopConversions();return E_OUTOFMEMORY;
}
using AddStateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device7*,const D3D12_STATE_OBJECT_DESC*,ID3D12StateObject*,REFIID,void**);
HRESULT STDMETHODCALLTYPE AddToState(ID3D12Device7* self,const D3D12_STATE_OBJECT_DESC* addition,ID3D12StateObject* from,REFIID iid,void** output) try {
    const auto call=Original<AddStateFn>(self,66,Kind::Device);
    const auto rewrite=C().rewrite.load(std::memory_order_acquire);
    if(!rewrite||!addition||!OwnedDevice(self))return call(self,addition,from,iid,output);
    return UniversalState(rewrite,addition,[&](const D3D12_STATE_OBJECT_DESC* d) {return call(self,d,from,iid,output);},output,L"state object addition");
} catch(...) {
    if(output)*output=nullptr;
    return E_OUTOFMEMORY;
}
using ComputeFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,REFIID,void**);
std::atomic<uint32_t> universalComputeLogs{};
// A ray-query compute shader with NVAPI LSS ops. A cached blob belongs to the game's
// own shader, so the rewritten one is compiled without it.
HRESULT STDMETHODCALLTYPE CreateCompute(ID3D12Device* self,const D3D12_COMPUTE_PIPELINE_STATE_DESC* original,REFIID iid,void** output) try {
    const auto call=Original<ComputeFn>(self,11,Kind::Device);
    const auto rewrite=C().rewrite.load(std::memory_order_acquire);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};std::vector<std::byte> code;
    if(!rewrite||!original||!CopyChecked(&desc,original,sizeof(desc))||!desc.CS.pShaderBytecode||!desc.CS.BytecodeLength
        ||desc.CS.BytecodeLength>64ull*1024*1024||!Readable(desc.CS.pShaderBytecode,desc.CS.BytecodeLength)||!OwnedDevice(self)
        ||!rewrite(desc.CS.pShaderBytecode,desc.CS.BytecodeLength,code)||code.empty())
        return call(self,original,iid,output);
    desc.CS={code.data(),code.size()};desc.CachedPSO={};
    HRESULT hr=call(self,&desc,iid,output);const HRESULT translated=hr;
    if(FAILED(hr)) {if(output)*output=nullptr;hr=call(self,original,iid,output);}
    if(universalComputeLogs.fetch_add(1,std::memory_order_relaxed)<32) {
        wchar_t line[200]{};
        swprintf_s(line,L"RTX_HAIR compute pipeline rewritten (NVAPI LSS ops), result 0x%08X%s",static_cast<unsigned>(translated),
            FAILED(translated)?L"; created again with the game's own shader":L"");
        single_module::Log(line);
    }
    return hr;
} catch(...) {
    if(output)*output=nullptr;
    return E_OUTOFMEMORY;
}
// A null root/pipeline means none was bound since Reset/ClearState: the game
// must bind its own before the next dispatch or draw, so nothing is restored.
void Restore(ID3D12GraphicsCommandList4* list,const Bindings& b,const RootInfo* bound) {
    if(b.rayPipeline)Original<RayPsoFn>(list,75)(list,b.rayPipeline.Get());
    else if(b.pipeline)Original<PsoFn>(list,25)(list,b.pipeline.Get());
    if(!bound)return;
    const auto& info=*bound;
    Original<RootFn>(list,29)(list,b.root.Get());
    for(UINT i=0;i<info.count;++i) {
        const auto& v=b.values[i];
        switch(v.kind) {
        case ValueKind::Table:Original<TableFn>(list,31)(list,i,{v.value});break;
        case ValueKind::Cbv:Original<VaFn>(list,37)(list,i,v.value);break;
        case ValueKind::Srv:Original<VaFn>(list,39)(list,i,v.value);break;
        case ValueKind::Uav:Original<VaFn>(list,41)(list,i,v.value);break;
        case ValueKind::Constants:
            for(UINT j=0;j<info.constants[i];++j)if(v.wordsKnown&(1ull<<j))Original<WordFn>(list,33)(list,i,v.words[j],j);
            break;
        default:break;
        }
    }
}
// Vtable data and its QueryInterface belong to the system or verified Agility
// runtime (debug layer included): not an application or overlay wrapper.
bool NativeVtable(IUnknown* object) {
    void* first{};HMODULE dataOwner{},callOwner{};
    auto** table=Table(object);
    return table&&Readable(table,sizeof(void*))&&ImageMethod(table,dataOwner,true)&&Read(table,0,first)
        &&ImageMethod(first,callOwner)&&NativeRuntime(dataOwner)&&NativeRuntime(callOwner);
}
// Vtables of resources the public unwrap proved unwrapped and runtime-owned:
// a resource carrying one of them is a native resource, used directly.
std::array<std::atomic<void*>,4> nativeResourceTables{};
bool Resource(void* owner,size_t offset,ComPtr<ID3D12Resource>& out) {
    IUnknown* p{};void* table{};
    if(!ReadFast(owner,offset,p)||!p)return false;
    if(ReadFast(p,0,table)&&table)for(const auto& known:nativeResourceTables)if(known.load(std::memory_order_acquire)==table) {
        out=static_cast<ID3D12Resource*>(p);
        return Child(out.Get())&&out->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;
    }
    if(!Unwrap(p,out)||!Child(out.Get())||out->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return false;
    if(table&&static_cast<void*>(out.Get())==static_cast<void*>(p)&&NativeVtable(p))for(auto& slot:nativeResourceTables) {
        void* expected{};
        if(slot.compare_exchange_strong(expected,table,std::memory_order_acq_rel)||expected==table)break;
    }
    return true;
}
}
// DLSS 5 must see the game's submissions; it hooks the runtime method our
// image-table Execute forwards to, which every tracked queue still reaches.
void* QueueExecuteForward(ID3D12CommandQueue* queue) noexcept {
    void** const table=queue?Table(queue):nullptr;void* current{};
    if(!table||!Read(table,10*sizeof(void*),current)||current!=reinterpret_cast<void*>(&Execute))return nullptr;
    const auto* entry=FindPublication(reinterpret_cast<uintptr_t>(table+10));
    return entry&&entry->hook==current?entry->original.load(std::memory_order_acquire):nullptr;
}
bool InitializeGpu(ID3D12Device5* device,ShaderCache* shaders,std::string& error) {
    auto& ctx=C();if(!device||!shaders||!shaders->Ready()){error="GPU shader cache not ready";return false;}
    ctx.device=device;ctx.shaders=shaders;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&ctx.identity)))) {error="device identity unavailable";return false;}
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,5};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={0,0};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor={1,0};
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor={0,0};
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=4;desc.pParameters=parameters;
    using SerializeFn=HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,D3D_ROOT_SIGNATURE_VERSION,ID3DBlob**,ID3DBlob**);
    const auto serialize=reinterpret_cast<SerializeFn>(GetProcAddress(single_module::LoadSystemModule(L"d3d12.dll"),"D3D12SerializeRootSignature"));
    ComPtr<ID3DBlob> blob,messages;
    if(!serialize||FAILED(serialize(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&messages))
        ||FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&ctx.converterRoot)))) {error="converter root signature failed";return false;}
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=ctx.converterRoot.Get();
    pso.CS={shaders->Converter().data(),shaders->Converter().size()};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&ctx.converterPso)))) {error="converter pipeline failed";return false;}
    const std::array<std::pair<size_t,void*>,5> hooks{{
        {8,reinterpret_cast<void*>(&CreateQueue)},{12,reinterpret_cast<void*>(&CreateList)},{16,reinterpret_cast<void*>(&CreateRoot)},
        {51,reinterpret_cast<void*>(&CreateList1)},{62,reinterpret_cast<void*>(&CreateState)}}};
    for(const auto& [index,hook]:hooks)if(!Publish(device,index,hook,Kind::Device)) {
        error="native D3D12 method publication failed at device slot "+std::to_string(index);return false;
    }
    // Validate the real CreateCommandList1 -> Reset transition before opening
    // any game hair gate. Agility may switch to an optimized private UMD table.
    // No commands are submitted and no game-owned list/allocator is borrowed.
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList4> probe;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))
        ||FAILED(device->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&probe)))
        ||FAILED(probe->Reset(allocator.Get(),nullptr))||FAILED(probe->Close())) {
        error="native D3D12 command-list preparation failed";return false;
    }
    {std::lock_guard lock(ctx.lock);auto* tracked=List(probe.Get());
        if(ctx.stats.lost||!tracked||!VerifyListTable(*tracked)||!CurrentListTable(*tracked)) {
            error="native D3D12 command-list Reset publication failed";return false;
        }
    }
    return true;
}
bool AbortGpuPreparation() noexcept {
    try {
        {std::lock_guard lock(C().lock);C().shaders=nullptr;C().rewrite=nullptr;}
        std::lock_guard lock(publicationLock);bool restored=true;
        for(auto& entry:publications) {
            const auto slot=entry.slot.load(std::memory_order_acquire);
            if(!slot||slot==kRemovedKey)continue; // Removed: its list (and table) was destroyed.
            const auto original=entry.original.load(std::memory_order_acquire);
            const auto observed=protected_pointer::ReadPointer(slot);
            if(observed==original) {
                restored=(protected_pointer::ProtectionMatches(slot,entry.protection,sizeof(void*))
                    ||protected_pointer::RestoreProtectionWithRetry(reinterpret_cast<void*>(slot),sizeof(void*),entry.protection,&VirtualProtect))&&restored;continue;
            }
            if(observed!=entry.hook) {restored=false;continue;}
            restored=ExchangeMethod(reinterpret_cast<void**>(slot),entry.hook,original,entry.protection)
                &&protected_pointer::ReadPointer(slot)==original
                &&protected_pointer::ProtectionMatches(slot,entry.protection,sizeof(void*))&&restored;
        }
        return restored;
    } catch(...) {return false;}
}
void StopConversions() noexcept {
    try {std::lock_guard lock(C().lock);Lost("stopped by hook validation (see the raster-fallback reason)");} catch(...) {}
}
bool InitializeTracking(ID3D12Device5* device,ShaderRewrite rewrite,std::string& error) {
    auto& ctx=C();
    if(!device||!rewrite) {error="universal tracking needs a device and a shader rewrite";return false;}
    if(ctx.device||ctx.shaders.load(std::memory_order_acquire)) {error="device tracking already prepared";return false;}
    // Before any list is instrumented: lists carry BuildRaytracingAccelerationStructure too.
    universalTracking.store(true,std::memory_order_release);
    ctx.device=device;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&ctx.identity)))) {error="device identity unavailable";return false;}
    ctx.rewrite.store(rewrite,std::memory_order_release);
    std::vector<std::pair<size_t,void*>> hooks{
        {8,reinterpret_cast<void*>(&CreateQueue)},{11,reinterpret_cast<void*>(&CreateCompute)},{12,reinterpret_cast<void*>(&CreateList)},
        {16,reinterpret_cast<void*>(&CreateRoot)},{51,reinterpret_cast<void*>(&CreateList1)},{62,reinterpret_cast<void*>(&CreateState)}};
    ComPtr<ID3D12Device7> seven;
    if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&seven))))hooks.push_back({66,reinterpret_cast<void*>(&AddToState)});
    for(const auto& [index,hook]:hooks)if(!Publish(device,index,hook,Kind::Device)) {
        error="native D3D12 method publication failed at device slot "+std::to_string(index);return false;
    }
    // As InitializeGpu: the real CreateCommandList1 -> Reset transition must reach a
    // tracked list on its current (possibly private Agility) table.
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList4> probe;
    if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))
        ||FAILED(device->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&probe)))
        ||FAILED(probe->Reset(allocator.Get(),nullptr))||FAILED(probe->Close())) {
        error="native D3D12 command-list preparation failed";return false;
    }
    std::lock_guard lock(ctx.lock);auto* tracked=List(probe.Get());
    if(ctx.stats.lost||!tracked||!VerifyListTable(*tracked)||!CurrentListTable(*tracked)) {
        error="native D3D12 command-list Reset publication failed";return false;
    }
    return true;
}
namespace {
// Caller holds C().lock. The tracked state of `native` when its compute bindings can be
// handed back after this runtime's own dispatch (as BuildTriangles requires), else null
// with `error`.
ListState* Restorable(ID3D12GraphicsCommandList4* native,const RootInfo*& bound,std::string& error) {
    auto& ctx=C();
    auto* list=List(native);
    // As BuildTriangles: no compute root signature since Reset leaves nothing to
    // restore; a bound but unregistered one cannot be restored and is refused.
    const bool rootBound=list&&list->bindings->root;
    bound=rootBound?FindRoot(list->bindings->root.Get()):nullptr;
    if(ctx.stats.lost||!list||!CurrentListTable(*list)||!list->open||!list->bindings->valid||!list->bindings->pipelineKnown
        ||(rootBound&&(!list->bindings->rootKnown||!bound))||(!rootBound&&list->bindings->used)) {
        error=ctx.stats.lost?"command-list tracking lost":!list?"build on an untracked command list":!CurrentListTable(*list)?"build list table not instrumented"
            :!list->open?"build list not recording":!list->bindings->valid?"build list bindings unreadable"
            :!list->bindings->pipelineKnown?"build list pipeline unknown"
            :rootBound?"build list compute root signature unknown":"build list root arguments without root signature";
        return nullptr;
    }
    const auto& values=list->bindings->values;
    for(uint32_t i=0;bound&&i<values.size();++i) {
        const auto& root=*bound;
        const auto kind=values[i].kind;
        if(kind==ValueKind::None)continue;
        if(i>=root.count||(kind==ValueKind::Table&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            ||(kind==ValueKind::Cbv&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_CBV)
            ||(kind==ValueKind::Srv&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_SRV)
            ||(kind==ValueKind::Uav&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_UAV)
            ||(kind==ValueKind::Constants&&(root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                ||(root.constants[i]<64&&(values[i].wordsKnown>>root.constants[i]))))) {
            error="compute bindings disagree with root signature";return nullptr;
        }
    }
    return list;
}
std::atomic<TopLevelPatch> instancePatch{};
std::atomic<uint64_t> topLevelPatched{},topLevelUnpatched{};
std::atomic<uint32_t> topLevelLogs{};
uint64_t lastLeaseSweep{};
// Caller holds C().lock. A lease buffer (NON_PIXEL_SHADER_RESOURCE between uses) of at
// least `bytes` for this recording of `list`: its queue fence keeps it from reuse until
// the submission that reads it completes (Execute). Idle leases are released after 5 s.
Lease* RecordingLease(ListState& list,ID3D12GraphicsCommandList4* native,uint64_t bytes) {
    auto& ctx=C();const uint64_t now=GetTickCount64();
    bytes=(bytes+0xFFFF)&~0xFFFFull;
    if(now-lastLeaseSweep>=1000) {
        lastLeaseSweep=now;
        for(auto& lease:ctx.leases)if(lease.vertices&&Available(lease)&&now-lease.lastUsed>=kIdleReleaseMs) {
            lease.vertices.Reset();ctx.stats.geometryBytes-=lease.capacity;lease.capacity=0;++ctx.stats.poolReleases;
        }
    }
    size_t chosen=kMaxLeases;
    for(size_t i=0;i<ctx.leases.size();++i) {
        const auto& l=ctx.leases[i];
        if(Available(l)&&l.vertices&&l.capacity>=bytes&&(chosen==kMaxLeases||l.capacity<ctx.leases[chosen].capacity))chosen=i;
    }
    for(size_t i=0;chosen==kMaxLeases&&i<ctx.leases.size();++i) {
        auto& l=ctx.leases[i];
        if(!Available(l)||(l.vertices&&l.capacity>=bytes))continue;
        if(l.vertices) {l.vertices.Reset();ctx.stats.geometryBytes-=l.capacity;l.capacity=0;++ctx.stats.poolReleases;}
        if(!Buffer(ctx.device.Get(),bytes,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,l.vertices))return nullptr;
        l.vertices->SetName(L"RTX hair instance copy");
        l.capacity=bytes;ctx.stats.geometryBytes+=bytes;++ctx.stats.poolAllocations;chosen=i;
    }
    if(chosen==kMaxLeases)return nullptr;
    auto& lease=ctx.leases[chosen];
    lease.queue=nullptr;lease.fenceValue=0;lease.lastUsed=now;lease.list=native;lease.generation=list.generation;
    list.leases.push_back(chosen);list.hasLeases.store(true,std::memory_order_release);
    return &lease;
}
// The patched copy of a top-level build's instances on the native list, or 0 with `error`.
D3D12_GPU_VIRTUAL_ADDRESS PatchInstances(ID3D12GraphicsCommandList4* native,D3D12_GPU_VIRTUAL_ADDRESS input,uint32_t count,std::string& error) {
    const auto patch=instancePatch.load(std::memory_order_acquire);
    if(!patch||!native||!input||!count) {error="no instance patch";return 0;}
    if(count>(1u<<24)) {error="too many instances";return 0;}
    auto& ctx=C();BuildLock lock(ctx.lock);
    const RootInfo* bound=nullptr;
    auto* list=Restorable(native,bound,error);
    if(!list)return 0;
    auto* lease=RecordingLease(*list,native,uint64_t(count)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    if(!lease) {error="instance copy buffer unavailable";return 0;}
    injecting=true;
    struct Injection {~Injection(){injecting=false;}} injection;
    D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition={lease->vertices.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    Original<BarrierFn>(native,26)(native,1,&transition);
    const auto output=lease->vertices->GetGPUVirtualAddress();
    const bool recorded=patch(native,input,count,output);
    D3D12_RESOURCE_BARRIER after[2]{};
    after[0].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;after[0].UAV.pResource=lease->vertices.Get();
    after[1]=transition;after[1].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;after[1].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Original<BarrierFn>(native,26)(native,2,after);
    Restore(native,*list->bindings,bound);
    if(!recorded) {error="instance patch declined";return 0;}
    return output;
}
void NoteUnpatched(const std::string& error) {
    topLevelUnpatched.fetch_add(1,std::memory_order_relaxed);
    if(topLevelLogs.fetch_add(1,std::memory_order_relaxed)<16) {
        wchar_t line[256]{};
        swprintf_s(line,L"RTX_HAIR top-level build left unpatched (hair there keeps its back faces culled): %S",error.c_str());
        single_module::Log(line);
    }
}
using BuildAsFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*,UINT,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
void STDMETHODCALLTYPE BuildAs(ID3D12GraphicsCommandList4* self,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc,
    UINT count,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* post) {
    ListProbe probe;const auto original=Original<BuildAsFn>(self,72);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC copy{};
    if(!injecting&&desc&&instancePatch.load(std::memory_order_acquire)&&desc->Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
        &&desc->Inputs.DescsLayout==D3D12_ELEMENTS_LAYOUT_ARRAY&&desc->Inputs.NumDescs&&desc->Inputs.InstanceDescs) {
        try {
            std::string error;
            if(const auto patched=PatchInstances(self,desc->Inputs.InstanceDescs,desc->Inputs.NumDescs,error)) {
                copy=*desc;copy.Inputs.InstanceDescs=patched;desc=&copy;topLevelPatched.fetch_add(1,std::memory_order_relaxed);
            } else NoteUnpatched(error);
        } catch(...) {NoteUnpatched("exception");}
    }
    probe.Before();original(self,desc,count,post);probe.After();
}
}
void SetInstancePatch(TopLevelPatch patch) noexcept {instancePatch.store(patch,std::memory_order_release);}
void SetRewriteRefused(RewriteRefused refused) noexcept {rewriteRefused.store(refused,std::memory_order_release);}
D3D12_GPU_VIRTUAL_ADDRESS PatchTopLevel(ID3D12GraphicsCommandList4* supplied,D3D12_GPU_VIRTUAL_ADDRESS input,uint32_t count,std::string& error) noexcept {
    try {
        ComPtr<ID3D12GraphicsCommandList4> native;
        if(!instancePatch.load(std::memory_order_acquire)) {error="no instance patch";return 0;}
        if(!UnwrapList(supplied,native)) {error="native command list unwrapping unavailable";NoteUnpatched(error);return 0;}
        const auto patched=PatchInstances(native.Get(),input,count,error);
        if(patched)topLevelPatched.fetch_add(1,std::memory_order_relaxed);
        else NoteUnpatched(error);
        return patched;
    } catch(...) {error="exception";return 0;}
}
TopLevelStats ReadTopLevelStats() noexcept {
    TopLevelStats s;s.patched=topLevelPatched.load(std::memory_order_relaxed);s.unpatched=topLevelUnpatched.load(std::memory_order_relaxed);
    try {
        std::lock_guard lock(C().lock);
        for(const auto& lease:C().leases)if(lease.vertices) {++s.leases;s.leaseBytes+=lease.capacity;}
    } catch(...) {}
    return s;
}
bool InjectCompute(ID3D12GraphicsCommandList4* supplied,const ComputeInjection& inject,std::string& error) try {
    ComPtr<ID3D12GraphicsCommandList4> native;
    if(!C().rewrite.load(std::memory_order_acquire)) {error="universal tracking not prepared";return false;}
    if(!UnwrapList(supplied,native)) {error="native command list unwrapping unavailable";return false;}
    auto& ctx=C();BuildLock lock(ctx.lock);
    const RootInfo* bound=nullptr;
    auto* list=Restorable(native.Get(),bound,error);
    if(!list) {Reject();return false;}
    injecting=true;
    struct Injection {~Injection(){injecting=false;}} injection;
    const auto* bindings=list->bindings.get();auto* const target=native.Get();
    inject(target,[target,bindings,bound] {Restore(target,*bindings,bound);});
    ++ctx.stats.builds;ctx.stats.lastBuildTick=GetTickCount64();
    return true;
} catch(...) {
    error="compute injection failed";return false;
}
bool TrackedDevice(ID3D12Device* device) noexcept {
    try {return C().rewrite.load(std::memory_order_acquire)&&OwnedDevice(device);} catch(...) {return false;}
}
namespace {
struct OwnerLayout { uint32_t scratch{0x4e0},blas{0x4e8},positions{0x4f8},indices{0x508}; } ownerLayout;
}
void SetHairOwnerLayout(uint32_t scratch,uint32_t blas,uint32_t positions,uint32_t indices) noexcept {
    ownerLayout={scratch,blas,positions,indices};
}
void SetMemoryAdapter(IDXGIAdapter3* adapter) noexcept {
    try {std::lock_guard lock(C().lock);C().memory=adapter;} catch(...) {}
}
void SetGeometryPoolLimits(uint64_t budget,uint64_t ceiling,uint64_t reserve) noexcept {
    try {
        std::lock_guard lock(C().lock);auto& ctx=C();
        ctx.poolBudget=budget;ctx.poolCeiling=ceiling;ctx.vramReserve=reserve;
    } catch(...) {}
}
bool ReadHairInput(void* owner,const ExtendedInputs& inputs,HairInput& out,std::string& error) {
    if(!owner||inputs.type!=1||inputs.count!=1||inputs.layout!=0||inputs.stride!=168
        ||(inputs.flags!=7&&inputs.flags!=0x27)||!CopyFast(&out.geometry,inputs.geometry,sizeof(out.geometry))) {error="unknown LSS descriptor";return false;}
    out.owner=owner;
    if(const char* field=InputsInvalid(out)) {
        const auto& g=out.geometry;char text[256]{};
        _snprintf_s(text,_TRUNCATE,"unsupported LSS %s (type=%u flags=%u vertices=%u indices=%u primitives=%u formats=%u/%u/%u strides=%llu/%llu/%llu endcaps=%u primitiveFormat=%u)",
            field,g.type,g.flags,g.vertexCount,g.indexCount,g.primitiveCount,g.positionFormat,g.radiusFormat,g.indexFormat,
            g.positions.stride,g.radii.stride,g.indices.stride,g.endcaps,g.primitiveFormat);
        error=text;return false;
    }
    if(!Resource(owner,ownerLayout.positions,out.positions)) {error="hair position buffer ownership unavailable";return false;}
    if(!Resource(owner,ownerLayout.indices,out.indices)) {error="hair index buffer ownership unavailable";return false;}
    const auto& g=out.geometry;
    const auto positions=out.positions->GetDesc(),indices=out.indices->GetDesc();
    if(out.positions->GetGPUVirtualAddress()!=g.positions.address||out.indices->GetGPUVirtualAddress()!=g.indices.address
        ||positions.Width<uint64_t(g.vertexCount)*16||indices.Width<uint64_t(g.indexCount)*4
        ||(positions.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||(indices.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        char text[256]{};
        _snprintf_s(text,_TRUNCATE,"hair buffer bounds mismatch (positions va %s width %llu for %u, indices va %s width %llu for %u)",
            out.positions->GetGPUVirtualAddress()==g.positions.address?"match":"differs",positions.Width,g.vertexCount,
            out.indices->GetGPUVirtualAddress()==g.indices.address?"match":"differs",indices.Width,g.indexCount);
        error=text;return false;
    }
    out.plan=MakePlan(g.primitiveCount,g.vertexCount,g.indexCount);
    const uint32_t strands=g.vertexCount>g.primitiveCount?g.vertexCount-g.primitiveCount:0;
    if(!out.plan||!strands||g.primitiveCount%strands) {
        char text[160]{};
        _snprintf_s(text,_TRUNCATE,"nonuniform or over-budget hair topology (vertices=%u primitives=%u)",g.vertexCount,g.primitiveCount);
        error=text;return false;
    }
    out.segmentsPerStrand=g.primitiveCount/strands;
    return out.segmentsPerStrand>0;
}
bool PrebuildTriangles(const HairInput& hair,uint32_t flags,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info) {
    BuildLock lock(C().lock);
    if(C().stats.lost||!hair.plan||!C().device||(flags!=7&&flags!=0x27)) {Reject();return false;}
    const auto geometry=Triangles(hair,0);const auto inputs=Inputs(flags,&geometry);
    C().device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&info);
    if(!info.ResultDataMaxSizeInBytes||!info.ScratchDataSizeInBytes
        ||info.ResultDataMaxSizeInBytes>256ull*1024*1024||info.ScratchDataSizeInBytes>kGeometryBudget
        ||info.UpdateScratchDataSizeInBytes>kGeometryBudget) {Reject();return false;}
    ++C().stats.prebuilds;return true;
}
bool BuildTriangles(HairInput hair,ID3D12GraphicsCommandList4* supplied,const ExtendedBuild& desc,std::string& error) {
    ComPtr<ID3D12GraphicsCommandList4> native;
    if(!UnwrapList(supplied,native)) {error="native command list unwrapping unavailable";return false;}
    if(!Resource(hair.owner,ownerLayout.blas,hair.blas)) {error="hair BLAS buffer ownership unavailable";return false;}
    if(!Resource(hair.owner,ownerLayout.scratch,hair.scratch)) {error="hair scratch buffer ownership unavailable";return false;}
    auto& ctx=C();BuildLock lock(ctx.lock);
    auto* list=List(native.Get());
    // No compute root signature since Reset (null) leaves nothing to restore;
    // a bound but unregistered one cannot be restored and is rejected.
    const bool rootBound=list&&list->bindings->root;
    const RootInfo* bound=rootBound?FindRoot(list->bindings->root.Get()):nullptr;
    if(ctx.stats.lost||!list||!CurrentListTable(*list)||!list->open||!list->bindings->valid||!list->bindings->pipelineKnown
        ||(rootBound&&(!list->bindings->rootKnown||!bound))||(!rootBound&&list->bindings->used)) {
        error=!list?"hair build on an untracked command list":!CurrentListTable(*list)?"hair build list table not instrumented"
            :!list->open?"hair build list not recording":!list->bindings->valid?"hair build list bindings unreadable"
            :!list->bindings->pipelineKnown?"hair build list pipeline unknown"
            :rootBound?"hair build list compute root signature unknown":"hair build list root arguments without root signature";
        Reject();return false;
    }
    const auto& values=list->bindings->values;
    for(uint32_t i=0;bound&&i<values.size();++i) {
        const auto& root=*bound;
        const auto kind=values[i].kind;
        if(kind==ValueKind::None)continue;
        if(i>=root.count||(kind==ValueKind::Table&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            ||(kind==ValueKind::Cbv&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_CBV)
            ||(kind==ValueKind::Srv&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_SRV)
            ||(kind==ValueKind::Uav&&root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_UAV)
            ||(kind==ValueKind::Constants&&(root.type[i]!=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                ||(root.constants[i]<64&&(values[i].wordsKnown>>root.constants[i]))))) {
            error="compute bindings disagree with root signature";Reject();return false;
        }
    }
    if(const char* reason=SourceUnreadable(*list,hair.positions.Get())) {error=std::string("hair positions ")+reason;Reject();return false;}
    if(const char* reason=SourceUnreadable(*list,hair.indices.Get())) {error=std::string("hair indices ")+reason;Reject();return false;}
    if(desc.destination!=hair.blas->GetGPUVirtualAddress()||desc.scratch!=hair.scratch->GetGPUVirtualAddress()
        ||desc.destination%256||desc.scratch%256
        ||!(hair.blas->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        ||!(hair.scratch->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {error="AS allocation/address mismatch";Reject();return false;}
    EvictReleased();
    Association* association=nullptr;
    for(auto& entry:ctx.associations)if(entry.owner==hair.owner){association=&entry;break;}
    const bool update=(desc.inputs.flags&0x20)!=0;
    if(update&&(!association||association->blas.Get()!=hair.blas.Get()||association->segments!=hair.plan.segments
        ||association->vertices!=hair.geometry.vertexCount||association->flags!=(desc.inputs.flags&~0x20u)
        ||desc.source!=desc.destination)) {error="unknown BLAS update generation/topology";Reject();return false;}
    if(!update&&desc.source) {error="unexpected BLAS source";Reject();return false;}
    if(!association)for(auto& entry:ctx.associations)if(!entry.owner){association=&entry;break;}
    if(!association) {error="hair owner capacity exhausted";Reject();return false;}
    // The game allocated these AS buffers from our prebuild sizes; this bound
    // only caps how much live hair is converted (stale owners are evicted).
    uint64_t totalAs=hair.blas->GetDesc().Width;uint32_t owners=1;
    for(const auto& entry:ctx.associations)if(&entry!=association&&entry.owner) {totalAs+=entry.bytes;++owners;}
    if(totalAs>kAsBudget) {
        char text[160]{};
        _snprintf_s(text,_TRUNCATE,"hair AS budget exhausted (%u live owners, %llu MiB)",owners,totalAs>>20);
        error=text;Reject();return false;
    }
    const auto emptyGeometry=Triangles(hair,0);const auto sizeInput=Inputs(desc.inputs.flags,&emptyGeometry);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO size{};
    ctx.device->GetRaytracingAccelerationStructurePrebuildInfo(&sizeInput,&size);
    if(!size.ResultDataMaxSizeInBytes||hair.blas->GetDesc().Width<size.ResultDataMaxSizeInBytes
        ||hair.scratch->GetDesc().Width<(update?size.UpdateScratchDataSizeInBytes:size.ScratchDataSizeInBytes)) {
        error="triangle BLAS/scratch capacity mismatch";Reject();return false;
    }
    size_t chosen=kMaxLeases;bool reused=false;
    // The game builds all of its hair in one burst at load, in one list. Later
    // builds in the same recording reuse this list's buffer: the converter's
    // NON_PIXEL_SHADER_RESOURCE->UNORDERED_ACCESS transition orders its writes
    // after the previous BLAS build's vertex reads. The game keeps its own
    // hair buffers alive through this submission (its own build uses them).
    for(size_t id:list->leases) {
        const auto& own=ctx.leases[id];
        if(own.list==native.Get()&&own.generation==list->generation&&!own.poisoned&&own.capacity>=hair.plan.bytes) {chosen=id;reused=true;break;}
    }
    // The first buffer of a recording is sized for the largest live hair, so a
    // burst (load, settings change) reuses one buffer rather than one per hair.
    uint64_t preferred=hair.plan.bytes;
    if(chosen==kMaxLeases)for(const auto& entry:ctx.associations)
        if(entry.owner)preferred=std::max<uint64_t>(preferred,uint64_t(entry.segments)*kBytesPerSegment);
    // Best fit keeps large idle buffers for large hair, avoiding reallocation.
    const auto bestFit=[&](uint64_t bytes) {
        size_t best=kMaxLeases;
        for(size_t i=0;i<ctx.leases.size();++i) {
            const auto& candidate=ctx.leases[i];
            if(Available(candidate)&&candidate.vertices&&candidate.capacity>=bytes
                &&(best==kMaxLeases||candidate.capacity<ctx.leases[best].capacity))best=i;
        }
        return best;
    };
    if(chosen==kMaxLeases)chosen=bestFit(preferred);
    if(chosen==kMaxLeases&&preferred>hair.plan.bytes)chosen=bestFit(hair.plan.bytes);
    const auto allocate=[&](Lease& candidate,uint64_t bytes,uint64_t limit) {
        if(ctx.stats.geometryBytes+bytes>limit
            ||!Buffer(ctx.device.Get(),bytes,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,candidate.vertices))return false;
        candidate.vertices->SetName(L"WitcherDOTS converted hair vertices");
        candidate.capacity=bytes;ctx.stats.geometryBytes+=bytes;++ctx.stats.poolAllocations;return true;
    };
    const auto empty=[&] {
        for(size_t i=0;i<ctx.leases.size();++i)if(Available(ctx.leases[i])&&!ctx.leases[i].vertices)return i;
        return kMaxLeases;
    };
    if(chosen==kMaxLeases)if(const size_t i=empty();i!=kMaxLeases) {
        if(allocate(ctx.leases[i],preferred,ctx.poolBudget)
            ||(preferred>hair.plan.bytes&&allocate(ctx.leases[i],hair.plan.bytes,ctx.poolBudget)))chosen=i;
    }
    // Budget full of buffers sized for earlier hair: retire idle, undersized
    // ones (never in flight) until the new buffer fits.
    if(chosen==kMaxLeases)for(size_t i=0;i<ctx.leases.size()&&chosen==kMaxLeases;++i) {
        auto& idle=ctx.leases[i];
        if(!Available(idle)||!idle.vertices||idle.capacity>=hair.plan.bytes)continue;
        idle.vertices.Reset();ctx.stats.geometryBytes-=idle.capacity;idle.capacity=0;++ctx.stats.poolReleases;
        if(allocate(idle,hair.plan.bytes,ctx.poolBudget))chosen=i;
    }
    // The budget is held by buffers still in flight (a load: the old area's
    // recordings run while the new hair is built). The game does not retry a
    // refused build, so grow past the budget into free video memory, up to the
    // ceiling. Idle buffers return within kIdleReleaseMs afterwards.
    if(chosen==kMaxLeases)if(const size_t i=empty();i!=kMaxLeases) {
        const uint64_t headroom=VramHeadroom(),limit=std::min(ctx.poolCeiling,ctx.stats.geometryBytes+headroom);
        if(allocate(ctx.leases[i],preferred,limit)||(preferred>hair.plan.bytes&&allocate(ctx.leases[i],hair.plan.bytes,limit))) {
            chosen=i;++ctx.stats.poolGrowths;
            if(poolGrowthLogs.fetch_add(1,std::memory_order_relaxed)<16) {
                const auto pool=PoolUse();wchar_t line[256]{};
                swprintf_s(line,L"WITCHER_DOTS hair vertex pool past its %llu MiB budget (buffers in flight): %llu MiB in %u buffers, %u in flight; VRAM headroom %llu MiB",
                    ctx.poolBudget>>20,ctx.stats.geometryBytes>>20,pool.buffers,pool.busy,headroom>>20);
                single_module::Log(line);
            }
        }
    }
    if(chosen==kMaxLeases) {
        const auto pool=PoolUse();char text[224]{};
        _snprintf_s(text,_TRUNCATE,"geometry pool unavailable (%llu MiB needed, %llu MiB pooled in %u buffers, %u in flight; VRAM headroom %llu MiB)",
            hair.plan.bytes>>20,ctx.stats.geometryBytes>>20,pool.buffers,pool.busy,VramHeadroom()>>20);
        error=text;Reject();return false;
    }
    const uint64_t now=GetTickCount64();
    auto& lease=ctx.leases[chosen];lease.lastUsed=now;
    // A newly chosen buffer's previous submission is complete: its fence (and
    // queue) belong to that submission, not to this recording's.
    if(!reused) {lease.queue=nullptr;lease.fenceValue=0;}
    lease.positions=hair.positions;lease.indices=hair.indices;lease.blas=hair.blas;lease.scratch=hair.scratch;
    lease.list=native.Get();lease.generation=list->generation;
    if(!reused)list->leases.push_back(chosen);
    else ++ctx.stats.leaseReuses;
    list->hasLeases.store(true,std::memory_order_release);
    injecting=true;
    struct Injection {~Injection(){injecting=false;}} injection;
    // With the opt-in removal report, name this runtime's commands in DRED.
    const bool marked=removalReportArmed.load(std::memory_order_acquire);
    if(marked) {static constexpr wchar_t text[]=L"WitcherDOTS hair conversion";native->SetMarker(0,text,sizeof(text));}
    // Every recording starts and ends in the same state, including its first
    // submission. This also permits serialized replay on the same queue.
    {
        D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition={lease.vertices.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        Original<BarrierFn>(native.Get(),26)(native.Get(),1,&transition);
    }
    Original<RootFn>(native.Get(),29)(native.Get(),ctx.converterRoot.Get());
    Original<PsoFn>(native.Get(),25)(native.Get(),ctx.converterPso.Get());
    const uint32_t constants[]{hair.plan.segments,hair.geometry.vertexCount,hair.geometry.indexCount,hair.plan.vertices,hair.segmentsPerStrand};
    Original<WordsFn>(native.Get(),35)(native.Get(),0,5,constants,0);
    Original<VaFn>(native.Get(),39)(native.Get(),1,hair.geometry.positions.address);
    Original<VaFn>(native.Get(),39)(native.Get(),2,hair.geometry.indices.address);
    Original<VaFn>(native.Get(),41)(native.Get(),3,lease.vertices->GetGPUVirtualAddress());
    native->Dispatch(hair.plan.groups,1,1);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barriers[0].UAV.pResource=lease.vertices.Get();
    barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition={lease.vertices.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    Original<BarrierFn>(native.Get(),26)(native.Get(),2,barriers);
    Restore(native.Get(),*list->bindings,bound);
    const auto geometry=Triangles(hair,lease.vertices->GetGPUVirtualAddress());
    // Refits keep the first build's tree; strands that travel far (riding,
    // wind) swell its boxes and slow every ray through the hair. When the
    // game's scratch fits a full build, occasionally rebuild in place instead.
    const bool rebuild=update&&association->refits>=kRebuildAfterRefits&&hair.plan.segments<=kRebuildMaxSegments
        &&hair.scratch->GetDesc().Width>=size.ScratchDataSizeInBytes&&now-ctx.lastRebuild>=kRebuildSpacingMs;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData=desc.destination;build.SourceAccelerationStructureData=rebuild?0:desc.source;
    build.ScratchAccelerationStructureData=desc.scratch;build.Inputs=Inputs(rebuild?desc.inputs.flags&~0x20u:desc.inputs.flags,&geometry);
    D3D12_RESOURCE_BARRIER asBarriers[2]{};
    asBarriers[0].Type=asBarriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    asBarriers[0].UAV.pResource=hair.blas.Get();asBarriers[1].UAV.pResource=hair.scratch.Get();
    Original<BarrierFn>(native.Get(),26)(native.Get(),2,asBarriers);
    if(marked) {static constexpr wchar_t text[]=L"WitcherDOTS triangle BLAS build";native->SetMarker(0,text,sizeof(text));}
    native->BuildRaytracingAccelerationStructure(&build,0,nullptr);
    Original<BarrierFn>(native.Get(),26)(native.Get(),1,asBarriers);
    association->owner=hair.owner;association->blas=hair.blas;association->positions=hair.positions;association->indices=hair.indices;
    association->address=desc.destination;association->bytes=hair.blas->GetDesc().Width;association->scratchBytes=hair.scratch->GetDesc().Width;
    association->segments=hair.plan.segments;association->vertices=hair.geometry.vertexCount;association->flags=desc.inputs.flags&~0x20u;
    association->refits=update&&!rebuild?association->refits+1:0;
    if(rebuild) {ctx.lastRebuild=now;++ctx.stats.fullRebuilds;}
    static std::atomic<uint32_t> resumeLogs{};
    if(ctx.stats.lastBuildTick&&now-ctx.stats.lastBuildTick>=2000&&resumeLogs.fetch_add(1,std::memory_order_relaxed)<16) {
        uint32_t live=0;for(const auto& entry:ctx.associations)if(entry.owner)++live;
        wchar_t line[192]{};
        swprintf_s(line,L"WITCHER_DOTS hair builds resumed after %llu ms (update=%d owners=%u)",now-ctx.stats.lastBuildTick,update?1:0,live);
        single_module::Log(line);
    }
    ctx.stats.lastBuildTick=now;
    ++ctx.stats.builds;if(update)++ctx.stats.updates;return true;
}
bool PrepareInstances(std::span<D3D12_RAYTRACING_INSTANCE_DESC> instances,bool hairTraced) {
    auto& ctx=C();std::lock_guard lock(ctx.lock);
    if(instances.size()>4096) {Reject();return false;}
    // No hair builds run while the game's hair is disabled; the TLAS copy
    // still runs, so destroyed hair is released here as well.
    const uint64_t now=GetTickCount64();
    if(now-ctx.lastSweep>=250) {ctx.lastSweep=now;EvictReleased();}
    uint32_t admitted=0;
    if(!hairTraced) {
        // HairWorks stays raster: no hair instance enters any ray traced pass.
        for(auto& instance:instances)if(instance.InstanceMask&0x80)instance.InstanceMask=0;
        ++ctx.stats.instanceCopies;return true;
    }
    for(auto& instance:instances)if(instance.InstanceMask&0x80) {
        Association* owned=nullptr;
        for(auto& entry:ctx.associations)if(entry.owner&&entry.address==instance.AccelerationStructure) {owned=&entry;break;}
        // A BLAS the game released can share its address with a newer game
        // allocation (placed-resource reuse): that address is no longer ours.
        // Evicting also drops idle leases' references so the BLAS is freed.
        if(owned&&!GameHolds(*owned)) {EvictReleased();owned=nullptr;}
        if(ctx.stats.lost||!owned||instance.InstanceMask!=0x80||instance.InstanceID>=32) {
            instance.InstanceMask=0;Reject();continue;
        }
        instance.Flags|=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;++admitted;
    }
    if(admitted) {ctx.stats.lastHairTick=now;ctx.stats.hairInstances=admitted;}
    ++ctx.stats.instanceCopies;return true;
}
RuntimeStats ReadRuntimeStats() {
    std::lock_guard lock(C().lock);auto stats=C().stats;
    stats.buildLockWaitTicks=buildLockWaitTicks.load(std::memory_order_relaxed);
    stats.buildLockHeldTicks=buildLockHeldTicks.load(std::memory_order_relaxed);
    stats.trackedLists=static_cast<uint32_t>(C().lists.size());stats.trackedRoots=static_cast<uint32_t>(C().roots.size());
    stats.tableChanges=tableChanges.load(std::memory_order_relaxed);stats.tableFailures=tableFailures.load(std::memory_order_relaxed);
    stats.tableChangeTicks=tableChangeTicks.load(std::memory_order_relaxed);
    stats.listCalls=listCalls.load(std::memory_order_relaxed);stats.listSamples=listSamples.load(std::memory_order_relaxed);
    stats.listOverheadTicks=listOverheadTicks.load(std::memory_order_relaxed);stats.listOriginalTicks=listOriginalTicks.load(std::memory_order_relaxed);
    stats.listOutliers=listOutliers.load(std::memory_order_relaxed);
    stats.executeCalls=executeCalls.load(std::memory_order_relaxed);stats.executeLists=executeLists.load(std::memory_order_relaxed);
    stats.executeHair=executeHair.load(std::memory_order_relaxed);stats.executeOverheadTicks=executeOverheadTicks.load(std::memory_order_relaxed);
    stats.qpcCostTicks=QpcCostTicks();
    for(const auto& entry:C().associations)if(entry.owner) {
        ++stats.liveOwners;stats.hairBlasBytes+=entry.bytes;stats.hairScratchBytes+=entry.scratchBytes;
    }
    return stats;
}
namespace {
// Structured-exception scopes hold no C++ objects that need unwinding.
size_t ScanHair(const D3D12_RAYTRACING_INSTANCE_DESC* in,size_t& at,size_t count,
    D3D12_RAYTRACING_INSTANCE_DESC* hair,uint32_t* where,size_t capacity,bool& faulted) noexcept {
    size_t found=0;faulted=false;
    __try {
        for(;at<count&&found<capacity;++at)
            if(in[at].InstanceMask&0x80) {hair[found]=in[at];where[found]=static_cast<uint32_t>(at);++found;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {faulted=true;}
    return found;
}
bool WriteHair(D3D12_RAYTRACING_INSTANCE_DESC* out,const D3D12_RAYTRACING_INSTANCE_DESC* hair,const uint32_t* where,size_t found) noexcept {
    __try {for(size_t k=0;k<found;++k)out[where[k]]=hair[k];}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    return true;
}
std::atomic<uint64_t> nextSweep{};
}
void SweepReleased() noexcept {
    const uint64_t now=GetTickCount64();
    if(now<nextSweep.load(std::memory_order_relaxed))return;
    try {
        std::lock_guard lock(C().lock);
        if(now-C().lastSweep>=250) {C().lastSweep=now;EvictReleased();}
        nextSweep.store(now+250,std::memory_order_relaxed);
    } catch(...) {}
}
InstancePatch PatchInstanceCopy(D3D12_RAYTRACING_INSTANCE_DESC* destination,const D3D12_RAYTRACING_INSTANCE_DESC* source,
    size_t count,bool hairTraced) noexcept {
    InstancePatch result;
    if(!destination||!source||count>UINT32_MAX)return {0,false};
    SweepReleased();
    std::array<D3D12_RAYTRACING_INSTANCE_DESC,64> hair;std::array<uint32_t,64> where;
    for(size_t at=0;at<count;) {
        bool faulted=false;
        const size_t found=ScanHair(source,at,count,hair.data(),where.data(),hair.size(),faulted);
        if(found) {
            if(!PrepareInstances(std::span(hair.data(),found),hairTraced))
                for(size_t k=0;k<found;++k)hair[k].InstanceMask=0;
            if(!WriteHair(destination,hair.data(),where.data(),found))return {result.hair,false};
            result.hair+=found;
        }
        if(faulted)return {result.hair,false};
    }
    return result;
}
namespace {
// Vtable data and its QueryInterface belong to the system or verified Agility
// runtime (debug layer included): not an application or overlay wrapper.
bool NativeObject(IUnknown* object) {
    return object&&Readable(object,sizeof(void*))&&NativeVtable(object);
}
}
bool ResolveNativeDevice(IUnknown* object,ComPtr<ID3D12Device5>& out) noexcept try {
    out.Reset();
    if(!object)return false;
    ComPtr<ID3D12Device5> candidate;
    if(single_overlay::native::Unwrap(object,candidate)&&NativeObject(candidate.Get())) {out=std::move(candidate);return true;}
    // The wrapper forwards object creation: a device child it does not wrap
    // (a fence) reports the native device that created it.
    ComPtr<ID3D12Device> wrapper;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12Device5> native;
    if(FAILED(object->QueryInterface(IID_PPV_ARGS(&wrapper)))
        ||FAILED(wrapper->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))||!NativeObject(fence.Get())
        ||FAILED(fence->GetDevice(IID_PPV_ARGS(&native)))||!NativeObject(native.Get()))return false;
    out=std::move(native);return true;
} catch(...) {return false;}
namespace {
// Opt-in device-removal report. The runtime signals every fence of a removed
// device to UINT64_MAX; this wait then records DRED and DOTS state before the
// game's own crash handler terminates the process.
struct RemovalWatch {
    ComPtr<ID3D12Device5> device;ComPtr<ID3D12Fence> fence;
    HANDLE event{},wait{};std::wstring path;std::atomic<bool> fired{};
};
const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static constexpr const char* names[]={"SetMarker","BeginEvent","EndEvent","DrawInstanced","DrawIndexedInstanced",
        "ExecuteIndirect","Dispatch","CopyBufferRegion","CopyTextureRegion","CopyResource","CopyTiles","ResolveSubresource",
        "ClearRenderTargetView","ClearUnorderedAccessView","ClearDepthStencilView","ResourceBarrier","ExecuteBundle","Present",
        "ResolveQueryData","BeginSubmission","EndSubmission","DecodeFrame","ProcessFrames","AtomicCopyBufferUint",
        "AtomicCopyBufferUint64","ResolveSubresourceRegion","WriteBufferImmediate","DecodeFrame1","SetProtectedResourceSession",
        "DecodeFrame2","ProcessFrames1","BuildRaytracingAccelerationStructure","EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure","DispatchRays","InitializeMetaCommand","ExecuteMetaCommand","EstimateMotion",
        "ResolveMotionVectorHeap","SetPipelineState1","InitializeExtensionCommand","ExecuteExtensionCommand","DispatchMesh",
        "EncodeFrame","ResolveEncoderOutputMetadata","Barrier","BeginCommandList","DispatchGraph","SetProgram"};
    const auto index=static_cast<size_t>(op);
    return index<std::size(names)?names[index]:"unknown op";
}
std::string Utf8(const char* narrow,const wchar_t* wide) {
    if(narrow)return std::string(narrow,strnlen(narrow,200));
    if(!wide)return {};
    const std::wstring text(wide,wcsnlen(wide,200));char out[1024]{};
    return WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),out,sizeof(out)-1,nullptr,nullptr)>0?out:"?";
}
struct ReportText {
    std::string text;
    template<class... A> void Line(const char* format,A... args) {
        char line[1024]{};_snprintf_s(line,_TRUNCATE,format,args...);text+=line;text+="\r\n";
    }
};
struct Range { const char* kind;uint64_t begin,bytes;size_t index; };
void Allocations(ReportText& r,const char* title,const D3D12_DRED_ALLOCATION_NODE1* node) {
    size_t n=0;
    for(;node&&n<48;node=node->pNext,++n)
        r.Line("  %s %p type=%d '%s'",title,node->pObject,static_cast<int>(node->AllocationType),Utf8(node->ObjectNameA,node->ObjectNameW).c_str());
    if(node)r.Line("  %s list truncated",title);
}
// Lists every submission the GPU had not finished, with the commands around
// the first unfinished one and any marker/event text recorded there.
std::string CallerText(const void* pso) {
    const void* caller=pso?FindCaller(pso):nullptr;
    if(!caller)return "unknown";
    HMODULE module{};wchar_t path[MAX_PATH]{};char text[400]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(caller),&module)
        ||!GetModuleFileNameW(module,path,MAX_PATH)) {_snprintf_s(text,_TRUNCATE,"%p",caller);return text;}
    const wchar_t* leaf=wcsrchr(path,L'\\');
    _snprintf_s(text,_TRUNCATE,"%s+0x%llX",Utf8(nullptr,leaf?leaf+1:path).c_str(),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(caller)-reinterpret_cast<uintptr_t>(module)));
    return text;
}
// Dispatch diagnostics: maps the first unfinished command (or the last
// dispatch before it) to the logged recording with the same dispatch count.
void DispatchDetails(ReportText& r,const D3D12_AUTO_BREADCRUMB_NODE1* node,UINT done) {
    if(!node->pCommandList||!node->pCommandHistory)return;
    UINT before=0,total=0;
    for(UINT i=0;i<node->BreadcrumbCount;++i)
        if(node->pCommandHistory[i]==D3D12_AUTO_BREADCRUMB_OP_DISPATCH) {if(i<done)++before;++total;}
    const bool atDispatch=done<node->BreadcrumbCount&&node->pCommandHistory[done]==D3D12_AUTO_BREADCRUMB_OP_DISPATCH;
    if(!total||(!atDispatch&&!before))return;
    const UINT k=atDispatch?before:before-1;
    std::unique_lock lock(C().lock,std::defer_lock);
    for(int i=0;i<40&&!lock.try_lock();++i)Sleep(5);
    if(!lock.owns_lock()) {r.Line("  dispatch details: runtime busy; not captured");return;}
    const auto* s=List(node->pCommandList);
    if(!s) {r.Line("  dispatch details: list not tracked");return;}
    bool matched=false;std::string seen;
    for(const auto& slot:s->dispatchLogs) {
        const auto* log=slot.load(std::memory_order_acquire);if(!log)continue;
        const uint32_t count=log->count.load(std::memory_order_acquire);
        seen+=" "+std::to_string(count);
        if(count!=total)continue;
        matched=true;
        r.Line("  %s dispatch %u of %u (recording generation %llu)%s",atDispatch?"hung":"hang follows",k,total,
            static_cast<unsigned long long>(log->generation),count>log->records.size()?" [log truncated]":"");
        const UINT available=std::min<UINT>(total,static_cast<UINT>(log->records.size()));
        for(UINT j=k>3?k-3:0;j<available&&j<=k+3;++j) {
            const auto& d=log->records[j];
            r.Line("    %s #%u pso=%p root=%p groups=%u,%u,%u%s caller=%s",j==k?">>":"  ",j,d.pso,d.root,d.x,d.y,d.z,
                d.injected?" (DOTS converter)":"",CallerText(d.pso).c_str());
        }
    }
    if(!matched)r.Line("  dispatch details: no logged recording of this list has %u dispatches (logged:%s)",total,seen.c_str());
}
void Breadcrumbs(ReportText& r,const D3D12_AUTO_BREADCRUMB_NODE1* head) {
    size_t nodes=0,incomplete=0;
    for(auto* node=head;node&&nodes<512;node=node->pNext,++nodes) {
        const UINT count=node->BreadcrumbCount,done=node->pLastBreadcrumbValue?*node->pLastBreadcrumbValue:0;
        if(done>=count)continue;
        ++incomplete;
        r.Line("INCOMPLETE list=%p '%s' queue=%p '%s' completed=%u of %u dotsTracked=%d",node->pCommandList,
            Utf8(node->pCommandListDebugNameA,node->pCommandListDebugNameW).c_str(),node->pCommandQueue,
            Utf8(node->pCommandQueueDebugNameA,node->pCommandQueueDebugNameW).c_str(),done,count,node->pCommandList&&List(node->pCommandList)?1:0);
        if(!node->pCommandHistory)continue;
        const UINT from=done>16?done-16:0,to=std::min(count,done+32);
        for(UINT i=from;i<to;++i) {
            std::string context;
            for(UINT c=0;c<node->BreadcrumbContextsCount&&node->pBreadcrumbContexts;++c)
                if(node->pBreadcrumbContexts[c].BreadcrumbIndex==i)context=Utf8(nullptr,node->pBreadcrumbContexts[c].pContextString);
            r.Line("  %s %5u %s%s%s",i==done?">>":"  ",i,OpName(node->pCommandHistory[i]),context.empty()?"":" : ",context.c_str());
        }
        if(dispatchDiagnostics.load(std::memory_order_relaxed))DispatchDetails(r,node,done);
    }
    r.Line("DRED breadcrumb lists=%zu incomplete=%zu",nodes,incomplete);
}
void Dred(ReportText& r,ID3D12Device5* device,uint64_t& faultVa) {
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {r.Line("DRED unavailable (not enabled before device creation)");return;}
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    const HRESULT breadcrumbs=dred->GetAutoBreadcrumbsOutput1(&crumbs);
    r.Line("DRED breadcrumbs hr=0x%08X",static_cast<unsigned>(breadcrumbs));
    if(SUCCEEDED(breadcrumbs))Breadcrumbs(r,crumbs.pHeadAutoBreadcrumbNode);
    ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
    if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2)))) {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 fault{};
        const HRESULT faultResult=dred2->GetPageFaultAllocationOutput2(&fault);
        r.Line("DRED page fault hr=0x%08X va=0x%016llX flags=0x%X deviceState=%d",static_cast<unsigned>(faultResult),
            fault.PageFaultVA,static_cast<unsigned>(fault.PageFaultFlags),static_cast<int>(dred2->GetDeviceState()));
        if(SUCCEEDED(faultResult)) {
            faultVa=fault.PageFaultVA;
            Allocations(r,"existing",fault.pHeadExistingAllocationNode);Allocations(r,"recently-freed",fault.pHeadRecentFreedAllocationNode);
        }
    } else {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        const HRESULT faultResult=dred->GetPageFaultAllocationOutput1(&fault);
        r.Line("DRED page fault hr=0x%08X va=0x%016llX",static_cast<unsigned>(faultResult),fault.PageFaultVA);
        if(SUCCEEDED(faultResult)) {
            faultVa=fault.PageFaultVA;
            Allocations(r,"existing",fault.pHeadExistingAllocationNode);Allocations(r,"recently-freed",fault.pHeadRecentFreedAllocationNode);
        }
    }
}
void DotsState(ReportText& r,uint64_t faultVa) {
    auto& ctx=C();std::unique_lock lock(ctx.lock,std::defer_lock);
    for(int i=0;i<40&&!lock.try_lock();++i)Sleep(5);
    if(!lock.owns_lock()) {r.Line("DOTS state: runtime busy; not captured");return;}
    const auto& s=ctx.stats;const uint64_t now=GetTickCount64();
    r.Line("DOTS builds=%llu updates=%llu prebuilds=%llu rejected=%llu evictions=%llu instanceCopies=%llu leaseReuses=%llu geometryMiB=%llu lost=%d",
        s.builds,s.updates,s.prebuilds,s.rejected,s.evictions,s.instanceCopies,s.leaseReuses,s.geometryBytes>>20,s.lost?1:0);
    r.Line("DOTS last build %lld ms ago; last admitted hair %lld ms ago (%u instances)",
        s.lastBuildTick?static_cast<long long>(now-s.lastBuildTick):-1ll,s.lastHairTick?static_cast<long long>(now-s.lastHairTick):-1ll,s.hairInstances);
    if(const uint64_t changed=settingChangeTick.load(std::memory_order_relaxed))
        r.Line("DOTS saw the game's Path Traced Hair turn %s %lld ms ago",settingChangeOn.load(std::memory_order_relaxed)?"on":"off",
            static_cast<long long>(now-changed));
    std::vector<Range> ranges;
    for(size_t i=0;i<ctx.associations.size();++i) {
        const auto& entry=ctx.associations[i];if(!entry.owner)continue;
        r.Line("association %zu owner=%p blas=0x%016llX+%llu segments=%u vertices=%u flags=0x%X gameHolds=%d",i,entry.owner,
            entry.address,entry.bytes,entry.segments,entry.vertices,entry.flags,GameHolds(entry)?1:0);
        ranges.push_back({"hair BLAS",entry.address,entry.bytes,i});
        if(entry.positions)ranges.push_back({"hair positions",entry.positions->GetGPUVirtualAddress(),entry.positions->GetDesc().Width,i});
        if(entry.indices)ranges.push_back({"hair indices",entry.indices->GetGPUVirtualAddress(),entry.indices->GetDesc().Width,i});
    }
    for(size_t i=0;i<ctx.leases.size();++i) {
        const auto& lease=ctx.leases[i];if(!lease.vertices&&!lease.blas)continue;
        const uint64_t base=lease.vertices?lease.vertices->GetGPUVirtualAddress():0;
        r.Line("lease %zu vertices=0x%016llX+%llu list=%p generation=%llu queue=%p fence=%llu poisoned=%d blas=0x%016llX",i,base,lease.capacity,
            lease.list,lease.generation,lease.queue,lease.fenceValue,lease.poisoned?1:0,lease.blas?lease.blas->GetGPUVirtualAddress():0ull);
        if(lease.vertices)ranges.push_back({"converted vertices",base,lease.capacity,i});
        if(lease.scratch)ranges.push_back({"hair scratch",lease.scratch->GetGPUVirtualAddress(),lease.scratch->GetDesc().Width,i});
    }
    if(!faultVa)return;
    bool matched=false;
    for(const auto& range:ranges)if(faultVa>=range.begin&&faultVa-range.begin<range.bytes) {
        r.Line("page fault lies in DOTS %s (entry %zu, offset %llu)",range.kind,range.index,faultVa-range.begin);matched=true;
    }
    if(!matched)r.Line("page fault lies outside every DOTS-held range");
}
void WriteRemovalReport(RemovalWatch& watch) {
    const HRESULT reason=watch.device->GetDeviceRemovedReason();
    if(reason==S_OK)return;
    ReportText r;SYSTEMTIME time{};GetLocalTime(&time);
    r.Line("RTXMFG Witcher DOTS device-removal report");
    r.Line("time=%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu reason=0x%08X",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,
        time.wSecond,time.wMilliseconds,GetCurrentProcessId(),static_cast<unsigned>(reason));
    uint64_t faultVa=0;
    Dred(r,watch.device.Get(),faultVa);
    DotsState(r,faultVa);
    const HANDLE file=CreateFileW(watch.path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD written=0;WriteFile(file,r.text.data(),static_cast<DWORD>(r.text.size()),&written,nullptr);
    FlushFileBuffers(file);CloseHandle(file);
    wchar_t line[600]{};
    swprintf_s(line,L"WITCHER_DOTS device removed reason=0x%08X report=%s",static_cast<unsigned>(reason),watch.path.c_str());
    single_module::Log(line);
}
void CALLBACK Removed(void* parameter,BOOLEAN) {
    auto* watch=static_cast<RemovalWatch*>(parameter);
    if(watch->fired.exchange(true))return;
    try {WriteRemovalReport(*watch);} catch(...) {}
}
}
bool EnableDispatchDiagnostics() noexcept {
    if(C().device)return false; // Lists may already be instrumented without Dispatch.
    dispatchDiagnostics.store(true,std::memory_order_relaxed);return true;
}
void NoteSettingChange(bool on) noexcept {
    settingChangeOn.store(on,std::memory_order_relaxed);settingChangeTick.store(GetTickCount64(),std::memory_order_relaxed);
}
bool ArmRemovalReport(ID3D12Device5* device,const std::wstring& path) noexcept try {
    if(!device||path.empty())return false;
    auto watch=std::make_unique<RemovalWatch>();
    watch->device=device;watch->path=path;
    watch->event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!watch->event||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&watch->fence)))
        ||FAILED(watch->fence->SetEventOnCompletion(UINT64_MAX,watch->event))
        ||!RegisterWaitForSingleObject(&watch->wait,watch->event,&Removed,watch.get(),INFINITE,WT_EXECUTEONLYONCE|WT_EXECUTELONGFUNCTION)) {
        if(watch->event)CloseHandle(watch->event);
        return false;
    }
    // Lives for the process: the wait can fire at any time until exit.
    watch.release();
    removalReportArmed.store(true,std::memory_order_release);
    return true;
} catch(...) {return false;}
}
