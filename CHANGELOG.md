# Changelog

What changed in each release. Downloads and the full release notes are on
[GitHub Releases](https://github.com/dashdogy/MFG-Unlocker/releases).

## [V1.7.0](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.7.0) - 25 Oct 2026

- **DLSS 5 support in the menu.** With NVIDIA's DLSS 5 dll (`nvngx_dlssnr.dll`)
  beside the game's executable or MFG Unlocker, a **DLSS 5** tab applies DLSS 5
  in games that run DLSS Super Resolution, DLAA or Ray Reconstruction. Choose
  the **Hook Method** (Auto, Before upscaling, After upscaling or Present), a
  **Look** (Subtle, Balanced or Strong) and the **Resolution Scale** (with
  **Auto**, which lowers it to keep DLSS 5's cost in budget, never below 75 %);
  **Advanced** holds the rest. **Half rate** runs the network on every other
  frame for about half the cost (DirectX 12, Before or After upscaling). In
  Vulkan games DLSS 5 runs After upscaling or Auto. MFG Unlocker does not
  include or download the dll, and the tab only appears when one is there.
- **XeMFG standalone (experimental).** XeSS Multi Frame Generation without
  OptiScaler, in DirectX 12 and Vulkan games, at 2x to 6x or Dynamic. See
  [XeMFG standalone](docs/xemfg.md#xemfg-standalone).
- **MFG Unlocker's menu beside OptiScaler.** With OptiScaler, MFG Unlocker's
  menu now shares the screen with OptiScaler's while the game runs DLSS Super
  Resolution, DLAA, Ray Reconstruction or DLSS 5, and offers the DLSS presets
  and DLSS 5 there. Frame generation stays in OptiScaler's menu. Forza
  Motorsport and Forza Horizon keep it hidden. To hide it in every game, set
  `"optiScalerUi": "optiscaler"` in `RTXMFG-Universal.json`.
- **Smoother XeMFG.** A new flip queue presents each batch of XeMFG frames
  on an even cadence, which removes the stutter some games had. On AMD Radeon
  RX 7000 and RX 9000 with Adrenalin 26.10.1 or newer it also drives the
  driver's flip meter. Radeon RX 6000 and older, and Wine/Proton, keep V1.6.1's
  pacing. To go back to V1.6.1's pacing on any GPU, set `XeFGFlipQueue = off`
  under `[XeFG]` in `OptiScaler.ini`.
- **XeMFG in Vulkan games through OptiScaler** runs on OptiScaler's
  XeFG-on-Vulkan path with any frame generation input. Use the OptiScaler
  build linked under [OptiScaler support](docs/optiscaler.md).
- **RTX 30 and RTX 20:** DLSS Frame Generation starts when the game's
  Streamline allocates fewer frame buffers than MFG Unlocker expected
  (Final Fantasy VII Rebirth on an RTX 2070 SUPER), and when NVIDIA's runtime
  answers Streamline's startup check with an error. When it can't start,
  MFG Unlocker tells Streamline to keep it off instead of retrying every
  frame (Doom: The Dark Ages's video memory grew until it crashed while the
  retries ran).
- **Also:** the Reflex FPS limit works with Streamline Reflex 2.7 to 2.14
  (Wuthering Waves); a French menu translation; the menu's cursor follows the
  mouse smoothly while frame generation runs; and the log has timestamps, a
  crash record with a minidump beside it, and a `Session end` line.

## [V1.6.1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.6.1) - 20 Oct 2026

- **Change DLSS presets while you play, on every RTX card.** Live preset
  switching now works on RTX 50, RTX 30 and RTX 20 cards as well as RTX 40:
  the DLSS Frame Generation preset (Game/driver, A, B or C) and the DLSS
  Super Resolution and Ray Reconstruction presets apply while the game runs.
  Frame Generation restarts once when its preset changes. Vulkan games, and
  DLSS Frame Generation versions that only have their original preset, still
  need a game restart. See [Usage](docs/menu.md).
- **RTX 50 series support.** MFG Unlocker recognises RTX 50 cards, and the
  menu works there as on the other cards: MFG mode and Dynamic, the DLSS
  presets, the Reflex FPS limit and the FPS readout. RTX 50 runs DLSS MFG
  natively, so MFG Unlocker changes nothing in NVIDIA's code there.
- **RTX 30 and RTX 20:** a fix for the DLSS Frame Generation option staying
  greyed out when the NVIDIA app's DLSS override supplies DLSS Frame
  Generation (reported in Final Fantasy VII Rebirth). If it still happens,
  the log now says why.

## [V1.6.0](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.6.0) - 19 Oct 2026

- **DLSS presets while you play.** DLSS Frame Generation presets
  apply while Frame Generation runs (RTX 40). A new menu section shows whether DLSS Super
  Resolution or Ray Reconstruction is running and switches their presets live.
  See [Usage](docs/menu.md).
- **DLSS Frame Generation on RTX 20** (experimental): RTX 2060 to RTX 2080 Ti,
  the same way as on RTX 30. Support is experimental; if you run into errors,
  please attach your logs to an issue.
- **RTX 30 DLSS Frame Generation in Vulkan games** (experimental). **V-Sync: Off**
  and the measured DLSS FPS now also work in Vulkan games.
- **XeMFG:** XeSS Frame Generation 1.3.4 support, smoother pacing after a game
  hitch, evenly spaced generated frames with flip metering, and no more freeze
  then jump on fast camera turns. See [XeSS MFG (XeMFG)](docs/xemfg.md).
- **Logs are written to the game folder** instead of `%TEMP%`.
- **Fixes two crashes at game launch:** when Streamline loads the NVIDIA app's
  newer DLSS Frame Generation plug-in in place of the game's own copy, and with
  OptiScaler in some Streamline games such as Subnautica 2.

**Many fixes for RTX 30 series cards:**

- **No more stall between generated frames at 3x and above.** RTX 30 DLSS
  Frame Generation now uses the driver's flip metering by default.
- **Star Wars Outlaws:** the black flicker after a resize or fullscreen switch
  is fixed.
- **OptiScaler:** RTX 30 DLSS Frame Generation is detected correctly when
  OptiScaler is loaded, and OptiScaler's own DLSSG output can now start it on
  RTX 30 (experimental). See [OptiScaler support](docs/optiscaler.md).
- **Game FG option: Unlock** shows the DLSS Frame Generation option in games
  that hide it on RTX 30 cards. See
  [Compatibility and troubleshooting](docs/troubleshooting.md).
- **XeMFG runs on the tensor cores** of RTX 30 cards.

Also new:

- **XeMFG on tensor cores.** On RTX 20, RTX 30, RTX 40 and RTX 50 GPUs the
  model's network layers run on the tensor cores. On other GPUs the model runs
  about 7% faster than in V1.5.0.
- **XeMFG uses the driver's flip metering on NVIDIA GPUs**, the way DLSS Frame
  Generation paces its frames on RTX 40. See [XeSS MFG (XeMFG)](docs/xemfg.md).
- **XeMFG Dynamic** holds its target better and no longer stays on a higher
  multiplier once the target is reached. It needs OptiScaler's Dynamic MFG for
  XeFG, in OptiScaler nightlies from 20 October 2026 on; see
  [XeSS MFG (XeMFG)](docs/xemfg.md).
- **Native 5x and 6x on Intel Arc.**
- **RTX 40 with OptiScaler's DLSSG output** is no longer stuck at 2x.
- **XeMFG is no longer stuck at 2x in games that start it right at launch**
  (seen in Trails in the Sky 2nd Chapter with OptiScaler). MFG Unlocker now
  hooks XeSS Frame Generation before OptiScaler first creates it.
- **Lower XeMFG latency without flip metering** (AMD and Intel GPUs, or with
  flip metering off). MFG Unlocker's pacing no longer holds each frame back by
  XeSS Frame Generation's GPU latency; frames go out on XeFG's own schedule.
- **Updated NVIDIA game list:** games such as Dragon's Dogma 2 are no longer
  capped at 2x.
- **Menu translations:** Simplified Chinese, Ukrainian, Italian, Swedish,
  Turkish and Vietnamese. A **Pin** button keeps the menu on screen, and
  **Pin FPS** shows a small real and DLSS FPS readout in the top left corner
  (DLSS Frame Generation only).
- Includes the VRAM leak fix from V1.5.0-hotfix.1.

## [V1.5.0 Hotfix 1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.5.0-hotfix.1) - 15 Oct 2026

- **Fixes a VRAM leak when Frame Generation is switched on again.**
  In The Witcher 3 each switch kept about 425 MiB of video memory until the
  game was closed.

## [V1.5.0](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.5.0) - 13 Oct 2026

- **New name: Universal MFG Unlocker.** The download contains
  `MFGUnlocker.dll`. When upgrading, remove the old `RTXMFG.dll` (or its renamed
  copy) first so only one copy loads. Settings in `RTXMFG-Universal.json` carry
  over.
- **OptiScaler support.** MFG Unlocker now runs as an OptiScaler plugin
  (`MFGUnlocker.asi` in `OptiScaler\plugins`), working with OptiScaler's frame
  generation, latency reduction and menu. See
  [OptiScaler support](docs/optiscaler.md).
- **XeSS Multi Frame Generation through OptiScaler**, 2x to 6x on any GPU, with
  Intel's multi frame model by default. See [XeSS MFG (XeMFG)](docs/xemfg.md).
- **DLSS Frame Generation Preset C**, NVIDIA's new performance mode, on RTX 40
  and RTX 30, including at 3x to 6x. Select **Preset C** in the menu or in the
  NVIDIA app's DLSS override. It needs DLSS Frame Generation 310.9.16, which
  the NVIDIA app provides.
- **RTX 30 series support is now stable**, with smoother pacing at 3x and above
  and DLSS Frame Generation 310.9.16.

## [V1.4.2](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.4.2) - 8 Oct 2026

- Added more resilient detection for path-traced hair in The Witcher 3, which
  should help keep the mod working after game patches.

## [V1.4.1 Hotfix 1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.4.1-hotfix.1) - 2 Oct 2026

- **Fixes path-traced HairWorks compatibility with ReShade and RenoDX in
  The Witcher 3.**
- Fixes cases of hair disappearing after loading a save or moving to another area.
- Keeps hair diagnostics available throughout longer sessions, improving logs
  for bug reports.

## [V1.4.1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.4.1) - 1 Oct 2026

- **Refines path-traced HairWorks in The Witcher 3 on RTX 40.**
- Fixes hair setting changes and save/reload behavior, and reduces per-frame
  CPU overhead from hair processing.
- Prevents unrelated overlay command lists from disabling path-traced hair
  for the rest of the session, and improves hair resource cleanup.
- Adds clearer hair activity and cost information to the menu and runtime log.
- Fixes menu access in **Where Winds Meet** and input handling in the
  **Microsoft Store version of Microsoft Flight Simulator 2024**.
- Keeps the previous session's runtime log as `*.previous.log` to help diagnose
  problems after a restart.

## [V1.4](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.4.0) - 1 Oct 2026

- **Unlocks path-traced HairWorks in The Witcher 3 on RTX 40 series GPUs.**
  V1.4 brings animated hair into the game's path-traced
  lighting, with support built into RTXMFG and controlled by the game's hair settings.
- Adds HUDless detection and an optional tint view for inspecting the game's
  HUDless input. UI recomposition remains controlled by the game.
- Adds a **V-Sync: Off** option alongside **Game**, with status showing whether
  the request was applied. NVIDIA Control Panel settings take priority.
- Fixes menu discovery through newer DXGI factory interfaces and Streamline's
  manual interface upgrades, including The Witcher 3's DirectX 12 path and Wuthering Waves.
- Improves early Streamline interception and plugin unload/reload handling.

## [V1.3.3 Hotfix 2](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.3.3-hotfix.2) - 18 Sep 2026

- Restores missing menus in MSFS 2024 and GTA V Enhanced, including HDR and
  locked-status-file handling. The MSFS menu fix is confirmed in-game.
- Improves renderer detection and fixes the small-stack startup overflow.
- Reuses one log and one status file per installation, overwritten on launch.

Dying Light 2 and Subnautica 2 in-game retests remain pending.

## [V1.3.3](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.3.3) - 13 Sep 2026

- Clean installs default to **Follow game** and open the full menu on first
  launch. Saved fixed/Dynamic choices are preserved when upgrading.
- Rebuilds menu startup and DirectX 12 swapchain handling to avoid patching
  shared Streamline methods.
- Corrects active Frame Generation detection, pending multiplier reporting,
  and NVIDIA per-game override limits.
- Shows Dynamic as unavailable for older runtimes and incompatible ranges,
  instead of leaving the option on "checking" after a limit is established.
- Restores the verified DLSS-G count/index gate on RTX 40, addressing the
  reported 2x lock, freezing and black-screen regression.
- Restores Vulkan menu interception through direct imports, dynamic resolvers
  and Streamline's internal loader path, including mixed semaphore ownership.

The latest Vulkan correction passed local rendering regression tests. An
Indiana Jones in-game retest is still pending; this is not a compatibility
guarantee for that game or every Vulkan title.

## [V1.2](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.2) - 4 Sep 2026

1. Added one Universal build for DirectX 12 and Vulkan games.
2. Added live fixed and Dynamic controls through the ReShade menu.
3. Added automatic game capability limits, Ada temporal correction, and telemetry.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## Older releases

- [V1.3.3 Hotfix 1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.3.3-hotfix.1) - 17 Sep 2026
- [V1.3.2](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.3.2) - 10 Sep 2026
- [V1.2.1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.2.1) - 9 Sep 2026
- [V1.1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.1) - 1 Sep 2026
- [V1.0.3](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.0.3) - 31 Aug 2026
- [V1.0.0](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v1.0.0) - 31 Aug 2026
- [V0.7.0](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v0.7.0) - 29 Aug 2026
- [V0.6.1](https://github.com/dashdogy/MFG-Unlocker/releases/tag/v0.6.1) - 29 Aug 2026
