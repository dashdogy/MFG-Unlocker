// slprov.1 / genswap.1: XeMFG behind the game's own Streamline (streamline_provider.h).
#include "streamline_provider.h"
#include "streamline_provider_policy.h"
#include "standalone_policy.h"
#include "d3d12_provider.h"
#include "d3d12_host.h"
#include "xemfg_host_common.h"
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace xess_streamline_provider
{
namespace
{
using xemfg_host::Log;

struct Feature
{
    // First, as NVSDK_NGX_Handle's Id (the SDK only declares that type): the handle Streamline holds is this object's
    // address.
    unsigned int id = 0;
    void* provider = nullptr;    // xess_d3d12_provider's feature
};

std::atomic<bool> gSwitch{false}, gRefused{false}, gRouteLoaded{false}, gPrepared{false};
std::atomic<uint32_t> gKey{0};
std::atomic<uint32_t> gLive{0}, gNextId{kFirstHandleId}, gRefusalLogs{0};
// An evaluation holds it shared for its whole call and a release exclusively: a feature is never freed under its
// evaluation.
std::shared_mutex gFeatures;
FeatureTable<Feature, kMaxFeatures> gTable;

// genswap.1, for the menu: when XeFG last generated and last released a feature (GetTickCount64; 0: never), the last
// batch's presented frames per game frame, the control path's swap state, and the frame rates.
std::atomic<uint64_t> gGeneratedMs{0}, gReleasedMs{0}, gBatches{0}, gOutputs{0};
std::atomic<uint32_t> gFrames{0};
std::atomic<bool> gSwitching{false}, gStuck{false};
std::mutex gFpsMutex;
xemfg_standalone::FpsWindow gFps;

NVSDK_NGX_Parameter* Mutable(const NVSDK_NGX_Parameter* parameters) noexcept
{
    return const_cast<NVSDK_NGX_Parameter*>(parameters);   // NGX's Get is not const; nothing here writes through it
}

// XeFG could not take a feature: NVIDIA's provider keeps the game's DLSS-G, and XeMFG goes back to the takeover.
void Refuse(const std::string& fields) noexcept
{
    const bool first = !gRefused.exchange(true);
    if (gRefusalLogs.fetch_add(1) < 8)
        Log("streamline_provider_refused", fields + ",\"falls_back\":\"nvidia_provider\"");
    if (first) xess_d3d12::RouteChanged("streamline_refused");
}

// One evaluation: DLSSG.MultiFrameIndex 1 starts a game frame's batch of DLSSG.MultiFrameCount generated frames.
void Count(const NVSDK_NGX_Parameter* parameters, bool generated) noexcept
{
    unsigned int index = 1, count = 1;
    NVSDK_NGX_Parameter* p = Mutable(parameters);
    if (NVSDK_NGX_FAILED(p->Get("DLSSG.MultiFrameIndex", &index))) index = 1;
    if (NVSDK_NGX_FAILED(p->Get("DLSSG.MultiFrameCount", &count)) || !count) count = 1;
    const uint64_t now = GetTickCount64();
    if (generated) gGeneratedMs.store(now);
    gOutputs.fetch_add(1);
    if (index > 1) return;
    gFrames.store(count + 1);
    const uint64_t batches = gBatches.fetch_add(1) + 1;
    gOutputs.fetch_add(1);   // the game's own frame of this batch
    std::lock_guard lock(gFpsMutex);
    gFps.Add(now, batches, gOutputs.load());
}
}

void Configure(bool switchOn, uint32_t key) noexcept
{
    if (key > uint32_t(Key::eOff)) key = uint32_t(Key::eAuto);
    const bool switched = gSwitch.exchange(switchOn) != switchOn;
    const bool keyed = gKey.exchange(key) != key;
    if (switched || keyed)
        Log("streamline_provider_config", std::string("\"switch\":") + (switchOn ? "true" : "false") + ",\"key\":" +
            std::to_string(key) + ",\"wanted\":" + (Wanted() ? "true" : "false") + ",\"route_loaded\":" +
            (gRouteLoaded.load() ? "true" : "false"));
}

uint32_t KeyValue() noexcept { return gKey.load(); }

bool Wanted() noexcept { return gSwitch.load() && gKey.load() != uint32_t(Key::eOff) && !gRefused.load(); }

bool Refused() noexcept { return gRefused.load(); }

uint32_t Live() noexcept { return gLive.load(); }

uint64_t SinceReleaseMs() noexcept
{
    const uint64_t released = gReleasedMs.load();
    if (!released) return UINT64_MAX;
    const uint64_t now = GetTickCount64();
    return now >= released ? now - released : 0;
}

void NoteRouteLoaded() noexcept
{
    if (!gRouteLoaded.exchange(true)) Log("streamline_provider_route", "\"loaded\":true");
}

bool RouteLoaded() noexcept { return gRouteLoaded.load(); }

bool Prewarm(ID3D12Device* device) noexcept
{
    if (!device || gRefused.load()) return false;
    if (gPrepared.load()) return true;
    const int32_t result = xess_d3d12_provider::Init(device);
    if (static_cast<NVSDK_NGX_Result>(result) == NVSDK_NGX_Result_Success) {
        gPrepared.store(true);
        return true;
    }
    // The caller takes the chain over itself (at its creation, or by moving it), so no route change is announced.
    gRefused.store(true);
    Log("streamline_provider_refused", "\"at\":\"prewarm\",\"result\":" + std::to_string(result) + ",\"falls_back\":\"takeover\"");
    return false;
}

bool Prepared() noexcept { return gPrepared.load(); }

void NoteSwap(bool holding, bool stuck) noexcept
{
    gSwitching.store(holding);
    gStuck.store(stuck);
}

Status ReadStatus() noexcept
{
    Status status;
    const uint64_t now = GetTickCount64();
    const uint64_t generated = gGeneratedMs.load();
    status.live = gLive.load();
    status.generating = status.live && generated && now - generated < 1000;
    status.frames = status.live ? gFrames.load() : 0;
    status.switching = gSwitching.load();
    status.stuck = gStuck.load();
    status.batches = gBatches.load();
    std::lock_guard lock(gFpsMutex);
    status.baseFpsMilli = gFps.baseMilli;
    status.outputFpsMilli = gFps.outputMilli;
    status.fpsAgeMs = gFps.sampleMs && now >= gFps.sampleMs ? now - gFps.sampleMs : UINT64_MAX;
    return status;
}

bool TryCreate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output,
               int32_t& result) noexcept
{
    if (!Wanted() || !list || !output) return false;
    void* provider = nullptr;
    const int32_t created = xess_d3d12_provider::CreateFeature(list, Mutable(parameters), &provider);
    if (static_cast<NVSDK_NGX_Result>(created) != NVSDK_NGX_Result_Success || !provider) {
        Refuse("\"result\":" + std::to_string(created));
        return false;
    }
    try {
        auto feature = std::make_unique<Feature>();
        feature->id = gNextId.fetch_add(1);
        feature->provider = provider;
        const unsigned int id = feature->id;
        {
            std::unique_lock lock(gFeatures);
            if (!gTable.Add(feature.get())) {
                lock.unlock();
                xess_d3d12_provider::ReleaseFeature(provider);
                Refuse("\"reason\":\"too_many_features\"");
                return false;
            }
            gLive.fetch_add(1);
        }
        gPrepared.store(true);
        *output = reinterpret_cast<NVSDK_NGX_Handle*>(feature.release());
        result = static_cast<int32_t>(NVSDK_NGX_Result_Success);
        Log("streamline_provider_feature", "\"id\":" + std::to_string(id) + ",\"live\":" + std::to_string(gLive.load()));
        return true;
    } catch (...) {
        xess_d3d12_provider::ReleaseFeature(provider);
        Refuse("\"reason\":\"exception\"");
        return false;
    }
}

