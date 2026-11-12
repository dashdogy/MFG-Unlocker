// DLSS Super Resolution and Ray Reconstruction
// detection and live preset switching. The pure tracker, then the production
// dlss_sr.cpp against a fake NGX runtime modelled on the 310.9.16 providers:
// typed parameter objects, one DRS preset read per provider when it initializes
// (cached for the session, and outranking every hint), hints read at each
// Create, and an exact Release ledger.
#include "dlss_sr.cpp"

#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

using namespace dlss_sr;

CreateKeys Keys(uint32_t width = 2560, uint32_t height = 1440, int32_t quality = 2)
{
    CreateKeys keys{};
    keys.present = CreateKeys::eRequired | CreateKeys::ePerfQuality | CreateKeys::eCreateFlags;
    keys.width = width;
    keys.height = height;
    keys.outWidth = 3840;
    keys.outHeight = 2160;
    keys.perfQuality = quality;
    keys.createFlags = 0x23;
    return keys;
}

CreateRecord Record(uintptr_t handle, uint32_t preset, uintptr_t parameters = 0x5000, Kind kind = Kind::eSuperResolution)
{
    CreateRecord record{};
    record.route = 1;
    record.handle = handle;
    record.parameters = parameters;
    record.kind = kind;
    record.keys = Keys();
    record.preset = preset;
    return record;
}

void TrackerCases()
{
    Tracker<4, 2> tracker;
    const CreateKeys keys = Keys();
    Check(tracker.Created(Record(0x100, kGameOrDriver), 0), "game SR Create recorded");
    Plan plan = tracker.Evaluate(1, 0x100, 0x5000, nullptr, kGameOrDriver, 10);
    Check(plan.action == Action::eForward && plan.handle == 0x100, "unchanged selection evaluates the game's handle");
    Check(tracker.Evaluate(1, 0x999, 0x5000, nullptr, kPresetK, 10).handle == 0x999,
        "an untracked (frame-generation) handle passes unchanged");
    Check(tracker.Running(Kind::eSuperResolution, 500) && !tracker.Running(Kind::eSuperResolution, 2000),
        "running only while evaluated within a second");

    Check(tracker.NeedsKeys(1, 0x100, kPresetK, false), "a new selection needs the create keys");
    Check(!tracker.NeedsKeys(1, 0x100, kPresetK, true), "no key reads while a driver override fixes the preset");
    Check(!tracker.NeedsKeys(1, 0x100, kRrPresetE, false), "an RR preset never reaches an SR feature");
    Check(tracker.Evaluate(1, 0x100, 0x5000, nullptr, kPresetK, 20).handle == 0x100,
        "keys not read: no re-create and no failure yet");
    plan = tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetK, 30);
    Check(plan.action == Action::eRecreate && plan.preset == kPresetK, "same parameters and keys: re-create with K");
    Check(tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetK, 31).action == Action::eForward,
        "a concurrent Evaluate during the re-create keeps the current handle");
    bool adopted = false;
    Check(tracker.Recreated(1, 0x100, kPresetK, 0x200, 32, adopted) == 0x200 && adopted,
        "the replacement is adopted");
    plan = tracker.Evaluate(1, 0x100, 0x5000, nullptr, kPresetK, 40);
    Check(plan.handle == 0x200 && plan.action == Action::eForward, "later Evaluates reach the replacement");
    Check(tracker.Primary().activePreset == kPresetK && tracker.Primary().replaced, "status shows K on a replacement");

    // K -> M: the K replacement is retired, not released at once.
    plan = tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetM, 50);
    Check(plan.action == Action::eRecreate && plan.preset == kPresetM, "K -> M re-creates");
    Check(tracker.Recreated(1, 0x100, kPresetM, 0x300, 51, adopted) == 0x300 && adopted, "M replacement adopted");
    Check(tracker.Read().retiredPending == 1, "the K replacement waits for release");
    std::array<uintptr_t, 4> due{};
    Check(tracker.DueRetired(60, due) == 0, "not released before later frames");
    for (int i = 0; i != 4; ++i) tracker.Evaluate(1, 0x100, 0x5000, nullptr, kPresetM, 70 + i);
    Check(tracker.DueRetired(100, due) == 0, "four frames but under 250 ms: still held");
    Check(tracker.DueRetired(400, due) == 1 && due[0] == 0x200, "four frames and 250 ms: released");

    // Back to the game's own preset: no Create, the game's handle again.
    plan = tracker.Evaluate(1, 0x100, 0x5000, nullptr, kGameOrDriver, 500);
    Check(plan.action == Action::eForward && plan.handle == 0x100, "back to game/driver uses the game's handle");
    Check(tracker.Read().retiredPending == 1 && !tracker.Primary().replaced, "the M replacement is retired");

    // A driver override fixed at the provider's initialization: nothing to switch.
    plan = tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetJ, 510, true);
    Check(plan.action == Action::eForward && plan.handle == 0x100
        && tracker.Primary().unavailable == Unavailable::eDriverOverride, "a cached NVIDIA app override blocks live switching");

    // Missing keys: no live switch, no retry until the selection changes.
    CreateKeys resized = Keys(1920, 1080);
    resized.present &= ~CreateKeys::eWidth;
    plan = tracker.Evaluate(1, 0x100, 0x5000, &resized, kPresetL, 600);
    Check(plan.action == Action::eForward && plan.handle == 0x100
        && tracker.Primary().unavailable == Unavailable::eParameters, "missing create keys: next Create only");
    Check(!tracker.NeedsKeys(1, 0x100, kPresetL, false), "the failed selection is not retried");
    plan = tracker.Evaluate(1, 0x100, 0x6000, &keys, kPresetM, 610);
    Check(plan.action == Action::eForward && tracker.Primary().unavailable == Unavailable::eParameters,
        "another parameter object: next Create only");
    plan = tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetK, 620);
    Check(plan.action == Action::eRecreate, "a new selection is tried again");
    Check(tracker.Recreated(1, 0x100, kPresetK, 0, 621, adopted) == 0x100 && !adopted
        && tracker.Primary().unavailable == Unavailable::eCreateFailed && tracker.Read().recreateFailures == 1,
        "a refused Create keeps the current handle");

    // The game releases: every handle of ours first.
    std::array<uintptr_t, 9> ours{};
    const size_t count = tracker.Releasing(1, 0x100, ours);
    Check(count == 1 && ours[0] == 0x300, "game Release takes the retired replacement with it");
    tracker.Released(1, 0x100, true);
    Check(tracker.Read().liveSr == 0 && tracker.Read().retiredPending == 0 && tracker.Read().releases == 1,
        "nothing tracked after the game's Release");
    Check(!tracker.Recreated(1, 0x100, kPresetK, 0x400, 700, adopted) && !adopted,
        "a re-create finishing after the game's Release is not adopted");
}

