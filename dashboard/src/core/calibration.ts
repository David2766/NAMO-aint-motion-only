import {
  CALIBRATION_BOX_MARGIN_MM,
  CALIBRATION_MAX_CLUSTER_AREA_MM2,
  CALIBRATION_MAX_CLUSTER_HEIGHT_MM,
  CALIBRATION_MAX_CLUSTER_WIDTH_MM,
  CALIBRATION_MIN_BOX_SIZE_MM,
  CALIBRATION_MIN_SAMPLES,
  CALIBRATION_OUTLIER_DISTANCE_MM,
  CALIBRATION_PERCENTILE_HIGH,
  CALIBRATION_PERCENTILE_LOW,
  CALIBRATION_SCORE_THRESHOLD,
  MAX_CALIBRATION_ZONES
} from "./constants";
import { clamp, rectPoints } from "./geometry";
import type { WebZone } from "./types";
import { clampZoneToHardwareBounds } from "./zones";

export interface CalibrationSample {
  x: number;
  y: number;
  speed: number;
}

export interface CalibrationRun {
  startedAt: number;
  samples: CalibrationSample[];
}

export interface CalibrationMetrics {
  samples: number;
  usedSamples: number;
  outliers: number;
  score: number;
  width: number;
  height: number;
  area: number;
  meanSpeed: number;
  acceptedBy: "score" | "area" | "none";
}

export function calibrationScore(samples: CalibrationSample[]): number {
  if (samples.length < 2) return 0;
  const center = sampleCenter(samples);
  const distances = samples.map((sample) => Math.hypot(sample.x - center.x, sample.y - center.y));
  const meanSpread = average(distances);
  const maxSpread = Math.max(...distances);
  const meanSpeed = average(samples.map((sample) => sample.speed));
  const spreadScore = clamp(45 - meanSpread / 12, 0, 45);
  const maxSpreadScore = clamp(25 - maxSpread / 28, 0, 25);
  const speedScore = clamp(20 - meanSpeed / 18, 0, 20);
  const sampleScore = clamp(samples.length / CALIBRATION_MIN_SAMPLES, 0, 1) * 10;
  return spreadScore + maxSpreadScore + speedScore + sampleScore;
}

export function calibrationMetrics(samples: CalibrationSample[]): CalibrationMetrics {
  if (samples.length === 0) {
    return {
      samples: 0,
      usedSamples: 0,
      outliers: 0,
      score: 0,
      width: 0,
      height: 0,
      area: 0,
      meanSpeed: 0,
      acceptedBy: "none"
    };
  }
  const score = calibrationScore(samples);
  const prepared = prepareCalibrationSamples(samples);
  const bounds = percentileBounds(prepared.usedSamples);
  const width = bounds.maxX - bounds.minX;
  const height = bounds.maxY - bounds.minY;
  const area = width * height;
  const meanSpeed = average(samples.map((sample) => sample.speed));
  let acceptedBy: CalibrationMetrics["acceptedBy"] = "none";
  if (samples.length >= CALIBRATION_MIN_SAMPLES && score >= CALIBRATION_SCORE_THRESHOLD) {
    acceptedBy = "score";
  } else if (
    samples.length >= CALIBRATION_MIN_SAMPLES &&
    width <= CALIBRATION_MAX_CLUSTER_WIDTH_MM &&
    height <= CALIBRATION_MAX_CLUSTER_HEIGHT_MM &&
    area <= CALIBRATION_MAX_CLUSTER_AREA_MM2
  ) {
    acceptedBy = "area";
  }
  return {
    samples: samples.length,
    usedSamples: prepared.usedSamples.length,
    outliers: prepared.outliers,
    score,
    width,
    height,
    area,
    meanSpeed,
    acceptedBy
  };
}

