// The monthly TURN ledger: one Durable Object (named "ledger") holds every credential the
// Worker issued, so the cap is counted in one place, strongly consistent.
//
// A lease is charged before its credential exists (pending), and keeps that charge unless
// Cloudflare answers that no credential was made, or confirms its revocation (ended). Whatever
// stopped halfway (a crash, a timeout) keeps its whole charge.
import { DurableObject } from "cloudflare:workers";

import { type BudgetConfig, type BudgetVars, budgetConfig, chargeFor, monthOf, settledCharge, ttlFor } from "./budget";
import { randomId } from "./ids";
import { type IceServer, type TurnApi, type TurnVars, turnApi } from "./turn";

type LeaseState = "pending" | "active" | "ending" | "ended" | "kept" | "unknown";

interface LeaseRow extends Record<string, SqlStorageValue> {
  id: string;
  room: string;
  issued_ms: number;
  ttl_s: number;
  rate_bps: number;
  charged: number;
  username: string | null;
  state: LeaseState;
}

export type Issue =
  | { ok: true; lease: string; ttl: number; iceServers: IceServer[] }
  | { ok: false; why: "budget" | "turn" };

// One credential, for the owner: no username, no credential, the room cut to its first 8 characters.
export interface LeaseView {
  id: string;
  room: string;
  issuedMs: number;
  ttlS: number;
  chargedBytes: number;
  state: LeaseState;
}

export interface LedgerStatus {
  month: string;
  budgetBytes: number;
  chargedBytes: number;
  remainingBytes: number;
  rateMbps: number;
  activeLeases: number;
}

const SCHEMA = `
CREATE TABLE IF NOT EXISTS lease (
  id TEXT PRIMARY KEY,
  room TEXT NOT NULL,
  month TEXT NOT NULL,
  issued_ms INTEGER NOT NULL,
  ttl_s INTEGER NOT NULL,
  rate_bps INTEGER NOT NULL,
  charged INTEGER NOT NULL,
  username TEXT,
  state TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS lease_month ON lease (month);`;

export class LedgerBook {
  constructor(private readonly sql: SqlStorage, private readonly api: TurnApi, private readonly cfg: BudgetConfig,
              private readonly now: () => number = Date.now) {
    sql.exec(SCHEMA);
  }

  chargedIn(month: string): number {
    return this.sql.exec<{ total: number | null }>("SELECT SUM(charged) AS total FROM lease WHERE month = ?", month).one().total ?? 0;
  }

  // The reservation is written before the first await: two issues never spend the same bytes.
  async issue(room: string): Promise<Issue> {
    const now = this.now(), month = monthOf(now);
    const ttl = ttlFor(this.cfg.budgetBytes - this.chargedIn(month), this.cfg);
    if (ttl === null) return { ok: false, why: "budget" };
    const id = randomId(12);
    this.sql.exec("INSERT INTO lease (id, room, month, issued_ms, ttl_s, rate_bps, charged, state) VALUES (?, ?, ?, ?, ?, ?, ?, 'pending')",
                  id, room, month, now, ttl, this.cfg.rateBytesPerSecond, chargeFor(ttl, this.cfg.rateBytesPerSecond));
    // Months before last are settled: their rows go.
    this.sql.exec("DELETE FROM lease WHERE month < ?", monthOf(Date.UTC(new Date(now).getUTCFullYear(), new Date(now).getUTCMonth() - 1, 1)));
    const made = await this.api.generate(ttl);
    if (made.kind === "refused") {
      this.sql.exec("DELETE FROM lease WHERE id = ?", id);
      return { ok: false, why: "turn" };
    }
    if (made.kind === "unknown") {
      this.sql.exec("UPDATE lease SET state = 'unknown' WHERE id = ?", id);
      return { ok: false, why: "turn" };
    }
    this.sql.exec("UPDATE lease SET state = 'active', username = ? WHERE id = ?", made.username, id);
    return { ok: true, lease: id, ttl, iceServers: made.iceServers };
  }

  // The room's guest is gone, or never needed the relay: revoke, then charge up to now.
  async end(room: string, id: string): Promise<boolean> {
    const lease = this.sql.exec<LeaseRow>("SELECT * FROM lease WHERE id = ? AND room = ?", id, room).toArray()[0];
    if (!lease || lease.state !== "active" || !lease.username) return false;
    return this.revoke(lease);
  }

