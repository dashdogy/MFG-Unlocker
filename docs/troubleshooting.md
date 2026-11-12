[README](../README.md) · [Install](install.md) · [Using the menu](menu.md) · [Game compatibility](compatibility.md) · [OptiScaler](optiscaler.md) · [XeMFG](xemfg.md) · **Troubleshooting**

# Troubleshooting

Compatibility varies by game; artifacts, freezes, black screens and crashes
remain possible. Check [Game compatibility](compatibility.md) for the filename
that works in your game. Fully close the game before changing any files.

## The menu does not appear

- Check that the DLL is beside the actual game executable, not the launcher.
- Check the filename. If no `RTXMFG-*.log` appeared beside the executable (or
  in `%TEMP%`), the game did not load that name; see
  [Choosing a filename](compatibility.md#choosing-a-filename).
- Remove any leftover older mod files; see [Upgrade](install.md#upgrade).
- With OptiScaler, MFG Unlocker's settings are in the **MFG Unlocker** tab of
  OptiScaler's menu, or its menu shows beside OptiScaler's only in some cases;
  see [OptiScaler](optiscaler.md#menus).

## Frozen picture

For frozen presentation, some users reported that **Preset B** helped in
[issue #6](https://github.com/dashdogy/MFG-Unlocker/issues/6). On RTX 30,
restart after changing presets. This is a reported workaround, not a fix for
every game.

## The game hides Frame Generation on RTX 30 or RTX 20

Some games remove their DLSS Frame Generation option on RTX 30 and RTX 20
cards. Tick **Game FG option: Unlock** in the menu; see
[Game FG option (RTX 30 and RTX 20)](menu.md#game-fg-option-rtx-30-and-rtx-20).

## RTX 30 pacing

RTX 30 DLSS Frame Generation uses the driver's flip metering by default. To go
back to the V1.5.0 pacing, set `Rtx30FlipMetering=0` under `[Compatibility]` in
`RTX40MFG-UI.ini` beside the game executable, then restart the game.

## Games with anti-cheat

MFG Unlocker is not supported in games with anti-cheat, so don't install it in
them; see [Games with anti-cheat](compatibility.md#games-with-anti-cheat).

## Reporting a bug

Open a [bug report](https://github.com/dashdogy/MFG-Unlocker/issues) with:

- the game, GPU and driver;
- the proxy filename (or OptiScaler plugin);
- the multiplier and preset;
- `RTXMFG-<game>-<path-hash>.log` and `RTXMFG-Universal.status.json`;
- with OptiScaler, also `OptiScaler.log`.

The log and status file are in the game folder, beside the game's executable
and the mod settings. If that folder cannot be written, the log goes to
`%TEMP%` instead. Each launch keeps the previous runtime log as
`RTXMFG-<game>-<path-hash>.previous.log` and creates a new log. The status file
is replaced on each launch, then rewritten when the mod's state changes and
every 30 seconds.

The log is on by default. To stop writing it, untick **Write log file** under
**Diagnostics** in the menu; with OptiScaler, it is at the bottom of MFG
Unlocker's tab in OptiScaler's menu. It is saved as `[Diagnostics] Logging=0`
in `RTX40MFG-UI.ini` beside the game. The change applies at the next game
start. Tick it again before you report a bug.

For a game that does not work at all, use the
[game compatibility report](https://github.com/dashdogy/MFG-Unlocker/issues/new?template=game-compatibility.yml)
instead.
