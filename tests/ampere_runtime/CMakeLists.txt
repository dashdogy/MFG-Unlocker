cmake_minimum_required(VERSION 3.24)
project(AmpereRuntimeRegression LANGUAGES C CXX RC ASM_MASM)
set(NATIVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
set(MINHOOK "${NATIVE_SOURCE}/third_party/minhook")
set(MINHOOK_SOURCES "${MINHOOK}/src/buffer.c" "${MINHOOK}/src/hook.c" "${MINHOOK}/src/trampoline.c" "${MINHOOK}/src/hde/hde64.c")
add_executable(AmpereBatchTests batch_tests.cpp)
# slinitfg.1: the RTX 30/20 FG requirements answer during slInit/startup.
add_executable(AmpereStartupRequirementsTests startup_requirements_tests.cpp)
add_executable(AmpereOptionsTests options_tests.cpp)
add_executable(AmpereFlipMeteringTests flip_metering_tests.cpp "${NATIVE_SOURCE}/ampere_flip_metering.cpp")
# rtx30flip.1: the flip-metering opt-in (settings, SetFlipConfig observation, Active state).
add_executable(AmpereFlipObserverTests flip_observer_tests.cpp "${NATIVE_SOURCE}/ampere_flip_metering.cpp")
target_compile_definitions(AmpereFlipObserverTests PRIVATE MFG_AMPERE_FLIP_METERING_TESTING=1)
# optirtx30.1: when OptiScaler's private Streamline carries the RTX 30 startup.
add_executable(OptiScalerPrivateStartupTests optiscaler_private_startup_tests.cpp)
add_executable(AmpereStatusFieldsTests status_fields_tests.cpp
    "${NATIVE_SOURCE}/ampere_provider_arch.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_link_libraries(AmpereStatusFieldsTests PRIVATE version psapi)
add_executable(StreamlineOtaTests ota_tests.cpp "${NATIVE_SOURCE}/nvidia_mfg_policy.cpp"
    "${NATIVE_SOURCE}/optiscaler_host.cpp")
add_executable(AmpereArchitectureTests architecture_tests.cpp
    "${NATIVE_SOURCE}/ampere_provider_arch.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_link_libraries(AmpereArchitectureTests PRIVATE version psapi)
# Needs the installed NVIDIA driver (System32 nvapi64.dll with the driver-store nvapi64_impl.dll).
add_executable(NvapiResolveTests nvapi_resolve_tests.cpp "${NATIVE_SOURCE}/ampere_provider_arch.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp" ${MINHOOK_SOURCES})
target_link_libraries(NvapiResolveTests PRIVATE version psapi)
# nvpresent.1: the driver-store path rule for the menu's NvPresent64.dll base and the NGX runtime (paths only).
add_executable(DriverPathTests driver_path_tests.cpp)
# winemenu.1: InsideLoader() without RtlIsThreadWithinLoaderCallout (Wine/Proton).
add_executable(LoaderLockTests loader_lock_tests.cpp)
# GitHub issue 158: the per-handle real-frame table reuses its least recently used slot.
add_executable(FirstSampleTableTests first_sample_table_tests.cpp)
# GitHub issue 149 follow-up (Indiana Jones, RTX 4090): the RTX 40 2X hold follows the provider a Create uses, and an
# old bundled provider inspected again mid-game cannot set it beside NVIDIA's provider with the check.
add_executable(AdaGateHoldTests ada_gate_hold_tests.cpp)
# Needs an NVIDIA DXGI hardware adapter; detours the real System32 dxgi.dll in this process only.
add_executable(GpuNameOverrideTests gpu_name_override_tests.cpp "${NATIVE_SOURCE}/gpu_name_override.cpp" ${MINHOOK_SOURCES})
target_compile_definitions(GpuNameOverrideTests PRIVATE MFG_GPU_NAME_OVERRIDE_TESTING=1)
target_link_libraries(GpuNameOverrideTests PRIVATE dxgi)
add_library(GpuNameProbe SHARED gpu_name_probe.cpp)
set_target_properties(GpuNameProbe PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(GpuNameOverrideTests GpuNameProbe)
add_executable(AmpereWrapperTests wrapper_tests.cpp
    "${NATIVE_SOURCE}/ampere_wrapper_capacity.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
# vulkan.1: the RTX 30 Vulkan route's output view, Vulkan selection policy and VkPhysicalDevice LUID.
# With NGX_INCLUDE_DIR (Streamline SDK external/ngx-sdk/include) and VULKAN_INCLUDE_DIR the prefixes are
# checked against the real headers. Argument "gpu" needs a Vulkan device (exit 2 otherwise).
set(NGX_INCLUDE_DIR "" CACHE PATH "NGX SDK headers for the Vulkan layout checks")
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Vulkan SDK headers for the Vulkan layout checks")
add_executable(AmpereVulkanRouteTests vulkan_route_tests.cpp)
target_link_libraries(AmpereVulkanRouteTests PRIVATE dxgi)
if(NGX_INCLUDE_DIR AND VULKAN_INCLUDE_DIR)
    target_include_directories(AmpereVulkanRouteTests PRIVATE "${NGX_INCLUDE_DIR}" "${VULKAN_INCLUDE_DIR}")
    target_compile_definitions(AmpereVulkanRouteTests PRIVATE MFG_TEST_NGX_VK=1)
endif()
add_executable(DlssgSelectionTests selection_tests.cpp
    "${NATIVE_SOURCE}/dlssg_provider_selection.cpp"
    "${NATIVE_SOURCE}/dlssg_provider_policy.cpp")
target_link_libraries(DlssgSelectionTests PRIVATE version wintrust crypt32)
target_compile_definitions(DlssgSelectionTests PRIVATE MFG_PROVIDER_SELECTION_TESTING=1)
# livepreset.1: live DLSS-G preset switching (switch policy, FG lifetime tracker, production preset latch).
add_executable(DlssgPresetSwitchTests preset_switch_tests.cpp "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_link_libraries(DlssgPresetSwitchTests PRIVATE bcrypt version)
# srpreset.1: DLSS SR detection and live SR preset switching (tracker + production dlss_sr.cpp on a fake NGX runtime).
# Needs the NGX SDK headers of the pinned Streamline SDK.
set(STREAMLINE_ROOT "" CACHE PATH "Streamline SDK 2.14.1 root (external/ngx-sdk/include)")
if(EXISTS "${STREAMLINE_ROOT}/external/ngx-sdk/include/nvsdk_ngx.h")
    add_executable(DlssSrPresetTests sr_preset_tests.cpp "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
    target_include_directories(DlssSrPresetTests PRIVATE "${STREAMLINE_ROOT}/external/ngx-sdk/include")
    target_compile_definitions(DlssSrPresetTests PRIVATE MFG_DLSS_SR_TESTING=1)
    set(MFG_DLSS_SR_TESTS DlssSrPresetTests)
else()
    message(STATUS "DlssSrPresetTests skipped: set STREAMLINE_ROOT to the Streamline SDK with external/ngx-sdk")
    set(MFG_DLSS_SR_TESTS)
endif()
configure_file("${NATIVE_SOURCE}/entry_detour.asm" "${CMAKE_CURRENT_BINARY_DIR}/preset_entry_detour.asm" COPYONLY)
add_executable(DlssgPresetHookInstallTests preset_hook_install_tests.cpp "${NATIVE_SOURCE}/dlssg_provider_policy.cpp"
    "${NATIVE_SOURCE}/entry_detour.cpp" "${CMAKE_CURRENT_BINARY_DIR}/preset_entry_detour.asm" ${MINHOOK_SOURCES})
target_link_libraries(DlssgPresetHookInstallTests PRIVATE version)
add_executable(DlssSrOverrideAudit sr_override_audit.cpp "${NATIVE_SOURCE}/entry_detour.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/preset_entry_detour.asm" ${MINHOOK_SOURCES})
# ngxforward.1: entry_detour's overlaid state (another hook over our entry) on real detours in a fixture DLL.
add_executable(EntryOverlayTests entry_overlay_tests.cpp "${NATIVE_SOURCE}/entry_detour.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/preset_entry_detour.asm" ${MINHOOK_SOURCES})
add_library(EntryOverlayFixture SHARED entry_overlay_fixture.cpp entry_overlay_fixture.asm entry_overlay_fixture.def)
set_target_properties(EntryOverlayFixture PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(EntryOverlayTests EntryOverlayFixture)
foreach(target AmpereBatchTests AmpereStartupRequirementsTests AmpereOptionsTests AmpereFlipMeteringTests AmpereFlipObserverTests AmpereStatusFieldsTests StreamlineOtaTests AmpereArchitectureTests AmpereWrapperTests DlssgSelectionTests NvapiResolveTests DriverPathTests LoaderLockTests FirstSampleTableTests AdaGateHoldTests GpuNameOverrideTests OptiScalerPrivateStartupTests DlssgPresetSwitchTests ${MFG_DLSS_SR_TESTS} AmpereVulkanRouteTests DlssgPresetHookInstallTests DlssSrOverrideAudit EntryOverlayTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${NATIVE_SOURCE}" "${MINHOOK}/include")
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
foreach(target SelectionOld SelectionNew SelectionUnsupported SelectionSibling)
    add_library(${target} SHARED selection_fixture.cpp selection_fixture.rc)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
target_compile_definitions(SelectionOld PRIVATE FIXTURE_MINOR=1 FIXTURE_BUILD=0)
target_compile_definitions(SelectionNew PRIVATE FIXTURE_MINOR=9 FIXTURE_BUILD=1)
target_compile_definitions(SelectionUnsupported PRIVATE FIXTURE_MINOR=10 FIXTURE_BUILD=0)
target_compile_definitions(SelectionSibling PRIVATE FIXTURE_MINOR=9 FIXTURE_BUILD=1 FIXTURE_SIBLING=1)
# No inputs: run with every build. The flip-metering, wrapper, selection and architecture tests take real
# Streamline wrappers and DLSS-G providers as arguments (see BUILD.md).
enable_testing()
foreach(target AmpereBatchTests AmpereStartupRequirementsTests AmpereOptionsTests AmpereFlipObserverTests AmpereStatusFieldsTests StreamlineOtaTests OptiScalerPrivateStartupTests DlssgPresetSwitchTests ${MFG_DLSS_SR_TESTS} AmpereVulkanRouteTests EntryOverlayTests DriverPathTests LoaderLockTests FirstSampleTableTests AdaGateHoldTests)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
# Exit code 2 is a setup failure (no NVIDIA driver or no split nvapi64_impl.dll), never a pass.
add_test(NAME NvapiResolveTests COMMAND NvapiResolveTests)
# Exit code 2 is a setup failure (no NVIDIA DXGI adapter or no GpuNameProbe.dll), never a pass.
add_test(NAME GpuNameOverrideTests COMMAND GpuNameOverrideTests)
