[README](../README.md) · [Install](install.md) · [Using the menu](menu.md) · **Game compatibility** · [OptiScaler](optiscaler.md) · [XeMFG](xemfg.md) · [Troubleshooting](troubleshooting.md)

# Game compatibility

Which filename to give MFG Unlocker in each game, and what players have
reported. The reports come from GitHub issues, tester logs and our own test
installs. A game patch can change which names it loads, so if the listed name
stops working, try the next one and [tell us](#reporting-a-game).

## GPUs and graphics APIs

| GPU | DLSS Multi Frame Generation | XeMFG |
|---|---|---|
| RTX 50 | Native in NVIDIA's runtime; MFG Unlocker changes nothing there and adds its menu controls | Yes |
| RTX 40 | Up to 6x, and Dynamic MFG where supported | Yes |
| RTX 30 | Up to 6x, and Dynamic MFG where supported | Yes |
| RTX 20 | Up to 6x, and Dynamic MFG where supported | Yes |
| AMD Radeon | No | Yes |
| Intel Arc | No | [Experimental](xemfg.md#intel-arc) |

DLSS MFG needs a game with a Streamline DLSS Frame Generation integration. RTX
50, RTX 40, RTX 30 and RTX 20 have DirectX 12 and experimental Vulkan paths, and
Dynamic MFG is unavailable on Vulkan. Available modes depend on the GPU, the
active runtime and NVIDIA's per game MFG override maximum. XeMFG runs through
[OptiScaler](optiscaler.md) or, experimentally, as
[XeMFG standalone](xemfg.md#xemfg-standalone) in DirectX 12 and Vulkan games.
In DirectX 11 games, use OptiScaler: MFG Unlocker's menu and XeMFG standalone
need DirectX 12 or Vulkan.

## Choosing a filename

1. Put the DLL beside the **game's real executable**, not the launcher. In
   Unreal Engine games that is usually `<Project>\Binaries\Win64`. For
   Microsoft Store and Game Pass copies it is the `Content` folder under
   `XboxGames\<Game>`.
2. Try **`version.dll`** first, then **`winmm.dll`**, then the other
   [supported filenames](install.md#supported-filenames).
3. Never overwrite a DLL the game or another mod already uses. These names are
   often taken:

   | Filename | Often used by |
   |---|---|
   | `version.dll` | Cyber Engine Tweaks (Cyberpunk 2077), Ultimate ASI Loader |
   | `winmm.dll` | RED4ext (Cyberpunk 2077), Crimson Desert's mod loader; Phantom Blade Zero ships its own |
   | `dinput8.dll` | REFramework (Monster Hunter Wilds, Dragon's Dogma 2, Resident Evil Requiem and other RE Engine games) |
   | `dxgi.dll` | ReShade, OptiScaler, Special K |
   | `dsound.dll` | FF7RebirthFix and some ASI loaders |

4. Start the game and press **Backspace**. If the menu does not appear and no
   `RTXMFG-*.log` was written beside the executable (or in `%TEMP%`), the game
   did not load that name: close it, rename the DLL and try again.

A game with its own `dxgi.dll` mod (ReShade, Special K) can hide MFG Unlocker's
menu or stop XeMFG standalone even when the name is right.

## Games

**Use** is the name with the most reliable working reports. **Also loaded**
names worked for some players. **Don't use** names were not loaded, or clash
with the game or a common mod.

| Game | Use | Also loaded | Don't use |
|---|---|---|---|
| Ace Combat 8 | `version.dll` | | |
| AION 2 | `d3d12.dll` | `dxgi.dll` | |
| Alan Wake 2 | `winmm.dll` | `dinput8.dll` | `version.dll` (Epic) |
| Assassin's Creed IV Black Flag Resynced | `version.dll` | `dinput8.dll` | |
| Black Myth: Wukong | `version.dll` | `dsound.dll`, `dxgi.dll` | |
| Call of Duty: Modern Warfare 4 | `dxgi.dll` | | |
| Control Resonant | `version.dll` | `winmm.dll`, `bink2w64.dll` ([Bink route](install.md#supported-filenames)) | |
| Crimson Desert | `d3d12.dll` | | `version.dll`, `dinput8.dll` (not loaded); `winmm.dll` (game's mod loader) |
| Cyberpunk 2077 | `xinput9_1_0.dll` | `dxgi.dll`, `d3d12.dll` | `version.dll` (CET), `winmm.dll` (RED4ext), `dinput8.dll` (not loaded) |
| DOOM: The Dark Ages (Vulkan) | `winmm.dll` | | |
| Dragon's Dogma 2 | `winmm.dll` | | |
| Dying Light 2 | `winmm.dll` | `version.dll` | |
| Dying Light: The Beast | `winmm.dll` | | |
| Enshrouded (Vulkan) | `winmm.dll` | | |
| Farming Simulator 25 | `dinput8.dll` | `version.dll` | |
| Final Fantasy VII Rebirth | `version.dll` | `dxgi.dll` | `dsound.dll` if FF7RebirthFix uses it |
| Gothic 1 Remake | `version.dll` | `winmm.dll` | |
| Hogwarts Legacy | `version.dll` | | |
| Indiana Jones and the Great Circle (Vulkan) | `dinput8.dll` | | |
| Marvel's Spider-Man 2 | `version.dll` | | |
| Microsoft Flight Simulator 2020 | `version.dll` (Steam), `dinput8.dll` (Store) | | |
| Microsoft Flight Simulator 2024 | `version.dll` | `dinput8.dll`, `d3d12.dll` | |
| Monster Hunter Wilds | `version.dll` | `winmm.dll` | `dinput8.dll` (REFramework) |
| Onimusha: Way of the Sword | `winmm.dll` | | `version.dll` (crash) |
| Phantom Blade Zero | `version.dll` | `dxgi.dll` | `winmm.dll` (the game's own) |
| Planet Zoo 2 | `winmm.dll` | | `dxgi.dll` (breaks HDR) |
| Pragmata | `version.dll` | | |
| Resident Evil Requiem | `version.dll` | `dxgi.dll` | `dinput8.dll` (REFramework) |
| S.T.A.L.K.E.R. 2 | `version.dll` | `d3d12.dll` | `dxgi.dll` (not loaded) |
| Silent Hill: Townfall | `version.dll` | | |
| Star Wars: Galactic Racer | `version.dll` | `dxgi.dll` | |
| Subnautica 2 | `version.dll` | | |
| The Blood of Dawnwalker | `version.dll` | | |
| The First Descendant | `dxgi.dll` | | |
| The Last of Us Part II | `dinput8.dll` | | |
| The Witcher 3 | `winmm.dll` | `version.dll`, `d3d11.dll` | `dxgi.dll` if ReShade uses it |
| Where Winds Meet | `winmm.dll` | | |
| Wuthering Waves | `version.dll` | `dxgi.dll` | `dinput8.dll`, `winmm.dll`, `d3d12.dll` (no menu on V1.3) |

### Through OptiScaler

These games have run MFG Unlocker as an [OptiScaler plugin](optiscaler.md)
(the menu or frame generation reached the game in a tester log or report). The
filename here is OptiScaler's, which its own documentation covers.

007 First Light, Arknights: Endfield, Clair Obscur: Expedition 33, Crimson
Desert, Cyberpunk 2077, DOOM: The Dark Ages, Dying Light 2, Enshrouded,
Forza Motorsport, GTA: The Trilogy (San Andreas, Vice City), Hitman World of
Assassination, Marvel's Spider-Man Remastered, Neverness to Everness, No Man's
Sky, No Rest for the Wicked, Red Dead Redemption 2, Resident Evil Requiem,
Silent Hill: Townfall, Subnautica 2, Tainted Grail: The Fall of Avalon,
The Ascent, The Last of Us Part II, The Witcher 3, X4: Foundations.

Arknights: Endfield, DOOM: The Dark Ages and Red Dead Redemption 2 need
[our OptiScaler build](optiscaler.md).

## The Witcher 3: path traced hair

**Path traced HairWorks on RTX 40.** A hair compatibility path built into the
mod, for supported Witcher 3 DirectX 12 builds, retains the game's hair
materials and shading while bringing animated strands into its path traced
lighting.

1. Install MFG Unlocker as **`winmm.dll`** beside `witcher3.exe` in
   `bin\x64_dx12`.
2. Enable the game's HairWorks, path tracing and **Path Traced Hair**
   settings; MFG Unlocker follows those settings automatically.
3. **Fully restart the game after changing Path Traced Hair or Frame
   Generation.**

The mod menu's **Path traced hair** line shows its activity or why it is
unavailable. Support is experimental and may stop working after a game patch.

## Games with anti-cheat

MFG Unlocker is not supported in any game with anti-cheat, so don't install it
in one. Players have already reported anti-cheat trouble in Battlefield 6 /
RedSec, Forza Horizon 6, Gears of War: E-Day, Grand Theft Auto V Enhanced and
Zenless Zone Zero ([#28], [#31], [#52], [#57], [#62], [#74]).

## Reporting a game

Open a [game compatibility report](https://github.com/dashdogy/MFG-Unlocker/issues/new?template=game-compatibility.yml),
or add a line to [#3]. Include the game and store, the **filename you used**
(or OptiScaler plugin), the folder, your GPU, the MFG Unlocker version, and
whether the menu appeared and frame generation ran. A name that did *not* load
is just as useful.

[#3]: https://github.com/dashdogy/MFG-Unlocker/issues/3
[#28]: https://github.com/dashdogy/MFG-Unlocker/issues/28
[#31]: https://github.com/dashdogy/MFG-Unlocker/issues/31
[#52]: https://github.com/dashdogy/MFG-Unlocker/issues/52
[#57]: https://github.com/dashdogy/MFG-Unlocker/issues/57
[#62]: https://github.com/dashdogy/MFG-Unlocker/issues/62
[#74]: https://github.com/dashdogy/MFG-Unlocker/issues/74
