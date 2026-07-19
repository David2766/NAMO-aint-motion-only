import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

function createStorage(): Storage {
  const values = new Map<string, string>();
  return {
    get length() {
      return values.size;
    },
    clear() {
      values.clear();
    },
    getItem(key) {
      return values.get(key) ?? null;
    },
    key(index) {
      return [...values.keys()][index] ?? null;
    },
    removeItem(key) {
      values.delete(key);
    },
    setItem(key, value) {
      values.set(key, String(value));
    }
  };
}

describe("demo mock API", () => {
  beforeEach(() => {
    vi.resetModules();
    vi.stubGlobal("sessionStorage", createStorage());
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("exposes the installed assist radar and its interactive tuning contract", async () => {
    const { mockApi } = await import("./mock-api");

    const state = await mockApi.getState();
    expect(state.debug?.staticRadar).toMatchObject({
      available: true,
      presence: true
    });

    await mockApi.setStaticRadarTuningSession?.(true);
    const active = await mockApi.getStaticRadarTuningStatus?.();
    expect(active).toMatchObject({
      ok: true,
      available: true,
      active: true,
      resolutionMm: 750
    });
    expect(active?.gates).toHaveLength(9);
    expect(active?.gates.some((gate) => gate.moveEnergy > 0 || gate.stillEnergy > 0)).toBe(true);

    await mockApi.setStaticRadarGateSensitivity?.(3, 72);
    expect((await mockApi.getStaticRadarTuningStatus?.())?.gates[3]?.sensitivity).toBe(72);

    await mockApi.setStaticRadarTuningSession?.(false);
    expect((await mockApi.getStaticRadarTuningStatus?.())?.active).toBe(false);
  });

  it("rejects an invalid assist radar gate", async () => {
    const { mockApi } = await import("./mock-api");
    await expect(mockApi.setStaticRadarGateSensitivity?.(9, 50)).rejects.toThrow("invalid static radar gate");
  });
});
