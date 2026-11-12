// Live DLSS-G preset switching. The switch policy, the
// frame-generation lifetime tracker, and the production dlssg_preset.cpp latch
// (entry_detour stubbed, so nothing is installed and no provider is loaded).
#include "dlssg_preset.cpp"
#include "dlssg_preset_switch_policy.h"
#include "fg_lifetime_tracker.h"

#include <iostream>
#include <thread>
#include <memory>

#include "entry_detour_stub.h"

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* text)
{
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
}

fg_lifetime::Tracker<4> gTracker;
bool TrackerQuiescent() noexcept { return gTracker.Quiescent(); }

void PolicyCases()
{
    using namespace dlssg_preset_switch;
    Inputs idle{};
    idle.pending = true;
    idle.supported = true;
    idle.gameWantsOn = true;
    idle.optionsKnown = true;
    idle.bridgeReady = true;
    Inputs in = idle;
    in.pending = false;
    Check(Decide(in) == Action::eNone, "nothing saved: no switch");
    in = idle;
    in.supported = false;
    Check(Decide(in) == Action::eNone, "unsupported provider or route keeps restart-to-apply");
    in = idle;
    in.quiescent = true;
    Check(Decide(in) == Action::eRelatch, "no live feature and no Create in flight: latch at once");
    in.gameWantsOn = false;
    Check(Decide(in) == Action::eRelatch, "frame generation off with its feature released: latch at once");
    in = idle;
    Check(Decide(in) == Action::eBeginDrain, "live feature: one forced re-create");
    in.optionsKnown = false;
    Check(Decide(in) == Action::eNone, "no captured game options: no forced Off that could not be resumed");
    in = idle;
    in.bridgeReady = false;
    Check(Decide(in) == Action::eNone, "control route not ready: no forced Off");
    in = idle;
    in.gameWantsOn = false;
    Check(Decide(in) == Action::eNone, "game Off with a retained feature waits for the game's next On");

    Inputs draining = idle;
    draining.phase = Phase::eDraining;
    in = draining;
    Check(Decide(in) == Action::eKeepDraining, "drain keeps Off until the feature is released");
    in.quiescent = true;
    Check(Decide(in) == Action::eRelatchAndResume, "released: latch, then the game's options again");
    in.pending = false;
    Check(Decide(in) == Action::eResume, "released after the selection returned: resume without a latch");
    in = draining;
    in.supported = false;
    Check(Decide(in) == Action::eKeepDraining, "support lost during the drain still waits for the release");
    in.quiescent = true;
    in.supported = false;
    Check(Decide(in) == Action::eAbort, "lost provider ownership aborts even after release");
    in = draining;
    in.elapsedMs = kDrainTimeoutMs - 1;
    Check(Decide(in) == Action::eKeepDraining, "drain before the timeout continues");
    in.elapsedMs = kDrainTimeoutMs;
    Check(Decide(in) == Action::eAbort, "drain timeout aborts and resumes");
    in = draining;
    in.bridgeReady = false;
    Check(Decide(in) == Action::eAbort, "a live feature without a ready route aborts");
    in.quiescent = true;
    Check(Decide(in) == Action::eRelatchAndResume, "an exact release permits a supported reset without a ready bridge");
}

