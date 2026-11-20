// The guest page's Steam Controller parser (web/guest/index.html, <script id="steampad">):
// input reports built as SDL 3.4.14 reads them, checked against the DualSense state they give.
// Run: node tests/fork/GuestSteamPadTests.mjs
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";

const page = readFileSync(new URL("../../web/guest/index.html", import.meta.url), "utf8");
const code = page.match(/<script id="steampad">([\s\S]*?)<\/script>/)[1];
const SteamPad = new Function(code + "\nreturn SteamPad;")();

const bit = i => 1 << i;
const CROSS = bit(0), R1 = bit(5), L2 = bit(6), SHARE = bit(8), L3 = bit(10), R3 = bit(11), UP = bit(12), RIGHT = bit(15), PS = bit(16), PAD = bit(17);

// 2015 controller, report 0: version 1, type 1, buttons at 8 (triggers in bytes 11, 12), pads at 16.
function d0g({ raw = 0, lt = 0, rt = 0, left = [0, 0], right = [0, 0] }) {
  const v = new DataView(new ArrayBuffer(64));
  v.setUint16(0, 1, true); v.setUint8(2, 1); v.setUint8(3, 60);
  v.setUint32(8, raw, true); v.setUint8(11, lt); v.setUint8(12, rt);
  v.setInt16(16, left[0], true); v.setInt16(18, left[1], true);
  v.setInt16(20, right[0], true); v.setInt16(22, right[1], true);
  return v;
}
const parse = (pid, id, v, mem = {}) => SteamPad.parse(pid, id, v, mem);

let s = parse(0x1102, 0, d0g({ raw: 0x80 | 0x04 | 0x2000 | 0x8000, lt: 203, left: [20000, 10000] }));
assert.equal(s.buttons, CROSS | R1 | PS | PAD, "A, RB, steam, left grip");
assert.deepEqual(s.axes.slice(0, 2), [20000, -10000], "left stick, +y down");
assert.equal(s.l2, 255, "a full trigger pull");
assert.equal(s.r2, 0);
assert.deepEqual(s.axes.slice(2), [0, 0], "right pad untouched: right stick centred");

// Right pad touched straight up: the right stick goes up (negative y).
s = parse(0x1102, 0, d0g({ raw: 0x100000, right: [0, 20000] }));
assert.ok(s.axes[3] < -20000 && Math.abs(s.axes[2]) < 9000, "right pad up = right stick up: " + s.axes);

// Left pad clicked at its top: d-pad up; the stick keeps its last value while the pad is in use.
const mem = {};
parse(0x1102, 0, d0g({ left: [5000, 0] }), mem);
s = parse(0x1102, 0, d0g({ raw: 0x80000 | 0x800000 | 0x20000, left: [0, 25000] }), mem);
assert.equal(s.buttons & (UP | RIGHT), UP, "left pad click at the top: d-pad up");
assert.equal(s.axes[0], 5000, "stick kept while interleaved with the pad");

// A left pad click with no finger on the pad is the stick click (older firmware).
s = parse(0x1102, 0, d0g({ raw: 0x20000 }));
assert.equal(s.buttons, L3);

// Dongle link events, and anything that is not a state report.
const link = new DataView(new ArrayBuffer(64)); link.setUint16(0, 1, true); link.setUint8(2, 3); link.setUint8(3, 1);
link.setUint8(4, 2); assert.deepEqual(parse(0x1142, 0, link), { wireless: "connect" });
link.setUint8(4, 1); assert.deepEqual(parse(0x1142, 0, link), { wireless: "disconnect" });
assert.equal(parse(0x1102, 3, d0g({})), null, "only report 0");

// 2025 controller, report 0x42: seq, buttons u32, triggers, sticks, then the pads at 17.
function triton({ raw = 0, lt = 0, rt = 0, ls = [0, 0], rs = [0, 0], lpad = [0, 0], rpad = [0, 0] }, at = 17) {
  const v = new DataView(new ArrayBuffer(48));
  v.setUint32(1, raw, true); v.setInt16(5, lt, true); v.setInt16(7, rt, true);
  [ls[0], ls[1], rs[0], rs[1]].forEach((x, i) => v.setInt16(9 + 2 * i, x, true));
  v.setInt16(at, lpad[0], true); v.setInt16(at + 2, lpad[1], true); v.setInt16(at + 6, rpad[0], true); v.setInt16(at + 8, rpad[1], true);
  return v;
}
s = parse(0x1302, 0x42, triton({ raw: 0x1 | 0x4000 | 0x8000 | 0x10 | 0x8000000, lt: 32767, rt: 16384, ls: [-32767, 32767], rs: [1000, 0] }));
assert.equal(s.buttons, CROSS | SHARE | L3 | PAD | L2, "A, menu, L3, quick access, LT click");
assert.equal(s.l2, 255); assert.equal(s.r2, 128);
assert.deepEqual(s.axes, [-32767, -32767, 1000, 0], "sticks, +y down");

// Right pad touched with the stick at rest: the pad drives the right stick.
s = parse(0x1302, 0x42, triton({ raw: 0x200000 | 0x400000, rpad: [16000, 0] }));
assert.equal(s.buttons, R3, "right pad click = R3");
assert.equal(s.axes[2], 20000);

// Report 0x47 puts a timestamp before the pads; a left pad click on the right edge is d-pad right.
s = parse(0x1304, 0x47, triton({ raw: 0x4000000, lpad: [30000, 0] }, 19));
assert.equal(s.buttons, RIGHT);
assert.deepEqual(parse(0x1304, 0x79, new DataView(new Uint8Array([2]).buffer)), { wireless: "connect" });

// What the page sends the controllers.
assert.deepEqual([...SteamPad.setup(0x1302)[0][1].slice(0, 5)], [0x87, 3, 9, 0, 0], "2025: lizard mode off");
assert.equal(SteamPad.setup(0x1302)[0][1].length, 63);
assert.deepEqual(SteamPad.setup(0x1142).map(([id, d]) => [id, d[0], d.length]), [[0, 0x81, 64], [0, 0x8e, 64], [0, 0x87, 64]]);
assert.deepEqual(SteamPad.rumble(0x1102, 0, 0), [], "2015: no pulse at rest");
assert.equal(SteamPad.rumble(0x1102, 255, 0)[0][2][2], 1, "2015: left motor = left pad");
assert.deepEqual([...SteamPad.rumble(0x1302, 255, 0)[0][2]], [0, 0, 0, 255, 255, 0, 0, 0, 0], "2025: rumble report");
assert.ok(SteamPad.supports({ vendorId: 0x28de, productId: 0x1142, collections: [{ usagePage: 0xff00 }] }));
assert.ok(!SteamPad.supports({ vendorId: 0x28de, productId: 0x11ff, collections: [{ usagePage: 1 }] }), "Steam's virtual pad is a gamepad already");

console.log("GuestSteamPadTests passed");
