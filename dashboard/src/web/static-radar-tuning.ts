import { pointAtPolar, toScreenPoint } from "../core/radar-math";
import type { RadarScreenPoint, RadarViewport, WebDeviceState } from "../core/types";
import type { WebStaticRadarGateTuning } from "./types";

const DEFAULT_ARC_SAMPLES = 24;

type StaticRadarState = NonNullable<NonNullable<WebDeviceState["debug"]>["staticRadar"]>;

export function staticRadarDetectedGateNumber(
  gates: Pick<WebStaticRadarGateTuning, "gate" | "startMm" | "endMm">[],
  staticRadar: StaticRadarState | null | undefined
): number | null {
  if (!staticRadar?.presence) return null;

  return gateAtDistance(gates, staticRadar.detectionDistanceMm);
}

export function staticRadarGatePath(
  gate: Pick<WebStaticRadarGateTuning, "startMm" | "endMm">,
  viewport: RadarViewport,
  sampleCount = DEFAULT_ARC_SAMPLES
): string {
  const startMm = clamp(gate.startMm, 0, viewport.rangeY);
  const endMm = clamp(gate.endMm, startMm, viewport.rangeY);
  if (endMm <= startMm) return "";

  const samples = Math.max(4, Math.floor(sampleCount));
  const halfFov = viewport.fovDegrees / 2;
  const points: RadarScreenPoint[] = [];

  appendArc(points, endMm, -halfFov, halfFov, samples, viewport);
  appendArc(points, startMm, halfFov, -halfFov, samples, viewport);

  return points
    .map((point, index) => `${index === 0 ? "M" : "L"}${round(point.x)} ${round(point.y)}`)
    .join(" ") + " Z";
}

export function staticRadarGateLabelPoint(
  gate: Pick<WebStaticRadarGateTuning, "startMm" | "endMm">,
  viewport: RadarViewport
): RadarScreenPoint {
  const distance = clamp((gate.startMm + gate.endMm) / 2, 0, viewport.rangeY);
  return toScreenPoint(0, distance, viewport);
}

function appendArc(
  target: RadarScreenPoint[],
  distance: number,
  startAngle: number,
  endAngle: number,
  sampleCount: number,
  viewport: RadarViewport
): void {
  for (let index = 0; index <= sampleCount; index++) {
    const progress = index / sampleCount;
    const angle = startAngle + (endAngle - startAngle) * progress;
    const radarPoint = pointAtPolar(distance, angle);
    target.push(toScreenPoint(radarPoint.x, radarPoint.y, viewport));
  }
}

function gateAtDistance(
  gates: Pick<WebStaticRadarGateTuning, "gate" | "startMm" | "endMm">[],
  distanceMm: number | undefined
): number | null {
  if (!Number.isFinite(distanceMm) || Number(distanceMm) < 0) return null;

  const distance = Number(distanceMm);
  const match = gates.find((gate, index) => {
    const isLast = index === gates.length - 1;
    return distance >= gate.startMm && (distance < gate.endMm || (isLast && distance === gate.endMm));
  });
  return match?.gate ?? null;
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function round(value: number): number {
  return Math.round(value * 100) / 100;
}