void TrackerCases()
{
    fg_lifetime::Tracker<2> tracker;
    Check(tracker.Quiescent() && tracker.Read().exact, "new tracker is quiescent");
    {
        fg_lifetime::CreateScope scope(tracker, true);
        Check(!tracker.Quiescent() && tracker.Read().createsInFlight == 1, "a Create in flight is not quiescent");
        tracker.ObserveProviderCreate();
        Check(tracker.Read().exact, "a provider Create nested in a runtime Create stays exact");
        tracker.Created(0, 0x1000);
    }
    Check(!tracker.Quiescent() && tracker.Read().liveFeatures == 1, "the created feature is live after its Create");
    tracker.Released(1, 0x1000);
    Check(tracker.Read().liveFeatures == 1, "a release on another route does not retire the feature");
    tracker.Released(0, 0x2000);
    Check(tracker.Read().liveFeatures == 1, "a release of another handle does not retire the feature");
    tracker.Released(0, 0x1000);
    Check(tracker.Quiescent() && tracker.Read().trackedReleases == 1, "the exact release makes it quiescent");
    {
        fg_lifetime::CreateScope scope(tracker, false);
        Check(tracker.Quiescent(), "an untracked route's Create is not counted");
    }
    {
        fg_lifetime::CreateScope scope(tracker, true);
        tracker.Created(0, 0);
    }
    Check(tracker.Quiescent(), "a failed Create leaves nothing live");
    tracker.Created(0, 1);
    tracker.Created(1, 2);
    tracker.Created(0, 3);
    Check(!tracker.Read().exact, "a feature that cannot be recorded makes the tracker inexact");
    tracker.Released(0, 1);
    tracker.Released(1, 2);
    Check(!tracker.Quiescent(), "an inexact tracker never reports quiescence");

    fg_lifetime::Tracker<2> direct;
    direct.ObserveProviderCreate();
    Check(!direct.Read().exact && !direct.Quiescent(), "a provider Create outside every runtime Create is inexact");
}

void LatchCases()
{
    using namespace dlssg_preset;
    SetCreateQuiescenceProbe(&TrackerQuiescent);
    Check(SetRequested(kPresetB) && FreezeSelection() == kPresetB, "first provider read freezes Preset B");
    Check(!RelatchPending(), "nothing pending: no latch");
    Check(SetRequested(kPresetC) && ReadSelection().restartRequired && FreezeSelection() == kPresetB,
        "a later request is saved; reads keep Preset B");
    gTracker.Created(0, 0x10);
    Check(!RelatchPending() && FreezeSelection() == kPresetB, "a live feature keeps the frozen preset");
    {
        fg_lifetime::CreateScope scope(gTracker, true);
        gTracker.Released(0, 0x10);
        Check(!RelatchPending(), "a Create in flight blocks the latch");
    }
    Check(RelatchPending(), "no feature and no Create in flight: the saved request is latched");
    const auto latched = ReadSelection();
    Check(latched.latched == kPresetC && !latched.selectionFrozen && !latched.restartRequired,
        "latched Preset C is not frozen until the next read");
    Check(FreezeSelection() == kPresetC && ReadSelection().selectionFrozen, "the next Create reads and freezes Preset C");
    Check(SetRequested(kGameOrDriver) && RelatchPending() && FreezeSelection() == kGameOrDriver,
        "Game / driver can be latched live too");
    SetCreateQuiescenceProbe(nullptr);
    Check(SetRequested(kPresetA) && !RelatchPending(), "without a probe nothing is latched");
    SetCreateQuiescenceProbe(&TrackerQuiescent);
    Check(RelatchPending() && FreezeSelection() == kPresetA, "the probe restores latching");
}

void ReplayCases()
{
    using namespace dlssg_preset;
    const HMODULE provider = reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x7000000));
    bool found = true;
    uint32_t value = 0;
    Check(!ReplayDriverRead(provider, found, value), "no NVAPI read recorded: the reader's own value is used");
    RecordDriverRead(provider, kOk, 2, false);
    Check(!ReplayDriverRead(provider, found, value), "an unmodified read left the driver value in the cache");
    RecordDriverRead(provider, kSettingNotFound, 0, true);
    Check(ReplayDriverRead(provider, found, value) && !found, "an overridden missing setting replays as missing");
    RecordDriverRead(provider, kOk, 1, true);
    Check(ReplayDriverRead(provider, found, value) && found && value == 1,
        "an overridden driver setting replays the driver's value");
    Check(!ReplayDriverRead(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x7100000)), found, value),
        "each provider keeps its own record");
}

