import { get, writable } from "svelte/store";
import type { FirmwareUploadProgress, WebSystemStatus } from "../types";
import type { FirmwareImageMetadata } from "./firmware-image";
import { readFirmwareFileMetadata } from "./firmware-package";

export type FirmwareUpdateStage =
  | "idle"
  | "selected"
  | "uploading"
  | "verifying"
  | "success"
  | "error"
  | "verify-error";

export type FirmwareUpdateStatus =
  | "idle"
  | "file_ready"
  | "bin_only"
  | "file_too_large"
  | "image_invalid"
  | "api_unavailable"
  | "uploading"
  | "response_lost"
  | "rebooting"
  | "waiting_response"
  | "checking_applied"
  | "upload_interrupted"
  | "verify_timeout"
  | "upload_failed"
  | "complete";

export type FirmwareUpdateResult = "updated" | "rebuilt" | "reinstalled" | "unknown";

export interface FirmwareStatusSnapshot {
  nodeId?: string;
  version?: string;
  buildTime?: string;
  dashboardVersion?: string;
  uptimeSeconds?: number;
  capturedAt: number;
}

export interface FirmwareUpdateState {
  stage: FirmwareUpdateStage;
  status: FirmwareUpdateStatus;
  file: File | null;
  fileName: string;
  fileSize: number;
  target: FirmwareImageMetadata | null;
  before: FirmwareStatusSnapshot | null;
  after: FirmwareStatusSnapshot | null;
  result: FirmwareUpdateResult | null;
  percent: number;
  loaded: number;
  total: number;
  otaSlotBytes?: number;
  errorMessage: string;
  startedAt: number;
}

export interface FirmwareUpdateCompletion {
  schemaVersion: 1;
  completedAt: number;
  before: FirmwareStatusSnapshot | null;
  after: FirmwareStatusSnapshot;
  result: FirmwareUpdateResult;
  target: FirmwareImageMetadata | null;
}

type PersistedFirmwareUpdate = Omit<FirmwareUpdateState, "file"> & {
  schemaVersion: 1;
};

type FirmwareUpdateDependencies = {
  getSystemStatus: () => Promise<WebSystemStatus>;
  uploadFirmware: (file: File, onProgress: (progress: FirmwareUploadProgress) => void) => Promise<void>;
  formatError: (error: unknown) => string;
  reload?: () => void;
};

const SESSION_MAX_AGE_MS = 24 * 60 * 60 * 1000;
const VERIFY_TIMEOUT_MS = 180000;
const VERIFY_POLL_MS = 2000;

function emptyState(): FirmwareUpdateState {
  return {
    stage: "idle",
    status: "idle",
    file: null,
    fileName: "",
    fileSize: 0,
    target: null,
    before: null,
    after: null,
    result: null,
    percent: 0,
    loaded: 0,
    total: 0,
    errorMessage: "",
    startedAt: 0
  };
}

function sessionKey(): string {
  if (typeof window === "undefined") return "namo.firmware-update";
  const target = new URLSearchParams(window.location.search).get("device")?.trim() || window.location.origin;
  return `namo.firmware-update:${target}`;
}

function completionKey(): string {
  if (typeof window === "undefined") return "namo.firmware-update-completion";
  const target = new URLSearchParams(window.location.search).get("device")?.trim() || window.location.origin;
  return `namo.firmware-update-completion:${target}`;
}

export function saveFirmwareUpdateCompletion(completion: FirmwareUpdateCompletion): void {
  if (typeof sessionStorage === "undefined") return;
  sessionStorage.setItem(completionKey(), JSON.stringify(completion));
}

export function takeFirmwareUpdateCompletion(): FirmwareUpdateCompletion | null {
  if (typeof sessionStorage === "undefined") return null;
  const key = completionKey();
  const value = sessionStorage.getItem(key);
  sessionStorage.removeItem(key);
  if (!value) return null;
  try {
    const completion = JSON.parse(value) as FirmwareUpdateCompletion;
    if (
      completion.schemaVersion !== 1 ||
      !Number.isFinite(completion.completedAt) ||
      Date.now() - completion.completedAt > SESSION_MAX_AGE_MS ||
      !completion.after ||
      !completion.result
    ) {
      return null;
    }
    return completion;
  } catch {
    return null;
  }
}

