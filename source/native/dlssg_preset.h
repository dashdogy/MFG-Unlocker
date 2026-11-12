#pragma once

#include "dlssg_preset_policy.h"
#include "entry_detour.h"

#include <Windows.h>
#include <cstdint>

namespace dlssg_preset
{
struct SelectionSnapshot
{
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
};

struct Snapshot
{
    bool installed = false;
    uint32_t failure = 0;
    uint64_t providerReads = 0;
    bool cachedReaderInstalled = false;
    uint32_t cachedReaderFailure = 0;
    uint32_t cachedReaderRva = 0;
    uint64_t cachedReaderReads = 0;
    uint32_t requested = 2;
    uint32_t latched = 2;
    bool selectionFrozen = false;
    bool restartRequired = false;
    uint32_t observedPreset = 0;
    bool observedPresetValid = false;
    uint64_t overrideReads = 0;
    // The provider implements Preset C (DLSS-G 310.9.16 and newer).
    bool presetCAvailable = false;
    bool configurationResetAvailable = false;
    uint32_t configurationSlotRva = 0;
    uint64_t configurationReads = 0;
    uint64_t reportReads = 0;
    uint64_t configurationResets = 0;
};

// 0 leaves game/driver reads untouched; 1 selects A, 2 selects B and 3 selects C
// on providers that implement it (elsewhere C leaves the game/driver value). The
// selection freezes at its first relevant provider query. Later requests are saved
// (restartRequired) without replacing cached settings or live programs; RelatchPending
// applies one at a point with no frame-generation feature, otherwise a restart does.
bool SetRequested(uint32_t preset) noexcept;
uint32_t FreezeSelection() noexcept;
// The selection a provider receives: C becomes the game/driver value (0) on providers without it.
uint32_t EffectiveSelection(uint32_t preset, HMODULE provider) noexcept;
SelectionSnapshot ReadSelection() noexcept;
using InitialSelectionLoader = uint32_t (*)() noexcept;
// Assign before enabling hooks. Only FreezeSelection calls this loader, once,
// outside the state lock and only if SetRequested has not already initialized it.
void SetInitialSelectionLoader(InitialSelectionLoader loader) noexcept;
// Query-result evidence only. The caller owns a validated, process-retained
// exact provider route and has established output-buffer ownership.
void RecordProviderRead(HMODULE provider, bool valueValid, uint32_t value, bool overridden) noexcept;

// Live switching. The probe reports true only while no frame-generation
// Create is in flight and no frame-generation feature lives. It must be
// read-only and nonblocking: RelatchPending calls it with the selection lock
// held, which orders it against every provider read (each read takes that lock
// after its Create has been counted in flight).
using CreateQuiescenceProbe = bool (*)() noexcept;
void SetCreateQuiescenceProbe(CreateQuiescenceProbe probe) noexcept;
// Replaces a frozen selection with the saved request while the probe reports
// quiescence. The next provider read freezes the new value. False when nothing
// is pending or the probe is absent or denies.
bool RelatchPending() noexcept;
// The configuration singleton is structurally identified and every endpoint
// Create/Evaluate plus the cached reader have current, generation-matched hooks.
bool ProviderSupportsLiveSwitch(HMODULE provider) noexcept;
// RTX 30/20 translated route: that route's own detour (`readerHook`)
// owns the provider's cached preset reader at `reader`. Discovers the endpoint
// configuration and installs only the endpoint gates; live switching then also
// requires `readerHook` to stay current with `generation` at `reader`. Call
// before the provider's first Create. False leaves the provider restart-only.
bool AdoptRouteReader(HMODULE module, void* reader, uint64_t generation,
    entry_detour::Handle readerHook) noexcept;
// A provider read through a route-owned reader, classified by its caller like
// the reads of this module's own reader hook (configuration, report or other).
void RecordRouteRead(HMODULE provider, const void* caller, bool valueValid, uint32_t value,
    bool overridden) noexcept;
// Translated route: its reader hook alone answers the FG preset key, so the
// NvAPI hook leaves that key to the driver (legacy providers accept only their
// original preset). Set before Prepare(); DRS setting filters are unaffected.
void SetNvapiFrameGenerationReads(bool enabled) noexcept;
// Before a Create selects a provider: every provider that has read the preset
// supports live switching, and at least one has read it.
bool ReadProvidersSupportLiveSwitch() noexcept;

// Call at a normal initialization/API boundary, outside DllMain. Installs only
// a process-memory read hook; no NVIDIA profile set/save API is used.
bool Prepare() noexcept;
// Reads of up to four other DRS keys (the DLSS SR and RR presets)
// reach `filter` after the driver answered; its return value is what the caller
// receives. `writable` is false when the hook is not current or the buffer is
// not owned. Each call adds `id`; a null filter clears every key.
using SettingFilter = int32_t (*)(uint32_t id, int32_t result, Setting* setting, bool writable,
    HMODULE caller) noexcept;
void SetSettingFilter(uint32_t id, SettingFilter filter) noexcept;
void PrepareForExport(HMODULE module, const char* name) noexcept;
// Bounded entry discovery; no NVIDIA initialization is invoked here.
void ObserveProvider(HMODULE module, uint64_t generation) noexcept;
// The provider's FG preset reader: every decoded preset-key call site must call this one
// reader (known prologue, at its unwind entry). Null when absent or when sites disagree.
void* FindPresetReader(HMODULE module) noexcept;
Snapshot ReadSnapshot(HMODULE provider) noexcept;
}
