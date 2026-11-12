cmake_minimum_required(VERSION 3.24)
project(RtxHairVkProbe LANGUAGES CXX)
# vkhair.1: universal RTX hair on Vulkan.
# RtxHairVkProbe (GPU): phase 0, does VK_KHR_ray_tracing_position_fetch work on this GPU through Vulkan, and
# does a DOTS hair segment decode back to its LSS endpoints from the hit triangle alone (the Vulkan twin of
# tests/rtxhair_probe phase 0); phase 1a, do hair shaders written for hardware LSS, rewritten by
# source/native/rtx_hair/lss_spirv, see correct LSS hits on DOTS. Not a CTest: needs a GPU with Vulkan ray
# queries and ray tracing pipelines; exit 3 means position fetch is unavailable here.
# RtxHairSpirvTests (CPU, CTest rtxhair-spirv): the SPIR-V rewrite on GLSL fixtures, every output checked by
# the SDK's SPIRV-Tools library (a test-only input).
# Shaders are GLSL compiled here by the pinned Vulkan SDK's glslangValidator and checked by its spirv-val; the
# rewriter itself needs no compiler (no DXC anywhere).
set(VULKAN_SDK_ROOT "" CACHE PATH "Vulkan SDK 1.4.363.0 root (Include, Lib/vulkan-1.lib, Bin/glslangValidator.exe, Bin/spirv-val.exe, Bin/SPIRV-Tools-shared.dll)")
foreach(required Include/vulkan/vulkan.h Include/spirv-tools/libspirv.h Lib/vulkan-1.lib Bin/glslangValidator.exe Bin/spirv-val.exe Bin/SPIRV-Tools-shared.dll)
    if(NOT EXISTS "${VULKAN_SDK_ROOT}/${required}")
        message(FATAL_ERROR "Set VULKAN_SDK_ROOT to the Vulkan SDK 1.4.363.0 root (missing ${required})")
    endif()
endforeach()
file(STRINGS "${VULKAN_SDK_ROOT}/Include/vulkan/vulkan_core.h" lssHeader REGEX "define VK_NV_ray_tracing_linear_swept_spheres 1")
file(STRINGS "${VULKAN_SDK_ROOT}/Include/vulkan/vulkan_core.h" fetchHeader REGEX "define VK_KHR_ray_tracing_position_fetch 1")
if(NOT lssHeader OR NOT fetchHeader)
    message(FATAL_ERROR "VULKAN_SDK_ROOT's headers lack VK_KHR_ray_tracing_position_fetch or VK_NV_ray_tracing_linear_swept_spheres (use SDK 1.4.363.0)")
endif()
set(NATIVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
set(GLSLANG "${VULKAN_SDK_ROOT}/Bin/glslangValidator.exe")
set(SPIRV_VAL "${VULKAN_SDK_ROOT}/Bin/spirv-val.exe")
set(RTX_HAIR_SPIRV_SOURCES "${NATIVE_SOURCE}/rtx_hair/lss_spirv.cpp" "${NATIVE_SOURCE}/rtx_hair/spirv_module.cpp")

# vkhair.3: the DOTS builder's converter and instance patch, compiled exactly as the DLL's build does
# (rtx_hair/generate_vk_hair_shaders.py with the module's pinned glslangValidator, SDK 1.2.176.1, and pinned SPIR-V).
get_filename_component(SDK_PARENT "${VULKAN_SDK_ROOT}" DIRECTORY)
set(GLSLANG_VALIDATOR "${SDK_PARENT}/1.2.176.1/Bin/glslangValidator.exe" CACHE FILEPATH "glslangValidator.exe from Vulkan SDK 1.2.176.1 (the DLL's pinned shader compiler)")
if(NOT EXISTS "${GLSLANG_VALIDATOR}")
    message(FATAL_ERROR "Set GLSLANG_VALIDATOR to glslangValidator.exe from Vulkan SDK 1.2.176.1")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
# The SPIR-V walker's operand table, written from the SDK's grammar as the DLL's build does.
file(MAKE_DIRECTORY "${GENERATED_DIR}/rtx_hair")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${NATIVE_SOURCE}/rtx_hair/spirv_grammar.py"
    "${VULKAN_SDK_ROOT}/Include/spirv/unified1/spirv.core.grammar.json" "${GENERATED_DIR}/rtx_hair/spirv_grammar.inl"
    COMMAND_ERROR_IS_FATAL ANY)
set(VK_HAIR_SHADERS "${GENERATED_DIR}/rtxhair_vk_shaders.h")
add_custom_command(OUTPUT "${VK_HAIR_SHADERS}"
    COMMAND "${Python3_EXECUTABLE}" "${NATIVE_SOURCE}/rtx_hair/generate_vk_hair_shaders.py" --glslang "${GLSLANG_VALIDATOR}"
            --source "${NATIVE_SOURCE}" --output "${GENERATED_DIR}"
    DEPENDS "${NATIVE_SOURCE}/rtx_hair/generate_vk_hair_shaders.py" "${NATIVE_SOURCE}/rtx_hair/vk_common.glsl"
            "${NATIVE_SOURCE}/rtx_hair/vk_dots_converter.comp" "${NATIVE_SOURCE}/rtx_hair/vk_instance_patch.comp"
    COMMENT "generate_vk_hair_shaders.py: the DOTS builder's shaders"
    VERBATIM)