  // The room's host left and did not come back: what it still holds would stay charged until
  // it expires (a host that crashed or was killed never says turn-end). Revoke each credential
  // still alive, a failed revocation included, and charge it up to now. Returns how many ended.
  async endRoom(room: string): Promise<number> {
    const rows = this.sql.exec<LeaseRow>("SELECT * FROM lease WHERE room = ? AND state IN ('active', 'kept') AND username IS NOT NULL AND issued_ms + ttl_s * 1000 > ?",
                                         room, this.now()).toArray();
    let ended = 0;
    for (const lease of rows) if (await this.revoke(lease)) ended++;
    return ended;
  }

  private async revoke(lease: LeaseRow): Promise<boolean> {
    this.sql.exec("UPDATE lease SET state = 'ending' WHERE id = ?", lease.id);
    if (!(await this.api.revoke(lease.username as string))) {
      this.sql.exec("UPDATE lease SET state = 'kept' WHERE id = ?", lease.id);
      return false;
    }
    const charged = settledCharge({ issuedMs: lease.issued_ms, rateBytesPerSecond: lease.rate_bps, chargedBytes: lease.charged },
                                  this.now(), this.cfg.graceSeconds);
    this.sql.exec("UPDATE lease SET state = 'ended', charged = ? WHERE id = ?", charged, lease.id);
    return true;
  }

  // The owner's correction for a credential that has expired: charged as if it ended at endMs,
  // never more than it already is. Only an expired one, so a live credential keeps its charge.
  settle(id: string, endMs: number): LeaseView | null {
    const lease = this.sql.exec<LeaseRow>("SELECT * FROM lease WHERE id = ?", id).toArray()[0];
    if (!lease || lease.state === "pending" || lease.issued_ms + lease.ttl_s * 1000 > this.now() || !(endMs >= lease.issued_ms)) return null;
    const charged = settledCharge({ issuedMs: lease.issued_ms, rateBytesPerSecond: lease.rate_bps, chargedBytes: lease.charged }, endMs, this.cfg.graceSeconds);
    this.sql.exec("UPDATE lease SET state = 'ended', charged = ? WHERE id = ?", charged, id);
    return this.leases().find((l) => l.id === id) ?? null;
  }

  leases(): LeaseView[] {
    return this.sql.exec<LeaseRow>("SELECT * FROM lease WHERE month = ? ORDER BY issued_ms", monthOf(this.now())).toArray().map((l) => ({
      id: l.id, room: l.room.slice(0, 8), issuedMs: l.issued_ms, ttlS: l.ttl_s, chargedBytes: l.charged, state: l.state,
    }));
  }

  status(): LedgerStatus {
    const now = this.now(), month = monthOf(now), charged = this.chargedIn(month);
    const active = this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM lease WHERE state = 'active' AND issued_ms + ttl_s * 1000 > ?", now).one().n;
    return {
      month,
      budgetBytes: this.cfg.budgetBytes,
      chargedBytes: charged,
      remainingBytes: Math.max(0, this.cfg.budgetBytes - charged),
      rateMbps: this.cfg.rateBytesPerSecond * 8 / 1e6,
      activeLeases: active,
    };
  }
}

export interface LedgerEnv extends BudgetVars, TurnVars {}

export class TurnLedger extends DurableObject<LedgerEnv> {
  private readonly book: LedgerBook;

  constructor(ctx: DurableObjectState, env: LedgerEnv) {
    super(ctx, env);
    this.book = new LedgerBook(ctx.storage.sql, turnApi(env), budgetConfig(env));
  }

  issue(room: string): Promise<Issue> { return this.book.issue(room); }
  end(room: string, lease: string): Promise<boolean> { return this.book.end(room, lease); }
  endRoom(room: string): Promise<number> { return this.book.endRoom(room); }
  settle(lease: string, endMs: number): LeaseView | null { return this.book.settle(lease, endMs); }
  leases(): LeaseView[] { return this.book.leases(); }
  status(): LedgerStatus { return this.book.status(); }
}