unsigned destroyed = 0;
struct FixtureConfiguration
{
    uint32_t preset = 0;
    ~FixtureConfiguration() { ++destroyed; }
};
std::shared_ptr<FixtureConfiguration> configuration;
bool createArguments = false, evaluateArguments = false;
uint32_t FixtureCreate(void* a, void* b, void* c, void* d, uintptr_t e, uintptr_t f)
{
    using namespace dlssg_preset;
    createArguments = a == reinterpret_cast<void*>(1) && b == reinterpret_cast<void*>(2)
        && c == reinterpret_cast<void*>(3) && d == reinterpret_cast<void*>(4) && e == 5 && f == 6;
    if (!configuration)
    {
        configuration = std::make_shared<FixtureConfiguration>();
        configuration->preset = FreezeSelection();
        RecordRead(GetModuleHandleW(nullptr), true, configuration->preset, true, ReadSource::eConfiguration);
    }
    // The case the latch exists for: the report sees the new request even
    // when Create reuses the configuration constructed on the first call.
    RecordRead(GetModuleHandleW(nullptr), true, FreezeSelection(), true, ReadSource::eReport);
    return 0x12345678;
}
uint32_t FixtureEvaluate(void* a, void* b, void* c, void* d)
{
    evaluateArguments = a == reinterpret_cast<void*>(1) && b == reinterpret_cast<void*>(2)
        && c == reinterpret_cast<void*>(3) && d == reinterpret_cast<void*>(4);
    return 0x87654321;
}

