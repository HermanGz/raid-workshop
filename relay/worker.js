// Raid Workshop relay: passes short text lines between the addons of players in the same room.
// One Durable Object per room. Nothing is stored; when everyone leaves, the room is gone.
export class Room {
  constructor(state) { this.state = state; }
  async fetch(request) {
    const pair = new WebSocketPair();
    this.state.acceptWebSocket(pair[1]);          // hibernation API: cheap while idle
    return new Response(null, { status: 101, webSocket: pair[0] });
  }
  webSocketMessage(ws, msg) {
    if (typeof msg !== "string" || msg.length > 2000) return;
    for (const other of this.state.getWebSockets()) {
      if (other !== ws) { try { other.send(msg); } catch (e) {} }
    }
  }
  webSocketClose(ws) { try { ws.close(); } catch (e) {} }
  webSocketError(ws) { try { ws.close(); } catch (e) {} }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const m = url.pathname.match(/^\/room\/([A-Za-z0-9_-]{1,80})$/);
    if (!m || request.headers.get("Upgrade") !== "websocket") {
      return new Response("Raid Workshop relay is running.", { status: 200 });
    }
    const id = env.ROOMS.idFromName(m[1]);
    return env.ROOMS.get(id).fetch(request);
  },
};
