import { describe, expect, it } from "vitest";
import { bootGuardRemainingSeconds } from "./boot-guard";

describe("bootGuardRemainingSeconds", () => {
  it("uses the firmware countdown and catches up after a background pause", () => {
    const status = { ok: true, boot: { initialGuardActive: true, guardSeconds: 60, remainingSeconds: 42 } };
    expect(bootGuardRemainingSeconds(status, 1000, 11000)).toBe(32);
    expect(bootGuardRemainingSeconds(status, 1000, 70000)).toBe(0);
  });

  it("falls back to uptime for older firmware", () => {
    const status = {
      ok: true,
      boot: { initialGuardActive: true, guardSeconds: 60 },
      firmware: { uptimeSeconds: 25 }
    };
    expect(bootGuardRemainingSeconds(status, 1000, 6000)).toBe(30);
  });
});
