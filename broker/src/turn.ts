// Cloudflare Realtime TURN: short-lived credentials from the owner's TURN key, and their
// revocation. The key's id and API token are Worker secrets; nothing here logs them.

export interface IceServer {
  urls: string | string[];
  username?: string;
  credential?: string;
}

export type Generated =
  | { kind: "issued"; iceServers: IceServer[]; username: string }
  | { kind: "refused"; status: number }   // Cloudflare answered no: no credential exists
  | { kind: "unknown" };                  // no clear answer: a credential may exist

export interface TurnApi {
  generate(ttlSeconds: number): Promise<Generated>;
  revoke(username: string): Promise<boolean>;
}

export interface TurnVars {
  TURN_KEY_ID?: string;
  TURN_KEY_API_TOKEN?: string;
}

const ORIGIN = "https://rtc.live.cloudflare.com";

function parseIssued(body: unknown): Generated {
  const servers = (body as { iceServers?: unknown })?.iceServers;
  if (!Array.isArray(servers)) return { kind: "unknown" };
  const iceServers = servers.filter((s): s is IceServer => typeof s === "object" && s !== null && "urls" in s);
  const username = iceServers.map((s) => s.username).find((u): u is string => typeof u === "string" && u.length > 0);
  return username ? { kind: "issued", iceServers, username } : { kind: "unknown" };
}

export function turnApi(vars: TurnVars): TurnApi {
  const key = vars.TURN_KEY_ID ?? "", token = vars.TURN_KEY_API_TOKEN ?? "";
  const base = `${ORIGIN}/v1/turn/keys/${encodeURIComponent(key)}/credentials`;
  const headers = { Authorization: `Bearer ${token}`, "Content-Type": "application/json" };
  return {
    async generate(ttlSeconds) {
      if (!key || !token) return { kind: "refused", status: 0 };
      let response: Response;
      try {
        response = await fetch(`${base}/generate-ice-servers`, { method: "POST", headers, body: JSON.stringify({ ttl: ttlSeconds }) });
      } catch {
        return { kind: "unknown" };
      }
      if (response.status >= 400 && response.status < 500) return { kind: "refused", status: response.status };
      if (!response.ok) return { kind: "unknown" };
      return parseIssued(await response.json().catch(() => null));
    },
    async revoke(username) {
      try {
        const response = await fetch(`${base}/${encodeURIComponent(username)}/revoke`, { method: "POST", headers });
        return response.ok;
      } catch {
        return false;
      }
    },
  };
}
