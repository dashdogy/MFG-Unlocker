cmake_minimum_required(VERSION 3.24)
# XeFG tensor-core route (source/native/xess/cuda): CUDA kernels against Intel's DXIL programs through
# the production route code. GPU test: needs an NVIDIA RTX 20/30/40/50 adapter and no game using the GPU.
project(XefgCudaTests LANGUAGES C CXX)
set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native")
enable_testing()
set(XEFG_CUDA_FATBIN "" CACHE FILEPATH "xefg_cuda_layers.fatbin from the product build (generated/xess_cuda)")
set(XEFG_NATIVE_PROGRAMS "" CACHE PATH "Directory with libxess_fg 3.0.2's U-Net and flow DXIL programs")
set(XEFG_PORTED_PROGRAMS "" CACHE PATH "Directory with RTXMFG's embedded ported programs (xess-programs/programs)")
# unet.1: RTXMFG's own shaders from the product build (generated/xess_shaders); the U-Net layers that ShaderPort
# swaps are also tested as RTXMFG's programs (output = Intel's, recognized by the route).
set(XEFG_OWN_SHADERS "" CACHE PATH "Directory with RTXMFG's generated XeFG shaders (generated/xess_shaders)")
# Optional: an Agility SDK runtime (a game's D3D12Core.dll, e.g. Witcher 3's SDK 619) and its D3D12SDKVersion.
# That runtime gives each command list its own table on its first Reset.
set(XEFG_AGILITY_CORE "" CACHE FILEPATH "Agility SDK D3D12Core.dll for the per-list table tests (optional)")
set(XEFG_AGILITY_SDK_VERSION "" CACHE STRING "D3D12SDKVersion of XEFG_AGILITY_CORE")
# Optional: ReShade (its dxgi.dll, a full add-on build, 6.7+ for IID_UnwrappedObject) and add-ons (e.g. RenoDX) beside
# copies of the tests. ReShade then wraps the device, lists and heaps the tests use, as it does XeFG's in a game
# (board #396/#397: Witcher 3 with ReShade 6.8.0 + RenoDX).
set(XEFG_RESHADE_DXGI "" CACHE FILEPATH "ReShade dxgi.dll for the wrapped-device tests (optional)")
set(XEFG_RESHADE_ADDONS "" CACHE STRING "ReShade add-on files loaded with it (optional, ;-separated)")
if(NOT EXISTS "${XEFG_CUDA_FATBIN}" OR NOT IS_DIRECTORY "${XEFG_NATIVE_PROGRAMS}" OR NOT IS_DIRECTORY "${XEFG_PORTED_PROGRAMS}"
    OR NOT IS_DIRECTORY "${XEFG_OWN_SHADERS}")
    message(FATAL_ERROR "Explicit XEFG_CUDA_FATBIN, XEFG_NATIVE_PROGRAMS, XEFG_PORTED_PROGRAMS and XEFG_OWN_SHADERS are required")
