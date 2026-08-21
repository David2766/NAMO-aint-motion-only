import { saveFloorplanStorage } from "../floorplan/floorplan-storage-client";
import { parseApiErrorResponse, type ApiInfo } from "./api-result";
import { uploadChunkedFormPayload } from "./chunked-form-upload";
import { deviceStorageQueue } from "./device-storage-queue";
import { readNamoFirmwareHeaderHex } from "../state/firmware-package";
import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../../core/types";
import type {
  DeviceApi,
  FirmwareUploadProgress,
  WebApiKeyResult,
  WebControlStatus,
  WebFirmwareTrustStatus,
  WebHaSetupHandoffResult,
  WebSystemRebootResult,
  WebSystemResetOptions,
  WebSystemResetResult,
  WebSystemStatus,
  WebStaticRadarTuningStatus
} from "../types";

const deviceBaseUrl = normalizeDeviceBaseUrl(new URLSearchParams(window.location.search).get("device") || "");

class HttpError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly code?: string,
    readonly errorInfo?: ApiInfo,
    readonly legacyError?: string,
    readonly legacyMessage?: string
  ) {
    super(message);
  }
}

function normalizeDeviceBaseUrl(value: string): string {
  const trimmed = value.trim().replace(/\/+$/, "");
  if (!trimmed) return "";
  if (/^https?:\/\//i.test(trimmed)) return trimmed;
  return `http://${trimmed}`;
}

function endpoint(path: string): string {
  return `${deviceBaseUrl}${path}`;
}

async function requestJson<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(endpoint(url), init);
  if (!response.ok) {
    throw await responseError(response);
  }
  return response.json() as Promise<T>;
}

async function requestOk(url: string, init?: RequestInit): Promise<void> {
  const response = await fetch(endpoint(url), init);
  if (!response.ok) {
    throw await responseError(response);
  }
}

async function postJsonData(url: string, value: unknown): Promise<void> {
  const body = new URLSearchParams({
    data: JSON.stringify(value)
  });
  await requestOk(url, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body
  });
}

async function postJsonRequest<T>(url: string, value: unknown): Promise<T> {
  return requestJson<T>(url, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body: new URLSearchParams({
      data: JSON.stringify(value)
    })
  });
}

async function waitForStaticRadarGateOperation(operationId: number): Promise<void> {
  const deadline = Date.now() + 6000;
  while (Date.now() < deadline) {
    const status = await requestJson<WebStaticRadarTuningStatus>("/api/control/static-radar-tuning", {
      cache: "no-store"
    });
    const operation = status.operation;
    if (!operation) throw new Error("static_radar_apply_protocol_unsupported");
    if (operation.id !== operationId) throw new Error("static_radar_apply_operation_lost");
    if (operation.state === "applied") return;
    if (operation.state === "failed") throw new Error(operation.result || "static_radar_apply_failed");
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  throw new Error("static_radar_apply_timeout");
}

async function uploadJsonPayload(
  paths: { start: string; chunk: string; commit: string },
  value: unknown,
  onProgress?: (progress: FirmwareUploadProgress) => void
): Promise<void> {
  const bytes = new TextEncoder().encode(JSON.stringify(value));
  await uploadChunkedFormPayload({ paths, bytes, postForm, onProgress });
}

async function postForm(url: string, values: Record<string, string>): Promise<void> {
  await requestOk(url, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body: new URLSearchParams(values)
  });
}

async function responseError(response: Response): Promise<HttpError> {
  const parsed = await parseApiErrorResponse(response);
  return new HttpError(
    parsed.message,
    parsed.status,
    parsed.code,
    parsed.errorInfo,
    parsed.legacyError,
    parsed.legacyMessage
  );
}

function firmwareUploadError(status: number, code: string, legacyMessage?: string): HttpError {
  return new HttpError(code, status, code, { code, severity: "error" }, undefined, legacyMessage);
}

async function firmwareUploadResponseError(xhr: XMLHttpRequest): Promise<HttpError> {
  if (xhr.status <= 0) return firmwareUploadError(0, "firmware_network_error");
  const contentType = xhr.getResponseHeader("content-type");
  const headers = new Headers();
  if (contentType) headers.set("content-type", contentType);
  const parsed = await responseError(new Response(xhr.responseText || "", {
    status: xhr.status,
    statusText: xhr.statusText,
    headers
  }));
  return parsed.code
    ? parsed
    : firmwareUploadError(xhr.status, "firmware_upload_failed", parsed.message);
}