bool TryEvaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                 int32_t& result) noexcept
{
    if (!gLive.load() || !handle) return false;   // every other feature's evaluation stops here
    std::shared_lock lock(gFeatures);
    Feature* feature = gTable.Find(handle);
    if (!feature) return false;
    result = xess_d3d12_provider::Evaluate(list, feature->provider, Mutable(parameters));
    if (parameters) Count(parameters, static_cast<NVSDK_NGX_Result>(result) == NVSDK_NGX_Result_Success);
    return true;
}

bool TryRelease(NVSDK_NGX_Handle* handle, int32_t& result) noexcept
{
    if (!gLive.load() || !handle) return false;
    Feature* feature = nullptr;
    {
        std::unique_lock lock(gFeatures);
        feature = gTable.Find(handle);
        if (!feature) return false;
        gTable.Remove(handle);
        gLive.fetch_sub(1);
    }
    const unsigned int id = feature->id;
    result = xess_d3d12_provider::ReleaseFeature(feature->provider);
    delete feature;
    gReleasedMs.store(GetTickCount64());
    Log("streamline_provider_released", "\"id\":" + std::to_string(id) + ",\"result\":" + std::to_string(result) +
        ",\"live\":" + std::to_string(gLive.load()));
    return true;
}

bool Holds(const NVSDK_NGX_Handle* handle) noexcept
{
    if (!gLive.load() || !handle) return false;
    std::shared_lock lock(gFeatures);
    return gTable.Find(handle) != nullptr;
}

void Decline(const char* reason) noexcept
{
    try {
        Refuse(std::string("\"reason\":\"") + (reason ? reason : "") + "\"");
    } catch (...) {}
}
}
