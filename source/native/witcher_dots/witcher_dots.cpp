#include "witcher_dots.h"
#include "gpu_runtime.h"
#include "game_profile.h"
#include "game_discovery.h"
#include "checked_memory.h"
#include "../protected_pointer.h"
#include "../overlay_native.h"
#include "../single_module.h"
#include "../diagnostic_paths.h"
#include "../dlss5.h"
#include "../rtx_hair/universal_hair.h"
#include <MinHook.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <climits>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace witcher_dots {
using Microsoft::WRL::ComPtr;
namespace {
struct State {
    std::mutex lock;
    std::wstring executable,directory,control;
    Snapshot snapshot{};
    HMODULE game{};
    ComPtr<ID3D12Device5> device;
    ComPtr<IUnknown> identity;
    ShaderCache shaders;
    std::vector<ComPtr<IUnknown>> deviceAliasRefs;
    ComPtr<IDXGIAdapter3> memoryAdapter;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{},shared{};
    uint64_t memorySampled{};
    bool preferencesRead{},attempted{},dredEnabled{};
};
State& S() {static auto* const state=new State;return *state;}
std::atomic<bool> active{};
std::atomic<uint32_t> rejectionLogs{};
std::atomic<bool> fallbackShown{};
std::atomic<bool> converted{};
// Wrapper pointers proven to front the prepared device (kept alive in State).
std::array<std::atomic<void*>,4> deviceAliases{};
std::atomic<bool> settingKnown{};
std::atomic<const profile::GameProfile*> gameProfile{};
std::atomic<uint64_t> buildTicks{},copyTicks{},copyCalls{},instancesScanned{};
// Diagnostics: the game's hair builder (its own work and DOTS's hooks inside
// it), hair prebuild/build calls, and the longest gap between instance copies
// (the game copies its TLAS instances every frame).
std::atomic<uint64_t> builderCalls{},builderTicks{},prebuildCalls{},buildCalls{};
std::atomic<int64_t> lastCopyQpc{},copyGapMax{};
// Where the builder's wall time goes: before the prebuild hook, between the
// hooks, after the build hook, and inside DOTS's two hooks; the CPU cycles its
// thread actually executed (QueryThreadCycleTime, TSC reference cycles); which
// threads run it and at what priority; and the busy share of the thread that
// copies TLAS instances (the game's render thread), sampled when it logs.
std::atomic<uint64_t> builderSampled{},builderPreTicks{},builderMidTicks{},builderPostTicks{},builderHookTicks{},builderCycles{};
std::atomic<uint64_t> builderOnCopyThread{},builderGateTicks{}; // gate: DOTS's own owner/context checks before the builder
std::atomic<uint32_t> copyThread{},builderThreadOverflow{};
std::array<std::atomic<uint32_t>,16> builderThreads{};
std::atomic<int> builderPriorityMin{INT_MAX},builderPriorityMax{INT_MIN};
std::atomic<uint64_t> tscOrigin{};std::atomic<int64_t> qpcOrigin{};
std::atomic<int> loggedSetting{-1};
std::atomic<uint32_t> settingLogs{};
// When the setting returns mid-game the game recreates all of its hair, in
// more than one burst; hair re-enters ray tracing once that has settled.
constexpr uint64_t kSettleMs=2000;
std::atomic<uint64_t> traceAfterTick{};
int64_t QpcNow() noexcept {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
struct HookTimer {
    std::atomic<uint64_t>& total;LARGE_INTEGER start{};
    explicit HookTimer(std::atomic<uint64_t>& counter) noexcept:total(counter) {QueryPerformanceCounter(&start);}
    ~HookTimer() {LARGE_INTEGER end{};QueryPerformanceCounter(&end);total.fetch_add(static_cast<uint64_t>(end.QuadPart-start.QuadPart),std::memory_order_relaxed);}
};
uint64_t Microseconds(uint64_t ticks) noexcept {
    LARGE_INTEGER frequency{};
    if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return 0;
    const uint64_t hz=static_cast<uint64_t>(frequency.QuadPart);
    return ticks/hz*1000000+ticks%hz*1000000/hz;
}
// The game's own Path Traced Hair state (profile-validated config variables).
// Off: DOTS declines hair builds and keeps hair out of ray tracing, so
// HairWorks behaves as on a stock RTX 30/40 series GPU.
bool GameHairTraced() noexcept {
    if(!settingKnown.load(std::memory_order_acquire))return false;
    const auto* base=reinterpret_cast<const std::byte*>(S().game);
    const auto* game=gameProfile.load(std::memory_order_acquire);
    return game&&*reinterpret_cast<const volatile uint8_t*>(base+game->ptEnable.value)!=0
        &&*reinterpret_cast<const volatile int32_t*>(base+game->ptHairQuality.value)>0;
}
bool HairTracedNow() noexcept {
    const bool on=GameHairTraced();const int value=on?1:0;
    const int previous=loggedSetting.exchange(value,std::memory_order_relaxed);
    if(previous==value)return on;
    NoteSettingChange(on);
    if(on&&previous==0)traceAfterTick.store(GetTickCount64()+kSettleMs,std::memory_order_relaxed);
    if(settingLogs.fetch_add(1,std::memory_order_relaxed)<32)
        single_module::Log(on?(previous==0?L"WITCHER_DOTS game Path Traced Hair on: converting hair; ray tracing hair after 2 s"
                :L"WITCHER_DOTS game Path Traced Hair on: converting hair")
            :L"WITCHER_DOTS game Path Traced Hair off: the game builds no hair BLAS, HairWorks stays raster");
    return on;
}
// One cost line per 30 s (bounded), so a log shows where DOTS spends time.
std::atomic<uint64_t> nextCostLog{};
std::atomic<uint32_t> costLogs{};
void LogCost() noexcept {
    const uint64_t now=GetTickCount64();uint64_t due=nextCostLog.load(std::memory_order_relaxed);
    if(!due) {nextCostLog.compare_exchange_strong(due,now+30000);return;}
    if(now<due||!nextCostLog.compare_exchange_strong(due,now+30000)||costLogs.fetch_add(1,std::memory_order_relaxed)>=240)return;
    static uint64_t lastTick,lastBuild,lastCopy,lastCalls,lastInstances,lastWait,lastHeld,lastTables,lastTableUs; // Only the CAS winner reaches here.
    const uint64_t build=Microseconds(buildTicks.load()),copy=Microseconds(copyTicks.load());
    const auto stats=ReadRuntimeStats();
    const uint64_t wait=Microseconds(stats.buildLockWaitTicks),held=Microseconds(stats.buildLockHeldTicks);
    const uint64_t calls=copyCalls.load(),instances=instancesScanned.load();
    const uint64_t tableUs=Microseconds(stats.tableChangeTicks);
    // Dedicated video memory against the OS budget: past it the game's
    // resources page over PCIe (low GPU use on smaller cards).
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};bool vram=false;
    {
        ComPtr<IDXGIAdapter3> adapter;{std::lock_guard lock(S().lock);adapter=S().memoryAdapter;}
        vram=adapter&&SUCCEEDED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local));
    }
    const double seconds=lastTick?(now-lastTick)/1000.0:30.0;
    wchar_t line[640]{};
    swprintf_s(line,L"WITCHER_DOTS cost window=%.0fs buildMsPerS=%.2f (lockWait=%.2f underLock=%.2f) builds=%llu copyMsPerS=%.2f copiesPerS=%.0f instancesPerS=%.0f hairTraced=%d lists=%u releasedLists=%llu roots=%u releasedRoots=%llu tableChanges=%llu tableMs=%.1f tableFailures=%llu vramMiB=%lld/%lld",
        seconds,(build-lastBuild)/1000.0/seconds,(wait-lastWait)/1000.0/seconds,(held-lastHeld)/1000.0/seconds,
        static_cast<unsigned long long>(stats.builds),(copy-lastCopy)/1000.0/seconds,(calls-lastCalls)/seconds,(instances-lastInstances)/seconds,
        GameHairTraced()?1:0,stats.trackedLists,static_cast<unsigned long long>(stats.releasedLists),
        stats.trackedRoots,static_cast<unsigned long long>(stats.releasedRoots),
        static_cast<unsigned long long>(stats.tableChanges-lastTables),(tableUs-lastTableUs)/1000.0,static_cast<unsigned long long>(stats.tableFailures),
        vram?static_cast<long long>(local.CurrentUsage>>20):-1ll,vram?static_cast<long long>(local.Budget>>20):-1ll);
    single_module::Log(line);
    lastTick=now;lastBuild=build;lastCopy=copy;lastCalls=calls;lastInstances=instances;lastWait=wait;lastHeld=held;
    lastTables=stats.tableChanges;lastTableUs=tableUs;
}
// One diagnostic line per 10 s (bounded, 2 h): whether DOTS sits on the game's
// critical path. listOverheadNs is DOTS's own work per instrumented list call
// (sampled, timer cost removed), runtimeNs the runtime/driver call itself;
// builderMsPerS is the game's whole hair build including DOTS's hooks.
std::atomic<uint64_t> nextDiagLog{};
std::atomic<uint32_t> diagLogs{};
void LogDiagnostics() noexcept {
    const uint64_t now=GetTickCount64();uint64_t due=nextDiagLog.load(std::memory_order_relaxed);
    static uint64_t lastTick; // Only a CAS winner reaches the statics; windows are 10 s apart.
    static RuntimeStats last{};
    static uint64_t lastBuilderCalls,lastBuilderUs,lastPrebuilds,lastBuildCalls,lastCopies,lastBuildUs,lastCopyUs,lastGateUs;
    const auto remember=[&](const RuntimeStats& stats) {
        lastTick=now;last=stats;lastBuilderCalls=builderCalls.load(std::memory_order_relaxed);
        lastBuilderUs=Microseconds(builderTicks.load(std::memory_order_relaxed));
        lastPrebuilds=prebuildCalls.load(std::memory_order_relaxed);lastBuildCalls=buildCalls.load(std::memory_order_relaxed);
        lastCopies=copyCalls.load(std::memory_order_relaxed);lastBuildUs=Microseconds(buildTicks.load(std::memory_order_relaxed));
        lastCopyUs=Microseconds(copyTicks.load(std::memory_order_relaxed));lastGateUs=Microseconds(builderGateTicks.load(std::memory_order_relaxed));
    };
    if(!due) {
        if(nextDiagLog.compare_exchange_strong(due,now+10000)) {remember(ReadRuntimeStats());copyGapMax.store(0,std::memory_order_relaxed);}
        return;
    }
    if(now<due||!nextDiagLog.compare_exchange_strong(due,now+10000)||diagLogs.fetch_add(1,std::memory_order_relaxed)>=720)return;
    const auto stats=ReadRuntimeStats();
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    const double tickNs=frequency.QuadPart>0?1e9/static_cast<double>(frequency.QuadPart):0.0;
    const double seconds=lastTick?(now-lastTick)/1000.0:10.0;
    const uint64_t calls=stats.listCalls-last.listCalls,samples=stats.listSamples-last.listSamples;
    const double overheadTicks=static_cast<double>(stats.listOverheadTicks-last.listOverheadTicks)-2.0*stats.qpcCostTicks*samples;
    const double overheadNs=samples?std::max(0.0,overheadTicks)*tickNs/samples:0.0;
    const double runtimeNs=samples?static_cast<double>(stats.listOriginalTicks-last.listOriginalTicks)*tickNs/samples:0.0;
    const double listMs=overheadNs*calls/1e6/seconds;
    const double executeMs=static_cast<double>(stats.executeOverheadTicks-last.executeOverheadTicks)*tickNs/1e6/seconds;
    const uint64_t builders=builderCalls.load(std::memory_order_relaxed),builderUs=Microseconds(builderTicks.load(std::memory_order_relaxed));
    const uint64_t prebuilds=prebuildCalls.load(std::memory_order_relaxed),builds=buildCalls.load(std::memory_order_relaxed);
    const uint64_t copies=copyCalls.load(std::memory_order_relaxed);
    const uint64_t buildUs=Microseconds(buildTicks.load(std::memory_order_relaxed)),copyUs=Microseconds(copyTicks.load(std::memory_order_relaxed));
    const uint64_t gateUs=Microseconds(builderGateTicks.load(std::memory_order_relaxed));
    const double hooksMs=((buildUs-lastBuildUs)+(copyUs-lastCopyUs)+(gateUs-lastGateUs))/1000.0/seconds;
    const int64_t gapTicks=copyGapMax.exchange(0,std::memory_order_relaxed);
    const uint64_t converted=stats.builds-last.builds,updates=stats.updates-last.updates;
    wchar_t line[1200]{}; // Truncating: an unexpected value never reaches the invalid-parameter handler.
    _snwprintf_s(line,_TRUNCATE,L"WITCHER_DOTS diag window=%.0fs dotsMsPerS=%.2f (hooks=%.2f list=%.2f execute=%.3f) listCallsPerS=%.0f listOverheadNs=%.1f listRuntimeNs=%.0f listOutliers=%llu "
        L"executesPerS=%.0f listsPerS=%.0f hairExecutesPerS=%.1f builderPerS=%.1f builderMsPerS=%.2f prebuildsPerS=%.1f buildsPerS=%.1f "
        L"convertedFullPerS=%.1f convertedUpdatesPerS=%.1f rejectsPerS=%.1f rebuilds=%llu evictions=%llu owners=%u hairAsMiB=%.1f scratchMiB=%.1f "
        L"poolMiB=%.1f copiesPerS=%.1f copyGapMaxMs=%.1f traced=%d lost=%d",
        seconds,hooksMs+listMs+executeMs,hooksMs,listMs,executeMs,calls/seconds,overheadNs,runtimeNs,
        static_cast<unsigned long long>(stats.listOutliers-last.listOutliers),
        (stats.executeCalls-last.executeCalls)/seconds,(stats.executeLists-last.executeLists)/seconds,(stats.executeHair-last.executeHair)/seconds,
        (builders-lastBuilderCalls)/seconds,(builderUs-lastBuilderUs)/1000.0/seconds,(prebuilds-lastPrebuilds)/seconds,(builds-lastBuildCalls)/seconds,
        (converted-updates)/seconds,updates/seconds,(stats.rejected-last.rejected)/seconds,
        static_cast<unsigned long long>(stats.fullRebuilds-last.fullRebuilds),static_cast<unsigned long long>(stats.evictions-last.evictions),
        stats.liveOwners,stats.hairBlasBytes/1048576.0,stats.hairScratchBytes/1048576.0,stats.geometryBytes/1048576.0,
        (copies-lastCopies)/seconds,gapTicks*tickNs/1e6,GameHairTraced()?1:0,stats.lost?1:0);
    single_module::Log(line);
    // Builder detail. Called from the instance copy, so this thread is the one
    // copying TLAS instances; its executed cycles give its busy share.
    {
        static uint64_t lastSampled,lastPre,lastMid,lastPost,lastHook,lastCycles,lastOnCopy,lastThreadCycles;static DWORD lastThread;
        const uint64_t sampled=builderSampled.load(std::memory_order_relaxed),pre=builderPreTicks.load(std::memory_order_relaxed);
        const uint64_t mid=builderMidTicks.load(std::memory_order_relaxed),post=builderPostTicks.load(std::memory_order_relaxed);
        const uint64_t hook=builderHookTicks.load(std::memory_order_relaxed),cycles=builderCycles.load(std::memory_order_relaxed);
        const uint64_t onCopy=builderOnCopyThread.load(std::memory_order_relaxed);
        double tscHz=0.0;
        if(const int64_t origin=qpcOrigin.load(std::memory_order_relaxed);origin&&frequency.QuadPart>0) {
            const double elapsed=static_cast<double>(QpcNow()-origin)/static_cast<double>(frequency.QuadPart);
            if(elapsed>1.0)tscHz=static_cast<double>(__rdtsc()-tscOrigin.load(std::memory_order_relaxed))/elapsed;
        }
        ULONG64 threadCycles{};QueryThreadCycleTime(GetCurrentThread(),&threadCycles);const DWORD thread=GetCurrentThreadId();
        const double threadBusy=tscHz>0&&lastThread==thread&&threadCycles>lastThreadCycles
            ?100.0*static_cast<double>(threadCycles-lastThreadCycles)/tscHz/seconds:-1.0;
        uint32_t threads=0;
        for(auto& slot:builderThreads)if(slot.exchange(0,std::memory_order_relaxed))++threads;
        const uint32_t overflow=builderThreadOverflow.exchange(0,std::memory_order_relaxed);
        const int low=builderPriorityMin.exchange(INT_MAX,std::memory_order_relaxed),high=builderPriorityMax.exchange(INT_MIN,std::memory_order_relaxed);
        const uint64_t calls=sampled-lastSampled;
        const auto ms=[&](uint64_t ticks) {return static_cast<double>(ticks)*tickNs/1e6/seconds;};
        _snwprintf_s(line,_TRUNCATE,L"WITCHER_DOTS diag builder calls=%llu dotsGateMsPerS=%.3f wallMsPerS=%.2f (beforePrebuild=%.2f dotsHooks=%.2f between=%.2f afterBuild=%.2f) "
            L"cpuMsPerS=%.2f threads=%u%s onCopyThread=%.0f%% priority=%d..%d copyThreadBusy=%.0f%% tscMHz=%.0f",
            static_cast<unsigned long long>(calls),(gateUs-lastGateUs)/1000.0/seconds,ms((pre-lastPre)+(hook-lastHook)+(mid-lastMid)+(post-lastPost)),ms(pre-lastPre),ms(hook-lastHook),
            ms(mid-lastMid),ms(post-lastPost),tscHz>0?static_cast<double>(cycles-lastCycles)/tscHz*1000.0/seconds:-1.0,
            threads,overflow?L"+":L"",calls?100.0*static_cast<double>(onCopy-lastOnCopy)/static_cast<double>(calls):0.0,
            calls?low:0,calls?high:0,threadBusy,tscHz/1e6);
        single_module::Log(line);
        lastSampled=sampled;lastPre=pre;lastMid=mid;lastPost=post;lastHook=hook;lastCycles=cycles;lastOnCopy=onCopy;
        lastThreadCycles=threadCycles;lastThread=thread;
    }
    lastTick=now;last=stats;lastBuilderCalls=builders;lastBuilderUs=builderUs;lastPrebuilds=prebuilds;lastBuildCalls=builds;
    lastCopies=copies;lastBuildUs=buildUs;lastCopyUs=copyUs;lastGateUs=gateUs;
    static std::atomic_flag legend=ATOMIC_FLAG_INIT;
    if(!legend.test_and_set()) {
        _snwprintf_s(line,_TRUNCATE,L"WITCHER_DOTS diag timer cost=%.1f ns per QueryPerformanceCounter (removed from listOverheadNs); one list call in 32 per thread is timed",
            stats.qpcCostTicks*tickNs);
        single_module::Log(line);
    }
}
using BuilderFn=int32_t(WINAPI*)(void*,const void*);
using PrebuildFn=int32_t(WINAPI*)(ID3D12Device5*,const PrebuildParams*);
using BuildFn=int32_t(WINAPI*)(ID3D12GraphicsCommandList4*,const BuildParams*);
using CopyFn=uintptr_t(WINAPI*)(void*,const void*,size_t);
BuilderFn originalBuilder{};PrebuildFn originalPrebuild{};BuildFn originalBuild{};CopyFn originalCopy{};
std::array<void*,4> gameTargets{};
std::array<bool,4> gameEnabled{};
struct OwnerScope {
    void* owner{};ID3D12GraphicsCommandList4* list{};bool havePrebuild{};
    int64_t prebuildIn{},prebuildOut{},buildIn{},buildOut{}; // QPC marks set by the hair hooks (diagnostics)
};
thread_local OwnerScope* ownerScope{};
// Marks a hair hook's entry and exit in the current builder scope.
class HookMarks {
    int64_t* out{};
public:
    HookMarks(int64_t OwnerScope::* in,int64_t OwnerScope::* exit) noexcept {
        if(auto* scope=ownerScope) {scope->*in=QpcNow();out=&(scope->*exit);}
    }
    ~HookMarks() {if(out)*out=QpcNow();}
    HookMarks(const HookMarks&)=delete;HookMarks& operator=(const HookMarks&)=delete;
};
// One builder call in the hair path: wall-time segments, executed cycles,
// thread identity and priority.
class BuilderSample {
    const OwnerScope& scope;int64_t entry{};ULONG64 cycles{};
public:
    explicit BuilderSample(const OwnerScope& s) noexcept:scope(s) {
        int64_t expected=0;
        if(!qpcOrigin.load(std::memory_order_relaxed)) {
            const uint64_t tsc=__rdtsc();const int64_t qpc=QpcNow();
            if(qpcOrigin.compare_exchange_strong(expected,qpc))tscOrigin.store(tsc,std::memory_order_relaxed);
        }
        QueryThreadCycleTime(GetCurrentThread(),&cycles);entry=QpcNow();
    }
    ~BuilderSample() {
        const int64_t exit=QpcNow();ULONG64 end{};QueryThreadCycleTime(GetCurrentThread(),&end);
        const auto add=[](std::atomic<uint64_t>& total,int64_t ticks) {if(ticks>0)total.fetch_add(static_cast<uint64_t>(ticks),std::memory_order_relaxed);};
        builderSampled.fetch_add(1,std::memory_order_relaxed);
        if(end>cycles)builderCycles.fetch_add(end-cycles,std::memory_order_relaxed);
        if(scope.prebuildIn) {
            add(builderPreTicks,scope.prebuildIn-entry);add(builderHookTicks,scope.prebuildOut-scope.prebuildIn);
            if(scope.buildIn) {
                add(builderMidTicks,scope.buildIn-scope.prebuildOut);add(builderHookTicks,scope.buildOut-scope.buildIn);
                add(builderPostTicks,exit-scope.buildOut);
            } else add(builderPostTicks,exit-scope.prebuildOut);
        } else add(builderPreTicks,exit-entry);
        const DWORD thread=GetCurrentThreadId();
        if(thread==copyThread.load(std::memory_order_relaxed))builderOnCopyThread.fetch_add(1,std::memory_order_relaxed);
        bool recorded=false;
        for(auto& slot:builderThreads) {
            uint32_t seen=slot.load(std::memory_order_relaxed);
            if(seen==thread) {recorded=true;break;}
            if(!seen&&slot.compare_exchange_strong(seen,thread,std::memory_order_relaxed)) {recorded=true;break;}
            if(seen==thread) {recorded=true;break;}
        }
        if(!recorded)builderThreadOverflow.fetch_add(1,std::memory_order_relaxed);
        const int priority=GetThreadPriority(GetCurrentThread());
        int low=builderPriorityMin.load(std::memory_order_relaxed),high=builderPriorityMax.load(std::memory_order_relaxed);
        while(priority<low&&!builderPriorityMin.compare_exchange_weak(low,priority,std::memory_order_relaxed)) {}
        while(priority>high&&!builderPriorityMax.compare_exchange_weak(high,priority,std::memory_order_relaxed)) {}
    }
    BuilderSample(const BuilderSample&)=delete;BuilderSample& operator=(const BuilderSample&)=delete;
};
void Preferences() {
    auto& state=S();if(state.preferencesRead)return;state.preferencesRead=true;
    std::wstring path(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n||n>=path.size())return;path.resize(n);
    state.executable=path;state.directory=std::filesystem::path(path).parent_path().wstring();
    state.control=(std::filesystem::path(state.directory)/L"RTXMFG.WitcherDOTS.ini").wstring();
    state.snapshot.applicable=_wcsicmp(std::filesystem::path(path).filename().c_str(),L"witcher3.exe")==0;
    if(!state.snapshot.applicable)return;
    state.game=GetModuleHandleW(nullptr);state.snapshot.processId=GetCurrentProcessId();
    // Always prepared in Witcher 3: the game's own Path Traced Hair setting
    // decides whether hair is traced. RTXMFG_WITCHER_DOTS=0 is a
    // troubleshooting off switch.
    state.snapshot.requested=true;
    state.snapshot.crashReportRequested=GetPrivateProfileIntW(L"WitcherDOTS",L"CrashReport",0,state.control.c_str())==1;
    // Issue 70 A/B switch: RoundHitDistance=0 keeps the ribbon hit distance.
    // Read once: the hair shaders are translated at preparation.
    SetRoundHitDistance(GetPrivateProfileIntW(L"WitcherDOTS",L"RoundHitDistance",1,state.control.c_str())!=0);
    // Hair translucency, 0-100 % (default 65, issue 70 stopgap; 0 = the game's own look).
    {
        const int percent=static_cast<int>(GetPrivateProfileIntW(L"WitcherDOTS",L"HairTranslucency",65,state.control.c_str()));
        SetHairTranslucency(static_cast<uint32_t>(percent<0?0:percent>100?100:percent));
        // Colour-aware (default): pale hair gets the full value, dark hair less.
        const int white=static_cast<int>(GetPrivateProfileIntW(L"WitcherDOTS",L"HairTranslucencyWhitePoint",35,state.control.c_str()));
        SetHairTranslucencyColour(GetPrivateProfileIntW(L"WitcherDOTS",L"HairTranslucencyByColour",1,state.control.c_str())!=0,
            static_cast<uint32_t>(white<5?5:white>100?100:white));
        // HairBounces: the path tracer's path length for hair pixels (the game's 4).
        const int bounces=static_cast<int>(GetPrivateProfileIntW(L"WitcherDOTS",L"HairBounces",4,state.control.c_str()));
        SetHairBounces(static_cast<uint32_t>(bounces<1?1:bounces>32?32:bounces));
        // HairAlbedoLinear: treat HairAlbedoBuffer as linear colour (default 1, issue 70 stopgap; 0 = the game's gamma decode).
        SetHairAlbedoLinear(GetPrivateProfileIntW(L"WitcherDOTS",L"HairAlbedoLinear",1,state.control.c_str())!=0);
        // HairIgnoreReflectionMask: hair pixels read the RT-reflection mask as 0 (default 0: the game's).
        SetHairIgnoreReflectionMask(GetPrivateProfileIntW(L"WitcherDOTS",L"HairIgnoreReflectionMask",0,state.control.c_str())!=0);
        // HairDebugLighting (diagnostic, 0-5): isolates the hair lighting terms.
        SetHairDebugLighting(GetPrivateProfileIntW(L"WitcherDOTS",L"HairDebugLighting",0,state.control.c_str()));
        // HairStencilCharacter: the path tracer sees stencil bit 2 on hair pixels (default 0: the game's).
        SetHairStencilCharacter(GetPrivateProfileIntW(L"WitcherDOTS",L"HairStencilCharacter",0,state.control.c_str())!=0);
    }
    wchar_t env[8]{};const DWORD count=GetEnvironmentVariableW(L"RTXMFG_WITCHER_DOTS",env,_countof(env));
    if(count==1&&env[0]==L'0') {state.snapshot.environmentOverride=true;state.snapshot.requested=false;}
    // rtxhair.1: universal hair takes Witcher 3's hair through the game's own NVAPI LSS path
    // instead; this game-specific path stands down.
    if(rtx_hair::universal::Requested()) {
        state.snapshot.requested=false;
        strcpy_s(state.snapshot.reason,"universal hair on (RTXMFG.RtxHair.ini [RtxHair] Universal=0 turns it off)");
        single_module::Log(L"WITCHER_DOTS off: universal hair is on (RTXMFG.RtxHair.ini [RtxHair] Universal=0 turns it off)");
    }
    state.snapshot.stage=state.snapshot.requested?Stage::WaitingForDevice:Stage::Disabled;
}
void Reason(Stage stage,std::string_view reason) {
    auto& state=S();std::lock_guard lock(state.lock);state.snapshot.stage=stage;
    const auto size=std::min(reason.size(),sizeof(state.snapshot.reason)-1);
    memcpy(state.snapshot.reason,reason.data(),size);state.snapshot.reason[size]=0;
    wchar_t line[512]{};swprintf_s(line,L"WITCHER_DOTS stage=%S reason=%S",StageText(stage),state.snapshot.reason);
    single_module::Log(line);
}
void RejectReason(std::string_view error) {
    auto& state=S();std::lock_guard lock(state.lock);
    // Log each change of reason (bounded) so a later blocker is not hidden
    // behind repeats of the first one.
    if(error.substr(0,sizeof(state.snapshot.reason)-1)!=std::string_view(state.snapshot.reason)
        &&rejectionLogs.fetch_add(1,std::memory_order_relaxed)<24) {
        wchar_t line[512]{};const std::string text(error.substr(0,256));
        swprintf_s(line,L"WITCHER_DOTS raster-fallback reason=%S",text.c_str());single_module::Log(line);
    }
    const size_t size=std::min(error.size(),sizeof(state.snapshot.reason)-1);
    memcpy(state.snapshot.reason,error.data(),size);state.snapshot.reason[size]=0;
    fallbackShown.store(true,std::memory_order_release);
}
// A rejection stays on screen until the next successful conversion, which
// keeps it only as the last fallback instead of the current state. The first
// conversion also retires the "waiting for game hair" preparation text.
void ConversionSucceeded() {
    const bool first=!converted.exchange(true,std::memory_order_acq_rel);
    if(!fallbackShown.exchange(false,std::memory_order_acq_rel)) {
        if(first) {auto& state=S();std::lock_guard lock(state.lock);state.snapshot.reason[0]=0;}
        return;
    }
    auto& state=S();std::lock_guard lock(state.lock);
    char text[sizeof(state.snapshot.reason)]{};
    _snprintf_s(text,_TRUNCATE,"last fallback: %s",state.snapshot.reason);
    strcpy_s(state.snapshot.reason,text);
}
bool At(const void* caller,uint32_t rva) {return caller==reinterpret_cast<const std::byte*>(S().game)+rva;}
// The identified build's profile (set once by VerifyProfile, before any hook).
const profile::GameProfile& Game() {return *gameProfile.load(std::memory_order_acquire);}
bool DeviceMatches(ID3D12Device5* device) {
    if(!device)return false;
    for(const auto& alias:deviceAliases)if(alias.load(std::memory_order_acquire)==device)return true;
    ComPtr<ID3D12Device5> native;ComPtr<IUnknown> identity;
    if(!ResolveNativeDevice(device,native)||FAILED(native.As(&identity))||identity.Get()!=S().identity.Get())return false;
    // Resolution through a wrapper may create a fence: do it once per front.
    auto& state=S();std::lock_guard lock(state.lock);
    if(state.deviceAliasRefs.size()<deviceAliases.size()) {
        const size_t slot=state.deviceAliasRefs.size();state.deviceAliasRefs.emplace_back(device);
        deviceAliases[slot].store(device,std::memory_order_release);
    }
    return true;
}
// The builder runs for every hair owner every frame, mostly on the render
// thread. VirtualQuery sizes the region by walking its page-table entries:
// in the game's large heap regions that cost 1.2 ms per call in play and 5 ms
// after a save load (about 60% of a core, starving the GPU). The
// owner range (under a page) is instead probed under a structured-exception
// guard: reading its first and last fields proves both pages it can touch are
// committed and readable. Owners are game heap objects passed by the game's
// own builder, and every later owner read is guarded the same way (ReadFast).
bool OwnerReadable(const void* owner,size_t size) noexcept {
    if(size<8||size>4096)return Readable(owner,size);
    uint64_t first{},last{};
    return owner&&!(reinterpret_cast<uintptr_t>(owner)&7)&&ReadFast(owner,0,first)&&ReadFast(owner,size-8,last);
}
int32_t WINAPI Builder(void* owner,const void* context) {
    if(!active.load(std::memory_order_acquire))return originalBuilder(owner,context);
    builderCalls.fetch_add(1,std::memory_order_relaxed);HookTimer builder(builderTicks);
    uint32_t version{};ID3D12GraphicsCommandList4* list{};
    bool accepted{};
    {
        HookTimer gate(builderGateTicks);
        accepted=OwnerReadable(owner,Game().owner.size)&&ReadFast(context,0,version)&&version==0x201&&ReadFast(context,8,list)&&list;
    }
    if(!accepted)return originalBuilder(owner,context);
    OwnerScope scope{owner,list,false};const auto previous=ownerScope;ownerScope=&scope;
    struct RestoreScope {OwnerScope* previous;~RestoreScope(){ownerScope=previous;}} restore{previous};
    BuilderSample sample(scope);
    return originalBuilder(owner,context);
}
int32_t WINAPI Prebuild(ID3D12Device5* device,const PrebuildParams* supplied) try {
    if(!active.load(std::memory_order_acquire)||!At(_ReturnAddress(),Game().hairCalls[0].ret))return originalPrebuild(device,supplied);
    HookTimer timer(buildTicks);prebuildCalls.fetch_add(1,std::memory_order_relaxed);
    HookMarks marks(&OwnerScope::prebuildIn,&OwnerScope::prebuildOut);
    // The game asks for hair only when its own predicate says so; converting
    // what it asks avoids leaving hair in its raster fallback across a load or
    // a settings change. The setting is read only for status and logging.
    HairTracedNow();
    PrebuildParams params{};ExtendedInputs inputs{};HairInput hair;std::string error;
    if(!ownerScope||!DeviceMatches(device)||!CopyFast(&params,supplied,sizeof(params))||params.version!=0x10018
        ||!CopyFast(&inputs,params.inputs,sizeof(inputs))||!params.info
        ||!ReadHairInput(ownerScope->owner,inputs,hair,error)) {
        RejectReason(error.empty()?"prebuild caller/device/layout not established":error);return -1;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    if(!PrebuildTriangles(hair,inputs.flags,info)||!CopyFast(params.info,&info,sizeof(info))) {
        RejectReason("triangle prebuild refused (capacity, or conversions stopped: see the tracking lost line)");return -1;
    }
    ownerScope->havePrebuild=true;return 0;
} catch(...) {StopConversions();return -1;
}
int32_t WINAPI Build(ID3D12GraphicsCommandList4* list,const BuildParams* supplied) try {
    if(!active.load(std::memory_order_acquire)||!At(_ReturnAddress(),Game().hairCalls[1].ret))return originalBuild(list,supplied);
    HookTimer timer(buildTicks);buildCalls.fetch_add(1,std::memory_order_relaxed);
    HookMarks marks(&OwnerScope::buildIn,&OwnerScope::buildOut);
    HairTracedNow();
    BuildParams params{};ExtendedBuild desc{};HairInput hair;std::string error;
    if(!ownerScope||list!=ownerScope->list||!CopyFast(&params,supplied,sizeof(params))
        ||params.version!=0x10020||params.postCount||params.post||!CopyFast(&desc,params.desc,sizeof(desc))
        ||!ReadHairInput(ownerScope->owner,desc.inputs,hair,error)) {
        RejectReason(error.empty()?"build owner/prebuild/layout not established":error);return -1;
    }
    // An unchanged topology may skip the engine's prebuild query on an update.
    // BuildTriangles still requires the exact owned AS generation, fixed
    // topology/flags and independently sufficient AS/scratch allocations.
    if(!ownerScope->havePrebuild&&desc.inputs.flags!=0x27) {RejectReason("new hair AS has no triangle prebuild");return -1;}
    if(!BuildTriangles(std::move(hair),list,desc,error)) {RejectReason(error);return -1;}
    ConversionSucceeded();
    static std::atomic_flag first=ATOMIC_FLAG_INIT;
    if(!first.test_and_set())single_module::Log(L"WITCHER_DOTS first triangle BLAS recorded (GPU completion and game image not yet verified)");
    return 0;
} catch(...) {StopConversions();return -1;
}
uintptr_t WINAPI Copy(void* destination,const void* source,size_t bytes) {
    if(!active.load(std::memory_order_acquire)||!At(_ReturnAddress(),Game().hairCalls[2].ret))return originalCopy(destination,source,bytes);
    if(!bytes)return originalCopy(destination,source,bytes);
    constexpr size_t stride=sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if(bytes%stride||bytes>1024ull*1024*stride) {
        StopConversions();RejectReason("hair instance copy bounds not established");return originalCopy(destination,source,bytes);
    }
    // The game's copy runs unchanged and in parallel with its other copies;
    // only hair entries are then rewritten in the destination (the GPU reads
    // this upload memory only after the game submits).
    const auto copied=originalCopy(destination,source,bytes);
    {
        HookTimer timer(copyTicks); // DOTS's own work only, not the game's copy.
        copyCalls.fetch_add(1,std::memory_order_relaxed);instancesScanned.fetch_add(bytes/stride,std::memory_order_relaxed);
        copyThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
        if(const int64_t previous=lastCopyQpc.exchange(timer.start.QuadPart,std::memory_order_relaxed)) {
            const int64_t gap=timer.start.QuadPart-previous;int64_t longest=copyGapMax.load(std::memory_order_relaxed);
            while(gap>longest&&!copyGapMax.compare_exchange_weak(longest,gap,std::memory_order_relaxed)) {}
        }
        const bool traced=HairTracedNow()&&GetTickCount64()>=traceAfterTick.load(std::memory_order_relaxed);
        if(!PatchInstanceCopy(static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(destination),
            static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(source),bytes/stride,traced).ok) {
            StopConversions();RejectReason("hair instance copy failed");
        }
    }
    LogCost();LogDiagnostics();
    return copied;
}
int64_t ExecutableSize() {
    std::error_code failed;
    const auto size=std::filesystem::file_size(std::filesystem::path(S().executable),failed);
    return failed?-1:static_cast<int64_t>(size);
}
// A profiled build is admitted only by its exact profile; any other
// executable size is an update whose hair code is found by structure.
bool ProfiledSize(int64_t size) {
    for(const auto& game:profile::kProfiles)if(size==static_cast<int64_t>(game.exeSize))return true;
    return false;
}
void LogDiscovered() {
    const auto* found=profile::DiscoveredSites();
    if(!found)return;
    const auto& p=found->profile;
    wchar_t line[900]{};
    swprintf_s(line,L"WITCHER_DOTS hair code found by structure: game=%S executable=%lld bytes builder=0x%x prebuild=0x%x build=0x%x "
        L"copy=0x%x caps=0x%x lss=0x%x gates=0x%x,0x%x device=0x%x,0x%x owner=0x%x ptEnable=0x%x ptHair=0x%x "
        L"shaders=0x%x,0x%x,0x%x,0x%x time=%uus",
        p.label,static_cast<long long>(ExecutableSize()),p.entries[0].rva,p.entries[1].rva,p.entries[2].rva,p.entries[3].rva,p.entries[4].rva,
        found->lssByte,found->gates[0].rva,found->gates[1].rva,p.deviceReturns[0],p.deviceReturns[1],p.owner.scratch,
        p.ptEnable.object,p.ptHairQuality.object,p.shaders[0],p.shaders[1],p.shaders[2],p.shaders[3],found->microseconds);
    single_module::Log(line);
}
// A D3D12CreateDevice caller DOTS prepares on: the renderer call of a profiled
// build, or of an unprofiled build whose hair code was found. `unsupported` is
// set once discovery has ruled the running build out.
bool RendererCaller(const void* caller,std::string& unsupported) {
    auto& state=S();
    if(profile::KnownDeviceCaller(caller,state.game))return true;
    HMODULE owner{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCWSTR>(caller),&owner)||owner!=state.game)return false;
    const int64_t size=ExecutableSize();
    if(ProfiledSize(size))return false;
    std::string why;
    const auto* found=profile::DiscoveredProfile(state.game,why);
    if(!found) {
        char text[160]{};_snprintf_s(text,_TRUNCATE,"hair code not found (executable %lld bytes): %s",static_cast<long long>(size),why.c_str());
        unsupported=text;return false;
    }
    return At(caller,found->deviceReturns[0])||At(caller,found->deviceReturns[1]);
}
// Identifies the running build by its exact executable size and SHA-256, or
// else by its hair code found by structure (game_discovery.cpp), then
// validates every site in the loaded image.
bool VerifyProfile(const void* deviceCaller,std::string& error) {
    auto& state=S();
    std::ifstream file(std::filesystem::path(state.executable),std::ios::binary|std::ios::ate);
    const auto size=file?static_cast<int64_t>(file.tellg()):-1;
    const profile::GameProfile* selected=nullptr;std::vector<std::byte> data;
    for(const auto& game:profile::kProfiles) {
        if(size!=static_cast<int64_t>(game.exeSize))continue;
        if(data.empty()) {
            data.resize(game.exeSize);file.seekg(0);
            if(!file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()))) {error="game executable unreadable";return false;}
        }
        if(HashEquals(data,game.exeHash)) {selected=&game;break;}
    }
    std::string why;
    if(!selected&&size>0&&!ProfiledSize(size)&&(selected=profile::DiscoveredProfile(state.game,why))!=nullptr)LogDiscovered();
    if(!selected) {
        char text[160]{};_snprintf_s(text,_TRUNCATE,"unsupported game build (executable %lld bytes)%s%s",static_cast<long long>(size),
            why.empty()?"":": ",why.c_str());
        error=text;return false;
    }
    // The device was created from this build's own renderer call site.
    if(!At(deviceCaller,selected->deviceReturns[0])&&!At(deviceCaller,selected->deviceReturns[1])) {error="renderer device caller does not belong to this build";return false;}
    if(!profile::ValidateMapped(state.game,*selected,error))return false;
    SetHairOwnerLayout(selected->owner.scratch,selected->owner.blas,selected->owner.positions,selected->owner.indices);
    gameProfile.store(selected,std::memory_order_release);
    return true;
}
bool VerifyDetour(void* target,void* hook) {
    uint8_t code[5]{},relay[14]{};int32_t displacement{};
    if(!CopyChecked(code,target,sizeof(code))||code[0]!=0xe9)return false;
    memcpy(&displacement,code+1,sizeof(displacement));
    const auto destination=reinterpret_cast<uintptr_t>(target)+5+displacement;
    if(!CopyChecked(relay,reinterpret_cast<void*>(destination),sizeof(relay))
        ||memcmp(relay,"\xff\x25\0\0\0\0",6))return false;
    uintptr_t address{};memcpy(&address,relay+6,sizeof(address));
    return address==reinterpret_cast<uintptr_t>(hook);
}
bool InstallGameHooks(std::string& error) {
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED) {error="game hook relocator unavailable";return false;}
    const auto* base=reinterpret_cast<const std::byte*>(S().game);
    const std::array<void*,4> hooks{reinterpret_cast<void*>(&Builder),reinterpret_cast<void*>(&Prebuild),reinterpret_cast<void*>(&Build),reinterpret_cast<void*>(&Copy)};
    auto& targets=gameTargets;auto& enabled=gameEnabled;std::array<void*,4> originals{};
    for(size_t i=0;i<targets.size();++i) {
        targets[i]=const_cast<std::byte*>(base+Game().entries[i].rva);
        if(MH_CreateHook(targets[i],hooks[i],&originals[i])!=MH_OK) {error="game hook conflict or unsupported relocation";return false;}
    }
    originalBuilder=reinterpret_cast<BuilderFn>(originals[0]);originalPrebuild=reinterpret_cast<PrebuildFn>(originals[1]);
    originalBuild=reinterpret_cast<BuildFn>(originals[2]);originalCopy=reinterpret_cast<CopyFn>(originals[3]);
    for(size_t i=0;i<targets.size();++i) {
        const bool ok=MH_EnableHook(targets[i])==MH_OK;enabled[i]=ok;
        if(!ok||!VerifyDetour(targets[i],hooks[i])) {
            for(size_t j=0;j<=i;++j)if(enabled[j])MH_DisableHook(targets[j]);
            error="game hook publication/decoded-target verification failed";return false;
        }
    }
    return true;
}
void AbortPreparation() noexcept {
    active.store(false,std::memory_order_release);
    for(size_t i=0;i<gameTargets.size();++i)if(gameEnabled[i]) {
        if(MH_DisableHook(gameTargets[i])==MH_OK)gameEnabled[i]=false;
    }
    if(!AbortGpuPreparation())single_module::Log(L"WITCHER_DOTS preparation rollback indeterminate; forwarding bindings retained");
}
bool PublishGates(std::string& error) {
    return profile::PublishGates(reinterpret_cast<uintptr_t>(S().game),Game().gates,error);
}
// RTX 40 series is Ada (CUDA compute capability 8.9) and RTX 30 series is
// Ampere (8.6), desktop, laptop and workstation parts. 8.0 (GA100) has no RT
// cores. The CUDA device must match the game's D3D12 adapter LUID exactly once;
// any incomplete or ambiguous enumeration is not admitted.
bool SupportedAdapter(const LUID& luid,int& major,int& minor) noexcept {
    using InitFn=int(WINAPI*)(unsigned);using CountFn=int(WINAPI*)(int*);using DeviceFn=int(WINAPI*)(int*,int);
    using CapabilityFn=int(WINAPI*)(int*,int*,int);using LuidFn=int(WINAPI*)(char*,unsigned*,int);
    const HMODULE cuda=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!cuda)return false;
    const auto init=reinterpret_cast<InitFn>(GetProcAddress(cuda,"cuInit"));
    const auto getCount=reinterpret_cast<CountFn>(GetProcAddress(cuda,"cuDeviceGetCount"));
    const auto getDevice=reinterpret_cast<DeviceFn>(GetProcAddress(cuda,"cuDeviceGet"));
    const auto getCapability=reinterpret_cast<CapabilityFn>(GetProcAddress(cuda,"cuDeviceComputeCapability"));
    const auto getLuid=reinterpret_cast<LuidFn>(GetProcAddress(cuda,"cuDeviceGetLuid"));
    int count=0;uint32_t matches=0;
    bool complete=init&&getCount&&getDevice&&getCapability&&getLuid&&init(0)==0&&getCount(&count)==0&&count>0;
    for(int ordinal=0;complete&&ordinal<count;++ordinal) {
        int device=0,candidateMajor=0,candidateMinor=0;unsigned nodeMask=0;LUID candidate{};
        std::array<char,sizeof(LUID)> raw{};
        if(getDevice(&device,ordinal)!=0||getCapability(&candidateMajor,&candidateMinor,device)!=0
            ||getLuid(raw.data(),&nodeMask,device)!=0) {complete=false;break;}
        memcpy(&candidate,raw.data(),sizeof(candidate));
        if(candidate.LowPart==luid.LowPart&&candidate.HighPart==luid.HighPart) {++matches;major=candidateMajor;minor=candidateMinor;}
    }
    FreeLibrary(cuda);
    return complete&&matches==1&&major==8&&(minor==9||minor==6);
}
bool Adapter(ID3D12Device5* device,DXGI_ADAPTER_DESC1& desc,ComPtr<IDXGIAdapter3>& memory) {
    const HMODULE module=single_module::LoadSystemModule(L"dxgi.dll");
    using Fn=HRESULT(WINAPI*)(REFIID,void**);
    const auto create=reinterpret_cast<Fn>(GetProcAddress(module,"CreateDXGIFactory1"));
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
    if(!create||FAILED(create(IID_PPV_ARGS(&factory)))
        ||FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))||FAILED(adapter->GetDesc1(&desc)))return false;
    adapter.As(&memory); // Optional: VRAM figures for the overlay.
    return true;
}
}
void BeforeDeviceCreate(const void* caller) noexcept {
    try {
        auto& state=S();
        {
            std::lock_guard lock(state.lock);Preferences();
            if(!state.snapshot.applicable||!state.snapshot.requested||!state.snapshot.crashReportRequested||state.dredEnabled
                ||state.attempted||single_overlay::native::InsideLoader())return;
        }
        std::string unsupported;
        if(!RendererCaller(caller,unsupported))return;
        {
            std::lock_guard lock(state.lock);
            if(state.dredEnabled||state.attempted)return;
            state.dredEnabled=true;
        }
        // DRED applies only to devices created after it is configured.
        using DebugFn=HRESULT(WINAPI*)(REFIID,void**);
        const auto debug=reinterpret_cast<DebugFn>(GetProcAddress(single_module::LoadSystemModule(L"d3d12.dll"),"D3D12GetDebugInterface"));
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if(!debug||FAILED(debug(IID_PPV_ARGS(&dred)))) {
            {std::lock_guard lock(state.lock);state.dredEnabled=false;}
            single_module::Log(L"WITCHER_DOTS crash report unavailable: DRED settings interface missing");return;
        }
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        single_module::Log(L"WITCHER_DOTS crash report requested: DRED breadcrumbs, contexts and page faults enabled");
        if(EnableDispatchDiagnostics())
            single_module::Log(L"WITCHER_DOTS crash report: dispatches logged (pipeline, thread groups, issuing code)");
    } catch(...) {}
}
void ObserveDevice(IUnknown* object,const void* caller) noexcept {
    try {
        auto& state=S();
      {
        std::lock_guard lock(state.lock);Preferences();
        if(!state.snapshot.applicable||!state.snapshot.requested||state.attempted||single_overlay::native::InsideLoader())return;
      }
        // Outside the state lock: an unprofiled build's hair code is found
        // once per process by scanning the executable image.
        std::string unsupported;
        const bool renderer=RendererCaller(caller,unsupported);
        if(!renderer&&unsupported.empty())return;
      {
        std::lock_guard lock(state.lock);
        if(state.attempted)return;
        // An unsupported build stays unsupported for the whole process.
        state.attempted=true;state.snapshot.stage=renderer?Stage::Preparing:Stage::UnsupportedGame;
      }
        if(!renderer) {Reason(Stage::UnsupportedGame,unsupported);return;}
        std::string error;
        if(!VerifyProfile(caller,error)) {Reason(Stage::UnsupportedGame,error);return;}
        settingKnown.store(true,std::memory_order_release);
        if(!ResolveNativeDevice(object,state.device)||FAILED(state.device.As(&state.identity))) {
            Reason(Stage::Failed,"native D3D12 device ownership unavailable");return;
        }
        {
            ComPtr<IUnknown> outer;object->QueryInterface(IID_PPV_ARGS(&outer));
            if(outer.Get()!=state.identity.Get())single_module::Log(L"WITCHER_DOTS device reached through a wrapper; native device resolved");
        }
        if(state.dredEnabled) {
            const auto& directory=diagnostic_paths::LogDirectory();
            std::wstring path;
            if(!directory.empty())path=(std::filesystem::path(directory)/(L"RTXMFG-witcher3-device-removed-"+std::to_wstring(GetCurrentProcessId())+L".txt")).wstring();
            const bool armed=ArmRemovalReport(state.device.Get(),path);
            {std::lock_guard lock(state.lock);state.snapshot.crashReportArmed=armed;}
            wchar_t line[600]{};
            swprintf_s(line,L"WITCHER_DOTS crash report %s path=%s",armed?L"armed":L"unavailable",path.c_str());single_module::Log(line);
        }
        DXGI_ADAPTER_DESC1 adapter{};ComPtr<IDXGIAdapter3> memoryAdapter;
        if(!Adapter(state.device.Get(),adapter,memoryAdapter)) {Reason(Stage::Failed,"authoritative DXGI adapter lookup failed");return;}
        {std::lock_guard lock(state.lock);state.memoryAdapter=memoryAdapter;}
        SetMemoryAdapter(memoryAdapter.Get());
        {
            std::lock_guard lock(state.lock);state.snapshot.deviceId=adapter.DeviceId;state.snapshot.luid=state.device->GetAdapterLuid();
        }
        using CapsFn=int32_t(WINAPI*)(ID3D12Device*,uint32_t,void*,uint32_t);
        uint32_t lss{};
        const auto caps=reinterpret_cast<CapsFn>(reinterpret_cast<std::byte*>(state.game)+Game().entries[4].rva);
        int32_t capsStatus=caps(state.device.Get(),6,&lss,sizeof(lss));
        if(capsStatus==-4) { // NVAPI_API_NOT_INITIALIZED on an early loader route.
            using QueryFn=void*(__cdecl*)(uint32_t);using InitializeFn=int32_t(__cdecl*)();
            const auto module=single_module::LoadSystemModule(L"nvapi64.dll");
            const auto query=reinterpret_cast<QueryFn>(GetProcAddress(module,"nvapi_QueryInterface"));
            const auto initialize=query?reinterpret_cast<InitializeFn>(query(0x0150e828)):nullptr;
            if(initialize&&initialize()==0) {lss=0;capsStatus=caps(state.device.Get(),6,&lss,sizeof(lss));}
        }
        // NVAPI_NOT_SUPPORTED: the driver has no LSS capability record for this
        // GPU, which is the same answer as a cleared support bit.
        if(capsStatus==-104) {lss=0;capsStatus=0;}
        if(capsStatus!=0) {Reason(Stage::Failed,"native LSS capability query failed");return;}
        if(lss&1) {Reason(Stage::NativeLss,"native LSS supported; game path retained");return;}
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
        int major=0,minor=0;
        if(adapter.VendorId!=0x10de||!SupportedAdapter(state.device->GetAdapterLuid(),major,minor)) {
            char text[128]{};
            _snprintf_s(text,_TRUNCATE,"requires an RTX 30 or RTX 40 series GPU; adapter compute capability %d.%d",major,minor);
            Reason(Stage::UnsupportedGpu,text);return;
        }
        if(FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))
            ||options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1
            ||FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))||model.HighestShaderModel<D3D_SHADER_MODEL_6_6) {
            Reason(Stage::UnsupportedGpu,"requires DXR 1.1 and shader model 6.6");return;
        }
        const HMODULE nvapi=GetModuleHandleW(L"nvapi64.dll");
        using QueryFn=void*(__cdecl*)(uint32_t);using DriverFn=int32_t(__cdecl*)(uint32_t*,char*);
        const auto query=reinterpret_cast<QueryFn>(GetProcAddress(nvapi,"nvapi_QueryInterface"));
        uint32_t driver{};char branch[64]{};
        const auto driverFn=query?reinterpret_cast<DriverFn>(query(0x2926aaad)):nullptr;
        if(driverFn&&driverFn(&driver,branch)==0) {std::lock_guard lock(state.lock);state.snapshot.driverVersion=driver;}
        // Validated from 617.14 onward; any later release is admitted. The UMD
        // image itself is identified independently of release in gpu_runtime.
        if(driver<61714) {
            char text[128]{};
            _snprintf_s(text,_TRUNCATE,"requires NVIDIA driver 617.14 or later (found %u.%02u)",driver/100,driver%100);
            Reason(Stage::UnsupportedDriver,text);return;
        }
        if(!state.shaders.Prepare(state.game,Game().shaders,state.directory,error)) {Reason(Stage::Failed,error);return;}
        {std::lock_guard lock(state.lock);state.snapshot.shaderReady=true;}
        // Before any queue table hook: DLSS 5 hooks the runtime method behind it.
        dlss5::SetQueueForward(&QueueExecuteForward);
        if(!InitializeGpu(state.device.Get(),&state.shaders,error)||!InstallGameHooks(error)) {AbortPreparation();Reason(Stage::Failed,error);return;}
        // All shader translations, hooks, converter state and budgets are ready
        // before the first game gate is published. The logical getter is last.
        active.store(true,std::memory_order_release);
        if(!PublishGates(error)) {AbortPreparation();Reason(Stage::Failed,error);return;}
        wchar_t line[512]{};
        swprintf_s(line,L"WITCHER_DOTS prepared pid=%lu game=%S gameSHA256=%S gpu=%s vendor=%04x device=%04x capability=%d.%d luid=%08x:%08x driver=%u nativeLSS=0 fourTrianglesPerSegment=1 geometryBudgetMiB=512",
            GetCurrentProcessId(),Game().label,Game().exeHash,adapter.Description,adapter.VendorId,adapter.DeviceId,major,minor,
            static_cast<uint32_t>(state.device->GetAdapterLuid().HighPart),state.device->GetAdapterLuid().LowPart,driver);
        single_module::Log(line);
        single_module::Log(state.shaders.RoundHitApplied()
            ?L"WITCHER_DOTS hair hit distance: round strand 1.12x (RoundHitDistance=1; set 0 in RTXMFG.WitcherDOTS.ini for the ribbon distance)"
            :RoundHitDistance()?L"WITCHER_DOTS hair hit distance: ribbon (round strand not available for this game build)"
            :L"WITCHER_DOTS hair hit distance: ribbon (RoundHitDistance=0)");
        single_module::Log(state.shaders.AlbedoApplied()
            ?L"WITCHER_DOTS hair albedo: linear (HairAlbedoLinear=1 in RTXMFG.WitcherDOTS.ini; prepass and hit shader gamma-encode HairAlbedoBuffer before the game's pow 2.2 decode)"
            :HairAlbedoLinear()?L"WITCHER_DOTS hair albedo: game's gamma 2.2 decode (linear albedo not available for this game build)"
            :L"WITCHER_DOTS hair albedo: game's gamma 2.2 decode (HairAlbedoLinear=0)");
        single_module::Log(HairIgnoreReflectionMask()
            ?L"WITCHER_DOTS hair reflection mask: ignored on hair pixels (HairIgnoreReflectionMask=1 in RTXMFG.WitcherDOTS.ini; the path tracer reads 0 for hair)"
            :L"WITCHER_DOTS hair reflection mask: game's (hair pixels inherit the RT-reflection mask behind them; HairIgnoreReflectionMask=0)");
        single_module::Log(HairStencilCharacter()
            ?L"WITCHER_DOTS hair stencil: bit 2 set on hair pixels (HairStencilCharacter=1 in RTXMFG.WitcherDOTS.ini; the path tracer's bit-2 test sees hair as character)"
            :L"WITCHER_DOTS hair stencil: game's (hair pixels inherit the stencil bits behind them; HairStencilCharacter=0)");
        {
            static constexpr const wchar_t* kDebugModes[]={L"off",L"prepass local-light term off",L"prepass sun term off",
                L"path tracer output zeroed on hair pixels",L"prepass all-lights loop forced",L"prepass one random light forced"};
            wchar_t debugLine[220]{};
            swprintf_s(debugLine,L"WITCHER_DOTS hair debug lighting: %u = %s (HairDebugLighting in RTXMFG.WitcherDOTS.ini, diagnostic)",
                HairDebugLighting(),kDebugModes[HairDebugLighting()]);
            single_module::Log(debugLine);
        }
        {
            wchar_t translucency[640]{};
            wchar_t mode[96]{};
            if(HairTranslucencyByColour())swprintf_s(mode,L"colour-aware by hair albedo, white point %u%%; shadow passes colour-blind",HairTranslucencyWhitePoint());
            else wcscpy_s(mode,L"colour-blind (HairTranslucencyByColour=0)");
            swprintf_s(translucency,L"WITCHER_DOTS hair translucency: %u%% (%s; HairTranslucency in RTXMFG.WitcherDOTS.ini; prepass rays %s; %S)",
                HairTranslucency(),mode,state.shaders.TranslucencyApplied()?L"rewritten":L"unchanged",state.shaders.PathTracerState().c_str());
            single_module::Log(translucency);
            wchar_t bounceLine[200]{};
            swprintf_s(bounceLine,L"WITCHER_DOTS hair bounces: %u (HairBounces in RTXMFG.WitcherDOTS.ini; the game uses 4)",HairBounces());
            single_module::Log(bounceLine);
        }
        single_module::Log(L"WITCHER_DOTS stage=prepared; waiting for game hair (game Path Traced Hair setting)");
        {std::lock_guard lock(state.lock);state.snapshot.stage=Stage::Active;}
    } catch(const std::exception& e) {AbortPreparation();Reason(Stage::Failed,e.what());}
    catch(...) {AbortPreparation();Reason(Stage::Failed,"DOTS preparation failed");}
}
Snapshot ReadSnapshot() noexcept {
    Snapshot out{};
    try {
        {std::lock_guard lock(S().lock);Preferences();out=S().snapshot;}
        if(!out.applicable)return out;
        const auto stats=ReadRuntimeStats();out.prebuilds=stats.prebuilds;out.builds=stats.builds;out.updates=stats.updates;
        out.rejected=stats.rejected;out.shaderLibraries=stats.shaderLibraries;out.instanceCopies=stats.instanceCopies;out.geometryBytes=stats.geometryBytes;
        const uint64_t now=GetTickCount64();
        if(stats.lastBuildTick)out.lastBuildAgeMs=now-stats.lastBuildTick;
        if(stats.lastHairTick)out.lastHairAgeMs=now-stats.lastHairTick;
        out.evictions=stats.evictions;out.liveOwners=stats.liveOwners;out.hairInstances=stats.hairInstances;
        out.poolAllocations=stats.poolAllocations;out.poolReleases=stats.poolReleases;out.poolReturns=stats.reclaims;out.fullRebuilds=stats.fullRebuilds;
        out.hairBlasBytes=stats.hairBlasBytes;out.hairScratchBytes=stats.hairScratchBytes;
        out.buildMicroseconds=Microseconds(buildTicks.load(std::memory_order_relaxed));
        out.buildLockWaitMicroseconds=Microseconds(stats.buildLockWaitTicks);out.buildLockHeldMicroseconds=Microseconds(stats.buildLockHeldTicks);
        out.copyMicroseconds=Microseconds(copyTicks.load(std::memory_order_relaxed));
        out.copyCalls=copyCalls.load(std::memory_order_relaxed);out.instancesScanned=instancesScanned.load(std::memory_order_relaxed);
        out.gameHairTraced=out.stage==Stage::Active&&GameHairTraced();
        {
            auto& state=S();std::lock_guard lock(state.lock);
            if(state.memoryAdapter&&(!state.memorySampled||now-state.memorySampled>=500)) {
                state.memorySampled=now;
                if(FAILED(state.memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&state.local))
                    ||FAILED(state.memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&state.shared))) {
                    state.local={};state.shared={};
                }
            }
            out.memoryKnown=state.local.Budget!=0;
            out.vramUsage=state.local.CurrentUsage;out.vramBudget=state.local.Budget;out.sharedUsage=state.shared.CurrentUsage;
        }
        if(stats.lost) {out.stage=Stage::Failed;strcpy_s(out.reason,"GPU tracking/fence lost; new conversions stopped");}
    } catch(...) {out.stage=Stage::Failed;}
    return out;
}
bool SetCrashReportRequested(bool requested) noexcept {
    try {
        auto& state=S();std::lock_guard lock(state.lock);Preferences();
        if(!state.snapshot.applicable)return false;
        if(!WritePrivateProfileStringW(L"WitcherDOTS",L"CrashReport",requested?L"1":L"0",state.control.c_str()))return false;
        state.snapshot.crashReportRequested=requested;return true;
    } catch(...) {return false;}
}
const char* StageText(Stage stage) noexcept {
    switch(stage) {
    case Stage::Disabled:return "disabled";
    case Stage::WaitingForDevice:return "waiting for early device";
    case Stage::Preparing:return "preparing";
    case Stage::UnsupportedGame:return "unsupported game build";
    case Stage::UnsupportedGpu:return "unsupported GPU";
    case Stage::NativeLss:return "native LSS";
    case Stage::Failed:return "unavailable";
    case Stage::Active:return "prepared";
    case Stage::UnsupportedDriver:return "unsupported driver";
    }
    return "unknown";
}
const char* ActivityText(const Snapshot& snapshot) noexcept {
    if(snapshot.stage!=Stage::Active)return StageText(snapshot.stage);
    if(!snapshot.gameHairTraced)return "off (game Path Traced Hair setting)";
    // Admitted hair instances prove the game traced converted hair recently;
    // the TLAS copy may pause in menus.
    if(snapshot.lastHairAgeMs<2000)return "active (tracing converted hair)";
    if(snapshot.builds)return "idle (no hair traced in the last 2 s)";
    return "ready (waiting for game hair)";
}
bool GameProcess() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,path,_countof(path));
    if(!n||n>=_countof(path))return false;
    const auto* leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
    return !_wcsicmp(leaf,L"witcher3.exe");
}
}
