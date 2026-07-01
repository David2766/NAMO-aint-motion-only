import { saveFloorplanStorage } from "../floorplan/floorplan-storage-client";
import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../../core/types";
import type { DeviceApi, FirmwareUploadProgress, WebControlStatus, WebSystemStatus } from "../types";

const deviceBaseUrl = normalizeDeviceBaseUrl(new URLSearchParams(window.location.search).get("device") || "");
const UPLOAD_CHUNK_BYTES = 192;

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
    throw new Error(await responseErrorText(response));
  }
  return response.json() as Promise<T>;
}

async function requestOk(url: string, init?: RequestInit): Promise<void> {
  const response = await fetch(endpoint(url), init);
  if (!response.ok) {
    throw new Error(await responseErrorText(response));
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

async function uploadJsonPayload(
  paths: { start: string; chunk: string; commit: string },
  value: unknown,
  onProgress?: (progress: FirmwareUploadProgress) => void
): Promise<void> {
  const bytes = new TextEncoder().encode(JSON.stringify(value));
  onProgress?.({ loaded: 0, total: bytes.byteLength, percent: 0 });

  await postForm(paths.start, { size: String(bytes.byteLength) });

  for (let offset = 0; offset < bytes.byteLength; offset += UPLOAD_CHUNK_BYTES) {
    const chunk = bytes.subarray(offset, Math.min(offset + UPLOAD_CHUNK_BYTES, bytes.byteLength));
    await postForm(paths.chunk, {
      offset: String(offset),
      data: bytesToHex(chunk)
    });
    const loaded = Math.min(bytes.byteLength, offset + chunk.byteLength);
    onProgress?.({
      loaded,
      total: bytes.byteLength,
      percent: Math.min(99, Math.max(0, Math.round((loaded / bytes.byteLength) * 100)))
    });
  }

  await postForm(paths.commit, {});
  onProgress?.({ loaded: bytes.byteLength, total: bytes.byteLength, percent: 100 });
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

function bytesToHex(bytes: Uint8Array): string {
  let hex = "";
  for (const byte of bytes) {
    hex += byte.toString(16).padStart(2, "0");
  }
  return hex;
}

async function responseErrorText(response: Response): Promise<string> {
  const fallback = `${response.status} ${response.statusText}`;
  try {
    const contentType = response.headers.get("content-type") || "";
    if (contentType.includes("application/json")) {
      const body = (await response.json()) as { error?: string; message?: string };
      return [fallback, body.error || body.message].filter(Boolean).join(" ");
    }
    const text = await response.text();
    return text ? `${fallback} ${text}` : fallback;
  } catch {
    return fallback;
  }
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
      reject(new Error(xhr.responseText || `${xhr.status} ${xhr.statusText}`));
    };

    xhr.onerror = () => reject(new Error("펌웨어 업로드 중 네트워크 오류가 발생했습니다."));
    xhr.onabort = () => reject(new Error("펌웨어 업로드가 취소되었습니다."));
    xhr.open("POST", endpoint("/update"));
    xhr.send(form);
  });
}

interface StoredStatsFile extends Partial<WebDeviceStats> {
  version?: number;
  updatedAt?: number;
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
    return requestJson<WebDeviceConfig>("/api/config");
  },

  async getStats(): Promise<WebDeviceStats> {
    const storedStats = await requestJson<StoredStatsFile>("/api/stats").catch(() => null);
    if (storedStats) {
      return {
        today: storedStats.today ?? null,
        daily: Array.isArray(storedStats.daily) ? storedStats.daily : [],
        summary: storedStats.summary,
        heatmap: storedStats.heatmap
      };
    }

    throw new Error("Stats API is not available");
  },

  async getSystemStatus(): Promise<WebSystemStatus> {
    return requestJson<WebSystemStatus>("/api/system/status");
  },

  async getControlStatus(): Promise<WebControlStatus> {
    return requestJson<WebControlStatus>("/api/control/status");
  },

  async saveConfig(config: WebDeviceConfig): Promise<void> {
    await postJsonData("/api/config", config);
  },

  async saveStats(stats: WebDeviceStats, onProgress?: (progress: FirmwareUploadProgress) => void): Promise<void> {
    await uploadJsonPayload(
      {
        start: "/api/stats/upload/start",
        chunk: "/api/stats/upload/chunk",
        commit: "/api/stats/upload/commit"
      },
      stats,
      onProgress
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

  async saveFloorplan(document, image): Promise<void> {
    await saveFloorplanStorage({ document, image }, { baseUrl: deviceBaseUrl });
  },

  async uploadFirmware(file, onProgress): Promise<void> {
    await uploadFirmwareFile(file, onProgress);
  }
};
