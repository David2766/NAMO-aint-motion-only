export type { WebDeviceConfig, WebDeviceState, WebDeviceStats, WebStatsEntry, WebTarget, WebZone, WebZoneType } from "../core/types";

import type { CalibrationMetrics } from "../core/calibration";
import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../core/types";

export interface DeviceApi {
  getState(): Promise<WebDeviceState>;
  getConfig(): Promise<WebDeviceConfig>;
  getStats(): Promise<WebDeviceStats>;
  getSystemStatus?(): Promise<WebSystemStatus>;
  getApiKey?(): Promise<WebApiKeyResult>;
  completeHaSetupHandoff?(): Promise<WebHaSetupHandoffResult>;
  getControlStatus?(): Promise<WebControlStatus>;
  getStaticRadarTuningStatus?(): Promise<WebStaticRadarTuningStatus>;
  rebootSystem?(): Promise<WebSystemRebootResult>;
  saveConfig(config: WebDeviceConfig): Promise<void>;
  saveStats(stats: WebDeviceStats, onProgress?: (progress: FirmwareUploadProgress) => void): Promise<void>;
  setStatusLed?(enabled: boolean): Promise<void>;
  setLedBlinkDuration?(seconds: number): Promise<void>;
  setEnvironmentCorrection?(enabled: boolean): Promise<void>;
  setTemperatureOffset?(value: number): Promise<void>;
  setHumidityOffset?(value: number): Promise<void>;
  setTimezone?(timezone: string): Promise<void>;
  setStaticRadarTuningSession?(active: boolean): Promise<void>;
  setStaticRadarGateSensitivity?(gate: number, sensitivity: number): Promise<void>;
  saveFloorplan?(document: FloorplanStorageDocument, image: Blob): Promise<void>;
  uploadFirmware?(file: File, onProgress: (progress: FirmwareUploadProgress) => void): Promise<void>;
  resetSystem?(options: WebSystemResetOptions): Promise<WebSystemResetResult>;
}

export interface WebApiKeyResult {
  ok: boolean;
  apiKey: string;
  visibleSeconds?: number;
}

export interface WebApiStatusInfo {
  code: string;
  severity?: "info" | "warning" | "error" | string;
  detail?: Record<string, unknown>;
}

export interface WebHaSetupHandoffResult {
  ok: boolean;
  message?: string;
  waitSeconds?: number;
  statusInfo?: WebApiStatusInfo;
}

export interface FirmwareUploadProgress {
  loaded: number;
  total: number;
  percent: number;
}

export interface WebControlStatus {
  ok: boolean;
  statusLedEnabled?: boolean;
  statusLedKnown?: boolean;
  ledBlinkDuration?: number;
  ledBlinkDurationKnown?: boolean;
  environmentCorrectionEnabled?: boolean;
  environmentCorrectionKnown?: boolean;
  temperatureOffset?: number;
  temperatureOffsetKnown?: boolean;
  humidityOffset?: number;
  humidityOffsetKnown?: boolean;
  timezone?: string;
  timezoneKnown?: boolean;
  timezoneApplyPending?: boolean;
}

export interface WebStaticRadarGateTuning {
  gate: number;
  startMm: number;
  endMm: number;
  sensitivity: number;
  moveEnergy: number;
  stillEnergy: number;
}

export interface WebStaticRadarTuningStatus {
  ok: boolean;
  available: boolean;
  active: boolean;
  resolutionMm: number;
  gates: WebStaticRadarGateTuning[];
}

export interface WebSystemStatus {
  ok: boolean;
  firmware?: {
    version?: string;
    buildTime?: string;
    uptimeSeconds?: number;
  };
  api?: {
    connected?: boolean;
    warning?: boolean;
    statusInfo?: WebApiStatusInfo;
  };
  boot?: {
    initialGuardActive?: boolean;
    guardSeconds?: number;
  };
  dashboard?: {
    version?: string;
    gzipBytes?: number;
  };
  schema?: {
    config?: number;
    floorplan?: number;
    stats?: number;
  };
  memory?: {
    freeHeap?: number;
    minFreeHeap?: number;
    internalTotalBytes?: number;
    internalFreeBytes?: number;
    internalMinFreeBytes?: number;
    psramTotal?: number;
    psramFree?: number;
    externalTotalBytes?: number;
    externalFreeBytes?: number;
  };
  flash?: {
    totalBytes?: number;
    firmwareUsedBytes?: number;
    firmwareSlotBytes?: number;
    otaSlotBytes?: number;
    storageUsedBytes?: number;
    storageTotalBytes?: number;
  };
  storage?: {
    ok?: boolean;
    partition?: string;
    totalBytes?: number;
    usedBytes?: number;
    floorplanConfigBytes?: number;
    floorplanImageBytes?: number;
    deviceConfigBytes?: number;
    statsBytes?: number;
    maxPayloadBytes?: number;
  };
  wifi?: {
    connected?: boolean;
    ssid?: string;
    rssi?: number;
  };
  bluetooth?: {
    enabled?: boolean;
    connected?: boolean;
  };
}

export interface WebSystemResetOptions {
  settings: boolean;
  wifi: boolean;
  stats: boolean;
}

export interface WebSystemResetResult {
  ok: boolean;
  reset?: {
    settings?: boolean;
    wifi?: boolean;
    stats?: boolean;
  };
  details?: {
    apiKey?: boolean;
    floorplan?: boolean;
    deviceConfig?: boolean;
    stats?: boolean;
    wifiScheduled?: boolean;
  };
  rebootRequired?: boolean;
  rebootInMs?: number;
  error?: string;
  message?: string;
}

export interface WebSystemRebootResult {
  ok: boolean;
  rebootInMs?: number;
}

export type SaveState = "idle" | "pending" | "saving" | "queued" | "saved" | "error";

export interface CalibrationResult {
  title: string;
  tone: "ok" | "warn" | "error";
  createdCount: number;
  reason: string;
  metrics?: CalibrationMetrics;
  logs?: string[];
}
