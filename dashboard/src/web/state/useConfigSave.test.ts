import { describe, expect, it, vi } from "vitest";
import type { DeviceApi, WebDeviceConfig } from "../types";
import { createConfigSave } from "./useConfigSave.svelte";

const labels = {
  idle: "idle",
  pending: "pending",
  saving: "saving",
  queued: "queued",
  saved: "saved",
  error: "error"
};

function configFixture(originX: number): WebDeviceConfig {
  return {
    version: 1,
    zones: [],
    calibrationZones: [],
    floorplan: {
      enabled: true,
      hasImage: true,
      radar: {
        originX,
        originY: 20,
        rotation: 0,
        scale: 1
      }
    }
  };
}

describe("config save", () => {
  it("flushes a newer config change queued while a save is in flight", async () => {
    let config = configFixture(10);
    let releaseFirstSave: (() => void) | undefined;
    const firstSaveGate = new Promise<void>((resolve) => {
      releaseFirstSave = resolve;
    });
    const savedOrigins: number[] = [];
    const api = {
      saveConfig: vi.fn(async (saved: WebDeviceConfig) => {
        savedOrigins.push(saved.floorplan?.radar?.originX ?? -1);
        if (savedOrigins.length === 1) await firstSaveGate;
      })
    } as unknown as DeviceApi;
    const saver = createConfigSave({
      api,
      getConfig: () => config,
      setStatus: () => undefined,
      errorMessage: (error) => String(error),
      getSaveStatusLabels: () => labels,
      savedMessage: () => "saved",
      saveFailedMessage: (error) => error
    });

    saver.markPending();
    const firstSave = saver.saveConfigNow();
    await vi.waitFor(() => expect(savedOrigins).toEqual([10]));

    config = configFixture(30);
    saver.markPending();
    const queuedSave = saver.saveConfigNow();
    releaseFirstSave?.();
    await Promise.all([firstSave, queuedSave]);

    expect(savedOrigins).toEqual([10, 30]);
    expect(saver.hasPendingChanges).toBe(false);
    expect(saver.saveState).toBe("saved");
    saver.destroy();
  });

  it("rejects a failed write instead of reporting the save as complete", async () => {
    const api = {
      saveConfig: vi.fn(async () => {
        throw new Error("write failed");
      })
    } as unknown as DeviceApi;
    const saver = createConfigSave({
      api,
      getConfig: () => configFixture(10),
      setStatus: () => undefined,
      errorMessage: (error) => error instanceof Error ? error.message : String(error),
      getSaveStatusLabels: () => labels,
      savedMessage: () => "saved",
      saveFailedMessage: (error) => error
    });

    saver.markPending();
    await expect(saver.saveConfigNow()).rejects.toThrow("write failed");
    expect(saver.hasPendingChanges).toBe(true);
    expect(saver.saveState).toBe("error");
    saver.destroy();
  });
});