void ConfigurationCases()
{
    using namespace dlssg_preset;
    configuration = std::make_shared<FixtureConfiguration>();
    config::Layout layout{};
    layout.module = GetModuleHandleW(nullptr);
    layout.slot = reinterpret_cast<void* volatile*>(&configuration);
    const auto control = reinterpret_cast<uintptr_t>(layout.slot[1]);
    provider_image::SafeRead(control, layout.controlVtable);
    config::ResetState state{};
    Check(config::ReadResetState(layout, state), "single-owner make_shared configuration has the verified layout");
    {
        auto secondOwner = configuration;
        Check(!config::ReadResetState(layout, state), "a second shared owner refuses reset");
    }
    {
        std::weak_ptr<FixtureConfiguration> weak = configuration;
        Check(!config::ReadResetState(layout, state), "an external weak owner refuses reset");
    }
    auto wrong = layout;
    wrong.controlVtable += 8;
    Check(!config::ReadResetState(wrong, state), "wrong control-block vtable refuses reset");
    wrong = layout;
    void* foreign[2]{};
    wrong.slot = foreign;
    Check(!config::ReadResetState(wrong, state), "a slot outside the owned image refuses reset");
    Check(config::ReadResetState(layout, state), "exact ownership is restored");
    configuration = std::make_shared<FixtureConfiguration>();
    const auto replacement = configuration.get();
    Check(!config::Reset(layout, state) && configuration.get() == replacement,
        "expected-value CAS refuses a replaced object and preserves the replacement");
    Check(config::ReadResetState(layout, state), "replacement has exact ownership");
    const auto before = destroyed;
    Check(config::Reset(layout, state) && !configuration && configuration.use_count() == 1 && destroyed == before,
        "reset clears only the object word, retaining the provider's control block");
    configuration = std::make_shared<FixtureConfiguration>();
    Check(destroyed == before + 1, "the next provider assignment destroys the old configuration exactly once");
    configuration.reset();

    gReads = {};
    gSelection = {};
    SetRequested(kPresetB);
    auto& reader = gCachedReaders[0];
    reader.config = layout;
    reader.generation.store(7);
    reader.target.store(reinterpret_cast<void*>(&FixtureCreate));
    reader.module.store(layout.module);
    reader.configReady.store(true);
    entry_detour::testOwner = layout.module;
    for (auto kind : {entry_detour::Kind::eDlssgCachedPresetRead,
        entry_detour::Kind::eDlssgEndpointCreate, entry_detour::Kind::eDlssgEndpointEvaluate})
    {
        auto& hook = entry_detour::testHooks[static_cast<size_t>(kind)];
        hook.current = true;
        hook.generation = 7;
        hook.target = kind == entry_detour::Kind::eDlssgCachedPresetRead
            ? reader.target.load() : kind == entry_detour::Kind::eDlssgEndpointCreate
            ? reinterpret_cast<void*>(&FixtureCreate) : reinterpret_cast<void*>(&FixtureEvaluate);
        hook.original = hook.target;
    }
    reader.config.create.begin = reinterpret_cast<uintptr_t>(&FixtureCreate);
    reader.config.evaluate.begin = reinterpret_cast<uintptr_t>(&FixtureEvaluate);
    Check(ProviderSupportsLiveSwitch(layout.module), "all three exact hooks admit live switching");
    reader.generation.store(8);
    Check(!ProviderSupportsLiveSwitch(layout.module), "stale hook generation refuses live switching");
    reader.generation.store(7);
    const auto create = [] {
        return HookEndpointCreate<0>(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
            reinterpret_cast<void*>(3), reinterpret_cast<void*>(4), 5, 6);
    };
    Check(create() == 0x12345678 && createArguments && configuration->preset == kPresetB,
        "typed endpoint Create forwards six arguments and the native result");
    Check(HookEndpointEvaluate<0>(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
        reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 0x87654321 && evaluateArguments,
        "typed endpoint Evaluate forwards four arguments and the native result");
    SetRequested(kPresetA);
    auto snapshot = ReadSnapshot(layout.module);
    RecordRead(layout.module, true, kPresetA, true, ReadSource::eReport);
    Check(ReadSnapshot(layout.module).observedPreset == kPresetB
        && ReadSnapshot(layout.module).configurationReads == 1,
        "a report query cannot replace configuration evidence");
    std::atomic<bool> held{false}, release{false};
    std::thread estimate([&] {
        std::shared_lock gate(gEndpointGate);
        held.store(true); held.notify_one();
        release.wait(false);
    });
    held.wait(false);
    Check(!RelatchPending() && configuration->preset == kPresetB,
        "an endpoint/VRAM call outside the tracker excludes the reset");
    release.store(true); release.notify_one(); estimate.join();
    SetCreateQuiescenceProbe(&TrackerQuiescent);
    gTracker.Created(0, 0xabc);
    Check(!RelatchPending(), "a tracked live feature prevents a singleton reset");
    gTracker.Released(0, 0xabc);
    const auto resetBefore = destroyed;
    Check(RelatchPending() && !ReadSnapshot(layout.module).observedPresetValid
        && ReadSnapshot(layout.module).configurationResets == 1,
        "exact release resets the singleton and invalidates old observed evidence");
    Check(create() == 0x12345678 && configuration->preset == kPresetA && destroyed == resetBefore + 1,
        "the next Create rebuilds the preset and frees the old configuration");
    snapshot = ReadSnapshot(layout.module);
    Check(snapshot.observedPresetValid && snapshot.observedPreset == kPresetA
        && snapshot.configurationReads == 2 && snapshot.reportReads == 3,
        "only a real configuration read establishes the new observed preset");
    {
        EndpointCallScope nested;
        SetRequested(kPresetC);
        Check(!RelatchPending() && HookEndpointEvaluate<0>(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2),
            reinterpret_cast<void*>(3), reinterpret_cast<void*>(4)) == 0x87654321,
            "nested endpoint calls forward without recursively acquiring the shared gate");
    }
    {
        auto extraOwner = configuration;
        Check(!RelatchPending() && ReadSelection().restartRequired && configuration->preset == kPresetA
            && !ProviderSupportsLiveSwitch(layout.module),
            "failed ownership validation keeps the saved request restart-only without changing the object");
    }
    gResetDenied.store(false); // Test isolation only; production denial is process-sticky.
    configuration.reset();
    reader.configReady.store(false);
    reader.module.store(nullptr);
    entry_detour::testOwner = nullptr;
    gReads = {};
}

