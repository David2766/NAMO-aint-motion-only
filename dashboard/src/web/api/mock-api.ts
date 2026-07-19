import type { WebDeviceConfig, WebDeviceState, WebDeviceStats, WebStatsEntry } from "../../core/types";
import version from "../../../../version.json";
import { hasMockFloorplanStorage, saveMockFloorplan } from "../floorplan/mock-floorplan-storage";
import type { DeviceApi, WebStaticRadarTuningStatus, WebSystemStatus } from "../types";

const startTime = Date.now();
const HEATMAP_COLS = 33;
const HEATMAP_ROWS = 26;
const HEATMAP_CELL_COUNT = HEATMAP_COLS * HEATMAP_ROWS;
const CONFIG_STORAGE_KEY = "presence-sensor-demo-config";
const STATIC_RADAR_GATE_COUNT = 9;
const STATIC_RADAR_RESOLUTION_MM = 750;

function plainClone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function loadStoredConfig(): WebDeviceConfig | null {
  try {
    const raw = sessionStorage.getItem(CONFIG_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as WebDeviceConfig;
    return parsed && typeof parsed === "object" ? parsed : null;
  } catch {
    return null;
  }
}

function storeConfig(nextConfig: WebDeviceConfig): void {
  sessionStorage.setItem(CONFIG_STORAGE_KEY, JSON.stringify(plainClone(nextConfig)));
}

function clearStoredConfig(): void {
  sessionStorage.removeItem(CONFIG_STORAGE_KEY);
}

function waveValue(base: number, spread: number, elapsed: number, speed: number, phase = 0): number {
  return Math.round((base + Math.sin(elapsed / speed + phase) * spread) * 10) / 10;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.min(maximum, Math.max(minimum, value));
}

function staticRadarDemoFrame(elapsed: number) {
  const cycle = elapsed % 36;
  const targetCount = cycle < 22 ? 1 + (Math.floor(cycle / 7.5) % 3) : 0;
  const staticPresence = cycle < 30;
  const assistActive = targetCount === 0 && staticPresence;
  const graceActive = targetCount === 0 && !staticPresence && cycle < 33;
  const detectionDistanceMm = staticPresence
    ? Math.round(clamp(1750 + Math.sin(elapsed / 4.2) * 1050, 350, 4100))
    : 0;
  const lastDistanceMm = 1897;
  const distanceDeltaMm = staticPresence ? Math.abs(detectionDistanceMm - lastDistanceMm) : null;

  return {
    targetCount,
    staticPresence,
    assistActive,
    presence: targetCount > 0 || assistActive || graceActive,
    detectionDistanceMm,
    distanceDeltaMm,
    distanceMatched: distanceDeltaMm != null && distanceDeltaMm <= 750
  };
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

const defaultConfig: WebDeviceConfig = {
  version: 1,
  integrationMode: "unknown",
  legacyPresenceFallback: false,
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
let config: WebDeviceConfig = loadStoredConfig() ?? plainClone(defaultConfig);
let statusLedEnabled = true;
let ledBlinkDuration = 60;
let environmentCorrectionEnabled = true;
let temperatureOffset = 0;
let humidityOffset = 0;
let timezone = "Asia/Seoul";
let stats: WebDeviceStats = createDemoStats();
let staticRadarTuningActive = false;
let staticRadarSensitivity = Array.from({ length: STATIC_RADAR_GATE_COUNT }, () => 50);

function createStaticRadarTuningStatus(elapsed: number): WebStaticRadarTuningStatus {
  const frame = staticRadarDemoFrame(elapsed);
  const detectedGate = frame.staticPresence
    ? clamp(Math.floor(frame.detectionDistanceMm / STATIC_RADAR_RESOLUTION_MM), 0, STATIC_RADAR_GATE_COUNT - 1)
    : -1;

  return {
    ok: true,
    available: true,
    active: staticRadarTuningActive,
    resolutionMm: STATIC_RADAR_RESOLUTION_MM,
    gates: staticRadarSensitivity.map((sensitivity, gate) => {
      const distanceFromDetection = detectedGate < 0 ? STATIC_RADAR_GATE_COUNT : Math.abs(gate - detectedGate);
      const signal = distanceFromDetection === 0 ? 76 : distanceFromDetection === 1 ? 24 : 4;
      const variation = Math.round(Math.sin(elapsed * 1.7 + gate * 0.8) * 5);
      return {
        gate,
        startMm: gate * STATIC_RADAR_RESOLUTION_MM,
        endMm: (gate + 1) * STATIC_RADAR_RESOLUTION_MM,
        sensitivity,
        moveEnergy: staticRadarTuningActive ? clamp(signal - 18 + variation, 0, 100) : 0,
        stillEnergy: staticRadarTuningActive ? clamp(signal + variation, 0, 100) : 0
      };
    })
  };
}

export const mockApi: DeviceApi = {
  async getState(): Promise<WebDeviceState> {
    const elapsed = (Date.now() - startTime) / 1000;
    const staticRadar = staticRadarDemoFrame(elapsed);
    const targetCount = staticRadar.targetCount;
    const targets = [
      {
        id: "target_1",
        name: "T1",
        color: "#ff6b7a",
        x: Math.round(Math.sin(elapsed / 2) * 1100),
        y: Math.round(1800 + Math.cos(elapsed / 2.8) * 700),
        active: targetCount >= 1
      },
      {
        id: "target_2",
        name: "T2",
        color: "#ffd166",
        x: Math.round(1250 + Math.sin(elapsed / 2.7 + 1.8) * 650),
        y: Math.round(2600 + Math.cos(elapsed / 3.4 + 0.6) * 850),
        active: targetCount >= 2
      },
      {
        id: "target_3",
        name: "T3",
        color: "#06d6a0",
        x: Math.round(-1350 + Math.sin(elapsed / 3.1 + 2.4) * 720),
        y: Math.round(3300 + Math.cos(elapsed / 3.8 + 1.2) * 900),
        active: targetCount >= 3
      }
    ];
    return {
      connected: true,
      updatedAt: Date.now(),
      presence: staticRadar.presence,
      motion: targetCount > 0,
      pirMotion: targetCount > 0,
      targetCount,
      movingTargetCount: targetCount > 0 ? 1 : 0,
      stillTargetCount: Math.max(0, targetCount - 1),
      temperatureC: waveValue(25.4, 0.25, elapsed, 17),
      humidityPercent: waveValue(45, 0.45, elapsed, 19, 1.1),
      illuminanceLux: Math.round(waveValue(180, 1.8, elapsed, 23, 2.2)),
      targets,
      debug: {
        presenceReason: "target",
        presenceOffReason: "none",
        motionReason: "moving_target",
        lastPresenceDropMs: 0,
        lastValidTargetAgeMs: 0,
        emptySamplesConsecutive: 0,
        shortPresenceDropCount: 1,
        longPresenceDropCount: 0,
        presenceEvidence: {
          pir: targetCount > 0,
          tracker: targetCount > 0,
          staticAssist: staticRadar.assistActive
        },
        still: {
          state: "candidate",
          reason: "same_area_still",
          confidence: Math.round(48 + Math.sin(elapsed / 8) * 18),
          holdActive: false,
          lastSeenAgeMs: Math.round((elapsed % 3) * 1000),
          anchor: {
            x: targets[0]?.x ?? 0,
            y: targets[0]?.y ?? 0
          }
        },
        range: {
          reason: targetCount >= 3 ? "remote_pir_validated" : "ok",
          suspectTargetCount: targetCount >= 3 ? 1 : 0,
          outOfRangeTargetCount: 0,
          remoteCandidateCount: targetCount >= 3 ? 1 : 0
        },
        staticRadar: {
          available: true,
          presence: staticRadar.staticPresence,
          moving: targetCount > 0 && Math.floor(elapsed) % 4 < 2,
          still: staticRadar.staticPresence,
          detectionDistanceMm: staticRadar.detectionDistanceMm,
          movingDistanceMm: targetCount > 0 ? staticRadar.detectionDistanceMm : 0,
          stillDistanceMm: staticRadar.detectionDistanceMm,
          movingEnergy: staticRadar.staticPresence ? Math.round(38 + Math.sin(elapsed * 1.3) * 12) : 0,
          stillEnergy: staticRadar.staticPresence ? Math.round(72 + Math.sin(elapsed * 0.9) * 10) : 0,
          reason: staticRadar.staticPresence ? (targetCount > 0 ? "target" : "still") : "clear",
          assist: {
            armed: true,
            active: staticRadar.assistActive,
            armPending: false,
            armElapsedMs: 0,
            exitVeto: false,
            heldTarget: staticRadar.assistActive
              ? {
                  id: "target_1",
                  x: 600,
                  y: 1800,
                  lastDistanceMm: 1897,
                  staticDistanceMm: staticRadar.detectionDistanceMm,
                  distanceDeltaMm: staticRadar.distanceDeltaMm,
                  distanceMatched: staticRadar.distanceMatched
                }
              : null
          }
        }
      }
    };
  },

  async getConfig(): Promise<WebDeviceConfig> {
    if (hasMockFloorplanStorage()) {
      config = {
        ...config,
        floorplan: {
          ...(config.floorplan ?? {}),
          enabled: true,
          hasImage: true
        }
      };
      storeConfig(config);
    }
    return plainClone(config);
  },

  async getStats(): Promise<WebDeviceStats> {
    return plainClone(stats);
  },

  async getSystemStatus(): Promise<WebSystemStatus> {
    return {
      ok: true,
      firmware: {
        version: version.firmware,
        buildTime: "mock",
        uptimeSeconds: Math.round((Date.now() - startTime) / 1000)
      },
      api: {
        connected: true,
        warning: false,
        statusInfo: {
          code: "api_client_connected",
          severity: "info",
          detail: {}
        }
      },
      boot: {
        initialGuardActive: Math.round((Date.now() - startTime) / 1000) < 60,
        guardSeconds: 60
      },
      dashboard: {
        version: version.dashboard,
        gzipBytes: 92000
      },
      schema: {
        config: version.configSchema,
        floorplan: version.floorplanSchema,
        stats: version.statsSchema
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
      humidityOffsetKnown: true,
      timezone,
      timezoneKnown: true,
      timezoneApplyPending: false
    };
  },

  async getStaticRadarTuningStatus(): Promise<WebStaticRadarTuningStatus> {
    return createStaticRadarTuningStatus((Date.now() - startTime) / 1000);
  },

  async saveConfig(nextConfig: WebDeviceConfig): Promise<void> {
    config = plainClone(nextConfig);
    storeConfig(config);
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

  async setTimezone(nextTimezone: string): Promise<void> {
    if (timezone === nextTimezone) return;
    timezone = nextTimezone;
    if (stats.today) {
      stats.today = {
        ...stats.today,
        f: 0,
        r: 0,
        fz: stats.today.fz.map(() => 0),
        rz: stats.today.rz.map(() => 0),
        sz: stats.today.sz.map(() => 0)
      };
    }
    if (stats.heatmap) {
      stats.heatmap = {
        ...stats.heatmap,
        today: `0x${HEATMAP_COLS * HEATMAP_ROWS}`
      };
    }
  },

  async setStaticRadarTuningSession(active: boolean): Promise<void> {
    staticRadarTuningActive = active;
  },

  async setStaticRadarGateSensitivity(gate: number, sensitivity: number): Promise<void> {
    if (!Number.isInteger(gate) || gate < 0 || gate >= STATIC_RADAR_GATE_COUNT) {
      throw new Error("invalid static radar gate");
    }
    staticRadarSensitivity[gate] = Math.round(clamp(sensitivity, 0, 100));
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
    storeConfig(config);
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
      config = plainClone(defaultConfig);
      config.zones = [];
      config.calibrationZones = [];
      config.floorplan = {
        enabled: false,
        hasImage: false
      };
      clearStoredConfig();
      statusLedEnabled = true;
      ledBlinkDuration = 60;
      environmentCorrectionEnabled = true;
      temperatureOffset = 0;
      humidityOffset = 0;
      timezone = "Asia/Seoul";
      staticRadarTuningActive = false;
      staticRadarSensitivity = Array.from({ length: STATIC_RADAR_GATE_COUNT }, () => 50);
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
      waitSeconds: 1,
      statusInfo: {
        code: "ha_handoff_started",
        severity: "info",
        detail: {
          waitSeconds: 1
        }
      }
    };
  }
};
