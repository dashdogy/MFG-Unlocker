# Universal MFG Unlocker V1.7.2

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

1. Download `MFGUnlocker-v1.7.2.zip` from
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

## What's new in V1.7.2

A bugfix release built from your V1.7.1 reports.

- **[XeMFG standalone](docs/xemfg.md#xemfg-standalone) in DLSS Frame
  Generation games** now swaps in for the game's own frame generation, so
  switching it on and off in game is more stable, RTX 50 to RTX 20
  (experimental). Everywhere else switching is more reliable and turning it
  back on is quicker.
- **Crash fixes:** quitting Cyberpunk 2077, A Plague Tale: Resonance with
  OptiScaler, and with [our OptiScaler build](docs/optiscaler.md#which-optiscaler-build)
  Assassin's Creed Mirage, World War Z on Vulkan, Dead Space on exit,
  Miles Morales and the Metro Redux games.
- **Dead Space and A Plague Tale: Requiem** get their DLSS back, **DLSS 5**
  works in Microsoft Flight Simulator 2024, and frame generation works in
  Trails through Daybreak, Daybreak II and Trails beyond the Horizon.
- **XeMFG through OptiScaler:** no more VRAM pile-up in Dying Light 2 and
  Dying Light: The Beast, and the tensor core port stays on all session.
- **Linux and Proton:** fixes for DLSS 5 and RTX 30 frame generation.
- **Windows Defender** should stop flagging the DLL, which is why it is
  bigger now; see [Antivirus warnings](docs/troubleshooting.md#antivirus-warnings).
- **NVIDIA's game list** has 414 titles: War Thunder and Where Winds Meet now
  go up to 6x.

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