// The RTX 30/20 translated route owns the provider's preset reader
// with its own detour; dlssg_preset adopts that exact entry for the reset.
void RouteOwnedCases()
{
    using namespace dlssg_preset;
    const HMODULE module = GetModuleHandleW(nullptr);
    void* const reader = reinterpret_cast<void*>(&FixtureEvaluate);
    entry_detour::Handle hook{3, 9};
    auto& route = entry_detour::testHandleHook;
    route = {};
    route.current = true;
    route.kind = entry_detour::Kind::eAmperePreset;
    route.owner = module;
    route.target = reader;
    route.generation = 11;
    entry_detour::testHandle = hook;
    Check(!AdoptRouteReader(nullptr, reader, 11, hook) && !AdoptRouteReader(module, nullptr, 11, hook)
        && !AdoptRouteReader(module, reader, 0, hook) && !AdoptRouteReader(module, reader, 11, {}),
        "adoption needs a module, reader, generation and an exact entry");
    const auto refused = [&](auto change, const char* text) {
        const auto saved = route;
        change(route);
        Check(!AdoptRouteReader(module, reader, 11, hook) && !gCachedReaders[0].module.load(), text);
        route = saved;
    };
    refused([](auto& r) { r.current = false; }, "a route reader hook that is not current is not adopted");
    refused([](auto& r) { r.kind = entry_detour::Kind::eDlssgCachedPresetRead; }, "only the translated route's preset entry is adopted");
    refused([&](auto& r) { r.owner = reinterpret_cast<HMODULE>(0x7000000); }, "an entry of another module is not adopted");
    refused([&](auto& r) { r.target = reinterpret_cast<void*>(&FixtureCreate); }, "an entry at another reader is not adopted");
    refused([](auto& r) { r.generation = 12; }, "an entry of another generation is not adopted");
    // This test executable is no DLSS-G provider: its layout stays unknown.
    Check(!AdoptRouteReader(module, reader, 11, hook) && !ProviderSupportsLiveSwitch(module),
        "an unknown configuration layout stays restart-only");
    auto snapshot = ReadSnapshot(module);
    Check(snapshot.cachedReaderInstalled && snapshot.cachedReaderFailure == 5 && !snapshot.configurationResetAvailable,
        "the route's current reader is reported, with the unknown layout");
    Check(!AdoptRouteReader(module, reader, 11, hook), "a repeated adoption keeps the unknown layout");

    // The same reader with a discovered layout (as ConfigurationCases).
    gReads = {};
    gSelection = {};
    SetRequested(kPresetB);
    configuration = std::make_shared<FixtureConfiguration>();
    auto& entry = gCachedReaders[0];
    config::Layout layout{};
    layout.module = module;
    layout.slot = reinterpret_cast<void* volatile*>(&configuration);
    provider_image::SafeRead(reinterpret_cast<uintptr_t>(layout.slot[1]), layout.controlVtable);
    layout.create.begin = reinterpret_cast<uintptr_t>(&FixtureCreate);
    layout.evaluate.begin = reinterpret_cast<uintptr_t>(&FixtureEvaluate);
    layout.configurationReturn = 0x5000;
    layout.report = {0x6000, 0x6100};
    entry.config = layout;
    entry.configReady.store(true);
    entry.resets.store(0);
    entry_detour::testOwner = module;
    for (auto kind : {entry_detour::Kind::eDlssgEndpointCreate, entry_detour::Kind::eDlssgEndpointEvaluate})
    {
        auto& endpoint = entry_detour::testHooks[static_cast<size_t>(kind)];
        endpoint = {};
        endpoint.current = true;
        endpoint.generation = 11;
        endpoint.target = kind == entry_detour::Kind::eDlssgEndpointCreate
            ? reinterpret_cast<void*>(&FixtureCreate) : reinterpret_cast<void*>(&FixtureEvaluate);
        endpoint.original = endpoint.target;
    }
    Check(ProviderSupportsLiveSwitch(module) && ReadSnapshot(module).configurationResetAvailable,
        "the adopted route reader plus both endpoint gates admit live switching");
    {
        const auto saved = route;
        route.current = false;
        auto& aggregate = entry_detour::testHooks[static_cast<size_t>(entry_detour::Kind::eDlssgCachedPresetRead)];
        aggregate = saved;
        aggregate.kind = entry_detour::Kind::eDlssgCachedPresetRead;
        Check(!ProviderSupportsLiveSwitch(module),
            "a lost route entry refuses live switching even with another reader hook present");
        aggregate = {};
        route = saved;
        route.kind = entry_detour::Kind::eDlssgCachedPresetRead;
        Check(!ProviderSupportsLiveSwitch(module), "the exact entry must stay the translated route's preset hook");
        route = saved;
        route.generation = 12;
        Check(!ProviderSupportsLiveSwitch(module), "a route entry of a new generation refuses live switching");
        route = saved;
    }
    RecordRouteRead(module, reinterpret_cast<void*>(0x5000), true, kPresetB, true);
    RecordRouteRead(module, reinterpret_cast<void*>(0x6040), true, kPresetB, true);
    RecordRouteRead(module, reinterpret_cast<void*>(0x7000), true, kPresetA, true);
    snapshot = ReadSnapshot(module);
    Check(snapshot.observedPresetValid && snapshot.observedPreset == kPresetB && snapshot.configurationReads == 1
        && snapshot.reportReads == 1 && snapshot.providerReads == 3,
        "route reads are classified by caller; only the configuration read is evidence");
    Check(FreezeSelection() == kPresetB && SetRequested(kPresetC) && ReadSelection().restartRequired,
        "a request after the route's first read is saved");
    SetCreateQuiescenceProbe(&TrackerQuiescent);
    gTracker.Created(0, 0xdef);
    Check(!RelatchPending(), "a live feature on the translated route prevents the reset");
    gTracker.Released(0, 0xdef);
    const auto before = destroyed;
    Check(RelatchPending() && !configuration && ReadSnapshot(module).configurationResets == 1
        && !ReadSnapshot(module).observedPresetValid,
        "an exact release resets the translated route's configuration");
    configuration = std::make_shared<FixtureConfiguration>();
    configuration->preset = FreezeSelection();
    RecordRouteRead(module, reinterpret_cast<void*>(0x5000), true, configuration->preset, true);
    Check(configuration->preset == kPresetC && destroyed == before + 1 && ReadSnapshot(module).observedPreset == kPresetC,
        "the next Create reads the new preset through the route reader");

    // GitHub issue 107: the status reports the translated route's
    // provider; the RTX 40/50 selected-provider slot is 0 on that route.
    Check(StatusProvider(0x1000, false, 0x2000) == 0x1000 && StatusProvider(0x1000, true, 0x2000) == 0x2000
        && StatusProvider(0x1000, true, 0) == 0x1000 && StatusProvider(0, true, 0) == 0
        && StatusProvider(0, false, 0x2000) == 0,
        "status provider: RTX 40/50 the selected one, RTX 30/20 its route's provider once it recorded one");
    const auto status = [](uintptr_t selected, bool translated, HMODULE provider) {
        return ReadSnapshot(reinterpret_cast<HMODULE>(StatusProvider(selected, translated,
            reinterpret_cast<uintptr_t>(provider))));
    };
    {
        std::lock_guard lock(gPresetCMutex);
        for (auto& support : gPresetC)
            if (!support.module || support.module == module) { support = {module, true}; break; }
    }
    snapshot = status(0, true, module);
    Check(snapshot.observedPresetValid && snapshot.observedPreset == kPresetC && snapshot.overrideReads > 0
        && snapshot.providerReads > 0 && snapshot.presetCAvailable && snapshot.cachedReaderInstalled,
        "RTX 30/20 status: observed Preset C, its reads and C support come from the route's provider");
    snapshot = status(0, false, module);
    Check(!snapshot.observedPresetValid && snapshot.observedPreset == 0 && snapshot.providerReads == 0
        && !snapshot.presetCAvailable && !snapshot.cachedReaderInstalled,
        "without a selected provider the RTX 40/50 status stays empty, as before");
    {
        std::lock_guard lock(gPresetCMutex);
        for (auto& support : gPresetC)
            if (support.module == module) support = {};
    }

    const HMODULE other = reinterpret_cast<HMODULE>(0x7200000);
    RecordRouteRead(other, reinterpret_cast<void*>(0x5000), true, kPresetA, true);
    Check(ReadSnapshot(other).providerReads == 1 && !ReadSnapshot(other).observedPresetValid,
        "a route provider without an adopted reader records an unclassified read");
    Check(!ReadProvidersSupportLiveSwitch(), "that provider keeps every provider restart-only");

    gResetDenied.store(false); // Test isolation only; production denial is process-sticky.
    configuration.reset();
    entry.configReady.store(false);
    entry.routeOwned = false;
    entry.routeHook = {};
    entry.module.store(nullptr);
    entry_detour::testOwner = nullptr;
    entry_detour::testHandle = {};
    route = {};
    gReads = {};
}