void KindCases()
{
    Tracker<4, 1> tracker;
    const CreateKeys keys = Keys();
    tracker.Created(Record(0x100, kGameOrDriver), 0);
    tracker.Created(Record(0x700, kGameOrDriver, 0x7000, Kind::eRayReconstruction), 0);
    Kind kind{};
    Check(tracker.Tracked(1, 0x700, &kind) && kind == Kind::eRayReconstruction, "the feature's kind is known");
    Check(tracker.Evaluate(1, 0x700, 0x7000, &keys, kPresetK, 5).action == Action::eForward,
        "an SR preset never reaches a Ray Reconstruction feature");
    Check(tracker.Running(Kind::eRayReconstruction, 10) && tracker.Read().liveRr == 1 && tracker.Read().rrCreates == 1,
        "Ray Reconstruction detected");
    Plan plan = tracker.Evaluate(1, 0x700, 0x7000, &keys, kRrPresetE, 6);
    Check(plan.action == Action::eRecreate && plan.preset == kRrPresetE, "an RR preset re-creates the RR feature");
    bool adopted = false;
    Check(tracker.Recreated(1, 0x700, kRrPresetE, 0x800, 7, adopted) == 0x800 && adopted
        && tracker.Primary(Kind::eRayReconstruction).activePreset == kRrPresetE
        && tracker.Primary(Kind::eSuperResolution).activePreset == kGameOrDriver,
        "RR and SR keep separate presets");

    tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetK, 10);
    tracker.Recreated(1, 0x100, kPresetK, 0x200, 10, adopted);
    tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetM, 20);
    tracker.Recreated(1, 0x100, kPresetM, 0x300, 20, adopted);
    Check(adopted && tracker.Read().retiredPending == 1, "one retired handle fills the table");
    plan = tracker.Evaluate(1, 0x100, 0x5000, &keys, kPresetL, 30);
    Check(plan.action == Action::eForward && plan.handle == 0x300
        && tracker.Primary().unavailable == Unavailable::eCapacity, "no third feature while one waits for release");

    Tracker<1, 1> full;
    full.Created(Record(0x100, kGameOrDriver), 0);
    Check(!full.Created(Record(0x200, kGameOrDriver), 0) && full.Read().overflow, "a full table reports overflow");
    Check(full.Created(Record(0x100, kPresetK), 0) && full.Read().liveSr == 1,
        "a recycled address replaces its stale record");
}

void DrsCases()
{
    dlssg_preset::Setting setting{};
    setting.version = dlssg_preset::kSettingVersion;
    setting.id = kDrsPresetSetting;
    setting.current.dword = kDrsLatest;
    uint32_t value = 0;
    Check(DriverValue(kDrsPresetSetting, dlssg_preset::kOk, setting, value) && value == kDrsLatest,
        "a value set in the profile is the driver's answer");
    setting.location = 1;
    Check(!DriverValue(kDrsPresetSetting, dlssg_preset::kOk, setting, value),
        "an inherited value is not one the provider accepts");
    Check(!DriverValue(kDrsPresetSetting, dlssg_preset::kSettingNotFound, setting, value), "no setting, no value");
    int32_t answer = dlssg_preset::kOk;
    Check(!SuppressDrsRead(kDrsPresetSetting, dlssg_preset::kOk, kGameOrDriver, answer) && answer == dlssg_preset::kOk,
        "game/driver keeps the driver's answer");
    Check(SuppressDrsRead(kDrsPresetSetting, dlssg_preset::kOk, kPresetK, answer)
        && answer == dlssg_preset::kSettingNotFound, "a selected SR preset answers not set");
    answer = dlssg_preset::kOk;
    Check(!SuppressDrsRead(kDrsRrPresetSetting, dlssg_preset::kOk, kPresetK, answer) && answer == dlssg_preset::kOk,
        "an SR preset never answers the RR key");
    Check(SuppressDrsRead(kDrsRrPresetSetting, dlssg_preset::kOk, kRrPresetF, answer)
        && answer == dlssg_preset::kSettingNotFound, "a selected RR preset answers the RR key not set");
    answer = dlssg_preset::kOk;
    Check(!SuppressDrsRead(dlssg_preset::kSettingId, dlssg_preset::kOk, kPresetK, answer) && answer == dlssg_preset::kOk,
        "the DLSS-G key is never touched");
    Kind kind{};
    Check(KindForDrsKey(kDrsPresetSetting, kind) && kind == Kind::eSuperResolution
        && KindForDrsKey(kDrsRrPresetSetting, kind) && kind == Kind::eRayReconstruction
        && !KindForDrsKey(dlssg_preset::kSettingId, kind), "SR 0x10E41DF3 and RR 0x10E41DF7 keys");
    Check(FixedByDriver(true, true, kDrsLatest, false) && !FixedByDriver(true, true, kDrsLatest, true)
        && !FixedByDriver(true, false, 0, false) && !FixedByDriver(false, true, 11, false)
        && !FixedByDriver(true, true, 0, false), "fixed only by an observed, unsuppressed, nonzero driver value");
    Check(IsValidSelection(0) && IsValidSelection(10) && IsValidSelection(13) && !IsValidSelection(9)
        && !IsValidSelection(14) && !IsValidSelection(kDrsLatest) && !IsValidSelection(kRrPresetE),
        "SR selections: game/driver and J..M only");
    Check(IsValidRrSelection(0) && IsValidRrSelection(4) && IsValidRrSelection(6) && !IsValidRrSelection(3)
        && !IsValidRrSelection(7) && !IsValidRrSelection(kPresetK), "RR selections: game/driver and D..F only");
}

