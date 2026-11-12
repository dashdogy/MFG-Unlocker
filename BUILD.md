# Building v1.7.1

The v1.7.1 release combines the DLSS MFG backend, the built-in menu with its
translations, RTX 50, RTX 30 and experimental RTX 20 support, XeSS Multi Frame
Generation (XeMFG) standalone, through OptiScaler or as OptiScaler's DLSS Frame
Generation provider, including the XeMFG tensor-core kernels, the DLSS 5 stage
and the RTX Hair conversion, in one DLL. ProductVersion is 1.7.1; FileVersion
is 1.7.1.177.

The release target is `RTXMFGUnified`, producing `Release/MFGUnlocker.dll`.
Use Windows x64, Visual Studio 2022/MSVC 14.38.33130, Windows SDK
10.0.22621.0 (including its `dxc.exe`), CMake 4.3.3, and Python 3.10 or newer.

Required external inputs:

- Streamline SDK **2.14.1**.
- Dear ImGui revision `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`
  from ReShade 6.8.0, including its backends.
- Vulkan SDK 1.2.176.1 headers and `glslangValidator.exe`.
- Vulkan SDK 1.4.363.0 headers (the XeMFG Vulkan layer; the RTX Hair SPIR-V operand
  table is generated from its `spirv/unified1/spirv.core.grammar.json`, pinned by
  SHA-256) and its `Bin/dxc.exe` with
  `dxcompiler.dll` and `spirv-val.exe`, which compile `dlss5.cpp`'s DLSS 5 passes
  to SPIR-V for the Vulkan stage (`build.ps1 -Dlss5VulkanDxc`; all three pinned
  by SHA-256, and the generator pins every output).
- XeSS SDK **3.0.2** public headers (`inc/xess_fg`, `inc/xell`).
- CUDA Toolkit **13.2** (`nvcc` V13.2.51), for the XeMFG tensor-core kernels
  (`source/native/xess/cuda/xefg_cuda_layers.cu`). `build.ps1` defaults to
  `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2`; pass `-CudaRoot`
  for another location. No CUDA runtime is linked or shipped: the kernels are
  embedded as one fatbin and run through the NVIDIA driver.
- The pinned XeSS Frame Generation program set: `programs/programs.tsv`
  (SHA-256 `D74BD783D29280ED372653701CE9B0395362E381FAACE1213206E0BDD9BFC9E5`)
  and the 54 DXIL programs it lists. Most are derived from Intel's XeSS SDK;
  see [NOTICE.md](NOTICE.md). They are a separately prepared build input and are
  not included in this source tree.
- The validated SM86 kernel cache, with this directory layout:

```text
native-cache/ampere_native_manifest.inc
native-cache/manifest.json
native-cache/RTX30MFG-Kernels/*.fatbin
native-cache-3109/ampere_native_manifest.inc
native-cache-3109/manifest.json
native-cache-3109/RTX30MFG-Kernels/*.fatbin
all-provider-layouts/native-cache-complete/ampere_native_manifest_additional.inc
all-provider-layouts/native-cache-complete/additional-resources.json
all-provider-layouts/native-cache-complete/RTX30MFG-Kernels/*.fatbin
```

The complete-program cache includes the DLSS Frame Generation 310.9.16 program
(additional program 10, resources 5200-5225).

Kernel caches and the XeSS program set are separately prepared build inputs. A
source checkout alone cannot reproduce the DLL without them. The build script
pins the tools, SDK headers, program set and manifests by SHA-256; CMake also
verifies every embedded kernel's and program's size and hash. No provider DLL is
modified on disk.

With those inputs available, run from the repository root, supplying their
actual paths and a fresh build directory:

```powershell
./source/native/build.ps1 `
    -StreamlineRoot 'C:/SDKs/streamline-sdk-v2.14.1' `
    -ImGuiRoot 'C:/SDKs/reshade-6.8.0/deps/imgui' `
    -NativeCacheRoot 'C:/BuildInputs/rtxmfg-sm86' `
    -XessSdkRoot 'C:/SDKs/xess-sdk-3.0.2' `
    -XessProgramsRoot 'C:/BuildInputs/xess-programs' `
    -BuildDirectory "$PWD/build/v1.7.1" `
    -EnableNgxCreateResultDiagnostics `
    -EnableSessionDiagnostics
```

This reproduces the release configuration: the built-in menu and bounded hook
installation are enabled, with the existing bounded NGX Create result log and
the session diagnostics below.
GPU fault capture, menu suppression, skipped GPU work and test fault injection
remain disabled. Do not enable those diagnostic switches for a release build.

