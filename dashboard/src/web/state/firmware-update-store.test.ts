import { describe, expect, it } from "vitest";
import {
  firmwareRebootObserved,
  saveFirmwareUpdateCompletion,
  takeFirmwareUpdateCompletion,
  firmwareUpdateStagePersists,
  firmwareUpdateResult,
  firmwareUploadResponseMayBeLost,
  type FirmwareStatusSnapshot
} from "./firmware-update-store";

function status(values: Partial<FirmwareStatusSnapshot>): FirmwareStatusSnapshot {
  return {
    nodeId: "node-a",
    version: "0.5.5-a",
    buildTime: "Aug  4 2026 10:00:00",
    uptimeSeconds: 120,
    capturedAt: 100000,
    ...values
  };
}

describe("firmware reboot verification", () => {
  it("does not treat a normal same-build response as a reboot", () => {
    expect(firmwareRebootObserved(status({}), status({ uptimeSeconds: 125, capturedAt: 105000 }))).toBe(false);
  });

  it("recognizes an uptime reset for an identical firmware reinstall", () => {
    expect(firmwareRebootObserved(status({}), status({ uptimeSeconds: 3, capturedAt: 106000 }))).toBe(true);
    expect(firmwareUpdateResult(status({}), status({ uptimeSeconds: 3, capturedAt: 106000 }))).toBe("reinstalled");
  });

  it("distinguishes a new version from a same-version rebuild", () => {
    expect(firmwareUpdateResult(status({}), status({ version: "0.5.5-b" }))).toBe("updated");
    expect(firmwareUpdateResult(status({}), status({ buildTime: "Aug  4 2026 11:00:00" }))).toBe("rebuilt");
  });

  it("treats a transport disconnect as an unknown response regardless of progress", () => {
    expect(firmwareUploadResponseMayBeLost(
      { status: 0, code: "firmware_upload_failed" }
    )).toBe(false);
    expect(firmwareUploadResponseMayBeLost(
      { status: 400, code: "offset_mismatch" }
    )).toBe(false);
    expect(firmwareUploadResponseMayBeLost(
      { status: 0, code: "firmware_upload_aborted" }
    )).toBe(true);
    expect(firmwareUploadResponseMayBeLost(
      { status: 0, code: "firmware_network_error" }
    )).toBe(true);
  });

  it("retains OTA failures for diagnostics but not an unstarted selection", () => {
    expect(firmwareUpdateStagePersists("error")).toBe(true);
    expect(firmwareUpdateStagePersists("verify-error")).toBe(true);
    expect(firmwareUpdateStagePersists("uploading")).toBe(true);
    expect(firmwareUpdateStagePersists("idle")).toBe(false);
    expect(firmwareUpdateStagePersists("selected")).toBe(false);
    expect(firmwareUpdateStagePersists("success")).toBe(false);
  });

  it("consumes a successful update receipt exactly once", () => {
    const values = new Map<string, string>();
    const storage = {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
      removeItem: (key: string) => values.delete(key),
      clear: () => values.clear(),
      key: (index: number) => [...values.keys()][index] ?? null,
      get length() { return values.size; }
    } satisfies Storage;
    Object.defineProperty(globalThis, "sessionStorage", {
      configurable: true,
      value: storage
    });
    const after = status({ version: "0.5.5-b", uptimeSeconds: 3, capturedAt: Date.now() });
    saveFirmwareUpdateCompletion({
      schemaVersion: 1,
      completedAt: Date.now(),
      before: status({ capturedAt: Date.now() - 6000 }),
      after,
      result: "updated",
      target: null
    });
    expect(takeFirmwareUpdateCompletion()?.after.version).toBe("0.5.5-b");
    expect(takeFirmwareUpdateCompletion()).toBeNull();
    delete (globalThis as { sessionStorage?: Storage }).sessionStorage;
  });
});