// --- fake NGX runtime -------------------------------------------------------

struct FakeParameters final : NVSDK_NGX_Parameter
{
    enum class Type { eUnsigned, eSigned, eOther };
    struct Value { Type type; uint64_t bits; };
    std::map<std::string, Value> values;
    const char* refuseKey = nullptr;
    uint32_t refuseNumber = 0;

    void Set(const char* name, unsigned long long value) override { values[name] = {Type::eOther, value}; }
    void Set(const char* name, float) override { values[name] = {Type::eOther, 0}; }
    void Set(const char* name, double) override { values[name] = {Type::eOther, 0}; }
    void Set(const char* name, unsigned int value) override
    {if(refuseKey&&strcmp(name,refuseKey)==0&&value==refuseNumber)return;values[name] = {Type::eUnsigned, value};}
    void Set(const char* name, int value) override { values[name] = {Type::eSigned, static_cast<uint32_t>(value)}; }
    void Set(const char* name, ID3D11Resource*) override { values[name] = {Type::eOther, 0}; }
    void Set(const char* name, ID3D12Resource*) override { values[name] = {Type::eOther, 0}; }
    void Set(const char* name, void*) override { values[name] = {Type::eOther, 0}; }
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
    // Typed like NGX: a read of the other integer type fails.
    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override
    {
        const auto found = values.find(name);
        if (found == values.end() || found->second.type != Type::eUnsigned) return NVSDK_NGX_Result_Fail;
        *value = static_cast<unsigned int>(found->second.bits);
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        const auto found = values.find(name);
        if (found == values.end() || found->second.type != Type::eSigned) return NVSDK_NGX_Result_Fail;
        *value = static_cast<int>(found->second.bits);
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override { values.clear(); }

    bool Has(const char* name, Type type, uint32_t value) const
    {
        const auto found = values.find(name);
        return found != values.end() && found->second.type == type && found->second.bits == value;
    }
};

// The 310.9.16 providers: at initialization one DRS read per profile (the
// application profile, then the global one), the first value set in its own
// profile cached for the session; at each Create that value, else the hint.
// RR without a hint, and RR "latest", use F.
const HMODULE kProvider = reinterpret_cast<HMODULE>(0x7000'0000);
const HMODULE kStranger = reinterpret_cast<HMODULE>(0x6000'0000);
bool TestProvider(HMODULE module) noexcept { return module == kProvider; }

struct ProviderCreate { uintptr_t handle = 0; Kind kind{}; uint32_t preset = 0; uint32_t hintSeen = 0; CreateKeys keys{}; };
struct ProviderState
{
    int32_t appResult = dlssg_preset::kSettingNotFound;
    int32_t globalResult = dlssg_preset::kSettingNotFound;
    uint32_t driverValue = 0;
    bool cached = false;
    uint32_t drs = 0;
};
std::vector<ProviderCreate> gProviderCreates;
std::set<uintptr_t> gLiveHandles;
std::vector<uintptr_t> gReleased;
unsigned gBadReleases = 0;
ProviderState gProviders[2];
bool gRefuseCreate = false;
// RR preset F needs driver 580 or newer; an older driver fails the Create.
bool gOldDriver = false;
uint64_t gNow = 1000;
uint64_t TestClock() noexcept { return gNow; }

ProviderState& Provider(Kind kind) { return gProviders[kind == Kind::eSuperResolution ? 0 : 1]; }

int32_t ProviderDrsRead(Kind kind, int32_t driverResult)
{
    ProviderState& provider = Provider(kind);
    const uint32_t key = kind == Kind::eSuperResolution ? kDrsPresetSetting : kDrsRrPresetSetting;
    dlssg_preset::Setting setting{};
    setting.version = dlssg_preset::kSettingVersion;
    if (driverResult == dlssg_preset::kOk)
    {
        setting.id = key;
        setting.current.dword = provider.driverValue;
    }
    const int32_t result = FilterDrsRead(key, driverResult, &setting, true, kProvider);
    if (result == dlssg_preset::kOk && setting.location == 0) provider.drs = setting.current.dword;
    return result;
}

void ProviderInitialize(Kind kind)
{
    ProviderState& provider = Provider(kind);
    provider.cached = true;
    provider.drs = 0;
    if (ProviderDrsRead(kind, provider.appResult) != dlssg_preset::kOk) ProviderDrsRead(kind, provider.globalResult);
}

uint32_t HintOf(const NVSDK_NGX_Parameter* parameters, const char* name)
{
    unsigned int hint = 0;
    int signedHint = 0;
    if (parameters->Get(name, &hint) != NVSDK_NGX_Result_Success
        && parameters->Get(name, &signedHint) == NVSDK_NGX_Result_Success)
        hint = static_cast<unsigned int>(signedHint);
    return hint;
}

NVSDK_NGX_Result NVSDK_CONV FakeCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    if (gRefuseCreate) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const Kind kind = feature == NVSDK_NGX_Feature_SuperSampling ? Kind::eSuperResolution : Kind::eRayReconstruction;
    ProviderState& provider = Provider(kind);
    if (!provider.cached) ProviderInitialize(kind);
    ProviderCreate create{};
    create.kind = kind;
    create.keys = ReadKeys(parameters);
    if (kind == Kind::eSuperResolution)
    {
        create.hintSeen = HintOf(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality);
        create.preset = provider.drs == kDrsLatest ? kPresetK : provider.drs ? provider.drs : create.hintSeen;
    }
    else
    {
        create.hintSeen = HintOf(parameters, "RayReconstruction.Hint.Render.Preset.Quality");
        create.preset = provider.drs == kDrsLatest ? kRrPresetF : provider.drs ? provider.drs
            : create.hintSeen ? create.hintSeen : kRrPresetF;
        if (create.preset == kRrPresetF && gOldDriver) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    // NVSDK_NGX_Handle is opaque to applications; the fake owns 8 bytes per handle.
    static uint64_t next = 1;
    auto* handle = reinterpret_cast<NVSDK_NGX_Handle*>(new uint64_t{next++});
    create.handle = reinterpret_cast<uintptr_t>(handle);
    gProviderCreates.push_back(create);
    gLiveHandles.insert(create.handle);
    *output = handle;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV FakeRelease(NVSDK_NGX_Handle* handle)
{
    const auto value = reinterpret_cast<uintptr_t>(handle);
    if (!gLiveHandles.erase(value)) { ++gBadReleases; return NVSDK_NGX_Result_FAIL_FeatureNotFound; }
    gReleased.push_back(value);
    delete reinterpret_cast<uint64_t*>(handle);
    return NVSDK_NGX_Result_Success;
}

FakeParameters GameParameters(Kind kind = Kind::eSuperResolution)
{
    FakeParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Width, 2560u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 1440u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 3840u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 2160u);
    parameters.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0x23);
    if (kind == Kind::eSuperResolution)
    {
        // The game's own hint, set as a signed int (UE style); others absent.
        parameters.Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, 10);
    }
    else
    {
        parameters.Set("DLSS.Denoise.Mode", 1);
        parameters.Set("DLSS.Roughness.Mode", 0u);
        parameters.Set("DLSS.Use.HW.Depth", 1u);
        parameters.Set("RayReconstruction.Hint.Render.Preset.Quality", 4);
    }
    return parameters;
}

void Session(int32_t appResult, int32_t globalResult, uint32_t driverValue, Kind kind = Kind::eSuperResolution)
{
    testing::Reset();
    testing::SetClock(&TestClock);
    testing::SetProviderCheck(&TestProvider);
    gProviderCreates.clear(); gLiveHandles.clear(); gReleased.clear();
    gBadReleases = 0;
    for (auto& provider : gProviders) provider = {};
    Provider(kind).appResult = appResult;
    Provider(kind).globalResult = globalResult;
    Provider(kind).driverValue = driverValue;
    gRefuseCreate = false;
    gOldDriver = false;
}

auto* const kCommand = reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234);

