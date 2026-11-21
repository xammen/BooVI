// One share's rendezvous: the host BooVI's socket and its guests' sockets. A guest's text goes
// to the host unread, wrapped with the guest's id and address; the host's replies go back to the
// guest it names. The picture and the pad never pass here: they go peer to peer over WebRTC.
//
// The room lives while its host is connected, at most ROOM_TTL_S. Sockets hibernate between
// messages, so a waiting room costs nothing. A host that leaves without coming back within
// HOST_GONE_MS has its TURN credentials revoked: a crashed or killed host never hands them back.
import { DurableObject } from "cloudflare:workers";

import { randomId } from "./ids";
import type { Issue, TurnLedger } from "./ledger";

export const MAX_GUESTS = 8;
export const MAX_GUEST_MESSAGE = 64 * 1024;
export const GUEST_MESSAGES_PER_MINUTE = 240;
// Long enough for a host's own reconnection, whose relayed guests keep playing meanwhile.
export const HOST_GONE_MS = 180_000;

// Close codes the page and the app read.
export const Closed = {
  replaced: 4000,
  hostLeft: 4404,
  expired: 4408,
  tooLarge: 4413,
  tooMany: 4429,
} as const;

type Attachment =
  | { role: "host"; room: string; entitled: boolean }
  | { role: "guest"; id: string; ip: string; windowMs: number; count: number };

// What the host sends: a reply to one guest, a guest to drop, a TURN request or its end.
type HostMessage =
  | { t: "msg"; g: string; d: string }
  | { t: "close"; g: string }
  | { t: "turn"; n: number }
  | { t: "turn-end"; n: number; lease: string };

type TurnReply = Issue | { ok: false; why: "not-entitled" };

export interface RoomEnv {
  LEDGER: DurableObjectNamespace<TurnLedger>;
  ROOM_TTL_S?: string;
}

function parseHostMessage(text: string): HostMessage | null {
  let m: unknown;
  try { m = JSON.parse(text); } catch { return null; }
  if (typeof m !== "object" || m === null) return null;
  const o = m as Record<string, unknown>;
  switch (o.t) {
    case "msg": return typeof o.g === "string" && typeof o.d === "string" ? { t: "msg", g: o.g, d: o.d } : null;
    case "close": return typeof o.g === "string" ? { t: "close", g: o.g } : null;
    case "turn": return { t: "turn", n: Number(o.n) || 0 };
    case "turn-end": return typeof o.lease === "string" ? { t: "turn-end", n: Number(o.n) || 0, lease: o.lease } : null;
    default: return null;
  }
}

