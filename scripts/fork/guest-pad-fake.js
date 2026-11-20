// Test hook for the guest page, injected before the page's own script by
// scripts\fork\guest-probe.ps1 -Init scripts\fork\guest-pad-fake.js -Url "<link>#fake=<scenario>".
// Nothing here ships: it fakes the browser's pad APIs and plays the player.
//   #fake=remap   a pad in a non-standard layout (buttons at odd indices, triggers on axes 4 and 5
//                 resting at -1, the d-pad as a hat on axis 9). It sends states for 20 s without a
//                 button (the host flags it), then runs "configurer ma manette" (pausing on L2 for a
//                 screenshot: body[data-shot="1"]) and holds ✕ + L2 + d-pad up through the mapping.
//                 The host should then receive buttons 0x1041.
//   #fake=steam   a wired 2015 Steam Controller over WebHID holding A + RB, right pad up.
//                 The host should receive buttons 0x21, and the page sends the three setup reports.
(() => {
  const scenario = (location.hash.match(/fake=(\w+)/) || [])[1];
  if (!scenario) return;
  const status = text => { window.__fakeStatus = text; };
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const until = async (test, ms = 30000) => { const t = Date.now(); while (!test()) { if (Date.now() - t > ms) throw new Error("timeout"); await sleep(50); } };
  const connected = () => (document.getElementById("hudState") || {}).textContent === "connecté";

  if (scenario === "remap") {
    const HAT_REST = 3.2857;
    const pad = { id: "Fake Pad (Vendor: 1234 Product: 5678)", index: 0, connected: true, mapping: "", timestamp: 0,
                  buttons: Array.from({ length: 16 }, () => ({ pressed: false, touched: false, value: 0 })),
                  axes: [0, 0, 0, 0, -1, -1, 0, 0, 0, HAT_REST] };
    Object.defineProperty(navigator, "getGamepads", { value: () => [pad, null, null, null] });
    const press = (i, on) => { pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
    // What the player does for each prompt (TARGETS order in the page), null = "passer".
    const plan = [["b", 1], ["b", 2], ["b", 0], ["b", 3], ["b", 4], ["b", 5], ["a", 4, 1], ["a", 5, 1],
                  ["a", 0, 1], ["a", 1, 1], ["a", 2, 1], ["a", 3, 1], ["b", 10], ["b", 11], ["b", 9], ["b", 8],
                  ["h", -1], ["h", 0.142857], ["h", 0.714286], ["h", -0.428571], null, null];
    const act = (step, on) => {
      const [t, i, v] = step;
      if (t === "b") press(i, on);
      else if (t === "a") pad.axes[i] = on ? v : (i === 4 || i === 5 ? -1 : 0);
      else pad.axes[9] = on ? i : HAT_REST;
    };
    (async () => {
      await until(connected);
      status("idle pad, no button");
      await sleep(20000);
      const button = text => [...document.querySelectorAll("#padActions button")].find(b => b.textContent === text);
      await until(() => button("configurer ma manette"));
      button("configurer ma manette").click();
      for (let n = 0; n < plan.length; n++) {
        await sleep(250);
        status("remap step " + (n + 1));
        if (n === 6) { document.body.dataset.shot = "1"; await sleep(2500); }
        if (!plan[n]) { document.getElementById("remapSkip").click(); continue; }
        act(plan[n], true); await sleep(200); act(plan[n], false);
      }
      await sleep(300);
      press(1, true); pad.axes[4] = 1; pad.axes[9] = -1;   // ✕, L2, d-pad up
      status("holding cross + L2 + up · map " + localStorage.getItem("boovi-pad-map:" + pad.id).length + " bytes");
    })().catch(e => status("failed: " + e.message));
  }

  if (scenario === "steam") {
    const features = [];
    const dev = { vendorId: 0x28de, productId: 0x1102, productName: "Steam Controller", opened: false, collections: [{ usagePage: 0xff00, usage: 1 }],
                  listeners: [], open: async () => { dev.opened = true; },
                  addEventListener: (type, fn) => { if (type === "inputreport") dev.listeners.push(fn); },
                  sendFeatureReport: async (id, data) => { features.push([id, data[0]]); }, sendReport: async () => {} };
    const hid = { getDevices: async () => [dev], requestDevice: async () => [dev], addEventListener: () => {} };
    Object.defineProperty(navigator, "hid", { value: hid });
    Object.defineProperty(navigator, "getGamepads", { value: () => [null, null, null, null] });
    let packet = 0;
    const report = () => {
      const v = new DataView(new ArrayBuffer(64));
      v.setUint16(0, 1, true); v.setUint8(2, 1); v.setUint8(3, 60); v.setUint32(4, ++packet, true);
      v.setUint32(8, 0x80 | 0x04 | 0x100000, true);   // A, RB, right pad touched
      v.setInt16(22, 20000, true);                    // right pad up
      return v;
    };
    (async () => {
      await until(connected);
      await until(() => dev.opened);
      setInterval(() => { for (const fn of dev.listeners) fn({ device: dev, reportId: 0, data: report() }); }, 8);
      setInterval(() => status("features " + features.map(([id, t]) => id + ":0x" + t.toString(16)).join(" ")), 500);
    })().catch(e => status("failed: " + e.message));
  }
})();
