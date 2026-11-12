# Universal MFG Unlocker V1.7.1

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

1. Download `MFGUnlocker-v1.7.1.zip` from
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

## What's new in V1.7.1

- **DLSS 5 presets:** save your settings under a name and share them as a code
  ([DLSS 5](docs/menu.md#dlss-5)); Auto resolution scale changes smoothly.
- **DirectX 11 games:** the menu and DLSS 5 now run there too.
- **[XeMFG as OptiScaler's DLSS Frame Generation](docs/xemfg.md#xemfg-as-optiscalers-dlss-frame-generation)
  (experimental):** up to 6x and Dynamic MFG in DirectX 12 and Vulkan games,
  with no extra DLL.
- **[RTX Hair on RTX 40 and RTX 30](docs/compatibility.md#path-traced-hair)**,
  on by default, Vulkan games included.
- **An MFG Unlocker tab in OptiScaler's menu**
  ([OptiScaler](docs/optiscaler.md#menus)), and a
  [Keybinds tab](docs/menu.md#keys) in MFG Unlocker's own menu.
- **XeMFG pacing:** a reworked pacer on AMD for smoother frames and lower
  latency.
- **RTX 30 and RTX 20** are no longer held at 2x in Streamline 2.12 games
  (Darktide).

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
