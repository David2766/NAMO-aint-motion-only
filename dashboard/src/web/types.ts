export type { WebDeviceConfig, WebDeviceState, WebDeviceStats, WebStatsEntry, WebTarget, WebZone, WebZoneType } from "../core/types";

import type { CalibrationMetrics } from "../core/calibration";
import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import type { WebDeviceConfig, WebDeviceState, WebDeviceStats } from "../core/types";

export interface DeviceApi {
  getState(): Promise<WebDeviceState>;
  getConfig(): Promise<WebDeviceConfig>;
  getStats(): Promise<WebDeviceStats>;
  getSystemStatus?(): Promise<WebSystemStatus>;
  getControlStatus?(): Promise<WebControlStatus>;
  saveConfig(config: WebDeviceConfig): Promise<void>;
  saveStats(stats: WebDeviceStats, onProgress?: (progress: FirmwareUploadProgress) => void): Promise<void>;
  setStatusLed?(enabled: boolean): Promise<void>;
  setLedBlinkDuration?(seconds: number): Promise<void>;
  setEnvironmentCorrection?(enabled: boolean): Promise<void>;
  setTemperatureOffset?(value: number): Promise<void>;
  setHumidityOffset?(value: number): Promise<void>;
  saveFloorplan?(document: FloorplanStorageDocument, image: Blob): Promise<void>;
  uploadFirmware?(file: File, onProgress: (progress: FirmwareUploadProgress) => void): Promise<void>;
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
}

export interface WebSystemStatus {
  ok: boolean;
  firmware?: {
    version?: string;
    buildTime?: string;
    uptimeSeconds?: number;
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

export type SaveState = "idle" | "pending" | "saving" | "queued" | "saved" | "error";

export interface CalibrationResult {
  title: string;
  tone: "ok" | "warn" | "error";
  createdCount: number;
  reason: string;
  metrics?: CalibrationMetrics;
  logs?: string[];
}
