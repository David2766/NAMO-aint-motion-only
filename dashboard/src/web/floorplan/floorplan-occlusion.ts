export const DEFAULT_OCCLUSION_GROUP_LINE_TOLERANCE_PX = 3;
export const DEFAULT_OCCLUSION_GROUP_TOUCH_TOLERANCE_PX = 6;

export type FloorplanPoint = [number, number];

export interface FloorplanRectBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface FloorplanBoundarySource {
  id: string;
  points: FloorplanPoint[];
  segmentPrefix: string;
}

export interface FloorplanOcclusionSegment {
  id: string;
  occlusionKey: string;
  locked?: boolean;
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  axis: "horizontal" | "vertical" | "diagonal";
}

export interface FloorplanCartesianPoint {
  x: number;
  y: number;
}

export function floorplanOcclusionSegmentKey(segment: FloorplanOcclusionSegment): string {
  return segment.occlusionKey ?? segment.id;
}

export function isFloorplanOcclusionSegmentIgnored(
  segment: FloorplanOcclusionSegment,
  ignoredEdges: ReadonlySet<string> | readonly string[]
): boolean {
  if (segment.locked) return false;
  const ignored = ignoredEdges instanceof Set ? ignoredEdges : new Set(ignoredEdges);
  return ignored.has(segment.id) || ignored.has(floorplanOcclusionSegmentKey(segment));
}

export function radarVisiblePolygonPoints(
  origin: FloorplanCartesianPoint,
  rayEndpoints: FloorplanCartesianPoint[],
  segments: FloorplanOcclusionSegment[],
  ignoredEdges: readonly string[] = []
): FloorplanCartesianPoint[] {
  const ignored = new Set(ignoredEdges);
  const blockingWalls = segments.filter(
    (segment) => !isFloorplanOcclusionSegmentIgnored(segment, ignored)
  );
  return [
    origin,
    ...rayEndpoints.map((rayEnd) => closestRadarRayWallHit(origin, rayEnd, blockingWalls) ?? rayEnd)
  ];
}

export function buildBoundaryOcclusionSegments(sources: FloorplanBoundarySource[]): FloorplanOcclusionSegment[] {
  return sources.flatMap((source) => {
    const points = source.points;
    if (points.length < 2) return [];
    return points.map((point, index) => {
      const next = points[(index + 1) % points.length];
      const [x1, y1] = point;
      const [x2, y2] = next;
      return {
        id: `${source.segmentPrefix}-${source.id}-${index}`,
        occlusionKey: wallSegmentGeometryKey(x1, y1, x2, y2),
        x1,
        y1,
        x2,
        y2,
        axis: segmentAxisFromPoints(x1, y1, x2, y2)
      };
    });
  });
}

export function nextIgnoredOcclusionEdges(
  ignoredEdges: string[],
  segment: FloorplanOcclusionSegment | string,
  segments: FloorplanOcclusionSegment[],
  lineTolerancePx = DEFAULT_OCCLUSION_GROUP_LINE_TOLERANCE_PX,
  touchTolerancePx = DEFAULT_OCCLUSION_GROUP_TOUCH_TOLERANCE_PX
): string[] {
  const target = resolveOcclusionSegment(segment, segments);
  const keys = occlusionSegmentGroupKeys(target, segments, lineTolerancePx, touchTolerancePx);
  const ignored = new Set(ignoredEdges);
  const shouldIgnore = keys.some((key) => !ignored.has(key));

  for (const key of occlusionSegmentLegacyKeys(target, segments, lineTolerancePx, touchTolerancePx)) {
    ignored.delete(key);
  }

  if (shouldIgnore) {
    for (const key of keys) {
      ignored.add(key);
    }
  } else {
    for (const key of keys) {
      ignored.delete(key);
    }
  }

  return [...ignored];
}

export function wallSegmentGeometryKey(x1: number, y1: number, x2: number, y2: number): string {
  const first = [Math.round(x1), Math.round(y1)];
  const second = [Math.round(x2), Math.round(y2)];
  const [start, end] =
    first[0] < second[0] || (first[0] === second[0] && first[1] <= second[1])
      ? [first, second]
      : [second, first];
  return `wall:${start[0]},${start[1]}-${end[0]},${end[1]}`;
}

function resolveOcclusionSegment(
  segment: FloorplanOcclusionSegment | string,
  segments: FloorplanOcclusionSegment[]
): FloorplanOcclusionSegment {
  if (segment && typeof segment === "object") return segment;
  return segments.find((candidate) => candidate.id === segment || candidate.occlusionKey === segment) ?? {
    id: segment,
    occlusionKey: segment,
    x1: 0,
    y1: 0,
    x2: 0,
    y2: 0,
    axis: "diagonal"
  };
}

function occlusionSegmentGroupKeys(
  target: FloorplanOcclusionSegment,
  segments: FloorplanOcclusionSegment[],
  lineTolerancePx: number,
  touchTolerancePx: number
): string[] {
  return uniqueStrings(
    occlusionSegmentGroup(target, segments, lineTolerancePx, touchTolerancePx)
      .map((segment) => segment.occlusionKey ?? segment.id)
      .filter(Boolean)
  );
}

function occlusionSegmentLegacyKeys(
  target: FloorplanOcclusionSegment,
  segments: FloorplanOcclusionSegment[],
  lineTolerancePx: number,
  touchTolerancePx: number
): string[] {
  return uniqueStrings(
    occlusionSegmentGroup(target, segments, lineTolerancePx, touchTolerancePx)
      .flatMap((segment) => [segment.id, segment.occlusionKey])
      .filter(Boolean)
  );
}

