// A stand-in for rtc.live.cloudflare.com. POST https://fake.control/mode {generate, revoke} sets
// what it answers; GET https://fake.control/calls lists what it was asked.
let mode = { generate: "ok", revoke: "ok" };
let calls = [];
let next = 0;

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.hostname === "fake.control") {
      if (url.pathname === "/mode") { mode = { generate: "ok", revoke: "ok", ...(await request.json()) }; calls = []; return new Response("ok"); }
      return Response.json(calls);
    }
    const auth = request.headers.get("Authorization");
    const body = request.method === "POST" && url.pathname.endsWith("/generate-ice-servers") ? await request.json() : null;
    calls.push({ host: url.hostname, path: url.pathname, auth, body });
    if (url.pathname.endsWith("/generate-ice-servers")) {
      if (mode.generate === "refuse") return new Response("no", { status: 403 });
      if (mode.generate === "fail") return new Response("down", { status: 503 });
      if (mode.generate === "garbage") return new Response("{}", { status: 201 });
      const username = `u${++next}`;
      return Response.json({ iceServers: [
        { urls: ["stun:stun.cloudflare.com:3478"] },
        { urls: ["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:443?transport=tcp"], username, credential: "secret" },
      ] }, { status: 201 });
    }
    if (url.pathname.endsWith("/revoke")) return new Response(null, { status: mode.revoke === "ok" ? 204 : 500 });
    return new Response("unexpected", { status: 404 });
  },
};