function loadPersistedState(): FirmwareUpdateState {
  if (typeof sessionStorage === "undefined") return emptyState();
  try {
    const value = sessionStorage.getItem(sessionKey());
    if (!value) return emptyState();
    const parsed = JSON.parse(value) as PersistedFirmwareUpdate;
    if (parsed.schemaVersion !== 1 || Date.now() - parsed.startedAt > SESSION_MAX_AGE_MS) {
      sessionStorage.removeItem(sessionKey());
      return emptyState();
    }
    if (parsed.stage === "success") {
      sessionStorage.removeItem(sessionKey());
      return emptyState();
    }
    if (parsed.stage === "uploading") {
      return {
        ...parsed,
        file: null,
        stage: "verify-error",
        status: "upload_interrupted"
      };
    }
    return { ...parsed, file: null };
  } catch {
    sessionStorage.removeItem(sessionKey());
    return emptyState();
  }
}

export function firmwareUpdateStagePersists(stage: FirmwareUpdateStage): boolean {
  return stage !== "idle" && stage !== "selected" && stage !== "success";
}

function persistState(state: FirmwareUpdateState): void {
  if (typeof sessionStorage === "undefined") return;
  if (!firmwareUpdateStagePersists(state.stage)) {
    sessionStorage.removeItem(sessionKey());
    return;
  }
  const { file: _file, ...persisted } = state;
  sessionStorage.setItem(sessionKey(), JSON.stringify({ schemaVersion: 1, ...persisted }));
}

function snapshot(status: WebSystemStatus): FirmwareStatusSnapshot {
  return {
    nodeId: status.platform?.nodeId,
    version: status.firmware?.version,
    buildTime: status.firmware?.buildTime,
    dashboardVersion: status.dashboard?.version,
    uptimeSeconds: status.firmware?.uptimeSeconds,
    capturedAt: Date.now()
  };
}

export function firmwareRebootObserved(before: FirmwareStatusSnapshot | null, after: FirmwareStatusSnapshot): boolean {
  if (!before) return false;
  if (before.nodeId && after.nodeId && before.nodeId !== after.nodeId) return false;
  if (before.version !== after.version || before.buildTime !== after.buildTime) return true;
  if (!Number.isFinite(before.uptimeSeconds) || !Number.isFinite(after.uptimeSeconds)) return false;
  const elapsedSeconds = Math.max(0, (after.capturedAt - before.capturedAt) / 1000);
  const expectedWithoutReboot = Number(before.uptimeSeconds) + elapsedSeconds;
  return Number(after.uptimeSeconds) + 3 < expectedWithoutReboot;
}

export function firmwareUpdateResult(
  before: FirmwareStatusSnapshot | null,
  after: FirmwareStatusSnapshot
): FirmwareUpdateResult {
  if (!before) return "unknown";
  if (before.version !== after.version) return "updated";
  if (before.buildTime !== after.buildTime) return "rebuilt";
  return "reinstalled";
}

export function firmwareUploadResponseMayBeLost(error: unknown): boolean {
  if (typeof error !== "object" || error === null) return false;
  const candidate = error as { status?: unknown; code?: unknown };
  return candidate.status === 0 &&
    (candidate.code === "firmware_network_error" || candidate.code === "firmware_upload_aborted");
}

const stateStore = writable<FirmwareUpdateState>(loadPersistedState());
let verificationTask: Promise<void> | null = null;

function replace(next: FirmwareUpdateState): void {
  stateStore.set(next);
  persistState(next);
}

