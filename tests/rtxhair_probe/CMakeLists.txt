cmake_minimum_required(VERSION 3.24)
project(RtxHairProbe LANGUAGES CXX)
# rtxhair.1. RtxHairProbe (GPU, phase 0 + 1a): does NVAPI triangle position fetch
# (ALLOW_DATA_ACCESS_EX, shader ops 86/98) work on this GPU, does a DOTS hair segment decode
# back to its LSS endpoints from the hit triangle alone, and does a shader written for
# hardware LSS, rewritten by source/native/rtx_hair, see correct LSS hits on DOTS? See
# main.cpp. Not a CTest: needs an NVIDIA GPU with DXR 1.1; exit 3 means position fetch is
# unavailable here. RtxHairIrTests (CPU): the rewrite on committed DXC fixtures (CTest
# rtxhair-ir), optionally on other IR or DXIL containers. Phase 1b (in RtxHairProbe): NVAPI LSS
# geometry in four layouts built through rtx_hair::DotsBuilder and traced by the rewritten shaders.
set(NVAPI_ROOT "" CACHE PATH "NVAPI SDK R610 or later (nvapi.h, nvHLSLExtns.h, nvHLSLExtnsInternal.h)")
if(NOT EXISTS "${NVAPI_ROOT}/nvapi.h" OR NOT EXISTS "${NVAPI_ROOT}/nvHLSLExtns.h" OR NOT EXISTS "${NVAPI_ROOT}/nvHLSLExtnsInternal.h")
    message(FATAL_ERROR "Set NVAPI_ROOT to an NVAPI SDK with nvapi.h and nvHLSLExtns.h (R610 or later: LSS and triangle position fetch)")
endif()
set(DXC_ROOT "" CACHE PATH "Folder with dxcompiler.dll and dxil.dll that sign DXIL (Windows SDK bin/<version>/x64)")
if(NOT EXISTS "${DXC_ROOT}/dxcompiler.dll" OR NOT EXISTS "${DXC_ROOT}/dxil.dll")
    message(FATAL_ERROR "Set DXC_ROOT to a folder with dxcompiler.dll and dxil.dll")
endif()
set(NATIVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
set(RTX_HAIR_SOURCES "${NATIVE_SOURCE}/rtx_hair/lss_ir.cpp" "${NATIVE_SOURCE}/rtx_hair/lss_dxil.cpp")
# Paths reach the programs through a generated header (the repository path has spaces).
file(TO_CMAKE_PATH "${NVAPI_ROOT}" RTXHAIR_NVAPI_DIR)
file(TO_CMAKE_PATH "${DXC_ROOT}" RTXHAIR_DXC_DIR)
set(RTXHAIR_HLSL "${CMAKE_CURRENT_LIST_DIR}/probe.hlsl")
set(RTXHAIR_LSS_HLSL "${CMAKE_CURRENT_LIST_DIR}/probe_lss.hlsl")
get_filename_component(RTXHAIR_CONVERTER_HLSL "${NATIVE_SOURCE}/rtx_hair/dots_converter.hlsl" ABSOLUTE)
get_filename_component(RTXHAIR_PATCH_HLSL "${NATIVE_SOURCE}/rtx_hair/instance_patch.hlsl" ABSOLUTE)
configure_file(probe_paths.h.in "${CMAKE_CURRENT_BINARY_DIR}/probe_paths.h" @ONLY)

add_executable(RtxHairProbe main.cpp ${RTX_HAIR_SOURCES} "${NATIVE_SOURCE}/rtx_hair/dots_builder.cpp" "${NATIVE_SOURCE}/rtx_hair/instance_patch.cpp")
target_include_directories(RtxHairProbe PRIVATE "${NVAPI_ROOT}" "${NATIVE_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(RtxHairProbe PRIVATE d3d12 dxgi)

add_executable(RtxHairIrTests ir_tests.cpp ${RTX_HAIR_SOURCES} "${NATIVE_SOURCE}/witcher_dots/shader_ir.cpp")
target_include_directories(RtxHairIrTests PRIVATE "${NATIVE_SOURCE}")

foreach(target RtxHairProbe RtxHairIrTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endforeach()

enable_testing()
add_test(NAME rtxhair-ir COMMAND RtxHairIrTests --fixtures "${CMAKE_CURRENT_LIST_DIR}/fixtures")