// Preset selected at game start, NVIDIA app "latest" set globally: the
// provider's read is answered "not set", and the hints switch live.
void SelectedAtStartCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kOk, kDrsLatest);
    FakeParameters parameters = GameParameters();
    const auto* fg = reinterpret_cast<const NVSDK_NGX_Handle*>(0xF000);
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, fg, &parameters) == fg, "no SR: Evaluate passes through");

    static uint32_t loaderCalls = 0;
    SetInitialSelectionLoader([]() noexcept -> uint32_t { ++loaderCalls; return kPresetK; });
    NVSDK_NGX_Handle* game = nullptr;
    Check(Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_SuperSampling, &parameters, &game)
        == NVSDK_NGX_Result_Success && game, "SR Create succeeds");
    Check(loaderCalls == 1 && Requested() == kPresetK, "the saved selection loads at the first SR use");
    Snapshot snapshot = Read();
    Check(snapshot.sr.observed && snapshot.sr.suppressed && snapshot.sr.driverValueValid
        && snapshot.sr.driverValue == kDrsLatest && !snapshot.sr.fixedByDriver && snapshot.sr.suppressedReads == 2,
        "the provider's app and global reads were answered not set; the driver's latest is recorded");
    Check(gProviderCreates.back().preset == kPresetK && gProviderCreates.back().hintSeen == kPresetK,
        "the provider uses the K hint, not the NVIDIA app override");
    Check(parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, FakeParameters::Type::eSigned, 10),
        "the game's signed hint is restored after the Create");
    Check(parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, FakeParameters::Type::eUnsigned, 0),
        "an absent hint reads as default afterwards");
    Check(snapshot.detected && !snapshot.rrDetected && snapshot.counters.liveSr == 1
        && snapshot.primary.activePreset == kPresetK && snapshot.primary.gameHint == kPresetJ
        && snapshot.primary.keys.width == 2560 && snapshot.primary.keys.perfQuality == 2,
        "status: SR detected, K, the game's own hint J, 2560x1440 Quality");

    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game,
        "same selection: the game's handle");
    Check(Read().running, "running after an Evaluate");

    SetRequested(kPresetM);
    const auto* replacement = Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(replacement != game && gProviderCreates.size() == 2 && gProviderCreates.back().preset == kPresetM,
        "a live change re-creates with M inside the game's Evaluate, and the provider uses M");
    Check(gLiveHandles.count(reinterpret_cast<uintptr_t>(game)) == 1, "the game's handle stays alive");
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == replacement,
        "later Evaluates reach the M feature");

    SetRequested(kPresetL);
    const auto* third = Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(third != replacement && gProviderCreates.back().preset == kPresetL && gReleased.empty(),
        "M -> L: the M feature is held for a few frames");
    for (int i = 0; i != 4; ++i) Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(gReleased.empty(), "four frames inside 250 ms: still held");
    gNow += 300;
    Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(gReleased.size() == 1 && gReleased[0] == reinterpret_cast<uintptr_t>(replacement),
        "then released on the render thread through the route's Release");

    SetRequested(kGameOrDriver);
    const auto* plain = Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(plain != game && gProviderCreates.back().preset == kPresetJ && gProviderCreates.back().hintSeen == kPresetJ,
        "game/driver re-creates with the game's own hint");

    SetRequested(kPresetK);
    const size_t creates = gProviderCreates.size();
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game
        && gProviderCreates.size() == creates, "back to the game's K handle without a Create");

    SetRequested(kPresetM);
    FakeParameters other = GameParameters();
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &other) == game
        && Read().primary.unavailable == Unavailable::eParameters && gProviderCreates.size() == creates,
        "an Evaluate with another parameter object does not re-create");

    SetRequested(kPresetJ);
    gRefuseCreate = true;
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game
        && Read().primary.unavailable == Unavailable::eCreateFailed, "a refused re-create keeps the game's handle");
    gRefuseCreate = false;
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game,
        "the refused selection is not retried every frame");

    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0, "the game's Release leaves no feature alive, none twice");
    snapshot = Read();
    Check(snapshot.counters.liveSr == 0 && snapshot.counters.retiredPending == 0 && !snapshot.running,
        "nothing tracked after the game's Release");
}

