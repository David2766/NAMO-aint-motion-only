import { saveFloorplanStorage } from "../floorplan/floorplan-storage-client";
import { parseApiErrorResponse, type ApiInfo } from "./api-result";
import { uploadChunkedFormPayload } from "./chunked-form-upload";
import { deviceStorageQueue } from "./device-storage-queue";
import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../../core/types";
import type {
  DeviceApi,
  FirmwareUploadProgress,
  WebApiKeyResult,
  WebControlStatus,
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

function uploadFirmwareFile(file: File, onProgress: (progress: FirmwareUploadProgress) => void): Promise<void> {
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
      const ok = xhr.status >= 200 && xhr.status < 300 && /successful/i.test(xhr.responseText || "");
      if (ok) {
        onProgress({ loaded: file.size, total: file.size, percent: 100 });
        resolve();
        return;
      }
      reject(firmwareUploadError(xhr.status, "firmware_upload_failed", xhr.responseText || `${xhr.status} ${xhr.statusText}`));
    };

    xhr.onerror = () => reject(firmwareUploadError(0, "firmware_network_error"));
    xhr.onabort = () => reject(firmwareUploadError(0, "firmware_upload_aborted"));
    xhr.open("POST", endpoint("/update"));
    xhr.send(form);
  });
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
    await postJsonData("/api/control/static-radar-tuning/gate", { gate, sensitivity });
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

  async completeHaSetupHandoff(): Promise<WebHaSetupHandoffResult> {
    return postJsonRequest<WebHaSetupHandoffResult>("/api/system/ha-setup-handoff", {});
  }
};