// A Create's reads all see one value, and a live feature keeps it, while
// another thread keeps saving and latching new presets.
void ConcurrencyCases()
{
    using namespace dlssg_preset;
    SetCreateQuiescenceProbe(&TrackerQuiescent);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> latches{0};
    std::thread control([&] {
        uint32_t preset = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            SetRequested(preset = (preset + 1) % 4);
            if (RelatchPending()) latches.fetch_add(1, std::memory_order_relaxed);
        }
    });
    bool consistent = true;
    unsigned creates = 0;
    for (uintptr_t handle = 0x100; creates != 20000; ++handle, ++creates)
    {
        uint32_t created = 0;
        {
            fg_lifetime::CreateScope scope(gTracker, true);
            // 310.9.16 reads the key twice in one Create (setting and report).
            created = FreezeSelection();
            std::this_thread::yield();
            consistent &= FreezeSelection() == created;
            gTracker.Created(0, handle);
        }
        for (int read = 0; read != 4; ++read)
        {
            std::this_thread::yield();
            consistent &= FreezeSelection() == created;
        }
        gTracker.Released(0, handle);
        std::this_thread::yield();
    }
    stop.store(true);
    control.join();
    std::cout << "creates=" << creates << " latches=" << latches.load() << '\n';
    Check(consistent, "every Create and its live feature kept one preset while presets were latched");
    Check(latches.load() > 0, "presets were latched between features");
}
}