function patch(values: Partial<FirmwareUpdateState>): FirmwareUpdateState {
  const next = { ...get(stateStore), ...values };
  replace(next);
  return next;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function verifyFirmware(dependencies: FirmwareUpdateDependencies, initialDelay: boolean): Promise<void> {
  if (verificationTask) return verificationTask;
  verificationTask = (async () => {
    if (initialDelay) await delay(3000);
    const deadline = Date.now() + VERIFY_TIMEOUT_MS;
    let attempts = 0;

    while (Date.now() < deadline) {
      attempts += 1;
      patch({
        stage: "verifying",
        status: attempts <= 2 ? "rebooting" : attempts <= 8 ? "waiting_response" : "checking_applied"
      });
      try {
        const current = get(stateStore);
        const after = snapshot(await dependencies.getSystemStatus());
        if (current.before?.nodeId && after.nodeId && current.before.nodeId !== after.nodeId) {
          patch({ stage: "verify-error", status: "upload_interrupted" });
          return;
        }
        if (firmwareRebootObserved(current.before, after)) {
          const result = firmwareUpdateResult(current.before, after);
          saveFirmwareUpdateCompletion({
            schemaVersion: 1,
            completedAt: Date.now(),
            before: current.before,
            after,
            result,
            target: current.target
          });
          patch({
            stage: "success",
            status: "complete",
            file: null,
            after,
            result,
            percent: 100,
            loaded: current.fileSize || current.loaded,
            total: current.fileSize || current.total
          });
          await delay(1000);
          (dependencies.reload ?? (() => window.location.reload()))();
          return;
        }
      } catch {
        // A short disconnect is expected while the device changes OTA slots.
      }
      await delay(VERIFY_POLL_MS);
    }

    patch({ stage: "verify-error", status: "verify_timeout" });
  })().finally(() => {
    verificationTask = null;
  });
  return verificationTask;
}

export const firmwareUpdateStore = {
  subscribe: stateStore.subscribe,

  async selectFile(file: File, otaSlotBytes?: number): Promise<void> {
    const lowerName = file.name.toLowerCase();
    if (!lowerName.endsWith(".bin") && !lowerName.endsWith(".namo-ota")) {
      replace({ ...emptyState(), stage: "error", status: "bin_only", fileName: file.name, fileSize: file.size });
      return;
    }
    const signedOverhead = lowerName.endsWith(".namo-ota") ? 320 : 0;
    if (Number.isFinite(otaSlotBytes) && file.size - signedOverhead > Number(otaSlotBytes)) {
      replace({
        ...emptyState(),
        stage: "error",
        status: "file_too_large",
        fileName: file.name,
        fileSize: file.size,
        otaSlotBytes
      });
      return;
    }
    try {
      const target = await readFirmwareFileMetadata(file);
      replace({
        ...emptyState(),
        stage: "selected",
        status: "file_ready",
        file,
        fileName: file.name,
        fileSize: file.size,
        total: file.size,
        target,
        otaSlotBytes
      });
    } catch {
      replace({ ...emptyState(), stage: "error", status: "image_invalid", fileName: file.name, fileSize: file.size });
    }
  },

  async start(dependencies: FirmwareUpdateDependencies): Promise<void> {
    const selected = get(stateStore);
    if (!selected.file || selected.stage !== "selected") return;
    const before = await dependencies.getSystemStatus().then(snapshot).catch(() => null);
    patch({
      stage: "uploading",
      status: "uploading",
      before,
      after: null,
      result: null,
      percent: 0,
      loaded: 0,
      total: selected.file.size,
      errorMessage: "",
      startedAt: Date.now()
    });
    try {
      await dependencies.uploadFirmware(selected.file, (progress) => {
        patch({ percent: progress.percent, loaded: progress.loaded, total: progress.total });
      });
      patch({
        stage: "verifying",
        status: "rebooting",
        percent: 100,
        loaded: selected.file.size,
        total: selected.file.size
      });
      await verifyFirmware(dependencies, true);
    } catch (error) {
      if (firmwareUploadResponseMayBeLost(error)) {
        patch({
          stage: "verifying",
          status: "response_lost",
          percent: 100,
          loaded: selected.file.size,
          total: selected.file.size
        });
        await verifyFirmware(dependencies, true);
        return;
      }
      patch({
        stage: "error",
        status: "upload_failed",
        errorMessage: dependencies.formatError(error)
      });
    }
  },

  async resume(dependencies: Omit<FirmwareUpdateDependencies, "uploadFirmware" | "formatError">): Promise<void> {
    const current = get(stateStore);
    if (current.stage !== "verifying") return;
    await verifyFirmware(
      {
        ...dependencies,
        uploadFirmware: async () => undefined,
        formatError: () => ""
      },
      false
    );
  },

  fail(status: Extract<FirmwareUpdateStatus, "api_unavailable">): void {
    replace({ ...emptyState(), stage: "error", status });
  },

  clear(): void {
    verificationTask = null;
    replace(emptyState());
  }
};