function uploadFirmwareForm(
  path: string,
  file: File,
  onProgress: (progress: FirmwareUploadProgress) => void,
  signed: boolean
): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const form = new FormData();
    form.append("file", file, file.name);

    xhr.upload.onprogress = (event) => {
      if (!event.lengthComputable) return;
      onProgress({
        loaded: event.loaded,
        total: event.total,
        percent: Math.min(100, Math.max(0, Math.round((event.loaded / event.total) * 100)))
      });
    };

    xhr.onload = () => {
      let signedOk = false;
      if (signed && xhr.status >= 200 && xhr.status < 300) {
        try {
          signedOk = JSON.parse(xhr.responseText || "{}").ok === true;
        } catch {
          signedOk = false;
        }
      }
      const ok = signed ? signedOk : xhr.status >= 200 && xhr.status < 300 && /successful/i.test(xhr.responseText || "");
      if (ok) {
        onProgress({ loaded: file.size, total: file.size, percent: 100 });
        resolve();
        return;
      }
      void firmwareUploadResponseError(xhr).then(reject);
    };

    xhr.onerror = () => reject(firmwareUploadError(0, "firmware_network_error"));
    xhr.onabort = () => reject(firmwareUploadError(0, "firmware_upload_aborted"));
    xhr.open("POST", endpoint(path));
    xhr.send(form);
  });
}

interface FirmwareTrustResponse {
  ok: boolean;
  data?: {
    mode?: string;
    ready?: boolean;
    trustRevision?: number;
    securityEpoch?: number;
    generation?: number;
    releaseKeyId?: string;
    developerKeyId?: string;
    sessionActive?: boolean;
    alphaInstallSupported?: boolean;
    alphaInstallAllowed?: boolean;
    alphaInstallRemainingSeconds?: number;
  };
}

function normalizeFirmwareTrust(response: FirmwareTrustResponse): WebFirmwareTrustStatus {
  return {
    ok: response.ok,
    mode: response.data?.mode ?? "signed",
    ready: response.data?.ready === true,
    trustRevision: response.data?.trustRevision,
    securityEpoch: response.data?.securityEpoch,
    generation: response.data?.generation,
    releaseKeyId: response.data?.releaseKeyId,
    developerKeyId: response.data?.developerKeyId,
    sessionActive: response.data?.sessionActive,
    alphaInstallSupported: response.data?.alphaInstallSupported === true,
    alphaInstallAllowed: response.data?.alphaInstallAllowed === true,
    alphaInstallRemainingSeconds: Math.max(0, Math.floor(response.data?.alphaInstallRemainingSeconds ?? 0))
  };
}

interface FirmwareStartResponse {
  ok: boolean;
  data?: {
    session?: number;
  };
}

async function uploadFirmwareFile(file: File, onProgress: (progress: FirmwareUploadProgress) => void): Promise<void> {
  const trustResponse = await fetch(endpoint("/api/firmware/trust"), { cache: "no-store" });
  if (trustResponse.status === 404) {
    if (!file.name.toLowerCase().endsWith(".bin")) throw firmwareUploadError(400, "legacy_firmware_bin_required");
    await uploadFirmwareForm("/update", file, onProgress, false);
    return;
  }
  if (!trustResponse.ok) throw await responseError(trustResponse);
  const trust = await trustResponse.json() as FirmwareTrustResponse;
  if (!trust.ok || trust.data?.mode !== "signed" || trust.data.ready !== true) {
    throw firmwareUploadError(503, "firmware_trust_unavailable");
  }
  const header = await readNamoFirmwareHeaderHex(file).catch(() => {
    throw firmwareUploadError(400, "signed_firmware_required");
  });
  const start = await requestJson<FirmwareStartResponse>("/api/firmware/update/start", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ header })
  });
  const session = start.data?.session;
  if (!start.ok || !Number.isSafeInteger(session) || Number(session) <= 0) {
    throw firmwareUploadError(500, "firmware_session_invalid");
  }
  await uploadFirmwareForm(
    `/api/firmware/update?session=${session}`,
    file,
    onProgress,
    true
  );
}

interface StoredStatsFile extends Partial<WebDeviceStats> {
  version?: number;
  updatedAt?: number;
}

function emptyDeviceConfig(): WebDeviceConfig {
  return {
    version: 1,
    integrationMode: "unknown",
    legacyPresenceFallback: false,
    zones: [],
    calibrationZones: [],
    floorplan: {
      enabled: false,
      hasImage: false
    }
  };
}

function isMissingDeviceConfig(error: unknown): boolean {
  return error instanceof HttpError && error.status === 404 && /config_not_found|not_found|404/i.test(error.message);
}