export class Room extends DurableObject<RoomEnv> {
  constructor(ctx: DurableObjectState, env: RoomEnv) {
    super(ctx, env);
    // Keepalives answered without waking the room.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  private host(): WebSocket | undefined { return this.ctx.getWebSockets("host")[0]; }
  private ledger() { return this.env.LEDGER.get(this.env.LEDGER.idFromName("ledger")); }

  // The Worker has checked the room key, the owner key and the rate limits.
  async fetch(request: Request): Promise<Response> {
    const role = request.headers.get("x-boovi-role");
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    if (role === "host") {
      for (const old of this.ctx.getWebSockets("host")) old.close(Closed.replaced, "replaced");
      const attachment: Attachment = { role: "host", room: request.headers.get("x-boovi-room") ?? "", entitled: request.headers.get("x-boovi-entitled") === "1" };
      this.ctx.acceptWebSocket(server, ["host"]);
      server.serializeAttachment(attachment);
      const ttl = Math.max(60, Number(this.env.ROOM_TTL_S) || 43200);
      // The room's expiry, kept across its host's reconnections; a pending sweep gives way to it.
      let expires = await this.ctx.storage.get<number>("expiresMs");
      if (expires === undefined) await this.ctx.storage.put("expiresMs", expires = Date.now() + ttl * 1000);
      await this.ctx.storage.put("room", attachment.room);
      await this.ctx.storage.delete("hostGoneMs");
      await this.ctx.storage.setAlarm(expires);
      server.send(JSON.stringify({ t: "ready", entitled: attachment.entitled, ttl }));
      return new Response(null, { status: 101, webSocket: client });
    }
    const id = randomId(6);
    const attachment: Attachment = { role: "guest", id, ip: request.headers.get("x-boovi-ip") ?? "", windowMs: Date.now(), count: 0 };
    this.ctx.acceptWebSocket(server, ["guest", `g:${id}`]);
    server.serializeAttachment(attachment);
    if (!this.host()) {
      server.send(JSON.stringify({ t: "closed", why: "offline" }));
      server.close(Closed.hostLeft, "host offline");
    } else if (this.ctx.getWebSockets("guest").length > MAX_GUESTS) {
      server.send(JSON.stringify({ t: "closed", why: "busy" }));
      server.close(Closed.tooMany, "too many guests");
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const a = ws.deserializeAttachment() as Attachment;
    if (a.role === "guest") return this.fromGuest(ws, a, message);
    if (typeof message !== "string") return;
    const m = parseHostMessage(message);
    if (!m) return;
    switch (m.t) {
      case "msg": this.ctx.getWebSockets(`g:${m.g}`)[0]?.send(m.d); return;
      case "close": this.ctx.getWebSockets(`g:${m.g}`)[0]?.close(1000, "done"); return;
      case "turn": {
        // A host without the owner key relays nothing through the owner's TURN key: STUN only.
        const reply: TurnReply = a.entitled ? await this.ledger().issue(a.room) : { ok: false, why: "not-entitled" };
        ws.send(JSON.stringify({ t: "turn", n: m.n, ...reply }));
        return;
      }
      case "turn-end": {
        const ok = a.entitled ? await this.ledger().end(a.room, m.lease) : false;
        ws.send(JSON.stringify({ t: "turn-end", n: m.n, ok }));
        return;
      }
    }
  }

  private fromGuest(ws: WebSocket, a: Extract<Attachment, { role: "guest" }>, message: string | ArrayBuffer): void {
    if (typeof message !== "string" || message.length > MAX_GUEST_MESSAGE) { ws.close(Closed.tooLarge, "message too large"); return; }
    const now = Date.now();
    if (now - a.windowMs >= 60_000) { a.windowMs = now; a.count = 0; }
    if (++a.count > GUEST_MESSAGES_PER_MINUTE) { ws.close(Closed.tooMany, "too many messages"); return; }
    ws.serializeAttachment(a);
    const host = this.host();
    if (!host) {
      ws.send(JSON.stringify({ t: "closed", why: "offline" }));
      ws.close(Closed.hostLeft, "host offline");
      return;
    }
    try { host.send(JSON.stringify({ t: "msg", g: a.id, ip: a.ip, d: message })); } catch { /* the host is leaving */ }
  }

  async webSocketClose(ws: WebSocket, code: number, reason: string): Promise<void> {
    const a = ws.deserializeAttachment() as Attachment;
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, reason); } catch { /* already closed */ }
    // A host replaced by its own reconnection leaves the room open.
    if (a.role === "host" && !this.ctx.getWebSockets("host").some((s) => s !== ws)) await this.hostGone(Closed.hostLeft, "host left");
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.webSocketClose(ws, 1011, "error");
  }

  // The room's expiry while its host is there; the sweep of its credentials once it is gone.
  async alarm(): Promise<void> {
    if (this.host()) { await this.hostGone(Closed.expired, "room expired"); return; }
    const gone = await this.ctx.storage.get<number>("hostGoneMs");
    const room = await this.ctx.storage.get<string>("room");
    if (gone === undefined || !room) return;
    if (Date.now() - gone < HOST_GONE_MS) { await this.ctx.storage.setAlarm(gone + HOST_GONE_MS); return; }
    await this.ctx.storage.delete("hostGoneMs");
    await this.ledger().endRoom(room);
  }

  private async hostGone(code: number, reason: string): Promise<void> {
    for (const ws of this.ctx.getWebSockets()) {
      try { ws.send(JSON.stringify({ t: "closed", why: code === Closed.expired ? "expired" : "offline" })); } catch { /* closing */ }
      try { ws.close(code, reason); } catch { /* closed */ }
    }
    const now = Date.now();
    await this.ctx.storage.delete("expiresMs");
    await this.ctx.storage.put("hostGoneMs", now);
    await this.ctx.storage.setAlarm(now + HOST_GONE_MS);
  }
}
