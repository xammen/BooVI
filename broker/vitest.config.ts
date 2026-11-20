import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: { OWNER_KEY: "owner-key-for-tests", TURN_KEY_ID: "test-key", TURN_KEY_API_TOKEN: "test-token", TURN_BUDGET_GB: "100" },
        // Cloudflare's TURN API, faked: every outbound fetch of the Worker goes to turn-fake.
        outboundService: "turn-fake",
        workers: [{ name: "turn-fake", modules: true, scriptPath: "./test/turn-fake.mjs" }],
      },
    }),
  ],
});
