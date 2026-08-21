import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { WebDeviceConfig } from "../../core/types";
import type { FloorplanScaleEstimate } from "../floorplan/floorplan-scale";
import {
  buildBoundaryOcclusionSegments,
  nextIgnoredOcclusionEdges,
  type FloorplanOcclusionSegment
} from "../floorplan/floorplan-occlusion";
import { findRoomContainingPoint } from "../floorplan/floorplan-room-context";
import { roomCandidatePoints } from "../floorplan/room-candidate-points";

export const FLOORPLAN_RADAR_SCALE_MIN_PERCENT = 95;
export const FLOORPLAN_RADAR_SCALE_MAX_PERCENT = 105;
export const FLOORPLAN_RADAR_SCALE_STEP_PERCENT = 1;

interface FloorplanRadarPlacement {
  originX: number;
  originY: number;
  rotation: number;
}

interface FloorplanBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

interface FloorplanImageSize {
  width?: number;
  height?: number;
}

interface FloorplanRadarPlacementSessionOptions {
  getDeviceConfig: () => WebDeviceConfig | null | undefined;
  getScaleEstimate: () => FloorplanScaleEstimate | null;
  getOuterBounds: () => FloorplanBounds | null;
  getImageSize: () => FloorplanImageSize | null;
  getConfirmedRoomCandidates: () => RoomCandidate[];
  getPhysicalWallSegments?: () => FloorplanOcclusionSegment[];
  onUpdateDeviceConfig?: (updater: (current: WebDeviceConfig) => WebDeviceConfig) => void;
}

