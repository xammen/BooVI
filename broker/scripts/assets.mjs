// The broker's static assets: the guest page and its font, copied from the app's own files, with
// the headers BooVI's local server sends them with (src/guest/GuestHttp.cpp).
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const repo = fileURLToPath(new URL("../../", import.meta.url));
const out = fileURLToPath(new URL("../.assets/", import.meta.url));
rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });
copyFileSync(`${repo}web/guest/index.html`, `${out}index.html`);
copyFileSync(`${repo}qml/Veyra/fonts/JetBrainsMono-Regular.ttf`, `${out}font.ttf`);
writeFileSync(`${out}_headers`, `/*
  Cache-Control: no-cache
  Referrer-Policy: no-referrer
  X-Content-Type-Options: nosniff
  X-Frame-Options: DENY
  Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; media-src 'self' blob:; img-src 'self' data:; font-src 'self'; frame-ancestors 'none'
/font.ttf
  ! Cache-Control
  Cache-Control: public, max-age=604800
`);
console.log(`assets in ${out}`);
