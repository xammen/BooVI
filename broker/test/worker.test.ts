// The broker as a host and a guest see it: rooms, relaying, entitlement, expiry, limits.
import { env, exports } from "cloudflare:workers";
import { runDurableObjectAlarm, runInDurableObject } from "cloudflare:test";
import { afterEach, describe, expect, it } from "vitest";

import { base64url, roomIdOf } from "../src/ids";

const OWNER = "owner-key-for-tests";
const open: WebSocket[] = [];

interface Socket {
  ws: WebSocket;
  next(): Promise<unknown>;
  closed: Promise<{ code: number; reason: string }>;
  send(m: unknown): void;
}

let ips = 0;
const freshIp = () => `198.51.100.${++ips % 250}`;

async function connect(path: string, headers: Record<string, string> = {}): Promise<Socket | Response> {
  const response = await exports.default.fetch(`https://broker.test${path}`, { headers: { Upgrade: "websocket", "CF-Connecting-IP": freshIp(), ...headers } });
  const ws = response.webSocket;
  if (!ws) return response;
  ws.accept();
  open.push(ws);
  const queue: unknown[] = [], waiting: ((m: unknown) => void)[] = [];
  ws.addEventListener("message", (e) => {
    const m = typeof e.data === "string" ? e.data : "";
    let v: unknown = m;
    try { v = JSON.parse(m); } catch { /* raw text */ }
    const w = waiting.shift();
    if (w) w(v); else queue.push(v);
  });
  const closed = new Promise<{ code: number; reason: string }>((resolve) => ws.addEventListener("close", (e) => resolve({ code: e.code, reason: e.reason })));
  return {
    ws, closed,
    next: () => queue.length ? Promise.resolve(queue.shift()) : new Promise((r) => waiting.push(r)),
    send: (m) => ws.send(typeof m === "string" ? m : JSON.stringify(m)),
  };
}

async function newRoom() {
  const key = base64url(crypto.getRandomValues(new Uint8Array(33)));
  return { key, id: await roomIdOf(key) };
}

async function host(owner = false) {
  const room = await newRoom();
  const s = await connect(`/api/host/${room.id}`, { Authorization: `Bearer ${room.key}`, ...(owner ? { "X-Boovi-Owner": OWNER } : {}) });
  if (s instanceof Response) throw new Error(`host refused ${s.status}`);
  const ready = await s.next();
  return { ...s, room, ready };
}

async function guest(room: string, ip?: string) {
  const s = await connect(`/api/room/${room}`, ip ? { "CF-Connecting-IP": ip } : {});
  if (s instanceof Response) throw new Error(`guest refused ${s.status}`);
  return s;
}

async function fake(mode: Record<string, string> = {}) {
  await fetch("https://fake.control/mode", { method: "POST", body: JSON.stringify(mode) });
}
async function fakeCalls(): Promise<{ path: string; auth: string; body: { ttl: number } | null }[]> {
  return (await fetch("https://fake.control/calls")).json();
}

afterEach(() => {
  for (const ws of open.splice(0)) { try { ws.close(1000); } catch { /* closed */ } }
});