export function createFloorplanRadarPlacementSession({
  getDeviceConfig,
  getScaleEstimate,
  getOuterBounds,
  getImageSize,
  getConfirmedRoomCandidates,
  getPhysicalWallSegments,
  onUpdateDeviceConfig
}: FloorplanRadarPlacementSessionOptions) {
  let scalePercent = $state(100);
  let placement = $state<FloorplanRadarPlacement | null>(null);
  let occlusionEditActive = $state(false);
  let ignoredOcclusionEdges = $state<string[]>([]);

  function reset(): void {
    scalePercent = 100;
    placement = null;
    occlusionEditActive = false;
    ignoredOcclusionEdges = [];
  }

  function clampScalePercent(value: number | string): number {
    const numericValue = Number(value);
    if (!Number.isFinite(numericValue)) return 100;
    return Math.max(
      FLOORPLAN_RADAR_SCALE_MIN_PERCENT,
      Math.min(FLOORPLAN_RADAR_SCALE_MAX_PERCENT, Math.round(numericValue))
    );
  }

  function updateScalePercent(value: number | string): void {
    scalePercent = clampScalePercent(value);
    commitPlacement();
  }

  function nudgeScalePercent(delta: number): void {
    updateScalePercent(scalePercent + delta);
  }

  function radarConfig() {
    return getDeviceConfig()?.floorplan?.radar ?? null;
  }

  function defaultPlacement(): FloorplanRadarPlacement {
    const bounds = getScaleEstimate()?.outerBounds ?? getOuterBounds();
    const imageSize = getImageSize();
    return {
      originX: bounds ? bounds.x + bounds.width / 2 : (imageSize?.width ?? 1) / 2,
      originY: bounds ? bounds.y + bounds.height - 16 : (imageSize?.height ?? 1) * 0.82,
      rotation: 0
    };
  }

  function initialize(): void {
    ignoredOcclusionEdges = getDeviceConfig()?.floorplan?.radarOcclusionIgnoredEdges ?? [];
    const saved = radarConfig();
    if (saved) {
      placement = {
        originX: saved.originX,
        originY: saved.originY,
        rotation: saved.rotation
      };
      scalePercent = clampScalePercent((saved.scale || 1) * 100);
      return;
    }
    placement = defaultPlacement();
  }

  function toggleOcclusionEdit(): void {
    occlusionEditActive = !occlusionEditActive;
  }

  function toggleOcclusionEdge(segment: FloorplanOcclusionSegment): void {
    const segments = boundarySegments();
    ignoredOcclusionEdges = nextIgnoredOcclusionEdges(ignoredOcclusionEdges, segment, segments);
    commitOcclusionEdges();
  }

  function resetOcclusionEdges(): void {
    if (!ignoredOcclusionEdges.length) return;
    ignoredOcclusionEdges = [];
    commitOcclusionEdges();
  }

  function commitOcclusionEdges(): void {
    if (!onUpdateDeviceConfig) return;
    onUpdateDeviceConfig((current) => ({
      ...current,
      floorplan: {
        ...(current.floorplan ?? { enabled: true, hasImage: true }),
        enabled: true,
        hasImage: true,
        radarOcclusionIgnoredEdges: ignoredOcclusionEdges
      }
    }));
  }

  function updatePlacement(nextPlacement: FloorplanRadarPlacement): void {
    placement = {
      originX: nextPlacement.originX,
      originY: nextPlacement.originY,
      rotation: nextPlacement.rotation
    };
  }

  function commitPlacement(nextPlacement = placement): void {
    const scaleEstimate = getScaleEstimate();
    if (!nextPlacement || !scaleEstimate || !onUpdateDeviceConfig) return;
    const scale = Math.round((scalePercent / 100) * 1000) / 1000;
    const radar = {
      originX: Math.round(nextPlacement.originX * 100) / 100,
      originY: Math.round(nextPlacement.originY * 100) / 100,
      rotation: Math.round(nextPlacement.rotation * 10) / 10,
      scale
    };
    onUpdateDeviceConfig((current) => ({
      ...current,
      floorplan: {
        ...(current.floorplan ?? { enabled: true, hasImage: true }),
        enabled: true,
        hasImage: true,
        scaleMmPerPx: (scaleEstimate.mmPerPxX + scaleEstimate.mmPerPxY) / 2,
        scaleMmPerPxX: scaleEstimate.mmPerPxX,
        scaleMmPerPxY: scaleEstimate.mmPerPxY,
        radar
      }
    }));
  }

  function placementOrDefault(): FloorplanRadarPlacement {
    return placement ?? defaultPlacement();
  }

  function roomForPlacement(nextPlacement: FloorplanRadarPlacement | null): RoomCandidate | null {
    if (!nextPlacement) return null;
    return findRoomContainingPoint(
      getConfirmedRoomCandidates(),
      { x: nextPlacement.originX, y: nextPlacement.originY },
      roomCandidatePoints
    );
  }

  function visibleRoomWallSegmentsForPlacement(
    nextPlacement: FloorplanRadarPlacement | null,
    segmentPrefix: string
  ): FloorplanOcclusionSegment[] {
    const physicalWalls = getPhysicalWallSegments?.() ?? [];
    if (physicalWalls.length) return physicalWalls;
    const room = roomForPlacement(nextPlacement);
    if (!room) return [];
    return buildBoundaryOcclusionSegments([{
      id: room.id,
      points: roomCandidatePoints(room),
      segmentPrefix
    }]);
  }

  function boundarySegments(): FloorplanOcclusionSegment[] {
    return visibleRoomWallSegmentsForPlacement(placementOrDefault(), "room");
  }

  function occlusionSegments(): FloorplanOcclusionSegment[] {
    return visibleRoomWallSegmentsForPlacement(placementOrDefault(), "room");
  }

  function hasPlacement(): boolean {
    return Boolean(placement);
  }

  return {
    get scalePercent() {
      return scalePercent;
    },
    get placement() {
      return placement;
    },
    get occlusionEditActive() {
      return occlusionEditActive;
    },
    get ignoredOcclusionEdges() {
      return ignoredOcclusionEdges;
    },
    minScalePercent: FLOORPLAN_RADAR_SCALE_MIN_PERCENT,
    maxScalePercent: FLOORPLAN_RADAR_SCALE_MAX_PERCENT,
    scaleStepPercent: FLOORPLAN_RADAR_SCALE_STEP_PERCENT,
    reset,
    clampScalePercent,
    updateScalePercent,
    nudgeScalePercent,
    radarConfig,
    defaultPlacement,
    initialize,
    toggleOcclusionEdit,
    toggleOcclusionEdge,
    resetOcclusionEdges,
    commitOcclusionEdges,
    updatePlacement,
    commitPlacement,
    placementOrDefault,
    roomForPlacement,
    visibleRoomWallSegmentsForPlacement,
    boundarySegments,
    occlusionSegments,
    hasPlacement
  };
}
