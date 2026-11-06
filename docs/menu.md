[README](../README.md) · [Install](install.md) · **Using the menu** · [Game compatibility](compatibility.md) · [OptiScaler](optiscaler.md) · [XeMFG](xemfg.md) · [Troubleshooting](troubleshooting.md)

# Using the menu

Press **Backspace** to open or close the menu. The hotkey can be changed there
(**Change key** in the footer). The first launch menu opens when the game
window is foreground and does not reopen automatically after it has been shown
and saved.

The menu has a **Frame generation** tab and, when NVIDIA's DLSS 5 dll is
present, a [DLSS 5](#dlss-5) tab. Each tab has a **Status** section; check it
for what is applied, pending changes or restart instructions.

With OptiScaler, OptiScaler's menu owns the frame generation controls; see
[OptiScaler](optiscaler.md#menus).

## Frame generation

**MFG mode** sets the multiplier. The default, **Follow game**, keeps the
game's requested multiplier; you can also choose a fixed multiplier, or
**Dynamic** when available. Dynamic's **Dynamic target** is the **Display
refresh rate** or a custom FPS value. Dynamic MFG is unavailable in Vulkan
games. On RTX 50 the menu offers the modes the game's DLSS Frame Generation
reports, up to NVIDIA's per game maximum.

**XeMFG standalone** runs XeSS Frame Generation instead; see
[XeMFG standalone](xemfg.md#xemfg-standalone).

### DLSS-G preset

Choose **Game / driver default**, **A**, **B**, or **Preset C (performance)**
where supported. A new preset applies while Frame Generation runs (it restarts
Frame Generation once) on RTX 50, RTX 40, RTX 30 and RTX 20 in DirectX 12
games. Vulkan games, and DLSS-G versions that only have their original preset,
need a game restart.

## DLSS Super Resolution and Ray Reconstruction presets

When the game runs **DLSS Super Resolution**, the menu shows it and lets you
pick its **SR preset** (J, K, L or M); with **Ray Reconstruction** the same
section offers an **RR preset** of D, E or F (F needs NVIDIA driver 580 or
newer). A new preset applies right away. **Game / driver default** leaves the
choice to the game and the NVIDIA app. DirectX 12 games, without OptiScaler.

## Latency and display

### V-Sync

**V-Sync: Off** presents the game's frames without V-Sync, for games that keep
V-Sync on with Frame Generation. **Game** keeps the game's setting. NVIDIA
Control Panel V-Sync settings take priority. In Vulkan games, Off applies when
the game next creates its swap chain.

### Reflex FPS limit

**Reflex FPS limit** provides a separate cap where supported. The manual cap is
unavailable while Dynamic is selected or active.

## Game FG option (RTX 30 and RTX 20)

Some games remove their DLSS Frame Generation option on RTX 30 and RTX 20 cards even
though MFG Unlocker supports it (for example Wuthering Waves, where the option
then appears but Frame Generation does not start yet). To show it, tick
**Game FG option: Unlock** in the menu's Frame generation section, then restart
the game. The choice is saved in `RTX40MFG-UI.ini` beside the game executable
(`GpuNameOverride` under `[Compatibility]`).

The game then sees an RTX 30 or RTX 20 card as an NVIDIA GeForce RTX 4090, so its
recommended graphics settings may assume one. DLSS, Streamline, the driver and
MFG Unlocker keep using your real GPU. The setting is off by default and does
nothing on other GPUs. MFG Unlocker is not supported in games with
[anti-cheat](compatibility.md#games-with-anti-cheat).

## DLSS 5

With NVIDIA's DLSS 5 dll (`nvngx_dlssnr.dll`) beside the game's executable or
MFG Unlocker, a **DLSS 5** tab applies DLSS 5 in games that run DLSS Super
Resolution, DLAA or Ray Reconstruction. MFG Unlocker does not include or
download the dll, and the tab only appears when one is there.

**The dll must be a version that supports your GPU.** DLSS 5 does not run with
an `nvngx_dlssnr.dll` that does not support your card. If you are looking for
one, the RenoDX Discord server is a good place to start.

### Neural Rendering

**Hook Method** sets where DLSS 5 runs. **Before upscaling (faster)**
processes the DLSS input at render resolution, **After upscaling** processes
the DLSS output, and **Present** processes the final image, game UI included.
**Auto** picks one and shows which, and **Off** turns DLSS 5 off.

**Look** sets Intensity, Model, Structure, Tone and Faces & skin together:
**Balanced** is the provider's own default, **Subtle** halves its change and
**Strong** goes further. The list shows **Custom** when you have changed those
values yourself. **Intensity** is the strength of the whole effect, **Model**
chooses Model A, B or C, and **Faces & skin** sets structure enhancement on
detected skin and faces; its **Same as Structure** takes the Structure
strength, the provider's own default.

**Resolution Scale** is the share of the hook method's resolution that the
network processes. **Auto** lowers it when DLSS 5 takes more than about a
third of each rendered frame, never below 75 %, and raises it again with
headroom, never above the slider. **Half rate** runs the network on every
other frame for about half the GPU cost (DirectX 12, Before or After
upscaling).

In Vulkan games DLSS 5 runs After upscaling or Auto. With OptiScaler, the
DLSS 5 tab is in MFG Unlocker's menu beside OptiScaler's; see
[OptiScaler](optiscaler.md#menus).

### Compare

**Split view** shows the original image left of the line and DLSS 5 to its
right, and **Split position** moves the line. Before upscaling draws no line,
because the upscaler would treat it as part of the image.

While the **Hold to show original** key is held, the game shows its original
image, so DLSS 5 can be compared in motion. DLSS 5 keeps running, and the key
still reaches the game.

### Status and Advanced

The Status section shows whether DLSS 5 is running, its **GPU cost**, the
**Half rate** cost, where **Auto scale** sits, and the **Route**.

**Advanced** holds the rest, including Structure and Tone Intensity, Character
Mask, Pass Count (running the network several times in a row, at extra GPU
cost) and blending.

## Language

The **language** list in the footer, beside the menu key, sets the menu
language. **Auto** follows the Windows display language. Simplified Chinese
(简体中文), Ukrainian (Українська), Italian (Italiano), Swedish (Svenska),
Turkish (Türkçe), Vietnamese (Tiếng Việt) and French (Français) are included;
diagnostics stay in English. To add a translation, see
[source/native/locale](../source/native/locale/README.md).

## Settings files

MFG Unlocker keeps its files beside the game's executable.
`RTXMFG-Universal.json` holds the menu settings; keep it when uninstalling if
you want to retain them. `RTX40MFG-UI.ini` holds compatibility switches under
`[Compatibility]`, such as `GpuNameOverride` (above) and `Rtx30FlipMetering`
([Troubleshooting](troubleshooting.md#rtx-30-pacing)).
`RTXMFG-<game>-<path-hash>.log` and `RTXMFG-Universal.status.json` are
diagnostics; see [Reporting a bug](troubleshooting.md#reporting-a-bug).
