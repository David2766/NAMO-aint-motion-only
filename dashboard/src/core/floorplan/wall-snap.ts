import type { FloorplanWallSegment, RoomCandidate } from "./floorplan-types";

export interface FloorplanSnapEdge {
  key?: string;
  index?: number;
  axis: "horizontal" | "vertical";
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

export interface FloorplanSnapCandidate {
  segment: FloorplanWallSegment;
  distance: number;
  overlap: number;
}

export interface FloorplanWallSnapOptions {
  maxDistance?: number;
  minOverlap?: number;
  minLength?: number | null;
  clusterDistance?: number;
  maxVisible?: number;
  imageWidth?: number;
  imageHeight?: number;
  imageEdgeMargin?: number;
}

const DEFAULT_MAX_DISTANCE = 128;
const DEFAULT_MIN_OVERLAP = 0.22;
const DEFAULT_MIN_LENGTH = 96;
const DEFAULT_CLUSTER_DISTANCE = 24;
const DEFAULT_MAX_VISIBLE = 8;
const DEFAULT_IMAGE_EDGE_MARGIN = 4;

export function getRoomCandidateSnapEdges(candidate: RoomCandidate): FloorplanSnapEdge[] {
  if (candidate.shape !== "polygon" || !candidate.points?.length) {
    const { x, y, width, height } = candidate.rect;
    return [
      { key: "rect:top", axis: "horizontal", x1: x, y1: y, x2: x + width, y2: y },
      { key: "rect:right", axis: "vertical", x1: x + width, y1: y, x2: x + width, y2: y + height },
      { key: "rect:bottom", axis: "horizontal", x1: x, y1: y + height, x2: x + width, y2: y + height },
      { key: "rect:left", axis: "vertical", x1: x, y1: y, x2: x, y2: y + height }
    ];
  }

  return candidate.points
    .map((_, index) => getPolygonSnapEdge(candidate.points!, index))
    .filter((edge): edge is FloorplanSnapEdge => Boolean(edge));
}

export function getPolygonSnapEdge(points: Array<[number, number]>, index: number): FloorplanSnapEdge | null {
  const [x1, y1] = points[index];
  const [x2, y2] = points[(index + 1) % points.length];
  const dx = Math.abs(x2 - x1);
  const dy = Math.abs(y2 - y1);
  if (dx <= 0.001 && dy <= 0.001) return null;
  if (dx >= dy * 2) return { key: `poly:${index}`, index, axis: "horizontal", x1, y1, x2, y2 };
  if (dy >= dx * 2) return { key: `poly:${index}`, index, axis: "vertical", x1, y1, x2, y2 };
  return null;
}

export function getVisibleSnapWallCandidates(
  edge: FloorplanSnapEdge,
  segments: FloorplanWallSegment[],
  options: FloorplanWallSnapOptions = {}
): FloorplanSnapCandidate[] {
  const maxDistance = options.maxDistance ?? DEFAULT_MAX_DISTANCE;
  const minOverlap = options.minOverlap ?? DEFAULT_MIN_OVERLAP;
  const minLengthBase = options.minLength === undefined ? DEFAULT_MIN_LENGTH : options.minLength;
  const clusterDistance = options.clusterDistance ?? DEFAULT_CLUSTER_DISTANCE;
  const maxVisible = options.maxVisible ?? DEFAULT_MAX_VISIBLE;
  const imageEdgeMargin = options.imageEdgeMargin ?? DEFAULT_IMAGE_EDGE_MARGIN;
  const edgeLength = edge.axis === "horizontal"
    ? Math.abs(edge.x2 - edge.x1)
    : Math.abs(edge.y2 - edge.y1);
  const minLength = minLengthBase === null
    ? 0
    : Math.min(Math.max(minLengthBase, edgeLength * 0.18), edgeLength * 0.9);

  const candidates = segments
    .filter((segment) => segment.axis === edge.axis)
    .filter((segment) => !isImageEdgeSegment(segment, options.imageWidth, options.imageHeight, imageEdgeMargin))
    .map((segment) => ({
      segment,
      distance: edgeDistanceToSegment(edge, segment),
      overlap: edgeSegmentOverlap(edge, segment)
    }))
    .filter(({ segment, distance, overlap }) =>
      segment.length >= minLength &&
      distance <= maxDistance &&
      overlap >= minOverlap
    );

  return chooseVisibleSnapCandidates(candidates, clusterDistance, maxVisible);
}

function isImageEdgeSegment(
  segment: FloorplanWallSegment,
  imageWidth: number | undefined,
  imageHeight: number | undefined,
  margin: number
): boolean {
  if (segment.axis === "horizontal" && imageHeight !== undefined) {
    return segment.y1 <= margin || segment.y1 >= imageHeight - margin;
  }
  if (segment.axis === "vertical" && imageWidth !== undefined) {
    return segment.x1 <= margin || segment.x1 >= imageWidth - margin;
  }
  return false;
}

export function edgeDistanceToSegment(edge: FloorplanSnapEdge, segment: FloorplanWallSegment): number {
  if (edge.axis === "horizontal") return Math.abs(((edge.y1 + edge.y2) / 2) - segment.y1);
  return Math.abs(((edge.x1 + edge.x2) / 2) - segment.x1);
}

export function edgeSegmentOverlap(edge: FloorplanSnapEdge, segment: FloorplanWallSegment): number {
  if (edge.axis === "horizontal") {
    return overlapRatio(
      Math.min(edge.x1, edge.x2),
      Math.max(edge.x1, edge.x2),
      Math.min(segment.x1, segment.x2),
      Math.max(segment.x1, segment.x2)
    );
  }
  return overlapRatio(
    Math.min(edge.y1, edge.y2),
    Math.max(edge.y1, edge.y2),
    Math.min(segment.y1, segment.y2),
    Math.max(segment.y1, segment.y2)
  );
}

function chooseVisibleSnapCandidates(
  candidates: FloorplanSnapCandidate[],
  clusterDistance: number,
  maxVisible: number
): FloorplanSnapCandidate[] {
  const groups: Array<{ coordinate: number; items: FloorplanSnapCandidate[] }> = [];
  const sorted = [...candidates].sort((a, b) => snapCoordinate(a.segment) - snapCoordinate(b.segment));
  for (const candidate of sorted) {
    const coordinate = snapCoordinate(candidate.segment);
    const group = groups.find((item) => Math.abs(item.coordinate - coordinate) <= clusterDistance);
    if (group) {
      group.items.push(candidate);
      group.coordinate = (group.coordinate * (group.items.length - 1) + coordinate) / group.items.length;
    } else {
      groups.push({ coordinate, items: [candidate] });
    }
  }

  return groups
    .map((group) => group.items.sort(compareSnapCandidates)[0])
    .sort(compareSnapCandidates)
    .slice(0, maxVisible);
}

function compareSnapCandidates(a: FloorplanSnapCandidate, b: FloorplanSnapCandidate): number {
  return (
    b.overlap - a.overlap ||
    a.distance - b.distance ||
    b.segment.length - a.segment.length
  );
}

function snapCoordinate(segment: FloorplanWallSegment): number {
  return segment.axis === "horizontal" ? segment.y1 : segment.x1;
}

function overlapRatio(aMin: number, aMax: number, bMin: number, bMax: number): number {
  const overlap = Math.max(0, Math.min(aMax, bMax) - Math.max(aMin, bMin));
  return overlap / Math.max(1, aMax - aMin);
}
