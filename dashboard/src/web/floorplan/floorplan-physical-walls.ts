import type {
  FloorplanStorageDocument,
  FloorplanStorageRoom
} from "../../core/floorplan/floorplan-storage";
import type { FloorplanWallSegment, RoomCandidate } from "../../core/floorplan/floorplan-types";
import {
  buildBoundaryOcclusionSegments,
  wallSegmentGeometryKey,
  type FloorplanOcclusionSegment,
  type FloorplanPoint
} from "./floorplan-occlusion";
import { roomCandidatePoints } from "./room-candidate-points";

interface PhysicalWallSource {
  id: string;
  points: FloorplanPoint[];
}

export function buildManualPhysicalWallSegments(
  outline: FloorplanPoint[],
  rooms: RoomCandidate[]
): FloorplanWallSegment[] {
  return physicalWallsFromSources([
    ...(outline.length >= 3 ? [{ id: "outline", points: outline }] : []),
    ...rooms
      .filter((room) => room.status !== "rejected")
      .map((room) => ({ id: `room-${room.id}`, points: roomCandidatePoints(room) }))
  ]);
}

export function ensureStoredPhysicalWalls(document: FloorplanStorageDocument): FloorplanStorageDocument {
  if (document.wallSegments?.length || document.source !== "manual") return document;
  const outline = document.outline?.pointsPx ?? [];
  const walls = physicalWallsFromSources([
    ...(outline.length >= 3
      ? [{ id: "outline", points: outline }]
      : []),
    ...document.rooms.map((room) => ({ id: `room-${room.id}`, points: room.pointsPx }))
  ]);
  return walls.length ? { ...document, wallSegments: walls } : document;
}

export function physicalWallOcclusionSegments(
  document: FloorplanStorageDocument | null | undefined
): FloorplanOcclusionSegment[] {
  if (!document) return [];
  if (document.wallSegments?.length) return occlusionSegmentsFromWalls(document.wallSegments);
  return legacyRoomOcclusionSegments(document.rooms);
}

export function occlusionSegmentsFromWalls(
  walls: FloorplanWallSegment[]
): FloorplanOcclusionSegment[] {
  return uniqueOcclusionSegments(walls.map((wall, index) => ({
    id: wall.id || `physical-wall-${index}`,
    occlusionKey: wallSegmentGeometryKey(wall.x1, wall.y1, wall.x2, wall.y2),
    x1: wall.x1,
    y1: wall.y1,
    x2: wall.x2,
    y2: wall.y2,
    axis: wall.axis ?? segmentAxis(wall.x1, wall.y1, wall.x2, wall.y2)
  })));
}

function physicalWallsFromSources(sources: PhysicalWallSource[]): FloorplanWallSegment[] {
  const walls: FloorplanWallSegment[] = [];
  const seen = new Set<string>();
  for (const source of sources) {
    if (source.points.length < 2) continue;
    source.points.forEach(([x1, y1], index) => {
      const [x2, y2] = source.points[(index + 1) % source.points.length];
      const key = wallSegmentGeometryKey(x1, y1, x2, y2);
      if (seen.has(key)) return;
      seen.add(key);
      walls.push({
        id: `physical-${source.id}-${index}`,
        axis: segmentAxis(x1, y1, x2, y2),
        x1,
        y1,
        x2,
        y2,
        length: Math.hypot(x2 - x1, y2 - y1)
      });
    });
  }
  return walls;
}

function legacyRoomOcclusionSegments(rooms: FloorplanStorageRoom[]): FloorplanOcclusionSegment[] {
  return uniqueOcclusionSegments(buildBoundaryOcclusionSegments(
    rooms.map((room) => ({
      id: room.id,
      points: room.pointsPx,
      segmentPrefix: "legacy-room"
    }))
  ));
}

function uniqueOcclusionSegments(segments: FloorplanOcclusionSegment[]): FloorplanOcclusionSegment[] {
  const seen = new Set<string>();
  return segments.filter((segment) => {
    const key = segment.occlusionKey || wallSegmentGeometryKey(segment.x1, segment.y1, segment.x2, segment.y2);
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

function segmentAxis(
  x1: number,
  y1: number,
  x2: number,
  y2: number
): FloorplanWallSegment["axis"] {
  if (Math.abs(y2 - y1) <= 1) return "horizontal";
  if (Math.abs(x2 - x1) <= 1) return "vertical";
  return "diagonal";
}
