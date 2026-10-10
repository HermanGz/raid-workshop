# Raid Workshop

Player-made challenges on top of Guild Wars 2 raid fights. The fight itself is never changed:
the addon only draws on screen and checks your own position.

## Bosses

| Boss | Challenge | Folder |
|---|---|---|
| Mursaat Overseer (CM) | **Chess**: a chess board on the floor, a piece per player, a move every 10 s, wrong move or shared square = death | [`bosses/mursaat-overseer`](bosses/mursaat-overseer) |

## Install the addon

1. Install Nexus (https://raidcore.gg/Nexus).
2. Download `RaidWorkshop.dll` from this repo's Releases and put it in `Guild Wars 2/addons/`.
3. In game: `CTRL+O` -> Addons -> load Raid Workshop. `ALT+SHIFT+B` opens the panel.

Nothing else to set up: while the board is on, the addon links up by itself with everyone in the
same map instance (in a raid: your squad).

## Repo layout

| Folder | What |
|---|---|
| `addon/` | The Nexus addon (C++), shared by all bosses. Build with Visual Studio 2022 or CMake. |
| `bosses/<boss>/` | Everything for one fight: rules, data, browser prototype. |
| `relay/` | The small server that passes positions between squad members (already running). |

Use at your own risk: third-party tools are not supported by ArenaNet.
