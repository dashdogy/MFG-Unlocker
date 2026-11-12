[README](../README.md) · [Install](install.md) · [Using the menu](menu.md) · [Game compatibility](compatibility.md) · **OptiScaler** · [XeMFG](xemfg.md) · [Troubleshooting](troubleshooting.md)

# OptiScaler

MFG Unlocker runs as an [OptiScaler](https://github.com/optiscaler/OptiScaler)
plugin, for both DLSS MFG and XeMFG. Use a current OptiScaler 10 build; the
0.9.x releases are not supported.

## Which OptiScaler build

MFG Unlocker V1.7.1 pairs with our OptiScaler build,
[OptiScaler for MFG Unlocker V1.7.1](https://github.com/dashdogy/OptiScaler/releases/tag/mfgunlocker-v1.7.1) (dashdogy/OptiScaler).
Install OptiScaler as usual from its nightly-20261111, then replace
`OptiScaler.dll` with the one from that release, keeping the name it was
installed under. Stock OptiScaler does not have these yet, until they are
upstreamed:

- [XeMFG as OptiScaler's DLSS Frame Generation](xemfg.md#xemfg-as-optiscalers-dlss-frame-generation)
  (`FGNvngxReplacement = MFGUnlocker`): MFG Unlocker registers its provider
  through OptiScaler's plugin API, and Dynamic MFG works with the `DLSSG`
  output.
- `FGOutput = XeFG` in Vulkan games, with any `FGInput` (`Upscaler` or
  `DLSSG`): XeMFG in Vulkan games needs this build.
- `FGOutput = XeFG` in DirectX 12 games: OptiScaler's menu and FPS overlay
  no longer hold the game in Present.
- `FGInput = FSRFG` or `FSRFG30` (the game's own FSR Frame Generation): XeFG
  leaves the swap chain to it, and the game's UI still shows while frame
  generation is off (Modern Warfare 4).
- Fixes for the `DLSSG` output: the FPS limit counts generated frames
  (Crimson Desert), and two kinds of game keep their own Streamline plugins
  instead of OptiScaler's newer ones: games whose Streamline refuses newer
  plugins (Modern Warfare 4 lost DLSS) and games whose older Streamline
  interposer cannot hook them (Like a Dragon: Pirate Yakuza in Hawaii crashed).

DLSS MFG through OptiScaler, and XeMFG in DirectX 12 games with the
`Upscaler` input, also work with a current upstream OptiScaler 10 build. The
**MFG Unlocker** tab in OptiScaler's menu (OptiScaler's plugin tabs) needs our
build or an upstream nightly from 4 November 2026 on.

## Install as a plugin

1. Rename `MFGUnlocker.dll` to `MFGUnlocker.asi` and place it in OptiScaler's
   plugins folder (`OptiScaler\plugins` beside OptiScaler).
2. In `OptiScaler.ini`, set `LoadAsiPlugins = true` under `[Plugins]`.
3. Choose the frame generation output under `[FrameGen]`: `FGOutput = DLSSG`
   for DLSS MFG, then pick the multiplier with **Override DLSSG Ratio** in
   OptiScaler's menu, or `FGOutput = XeFG` for
   [XeMFG](xemfg.md#xemfg-with-optiscaler). For XeMFG through OptiScaler's
   DLSS Frame Generation, see
   [XeMFG as OptiScaler's DLSS Frame Generation](xemfg.md#xemfg-as-optiscalers-dlss-frame-generation).

Do not also install MFG Unlocker under a proxy filename. On RTX 30 the DLSS MFG
route is experimental; if it does not start, use XeMFG.

## Menus

With OptiScaler, its menu owns the frame generation controls. With our
OptiScaler build or an upstream nightly from 4 November 2026 on, the DLSS
presets and [DLSS 5](menu.md#dlss-5) are in the **MFG Unlocker** tab of
OptiScaler's menu. With older builds, or MFG Unlocker under a proxy filename,
MFG Unlocker's menu shows beside OptiScaler's only while the game runs DLSS
Super Resolution, DLAA, Ray Reconstruction or DLSS 5, and offers them there
(hidden in Forza Motorsport and Forza Horizon). In `RTXMFG-Universal.json`,
`"optiScalerUi": "shared"` keeps MFG Unlocker's menu instead of the tab, and
`"optiScalerUi": "optiscaler"` hides MFG Unlocker's menu in every game.

## Uninstall

Close the game and remove `MFGUnlocker.asi` from OptiScaler's plugins folder.
