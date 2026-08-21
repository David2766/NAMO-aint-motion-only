import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type {
  FloorplanScaleEstimate,
  FloorplanSizeInput
} from "../floorplan/floorplan-scale";
import {
  calculateFloorplanScale,
  floorplanScaleSummary,
  parseDimensionInput
} from "../floorplan/floorplan-scale";
import { rectFromPoints, roundPoint } from "../floorplan/floorplan-room-edit-geometry";
import { roomCandidatePoints } from "../floorplan/room-candidate-points";

interface RoomMeasurement {
  width?: string;
  height?: string;
}

interface RoomSizeEstimate {
  widthPx: string;
  heightPx: string;
  widthMm: number;
  heightMm: number;
  estimatedWidthMm: number;
  estimatedHeightMm: number;
  widthFromOuter: boolean;
  heightFromOuter: boolean;
  widthManual: boolean;
  heightManual: boolean;
  manuallyEdited: boolean;
}

interface RoomScaleText {
  sizeRequired: string;
  roomSizesCalculated(count: number): string;
  roomSizeHint: string;
  preciseSizeHint: string;
}

interface FloorplanRoomScaleSessionOptions {
  getRoomCandidates: () => RoomCandidate[];
  getOuterBounds?: () => { x: number; y: number; width: number; height: number } | null;
  getText: () => RoomScaleText;
  outerBoundsTouchTolerancePx: number;
}

export function createFloorplanRoomScaleSession({
  getRoomCandidates,
  getOuterBounds = () => null,
  getText,
  outerBoundsTouchTolerancePx
}: FloorplanRoomScaleSessionOptions) {
  let totalSize = $state<FloorplanSizeInput>({ width: "", height: "" });
  let estimates = $state<Record<string, RoomSizeEstimate>>({});
  let calculated = $state(false);
  let error = $state("");
  let measurements = $state<Record<string, RoomMeasurement>>({});
  let preciseEditing = $state(false);

  function reset(): void {
    totalSize = { width: "", height: "" };
    estimates = {};
    calculated = false;
    error = "";
    measurements = {};
    preciseEditing = false;
  }

  function updateMeasurement(candidateId: string, field: "width" | "height", value: string): void {
    measurements = {
      ...measurements,
      [candidateId]: {
        ...(measurements[candidateId] ?? { width: "", height: "" }),
        [field]: normalizeDimensionInput(value)
      }
    };
    calculate({ keepError: true });
  }

  function updateTotalSize(field: "width" | "height", value: string): void {
    totalSize = {
      ...totalSize,
      [field]: normalizeDimensionInput(value)
    };
    calculate({ keepError: true });
  }

  function togglePreciseEditing(): void {
    preciseEditing = !preciseEditing;
    calculate();
  }

  function outerBounds() {
    const providedBounds = getOuterBounds();
    if (providedBounds) return providedBounds;
    const points = getRoomCandidates().flatMap(roomCandidatePoints);
    if (!points.length) return null;
    return rectFromPoints(points);
  }

  function scaleEstimate(): FloorplanScaleEstimate | null {
    return calculateFloorplanScale(outerBounds(), totalSize);
  }

  function scaleSummary() {
    return floorplanScaleSummary(scaleEstimate());
  }

  function canCalculate(): boolean {
    return Boolean(scaleEstimate());
  }

  function estimateRoomSize(candidate: RoomCandidate, scale: FloorplanScaleEstimate): RoomSizeEstimate {
    const bounds = rectFromPoints(roomCandidatePoints(candidate));
    const measurement = measurements[candidate.id] ?? {};
    const manualWidthMm = parseDimensionInput(measurement.width ?? "");
    const manualHeightMm = parseDimensionInput(measurement.height ?? "");
    const touchesLeft = Math.abs(bounds.x - scale.outerBounds.x) <= outerBoundsTouchTolerancePx;
    const touchesRight = Math.abs(bounds.x + bounds.width - (scale.outerBounds.x + scale.outerBounds.width)) <= outerBoundsTouchTolerancePx;
    const touchesTop = Math.abs(bounds.y - scale.outerBounds.y) <= outerBoundsTouchTolerancePx;
    const touchesBottom = Math.abs(bounds.y + bounds.height - (scale.outerBounds.y + scale.outerBounds.height)) <= outerBoundsTouchTolerancePx;
    const widthFromOuter = touchesLeft && touchesRight;
    const heightFromOuter = touchesTop && touchesBottom;
    const estimatedWidthMm = widthFromOuter ? scale.widthMm : bounds.width * scale.mmPerPxX;
    const estimatedHeightMm = heightFromOuter ? scale.heightMm : bounds.height * scale.mmPerPxY;
    return {
      widthPx: roundPoint(bounds.width).toFixed(2),
      heightPx: roundPoint(bounds.height).toFixed(2),
      widthMm: Math.round(manualWidthMm ?? estimatedWidthMm),
      heightMm: Math.round(manualHeightMm ?? estimatedHeightMm),
      estimatedWidthMm: Math.round(estimatedWidthMm),
      estimatedHeightMm: Math.round(estimatedHeightMm),
      widthFromOuter,
      heightFromOuter,
      widthManual: Boolean(manualWidthMm),
      heightManual: Boolean(manualHeightMm),
      manuallyEdited: Boolean(manualWidthMm || manualHeightMm)
    };
  }

  function calculate(options: { keepError?: boolean } = {}): void {
    const scale = scaleEstimate();
    if (!scale) {
      estimates = {};
      calculated = false;
      error = options.keepError ? "" : getText().sizeRequired;
      return;
    }

    estimates = buildEstimates(scale);
    calculated = true;
    error = "";
  }

  function message(): string {
    const text = getText();
    if (error) return error;
    if (calculated) return text.roomSizesCalculated(Object.keys(estimates).length);
    if (!scaleEstimate()) return text.roomSizeHint;
    return text.preciseSizeHint;
  }

  function storageEstimates(scale = scaleEstimate()): Record<string, RoomSizeEstimate> {
    if (!scale) return {};
    return calculated
      ? estimates
      : buildEstimates(scale);
  }

  function hasInput(): boolean {
    return Boolean(totalSize.width || totalSize.height || Object.keys(measurements).length);
  }

  function buildEstimates(scale: FloorplanScaleEstimate): Record<string, RoomSizeEstimate> {
    return Object.fromEntries(
      getRoomCandidates().map((candidate) => [candidate.id, estimateRoomSize(candidate, scale)])
    );
  }

  return {
    get totalSize() {
      return totalSize;
    },
    get estimates() {
      return estimates;
    },
    get calculated() {
      return calculated;
    },
    get error() {
      return error;
    },
    get measurements() {
      return measurements;
    },
    get preciseEditing() {
      return preciseEditing;
    },
    reset,
    updateMeasurement,
    updateTotalSize,
    togglePreciseEditing,
    outerBounds,
    scaleEstimate,
    scaleSummary,
    canCalculate,
    estimateRoomSize,
    calculate,
    message,
    storageEstimates,
    hasInput
  };
}

function normalizeDimensionInput(value: string): string {
  return value.replace(/[^\d.]/g, "").replace(/(\..*)\./g, "$1");
}
