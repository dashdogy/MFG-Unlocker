cmake_minimum_required(VERSION 3.24)
project(Dlss5ProviderHarness LANGUAGES C CXX)
# dlss5.1: runs NVIDIA's DLSSNR provider (NGX feature 18) on synthetic frames through the
# DLSS 5 key mapping of source/native/dlss5_policy.h. Needs an NVIDIA GPU, the driver NGX
# runtime and a provider directory holding nvngx_dlssnr.dll (--dir). Not a CTest: the
# provider is not shipped and its architecture check refuses some GPUs (exit code 3).
set(STREAMLINE_ROOT "" CACHE PATH "Streamline SDK 2.14.1 root (external/ngx-sdk)")
if(NOT EXISTS "${STREAMLINE_ROOT}/external/ngx-sdk/include/nvsdk_ngx.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to the Streamline SDK with external/ngx-sdk")
endif()
set(NATIVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../source/native")
# dx11nr.1: --d3d11 runs the same frames through the DirectX 11 bridge (dlss5_d3d11.cpp) on a D3D11 device.
add_executable(Dlss5ProviderHarness main.cpp "${NATIVE_SOURCE}/dlss5_provider.cpp" "${NATIVE_SOURCE}/dlss5.cpp" "${NATIVE_SOURCE}/dlss5_d3d11.cpp"
 "${NATIVE_SOURCE}/third_party/minhook/src/buffer.c" "${NATIVE_SOURCE}/third_party/minhook/src/hook.c"
 "${NATIVE_SOURCE}/third_party/minhook/src/trampoline.c" "${NATIVE_SOURCE}/third_party/minhook/src/hde/hde64.c")
target_compile_features(Dlss5ProviderHarness PRIVATE cxx_std_20)
target_include_directories(Dlss5ProviderHarness PRIVATE "${NATIVE_SOURCE}" "${NATIVE_SOURCE}/third_party/minhook/include" "${STREAMLINE_ROOT}/external/ngx-sdk/include")
target_compile_definitions(Dlss5ProviderHarness PRIVATE MFG_DLSS5_TESTING=1 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(Dlss5ProviderHarness PRIVATE d3d11 d3d12 dxgi d3dcompiler
    "${STREAMLINE_ROOT}/external/ngx-sdk/lib/Windows_x86_64/nvsdk_ngx_d.lib")
set_target_properties(Dlss5ProviderHarness PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
