# Universal MFG Unlocker V1.7.0

**README** · [Install](docs/install.md) · [Using the menu](docs/menu.md) · [Game compatibility](docs/compatibility.md) · [OptiScaler](docs/optiscaler.md) · [XeMFG](docs/xemfg.md) · [Troubleshooting](docs/troubleshooting.md)

Multi Frame Generation for Windows x64 games, in one DLL. It brings **DLSS
Multi Frame Generation** controls to RTX 50, RTX 40, RTX 30 and RTX 20 series
GPUs, with fixed multipliers up to 6x and Dynamic MFG where supported, on their
own or through [OptiScaler](docs/optiscaler.md). RTX 50
cards run DLSS MFG natively, so MFG Unlocker changes nothing in NVIDIA's code
there and only adds its menu controls.

It also runs **XeSS Multi Frame Generation (XeMFG)** on NVIDIA, AMD and Intel
GPUs, on its own ([XeMFG standalone](docs/xemfg.md#xemfg-standalone),
experimental) or through OptiScaler, and **DLSS 5** in games that run DLSS
Super Resolution, DLAA or Ray Reconstruction, with an NVIDIA DLSS 5 dll that
supports your GPU ([details](docs/menu.md#dlss-5)).

For DLSS MFG the game must already have a Streamline DLSS Frame Generation
integration. Available modes depend on the GPU, the active runtime and NVIDIA's
per game MFG override maximum. A listed maximum does not guarantee that the
active Frame Generation pipeline can apply it.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## Quick start

1. Download `MFGUnlocker-v1.7.0.zip` from
   [Releases](https://github.com/dashdogy/MFG-Unlocker/releases) and extract it.
2. Rename `MFGUnlocker.dll` to a filename the game loads early, usually
   `version.dll` or `winmm.dll`. [Game compatibility](docs/compatibility.md)
   lists the name for each game.
3. Place it beside the **actual game executable**, not the launcher.
4. Launch the game, enable Frame Generation, and press **Backspace** to open
   the menu.

The [install guide](docs/install.md) has the full steps, every supported
filename, upgrading and uninstalling. To use OptiScaler, see
[OptiScaler](docs/optiscaler.md). MFG Unlocker is not supported in games with
[anti-cheat](docs/compatibility.md#games-with-anti-cheat).

## What's new in V1.7.0

- **DLSS 5 in the menu:** a [DLSS 5 tab](docs/menu.md#dlss-5) with Hook Method,
  Look, Resolution Scale (with Auto) and Half rate.
- **[XeMFG standalone](docs/xemfg.md#xemfg-standalone) (experimental):** XeSS
  Multi Frame Generation without OptiScaler, in DirectX 12 and Vulkan games, at
  2x to 6x or Dynamic.
- **MFG Unlocker's menu beside OptiScaler's** for the DLSS presets and DLSS 5
  ([OptiScaler](docs/optiscaler.md#menus)).
- **Smoother XeMFG:** a new [flip queue](docs/xemfg.md#pacing) presents each
  batch of frames on an even cadence.
- **XeMFG in Vulkan games through OptiScaler**, with any frame generation input.
- **RTX 30 and RTX 20:** DLSS Frame Generation starts in more games, and stays
  off cleanly when it can't.

The full list, and every earlier release, is in the [changelog](CHANGELOG.md).

## Reporting problems

Read [Troubleshooting](docs/troubleshooting.md) first. Report bugs in
[Issues](https://github.com/dashdogy/MFG-Unlocker/issues), with the log files
listed there. To report how a game works, use the
[game compatibility report](https://github.com/dashdogy/MFG-Unlocker/issues/new?template=game-compatibility.yml).

## Translation credits

Thanks to Shi (@ shinsir on Discord) for editing the whole Vietnamese
translation, and to Slideulidou (@ slideulidou on Discord) for editing the
French translation.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third party components retain their respective licences and notices.
`MFGUnlocker.dll` also embeds XeSS Frame Generation program data derived from
Intel's XeSS SDK. That data is Intel's work, is not covered by the MIT License,
and remains subject to Intel's licence terms. See [NOTICE.md](NOTICE.md).
