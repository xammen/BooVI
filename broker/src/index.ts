// BooVI's share broker. Static assets serve the guest page at /<room id>; the code below only
// answers /api/*:
//   GET /api/host/<room>   the host BooVI's socket. Authorization: Bearer <room key>, and
//                          X-Boovi-Owner: <owner key> for the owner's TURN relay.
//   GET /api/room/<room>   a guest's socket (the page, or a BooVI client).
//   GET /api/turn          the month's TURN budget, for the owner key's holder.
//   GET /api/legacy        the room an old https://<host>/?a=… link opens (LEGACY_ROOM).
// Share secrets never come here: they ride in the link's fragment and travel to the host
// inside the room's messages, which the Worker relays without reading.
import { budgetConfig } from "./budget";
import { isRoomId, isRoomKey, roomIdOf, sameSecret } from "./ids";
import { type LedgerEnv, TurnLedger } from "./ledger";
import { Room } from "./room";

export { Room, TurnLedger };

export interface Env extends LedgerEnv {
  ASSETS: Fetcher;
  ROOM: DurableObjectNamespace<Room>;
  LEDGER: DurableObjectNamespace<TurnLedger>;
  JOIN_LIMIT: RateLimit;
  HOST_LIMIT: RateLimit;
  OWNER_KEY?: string;
  ROOM_TTL_S?: string;
  LEGACY_ROOM?: string;
}

const json = (status: number, body: unknown): Response =>
  new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });

const isOwner = (request: Request, env: Env): boolean =>
  !!env.OWNER_KEY && sameSecret(request.headers.get("X-Boovi-Owner") ?? "", env.OWNER_KEY);

async function limited(limiter: RateLimit, ip: string): Promise<boolean> {
  return !(await limiter.limit({ key: ip })).success;
}

// The room's Durable Object gets the upgrade with only what the Worker vouches for.
function toRoom(request: Request, env: Env, room: string, headers: Record<string, string>): Promise<Response> {
  const stub = env.ROOM.get(env.ROOM.idFromName(room));
  return stub.fetch(new Request(request.url, { headers: { Upgrade: "websocket", "x-boovi-room": room, ...headers } }));
}

async function host(request: Request, env: Env, room: string, ip: string): Promise<Response> {
  if (await limited(env.HOST_LIMIT, ip)) return json(429, { error: "too many rooms" });
  const key = (request.headers.get("Authorization") ?? "").replace(/^Bearer /, "");
  if (!isRoomKey(key) || (await roomIdOf(key)) !== room) return json(403, { error: "not this room's key" });
  return toRoom(request, env, room, { "x-boovi-role": "host", "x-boovi-ip": ip, "x-boovi-entitled": isOwner(request, env) ? "1" : "0" });
}

async function guest(request: Request, env: Env, room: string, ip: string): Promise<Response> {
  if (await limited(env.JOIN_LIMIT, ip)) return json(429, { error: "too many joins" });
  return toRoom(request, env, room, { "x-boovi-role": "guest", "x-boovi-ip": ip });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const [api, kind, room, ...rest] = url.pathname.split("/").filter(Boolean);
    if (api !== "api") return env.ASSETS.fetch(request);
    const ip = request.headers.get("CF-Connecting-IP") ?? "";
    if (kind === "host" || kind === "room") {
      if (!room || !isRoomId(room) || rest.length) return json(404, { error: "no such room" });
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return json(426, { error: "websocket only" });
      return kind === "host" ? host(request, env, room, ip) : guest(request, env, room, ip);
    }
    if (kind === "turn" && !room) {
      if (!isOwner(request, env)) return json(403, { error: "owner key required" });
      const ledger = env.LEDGER.get(env.LEDGER.idFromName("ledger"));
      return json(200, { ...(await ledger.status()), config: budgetConfig(env) });
    }
    if (kind === "legacy" && !room) {
      return env.LEGACY_ROOM && isRoomId(env.LEGACY_ROOM) ? json(200, { room: env.LEGACY_ROOM }) : json(404, { error: "no legacy room" });
    }
    return json(404, { error: "not found" });
  },
} satisfies ExportedHandler<Env>;
