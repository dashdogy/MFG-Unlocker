# Release regression harnesses

Use the pinned Windows x64 tools and SDK inputs in [BUILD.md](../../BUILD.md).
Build the release DLL first, then configure this separate project:

```powershell
cmake -S tests/release_regression -B build/release-tests `
    -G 'Visual Studio 17 2022' -A 'x64,version=10.0.22621.0' -T v143 `
    '-DCMAKE_VS_GLOBALS=VCToolsVersion=14.38.33130' `
    '-DSTREAMLINE_ROOT=C:/SDKs/streamline-sdk-v2.14.1' `
    '-DIMGUI_ROOT=C:/SDKs/reshade-6.8.0/deps/imgui' `
    '-DVULKAN_INCLUDE_DIR=C:/VulkanSDK/1.2.176.1/Include' `
    '-DVULKAN_LIBRARY=C:/VulkanSDK/1.2.176.1/Lib/vulkan-1.lib'
cmake --build build/release-tests --config Release --parallel 6
python tests/release_regression/run.py `
    --dll build/v1.5.1/Release/MFGUnlocker.dll `
    --harness-dir build/release-tests/Release `
    --output build/release-test-results `
    --provider C:/Fixtures/nvngx_dlssg.dll
```

Supply an identified, supported DLSS-G provider for `--provider`. Its CPU
count/index gate is exercised using the normal publisher in the exact release
DLL. The runner verifies that the provider file remains unchanged. Without this
argument, that case is skipped and provider gate validation is incomplete.

