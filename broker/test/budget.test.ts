import { describe, expect, it } from "vitest";

import { budgetConfig, monthOf, settledCharge, ttlFor } from "../src/budget";

const cfg = budgetConfig({ TURN_BUDGET_GB: "900", TURN_RATE_MBPS: "120", TURN_TTL_MAX_S: "14400", TURN_TTL_MIN_S: "600", TURN_GRACE_S: "60" });

describe("budget config", () => {
  it("reads the vars", () => {
    expect(cfg).toEqual({ budgetBytes: 900e9, rateBytesPerSecond: 15e6, maxTtlSeconds: 14400, minTtlSeconds: 600, graceSeconds: 60 });
  });

  it("fails closed on vars it cannot read", () => {
    const bad = budgetConfig({ TURN_BUDGET_GB: "lots", TURN_RATE_MBPS: "-1", TURN_TTL_MAX_S: "0", TURN_TTL_MIN_S: "x", TURN_GRACE_S: "nope" });
    expect(bad.budgetBytes).toBe(0);
    expect(bad.rateBytesPerSecond).toBe(125e6);   // 1 Gb/s, far above anything a share sends
    expect(bad.graceSeconds).toBe(300);
    expect(ttlFor(1e12, bad)).not.toBeNull();     // the rate still bounds what a credential can cost
    expect(budgetConfig({}).budgetBytes).toBe(0);
  });

  it("never asks for a shortest credential longer than the longest", () => {
    expect(budgetConfig({ TURN_TTL_MAX_S: "300", TURN_TTL_MIN_S: "900" }).minTtlSeconds).toBe(300);
  });
});

describe("credential lifetime", () => {
  it("is the longest one when the budget allows it", () => {
    expect(ttlFor(900e9, cfg)).toBe(14400);
  });

  it("shrinks to what the rest of the budget pays for", () => {
    expect(ttlFor(15e6 * 1000, cfg)).toBe(1000);
    expect(ttlFor(15e6 * 1000 + 14e6, cfg)).toBe(1000);
  });

  it("is refused below the shortest useful one", () => {
    expect(ttlFor(15e6 * 600, cfg)).toBe(600);
    expect(ttlFor(15e6 * 600 - 1, cfg)).toBeNull();
    expect(ttlFor(0, cfg)).toBeNull();
    expect(ttlFor(-5e9, cfg)).toBeNull();
  });

  it("costs at most what remains", () => {
    for (const remaining of [9e6, 15e6 * 600, 1.234e10, 899.9e9, 900e9]) {
      const ttl = ttlFor(remaining, cfg);
      if (ttl !== null) expect(ttl * cfg.rateBytesPerSecond).toBeLessThanOrEqual(remaining);
    }
  });
});

describe("settling a revoked credential", () => {
  const lease = { issuedMs: 1_000_000, rateBytesPerSecond: 15e6, chargedBytes: 14400 * 15e6 };

  it("charges the time it lived plus the grace", () => {
    expect(settledCharge(lease, 1_000_000 + 600_000, 60)).toBe(660 * 15e6);
    expect(settledCharge(lease, 1_000_000 + 600_001, 60)).toBe(661 * 15e6);
  });

  it("never charges more than at issue", () => {
    expect(settledCharge(lease, 1_000_000 + 20 * 3600_000, 60)).toBe(lease.chargedBytes);
  });

  it("charges the grace even for a clock that went back", () => {
    expect(settledCharge(lease, 0, 60)).toBe(60 * 15e6);
  });
});

describe("month", () => {
  it("is the UTC calendar month", () => {
    expect(monthOf(Date.UTC(2026, 10, 30, 23, 59, 59))).toBe("2026-11");
    expect(monthOf(Date.UTC(2026, 11, 1, 0, 0, 0))).toBe("2026-12");
  });
});
