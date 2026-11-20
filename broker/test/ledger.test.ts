// The money: the ledger's rules against real SQLite (a Durable Object's storage) and a TURN API
// whose answers each test chooses.
import { env } from "cloudflare:workers";
import { runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";

import { budgetConfig } from "../src/budget";
import { LedgerBook } from "../src/ledger";
import type { Generated, TurnApi } from "../src/turn";

const GB = 1e9;
const NOV = Date.UTC(2026, 10, 20, 12);

class FakeTurn implements TurnApi {
  generated = 0;
  revoked: string[] = [];
  answer: () => Promise<Generated> = async () => ({ kind: "issued", iceServers: [{ urls: "turn:x", username: `u${this.generated}`, credential: "c" }], username: `u${this.generated}` });
  revokes = true;
  async generate(): Promise<Generated> { this.generated++; return this.answer(); }
  async revoke(username: string): Promise<boolean> { this.revoked.push(username); return this.revokes; }
}

// A fresh ledger's storage for each test.
async function withBook(vars: Record<string, string>, run: (book: LedgerBook, api: FakeTurn, clock: { now: number }, sql: SqlStorage) => Promise<void>) {
  const stub = env.LEDGER.get(env.LEDGER.idFromName(crypto.randomUUID()));
  await runInDurableObject(stub, async (_instance, state) => {
    const api = new FakeTurn(), clock = { now: NOV };
    const cfg = budgetConfig({ TURN_RATE_MBPS: "120", TURN_TTL_MAX_S: "14400", TURN_TTL_MIN_S: "600", TURN_GRACE_S: "60", ...vars });
    await run(new LedgerBook(state.storage.sql, api, cfg, () => clock.now), api, clock, state.storage.sql);
  });
}

const sum = (book: LedgerBook) => book.chargedIn("2026-11");

describe("the monthly cap", () => {
  it("never charges past the budget, one request after another", async () => {
    await withBook({ TURN_BUDGET_GB: "500" }, async (book, api) => {
      const issued = [];
      for (let i = 0; i < 20; i++) issued.push(await book.issue("room"));
      expect(sum(book)).toBeLessThanOrEqual(500 * GB);
      // 4 h at 120 Mb/s is 216 GB: two whole credentials, then one cut to what is left (68 GB, 4533 s).
      expect(issued.filter((r) => r.ok).map((r) => r.ok && r.ttl)).toEqual([14400, 14400, 4533]);
      expect(issued.slice(3).every((r) => !r.ok && r.why === "budget")).toBe(true);
      expect(api.generated).toBe(3);
    });
  });

  it("never charges past the budget when requests overlap", async () => {
    await withBook({ TURN_BUDGET_GB: "500" }, async (book, api) => {
      let release!: () => void;
      const gate = new Promise<void>((r) => { release = r; });
      const issue = api.answer;
      api.answer = async () => { await gate; return issue(); };
      const pending = Array.from({ length: 12 }, () => book.issue("room"));
      release();
      const results = await Promise.all(pending);
      expect(results.filter((r) => r.ok)).toHaveLength(3);
      expect(sum(book)).toBeLessThanOrEqual(500 * GB);
    });
  });

  it("charges nothing when Cloudflare refuses, everything when it does not answer clearly", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api) => {
      api.answer = async () => ({ kind: "refused", status: 403 });
      expect(await book.issue("room")).toEqual({ ok: false, why: "turn" });
      expect(sum(book)).toBe(0);
      api.answer = async () => ({ kind: "unknown" });
      expect(await book.issue("room")).toEqual({ ok: false, why: "turn" });
      expect(sum(book)).toBe(216 * GB);
    });
  });

  it("keeps the charge of a request that never came back", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api) => {
      api.answer = async () => { throw new Error("evicted"); };
      await expect(book.issue("room")).rejects.toThrow();
      expect(sum(book)).toBe(216 * GB);
    });
  });

  it("refuses everything with no budget", async () => {
    await withBook({ TURN_BUDGET_GB: "0" }, async (book, api) => {
      expect(await book.issue("room")).toEqual({ ok: false, why: "budget" });
      expect(api.generated).toBe(0);
    });
  });

  it("starts again each month and forgets the months before last", async () => {
    await withBook({ TURN_BUDGET_GB: "216" }, async (book, _api, clock, sql) => {
      expect((await book.issue("room")).ok).toBe(true);
      expect((await book.issue("room")).ok).toBe(false);
      clock.now = Date.UTC(2026, 11, 1, 0, 0, 1);
      expect((await book.issue("room")).ok).toBe(true);
      expect(book.status().month).toBe("2026-12");
      clock.now = Date.UTC(2027, 1, 2);
      expect((await book.issue("room")).ok).toBe(true);
      const months = sql.exec<{ month: string }>("SELECT DISTINCT month FROM lease ORDER BY month").toArray().map((r) => r.month);
      expect(months).toEqual(["2027-02"]);
    });
  });
});