// Ray Reconstruction: its own selection, hints, DRS key and live switching.
void RayReconstructionCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kOk, kDrsLatest, Kind::eRayReconstruction);
    SetRequested(kPresetK);
    SetRequestedRr(kRrPresetE);
    FakeParameters parameters = GameParameters(Kind::eRayReconstruction);
    NVSDK_NGX_Handle* game = nullptr;
    Check(Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_RayReconstruction, &parameters, &game)
        == NVSDK_NGX_Result_Success && game, "RR Create succeeds");
    Snapshot snapshot = Read();
    Check(snapshot.rr.observed && snapshot.rr.suppressed && snapshot.rr.driverValue == kDrsLatest
        && !snapshot.sr.observed, "the RR provider's own key was answered not set");
    Check(gProviderCreates.back().preset == kRrPresetE && gProviderCreates.back().hintSeen == kRrPresetE,
        "the RR provider uses the E hint");
    Check(parameters.Has("RayReconstruction.Hint.Render.Preset.Quality", FakeParameters::Type::eSigned, 4)
        && !parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, FakeParameters::Type::eUnsigned, kPresetK),
        "only the RR hints are set, and the game's RR hint is restored");
    Check(snapshot.rrDetected && !snapshot.detected && snapshot.rrPrimary.activePreset == kRrPresetE
        && snapshot.rrPrimary.gameHint == kRrPresetD && snapshot.rrPrimary.keys.useHwDepth == 1,
        "status: RR detected, E, the game's own hint D");
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game && Read().rayReconstructionRunning,
        "RR runs");

    SetRequestedRr(kRrPresetF);
    const auto* replacement = Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(replacement != game && gProviderCreates.back().kind == Kind::eRayReconstruction
        && gProviderCreates.back().preset == kRrPresetF, "a live RR change re-creates the RR feature with F");
    FakeParameters changed = GameParameters(Kind::eRayReconstruction);
    changed.Set("DLSS.Use.HW.Depth", 0u);
    SetRequestedRr(kRrPresetE);
    const size_t creates = gProviderCreates.size();
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game
        && gProviderCreates.size() == creates && Read().rrPrimary.activePreset == kRrPresetE,
        "back to the game's E handle without a Create");
    SetRequestedRr(kRrPresetF);
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &changed) == game
        && Read().rrPrimary.unavailable == Unavailable::eParameters, "a changed RR create key: next Create only");

    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0, "RR released cleanly");

    // Driver 579: the provider refuses F at the game's Create; the game gets its own.
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kSettingNotFound, 0, Kind::eRayReconstruction);
    SetRequestedRr(kRrPresetF);
    gOldDriver = true;
    FakeParameters older = GameParameters(Kind::eRayReconstruction);
    Check(Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_RayReconstruction, &older, &game)
        == NVSDK_NGX_Result_Success && game && gProviderCreates.back().preset == kRrPresetD
        && Read().createRetries == 1 && Read().rrPrimary.activePreset == kGameOrDriver,
        "a Create refused with the selection is retried with the game's hints");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
}

// Game/driver at game start with an NVIDIA app override: the provider caches it,
// and a later selection is reported as needing a restart instead of re-creating.
void DriverFixedCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kOk, kDrsLatest);
    SetRequested(kGameOrDriver);
    FakeParameters parameters = GameParameters();
    NVSDK_NGX_Handle* game = nullptr;
    Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_SuperSampling, &parameters, &game);
    Snapshot snapshot = Read();
    Check(snapshot.sr.observed && !snapshot.sr.suppressed && snapshot.sr.fixedByDriver
        && gProviderCreates.back().preset == kPresetK, "game/driver at start: the provider caches the NVIDIA app's latest");
    Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    SetRequested(kPresetM);
    const size_t creates = gProviderCreates.size();
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == game
        && gProviderCreates.size() == creates && Read().primary.unavailable == Unavailable::eDriverOverride,
        "no pointless re-create; status asks for a restart");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0, "released cleanly");

    Session(dlssg_preset::kOk, dlssg_preset::kSettingNotFound, kPresetL);
    SetRequested(kGameOrDriver);
    Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_SuperSampling, &parameters, &game);
    snapshot = Read();
    Check(snapshot.sr.fixedByDriver && snapshot.sr.driverValue == kPresetL && snapshot.sr.reads == 1
        && gProviderCreates.back().preset == kPresetL, "an app-profile L override is cached from the first read");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
}