`--xess-pair <folder>` takes the profiled XeSS SDK 3.0.2 `libxess_fg.dll` and
`libxell.dll` (verified by SHA-256) and adds `optiscaler-xefg-proxy` and
`optiscaler-xefg-plugin`. The OptiScaler stand-in loads that pair from
`OptiScaler\` and resolves `xefgSwapChainD3D12CreateContext` in its `DllMain`
before plug-ins, as OptiScaler does. The exact DLL then loads under a proxy name
or as the plug-in. Its detour of that entry must be live within 150 ms of the
load. V1.6.0 needed about 380 ms, so OptiScaler's early first creation stayed
stock 2x. A host creation on a WARP device must then reach the host route, with
the unlock applied.

The same argument adds the `optiscaler-xefg-output*` cases (configure with
`XESS_SDK_ROOT` at the XeSS SDK 3.0.2 so the stand-in can build OptiScaler's
XeFG output). Through the plug-in route, before the executable makes any factory
call, the stand-in creates a XeFG context, initializes it from a swapchain
description with DXGI's own factory and takes the XeFG swapchain, as OptiScaler's
FGOutput XeFG does. RTXMFG's menu must register on XeFG's native output chain
(device, queue and LUID matched), see the 120 Presents of a test window on the
default adapter, draw there in the "rtxmfg" UI mode (the startup hint), draw
nothing in the shared mode while no DLSS feature runs, attach nothing in the
"optiscaler" mode, and retire with the XeFG context. `optiscaler-xefg-output-factory`
lets the stand-in's hook on DXGI's CreateSwapChainForHwnd answer the executable's
own creation through RTXMFG's factory proxy: the factory route then registers
its single renderer and the output chain gets none. Without this argument, all
of these cases are skipped.

The runner creates a separate directory per case, copies the exact DLL and
required fixtures, captures exit codes and hashes, and stops on failure.
`--only` accepts case names from `run.py` for focused reruns. It does not launch
games or replace installed files. WARP menu cases briefly use a foreground test
window; Vulkan cases require a compatible NVIDIA Vulkan device. Missing debug
or validation layers are reported in the individual logs.

Coverage includes Dynamic availability for legacy runtimes and range limits,
clean/default and saved controls, renamed loader forwarding
and duplicate ownership, observed wrapper controls, NVIDIA policy limits,
first-launch menu pixels and persistence across relaunch, WARP presentation,
layered and ReShade-wrapped swapchains, and Vulkan
direct/dynamic/internal-Streamline/mixed routes.
Hotfix coverage also includes unaligned DXGI factory imports, native adapter
parent-factory returns, coexistence with existing hooks, writable/executable import tables,
HDR interface forwarding, locked status files and process identity, renderer DLL
imports and late loading, 64 KiB/128 KiB resolver threads, and log/status reuse
across launches. The first-launch case reuses its settings directory in a second process to
verify that the automatic menu does not reopen. `first-launch-zh`, `first-launch-uk`,
`first-launch-it`, `first-launch-sv`, `first-launch-tr`, `first-launch-vi` and `first-launch-fr` repeat the first
launch with `Language=zh-CN` / `uk-UA` / `it-IT` / `sv-SE` / `tr-TR` / `vi-VN` / `fr-FR` saved and require the runtime log to
report that language's font ready.

`factory-revision-menu` loads the exact DLL and reproduces Witcher 3's base
`IDXGIFactory` creation followed by `EnumAdapters1`, `IsCurrent` and newer
swapchain creation without requesting those factory revisions first. It also
checks the existing WARP menu, presentation and teardown behavior.
The supported-path fixture requests `IID_IDXGIFactory` through
`CreateDXGIFactory1`; `factory-revision-unsupported` uses the older
`CreateDXGIFactory` API, whose factory rejects `IDXGIFactory1`, and checks
safe error returns and proxy retirement in the exact DLL.
`factory-revision-present` exercises the same factory calls and WARP
presentation/teardown without the foreground-dependent keyboard assertions.
`interposer-entry-menu` reproduces Where Winds Meet's packed executable. It
reads the interposer's `CreateDXGIFactory*` exports from the export table
after a non-manual `slInit`, with no import slot or `GetProcAddress` call.
Executable calls must return exactly one menu factory proxy, including after
a second `slInit`. A foreign DLL calling the same exported entry must stay
native. The swapchain must then present, open and close the menu.
`rwx-aligned-menu` reproduces the Microsoft Store MSFS 2024 executable: its
eight-byte-aligned `USER32` import slots sit in a data page that becomes
executable after startup. The menu must open with game-only input suppression
through public-entry detours, leaving the slot bytes and page protection as found.
`gpuname-off`, `gpuname-on` and `gpuname-invalid` load the exact DLL with the
opt-in `[Compatibility] GpuNameOverride` setting absent, `1` and unrecognized.
Off and unrecognized must leave System32 `dxgi.dll`'s `GetDesc1` byte-identical;
on must arm all four adapter description detours from the worker before the
startup status. Every adapter the executable queries must keep its real identity
unless it is an RTX 30 card with the setting on, and a non-RTX 30 NVIDIA query
must be logged as left unchanged.
The DirectX 11 cases (dx11menu.1) run in `D3D11PresentHarness.exe`, which, like
a DirectX 11 game, links no D3D12 creation: to the menu, an executable that links
D3D12 or Vulkan creation and no D3D11 creation is a D3D12 or Vulkan game. Each
case resolves `d3d11.dll`'s entries through the executable's `GetProcAddress`, so
it reaches the menu's D3D11 gateways. It uses the D3D11 debug layer when the
Graphics Tools are installed, and the runtime log must show
`renderer registered api=d3d11`. The menu checks are the same in every case:
- the menu key opens the menu with visible pixels;
- the game's bound pipeline state is identical across a Present with the menu
  drawn: viewport, scissor, rasterizer, blend, topology, the off-screen render
  target and depth view, compute shader and UAV, stream output, predication, a
  vertex shader resource, and constant buffers in slots 0 and 2;
- a TEST Present draws nothing, keyboard messages are captured, and
  `ResizeBuffers` works with the menu open;
- the menu closes and every proxy retires.

`d3d11-menu` takes the application's factory route, with `PinnedFps=1` saved.
The startup hint must appear and no FPS readout after it. A Present without UI
must keep the whole state (the control for the state check). The legacy
`CreateSwapChain` path (blt-model `SEQUENTIAL`, one `R8G8B8A8_UNORM_SRGB`
buffer) on the same window must draw the menu too. `d3d11-mt-menu` takes the
same route with the game's `ID3D11Multithread` protection on: another thread
holding that lock must never see the menu's context state.
`d3d11-adapter-menu` creates a WARP device with `D3D11CreateDevice` and no
adapter. The factory the executable then takes through `IDXGIDevice` ->
`GetAdapter` -> `GetParent` must be the menu's proxy, and the log must show the
adapter-parent entry armed from the D3D11 device adapter.
`d3d11-chain-menu` uses `D3D11CreateDeviceAndSwapChain`, and the log must show
`boundary=d3d11-swapchain-return`. The chain's parent factory must be the
menu's proxy, and a chain created again from it keeps the menu. A call that asks
for no device (blt-model sRGB) must be wrapped as well.

Two cases cover D3D12 applications. `d3d11-after-d3d12` creates a D3D12 device
first through `D3D12CreateDevice` (the default adapter, else WARP).
`d3d12-with-d3d11-menu` runs in `WarpPresentHarness.exe`, which links D3D12, and
adds a D3D11 device and window to the D3D12 game. In both, the D3D11 swap chain
must get no DirectX 11 menu (`step=application_api` in the log), and no Present
of it may be counted or drawn on. The D3D11 device's adapter must give the native
factory, and the log must never show `api=d3d11` or arming from the D3D11 device
adapter. In `d3d12-with-d3d11-menu`, the D3D12 menu must then pass the usual
menu checks.

Run them alone with
`python tests/release_regression/run.py --dll <MFGUnlocker.dll> --harness-dir build/release-tests/Release --output <folder> --only d3d11-menu d3d11-mt-menu d3d11-adapter-menu d3d11-chain-menu d3d11-after-d3d12 d3d12-with-d3d11-menu`,
or directly as `D3D11PresentHarness.exe <case> <folder>\winmm.dll` (or
`WarpPresentHarness.exe d3d12-with-d3d11-menu ...`) from a folder holding
`RTX40MFG-UI.ini` with `[Overlay] FirstLaunchMenuShown=1` (and, for
`d3d11-menu`, `PinnedFps=1`).
Run `FactoryRevisionTests.exe` directly from the Release harness directory for
the source-level supported/unsupported revision, lazy-query and concurrent
cache-lifetime cases. This target compiles the production proxy with isolated
renderer/coordinator stubs; it does not exercise the release DLL's backend.

These tests establish synthetic rendering, forwarding and CPU gate behavior.
They do not establish live DLSS-G provider Evaluate output or ordered, distinct
generated frames in any game. RTX 30 GPU execution remains separately unverified.
