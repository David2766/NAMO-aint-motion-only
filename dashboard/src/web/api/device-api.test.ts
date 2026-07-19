import { afterEach, describe, expect, it, vi } from "vitest";
import type { WebDeviceConfig, WebDeviceStats } from "../../core/types";

type FormCall = {
  url: string;
  values: Record<string, string>;
};

function okResponse(): Response {
  return new Response(JSON.stringify({ ok: true }), {
    status: 200,
    headers: {
      "Content-Type": "application/json"
    }
  });
}

async function loadDeviceApi(search = "") {
  vi.resetModules();
  vi.stubGlobal("window", {
    location: { search }
  });
  return import("./device-api");
}

function readFormCall(input: RequestInfo | URL, init?: RequestInit): FormCall {
  const values: Record<string, string> = {};
  const body = init?.body;
  if (body instanceof URLSearchParams) {
    for (const [key, value] of body.entries()) {
      values[key] = value;
    }
  }
  return {
    url: String(input),
    values
  };
}

function minimalConfig(): WebDeviceConfig {
  return {
    version: 1,
    integrationMode: "edge",
    zones: [],
    calibrationZones: [],
    floorplan: {
      enabled: false,
      hasImage: false
    }
  };
}

function largeStats(): WebDeviceStats {
  return {
    today: {
      f: 1,
      r: 2,
      fz: [1, 2, 3],
      rz: [4, 5, 6],
      sz: [7, 8, 9]
    },
    daily: Array.from({ length: 20 }, (_, index) => ({
      d: 20260710 + index,
      f: index,
      r: index + 1,
      fz: [index, index + 1, index + 2],
      rz: [index + 3, index + 4, index + 5],
      sz: [index + 6, index + 7, index + 8]
    }))
  };
}

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe("deviceApi chunked storage uploads", () => {
  it("saves config through the chunk upload endpoints without floorplan target fields", async () => {
    const calls: FormCall[] = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      calls.push(readFormCall(input, init));
      return okResponse();
    }));

    const { deviceApi } = await loadDeviceApi();
    await deviceApi.saveConfig(minimalConfig());

    expect(calls.map((call) => call.url)).toEqual([
      "/api/config/upload/start",
      "/api/config/upload/chunk",
      "/api/config/upload/commit"
    ]);

    const session = calls[0].values.session;
    expect(session).toBeTruthy();
    expect(calls[0].values).toEqual({
      session,
      size: expect.stringMatching(/^\d+$/)
    });
    expect(calls[1].values).toMatchObject({
      session,
      offset: "0",
      data: expect.stringMatching(/^[0-9a-f]+$/)
    });
    expect(calls[2].values).toEqual({ session });
    expect(calls.some((call) => "target" in call.values)).toBe(false);
  });

  it("sends timezone changes through the bounded control endpoint", async () => {
    const calls: FormCall[] = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      calls.push(readFormCall(input, init));
      return okResponse();
    }));

    const { deviceApi } = await loadDeviceApi();
    await deviceApi.setTimezone?.("America/Los_Angeles");

    expect(calls).toEqual([
      {
        url: "/api/control/timezone",
        values: {
          data: JSON.stringify({ timezone: "America/Los_Angeles" })
        }
      }
    ]);
  });

  it("uses bounded control endpoints for the static radar tuning session and gate", async () => {
    const calls: FormCall[] = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      calls.push(readFormCall(input, init));
      return okResponse();
    }));

    const { deviceApi } = await loadDeviceApi();
    await deviceApi.setStaticRadarTuningSession?.(true);
    await deviceApi.setStaticRadarGateSensitivity?.(3, 72);

    expect(calls).toEqual([
      {
        url: "/api/control/static-radar-tuning/session",
        values: { data: JSON.stringify({ active: true }) }
      },
      {
        url: "/api/control/static-radar-tuning/gate",
        values: { data: JSON.stringify({ gate: 3, sensitivity: 72 }) }
      }
    ]);
  });

  it("reads static radar tuning telemetry from the bounded status endpoint", async () => {
    const calls: Array<{ url: string; cache?: RequestCache }> = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      calls.push({ url: String(input), cache: init?.cache });
      return new Response(JSON.stringify({
        ok: true,
        available: true,
        active: true,
        resolutionMm: 750,
        gates: []
      }), { status: 200, headers: { "Content-Type": "application/json" } });
    }));

    const { deviceApi } = await loadDeviceApi();
    const result = await deviceApi.getStaticRadarTuningStatus?.();

    expect(result?.available).toBe(true);
    expect(calls).toEqual([{ url: "/api/control/static-radar-tuning", cache: "no-store" }]);
  });

  it("saves stats through the device base URL and forwards upload progress", async () => {
    const calls: FormCall[] = [];
    const progress: Array<{ loaded: number; total: number; percent: number }> = [];
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      calls.push(readFormCall(input, init));
      return okResponse();
    }));

    const { deviceApi } = await loadDeviceApi("?device=192.168.29.203");
    await deviceApi.saveStats(largeStats(), (nextProgress) => progress.push(nextProgress));

    expect(calls[0].url).toBe("http://192.168.29.203/api/stats/upload/start");
    expect(calls[calls.length - 1].url).toBe("http://192.168.29.203/api/stats/upload/commit");
    expect(calls.filter((call) => call.url.endsWith("/chunk")).length).toBeGreaterThan(1);

    const session = calls[0].values.session;
    expect(calls.every((call) => call.values.session === session)).toBe(true);
    expect(calls[0].values).toEqual({
      session,
      size: expect.stringMatching(/^\d+$/)
    });
    expect(calls[calls.length - 1].values).toEqual({ session });
    expect(calls.some((call) => "target" in call.values)).toBe(false);
    expect(progress[0]).toEqual({ loaded: 0, total: Number(calls[0].values.size), percent: 0 });
    expect(progress[progress.length - 1]).toEqual({
      loaded: Number(calls[0].values.size),
      total: Number(calls[0].values.size),
      percent: 100
    });
  });
});