describe("a room", () => {
  it("opens only with the key its id is made from", async () => {
    const room = await newRoom(), other = await newRoom();
    const wrong = await connect(`/api/host/${room.id}`, { Authorization: `Bearer ${other.key}` });
    expect(wrong instanceof Response && wrong.status).toBe(403);
    const none = await connect(`/api/host/${room.id}`);
    expect(none instanceof Response && none.status).toBe(403);
    const h = await host();
    expect(h.ready).toEqual({ t: "ready", entitled: false, ttl: 43200 });
  });

  it("knows the owner by the owner key, and nobody else", async () => {
    expect((await host(true)).ready).toMatchObject({ entitled: true });
    const room = await newRoom();
    const s = await connect(`/api/host/${room.id}`, { Authorization: `Bearer ${room.key}`, "X-Boovi-Owner": "owner-key-for-testz" });
    if (s instanceof Response) throw new Error("refused");
    expect(await s.next()).toMatchObject({ entitled: false });
  });

  it("refuses malformed ids and plain requests", async () => {
    const bad = await connect("/api/room/short");
    expect(bad instanceof Response && bad.status).toBe(404);
    const h = await host();
    const plain = await exports.default.fetch(`https://broker.test/api/room/${h.room.id}`);
    expect(plain.status).toBe(426);
  });

  it("relays a guest's text to the host unread, and the host's reply to that guest only", async () => {
    const h = await host();
    const a = await guest(h.room.id, "203.0.113.7"), b = await guest(h.room.id);
    const secret = JSON.stringify({ id: 1, path: "/session", body: { token: "fragment-secret" } });
    a.send(secret);
    const got = await h.next() as { t: string; g: string; ip: string; d: string };
    expect(got).toMatchObject({ t: "msg", ip: "203.0.113.7", d: secret });
    h.send({ t: "msg", g: got.g, d: JSON.stringify({ id: 1, status: 200, body: "{}" }) });
    expect(await a.next()).toEqual({ id: 1, status: 200, body: "{}" });
    b.send("ping-from-b");
    const fromB = await h.next() as { g: string };
    expect(fromB.g).not.toBe(got.g);
  });

  it("tells a guest the host is not there", async () => {
    const room = await newRoom();
    const g = await guest(room.id);
    expect(await g.next()).toEqual({ t: "closed", why: "offline" });
    expect((await g.closed).code).toBe(4404);
  });

  it("closes with its host", async () => {
    const h = await host();
    const g = await guest(h.room.id);
    h.ws.close(1000, "bye");
    expect(await g.next()).toEqual({ t: "closed", why: "offline" });
    expect((await g.closed).code).toBe(4404);
  });

  it("stays open when its host reconnects", async () => {
    const h = await host();
    const g = await guest(h.room.id);
    const again = await connect(`/api/host/${h.room.id}`, { Authorization: `Bearer ${h.room.key}` });
    if (again instanceof Response) throw new Error("refused");
    await again.next();
    expect((await h.closed).code).toBe(4000);
    g.send("still here");
    expect(await again.next()).toMatchObject({ t: "msg", d: "still here" });
  });

  it("expires at its hard TTL", async () => {
    const h = await host();
    const g = await guest(h.room.id);
    expect(await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(h.room.id)))).toBe(true);
    expect(await g.next()).toEqual({ t: "closed", why: "expired" });
    expect((await g.closed).code).toBe(4408);
    expect((await h.closed).code).toBe(4408);
  });

  it("drops a guest's oversized message", async () => {
    const h = await host();
    const g = await guest(h.room.id);
    g.send("x".repeat(64 * 1024 + 1));
    expect((await g.closed).code).toBe(4413);
  });

  it("limits joins per address", async () => {
    const h = await host();
    const statuses = [];
    for (let i = 0; i < 31; i++) {
      const s = await connect(`/api/room/${h.room.id}`, { "CF-Connecting-IP": "192.0.2.99" });
      statuses.push(s instanceof Response ? s.status : 101);
      if (!(s instanceof Response)) s.ws.close(1000);
    }
    expect(statuses.slice(0, 30).every((s) => s === 101)).toBe(true);
    expect(statuses[30]).toBe(429);
  });

  it("limits room creation per address", async () => {
    const statuses = [];
    for (let i = 0; i < 11; i++) {
      const room = await newRoom();
      const s = await connect(`/api/host/${room.id}`, { Authorization: `Bearer ${room.key}`, "CF-Connecting-IP": "192.0.2.98" });
      statuses.push(s instanceof Response ? s.status : 101);
    }
    expect(statuses.slice(0, 10).every((s) => s === 101)).toBe(true);
    expect(statuses[10]).toBe(429);
  });
});

