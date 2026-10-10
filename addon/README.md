# Raid Workshop

A Nexus addon for player-made challenges on top of Guild Wars 2 raid fights. The fight itself is never changed: the addon only draws on screen and checks your own position.

Encounters so far: **Mursaat Overseer - Chess**. Other fights get their own section later.

## Install

1. Nexus `d3d11.dll` in the Guild Wars 2 folder (https://github.com/RaidcoreGG/Nexus/releases). `CTRL+O` opens Nexus.
2. Remove `MOChessCalibration.dll` (and any older `RaidWorkshop.dll`) from `addons/`.
3. Copy `RaidWorkshop.dll` into `addons/` and load **Raid Workshop** in Nexus.

Data lives in `addons/RaidWorkshop/MO/` (`calibration.txt`, `settings.txt`). An old `addons/MOChess/calibration.txt` is read if there is no new one. Without any file the built-in MO board is used.

## Keys (change in Nexus -> Keybinds)

| Key | Action |
|---|---|
| ALT+SHIFT+B | Player panel |
| ALT+SHIFT+D | Debug mode on/off (opens/closes the debug window) |
| ALT+SHIFT+J | Practice: one move now |
| ALT+SHIFT+M / N | Debug: mark / undo a calibration point |

## MO Chess

- The board is off when you arrive. Stand in the **gold circle** for 5 s to turn it on (again to turn it off).
- Go to one of the 10 gold **start squares** (a4-a8, b4-b8). The square you stand on shimmers gold.
- Pull the boss: everyone gets a random piece (the Claim holder the Pawn), first move after 1.5 s, then every 10 s. No hints during a move. At the check, every player's square shows a green ring (good) or a red X (bad) for 1 s. Two players on the same square both fail.
- Not on a start square at the first move = fail. Fail = red flash, then `/gg` if switched on.
- Out of combat for 4 s (wipe, kill, reset) = reset: pieces removed, start squares flash, everyone goes back before the next pull.
- Test without a fight: panel -> **Start test fight**. Debug window -> **Test board here** puts the board under you in any map.

## Debug mode

Calibration (corners, inner corner, beams, mouse marking), grid lines, plates, letters and numbers, markers, your square highlighted, move the gold circle, and the experimental depth probe (looks for the game's depth buffer; draws nothing).

## Build

Visual Studio 2022 (Desktop development with C++): open the folder, x64-Release, Build All. Or `cmake -S . -B build -A x64` then `cmake --build build --config Release`. `prebuilt/RaidWorkshop.dll` is a MinGW cross-build for quick tests.
