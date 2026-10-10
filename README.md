# Raid Workshop

Player-made challenges on top of Guild Wars 2 raid fights. The fight itself is never changed:
the addon only draws on screen and checks your own position.

First encounter: **Mursaat Overseer - Chess** (a chess board on the floor, a piece per player,
a move every 10 seconds, wrong move or shared square = death).

| Folder | What |
|---|---|
| `addon/` | The Nexus addon (C++). Build with Visual Studio 2022 or download the DLL from Releases. |
| `relay/` | Tiny Cloudflare server that lets squad members' addons see each other. |

## Install the addon
1. Install Nexus (https://raidcore.gg/Nexus).
2. Download `RaidWorkshop.dll` from this repo's Releases and put it in `Guild Wars 2/addons/`.
3. In game: `CTRL+O` -> Addons -> load Raid Workshop. `ALT+SHIFT+B` opens the panel.

## Set up the relay (once per squad)
[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/HermanGz/raid-workshop/tree/main/relay)

Then in game, debug mode (`ALT+SHIFT+D`) -> Squad link:
address `wss://raid-relay.<your-subdomain>.workers.dev`, any squad code, Connect.

Use at your own risk: third-party tools are not supported by ArenaNet.