Release, alpha and beta builds add `-EnableSessionDiagnostics`
(`MFG_UNLOCK_SESSION_DIAGNOSTICS`).
The runtime log then stamps every line with local time and opens with the
Windows build, CPU, memory, display adapters and driver versions. It logs
hook-state lines only when they change, and adds a `State` line when the
frame-generation state changes. It records first-chance faults (observed, never
handled) and unhandled exceptions with registers and an unwound stack, and
writes a minidump beside the log (`*.crash.dmp`). A `Session end` record at
process exit names the exiting thread's stack and the last state. A log that
stops without `Session end` was ended abruptly: fail-fast, TerminateProcess or
Task Manager, or a hang. If the runtime log is still held open by another
process, the session writes `<log>.<pid>.log` and says why. The switch is off by
default.

The proxy dispatchers, Vulkan shader arrays, adapted ImGui DX12 and DX11 backends, the
embedded XeSS program table, MFG Unlocker's own XeFG shaders
(`source/native/xess/port/splat_motion_aware.hlsl` and `unet_conv.hlsl`,
compiled with the Windows SDK's `dxc.exe`) and the tensor-core kernel fatbin (sm_75, sm_86, sm_89 and sm_120 SASS,
compiled by `xess/cuda/generate_cuda_layers.py` with the pinned `nvcc` in the
pinned MSVC toolset's environment) are generated in the build directory. The proxy and shader
outputs' normalized SHA-256 values must match
`source/native/generated_sha256.json`; the XeSS generators pin their own inputs
and outputs. The fixed export map is recorded in `proxy_exports.txt`; generation
does not scan DLLs on the build machine. Third-party notices, including the
XeSS program-data notice, remain embedded in the release DLL.

The Witcher 3 hair converter header is generated from
`source/native/witcher_dots/converter.hlsl` and `converter_source.h.in`.
Generated headers and runtime shader caches are not source inputs to publish.
The hair code discovery in `source/native/witcher_dots/game_discovery.cpp`
decodes instructions with the hde64 decoder bundled with MinHook in
`source/native/third_party/minhook`; it needs no additional build input.

Test projects:

- `tests/release_regression`: menu, loader, control, NVIDIA policy and provider
  gate harnesses. See its [README](tests/release_regression/README.md). They
  must load the freshly built DLL. The Vulkan cases use a real Vulkan device;
  DirectX 12 presentation cases use WARP. They exercise synthetic presentation
  and control paths, not a game's Frame Generation output.
- `tests/provider_audits`: DLSS Frame Generation provider admission audits
  (310.9.0 and 310.9.16, Preset C) against those providers, mapped without
  running provider code. Its RTX 30 audit is built only when configured with the
  same `MFG_AMPERE_*` SM86 cache arguments as the release, and checks that the
  complete 26-slot native program is selected from the embedded kernels.
- `tests/ampere_audits` and `tests/ampere_runtime`: RTX 30 font, publication,
  selection and policy checks with data-only provider images and mock CUDA calls.
  `ampere_runtime`'s CTest runs the batch, options, status, Streamline OTA and
  OptiScaler private Streamline startup checks, which need no inputs, the NVAPI
  factory-detour check, which needs
  the installed NVIDIA driver (a split driver-store `nvapi64_impl.dll`), and the
  GPU name override check, which needs an NVIDIA DXGI adapter and detours the
  real System32 `dxgi.dll` inside its own process only. Its
  flip-metering, wrapper, selection and architecture tests take real
  `sl.dlss_g.dll` wrappers and DLSS-G providers as arguments.
- `tests/blackwell_runtime`: RTX 50 identification and menu policy. Its CTest
  runs the selection, Vulkan admission, Streamline OTA, menu family and Dynamic
  MFG checks, which need no inputs, and the adapter probe, which runs the
  production RTX 50 check against the machine's NVIDIA adapters through the
  installed driver's CUDA (exit 2 without one). RTX 50 runs DLSS-G unmodified,
  so nothing here maps or patches a provider.
- `tests/xess_cuda`: the XeMFG tensor-core route against Intel's DXIL programs,
  through the production route code, on an NVIDIA RTX 20/30/40/50 GPU. Every layer
  program, padded and poisoned outputs, fuzzed and shrunken constants, and the
  fallback for incomplete recording state must be byte-identical. It needs the
  build's fatbin, libxess_fg 3.0.2's layer programs and the pinned program set.
- `tests/intel_native`: the Intel Arc native route's driver contract and XeLL
  pairing rules. It maps the Intel driver's XeFG and XeLL files and the XeSS SDK
  3.0.2 runtime without running their code; configure with `XESS_SDK_ROOT` and
  the `INTEL_*` and `XESS_*` fixture paths.
- `tests/localization`: menu translation lookup and Auto language rules,
  the shipped catalogs and whether the Windows fonts they list draw every
  character, the catalog generator's rejections, and that every catalog
  matches the sources. Configure with `IMGUI_ROOT`.

The menu catalogs in `source/native/locale` are validated and compiled in at
configure time by `source/native/generate_locale.py`; a catalog error fails the
configure. See [locale/README.md](source/native/locale/README.md).

These checks do not establish live provider execution or correct final
presentation in a game.