export const deviceApi: DeviceApi = {
  async getState(): Promise<WebDeviceState> {
    const state = await requestJson<WebDeviceState>("/api/state");
    return {
      ...state,
      updatedAt: Date.now()
    };
  },

  async getConfig(): Promise<WebDeviceConfig> {
    try {
      return await requestJson<WebDeviceConfig>("/api/config");
    } catch (error) {
      if (isMissingDeviceConfig(error)) {
        return emptyDeviceConfig();
      }
      throw error;
    }
  },

  async getStats(): Promise<WebDeviceStats> {
    const storedStats = await requestJson<StoredStatsFile>("/api/stats");
    return {
      today: storedStats.today ?? null,
      daily: Array.isArray(storedStats.daily) ? storedStats.daily : [],
      summary: storedStats.summary,
      heatmap: storedStats.heatmap
    };
  },

  async getSystemStatus(): Promise<WebSystemStatus> {
    return requestJson<WebSystemStatus>("/api/system/status");
  },

  async getFirmwareTrust(): Promise<WebFirmwareTrustStatus> {
    const response = await fetch(endpoint("/api/firmware/trust"), { cache: "no-store" });
    if (response.status === 404) {
      return {
        ok: true,
        mode: "legacy",
        ready: false,
        alphaInstallSupported: false,
        alphaInstallAllowed: false,
        alphaInstallRemainingSeconds: 0
      };
    }
    if (!response.ok) throw await responseError(response);
    return normalizeFirmwareTrust(await response.json() as FirmwareTrustResponse);
  },

  async setAlphaFirmwareInstallAllowed(enabled: boolean): Promise<WebFirmwareTrustStatus> {
    const response = await requestJson<FirmwareTrustResponse>("/api/firmware/alpha-permission", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({ enabled: String(enabled) })
    });
    return normalizeFirmwareTrust(response);
  },

  async getControlStatus(): Promise<WebControlStatus> {
    return requestJson<WebControlStatus>("/api/control/status");
  },

  async getStaticRadarTuningStatus(): Promise<WebStaticRadarTuningStatus> {
    return requestJson<WebStaticRadarTuningStatus>("/api/control/static-radar-tuning", { cache: "no-store" });
  },

  async saveConfig(config: WebDeviceConfig): Promise<void> {
    await deviceStorageQueue.run("config", () =>
      uploadJsonPayload(
        {
          start: "/api/config/upload/start",
          chunk: "/api/config/upload/chunk",
          commit: "/api/config/upload/commit"
        },
        config
      )
    );
  },

  async saveStats(stats: WebDeviceStats, onProgress?: (progress: FirmwareUploadProgress) => void): Promise<void> {
    await deviceStorageQueue.run("stats", () =>
      uploadJsonPayload(
        {
          start: "/api/stats/upload/start",
          chunk: "/api/stats/upload/chunk",
          commit: "/api/stats/upload/commit"
        },
        stats,
        onProgress
      )
    );
  },

  async setStatusLed(enabled: boolean): Promise<void> {
    await postJsonData("/api/control/status-led", { enabled });
  },

  async setLedBlinkDuration(seconds: number): Promise<void> {
    await postJsonData("/api/control/led-duration", { seconds });
  },

  async setEnvironmentCorrection(enabled: boolean): Promise<void> {
    await postJsonData("/api/control/environment-correction", { enabled });
  },

  async setTemperatureOffset(value: number): Promise<void> {
    await postJsonData("/api/control/temperature-offset", { value });
  },

  async setHumidityOffset(value: number): Promise<void> {
    await postJsonData("/api/control/humidity-offset", { value });
  },

  async setTimezone(timezone: string): Promise<void> {
    await postJsonData("/api/control/timezone", { timezone });
  },

  async setStaticRadarTuningSession(active: boolean): Promise<void> {
    await postJsonData("/api/control/static-radar-tuning/session", { active });
  },

  async setStaticRadarGateSensitivity(gate: number, sensitivity: number): Promise<void> {
    const result = await postJsonRequest<{ ok: boolean; operationId: number }>(
      "/api/control/static-radar-tuning/gate",
      { gate, sensitivity }
    );
    if (!Number.isInteger(result.operationId) || result.operationId <= 0) {
      throw new Error("static_radar_apply_protocol_unsupported");
    }
    await waitForStaticRadarGateOperation(result.operationId);
  },

  async resetStaticRadarFactoryDefaults(): Promise<void> {
    await postJsonData("/api/control/static-radar-tuning/factory-reset", {});
  },

  async saveFloorplan(document, image): Promise<void> {
    await deviceStorageQueue.run("floorplan", () => saveFloorplanStorage({ document, image }, { baseUrl: deviceBaseUrl }));
  },

  async uploadFirmware(file, onProgress): Promise<void> {
    await uploadFirmwareFile(file, onProgress);
  },

  async resetSystem(options: WebSystemResetOptions): Promise<WebSystemResetResult> {
    return postJsonRequest<WebSystemResetResult>("/api/system/reset", options);
  },

  async rebootSystem(): Promise<WebSystemRebootResult> {
    return postJsonRequest<WebSystemRebootResult>("/api/system/reboot", {});
  },

  async getApiKey(): Promise<WebApiKeyResult> {
    return requestJson<WebApiKeyResult>("/api/system/api-key", { cache: "no-store" });
  },

  async rotateApiKey(): Promise<WebApiKeyResult> {
    return postJsonRequest<WebApiKeyResult>("/api/system/api-key/rotate", {});
  },

  async completeHaSetupHandoff(): Promise<WebHaSetupHandoffResult> {
    return postJsonRequest<WebHaSetupHandoffResult>("/api/system/ha-setup-handoff", {});
  }
};
