import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createRadarPolling } from "./useRadarPolling.svelte";

describe("radar polling", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("window", {
      setInterval: globalThis.setInterval.bind(globalThis),
      clearInterval: globalThis.clearInterval.bind(globalThis)
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("checks replicated floorplan availability on its bounded interval and stops cleanly", async () => {
    const refreshFloorplanAvailability = vi.fn(async () => undefined);
    const polling = createRadarPolling({
      loadConfig: vi.fn(async () => undefined),
      refreshState: vi.fn(async () => undefined),
      refreshFloorplanAvailability,
      intervalMs: 1000,
      floorplanIntervalMs: 5000
    });

    polling.start();
    expect(refreshFloorplanAvailability).not.toHaveBeenCalled();

    await vi.advanceTimersByTimeAsync(5000);
    expect(refreshFloorplanAvailability).toHaveBeenCalledTimes(1);

    polling.destroy();
    await vi.advanceTimersByTimeAsync(5000);
    expect(refreshFloorplanAvailability).toHaveBeenCalledTimes(1);
  });

  it("refreshes the optional platform display without increasing the target polling rate", async () => {
    const refreshState = vi.fn(async () => undefined);
    const refreshPlatformDisplay = vi.fn(async () => undefined);
    const polling = createRadarPolling({
      loadConfig: vi.fn(async () => undefined),
      refreshState,
      refreshPlatformDisplay,
      intervalMs: 500,
      platformIntervalMs: 5000
    });

    polling.start();
    expect(refreshPlatformDisplay).toHaveBeenCalledTimes(1);

    await vi.advanceTimersByTimeAsync(5000);
    expect(refreshState).toHaveBeenCalledTimes(11);
    expect(refreshPlatformDisplay).toHaveBeenCalledTimes(2);

    polling.destroy();
  });
});
