import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../../core/types";
import type { DeviceApi, WebSystemStatus } from "../types";

const startTime = Date.now();

let config: WebDeviceConfig = {
  version: 1,
  zones: [
    {
      id: "zone_1",
      name: "침대",
      type: "detection",
      shape: "rect",
      points: [
        [-900, 1000],
        [900, 1000],
        [900, 2600],
        [-900, 2600]
      ]
    },
    {
      id: "filter_1",
      name: "커튼 오탐",
      type: "filter",
      shape: "rect",
      points: [
        [1800, 2600],
        [2600, 2600],
        [2600, 3800],
        [1800, 3800]
      ]
    }
  ],
  calibrationZones: [],
  floorplan: {
    enabled: false,
    hasImage: false
  }
};
let floorplanDocument: unknown = null;
let floorplanImage: Blob | null = null;
let statusLedEnabled = true;
let ledBlinkDuration = 60;
let environmentCorrectionEnabled = true;
let temperatureOffset = 0;
let humidityOffset = 0;
let stats: WebDeviceStats = {
  today: {
    d: 20260622,
    f: 14,
    r: 6,
    fz: [8, 4, 2, 0],
    rz: [3, 2, 1, 0],
    sz: [12, 7, 3, 0, 0, 1]
  },
  daily: [
    { d: 20260621, f: 18, r: 9, fz: [9, 6, 3, 0], rz: [5, 3, 1, 0], sz: [14, 8, 4, 1, 0, 2] },
    { d: 20260620, f: 11, r: 5, fz: [6, 4, 1, 0], rz: [3, 1, 1, 0], sz: [9, 5, 2, 0, 0, 1] },
    { d: 20260618, f: 7, r: 2, fz: [4, 2, 1, 0], rz: [1, 1, 0, 0], sz: [6, 3, 1, 0, 0, 0] }
  ],
  heatmap: {
    version: 1,
    cols: 33,
    rows: 26,
    cellMm: 300,
    encoding: "rle",
    today: "",
    daily: []
  }
};

export const mockApi: DeviceApi = {
  async getState(): Promise<WebDeviceState> {
    const elapsed = (Date.now() - startTime) / 1000;
    return {
      connected: true,
      updatedAt: Date.now(),
      pirMotion: false,
      targets: [
        {
          id: "target_1",
          name: "T1",
          color: "#ff6b7a",
          x: Math.round(Math.sin(elapsed / 2) * 1100),
          y: Math.round(1800 + Math.cos(elapsed / 2.8) * 700),
          active: true
        },
        {
          id: "target_2",
          name: "T2",
          color: "#ffd166",
          x: 0,
          y: 0,
          active: false
        },
        {
          id: "target_3",
          name: "T3",
          color: "#06d6a0",
          x: 0,
          y: 0,
          active: false
        }
      ]
    };
  },

  async getConfig(): Promise<WebDeviceConfig> {
    return structuredClone(config);
  },

  async getStats(): Promise<WebDeviceStats> {
    return structuredClone(stats);
  },

  async getSystemStatus(): Promise<WebSystemStatus> {
    return {
      ok: true,
      firmware: {
        version: "0.4.0",
        buildTime: "mock",
        uptimeSeconds: Math.round((Date.now() - startTime) / 1000)
      },
      dashboard: {
        version: "0.4.0",
        gzipBytes: 92000
      },
      schema: {
        config: 1,
        floorplan: 1,
        stats: 1
      },
      memory: {
        freeHeap: 6370000,
        minFreeHeap: 152000,
        internalTotalBytes: 327680,
        internalFreeBytes: 250220,
        internalMinFreeBytes: 180000,
        psramTotal: 8388608,
        psramFree: 6120000,
        externalTotalBytes: 8388608,
        externalFreeBytes: 6120000
      },
      flash: {
        totalBytes: 8388608,
        firmwareUsedBytes: 1561599,
        firmwareSlotBytes: 3145728,
        otaSlotBytes: 3145728,
        storageUsedBytes: 34504,
        storageTotalBytes: 2031616
      },
      storage: {
        ok: true,
        partition: "spiffs",
        totalBytes: 2031616,
        usedBytes: 34504,
        floorplanConfigBytes: 1804,
        floorplanImageBytes: 28234,
        deviceConfigBytes: 683,
        statsBytes: 2400,
        maxPayloadBytes: 65536
      },
      wifi: {
        connected: true,
        ssid: "mock-wifi",
        rssi: -48
      },
      bluetooth: {
        enabled: true,
        connected: false
      }
    };
  },

  async getControlStatus() {
    return {
      ok: true,
      statusLedEnabled,
      statusLedKnown: true,
      ledBlinkDuration,
      ledBlinkDurationKnown: true,
      environmentCorrectionEnabled,
      environmentCorrectionKnown: true,
      temperatureOffset,
      temperatureOffsetKnown: true,
      humidityOffset,
      humidityOffsetKnown: true
    };
  },

  async saveConfig(nextConfig: WebDeviceConfig): Promise<void> {
    config = structuredClone(nextConfig);
  },

  async saveStats(nextStats: WebDeviceStats): Promise<void> {
    stats = structuredClone(nextStats);
  },

  async setStatusLed(enabled: boolean): Promise<void> {
    statusLedEnabled = enabled;
  },

  async setLedBlinkDuration(seconds: number): Promise<void> {
    ledBlinkDuration = Math.max(0, Math.min(300, seconds));
  },

  async setEnvironmentCorrection(enabled: boolean): Promise<void> {
    environmentCorrectionEnabled = enabled;
  },

  async setTemperatureOffset(value: number): Promise<void> {
    temperatureOffset = Math.max(-2, Math.min(2, value));
  },

  async setHumidityOffset(value: number): Promise<void> {
    humidityOffset = Math.max(-5, Math.min(5, value));
  },

  async saveFloorplan(document, image): Promise<void> {
    floorplanDocument = structuredClone(document);
    floorplanImage = image;
  },

  async uploadFirmware(file, onProgress): Promise<void> {
    const total = Math.max(1, file.size);
    for (let step = 1; step <= 10; step++) {
      await new Promise((resolve) => setTimeout(resolve, 80));
      const loaded = step === 10 ? total : Math.round((total * step) / 10);
      onProgress({
        loaded,
        total,
        percent: Math.round((loaded / total) * 100)
      });
    }
  }
};
