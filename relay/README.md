# Relay

Passes short messages (position, move result) between the Raid Workshop addons of players in the
same room. One room per map + game instance (in a raid: the squad). Nothing is stored.

A public relay is already running and built into the addon. You only need this folder if you want
to host your own.

## Host your own

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/HermanGz/raid-workshop/tree/main/relay)

Or with Node.js: `npx wrangler login`, then `npx wrangler deploy` in this folder.

Then in game: debug mode -> Squad link -> Advanced -> set the relay address to your
`wss://<name>.<account>.workers.dev`.