set(RTX_HAIR_BUILDER_SOURCES "${NATIVE_SOURCE}/rtx_hair/vk_dots_builder.cpp" "${VK_HAIR_SHADERS}")

# One shader: GLSL -> <output dir>/<file>.spv (validated); with `variable`, also <base>_<stage>.h holding the
# same SPIR-V as a C array of that name.
set(SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirv")
set(FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${SPIRV_DIR}" "${FIXTURE_DIR}")
set(SHADER_OUTPUTS "")
function(rtxhair_shader source outputDir variable)
    get_filename_component(file "${source}" NAME)
    set(binary "${outputDir}/${file}.spv")
    set(outputs "${binary}")
    set(commands COMMAND "${GLSLANG}" --target-env vulkan1.3 -I${CMAKE_CURRENT_LIST_DIR} -o "${binary}" "${source}"
                 COMMAND "${SPIRV_VAL}" --target-env vulkan1.3 "${binary}")
    if(variable)
        string(REPLACE "." "_" header "${file}")
        set(header "${outputDir}/${header}.h")
        list(APPEND outputs "${header}")
        list(APPEND commands COMMAND "${GLSLANG}" --target-env vulkan1.3 -I${CMAKE_CURRENT_LIST_DIR} --vn ${variable} -o "${header}" "${source}")
    endif()
    add_custom_command(OUTPUT ${outputs} ${commands}
        DEPENDS "${source}" "${CMAKE_CURRENT_LIST_DIR}/probe_common.glsl" "${CMAKE_CURRENT_LIST_DIR}/probe_lss_common.glsl"
        COMMENT "glslangValidator + spirv-val: ${file}"
        VERBATIM)
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} ${outputs} PARENT_SCOPE)
endfunction()

foreach(pair probe.comp=kProbeComp probe.rgen=kProbeRgen probe.rmiss=kProbeRmiss probe.rchit=kProbeRchit
        probe_lss.comp=kProbeLssComp probe_lss.rgen=kProbeLssRgen probe_lss.rmiss=kProbeLssRmiss probe_lss.rchit=kProbeLssRchit)
    string(REPLACE "=" ";" parts "${pair}")
    list(GET parts 0 file)
    list(GET parts 1 variable)
    rtxhair_shader("${CMAKE_CURRENT_LIST_DIR}/${file}" "${SPIRV_DIR}" ${variable})
endforeach()
set(PROBE_SHADERS ${SHADER_OUTPUTS})
set(SHADER_OUTPUTS "")
foreach(fixture lss_closest.rchit lss_anyhit.rahit lss_query.comp lss_candidate.comp lss_query.frag sphere.rchit ser_lss.rgen plain.rchit)
    rtxhair_shader("${CMAKE_CURRENT_LIST_DIR}/fixtures/${fixture}" "${FIXTURE_DIR}" "")
endforeach()
set(FIXTURE_SHADERS ${SHADER_OUTPUTS})

add_executable(RtxHairVkProbe main.cpp ${RTX_HAIR_SPIRV_SOURCES} ${RTX_HAIR_BUILDER_SOURCES} ${PROBE_SHADERS})
target_include_directories(RtxHairVkProbe PRIVATE "${VULKAN_SDK_ROOT}/Include" "${NATIVE_SOURCE}" "${SPIRV_DIR}" "${GENERATED_DIR}")
# winmm: with --game, MFGUnlocker.dll beside the probe as winmm.dll does the conversion (as Dlss5VkHarness loads it).
target_link_libraries(RtxHairVkProbe PRIVATE "${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib" winmm)

add_executable(RtxHairSpirvTests spirv_tests.cpp ${RTX_HAIR_SPIRV_SOURCES} ${PROBE_SHADERS} ${FIXTURE_SHADERS})
target_include_directories(RtxHairSpirvTests PRIVATE "${VULKAN_SDK_ROOT}/Include" "${NATIVE_SOURCE}" "${GENERATED_DIR}")

# hairmem.1 (CPU, CTest rtxhair-vkbuilder): the DOTS builder's pools, spares, counters and fail-closed path through
# fake device functions and an injected clock; links the generated shaders (Ribbons=2 picks the second converter).
add_executable(RtxHairVkBuilderTests builder_tests.cpp ${RTX_HAIR_BUILDER_SOURCES})
target_include_directories(RtxHairVkBuilderTests PRIVATE "${VULKAN_SDK_ROOT}/Include" "${NATIVE_SOURCE}" "${GENERATED_DIR}")
# One generator run: the probe's project owns the custom command, so a parallel build cannot run it twice at once.
add_dependencies(RtxHairVkBuilderTests RtxHairVkProbe)

foreach(target RtxHairVkProbe RtxHairSpirvTests RtxHairVkBuilderTests)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endforeach()

enable_testing()
add_test(NAME rtxhair-spirv COMMAND RtxHairSpirvTests --fixtures "${FIXTURE_DIR}" --probe "${SPIRV_DIR}"
    --spirv-tools "${VULKAN_SDK_ROOT}/Bin/SPIRV-Tools-shared.dll")
add_test(NAME rtxhair-vkbuilder COMMAND RtxHairVkBuilderTests)
