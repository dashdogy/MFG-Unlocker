[README](../README.md) · [Install](install.md) · [Using the menu](menu.md) · [Game compatibility](compatibility.md) · **OptiScaler** · [XeMFG](xemfg.md) · [Troubleshooting](troubleshooting.md)

# OptiScaler

MFG Unlocker runs as an [OptiScaler](https://github.com/optiscaler/OptiScaler)
plugin, for both DLSS MFG and XeMFG. Use a current OptiScaler 10 build; the
0.9.x releases are not supported.

## Which OptiScaler build

MFG Unlocker V1.7.0 pairs with our OptiScaler build,
[OptiScaler for MFG Unlocker V1.7.0](https://github.com/dashdogy/OptiScaler/releases/tag/mfgunlocker-v1.7.0) (dashdogy/OptiScaler).
Install OptiScaler as usual from its nightly-20261024, then replace
`OptiScaler.dll` with the one from that release, keeping the name it was
installed under. Stock OptiScaler does not have these yet, until they are
upstreamed:

- `FGOutput = XeFG` in Vulkan games, with any `FGInput` (`Upscaler` or
  `DLSSG`): XeMFG in Vulkan games needs this build.
- `FGInput = DLSSG` in games that check DLSS Frame Generation at startup;
  with stock OptiScaler, Doom: The Dark Ages greys out its Frame Generation
  option.
- `FGOutput = XeFG` in DirectX 12 games: OptiScaler's menu and FPS overlay
  no longer hold the game in Present.
- `FGInput = FSRFG` or `FSRFG30` (the game's own FSR Frame Generation): XeFG
  leaves the swap chain to it, and the game's UI still shows while frame
  generation is off (Modern Warfare 4).

DLSS MFG through OptiScaler, and XeMFG in DirectX 12 games with the
`Upscaler` input, also work with a current upstream OptiScaler 10 build.

## Install as a plugin

1. Rename `MFGUnlocker.dll` to `MFGUnlocker.asi` and place it in OptiScaler's
   plugins folder (`OptiScaler\plugins` beside OptiScaler).
2. In `OptiScaler.ini`, set `LoadAsiPlugins = true` under `[Plugins]`.
3. Choose the frame generation output under `[FrameGen]`: `FGOutput = DLSSG`
   for DLSS MFG, then pick the multiplier with **Override DLSSG Ratio** in
   OptiScaler's menu, or `FGOutput = XeFG` for
   [XeMFG](xemfg.md#xemfg-with-optiscaler).

Do not also install MFG Unlocker under a proxy filename. On RTX 30 the DLSS MFG
route is experimental; if it does not start, use XeMFG.

## Menus

With OptiScaler, its menu owns the frame generation controls. MFG Unlocker's
menu shows beside it only while the game runs DLSS Super Resolution, DLAA,
Ray Reconstruction or DLSS 5, and offers the DLSS presets and
[DLSS 5](menu.md#dlss-5) there (hidden in Forza Motorsport and Forza Horizon).
Set `"optiScalerUi": "optiscaler"` in `RTXMFG-Universal.json` to keep it
hidden in every game.

## Uninstall

Close the game and remove `MFGUnlocker.asi` from OptiScaler's plugins folder.
