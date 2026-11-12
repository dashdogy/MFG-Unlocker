cmake_minimum_required(VERSION 3.24)
project(XemfgStandaloneTests LANGUAGES CXX)
# xemfgsa.1 (V1.7.0): XeMFG standalone's API-free decisions (source/native/xess/standalone_policy.h), on the CPU.
set(NATIVE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
add_executable(XemfgStandalonePolicyTests policy_tests.cpp)
target_compile_features(XemfgStandalonePolicyTests PRIVATE cxx_std_20)
target_include_directories(XemfgStandalonePolicyTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgStandalonePolicyTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# xemfgsa.6: the Dynamic MFG controllers ported from our OptiScaler fork, with its simulations (/W4 /WX as there).
foreach(test dynamic_count dynamic_cap)
    add_executable(Xemfg_${test}_tests ${test}_tests.cpp)
    target_compile_features(Xemfg_${test}_tests PRIVATE cxx_std_20)
    target_include_directories(Xemfg_${test}_tests PRIVATE "${NATIVE}")
    target_compile_options(Xemfg_${test}_tests PRIVATE /W4 /WX)
endforeach()
# latency.1: the low-latency pacing controller (source/native/xess/latency_pacing.h) and its closed-loop simulations.
add_executable(XemfgLatencyPacingTests latency_pacing_tests.cpp)
target_compile_features(XemfgLatencyPacingTests PRIVATE cxx_std_20)
target_include_directories(XemfgLatencyPacingTests PRIVATE "${NATIVE}")
target_compile_options(XemfgLatencyPacingTests PRIVATE /W4 /WX)
# alpha.15 (B4-12): the runtime search both hosts share (source/native/xess/runtime_search.h), on folder fixtures.
add_executable(XemfgRuntimeSearchTests runtime_search_tests.cpp)
target_compile_features(XemfgRuntimeSearchTests PRIVATE cxx_std_20)
target_include_directories(XemfgRuntimeSearchTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgRuntimeSearchTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(XemfgRuntimeSearchTests PRIVATE /W4 /WX /utf-8)
# The D3D12 takeover on a real device (GPU, a window, the profiled XeSS SDK 3.0.2 pair): run by a build folder's
# runner against an exact candidate DLL, not by CTest.
add_library(XessFixture SHARED xess_fixture.cpp)
add_executable(XemfgTakeoverHarness takeover_harness.cpp)
target_compile_features(XemfgTakeoverHarness PRIVATE cxx_std_20)
target_include_directories(XemfgTakeoverHarness PRIVATE "${NATIVE}" "${CMAKE_CURRENT_LIST_DIR}/../release_regression")
target_compile_definitions(XemfgTakeoverHarness PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(XemfgTakeoverHarness PRIVATE d3d12 dxgi user32)
enable_testing()
add_test(NAME xemfg-frame-ids COMMAND XemfgStandalonePolicyTests ids)
add_test(NAME xemfg-present-anchor COMMAND XemfgStandalonePolicyTests anchor)
add_test(NAME xemfg-sources COMMAND XemfgStandalonePolicyTests sources)
add_test(NAME xemfg-conventions COMMAND XemfgStandalonePolicyTests conventions)
add_test(NAME xemfg-generation COMMAND XemfgStandalonePolicyTests generation)
add_test(NAME xemfg-rates COMMAND XemfgStandalonePolicyTests rates)
add_test(NAME xemfg-dynamic COMMAND XemfgStandalonePolicyTests dynamic)
# sapacing.1: the flip queue per host, the control key xemfgFlipQueue and the OptiScaler.ini override.
add_test(NAME xemfg-flip-queue COMMAND XemfgStandalonePolicyTests queue)
# winequeue.1: under Wine/Proton the queue's auto default is off on every host; the ini and the key still decide.
add_test(NAME xemfg-flip-queue-wine COMMAND XemfgStandalonePolicyTests wine)
# metergate.1: NVIDIA flip metering only with the driver's SetFlipConfig V2 report. hosthandle.1: host context routing.
add_test(NAME xemfg-flip-metering COMMAND XemfgStandalonePolicyTests metering)
add_test(NAME xemfg-host-routing COMMAND XemfgStandalonePolicyTests routing)
# optisleep.1: the libxell probes act on OptiScaler's calls only; the Present markers.
add_test(NAME xemfg-host-probes COMMAND XemfgStandalonePolicyTests probes)
# sdkscan.1: the XeSS discoveries' module list (psapi without a cap, plus Toolhelp).
add_test(NAME xemfg-module-list COMMAND XemfgStandalonePolicyTests modules)
# ngxprov.1: the Nvngx replacement provider's batch rule, OptiScaler's custom DLSSG DLL choice and the D3D12 formats.
add_test(NAME xemfg-ngx-provider COMMAND XemfgStandalonePolicyTests provider)
# pluginprov.2: Chromium-family helpers (CEF: BeamNG.drive's --type=renderer) get proxy forwarding only.
add_test(NAME xemfg-chromium-helper COMMAND XemfgStandalonePolicyTests chromium)
# hostport.1: OptiScaler's own XeFG publishes the port on a tracked route's idle unlock (a game's own XeFG probe first).
add_test(NAME xemfg-host-port COMMAND XemfgStandalonePolicyTests hostport)
add_test(NAME xemfg-dynamic-count COMMAND Xemfg_dynamic_count_tests)
add_test(NAME xemfg-dynamic-cap COMMAND Xemfg_dynamic_cap_tests)
add_test(NAME xemfg-runtime-search COMMAND XemfgRuntimeSearchTests)
add_test(NAME xemfg-latency-pacing COMMAND XemfgLatencyPacingTests)
# pacerfix.1: the flip queue's small-chain rule, pass-latency spacing, gate lead bound and precise-wait margin
# (sdk_pacing_meter.h). pacerfix.2: the DLSS-G cadence estimator (pacelock.1's xemfg-cadence-drain) is gone with the CPU
# deadline override; the clock's evidence rule, gate-ahead slots and ungated metered batches are tested here instead.
add_executable(XemfgFlipQueueClockTests flip_queue_tests.cpp)
target_compile_features(XemfgFlipQueueClockTests PRIVATE cxx_std_20)
target_include_directories(XemfgFlipQueueClockTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgFlipQueueClockTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME xemfg-flip-queue-small-chain COMMAND XemfgFlipQueueClockTests small)
add_test(NAME xemfg-flip-queue-pass COMMAND XemfgFlipQueueClockTests pass)
add_test(NAME xemfg-flip-queue-lead COMMAND XemfgFlipQueueClockTests lead)
add_test(NAME xemfg-flip-queue-margin COMMAND XemfgFlipQueueClockTests margin)
add_test(NAME xemfg-flip-queue-evidence COMMAND XemfgFlipQueueClockTests evidence)
add_test(NAME xemfg-flip-queue-gate-ahead COMMAND XemfgFlipQueueClockTests ahead)
add_test(NAME xemfg-flip-queue-ungated COMMAND XemfgFlipQueueClockTests ungated)
# pacerfix.3: the user's Subnautica 2 start replayed, the game-wait baseline, no floor reset at the game's own floor, and a
# restarted rate's two hand-overs.
add_test(NAME xemfg-flip-queue-startup COMMAND XemfgFlipQueueClockTests startup)
add_test(NAME xemfg-flip-queue-baseline COMMAND XemfgFlipQueueClockTests baseline)
add_test(NAME xemfg-flip-queue-floor COMMAND XemfgFlipQueueClockTests floor)
add_test(NAME xemfg-flip-queue-relearn COMMAND XemfgFlipQueueClockTests relearn)
# heldprobe.1: a lock-in the host's evidence cannot see is probed out by held first outputs, never under the floor.
add_test(NAME xemfg-flip-queue-held COMMAND XemfgFlipQueueClockTests held)
# amdmodel.1 (sleepsight.1): AMD on OptiScaler's route as closed loops: the XeLL / Anti-Lag 2 lock escapes, a GPU-bound
# game stays at the GPU's rate (with and without a measured sleep), a Reflex cap holds, and a measured zero sleep runs no
# held probe.
add_test(NAME xemfg-flip-queue-amd COMMAND XemfgFlipQueueClockTests amd)
# pacerfix.2: the flip queue's per-output trace (pacing_trace.h): a write/read round trip, and a replay of a recorded
# trace through a fresh clock that must reproduce every decision. `XemfgTraceReplayTests <file>` replays a user's
# <game>.pacing.trace (not run by CTest).
add_executable(XemfgTraceReplayTests trace_replay_tests.cpp)
target_compile_features(XemfgTraceReplayTests PRIVATE cxx_std_20)
target_include_directories(XemfgTraceReplayTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgTraceReplayTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME xemfg-trace-replay COMMAND XemfgTraceReplayTests)
# pacediag.1: the display-side diagnostics' recorder (two chunks, drops counted, a cap per queue) and CSV rows
# (source/native/xess/pacing_diag.h).
add_executable(XemfgPacingDiagTests pacing_diag_tests.cpp)
target_compile_features(XemfgPacingDiagTests PRIVATE cxx_std_20)
target_include_directories(XemfgPacingDiagTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgPacingDiagTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME xemfg-pacing-diag COMMAND XemfgPacingDiagTests)
# latmon.1: the latency probe's queue and CSV row (source/native/latency_probe_ring.h).
add_executable(XemfgLatencyProbeRingTests latency_probe_ring_tests.cpp)
target_compile_features(XemfgLatencyProbeRingTests PRIVATE cxx_std_20)
target_include_directories(XemfgLatencyProbeRingTests PRIVATE "${NATIVE}")
target_compile_options(XemfgLatencyProbeRingTests PRIVATE /W4 /WX)
add_test(NAME xemfg-latency-probe-ring COMMAND XemfgLatencyProbeRingTests)
# latmon.2: the live PC-latency estimator behind the menu's Latency row (source/native/latency_monitor.h).
add_executable(XemfgLatencyMonitorTests latency_monitor_tests.cpp)
target_compile_features(XemfgLatencyMonitorTests PRIVATE cxx_std_20)
target_include_directories(XemfgLatencyMonitorTests PRIVATE "${NATIVE}")
target_compile_options(XemfgLatencyMonitorTests PRIVATE /W4 /WX)
add_test(NAME xemfg-latency-monitor COMMAND XemfgLatencyMonitorTests)
# fg95prov.1: the Nvngx replacement provider's XeFG image profiles (fg78, fg95) and Resolve's routing
# (source/native/xess/vulkan/xefg_internal_profiles.h, xefg_internal.cpp). With XEFG_FG78_IMAGE / XEFG_FG95_IMAGE set
# to those libxess_fg.dll files, Resolve also runs on the real images (mapped without running any of their code).
add_executable(XemfgXefgProfileTests xefg_profile_tests.cpp "${NATIVE}/xess/vulkan/xefg_internal.cpp")
target_compile_features(XemfgXefgProfileTests PRIVATE cxx_std_20)
target_include_directories(XemfgXefgProfileTests PRIVATE "${NATIVE}")
target_compile_definitions(XemfgXefgProfileTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME xemfg-xefg-profiles COMMAND XemfgXefgProfileTests all)
set(XEFG_FG78_IMAGE "" CACHE FILEPATH "libxess_fg.dll 1.3.1.78 (EC5E0C65) for xemfg-xefg-image-fg78")
set(XEFG_FG95_IMAGE "" CACHE FILEPATH "libxess_fg.dll 1.3.4.95 (752B1491) for xemfg-xefg-image-fg95")
if(XEFG_FG78_IMAGE)
    add_test(NAME xemfg-xefg-image-fg78 COMMAND XemfgXefgProfileTests image "${XEFG_FG78_IMAGE}" fg78)
endif()
if(XEFG_FG95_IMAGE)
    add_test(NAME xemfg-xefg-image-fg95 COMMAND XemfgXefgProfileTests image "${XEFG_FG95_IMAGE}" fg95)
endif()
