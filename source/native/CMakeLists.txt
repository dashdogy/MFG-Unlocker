cmake_minimum_required(VERSION 3.24)
project(RTX40MFGNative VERSION 1.7.0.1081 LANGUAGES C CXX ASM_MASM)
option(MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK "Isolated comparison: keep proxies/input but skip UI GPU work" OFF)
option(MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS "Bounded NGX runtime Create results without DRED or new hooks" OFF)
option(MFG_UNLOCK_GPU_FAULT_CAPTURE "Isolated local DRED and NGX result diagnostics" OFF)
option(MFG_UNLOCK_SESSION_DIAGNOSTICS "Beta builds: runtime-log stamps, state lines, crash and exit records, minidump" OFF)
option(MFG_UNLOCK_XEFG_VK_STANDALONE "Harness: XeMFG on Vulkan without OptiScaler (RTXMFG's own swap-chain takeover)" OFF)
option(MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY "Isolated comparison without integrated overlay interception" OFF)
if(MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY AND NOT MFG_UNLOCK_GPU_FAULT_CAPTURE)
    message(FATAL_ERROR "The overlay isolation requires the local GPU fault recorder")
endif()

option(MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN
    "Built-in UI using owned DXGI proxies and bounded application import gateways" ON)
option(MFG_UNLOCK_OVERLAY_MENU_DRAW
    "Enable menu drawing on owned swapchain proxies" ON)
option(MFG_UNLOCK_OVERLAY_TEST_FAULTS
    "Deterministic coordinator and patch fault points for the regression harness" OFF)
if(MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN AND MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY)
    message(FATAL_ERROR "The bounded redesign and the no-overlay diagnostic are exclusive")
endif()
if(MFG_UNLOCK_OVERLAY_MENU_DRAW AND NOT MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN)
    message(FATAL_ERROR "The restored menu requires the bounded installation")
endif()
if(MFG_UNLOCK_OVERLAY_TEST_FAULTS AND NOT MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN)
    message(FATAL_ERROR "Coordinator fault points require the bounded installation")
endif()
if(MFG_UNLOCK_OVERLAY_TEST_FAULTS AND NOT MFG_UNLOCK_GPU_FAULT_CAPTURE)
    message(FATAL_ERROR "Test-fault builds are diagnostic candidates and require the fault recorder")
endif()

set(STREAMLINE_ROOT "" CACHE PATH "Path to the NVIDIA Streamline SDK directory")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to a Streamline SDK directory containing include/sl_dlss_g.h")
endif()

option(MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    "Apply the exact-profile equivalent OutputPull mask simplification before first Ada FG Create" ON)
option(MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY
    "Isolated mask candidate with the six-block occupancy hint; requires separate runtime validation" OFF)
if(MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY AND NOT MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY)
    message(FATAL_ERROR "The OutputPull mask occupancy candidate requires the mask-only path")
endif()
set(IMGUI_ROOT "" CACHE PATH
    "Path to the Dear ImGui source tree used by that ReShade version")

include(ampere_native_cache.cmake)
if(NOT MFG_AMPERE_EMBEDDED_KERNELS)
    message(FATAL_ERROR "The unified distribution requires embedded SM86 kernels")
endif()

set(MFG_UNLOCK_CORE_SOURCES
    patcher.cpp
    optiscaler_host.cpp
    optiscaler_menu_tab.cpp
    midpoint_fix.cpp
    blackwell_gpu.cpp
    gpu_dispatch.cpp
    ampere_gpu.cpp
    ampere_cuda_program.cpp
    ampere_native_cache.cpp
    ampere_mask_transform.cpp
    ampere_backend.cpp
    ampere_flip_metering.cpp
    ampere_wrapper_capacity.cpp
    ampere_provider_arch.cpp
    dlssg_provider_selection.cpp
    dlssg_provider_policy.cpp
    dlssg_preset.cpp
    dlss_sr.cpp
    dlss5.cpp
    dlss5_provider.cpp
    dlss5_vulkan.cpp
    dlss5_d3d11.cpp
    reflex_control.cpp
    latency_probe.cpp
    vsync_control.cpp
    entry_detour.cpp
    entry_detour.asm
    nvidia_mfg_policy.cpp
    temporal_interval_trace.cpp
    fault_capture.cpp
)

