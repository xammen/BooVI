import { describe, expect, it } from "vitest";

import { isRoomId, isRoomKey, roomIdOf } from "../src/ids";

describe("room ids", () => {
  // The same vector is in tests/fork/GuestProtocolTests.cpp: the app derives ids the same way.
  it("are the hash of the room key", async () => {
    const key = "BooVI-room-key-test-vector-0123456789abcdefg";
    expect(isRoomKey(key)).toBe(true);
    expect(await roomIdOf(key)).toBe("BD0Yl_nIfZ9Outt9sJj5xG");
  });

  it("have 132 bits in 22 base64url characters", () => {
    expect(isRoomId("BD0Yl_nIfZ9Outt9sJj5xG")).toBe(true);
    expect(isRoomId("BD0Yl_nIfZ9Outt9sJj5x")).toBe(false);
    expect(isRoomId("BD0Yl_nIfZ9Outt9sJj5x/")).toBe(false);
  });
});
