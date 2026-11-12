#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

struct IDXGISwapChain;

// latmon.1: PC latency, live in the menu and optionally recorded to a file for offline analysis against PresentMon
// (tests/analyze_latency_probe.py): from the game's simulation-start marker of a real frame to the moment the first
// output carrying that frame is flipped on screen (latency_monitor.h has the rules).
//
// Switches in RTX40MFG-UI.ini beside the game, read once per launch:
// - [Overlay] LatencyMonitor=0 turns it all off: no marker wrap, no recording, no Latency row. Default on.
// - [Diagnostics] LatencyProbe=1 also writes every event to RTXMFG-<game>-<hash>-latency-<pid>.csv in the log
//   directory (up to 512 MB).
//
// It measures all the time while available, so the menu's row and the pinned readout are live the moment they are
// shown (ReadLive, refreshed every 100 ms by the backend's loop).
//
// The events (latency_probe_ring.h Kind): the game's latency markers, stamped before they are forwarded (the Streamline
// PCL / legacy Reflex marker wrap); its Present on its own chain; every output Present with that native chain's
// present count and frame statistics read right after it; XeFG's per-Present frame labels; DLSS-G's state queries.
// Producers never block, allocate or write files: a bounded queue takes the events and counts any it refuses. The
// backend's 100 ms loop drains it into the estimator and, with the probe switch, the file.
namespace latency_probe
{
// Reads the switches (the first caller; the backend calls Start before its loop) and logs them.
void Start() noexcept;
// The monitor may run this launch ([Overlay] LatencyMonitor is not 0): the marker setters are wrapped at resolve time.
bool Available() noexcept;
// Events are being recorded now (cheap; the producers' check).
bool Recording() noexcept;
int64_t Now() noexcept;

void Marker(uint32_t marker, uint64_t frame, uint32_t source) noexcept;
void GamePresent(const void* chain, int64_t entry, int64_t returned, HRESULT result, UINT sync, UINT flags,
    uint16_t role) noexcept;
// Records the output Present, then reads `native` on this thread (the Present has returned; no other call is made).
void OutputPresent(const void* chain, IDXGISwapChain* native, int64_t entry, int64_t returned, HRESULT result,
    UINT sync, UINT flags, uint16_t role) noexcept;
void XefgOutput(uint64_t frame, bool generated) noexcept;
void DlssgState(uint32_t presented, uint32_t status) noexcept;

// The backend's loop only (one thread): starts and stops recording, feeds the estimator, writes the file.
void Flush() noexcept;

// The live readout, for the menu (ReadLive, any thread) and the status file (latencyAvailable, latencyState, ...).
// State: latency_monitor::State.
struct Live
{
    bool available = false;
    uint32_t state = 0, frames = 0, exactPermille = 0;
    uint32_t meanUs = 0, p99Us = 0, realUs = 0, gameUs = 0, fgUs = 0, displayUs = 0;
    uint64_t ageMs = UINT64_MAX; // since the newest measured frame's flip
};
Live ReadLive() noexcept;
// The status file's latency fields, starting with a comma.
std::string StatusFragment();
}
