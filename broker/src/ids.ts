// A room's id is the first 132 bits of SHA-256 over the host's room key. Only the holder of the
// key can open the room its id names, so knowing a link never lets anyone stand in for its host.

const ROOM_ID = /^[A-Za-z0-9_-]{22}$/;
const ROOM_KEY = /^[A-Za-z0-9_-]{44}$/;

export const isRoomId = (s: string): boolean => ROOM_ID.test(s);
export const isRoomKey = (s: string): boolean => ROOM_KEY.test(s);

export function base64url(bytes: Uint8Array): string {
  let text = "";
  for (const b of bytes) text += String.fromCharCode(b);
  return btoa(text).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/, "");
}

export async function roomIdOf(key: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`boovi room v1\n${key}`));
  return base64url(new Uint8Array(digest)).slice(0, 22);
}

export function randomId(bytes: number): string {
  return base64url(crypto.getRandomValues(new Uint8Array(bytes)));
}

export function sameSecret(a: string, b: string): boolean {
  const x = new TextEncoder().encode(a), y = new TextEncoder().encode(b);
  return x.byteLength === y.byteLength && x.byteLength > 0 && crypto.subtle.timingSafeEqual(x, y);
}
