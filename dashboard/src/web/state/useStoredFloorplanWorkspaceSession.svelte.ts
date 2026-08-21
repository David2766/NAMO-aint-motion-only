import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import { floorplanImageFrameStyle } from "../floorplan/floorplan-image-frame";
import { rectFromPoints } from "../floorplan/floorplan-room-edit-geometry";
import type { FloorplanOcclusionSegment } from "../floorplan/floorplan-occlusion";
import {
  storedFloorplanScaleEstimate,
  type FloorplanScaleEstimate
} from "../floorplan/floorplan-scale";
import { roomCandidatePoints } from "../floorplan/room-candidate-points";
import {
  storedFloorplanViewBounds,
  type StoredFloorplanViewBounds
} from "../floorplan/stored-floorplan-view";
import type { StoredRadarPlacement } from "./useStoredRadarPlacementEditor.svelte";

interface FloorplanFurniturePlacementContext {
  bounds: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
  roomId?: string;
}

interface FloorplanFurnitureRoom {
  id: string;
  pointsPx: Array<[number, number]>;
}

interface StoredFloorplanWorkspaceSessionOptions {
  getDocument: () => FloorplanStorageDocument | null;
  getVisibleRoomCandidates: () => RoomCandidate[];
  getSelectedRoomId: () => string;
  getRadarPlacement: () => StoredRadarPlacement | null;
  getRoomWallSegments: (placement: StoredRadarPlacement | null, segmentPrefix: string) => FloorplanOcclusionSegment[];
}

export function createStoredFloorplanWorkspaceSession({
  getDocument,
  getVisibleRoomCandidates,
  getSelectedRoomId,
  getRadarPlacement,
  getRoomWallSegments
}: StoredFloorplanWorkspaceSessionOptions) {
  function scaleEstimate(): FloorplanScaleEstimate | null {
    return storedFloorplanScaleEstimate(getDocument()?.scale);
  }

  function imageFrameStyle(): string {
    const bounds = viewBounds();
    return floorplanImageFrameStyle(bounds.width, bounds.height, 72);
  }

  function viewBounds(): StoredFloorplanViewBounds {
    return storedFloorplanViewBounds(getDocument());
  }

  function defaultFurniturePlacementContext(): FloorplanFurniturePlacementContext | null {
    const scale = scaleEstimate();
    if (!scale) return null;
    const selectedCandidate = getVisibleRoomCandidates().find((candidate) => candidate.id === getSelectedRoomId());
    return selectedCandidate
      ? {
          roomId: selectedCandidate.id,
          bounds: rectFromPoints(roomCandidatePoints(selectedCandidate))
        }
      : {
          bounds: scale.outerBounds
        };
  }

  function furnitureRooms(): FloorplanFurnitureRoom[] {
    return getVisibleRoomCandidates().map((candidate) => ({
      id: candidate.id,
      pointsPx: roomCandidatePoints(candidate).map(([x, y]) => [x, y])
    }));
  }

  function roomBoundarySegments(): FloorplanOcclusionSegment[] {
    return getRoomWallSegments(getRadarPlacement(), "stored-room");
  }

  function radarOcclusionSegments(): FloorplanOcclusionSegment[] {
    return getRoomWallSegments(getRadarPlacement(), "stored-room");
  }

  function radarScalePercent(): number {
    return (getDocument()?.radar?.scale ?? 1) * 100;
  }

  function ignoredOcclusionSegmentIds(): string[] {
    return getDocument()?.occlusion?.ignoredEdges ?? [];
  }

  function furnitureBounds() {
    return scaleEstimate()?.outerBounds;
  }

  return {
    scaleEstimate,
    viewBounds,
    imageFrameStyle,
    defaultFurniturePlacementContext,
    furnitureRooms,
    roomBoundarySegments,
    radarOcclusionSegments,
    radarScalePercent,
    ignoredOcclusionSegmentIds,
    furnitureBounds
  };
}
