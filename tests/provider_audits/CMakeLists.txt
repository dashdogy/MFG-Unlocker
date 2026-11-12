cmake_minimum_required(VERSION 3.24)
project(ProviderAudits LANGUAGES C CXX RC)
# DLSS-G provider admission audits (310.9.0 and 310.9.16, Frame Generation Preset C). Each takes the 310.9.0
# and 310.9.16 providers as arguments; no provider code runs (data mapping, or a mapped image without
# DllMain or imports).
set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../source/native")
add_executable(Ada310916Audit ada.cpp "${NATIVE}/dlssg_provider_policy.cpp")
add_executable(PresetC310916Audit preset.cpp "${NATIVE}/dlssg_provider_policy.cpp"
    "${NATIVE}/third_party/minhook/src/hde/hde64.c")
foreach(target IN ITEMS Ada310916Audit PresetC310916Audit)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY=1)
    target_include_directories(${target} PRIVATE "${NATIVE}")
    target_link_libraries(${target} PRIVATE bcrypt version)
endforeach()

# RTX 30 audit: built only with the release's embedded SM86 cache arguments (MFG_AMPERE_*, AUTO mode).
include("${NATIVE}/ampere_native_cache.cmake")
if(MFG_AMPERE_EMBEDDED_KERNELS)
    add_executable(Ampere310916Audit ampere.cpp "${NATIVE}/dlssg_provider_policy.cpp" "${NATIVE}/ampere_native_cache.cpp"
        "${NATIVE}/ampere_cuda_program.cpp" "${NATIVE}/ampere_mask_transform.cpp" "${NATIVE}/third_party/minhook/src/hde/hde64.c" "${MFG_AMPERE_KERNEL_RESOURCE}"
        ${MFG_EMBEDDED_PAYLOAD_SOURCES})
    target_compile_features(Ampere310916Audit PRIVATE cxx_std_20)
    target_compile_definitions(Ampere310916Audit PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(Ampere310916Audit PRIVATE "${NATIVE}")
    target_link_libraries(Ampere310916Audit PRIVATE bcrypt version)
endif()
