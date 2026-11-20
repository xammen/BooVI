// The monthly TURN cap, as pure arithmetic. A credential is charged its whole lifetime at the
// most a share can send, before it exists: Cloudflare stops billing a credential at its expiry
// (TURN FAQ), so the charge bounds what it can cost. A credential revoked early is charged up to
// its revocation plus a grace period, never less.

export interface BudgetConfig {
  budgetBytes: number;
  rateBytesPerSecond: number;
  maxTtlSeconds: number;
  minTtlSeconds: number;
  graceSeconds: number;
}

export interface BudgetVars {
  TURN_BUDGET_GB?: string;
  TURN_RATE_MBPS?: string;
  TURN_TTL_MAX_S?: string;
  TURN_TTL_MIN_S?: string;
  TURN_GRACE_S?: string;
}

// Unreadable vars fail closed: no budget, the highest rate, the longest grace.
function number(text: string | undefined, fallback: number, min: number, max: number): number {
  const v = Number(text);
  return Number.isFinite(v) && v >= min && v <= max ? v : fallback;
}

export function budgetConfig(vars: BudgetVars): BudgetConfig {
  const maxTtl = Math.floor(number(vars.TURN_TTL_MAX_S, 14400, 60, 48 * 3600));
  return {
    budgetBytes: Math.floor(number(vars.TURN_BUDGET_GB, 0, 0, 1e6) * 1e9),
    rateBytesPerSecond: Math.ceil(number(vars.TURN_RATE_MBPS, 1000, 1, 10000) * 1e6 / 8),
    maxTtlSeconds: maxTtl,
    minTtlSeconds: Math.min(maxTtl, Math.floor(number(vars.TURN_TTL_MIN_S, 600, 60, 48 * 3600))),
    graceSeconds: Math.ceil(number(vars.TURN_GRACE_S, 300, 0, 3600)),
  };
}

// The budget is per calendar month, UTC.
export function monthOf(ms: number): string {
  return new Date(ms).toISOString().slice(0, 7);
}

// The longest credential the rest of the month's budget pays for, at most maxTtl; null when that
// is under minTtl.
export function ttlFor(remainingBytes: number, cfg: BudgetConfig): number | null {
  if (remainingBytes <= 0) return null;
  const ttl = Math.min(cfg.maxTtlSeconds, Math.floor(remainingBytes / cfg.rateBytesPerSecond));
  return ttl >= cfg.minTtlSeconds ? ttl : null;
}

export const chargeFor = (ttlSeconds: number, rateBytesPerSecond: number): number => ttlSeconds * rateBytesPerSecond;

export interface Charged {
  issuedMs: number;
  rateBytesPerSecond: number;
  chargedBytes: number;
}

// What a credential revoked at endMs is charged: its time until then plus the grace, never more
// than it was charged at issue.
export function settledCharge(lease: Charged, endMs: number, graceSeconds: number): number {
  const seconds = Math.max(0, Math.ceil((endMs - lease.issuedMs) / 1000)) + graceSeconds;
  return Math.min(lease.chargedBytes, seconds * lease.rateBytesPerSecond);
}