describe("turn", () => {
  it("is refused to a host without the owner key, without asking Cloudflare", async () => {
    await fake();
    const h = await host(false);
    h.send({ t: "turn", n: 1 });
    expect(await h.next()).toEqual({ t: "turn", n: 1, ok: false, why: "not-entitled" });
    expect(await fakeCalls()).toEqual([]);
  });

  it("is issued to the owner's host, then revoked when it ends", async () => {
    await fake();
    const h = await host(true);
    h.send({ t: "turn", n: 7 });
    const issued = await h.next() as { ok: boolean; lease: string; ttl: number; iceServers: { username?: string }[] };
    expect(issued).toMatchObject({ t: "turn", n: 7, ok: true, ttl: 6666 });   // 100 GB at 120 Mb/s
    expect(issued.iceServers.some((s) => s.username)).toBe(true);
    h.send({ t: "turn-end", n: 8, lease: issued.lease });
    expect(await h.next()).toEqual({ t: "turn-end", n: 8, ok: true });
    const calls = await fakeCalls();
    expect(calls.map((c) => c.path)).toEqual(["/v1/turn/keys/test-key/credentials/generate-ice-servers", expect.stringMatching(/\/credentials\/u\d+\/revoke$/)]);
    expect(calls[0]).toMatchObject({ auth: "Bearer test-token", body: { ttl: 6666 } });
  });

  it("is revoked once its host has been gone for 3 minutes, not before, not if it came back", async () => {
    await fake();
    const h = await host(true);
    h.send({ t: "turn", n: 1 });
    expect(await h.next()).toMatchObject({ ok: true });
    const stub = env.ROOM.get(env.ROOM.idFromName(h.room.id));
    // killed: no turn-end
    h.ws.close(1000);
    await h.closed;
    await new Promise((r) => setTimeout(r, 50));
    expect(await runDurableObjectAlarm(stub)).toBe(true);    // too early: waits again
    expect((await fakeCalls()).filter((c) => c.path.endsWith("/revoke"))).toEqual([]);
    await runInDurableObject(stub, async (_room, state) => { await state.storage.put("hostGoneMs", Date.now() - 180_001); });
    expect(await runDurableObjectAlarm(stub)).toBe(true);
    expect((await fakeCalls()).filter((c) => c.path.endsWith("/revoke"))).toHaveLength(1);

    // the same room's host back within 3 minutes keeps its credential
    await fake();
    const again = await connect(`/api/host/${h.room.id}`, { Authorization: `Bearer ${h.room.key}`, "X-Boovi-Owner": OWNER });
    if (again instanceof Response) throw new Error("refused");
    await again.next();
    again.send({ t: "turn", n: 2 });
    expect(await again.next()).toMatchObject({ ok: true });
    again.ws.close(1000);
    await again.closed;
    await new Promise((r) => setTimeout(r, 50));
    const back = await connect(`/api/host/${h.room.id}`, { Authorization: `Bearer ${h.room.key}`, "X-Boovi-Owner": OWNER });
    if (back instanceof Response) throw new Error("refused");
    expect(await back.next()).toMatchObject({ t: "ready" });
    await runInDurableObject(stub, async (_room, state) => { expect(await state.storage.get("hostGoneMs")).toBeUndefined(); });
    expect((await fakeCalls()).filter((c) => c.path.endsWith("/revoke"))).toEqual([]);
  });

  it("stops at the cap", async () => {
    await fake();
    const h = await host(true);
    const ledger = env.LEDGER.get(env.LEDGER.idFromName("ledger"));
    const before = await ledger.status();
    let refused = false;
    for (let n = 0; n < 5 && !refused; n++) {
      h.send({ t: "turn", n });
      const r = await h.next() as { ok: boolean; why?: string };
      refused = !r.ok && r.why === "budget";
    }
    expect(refused).toBe(true);
    const after = await ledger.status();
    expect(after.chargedBytes).toBeLessThanOrEqual(after.budgetBytes);
    expect(after.chargedBytes).toBeGreaterThanOrEqual(before.chargedBytes);
  });

  it("lists and settles credentials for the owner only", async () => {
    expect((await exports.default.fetch("https://broker.test/api/turn/leases")).status).toBe(403);
    const list = await exports.default.fetch("https://broker.test/api/turn/leases", { headers: { "X-Boovi-Owner": OWNER } });
    expect(Array.isArray(await list.json())).toBe(true);
    const post = (body: unknown, owner = true) => exports.default.fetch("https://broker.test/api/turn/settle",
      { method: "POST", body: JSON.stringify(body), headers: owner ? { "X-Boovi-Owner": OWNER } : {} });
    expect((await post({ lease: "x", endMs: 0 }, false)).status).toBe(403);
    expect((await post({ lease: 1 })).status).toBe(400);
    expect((await post({ lease: "no such lease", endMs: Date.now() })).status).toBe(409);
  });

  it("reports the budget to the owner only", async () => {
    expect((await exports.default.fetch("https://broker.test/api/turn")).status).toBe(403);
    const r = await exports.default.fetch("https://broker.test/api/turn", { headers: { "X-Boovi-Owner": OWNER } });
    expect(await r.json()).toMatchObject({ budgetBytes: 100e9, rateMbps: 120 });
  });
});

describe("the page", () => {
  it("is served at a room's link", async () => {
    const h = await host();
    const r = await exports.default.fetch(`https://broker.test/${h.room.id}`, { headers: { "Sec-Fetch-Mode": "navigate", Accept: "text/html" } });
    expect(r.status).toBe(200);
    expect(await r.text()).toContain("<html");
  });

  it("has no legacy room unless one is set", async () => {
    expect((await exports.default.fetch("https://broker.test/api/legacy")).status).toBe(404);
  });
});