describe("ending a lease", () => {
  it("refunds what a revoked credential did not live", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api, clock) => {
      const r = await book.issue("room");
      if (!r.ok) throw new Error("not issued");
      clock.now += 10 * 60_000;
      expect(await book.end("room", r.lease)).toBe(true);
      expect(api.revoked).toEqual(["u1"]);
      expect(sum(book)).toBe((600 + 60) * 15e6);
    });
  });

  it("keeps the whole charge when the revocation fails", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api, clock) => {
      const r = await book.issue("room");
      if (!r.ok) throw new Error("not issued");
      api.revokes = false;
      clock.now += 60_000;
      expect(await book.end("room", r.lease)).toBe(false);
      expect(sum(book)).toBe(216 * GB);
      api.revokes = true;
      expect(await book.end("room", r.lease)).toBe(false);   // kept for good
      expect(sum(book)).toBe(216 * GB);
    });
  });

  it("only for its own room, once", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api) => {
      const r = await book.issue("room");
      if (!r.ok) throw new Error("not issued");
      expect(await book.end("another room", r.lease)).toBe(false);
      expect(api.revoked).toEqual([]);
      expect(await book.end("room", "no such lease")).toBe(false);
      expect(await book.end("room", r.lease)).toBe(true);
      expect(await book.end("room", r.lease)).toBe(false);
      expect(api.revoked).toEqual(["u1"]);
    });
  });

  it("two ends at once revoke once", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, api) => {
      const r = await book.issue("room");
      if (!r.ok) throw new Error("not issued");
      const both = await Promise.all([book.end("room", r.lease), book.end("room", r.lease)]);
      expect(both.filter(Boolean)).toHaveLength(1);
      expect(api.revoked).toHaveLength(1);
    });
  });

  it("frees budget for the next guest", async () => {
    await withBook({ TURN_BUDGET_GB: "216" }, async (book, _api, clock) => {
      const first = await book.issue("room");
      if (!first.ok) throw new Error("not issued");
      expect((await book.issue("room")).ok).toBe(false);
      clock.now += 30_000;
      await book.end("room", first.lease);
      const second = await book.issue("room");
      expect(second.ok && second.ttl).toBe(14400 - 90);
      expect(sum(book)).toBeLessThanOrEqual(216 * GB);
    });
  });
});

describe("status", () => {
  it("reports the month's budget and live leases", async () => {
    await withBook({ TURN_BUDGET_GB: "900" }, async (book, _api, clock) => {
      await book.issue("room");
      expect(book.status()).toEqual({ month: "2026-11", budgetBytes: 900 * GB, chargedBytes: 216 * GB, remainingBytes: 684 * GB, rateMbps: 120, activeLeases: 1 });
      clock.now += 14400_000;
      expect(book.status().activeLeases).toBe(0);
    });
  });
});
