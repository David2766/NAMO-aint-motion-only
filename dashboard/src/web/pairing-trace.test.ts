import { afterEach, describe, expect, it, vi } from "vitest";
import {
  emitPairingTrace,
  resetPairingTraceForTests,
  tracePairingPhase,
  type PairingTraceState
} from "./pairing-trace";

afterEach(() => {
  vi.unstubAllGlobals();
  resetPairingTraceForTests();
});

describe("pairing trace phase wrapper", () => {
  it("returns the original result and emits bounded phase metadata", async () => {
    const events: Array<{
      phase: string;
      state: PairingTraceState;
      detail?: Record<string, unknown>;
      durationMs?: number;
    }> = [];
    const emitter = vi.fn((phase, state, detail, durationMs) => {
      events.push({ phase, state, detail, durationMs });
    });

    const result = await tracePairingPhase(
      "pairing.confirm",
      async () => ({ ok: true }),
      { peer: "http://192.168.29.203" },
      emitter
    );

    expect(result).toEqual({ ok: true });
    expect(events.map((event) => event.state)).toEqual(["begin", "end"]);
    expect(events[1].durationMs).toBeGreaterThanOrEqual(0);
  });

  it("rethrows the exact error and records only safe error fields", async () => {
    const events: Array<Record<string, unknown>> = [];
    const cause = Object.assign(new Error("contains internal response text"), {
      code: "site_configuration_continuity_unavailable",
      status: 409,
      secret: "must-not-be-copied"
    });

    await expect(tracePairingPhase(
      "floorplan.convergence",
      async () => { throw cause; },
      undefined,
      (_phase, state, detail) => events.push({ state, ...detail })
    )).rejects.toBe(cause);

    expect(events[1]).toMatchObject({
      state: "error",
      name: "Error",
      code: "site_configuration_continuity_unavailable",
      status: 409
    });
    expect(events[1]).not.toHaveProperty("secret");
  });

  it("enables delivery only from the explicit capture URL", () => {
    const values = new Map<string, string>();
    const sendBeacon = vi.fn((_url: string | URL, _data?: BodyInit | null) => true);
    vi.stubGlobal("window", {
      location: {
        search: "?namoTrace=http%3A%2F%2F192.168.29.112%3A8765%2Fevents" +
          "&namoTraceToken=test-token&namoTraceSession=test-session"
      },
      sessionStorage: {
        getItem: (key: string) => values.get(key) ?? null,
        setItem: (key: string, value: string) => values.set(key, value)
      }
    });
    vi.stubGlobal("navigator", { sendBeacon });

    emitPairingTrace("pairing.invitation", "begin", { peer: "203" });

    expect(sendBeacon).toHaveBeenCalledOnce();
    const [url, body] = sendBeacon.mock.calls[0]!;
    expect(String(url)).toContain("/events?token=test-token");
    expect(JSON.parse(String(body))).toMatchObject({
      sessionId: "test-session",
      phase: "pairing.invitation",
      state: "begin"
    });
  });
});