// No NVIDIA app override: game/driver at start still switches live.
void NoOverrideCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kSettingNotFound, 0);
    SetRequested(kGameOrDriver);
    FakeParameters parameters = GameParameters();
    NVSDK_NGX_Handle* game = nullptr;
    Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_SuperSampling, &parameters, &game);
    Check(!Read().sr.fixedByDriver && gProviderCreates.back().preset == kPresetJ, "no override: the game's J hint");
    SetRequested(kPresetK);
    const auto* replacement = Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
    Check(replacement != game && gProviderCreates.back().preset == kPresetK, "game/driver at start, then K live");

    dlssg_preset::Setting setting{};
    setting.version = dlssg_preset::kSettingVersion;
    const uint64_t reads = Read().sr.reads;
    Check(FilterDrsRead(kDrsPresetSetting, dlssg_preset::kSettingNotFound, &setting, true, kStranger)
        == dlssg_preset::kSettingNotFound && Read().sr.reads == reads && Read().drsNotProvider == 1,
        "a read from a module that is no NGX provider is untouched");
    Check(FilterDrsRead(kDrsPresetSetting, dlssg_preset::kOk, &setting, false, kProvider) == dlssg_preset::kOk
        && Read().drsNotWritable == 1, "a provider read without an owned buffer is untouched");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0, "released cleanly");
}

void ConcurrencyCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kSettingNotFound, 0);
    FakeParameters parameters = GameParameters();
    FakeParameters rrParameters = GameParameters(Kind::eRayReconstruction);
    SetRequested(kPresetK);
    SetRequestedRr(kRrPresetE);
    NVSDK_NGX_Handle* game = nullptr;
    NVSDK_NGX_Handle* rr = nullptr;
    Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_SuperSampling, &parameters, &game);
    Create(0, &FakeCreate, kCommand, NVSDK_NGX_Feature_RayReconstruction, &rrParameters, &rr);
    std::atomic<bool> stop{false};
    std::thread selector([&]
    {
        const uint32_t presets[]{kPresetK, kPresetM, kPresetL, kGameOrDriver, kPresetJ};
        const uint32_t rrPresets[]{kRrPresetE, kRrPresetF, kGameOrDriver, kRrPresetD};
        for (unsigned i = 0; !stop.load(); ++i)
        {
            SetRequested(presets[i % 5]);
            SetRequestedRr(rrPresets[i % 4]);
            std::this_thread::yield();
        }
    });
    std::thread status([&] { while (!stop.load()) Read(); });
    for (int frame = 0; frame != 4000; ++frame)
    {
        gNow += 5;
        Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters);
        Evaluate(0, &FakeCreate, &FakeRelease, kCommand, rr, &rrParameters);
    }
    stop = true;
    selector.join();
    status.join();
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    BeforeRelease(0, &FakeRelease, rr);
    AfterRelease(0, rr, FakeRelease(rr));
    Check(gLiveHandles.empty() && gBadReleases == 0 && Read().counters.retiredPending == 0,
        "4000 frames of SR and RR with racing selections: every feature released exactly once");
    Check(parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, FakeParameters::Type::eSigned, 10)
        && rrParameters.Has("RayReconstruction.Hint.Render.Preset.Quality", FakeParameters::Type::eSigned, 4),
        "the game's SR and RR hints survive every re-create");
}
}

namespace
{
NVSDK_NGX_Result NVSDK_CONV ThrowCreate(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**)
{RaiseException(0xe0424242,0,0,nullptr);return NVSDK_NGX_Result_FAIL_NotInitialized;}
bool CatchCreateSeh(const NVSDK_NGX_Parameter* p,const SavedHints* saved,dlss_sr_override::Request* request,
    const SavedCreateKeys* keys=nullptr)
{
    CreateCall call{};call.d3d12=&ThrowCreate;
    __try{InvokeCreate(call,NVSDK_NGX_Feature_SuperSampling,p,nullptr,saved,request,keys);}
    __except(GetExceptionCode()==0xe0424242?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return true;}
    return false;
}
void SehCases()
{
    auto parameters=GameParameters();
    CreateRecord record{};record.keys=ReadKeys(&parameters);
    parameters.Set(NVSDK_NGX_Parameter_Width,3840u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth,1920u);
    const auto current=ReadKeys(&parameters);
    const auto keys=ApplyCreateKeys(&parameters,record);
    dlss_sr_override::Request request(Kind::eSuperResolution,kPresetK,true);
    const auto saved=ApplyHints(Kind::eSuperResolution,&parameters,kPresetK);
    Check(CatchCreateSeh(&parameters,&saved,&request,&keys)
        && parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,FakeParameters::Type::eSigned,10)
        && ReadKeys(&parameters)==current
        && dlss_sr_override::gRequest==request.previous,
        "native Create SEH restores typed creation keys, hints and request TLS");
}

