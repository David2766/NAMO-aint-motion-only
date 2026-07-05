import type { WebDeviceConfig, WebDeviceState, WebDeviceStats, WebStatsEntry } from "../../core/types";
import { saveMockFloorplan } from "../floorplan/mock-floorplan-storage";
import type { DeviceApi, WebSystemStatus } from "../types";

const startTime = Date.now();
const HEATMAP_COLS = 33;
const HEATMAP_ROWS = 26;
const HEATMAP_CELL_COUNT = HEATMAP_COLS * HEATMAP_ROWS;

function plainClone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function dayKeyFromDate(date: Date): number {
  return date.getFullYear() * 10000 + (date.getMonth() + 1) * 100 + date.getDate();
}

function dateFromOffset(daysAgo: number): Date {
  const date = new Date();
  date.setHours(12, 0, 0, 0);
  date.setDate(date.getDate() - daysAgo);
  return date;
}

function makeStatsEntry(daysAgo: number): WebStatsEntry {
  const wave = Math.sin(daysAgo * 0.72);
  const weekendBoost = daysAgo % 7 === 0 || daysAgo % 7 === 6 ? 6 : 0;
  const base = 26 + Math.round(wave * 7) + weekendBoost;
  const filterHits = 5 + ((daysAgo * 3) % 9);
  const reducedHits = 2 + ((daysAgo * 5) % 6);
  return {
    d: dayKeyFromDate(dateFromOffset(daysAgo)),
    f: filterHits,
    r: reducedHits,
    fz: [filterHits, Math.max(1, Math.floor(filterHits * 0.55)), daysAgo % 4, daysAgo % 3],
    rz: [reducedHits, Math.max(0, Math.floor(reducedHits * 0.5)), daysAgo % 2, (daysAgo + 1) % 2],
    sz: [
      Math.max(0, base),
      Math.max(0, base - 7 + (daysAgo % 5)),
      Math.max(0, Math.floor(base * 0.55)),
      daysAgo % 6,
      (daysAgo * 2) % 5,
      daysAgo % 4
    ]
  };
}

function makeHeatmapRle(daysAgo: number): string {
  const cells = Array.from({ length: HEATMAP_CELL_COUNT }, () => 0);
  const centers = [
    { col: 15 + Math.sin(daysAgo * 0.45) * 3.5, row: 8 + Math.cos(daysAgo * 0.31) * 2.3, weight: 8 },
    { col: 10 + Math.cos(daysAgo * 0.26) * 2.2, row: 14 + Math.sin(daysAgo * 0.38) * 2.5, weight: 5 },
    { col: 22 + Math.sin(daysAgo * 0.21) * 2.8, row: 17 + Math.cos(daysAgo * 0.42) * 2.1, weight: 4 }
  ];

  for (let row = 0; row < HEATMAP_ROWS; row += 1) {
    for (let col = 0; col < HEATMAP_COLS; col += 1) {
      let value = 0;
      for (const center of centers) {
        const dx = (col - center.col) / 2.4;
        const dy = (row - center.row) / 1.8;
        const distance = dx * dx + dy * dy;
        if (distance < 5.4) {
          value += Math.max(0, Math.round(center.weight * (1 - distance / 5.4)));
        }
      }
      if ((row + col + daysAgo) % 19 === 0) value += 1;
      cells[row * HEATMAP_COLS + col] = value;
    }
  }

  return encodeHeatmapRle(cells);
}

function encodeHeatmapRle(cells: number[]): string {
  const tokens: string[] = [];
  let current = Math.max(0, Math.round(cells[0] ?? 0));
  let count = 0;
  for (const raw of cells) {
    const value = Math.max(0, Math.round(raw));
    if (value === current) {
      count += 1;
    } else {
      tokens.push(`${current}x${count}`);
      current = value;
      count = 1;
    }
  }
  if (count > 0) tokens.push(`${current}x${count}`);
  return tokens.join(",");
}

function createDemoStats(): WebDeviceStats {
  const entries = Array.from({ length: 30 }, (_, index) => makeStatsEntry(index));
  return {
    today: entries[0] ?? null,
    daily: entries.slice(1),
    heatmap: {
      version: 1,
      cols: HEATMAP_COLS,
      rows: HEATMAP_ROWS,
      cellMm: 300,
      encoding: "rle",
      today: makeHeatmapRle(0),
      daily: entries.slice(1).map((entry, index) => ({ d: entry.d, data: makeHeatmapRle(index + 1) }))
    }
  };
}

let config: WebDeviceConfig = {
  version: 1,
  integrationMode: "unknown",
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
let statusLedEnabled = true;
let ledBlinkDuration = 60;
let environmentCorrectionEnabled = true;
let temperatureOffset = 0;
let humidityOffset = 0;
let stats: WebDeviceStats = createDemoStats();

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
    return plainClone(config);
  },

  async getStats(): Promise<WebDeviceStats> {
    return plainClone(stats);
  },

  async getSystemStatus(): Promise<WebSystemStatus> {
    return {
      ok: true,
      firmware: {
        version: "0.4.0",
        buildTime: "mock",
        uptimeSeconds: Math.round((Date.now() - startTime) / 1000)
      },
      api: {
        connected: true,
        warning: false
      },
      boot: {
        initialGuardActive: Math.round((Date.now() - startTime) / 1000) < 60,
        guardSeconds: 60
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
    config = plainClone(nextConfig);
  },

  async saveStats(nextStats: WebDeviceStats): Promise<void> {
    stats = plainClone(nextStats);
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
    await saveMockFloorplan(document, image);
    config = {
      ...config,
      floorplan: {
        ...(config.floorplan ?? {}),
        enabled: true,
        hasImage: true
      }
    };
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
  },

  async resetSystem(options) {
    if (options.settings) {
      config = {
        version: 1,
        integrationMode: "unknown",
        zones: [],
        calibrationZones: [],
        floorplan: {
          enabled: false,
          hasImage: false
        }
      };
      statusLedEnabled = true;
      ledBlinkDuration = 60;
      environmentCorrectionEnabled = true;
      temperatureOffset = 0;
      humidityOffset = 0;
    }
    if (options.stats) {
      stats = createDemoStats();
    }
    return {
      ok: true,
      reset: {
        settings: options.settings,
        wifi: options.wifi,
        stats: options.stats
      },
      rebootRequired: options.settings || options.wifi,
      rebootInMs: options.settings || options.wifi ? 2500 : 0
    };
  },

  async rebootSystem() {
    return {
      ok: true,
      rebootInMs: 2500
    };
  },

  async completeHaSetupHandoff() {
    return {
      ok: true,
      message: "mock_handoff_started",
      waitSeconds: 1
    };
  }
};