// srpreset.1: the NvAPI DRS hook hands the DLSS SR preset key to its filter
// after the driver answered; every other key keeps the DLSS-G path.
void SettingFilterCases()
{
    using namespace dlssg_preset;
    static int originalCalls = 0;
    static int filterCalls = 0;
    static bool lastWritable = true;
    gOriginal.store([](void*, void*, uint32_t id, Setting* setting) -> int32_t
    {
        ++originalCalls;
        setting->id = id;
        return kSettingNotFound;
    });
    SetSettingFilter(0x10E41DF3u, [](uint32_t, int32_t result, Setting*, bool writable, HMODULE) noexcept -> int32_t
    {
        ++filterCalls;
        lastWritable = writable;
        return result == kSettingNotFound ? 7 : result;
    });
    Setting setting{};
    setting.version = kSettingVersion;
    Check(HookGetSetting(nullptr, nullptr, 0x10E41DF3u, &setting) == 7 && originalCalls == 1 && filterCalls == 1,
        "the SR preset key reaches its filter after the driver answered");
    Check(!lastWritable, "a hook that is not current hands the filter no writable setting");
    Check(HookGetSetting(nullptr, nullptr, 0x10E41DF4u, &setting) == kSettingNotFound && filterCalls == 1,
        "other keys bypass the filter");
    HookGetSetting(nullptr, nullptr, kSettingId, &setting);
    Check(filterCalls == 1 && originalCalls == 3, "the DLSS-G key never reaches the SR filter");
    SetSettingFilter(0, nullptr);
    gOriginal.store(nullptr);
}

int main()
{
    SettingFilterCases();
    PolicyCases();
    TrackerCases();
    LatchCases();
    ReplayCases();
    ConfigurationCases();
    RouteOwnedCases();
    ConcurrencyCases();
    std::cout << "PRESET_SWITCH checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
