import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { MAX_CALIBRATION_ZONES } from "../../core/constants";
import type { WebDeviceConfig, WebDeviceState } from "../../core/types";
import { en } from "../i18n/en";
import type { DeviceApi, WebStaticRadarTuningStatus } from "../types";
import { createCalibrationRun } from "./useCalibrationRun.svelte";

function staticState(presence = true): WebDeviceState {
  return {
    connected: true,
    targets: [],
    updatedAt: Date.now(),
    pirMotion: false,
    debug: {
      staticRadar: {
        available: true,
        presence,
        detectionDistanceMm: 1800
      }
    }
  };
}

function tuningStatus(): WebStaticRadarTuningStatus {
  return {
    ok: true,
    available: true,
    active: true,
    resolutionMm: 750,
    gates: Array.from({ length: 9 }, (_, gate) => ({
      gate,
      startMm: gate * 750,
      endMm: (gate + 1) * 750,
      sensitivity: 50,
      moveEnergy: gate === 2 ? 52 : 0,
      stillEnergy: gate === 2 ? 72 : 0
    }))
  };
}

function apiFixture(
  getState: () => WebDeviceState,
  setSensitivity: (gate: number, sensitivity: number) => void = () => {}
): DeviceApi {
  return {
    getState: vi.fn(async () => getState()),
    getConfig: vi.fn(),
    getStats: vi.fn(),
    saveConfig: vi.fn(),
    saveStats: vi.fn(),
    getStaticRadarTuningStatus: vi.fn(async () => tuningStatus()),
    setStaticRadarTuningSession: vi.fn(async () => {}),
    setStaticRadarGateSensitivity: vi.fn(async (gate, sensitivity) => setSensitivity(gate, sensitivity))
  };
}

function sessionFixture(
  api: DeviceApi,
  getState: () => WebDeviceState,
  initialConfig: WebDeviceConfig = { version: 1, zones: [], calibrationZones: [] }
) {
  let config = initialConfig;
  const session = createCalibrationRun({
    getApi: () => api,
    getConfig: () => config,
    getState,
    getCalibrationZoneCount: () => config.calibrationZones?.length ?? 0,
    getMessages: () => en,
    updateConfig: (mutator) => {
      config = mutator(config);
    },
    selectZone: vi.fn(),
    setStatus: vi.fn()
  });
  return { session, getConfig: () => config };
}

async function collectStaticSamples(session: ReturnType<typeof createCalibrationRun>): Promise<void> {
  session.start();
  await vi.advanceTimersByTimeAsync(0);
  for (let tick = 1; tick <= 30; tick += 1) {
    await vi.advanceTimersByTimeAsync(500);
    session.update();
    await vi.advanceTimersByTimeAsync(0);
  }
}

describe("combined false-positive calibration session", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-07-20T00:00:00Z"));
    vi.stubGlobal("window", globalThis);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("calibrates LD2410C without requiring an LD2450 target", async () => {
    let state = staticState(true);
    const applied: Array<[number, number]> = [];
    const api = apiFixture(
      () => state,
      (gate, sensitivity) => {
        applied.push([gate, sensitivity]);
        state = staticState(false);
      }
    );
    const { session, getConfig } = sessionFixture(api, () => state);

    await collectStaticSamples(session);
    await vi.advanceTimersByTimeAsync(2500);

    expect(applied).toEqual([[2, 23]]);
    expect(session.result).toMatchObject({
      tone: "ok",
      createdCount: 0,
      adjustedStaticRadarGates: 1
    });
    expect(getConfig().calibrationZones).toEqual([]);
    expect(api.setStaticRadarTuningSession).toHaveBeenLastCalledWith(false);
  });

  it("allows assist-radar correction when all spatial correction slots are occupied", () => {
    const state = staticState(true);
    const api = apiFixture(() => state);
    const calibrationZones = Array.from({ length: MAX_CALIBRATION_ZONES }, (_, index) => ({
      id: `calibration_${index}`,
      name: `Calibration ${index}`,
      type: "filter" as const,
      shape: "polygon" as const,
      points: [
        [0, 0] as [number, number],
        [100, 0] as [number, number],
        [100, 100] as [number, number]
      ]
    }));
    const { session } = sessionFixture(api, () => state, {
      version: 1,
      zones: [],
      calibrationZones
    });

    expect(session.canStart()).toBe(true);
    expect(session.statusText()).toBe(en.zones.calibrationStaticRadarOnlyAvailable);
  });

  it("creates an LD2450 correction zone and tunes LD2410C in the same run", async () => {
    let state: WebDeviceState = {
      ...staticState(true),
      targets: [
        {
          id: "target_1",
          name: "T1",
          color: "#ff6b7a",
          x: 300,
          y: 1800,
          active: true
        }
      ]
    };
    const applied: Array<[number, number]> = [];
    const api = apiFixture(
      () => state,
      (gate, sensitivity) => {
        applied.push([gate, sensitivity]);
        state = {
          ...state,
          debug: {
            ...state.debug,
            staticRadar: { ...state.debug?.staticRadar, presence: false }
          }
        };
      }
    );
    const { session, getConfig } = sessionFixture(api, () => state);

    await collectStaticSamples(session);
    await vi.advanceTimersByTimeAsync(2500);

    expect(applied).toEqual([[2, 23]]);
    expect(getConfig().calibrationZones).toHaveLength(1);
    expect(session.result).toMatchObject({
      tone: "ok",
      createdCount: 1,
      adjustedStaticRadarGates: 1
    });
  });

  it("rechecks once and reports one adjusted gate when the same gate needs a second reduction", async () => {
    let state = staticState(true);
    const applied: Array<[number, number]> = [];
    const api = apiFixture(
      () => state,
      (gate, sensitivity) => {
        applied.push([gate, sensitivity]);
        if (applied.length === 2) state = staticState(false);
      }
    );
    const { session } = sessionFixture(api, () => state);

    await collectStaticSamples(session);
    await vi.advanceTimersByTimeAsync(5000);

    expect(applied).toEqual([
      [2, 23],
      [2, 20]
    ]);
    expect(session.result).toMatchObject({
      tone: "ok",
      adjustedStaticRadarGates: 1
    });
  });

  it("does not start either correction while PIR motion is active", () => {
    const state = { ...staticState(true), pirMotion: true };
    const api = apiFixture(() => state);
    const { session } = sessionFixture(api, () => state);

    session.start();

    expect(session.result?.tone).toBe("error");
    expect(api.setStaticRadarTuningSession).not.toHaveBeenCalled();
    expect(api.setStaticRadarGateSensitivity).not.toHaveBeenCalled();
  });

  it("keeps the existing no-target result when neither radar reports a target", () => {
    const state = staticState(false);
    const api = apiFixture(() => state);
    const { session } = sessionFixture(api, () => state);

    session.start();

    expect(session.result?.reason).toBe(en.zones.calibrationStatus.noTarget);
    expect(api.setStaticRadarTuningSession).not.toHaveBeenCalled();
  });
});