function occlusionSegmentGroup(
  target: FloorplanOcclusionSegment,
  segments: FloorplanOcclusionSegment[],
  lineTolerancePx: number,
  touchTolerancePx: number
): FloorplanOcclusionSegment[] {
  if (!target) return [];
  return segments.filter((segment) => areSameOcclusionWall(target, segment, lineTolerancePx, touchTolerancePx));
}

function areSameOcclusionWall(
  a: FloorplanOcclusionSegment,
  b: FloorplanOcclusionSegment,
  lineTolerancePx: number,
  touchTolerancePx: number
): boolean {
  if (!a || !b) return false;
  if ((a.occlusionKey ?? a.id) === (b.occlusionKey ?? b.id)) return true;
  const axis = occlusionSegmentAxis(a);
  if (axis !== occlusionSegmentAxis(b)) return false;
  if (axis === "horizontal") {
    return (
      Math.abs(segmentMidY(a) - segmentMidY(b)) <= lineTolerancePx &&
      rangesTouchOrOverlap(segmentRangeX(a), segmentRangeX(b), touchTolerancePx)
    );
  }
  if (axis === "vertical") {
    return (
      Math.abs(segmentMidX(a) - segmentMidX(b)) <= lineTolerancePx &&
      rangesTouchOrOverlap(segmentRangeY(a), segmentRangeY(b), touchTolerancePx)
    );
  }
  return false;
}

function occlusionSegmentAxis(segment: FloorplanOcclusionSegment): FloorplanOcclusionSegment["axis"] {
  if (segment.axis === "horizontal" || segment.axis === "vertical") return segment.axis;
  return segmentAxisFromPoints(segment.x1, segment.y1, segment.x2, segment.y2);
}

function segmentAxisFromPoints(
  x1: number,
  y1: number,
  x2: number,
  y2: number
): FloorplanOcclusionSegment["axis"] {
  const dx = Math.abs(Number(x2) - Number(x1));
  const dy = Math.abs(Number(y2) - Number(y1));
  if (dy <= 1) return "horizontal";
  if (dx <= 1) return "vertical";
  return "diagonal";
}

function segmentMidX(segment: FloorplanOcclusionSegment): number {
  return (Number(segment.x1) + Number(segment.x2)) / 2;
}

function segmentMidY(segment: FloorplanOcclusionSegment): number {
  return (Number(segment.y1) + Number(segment.y2)) / 2;
}

function segmentRangeX(segment: FloorplanOcclusionSegment): [number, number] {
  return [Math.min(Number(segment.x1), Number(segment.x2)), Math.max(Number(segment.x1), Number(segment.x2))];
}

function segmentRangeY(segment: FloorplanOcclusionSegment): [number, number] {
  return [Math.min(Number(segment.y1), Number(segment.y2)), Math.max(Number(segment.y1), Number(segment.y2))];
}

function rangesTouchOrOverlap(a: [number, number], b: [number, number], tolerance: number): boolean {
  return Math.max(a[0], b[0]) <= Math.min(a[1], b[1]) + tolerance;
}

function uniqueStrings(values: string[]): string[] {
  return [...new Set(values)];
}

function closestRadarRayWallHit(
  origin: FloorplanCartesianPoint,
  rayEnd: FloorplanCartesianPoint,
  walls: FloorplanOcclusionSegment[]
): FloorplanCartesianPoint | null {
  let nearest: (FloorplanCartesianPoint & { distance: number }) | null = null;
  for (const wall of walls) {
    const hit = segmentIntersectionPoint(
      origin,
      rayEnd,
      { x: wall.x1, y: wall.y1 },
      { x: wall.x2, y: wall.y2 }
    );
    if (!hit) continue;
    const distance = Math.hypot(hit.x - origin.x, hit.y - origin.y);
    if (distance < 0.5 || (nearest && distance >= nearest.distance)) continue;
    nearest = { ...hit, distance };
  }
  return nearest ? { x: nearest.x, y: nearest.y } : null;
}

function segmentIntersectionPoint(
  a: FloorplanCartesianPoint,
  b: FloorplanCartesianPoint,
  c: FloorplanCartesianPoint,
  d: FloorplanCartesianPoint
): FloorplanCartesianPoint | null {
  const denominator = (a.x - b.x) * (c.y - d.y) - (a.y - b.y) * (c.x - d.x);
  if (Math.abs(denominator) < 0.001) return null;
  const determinantA = a.x * b.y - a.y * b.x;
  const determinantB = c.x * d.y - c.y * d.x;
  const point = {
    x: normalizeCoordinate(
      (determinantA * (c.x - d.x) - (a.x - b.x) * determinantB) / denominator
    ),
    y: normalizeCoordinate(
      (determinantA * (c.y - d.y) - (a.y - b.y) * determinantB) / denominator
    )
  };
  return pointOnSegment(a, point, b) && pointOnSegment(c, point, d) ? point : null;
}

function normalizeCoordinate(value: number): number {
  return Math.abs(value) < 0.000001 ? 0 : value;
}

function pointOnSegment(
  start: FloorplanCartesianPoint,
  point: FloorplanCartesianPoint,
  end: FloorplanCartesianPoint
): boolean {
  return (
    Math.min(start.x, end.x) <= point.x + 0.001 &&
    point.x <= Math.max(start.x, end.x) + 0.001 &&
    Math.min(start.y, end.y) <= point.y + 0.001 &&
    point.y <= Math.max(start.y, end.y) + 0.001
  );
}