void SharedParameterCases()
{
    Session(dlssg_preset::kSettingNotFound,dlssg_preset::kSettingNotFound,0);
    SetRequested(kPresetK);
    auto parameters=GameParameters();
    const auto original=ReadKeys(&parameters);
    NVSDK_NGX_Handle* game=nullptr;
    Create(0,&FakeCreate,kCommand,NVSDK_NGX_Feature_SuperSampling,&parameters,&game);
    // sl.dlss getOptimalSettings writes output dimensions into Width/Height,
    // returns the render dimensions in OutWidth/OutHeight, and leaves them there.
    parameters.Set(NVSDK_NGX_Parameter_Width,3840u);
    parameters.Set(NVSDK_NGX_Parameter_Height,2160u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth,1920u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight,1080u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,0x79);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,1);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,12);
    const auto current=ReadKeys(&parameters);
    SetRequested(kPresetM);
    const auto* replacement=Evaluate(0,&FakeCreate,&FakeRelease,kCommand,game,&parameters);
    auto seen=gProviderCreates.back().keys;
    // An initially absent output-subrect flag is its documented default false.
    seen.present&=~CreateKeys::eOutputSubrects;seen.signedValues&=~CreateKeys::eOutputSubrects;
    Check(replacement!=game&&seen==original&&gProviderCreates.back().preset==kPresetM,
        "Streamline optimal-settings drift: M re-creates with the tracked handle's original creation values");
    Check(ReadKeys(&parameters)==current
        &&parameters.Has(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,FakeParameters::Type::eSigned,0x79)
        &&parameters.Has(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,FakeParameters::Type::eSigned,1),
        "live Create restores current shared dimensions, flags and integer types before Evaluate");
    SetRequested(kGameOrDriver);
    const auto* plain=Evaluate(0,&FakeCreate,&FakeRelease,kCommand,game,&parameters);
    Check(plain!=game&&plain!=replacement&&gProviderCreates.back().hintSeen==kPresetJ,
        "selected-at-start -> Game replays original game hints rather than later shared query hints");
    Check(ReadKeys(&parameters)==current
        &&parameters.Has(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,FakeParameters::Type::eSigned,12),
        "Game recreation restores current shared creation fields and game hints");
    BeforeRelease(0,&FakeRelease,game);AfterRelease(0,game,FakeRelease(game));
    Check(gLiveHandles.empty()&&gBadReleases==0,"shared-parameter SR releases every replacement exactly once");

    Session(dlssg_preset::kSettingNotFound,dlssg_preset::kSettingNotFound,0,Kind::eRayReconstruction);
    SetRequestedRr(kRrPresetE);
    parameters=GameParameters(Kind::eRayReconstruction);
    const auto rrOriginal=ReadKeys(&parameters);
    Create(0,&FakeCreate,kCommand,NVSDK_NGX_Feature_RayReconstruction,&parameters,&game);
    parameters.Set("DLSS.Denoise.Mode",0);
    parameters.Set("DLSS.Roughness.Mode",1u);
    parameters.Set("DLSS.Use.HW.Depth",0u);
    parameters.Set(NVSDK_NGX_Parameter_Width,3840u);
    const auto rrCurrent=ReadKeys(&parameters);
    SetRequestedRr(kRrPresetF);
    replacement=Evaluate(0,&FakeCreate,&FakeRelease,kCommand,game,&parameters);
    Check(replacement!=game&&gProviderCreates.back().keys==rrOriginal&&gProviderCreates.back().preset==kRrPresetF,
        "RR recreates using captured denoise, roughness, depth and resolution values after shared-state drift");
    Check(ReadKeys(&parameters)==rrCurrent,"RR restores every current shared creation scalar before Evaluate");
    BeforeRelease(0,&FakeRelease,game);AfterRelease(0,game,FakeRelease(game));
    Check(gLiveHandles.empty()&&gBadReleases==0,"shared-parameter RR releases every replacement exactly once");

    Session(dlssg_preset::kSettingNotFound,dlssg_preset::kSettingNotFound,0);
    parameters=GameParameters();SetRequested(kGameOrDriver);
    Create(0,&FakeCreate,kCommand,NVSDK_NGX_Feature_SuperSampling,&parameters,&game);
    parameters.Set(NVSDK_NGX_Parameter_Width,3840u);
    parameters.refuseKey=NVSDK_NGX_Parameter_Width;parameters.refuseNumber=3840;
    SetRequested(kPresetM);
    replacement=Evaluate(0,&FakeCreate,&FakeRelease,kCommand,game,&parameters);
    Check(replacement==game&&Read().primary.unavailable==Unavailable::eCreateFailed&&gReplayDenied.load()
        &&gLiveHandles.size()==1&&gReleased.size()==1,
        "failed restoration readback rejects/releases the replacement and disables further replay");
    const auto creates=gProviderCreates.size();SetRequested(kPresetJ);
    Check(Evaluate(0,&FakeCreate,&FakeRelease,kCommand,game,&parameters)==game&&gProviderCreates.size()==creates,
        "a later preset cannot retry native creation after parameter restoration failed");
    parameters.refuseKey=nullptr;
    BeforeRelease(0,&FakeRelease,game);AfterRelease(0,game,FakeRelease(game));
}

// The runtime's Vulkan entries. CreateFeature1 names the
// game's VkDevice, CreateFeature does not; both record on a VkCommandBuffer.
struct VulkanCall { unsigned creates = 0; void* device = nullptr; void* command = nullptr; };
VulkanCall gVkCreate, gVkCreate1;
NVSDK_NGX_Result NVSDK_CONV FakeVkCreate(void* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    ++gVkCreate.creates; gVkCreate.command = command; gVkCreate.device = nullptr;
    return FakeCreate(nullptr, feature, parameters, output);
}
NVSDK_NGX_Result NVSDK_CONV FakeVkCreate1(void* device, void* command, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output)
{
    ++gVkCreate1.creates; gVkCreate1.command = command; gVkCreate1.device = device;
    return FakeCreate(nullptr, feature, parameters, output);
}
std::vector<ID3D12Device*> gInstallerDevices;
bool TestInstaller(ID3D12Device* device) noexcept { gInstallerDevices.push_back(device); return true; }

void* const kVkDevice = reinterpret_cast<void*>(0xD0D0);
void* const kGameBuffer = reinterpret_cast<void*>(0xB001);
void* const kEvaluateBuffer = reinterpret_cast<void*>(0xB002);

// The game's Create through one Vulkan entry, and a later Evaluate's call with
// both of the route's entries on another command buffer.
CreateCall VulkanCreateOf(bool create1)
{
    CreateCall call{};
    call.api = create1 ? CreateApi::eVulkan1 : CreateApi::eVulkan;
    call.vulkan = &FakeVkCreate;
    call.vulkan1 = &FakeVkCreate1;
    call.device = create1 ? kVkDevice : nullptr;
    call.command = kGameBuffer;
    return call;
}
CreateCall VulkanEvaluateOf(bool withCreate1 = true)
{
    CreateCall call{};
    call.api = CreateApi::eVulkan;
    call.vulkan = &FakeVkCreate;
    call.vulkan1 = withCreate1 ? &FakeVkCreate1 : nullptr;
    call.command = kEvaluateBuffer;
    return call;
}