endif()
# V0's import prerequisite uses the real production module loader, independently of any game.
option(XEFG_CUDA_VULKAN_PROBE "Build the gated NVX module-import probe" OFF)
if(XEFG_CUDA_VULKAN_PROBE)
    set(XVK_VULKAN_INCLUDE_DIR "" CACHE PATH "Pinned Vulkan 1.4.363.0 headers")
    if(NOT EXISTS "${XVK_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
        message(FATAL_ERROR "Explicit XVK_VULKAN_INCLUDE_DIR required for the NVX probe")
    endif()
    add_executable(XefgCudaVkTests cuda_vk_tests.cpp "${NATIVE}/xess/vulkan/xvk_tensor.cpp" "${NATIVE}/xess/cuda/cuda_route.cpp")
    set(XEFG_CUDA_GENERATED "" CACHE PATH "Candidate native generated directory (program-set headers)")
    if(NOT EXISTS "${XEFG_CUDA_GENERATED}/embedded_xvk_programs.generated.h")
        message(FATAL_ERROR "Explicit XEFG_CUDA_GENERATED with the candidate program headers required")
    endif()
    target_sources(XefgCudaVkTests PRIVATE
        "${NATIVE}/xess/vulkan/xvk_device.cpp" "${NATIVE}/xess/vulkan/xvk_resource.cpp"
        "${NATIVE}/xess/vulkan/xvk_commandlist.cpp" "${NATIVE}/xess/vulkan/xvk_queue.cpp"
        "${NATIVE}/xess/vulkan/xvk_pipeline.cpp" "${NATIVE}/xess/vulkan/xvk_programs.cpp"
        "${NATIVE}/xess/vulkan/xvk_dxgi.cpp" "${NATIVE}/xess/vulkan/xvk_util.cpp"
        "${NATIVE}/embedded_payload.cpp" "${NATIVE}/third_party/lzma/LzmaDec.c")
    target_include_directories(XefgCudaVkTests PRIVATE "${XEFG_CUDA_GENERATED}")
    target_compile_features(XefgCudaVkTests PRIVATE cxx_std_20)
    target_compile_definitions(XefgCudaVkTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(XefgCudaVkTests PRIVATE "${NATIVE}" "${XVK_VULKAN_INCLUDE_DIR}")
    target_link_libraries(XefgCudaVkTests PRIVATE d3d12 dxgi bcrypt user32)
    set_target_properties(XefgCudaVkTests PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set(XEFG_CUDA_CANDIDATE_DLL "" CACHE FILEPATH "Candidate product DLL providing its embedded program set")
    foreach(case equivalence borders shadow_refresh refusals gate)
        add_test(NAME xefg_cuda_vk_${case} COMMAND XefgCudaVkTests --fatbin "${XEFG_CUDA_FATBIN}" --programs "${XEFG_NATIVE_PROGRAMS}"
            --programs "${XEFG_PORTED_PROGRAMS}" --programs "${XEFG_OWN_SHADERS}"
            --candidate-dll "${XEFG_CUDA_CANDIDATE_DLL}" --case ${case} --texturing 64 --module fatbin --params extras --barriers xefg)
    endforeach()
endif()
# psorefused.1 (board #543): a device wrapper and a module that hooks only pipeline creation (the shape of the SCSKiller
# shader recorder's d3d12.dll under ReShade), each in its own image beside every test executable; loaded only by
# --recorder / --wrapper-stream-subobject.
add_library(XefgPsoWrapper SHARED pso_wrapper.cpp)
add_library(XefgPsoRecorder SHARED pso_recorder.cpp)
foreach(module XefgPsoWrapper XefgPsoRecorder)
    target_compile_features(${module} PRIVATE cxx_std_20)
    target_compile_definitions(${module} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${module} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endforeach()
function(xefg_cuda_test_target name)
    add_executable(${name} cuda_route_tests.cpp "${NATIVE}/xess/cuda/cuda_route.cpp")
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(${name} PRIVATE "${NATIVE}")
    target_link_libraries(${name} PRIVATE d3d12 dxgi bcrypt)
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_dependencies(${name} XefgPsoWrapper XefgPsoRecorder)
    add_custom_command(TARGET ${name} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:XefgPsoWrapper>" "$<TARGET_FILE:XefgPsoRecorder>" "$<TARGET_FILE_DIR:${name}>")
endfunction()
set(XEFG_CUDA_ARGS --fatbin "${XEFG_CUDA_FATBIN}" --programs "${XEFG_NATIVE_PROGRAMS}" --programs "${XEFG_PORTED_PROGRAMS}"
    --programs "${XEFG_OWN_SHADERS}")
xefg_cuda_test_target(XefgCudaRouteTests)
add_test(NAME xefg_cuda_route_equivalence COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create)
add_test(NAME xefg_cuda_route_equivalence_list1 COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create1)
add_test(NAME xefg_cuda_route_wrapped_device COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --wrapped-device)
add_test(NAME xefg_cuda_route_architecture_gate COMMAND XefgCudaRouteTests --architecture-gate)
# dredtable.1 (board #432): with DRED on, the system runtime keeps its shared class table.
add_test(NAME xefg_cuda_route_dred COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create --dred)
# psorefused.1 (board #543): the provider's pipelines pass a module that hooks only pipeline creation, then a device wrapper
# (XefgPsoWrapper.dll), as compute pipeline calls or as streams; its lists are created directly. With the route's stack
# recognition every layer is recognized and launches; --strict (the rule before) recognizes none and the refusal names
# the recorder module. A wrapper that adds a SAMPLE_MASK subobject to the compute stream: logged, not recognized.
add_test(NAME xefg_cuda_route_recorder COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create --recorder)
add_test(NAME xefg_cuda_route_recorder_stream COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create1 --recorder --stream)
add_test(NAME xefg_cuda_route_recorder_strict COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create --recorder --strict)
add_test(NAME xefg_cuda_route_recorder_stream_strict COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create --recorder --stream --strict)
add_test(NAME xefg_cuda_route_wrapper_stream_subobject COMMAND XefgCudaRouteTests ${XEFG_CUDA_ARGS} --list create --wrapper-stream-subobject)
if(XEFG_AGILITY_CORE)
    if(NOT EXISTS "${XEFG_AGILITY_CORE}" OR NOT XEFG_AGILITY_SDK_VERSION MATCHES "^[0-9]+$")
        message(FATAL_ERROR "XEFG_AGILITY_CORE must name a D3D12Core.dll and XEFG_AGILITY_SDK_VERSION its SDK version")
    endif()
    xefg_cuda_test_target(XefgCudaRouteTestsAgility)
    target_compile_definitions(XefgCudaRouteTestsAgility PRIVATE XEFG_CUDA_AGILITY_SDK_VERSION=${XEFG_AGILITY_SDK_VERSION})
    add_custom_command(TARGET XefgCudaRouteTestsAgility POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:XefgCudaRouteTestsAgility>/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XEFG_AGILITY_CORE}" "$<TARGET_FILE_DIR:XefgCudaRouteTestsAgility>/D3D12/D3D12Core.dll")
    add_test(NAME xefg_cuda_route_agility COMMAND XefgCudaRouteTestsAgility ${XEFG_CUDA_ARGS} --list create --expect-agility)
    add_test(NAME xefg_cuda_route_agility_list1 COMMAND XefgCudaRouteTestsAgility ${XEFG_CUDA_ARGS} --list create1 --expect-agility)
    # dredtable.1 (board #432): with DRED on, an Agility runtime (SDK 618 as in Wuthering Waves, 619) keeps each list on
    # its class table in its own image; the route adopts that table as shared.
    add_test(NAME xefg_cuda_route_agility_dred COMMAND XefgCudaRouteTestsAgility ${XEFG_CUDA_ARGS} --list create --dred --expect-class-table)
    add_test(NAME xefg_cuda_route_agility_dred_list1 COMMAND XefgCudaRouteTestsAgility ${XEFG_CUDA_ARGS} --list create1 --dred --expect-class-table)
endif()
if(XEFG_RESHADE_DXGI)
    if(NOT EXISTS "${XEFG_RESHADE_DXGI}")
        message(FATAL_ERROR "XEFG_RESHADE_DXGI must name ReShade's dxgi.dll")
    endif()
    # No update check or tutorial: the tests create no swap chain, so ReShade's runtime never starts anyway.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ReShade.ini" "[GENERAL]\nCheckForUpdates=0\n[OVERLAY]\nTutorialProgress=4\n")
    function(xefg_cuda_reshade_files name)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XEFG_RESHADE_DXGI}" "$<TARGET_FILE_DIR:${name}>/dxgi.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/ReShade.ini" "$<TARGET_FILE_DIR:${name}>/ReShade.ini")
        foreach(addon IN LISTS XEFG_RESHADE_ADDONS)
            if(NOT EXISTS "${addon}")
                message(FATAL_ERROR "ReShade add-on not found: ${addon}")
            endif()
            add_custom_command(TARGET ${name} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${addon}" "$<TARGET_FILE_DIR:${name}>")
        endforeach()
    endfunction()
    # Own output directories: dxgi.dll beside an executable is loaded by every executable there.
    xefg_cuda_test_target(XefgCudaRouteTestsReShade)
    set_target_properties(XefgCudaRouteTestsReShade PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/reshade")
    xefg_cuda_reshade_files(XefgCudaRouteTestsReShade)
    add_test(NAME xefg_cuda_route_reshade COMMAND XefgCudaRouteTestsReShade ${XEFG_CUDA_ARGS} --list create --expect-wrapper)
    add_test(NAME xefg_cuda_route_reshade_list1 COMMAND XefgCudaRouteTestsReShade ${XEFG_CUDA_ARGS} --list create1 --expect-wrapper)
    # psorefused.1: the recorder module hooks ReShade's device table (the SCSKiller shape: recorder, then ReShade).
    add_test(NAME xefg_cuda_route_reshade_recorder COMMAND XefgCudaRouteTestsReShade ${XEFG_CUDA_ARGS} --list create --expect-wrapper --recorder)
    add_test(NAME xefg_cuda_route_reshade_recorder_strict COMMAND XefgCudaRouteTestsReShade ${XEFG_CUDA_ARGS} --list create --expect-wrapper --recorder --strict)
    if(XEFG_AGILITY_CORE)
        xefg_cuda_test_target(XefgCudaRouteTestsReShadeAgility)
        set_target_properties(XefgCudaRouteTestsReShadeAgility PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/reshade-agility")
        target_compile_definitions(XefgCudaRouteTestsReShadeAgility PRIVATE XEFG_CUDA_AGILITY_SDK_VERSION=${XEFG_AGILITY_SDK_VERSION})
        add_custom_command(TARGET XefgCudaRouteTestsReShadeAgility POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:XefgCudaRouteTestsReShadeAgility>/D3D12"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XEFG_AGILITY_CORE}" "$<TARGET_FILE_DIR:XefgCudaRouteTestsReShadeAgility>/D3D12/D3D12Core.dll")
        xefg_cuda_reshade_files(XefgCudaRouteTestsReShadeAgility)
        add_test(NAME xefg_cuda_route_reshade_agility COMMAND XefgCudaRouteTestsReShadeAgility ${XEFG_CUDA_ARGS} --list create --expect-agility --expect-wrapper)
        add_test(NAME xefg_cuda_route_reshade_agility_list1 COMMAND XefgCudaRouteTestsReShadeAgility ${XEFG_CUDA_ARGS} --list create1 --expect-agility --expect-wrapper)
    endif()
endif()

# Provider command-list proxy; separate executable preserves the existing D3D12 test entry point.
function(xefg_cuda_proxy_target name)
    add_executable(${name} proxy_tests.cpp "${NATIVE}/xess/cuda/cuda_route.cpp" "${NATIVE}/xess/cuda/provider_list.cpp")
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(${name} PRIVATE "${NATIVE}")
    target_link_libraries(${name} PRIVATE d3d12 dxgi bcrypt)
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endfunction()
xefg_cuda_proxy_target(XefgCudaProxyTests)
foreach(pair "equivalence;equivalence" "no_table_writes;table" "wrapped_list;wrapped" "stale_state;stale" "escape;escape" )
    list(GET pair 0 suffix)
    list(GET pair 1 mode)
    add_test(NAME xefg_cuda_proxy_${suffix} COMMAND XefgCudaProxyTests ${XEFG_CUDA_ARGS} --proxy-list --test ${mode})
endforeach()
add_test(NAME xefg_cuda_proxy_dred COMMAND XefgCudaProxyTests ${XEFG_CUDA_ARGS} --proxy-list --test table --dred)
if(XEFG_AGILITY_CORE)
    xefg_cuda_proxy_target(XefgCudaProxyTestsAgility)
    target_compile_definitions(XefgCudaProxyTestsAgility PRIVATE XEFG_CUDA_AGILITY_SDK_VERSION=${XEFG_AGILITY_SDK_VERSION})
    add_custom_command(TARGET XefgCudaProxyTestsAgility POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:XefgCudaProxyTestsAgility>/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XEFG_AGILITY_CORE}" "$<TARGET_FILE_DIR:XefgCudaProxyTestsAgility>/D3D12/D3D12Core.dll")
    add_test(NAME xefg_cuda_proxy_agility COMMAND XefgCudaProxyTestsAgility ${XEFG_CUDA_ARGS} --proxy-list --test equivalence --expect-agility)
    add_test(NAME xefg_cuda_proxy_agility_dred COMMAND XefgCudaProxyTestsAgility ${XEFG_CUDA_ARGS} --proxy-list --test table --dred --expect-agility)
endif()

add_test(NAME xefg_cuda_pipeline_tokens COMMAND XefgCudaProxyTests ${XEFG_CUDA_ARGS} --proxy-list --test tokens)

if(XEFG_CUDA_CANDIDATE_DLL)
    enable_language(RC)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(STREAMLINE_ROOT "" CACHE PATH "Established Streamline SDK containing external/ngx-sdk/include")
    set(XEFG_PROVIDER_RUNTIME "" CACHE PATH "Profiled XeFG 1.3.1.78 / XeLL runtime pair")
    if(NOT EXISTS "${STREAMLINE_ROOT}/external/ngx-sdk/include/nvsdk_ngx_params.h"
       OR NOT EXISTS "${XEFG_PROVIDER_RUNTIME}/libxess_fg.dll" OR NOT EXISTS "${XEFG_PROVIDER_RUNTIME}/libxell.dll")
        message(FATAL_ERROR "Provider fixture needs explicit STREAMLINE_ROOT and XEFG_PROVIDER_RUNTIME")
    endif()
    add_library(XefgProviderHostFixture SHARED ../release_regression/optiscaler_host_fixture.cpp ../release_regression/optiscaler_host_fixture.rc)
    target_compile_features(XefgProviderHostFixture PRIVATE cxx_std_20)
    target_compile_definitions(XefgProviderHostFixture PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(XefgProviderHostFixture PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_executable(XefgD3D12ProviderFixture provider_fixture.cpp "${NATIVE}/xess/cuda/cuda_route.cpp")
    target_compile_features(XefgD3D12ProviderFixture PRIVATE cxx_std_20)
    target_compile_definitions(XefgD3D12ProviderFixture PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(XefgD3D12ProviderFixture PRIVATE "${NATIVE}" "${XVK_VULKAN_INCLUDE_DIR}" "${STREAMLINE_ROOT}/external/ngx-sdk/include")
    target_link_libraries(XefgD3D12ProviderFixture PRIVATE d3d12 dxgi bcrypt)
    set_target_properties(XefgD3D12ProviderFixture PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_test(NAME xefg_cuda_d3d12_provider_fixture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/provider_fixture.py"
        --exe "$<TARGET_FILE:XefgD3D12ProviderFixture>" --dll "${XEFG_CUDA_CANDIDATE_DLL}" --host "$<TARGET_FILE:XefgProviderHostFixture>"
        --runtime "${XEFG_PROVIDER_RUNTIME}" --output "${CMAKE_CURRENT_BINARY_DIR}/provider-fixture")
    set_tests_properties(xefg_cuda_d3d12_provider_fixture PROPERTIES TIMEOUT 400 RUN_SERIAL TRUE)
endif()