option(MFG_UNLOCK_BUILD_SINGLE_MODULE "Build integrated backend and ImGui UI" ON)
option(MFG_UNLOCK_XESS "Integrate the XeSS-FG multi-frame backend (V1.5)" ON)
if(NOT MFG_UNLOCK_XESS)
    message(FATAL_ERROR "MFG_UNLOCK_XESS=OFF is not supported: the single module always integrates the XeSS-FG backend")
endif()
set(XESS_SDK_ROOT "" CACHE PATH "XeSS SDK 3.0.2 directory containing inc/xess_fg and inc/xell")
set(XESS_PROGRAMS_ROOT "" CACHE PATH "Directory containing programs/programs.tsv and the 54 portable DXIL programs")
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Explicit Vulkan SDK headers for the standalone renderer")
if(MFG_UNLOCK_BUILD_SINGLE_MODULE)
    if(NOT EXISTS "${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp" OR
       NOT EXISTS "${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp" OR
       NOT EXISTS "${IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
        message(FATAL_ERROR "The integrated UI requires the pinned ImGui source and render backends")
    endif()
    if(NOT EXISTS "${VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
        message(FATAL_ERROR "Set VULKAN_INCLUDE_DIR to the explicit Vulkan SDK include directory")
    endif()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(GLSLANG_VALIDATOR "" CACHE FILEPATH "glslangValidator.exe from Vulkan SDK 1.2.176.1")
    if(NOT EXISTS "${GLSLANG_VALIDATOR}")
        message(FATAL_ERROR "Set GLSLANG_VALIDATOR to the pinned Vulkan shader compiler")
    endif()
    set(MFG_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/witcher_dots/converter.hlsl" DOTS_CONVERTER_SOURCE)
    configure_file(witcher_dots/converter_source.h.in "${MFG_GENERATED}/converter_source.h" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS witcher_dots/converter.hlsl)
    # rtxhair.1 universal hair: the LSS -> DOTS converter, compiled in the game by its DXC.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/rtx_hair/dots_converter.hlsl" RTXHAIR_CONVERTER_SOURCE)
    configure_file(rtx_hair/converter_source.h.in "${MFG_GENERATED}/rtxhair_converter_source.h" @ONLY)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/rtx_hair/instance_patch.hlsl" RTXHAIR_INSTANCE_PATCH_SOURCE)
    configure_file(rtx_hair/instance_patch_source.h.in "${MFG_GENERATED}/rtxhair_instance_patch_source.h" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS rtx_hair/dots_converter.hlsl rtx_hair/converter_source.h.in
        rtx_hair/instance_patch.hlsl rtx_hair/instance_patch_source.h.in)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_build_inputs.py"
        --output "${MFG_GENERATED}" --imgui "${IMGUI_ROOT}" --glslang "${GLSLANG_VALIDATOR}"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        generate_build_inputs.py proxy_exports.txt generated_sha256.json ui_color.frag
        "${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp" "${IMGUI_ROOT}/backends/imgui_impl_dx11.cpp" "${GLSLANG_VALIDATOR}")
    # Menu translations: every locale/<code>.po is validated and compiled in (locale/README.md).
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_locale.py" generate
        --locale "${CMAKE_CURRENT_SOURCE_DIR}/locale" --output "${MFG_GENERATED}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB MFG_LOCALE_CATALOGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/locale/*.po")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS generate_locale.py ${MFG_LOCALE_CATALOGS})
    # vkdlss5.1: the DLSS 5 Vulkan stage's passes, dlss5.cpp's own HLSL compiled to SPIR-V by the Vulkan SDK
    # 1.4.363.0 dxc (the Windows SDK dxc has no SPIR-V code generation); the generator pins its version and outputs.
    set(DLSS5_VK_DXC "" CACHE FILEPATH "Vulkan SDK 1.4.363.0 Bin/dxc.exe for the DLSS 5 Vulkan passes")
    if(NOT EXISTS "${DLSS5_VK_DXC}")
        message(FATAL_ERROR "Set DLSS5_VK_DXC to the pinned Vulkan SDK 1.4.363.0 dxc.exe")
    endif()
    get_filename_component(DLSS5_VK_BIN "${DLSS5_VK_DXC}" DIRECTORY)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_dlss5_vk_shaders.py"
        --dxc "${DLSS5_VK_DXC}" --spirv-val "${DLSS5_VK_BIN}/spirv-val.exe"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${MFG_GENERATED}"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS generate_dlss5_vk_shaders.py dlss5.cpp)
    include_directories("${MFG_GENERATED}")
    # slim.1 (V1.7.1): every generated payload list ships as one LZMA-compressed resource (embedded_payload.cmake);
    # the ids are unique in the module: 3900 ampere_kernels (ampere_native_cache.cmake), 3901-3905 below.
    include(embedded_payload.cmake)
    mfg_pack_payloads("${MFG_GENERATED}/dlss5_vk_spirv.rc" dlss5_vk 5 3905 "${MFG_GENERATED}" MFG_DLSS5_VK_RESOURCE)
    set(MFG_XESS_SOURCES)
    set(MFG_XESS_RESOURCES)
    if(MFG_UNLOCK_XESS)
        if(NOT EXISTS "${XESS_SDK_ROOT}/inc/xess_fg/xefg_swapchain_d3d12.h" OR
           NOT EXISTS "${XESS_SDK_ROOT}/inc/xell/xell_d3d12.h")
            message(FATAL_ERROR "Set XESS_SDK_ROOT to the pinned XeSS SDK 3.0.2 headers")
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xess/generate_xess_programs.py"
            --programs "${XESS_PROGRAMS_ROOT}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        file(GLOB XESS_PROGRAM_INPUTS CONFIGURE_DEPENDS "${XESS_PROGRAMS_ROOT}/programs/*")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            xess/generate_xess_programs.py ${XESS_PROGRAM_INPUTS})
        # splat.1: RTXMFG's own XeFG shaders, compiled by the generator's pinned Windows SDK dxc
        # (the SDK selected by -A x64,version=...); the generator pins the dxc version and outputs.
        get_filename_component(MFG_WINDOWS_KITS
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
        set(MFG_DXC "${MFG_WINDOWS_KITS}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64/dxc.exe")
        if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION OR NOT EXISTS "${MFG_DXC}")
            message(FATAL_ERROR "XeFG shaders need dxc.exe from the selected Windows SDK: ${MFG_DXC}")
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xess/generate_xess_shaders.py"
            --dxc "${MFG_DXC}" --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            xess/generate_xess_shaders.py xess/port/splat_motion_aware.hlsl xess/port/unet_conv.hlsl)
        # V1.5.1 tensorcore.1: the ported model's int8 layers as CUDA kernels (xess/cuda), compiled by the pinned
        # CUDA Toolkit 13.2 nvcc in the build's MSVC toolset environment; the generator pins compiler and output.
        set(MFG_CUDA_ROOT "" CACHE PATH "CUDA Toolkit 13.2 directory (bin/nvcc.exe) for the XeFG tensor-core kernels")
        if(NOT EXISTS "${MFG_CUDA_ROOT}/bin/nvcc.exe")
            message(FATAL_ERROR "Set MFG_CUDA_ROOT to the pinned CUDA Toolkit 13.2")
        endif()
        string(REGEX MATCH "VCToolsVersion=([0-9.]+)" MFG_VC_TOOLS_MATCH "${CMAKE_VS_GLOBALS}")
        if(NOT CMAKE_MATCH_1 OR NOT CMAKE_GENERATOR_INSTANCE)
            message(FATAL_ERROR "The XeFG tensor-core kernels need CMAKE_GENERATOR_INSTANCE and VCToolsVersion (build.ps1 sets both)")
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xess/cuda/generate_cuda_layers.py"
            --nvcc "${MFG_CUDA_ROOT}/bin/nvcc.exe" --vcvars "${CMAKE_GENERATOR_INSTANCE}/VC/Auxiliary/Build/vcvars64.bat"
            --vcvars-ver "${CMAKE_MATCH_1}" --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            xess/cuda/generate_cuda_layers.py xess/cuda/xefg_cuda_layers.cu xess/cuda/xefg_cuda_params.h)
        # beta.8: XeSS MFG under OptiScaler (no Reflex bridge or Intel native control); xemfgsa.1 (V1.7.0): XeMFG
        # standalone, RTXMFG as the D3D12 XeFG host (xess/d3d12_host.cpp).
        set(MFG_XESS_SOURCES
            xess/xess_mod.cpp xess/universal_hooks.cpp xess/internal_api_guards.asm
            xess/sdk_pacing.cpp xess/game_profile.cpp xess/runtime_payload.cpp xess/xess_runtime.cpp
            xess/host_unlock.cpp xess/intel_native.cpp xess/motion_fill.cpp xess/hudless_match.cpp xess/cuda/cuda_route.cpp xess/cuda/provider_list.cpp
            xess/d3d12_host.cpp xess/host_xell_binding.cpp)
        mfg_pack_payloads("${MFG_GENERATED}/embedded_xess_programs.rc" xess_programs 1 3901 "${MFG_GENERATED}" packed_programs)
        mfg_pack_payloads("${MFG_GENERATED}/embedded_xess_shaders.rc" xess_shaders 2 3902 "${MFG_GENERATED}" packed_shaders)
        mfg_pack_payloads("${MFG_GENERATED}/embedded_xefg_cuda.rc" xefg_cuda 3 3903 "${MFG_GENERATED}" packed_cuda)
        set(MFG_XESS_RESOURCES "${packed_programs}" "${packed_shaders}" "${packed_cuda}")
        # vkxefg.1 (V1.6.2): XeMFG on Vulkan games. The xvk D3D12-on-Vulkan layer, its embedded SPIR-V program
        # set and the Vulkan host build as their own objects against the Vulkan 1.4 headers they need; the rest of
        # the module keeps its pinned Vulkan 1.2 headers.
        set(XVK_PROGRAMS_ROOT "" CACHE PATH "Directory with SHA256SUMS.txt and the xvk <dxil-sha256>.spv/.json program pairs")
        set(XVK_VULKAN_INCLUDE_DIR "" CACHE PATH "Vulkan SDK 1.4.363.0 include directory for the xvk layer")
        if(NOT EXISTS "${XVK_PROGRAMS_ROOT}/SHA256SUMS.txt")
            message(FATAL_ERROR "Set XVK_PROGRAMS_ROOT to the pinned xvk SPIR-V program set")
        endif()
        if(NOT EXISTS "${XVK_VULKAN_INCLUDE_DIR}/vulkan/vulkan_core.h")
            message(FATAL_ERROR "Set XVK_VULKAN_INCLUDE_DIR to the pinned Vulkan SDK 1.4.363.0 headers")
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xess/vulkan/generate_xvk_programs.py"
            --programs "${XVK_PROGRAMS_ROOT}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xess/vulkan/generate_vk_input_shaders.py"
            --glslang "${GLSLANG_VALIDATOR}" --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        # vkhair.4: universal hair's Vulkan converter and instance patch (rtx_hair/vk_dots_builder), pinned like the input copy.
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rtx_hair/generate_vk_hair_shaders.py"
            --glslang "${GLSLANG_VALIDATOR}" --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${MFG_GENERATED}"
            COMMAND_ERROR_IS_FATAL ANY)
        # rtx_hair/spirv_module.cpp's operand table, written from the pinned SDK's SPIR-V grammar.
        set(MFG_SPIRV_GRAMMAR "${XVK_VULKAN_INCLUDE_DIR}/spirv/unified1/spirv.core.grammar.json")
        file(SHA256 "${MFG_SPIRV_GRAMMAR}" spirvGrammarSha)
        if(NOT spirvGrammarSha STREQUAL "b3a3be9ce86e016bb0c0328ec4f2d5876bd7af5f0386adfaa544eec1c89fe080")
            message(FATAL_ERROR "${MFG_SPIRV_GRAMMAR} is not Vulkan SDK 1.4.363.0's SPIR-V grammar")
        endif()
        file(MAKE_DIRECTORY "${MFG_GENERATED}/rtx_hair")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rtx_hair/spirv_grammar.py"
            "${MFG_SPIRV_GRAMMAR}" "${MFG_GENERATED}/rtx_hair/spirv_grammar.inl"
            COMMAND_ERROR_IS_FATAL ANY)
        file(GLOB XVK_PROGRAM_INPUTS CONFIGURE_DEPENDS "${XVK_PROGRAMS_ROOT}/*")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            xess/vulkan/generate_xvk_programs.py xess/vulkan/generate_vk_input_shaders.py xess/vulkan/vk_input_copy.comp
            rtx_hair/generate_vk_hair_shaders.py rtx_hair/vk_common.glsl rtx_hair/vk_dots_converter.comp rtx_hair/vk_instance_patch.comp
            rtx_hair/spirv_grammar.py "${MFG_SPIRV_GRAMMAR}" ${XVK_PROGRAM_INPUTS})
        set(MFG_XVK_SOURCES
            xess/vulkan/xvk_device.cpp xess/vulkan/xvk_resource.cpp xess/vulkan/xvk_queue.cpp xess/vulkan/xvk_commandlist.cpp
            xess/vulkan/xvk_tensor.cpp xess/vulkan/xvk_pipeline.cpp xess/vulkan/xvk_programs.cpp xess/vulkan/xvk_dxgi.cpp xess/vulkan/xvk_util.cpp
            xess/vulkan/vk_host.cpp xess/vulkan/vk_xefg_api.cpp xess/vulkan/vk_provider.cpp xess/vulkan/xefg_internal.cpp
            # ngxprov.1: the provider API (both APIs) and the D3D12 provider, built with the Vulkan provider they share
            # XeFG's internal API and runtime with.
            xess/ngx_provider.cpp xess/d3d12_provider.cpp
            # pluginprov.1: the shim's entry points inside the module, for OptiScaler's RegisterProvider.
            xess/vulkan/xemfg_provider_inprocess.cpp
            rtx_hair/vk_hair.cpp rtx_hair/vk_dots_builder.cpp rtx_hair/lss_spirv.cpp rtx_hair/spirv_module.cpp)
        mfg_pack_payloads("${MFG_GENERATED}/embedded_xvk_programs.rc" xvk_programs 4 3904 "${MFG_GENERATED}" packed_xvk)
        list(APPEND MFG_XESS_RESOURCES "${packed_xvk}")
    endif()

    add_library(MfgSingleMinHook STATIC
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde64.c)
    target_include_directories(MfgSingleMinHook PUBLIC third_party/minhook/include)
    target_compile_definitions(MfgSingleMinHook PRIVATE WIN32_LEAN_AND_MEAN)
    if(MFG_UNLOCK_OVERLAY_TEST_FAULTS)
        target_compile_definitions(MfgSingleMinHook PRIVATE MFG_UNLOCK_TEST_FAULTS=1)
    endif()
    set_target_properties(MfgSingleMinHook PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_library(MfgSingleImGui STATIC
        "${IMGUI_ROOT}/imgui.cpp"
        "${IMGUI_ROOT}/imgui_draw.cpp"
        "${IMGUI_ROOT}/imgui_widgets.cpp"
        "${IMGUI_ROOT}/imgui_tables.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
        "${MFG_GENERATED}/imgui_impl_dx12.cpp"
        "${MFG_GENERATED}/imgui_impl_dx11.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
    target_compile_features(MfgSingleImGui PRIVATE cxx_std_20)
    target_compile_options(MfgSingleImGui PRIVATE /Zi)
    target_include_directories(MfgSingleImGui PUBLIC "${IMGUI_ROOT}" "${VULKAN_INCLUDE_DIR}")
    target_compile_definitions(MfgSingleImGui PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN ImTextureID=ImU64
        VK_USE_PLATFORM_WIN32_KHR=1 VK_NO_PROTOTYPES=1 IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1 IMGUI_DISABLE_DEBUG_TOOLS)
    set_target_properties(MfgSingleImGui PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # slim.1: ImGui runs only while the menu draws, so it is built for size; IMGUI_DISABLE_DEBUG_TOOLS drops the
    # metrics, debug-log and ID-stack windows the menu never opens (MfgSingleObjects sets it too).
    target_compile_options(MfgSingleImGui PRIVATE $<$<CONFIG:Release>:/O1>)

    set(MFG_SINGLE_CORE_SOURCES ${MFG_UNLOCK_CORE_SOURCES})
    add_library(MfgSingleObjects OBJECT ${MFG_SINGLE_CORE_SOURCES}
        single_module.cpp session_diagnostics.cpp single_overlay.cpp overlay_application_imports.cpp overlay_platform.cpp caller_scoped_import.cpp
        overlay_dx12.cpp overlay_dx11.cpp overlay_dxgi_proxy.cpp overlay_back_buffers.cpp overlay_adapter_parent.cpp gpu_name_override.cpp overlay_vulkan.cpp overlay_install.cpp reshade_frontend.cpp
        localization.cpp
        witcher_dots/geometry.cpp witcher_dots/shader_ir.cpp witcher_dots/shader_cache.cpp
        witcher_dots/game_profile.cpp witcher_dots/game_discovery.cpp
        witcher_dots/gpu_runtime.cpp witcher_dots/witcher_dots.cpp
        rtx_hair/lss_ir.cpp rtx_hair/lss_dxil.cpp rtx_hair/dots_builder.cpp rtx_hair/instance_patch.cpp rtx_hair/universal_hair.cpp
        ${MFG_XESS_SOURCES} ${MFG_EMBEDDED_PAYLOAD_SOURCES}
        "${MFG_GENERATED}/proxy_generated/renamable_exports.asm")
    target_compile_features(MfgSingleObjects PRIVATE cxx_std_20)
    target_compile_options(MfgSingleObjects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    target_compile_options(MfgSingleObjects PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/Zi>)
    target_compile_definitions(MfgSingleObjects PRIVATE
        MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK=$<BOOL:${MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK}>
        MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS=$<BOOL:${MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS}>
        MFG_UNLOCK_GPU_FAULT_CAPTURE=$<BOOL:${MFG_UNLOCK_GPU_FAULT_CAPTURE}>
        MFG_UNLOCK_SESSION_DIAGNOSTICS=$<BOOL:${MFG_UNLOCK_SESSION_DIAGNOSTICS}>
        MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY=$<BOOL:${MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY}>
        MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN=$<BOOL:${MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN}>
        MFG_UNLOCK_OVERLAY_MENU_DRAW=$<BOOL:${MFG_UNLOCK_OVERLAY_MENU_DRAW}>
        MFG_UNLOCK_OVERLAY_TEST_FAULTS=$<BOOL:${MFG_UNLOCK_OVERLAY_TEST_FAULTS}>
        MFG_UNLOCK_XESS=$<BOOL:${MFG_UNLOCK_XESS}>
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN ImTextureID=ImU64
        VK_USE_PLATFORM_WIN32_KHR=1 VK_NO_PROTOTYPES=1 IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1 IMGUI_DISABLE_DEBUG_TOOLS
        MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY}>
        MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY}>)
    target_include_directories(MfgSingleObjects PRIVATE
        "${MFG_GENERATED}"
        "${STREAMLINE_ROOT}/include" "${STREAMLINE_ROOT}/external/ngx-sdk/include"
        "${IMGUI_ROOT}" "${VULKAN_INCLUDE_DIR}" third_party/minhook/include
        "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MFG_UNLOCK_XESS)
        target_include_directories(MfgSingleObjects PRIVATE
            "${XESS_SDK_ROOT}/inc/xess_fg" "${XESS_SDK_ROOT}/inc/xell")
    endif()
    set_target_properties(MfgSingleObjects PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # slim.1: code that runs at load, in the menu or in diagnostics is built for size (/O1); the present, pacing,
    # NGX/Streamline hook (patcher.cpp) and XeFG host paths keep /O2.
    set_source_files_properties(
        single_module.cpp session_diagnostics.cpp overlay_install.cpp overlay_application_imports.cpp
        caller_scoped_import.cpp gpu_name_override.cpp overlay_adapter_parent.cpp reshade_frontend.cpp localization.cpp
        dlssg_provider_selection.cpp dlssg_provider_policy.cpp nvidia_mfg_policy.cpp
        witcher_dots/game_discovery.cpp witcher_dots/game_profile.cpp witcher_dots/shader_ir.cpp witcher_dots/shader_cache.cpp
        xess/universal_hooks.cpp xess/game_profile.cpp xess/host_unlock.cpp xess/runtime_payload.cpp
        PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Release>:/O1>)

    set(MFG_XVK_OBJECTS)
    if(MFG_UNLOCK_XESS)
        add_library(MfgXvkObjects OBJECT ${MFG_XVK_SOURCES})
        target_compile_features(MfgXvkObjects PRIVATE cxx_std_20)
        target_compile_options(MfgXvkObjects PRIVATE /utf-8 /Zi)
        target_compile_definitions(MfgXvkObjects PRIVATE
            MFG_UNLOCK_XESS=1 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN VK_USE_PLATFORM_WIN32_KHR=1 VK_NO_PROTOTYPES=1
            MFG_UNLOCK_XEFG_VK_STANDALONE=$<BOOL:${MFG_UNLOCK_XEFG_VK_STANDALONE}>)
        # The 1.4 headers come first: these objects never see the module's pinned Vulkan 1.2 headers.
        target_include_directories(MfgXvkObjects PRIVATE
            "${XVK_VULKAN_INCLUDE_DIR}" "${MFG_GENERATED}" "${CMAKE_CURRENT_SOURCE_DIR}"
            "${STREAMLINE_ROOT}/include" "${STREAMLINE_ROOT}/external/ngx-sdk/include"
            "${XESS_SDK_ROOT}/inc/xess_fg" "${XESS_SDK_ROOT}/inc/xell")
        set_target_properties(MfgXvkObjects PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        set(MFG_XVK_OBJECTS $<TARGET_OBJECTS:MfgXvkObjects>)
    endif()

    add_library(RTXMFGUnified SHARED $<TARGET_OBJECTS:MfgSingleObjects> ${MFG_XVK_OBJECTS}
        single_version.rc "${MFG_GENERATED}/proxy_generated/renamable.def" ${MFG_AMPERE_KERNEL_RESOURCE}
        ${MFG_DLSS5_VK_RESOURCE} ${MFG_XESS_RESOURCES})
    target_compile_definitions(RTXMFGUnified PRIVATE MFG_SINGLE_FILENAME="MFGUnlocker.dll")
    target_link_libraries(RTXMFGUnified PRIVATE MfgSingleImGui MfgSingleMinHook
        bcrypt version wintrust crypt32 advapi32 delayimp d3dcompiler d3d12 dxgi user32 gdi32 imm32 dwmapi psapi)
    target_link_options(RTXMFGUnified PRIVATE /DELAYLOAD:version.dll /DELAYLOAD:d3d12.dll /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dxgi.dll
        /DELAYLOAD:shell32.dll /DELAYLOAD:imm32.dll /DELAYLOAD:bcrypt.dll /DELAYLOAD:wintrust.dll /DELAYLOAD:crypt32.dll
        /DEBUG:FULL /OPT:REF /OPT:ICF /IGNORE:4222)
    target_include_directories(RTXMFGUnified PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(RTXMFGUnified PROPERTIES PREFIX "" OUTPUT_NAME "MFGUnlocker"
        LINKER_LANGUAGE CXX MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_custom_target(RTX40MFGSingle ALL DEPENDS RTXMFGUnified)
endif()