export function calibrationZoneFromSamples(samples: CalibrationSample[], existingZones: WebZone[]): WebZone | null {
  if (samples.length < CALIBRATION_MIN_SAMPLES) return null;
  const prepared = prepareCalibrationSamples(samples);
  if (prepared.usedSamples.length < CALIBRATION_MIN_SAMPLES) return null;
  const center = sampleCenter(prepared.usedSamples);
  const bounds = percentileBounds(prepared.usedSamples);
  const spreadX = bounds.maxX - bounds.minX;
  const spreadY = bounds.maxY - bounds.minY;
  const width = clamp(Math.max(CALIBRATION_MIN_BOX_SIZE_MM, spreadX + CALIBRATION_BOX_MARGIN_MM), CALIBRATION_MIN_BOX_SIZE_MM, CALIBRATION_MAX_CLUSTER_WIDTH_MM);
  const height = clamp(Math.max(CALIBRATION_MIN_BOX_SIZE_MM, spreadY + CALIBRATION_BOX_MARGIN_MM), CALIBRATION_MIN_BOX_SIZE_MM, CALIBRATION_MAX_CLUSTER_HEIGHT_MM);
  const id = nextCalibrationZoneId(existingZones);
  if (!id) return null;
  const points = rectPoints(center.x - width / 2, center.y - height / 2, center.x + width / 2, center.y + height / 2);
  const zone = clampZoneToHardwareBounds({
    id,
    name: `보정 ${id.replace("calibration_", "")}`,
    type: "filter" as const,
    shape: "rect" as const,
    points
  });
  return {
    ...zone,
    minPoints: zone.points
  };
}

function prepareCalibrationSamples<T extends { x: number; y: number }>(samples: T[]): { usedSamples: T[]; outliers: number } {
  if (samples.length < 3) return { usedSamples: samples, outliers: 0 };
  const center = sampleMedianCenter(samples);
  const usedSamples = samples.filter((sample) => Math.hypot(sample.x - center.x, sample.y - center.y) <= CALIBRATION_OUTLIER_DISTANCE_MM);
  return {
    usedSamples: usedSamples.length >= CALIBRATION_MIN_SAMPLES ? usedSamples : samples,
    outliers: usedSamples.length >= CALIBRATION_MIN_SAMPLES ? samples.length - usedSamples.length : 0
  };
}

function sampleCenter(samples: Array<{ x: number; y: number }>): { x: number; y: number } {
  return {
    x: Math.round(average(samples.map((sample) => sample.x))),
    y: Math.round(average(samples.map((sample) => sample.y)))
  };
}

function sampleMedianCenter(samples: Array<{ x: number; y: number }>): { x: number; y: number } {
  return {
    x: percentile(samples.map((sample) => sample.x), 0.5),
    y: percentile(samples.map((sample) => sample.y), 0.5)
  };
}

function percentileBounds(samples: Array<{ x: number; y: number }>): { minX: number; maxX: number; minY: number; maxY: number } {
  if (samples.length === 0) return { minX: 0, maxX: 0, minY: 0, maxY: 0 };
  return {
    minX: percentile(samples.map((sample) => sample.x), CALIBRATION_PERCENTILE_LOW),
    maxX: percentile(samples.map((sample) => sample.x), CALIBRATION_PERCENTILE_HIGH),
    minY: percentile(samples.map((sample) => sample.y), CALIBRATION_PERCENTILE_LOW),
    maxY: percentile(samples.map((sample) => sample.y), CALIBRATION_PERCENTILE_HIGH)
  };
}

function percentile(values: number[], ratio: number): number {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = clamp(Math.round((sorted.length - 1) * ratio), 0, sorted.length - 1);
  return sorted[index] ?? 0;
}

function average(values: number[]): number {
  return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : 0;
}

function nextCalibrationZoneId(zones: WebZone[]): string | null {
  const used = new Set(zones.map((zone) => zone.id));
  for (let index = 1; index <= MAX_CALIBRATION_ZONES; index += 1) {
    const id = `calibration_${index}`;
    if (!used.has(id)) return id;
  }
  return null;
}