void VulkanCases()
{
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kOk, kDrsLatest);
    gVkCreate = {}; gVkCreate1 = {}; gInstallerDevices.clear();
    SetDrsHookInstaller(&TestInstaller);
    SetRequested(kPresetK);
    FakeParameters parameters = GameParameters();
    NVSDK_NGX_Handle* game = nullptr;
    Check(Create(0, VulkanCreateOf(true), NVSDK_NGX_Feature_SuperSampling, &parameters, &game)
        == NVSDK_NGX_Result_Success && game, "Vulkan CreateFeature1: SR Create succeeds");
    Check(gVkCreate1.creates == 1 && gVkCreate.creates == 0 && gVkCreate1.device == kVkDevice
        && gVkCreate1.command == kGameBuffer, "it enters CreateFeature1 with the game's VkDevice and command buffer");
    Check(gInstallerDevices.size() == 1 && gInstallerDevices[0] == nullptr && gDrsHookReady.load(),
        "the DRS hook is prepared for Vulkan (no D3D12 device) before the provider's first read");
    Check(gProviderCreates.back().preset == kPresetK && Read().detected && Read().counters.liveSr == 1,
        "Vulkan SR detected; the provider uses the K hint, not the NVIDIA app override");
    Check(Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters) == game && Read().running,
        "same selection: the game's handle");

    SetRequested(kPresetM);
    const auto* replacement = Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters);
    Check(replacement != game && gProviderCreates.back().preset == kPresetM && gVkCreate1.creates == 2
        && gVkCreate.creates == 0, "a live change re-creates M through CreateFeature1, as the game's Create");
    Check(gVkCreate1.device == kVkDevice && gVkCreate1.command == kEvaluateBuffer,
        "with the game's VkDevice, on the Evaluate's command buffer");
    Check(Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters) == replacement,
        "later Vulkan Evaluates reach the M feature");

    // A D3D12 Evaluate cannot re-create a feature made through Vulkan.
    SetRequested(kPresetL);
    const size_t creates = gProviderCreates.size();
    Check(Evaluate(0, &FakeCreate, &FakeRelease, kCommand, game, &parameters) == replacement
        && gProviderCreates.size() == creates && Read().primary.unavailable == Unavailable::eCreateFailed,
        "another API's Evaluate never re-creates a Vulkan feature");

    // A route without CreateFeature1 cannot repeat the game's CreateFeature1.
    SetRequested(kPresetJ);
    Check(Evaluate(0, VulkanEvaluateOf(false), &FakeRelease, game, &parameters) == replacement
        && gProviderCreates.size() == creates && gVkCreate.creates == 0,
        "no CreateFeature1 on the route: no re-create through CreateFeature instead");

    SetRequested(kGameOrDriver);
    const auto* plain = Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters);
    Check(plain != replacement && plain != game && gProviderCreates.back().hintSeen == kPresetJ
        && gVkCreate1.creates == 3, "game/driver re-creates through CreateFeature1 with the game's own hint");
    gNow += 300;
    for (int i = 0; i != 5; ++i) Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters);
    Check(gReleased.size() == 1 && gReleased[0] == reinterpret_cast<uintptr_t>(replacement),
        "the replaced M feature is released through the Vulkan route's Release");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0 && Read().counters.liveSr == 0,
        "the game's Vulkan Release leaves no feature alive, none twice");

    // RR through CreateFeature (no VkDevice), selected at start.
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kOk, kDrsLatest, Kind::eRayReconstruction);
    gVkCreate = {}; gVkCreate1 = {}; gInstallerDevices.clear();
    SetDrsHookInstaller(&TestInstaller);
    PrepareForVulkan();
    PrepareForVulkan();
    Check(gInstallerDevices.size() == 1 && gInstallerDevices[0] == nullptr,
        "Vulkan NGX Init prepares the DRS hook once, without a device");
    SetRequestedRr(kRrPresetE);
    parameters = GameParameters(Kind::eRayReconstruction);
    Check(Create(0, VulkanCreateOf(false), NVSDK_NGX_Feature_RayReconstruction, &parameters, &game)
        == NVSDK_NGX_Result_Success && game && gVkCreate.creates == 1 && gVkCreate.command == kGameBuffer
        && gProviderCreates.back().preset == kRrPresetE && Read().rrDetected,
        "Vulkan CreateFeature: RR E detected");
    SetRequestedRr(kRrPresetD);
    replacement = Evaluate(0, VulkanEvaluateOf(), &FakeRelease, game, &parameters);
    Check(replacement != game && gVkCreate.creates == 2 && gVkCreate1.creates == 0
        && gVkCreate.command == kEvaluateBuffer && gProviderCreates.back().preset == kRrPresetD,
        "RR re-creates D through CreateFeature on the Evaluate's command buffer");
    Check(parameters.Has("RayReconstruction.Hint.Render.Preset.Quality", FakeParameters::Type::eSigned, 4),
        "the game's RR hint is restored after the Vulkan re-create");
    BeforeRelease(0, &FakeRelease, game);
    AfterRelease(0, game, FakeRelease(game));
    Check(gLiveHandles.empty() && gBadReleases == 0, "Vulkan RR: every feature released exactly once");

    // A Create1 without a VkDevice is not callable.
    Session(dlssg_preset::kSettingNotFound, dlssg_preset::kSettingNotFound, 0);
    CreateCall noDevice = VulkanCreateOf(true);
    noDevice.device = nullptr;
    game = nullptr;
    Check(Create(0, noDevice, NVSDK_NGX_Feature_SuperSampling, &parameters, &game)
        == NVSDK_NGX_Result_FAIL_NotInitialized && !game && gProviderCreates.empty(),
        "CreateFeature1 without its VkDevice is refused, never forwarded");
    SetDrsHookInstaller(nullptr);
}
}

int main()
{
    TrackerCases();
    KindCases();
    DrsCases();
    SelectedAtStartCases();
    RayReconstructionCases();
    DriverFixedCases();
    NoOverrideCases();
    ConcurrencyCases();
    SehCases();
    SharedParameterCases();
    VulkanCases();
    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
