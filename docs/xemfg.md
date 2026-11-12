[README](../README.md) · [Install](install.md) · [Using the menu](menu.md) · [Game compatibility](compatibility.md) · [OptiScaler](optiscaler.md) · **XeMFG** · [Troubleshooting](troubleshooting.md)

# XeSS MFG (XeMFG)

XeMFG unlocks Intel XeSS Frame Generation beyond 2x, up to 6x, on NVIDIA, AMD
and Intel GPUs. It runs on its own ([XeMFG standalone](#xemfg-standalone),
experimental), [through OptiScaler](#xemfg-with-optiscaler), or as
[OptiScaler's DLSS Frame Generation](#xemfg-as-optiscalers-dlss-frame-generation)
(experimental).

MFG Unlocker uses the XeSS Frame Generation files that come with the game or
OptiScaler (or, standalone, the `RTXMFG-XeSS` folder); it does not include
Intel's runtime.

## XeMFG standalone

Experimental. MFG Unlocker runs XeSS Frame Generation itself, without
OptiScaler, in DirectX 12 and Vulkan games:

1. Download Intel's XeSS SDK (3.0.2 is tested) and copy `libxess_fg.dll` and
   `libxell.dll` into a folder named `RTXMFG-XeSS` beside MFG Unlocker. MFG
   Unlocker does not include Intel's runtime, and leaves XeMFG off with a
   version it does not recognize (the menu says why).
2. Open the menu, tick **XeMFG standalone** and choose the **MFG mode** (2x to
   6x, or **Dynamic**). Restart the game; the mode can change while playing.
3. In games with DLSS Frame Generation, turn it on in the game's settings.
   XeMFG replaces it: DLSS Frame Generation itself stays off, and XeMFG uses its
   inputs, including the game's HUD-less image. Other games need DLSS, XeSS or
   FSR 3.1 upscaling (or newer) on; XeMFG uses that upscaler's inputs. Turn off
   the game's own FSR or XeSS frame generation.

**Dynamic** changes the frame count every game frame so XeMFG's frame rate
follows the **Dynamic target** (the FPS limit when one is set, otherwise just
under the display's refresh rate, or a value you set), up to the **Up to**
count. **Rounding** picks the count closest to the target, the fewest that
reach it, or the most that stay under it. **Allow 1X** lets it turn frame
generation off, and **Use target as cap** holds the frame rate at the target
with the FPS limit.

Its limits for now: on Vulkan only DLSS upscaling inputs are used; games with
display-resolution motion vectors are not supported; FSR 2 and FSR 3.0 built
into a game cannot be read; and without the game's HUD-less image, the HUD is
interpolated with the scene. The menu's status shows the inputs in use and what
XeMFG is waiting for.

## XeMFG with OptiScaler

In OptiScaler, set the frame generation output to XeFG (`FGOutput = XeFG`
under `[FrameGen]`) and choose the frame count in its menu. See
[OptiScaler](optiscaler.md) for installing MFG Unlocker as a plugin.
OptiScaler's frame limit applies to output frames: at 6x, a 117 FPS limit
leaves about 19.5 real frames per second, so raise or remove it.

**XeMFG in Vulkan games** runs on OptiScaler's XeFG-on-Vulkan path with any
frame generation input. It needs
[our OptiScaler build](optiscaler.md#which-optiscaler-build).

**Dynamic XeMFG** (XeMFG standalone has its own, above) needs OptiScaler with
Dynamic MFG for XeFG
([pull request #1220](https://github.com/optiscaler/OptiScaler/pull/1220),
merged on 19 October 2026): an
[OptiScaler nightly](https://github.com/optiscaler/OptiScaler-nightly/releases)
from 20 October 2026 on, or the next OptiScaler release. Tick **Dynamic MFG**
next to **Frame Gen** in OptiScaler's menu and set the **Target FPS**; the
slider, then shown as **Max Frame Gen**, sets the highest multiplier.

## XeMFG as OptiScaler's DLSS Frame Generation

Experimental. OptiScaler can hand DLSS Frame Generation to another provider
(its Nvngx replacement). MFG Unlocker's provider runs XeMFG there, up to 6x, in
Vulkan and DirectX 12 games; Streamline presents and paces the frames as it does
for DLSS Frame Generation. The ported model can use CUDA on both APIs under the
[tensor core settings](#model-and-tensor-cores). On DirectX 12, wrapped command
lists that cannot be established as runtime lists keep DXIL. On Vulkan, missing
NVX extensions or an unsupported descriptor shape keep SPIR-V.

On this route every GPU runs MFG Unlocker's port of Intel's multi frame model
by default (on NVIDIA with its tensor cores). `XeFGModel = default` under
`[XeFG]` in `OptiScaler.ini` selects XeFG's own 2x model instead, and
`XeFGModel = port` the multi frame model. Restart after changing it. Each
generated frame is made when DLSS Frame Generation asks for it, as XeSS Frame
Generation spreads its own frames.

1. Install MFG Unlocker as an OptiScaler plugin (`MFGUnlocker.asi` in
   `OptiScaler\plugins`, `LoadAsiPlugins = true` under `[Plugins]`) with
   [our OptiScaler build](optiscaler.md#which-optiscaler-build). MFG Unlocker
   registers its provider with OptiScaler under the name `MFGUnlocker`, so no
   extra DLL is needed. Without the plugin the provider does not load and
   OptiScaler falls back to another one.
2. If an earlier version left `mfgunlocker_xemfg.dll` in
   `OptiScaler\custom\MFGUnlocker\dlssg`, delete that folder; the plugin's
   provider replaces it.
3. Under `[FrameGen]` in `OptiScaler.ini`, set `FGOutput = DLSSG` and
   `FGNvngxReplacement = MFGUnlocker`, or pick **MFGUnlocker** in the FG Nvngx
   list of OptiScaler's menu. In games without their own DLSS Frame Generation
   use `FGInput = Upscaler`; `FGInput = NvngxFG` uses the game's own DLSS Frame
   Generation instead.
4. OptiScaler needs Streamline for this route: keep the `streamline` folder
   from OptiScaler's package beside it.

Its limits for now: it needs XeSS Frame Generation 1.3.1.78 (the XeSS 3.0.2
SDK's `libxess_fg.dll`) or 1.3.4.95; with another version the provider stays
unloaded and OptiScaler says it fell back. In DirectX 12 games the motion
vectors must cover the same pixel area as the depth (the game's own rectangles
are used when it passes them), and the game's UI texture is not used (its
HUD-less image is). With `FGInput = NvngxFG` in Vulkan games, stock OptiScaler
applies its frame limit to each generated frame once more, which drops the
frame rate far below the limit; set `FramerateLimit = 0` there, or use our
OptiScaler build. When the route does not start, the RTXMFG log names the
reason (`ngx_provider_registered` with `"selected":false`,
`ngx_provider_unavailable`, or `vk_provider_refused` / `d3d12_provider_refused`).

## Pacing

A flip queue presents each batch of XeMFG frames on an even cadence, which
removes the stutter some games had. On AMD Radeon RX 9000 with Adrenalin
26.10.1 or newer it also turns on the driver's flip meter at 2X; the driver
offers it only at 2X, so above that the flip queue paces every frame itself.
Radeon RX 7000 and older have no driver flip meter and use the flip queue
alone. Wine/Proton keep V1.6.1's pacing.

On NVIDIA GPUs whose driver reports flip metering support, MFG Unlocker paces
XeMFG's frames with the driver's flip metering, the way DLSS Frame Generation
does on RTX 40. Other NVIDIA GPUs get every frame paced by MFG Unlocker, as on
AMD.

## Model and tensor cores

MFG Unlocker runs Intel's multi frame model on any GPU by default.

On RTX 20, RTX 30, RTX 40 and RTX 50 GPUs, the model's int8 network layers run
on the tensor cores (CUDA kernels launched inside XeFG's own command list;
their output is identical to Intel's). Other GPUs, and any layer this does not
cover, run as before.

This also applies to XeMFG as OptiScaler's DLSS Frame Generation, on DirectX 12
and Vulkan. Both routes admit the same supported RTX architectures by default;
layers the route cannot handle keep the existing DXIL or SPIR-V path in the
same frame. `XeFGTensorCores = off` turns CUDA off on every route. For
diagnosis, `XeFGTensorCoresMask` selects layers by a hexadecimal 17-bit mask
(default `1ffff`), and `XeFGTensorCoresVulkanBarriers = on` adds a full barrier
after each Vulkan launch (default `off`).

## Intel Arc

On Intel Arc, XeMFG uses Intel's native driver model and pacer. MFG Unlocker
restores 5x and 6x when it recognizes the loaded XeFG and XeLL driver pair; an
unknown driver keeps its stock limits. Arc support is experimental. Leave the
frame generation override in Intel Graphics Software off or following the game.
The model and tensor core settings do not apply on Arc.

## OptiScaler.ini settings

These go under `[XeFG]` in `OptiScaler.ini`. Restart the game after changing
them.

| Setting | Effect |
|---|---|
| `XeFGModel = default` | Uses Intel's 2x model instead of its multi frame model. |
| `XeFGTensorCores = off` | Turns off the tensor core path on RTX GPUs, on every route. |
| `XeFGFlipMetering = off` | Turns off flip metering pacing on NVIDIA GPUs; `on` uses it even where the driver does not report it. |
| `XeFGFlipQueue = off` | Goes back to V1.6.1's pacing on any GPU. |
| `XeFGSceneChangeThreshold = intel` (or a threshold from 0 to 1) | Uses Intel's scene-change detector. |
| `XeFGHdr10Flag = stock` | Uses XeFG's own HDR10 handling. Does not apply on Arc. |

XeFG's scene-change detector is off by default. It treated fast camera turns as
scene cuts, so the picture froze and then jumped; real cuts still reset when
the game reports them.

XeFG's own HDR10 handling is off by default. With it on, players with very
bright HDR displays saw interpolation errors in the brightest areas.
