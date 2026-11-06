[README](../README.md) · **Install** · [Using the menu](menu.md) · [Game compatibility](compatibility.md) · [OptiScaler](optiscaler.md) · [XeMFG](xemfg.md) · [Troubleshooting](troubleshooting.md)

# Install

The download contains **`MFGUnlocker.dll`**. The menu is built in; no separate
ReShade or external loader installation is needed. To run MFG Unlocker inside
OptiScaler instead, install it as an OptiScaler plugin; see
[OptiScaler](optiscaler.md).

## Steps

1. Close the game. If upgrading, remove the old mod DLL first, as described
   under [Upgrade](#upgrade).
2. Download `MFGUnlocker-v1.7.0.zip` from
   [Releases](https://github.com/dashdogy/MFG-Unlocker/releases) and extract it.
3. Rename `MFGUnlocker.dll` to **one** [supported filename](#supported-filenames)
   that the game loads early. [Game compatibility](compatibility.md) lists the
   name that works in each game.
4. Place it beside the **actual game executable**, not the launcher.
5. Launch the game, enable Frame Generation where available, and press
   **Backspace** to open the menu. On first launch it opens automatically.

Install only one copy. Do not overwrite a DLL belonging to the game or another
mod; choose another suitable name if it is already occupied. Leaving the file
named `MFGUnlocker.dll` does not make the game load it automatically.

## Supported filenames

```text
version.dll
dinput8.dll
winmm.dll
d3d9.dll
d3d10.dll
d3d11.dll
d3d12.dll
dxgi.dll
dsound.dll
wininet.dll
winhttp.dll
binkw64.dll
bink2w64.dll
xinput1_1.dll
xinput1_2.dll
xinput1_3.dll
xinput1_4.dll
xinput9_1_0.dll
xinputuap.dll
```

The correct name depends on the game. A supported name must actually be loaded
before Frame Generation starts; Vulkan games may not load DirectX proxy names.
If you are not sure which one to use, see
[Choosing a filename](compatibility.md#choosing-a-filename).

### Bink games

Back up the game's original DLL first, then rename `binkw64.dll` to
`binkw64Hooked.dll`, or `bink2w64.dll` to `bink2w64Hooked.dll`. Give MFG
Unlocker the original filename and keep both files beside the executable. These
routes require the game's original Bink DLL.

## Upgrade

From v1.4 or another single DLL release: close the game, delete the installed
`RTXMFG.dll` (or the name you gave it), and install `MFGUnlocker.dll` under the
same filename. Settings and logs keep their `RTXMFG` file names, so your
preferences carry over.

When upgrading from v1.2, move `RTX40MFGCore.dll`, `RTX40MFG.asi`, and
`RTX40MFG-UI.addon64` to a backup outside the game's loading directories.
Also remove older Universal/Bridge mod components or corresponding RTX 30 split
files, if installed. Preserve unrelated mods and their loaders.

## Uninstall

Close the game and remove the renamed MFG Unlocker DLL, or `MFGUnlocker.asi`
from OptiScaler's plugins folder. Restore any original DLL you renamed to a
`Hooked` filename. Keep `RTXMFG-Universal.json` if you want to retain your
settings. The `RTXMFG-*.log` files in the game folder are diagnostics only and
can be deleted.
