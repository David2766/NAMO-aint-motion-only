import type { RoomCandidate } from "./floorplan-types";

const ANGLE_TOLERANCE_DEG = 10;
const MAX_POINT_MOVE_PX = 30;
const MAX_AREA_SHRINK_RATIO = 0.15;

type Point = [number, number];

export interface RoomCandidateCleanupResult {
  candidate: RoomCandidate;
  changed: boolean;
  reason: "not_polygon" | "not_orthogonal_enough" | "expanded" | "over_shrunk" | "self_intersection" | "point_moved_too_far" | "cleaned";
}

export function cleanupOrthogonalRoomCandidate(candidate: RoomCandidate): RoomCandidateCleanupResult {
  if (candidate.shape !== "polygon" || !candidate.points || candidate.points.length < 3) {
    return { candidate, changed: false, reason: "not_polygon" };
  }

  const points = candidate.points.map(([x, y]) => [x, y] as Point);
  const bounds = boundsFromPoints(points);
  const area = polygonArea(points);
  if (area <= 0) return { candidate, changed: false, reason: "not_orthogonal_enough" };

  const proposed = points.map(([x, y]) => ({ x, y }));
  let adjustedEdges = 0;

  points.forEach(([x1, y1], index) => {
    const nextIndex = (index + 1) % points.length;
    const [x2, y2] = points[nextIndex];
    const dx = x2 - x1;
    const dy = y2 - y1;
    const length = Math.hypot(dx, dy);
    if (length < 8) return;

    const angle = Math.abs(Math.atan2(dy, dx) * 180 / Math.PI);
    const horizontalDelta = Math.min(angle, Math.abs(180 - angle));
    const verticalDelta = Math.abs(90 - angle);

    if (horizontalDelta <= ANGLE_TOLERANCE_DEG) {
      const edgeMidY = (y1 + y2) / 2;
      const targetY = edgeMidY <= bounds.y + bounds.height / 2
        ? Math.max(y1, y2)
        : Math.min(y1, y2);
      proposed[index].y = targetY;
      proposed[nextIndex].y = targetY;
      adjustedEdges += 1;
    } else if (verticalDelta <= ANGLE_TOLERANCE_DEG) {
      const edgeMidX = (x1 + x2) / 2;
      const targetX = edgeMidX <= bounds.x + bounds.width / 2
        ? Math.max(x1, x2)
        : Math.min(x1, x2);
      proposed[index].x = targetX;
      proposed[nextIndex].x = targetX;
      adjustedEdges += 1;
    }
  });

  if (adjustedEdges < Math.max(2, Math.ceil(points.length / 2))) {
    return { candidate, changed: false, reason: "not_orthogonal_enough" };
  }

  const nextPoints = proposed.map(({ x, y }) => [Math.round(x), Math.round(y)] as Point);
  if (samePoints(points, nextPoints)) return { candidate, changed: false, reason: "not_orthogonal_enough" };

  const maxMove = Math.max(...points.map(([x, y], index) => {
    const [nextX, nextY] = nextPoints[index];
    return Math.hypot(nextX - x, nextY - y);
  }));
  if (maxMove > MAX_POINT_MOVE_PX) return { candidate, changed: false, reason: "point_moved_too_far" };

  const nextBounds = boundsFromPoints(nextPoints);
  const nextArea = polygonArea(nextPoints);
  if (nextBounds.width > bounds.width || nextBounds.height > bounds.height || nextArea > area) {
    return { candidate, changed: false, reason: "expanded" };
  }
  if (nextArea < area * (1 - MAX_AREA_SHRINK_RATIO)) {
    return { candidate, changed: false, reason: "over_shrunk" };
  }
  if (hasSelfIntersection(nextPoints)) {
    return { candidate, changed: false, reason: "self_intersection" };
  }

  return {
    candidate: {
      ...candidate,
      points: nextPoints,
      rect: nextBounds,
      debug: {
        ...(candidate.debug ?? {}),
        finalPoints: nextPoints.length,
        orthogonalCleanup: true
      }
    },
    changed: true,
    reason: "cleaned"
  };
}

function samePoints(a: Point[], b: Point[]): boolean {
  return a.length === b.length && a.every(([x, y], index) => x === b[index][0] && y === b[index][1]);
}

function boundsFromPoints(points: Point[]) {
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  const x = Math.min(...xs);
  const y = Math.min(...ys);
  return {
    x,
    y,
    width: Math.max(...xs) - x,
    height: Math.max(...ys) - y
  };
}

function polygonArea(points: Point[]): number {
  let sum = 0;
  for (let index = 0; index < points.length; index += 1) {
    const [x1, y1] = points[index];
    const [x2, y2] = points[(index + 1) % points.length];
    sum += x1 * y2 - x2 * y1;
  }
  return Math.abs(sum / 2);
}

function hasSelfIntersection(points: Point[]): boolean {
  for (let a = 0; a < points.length; a += 1) {
    const aNext = (a + 1) % points.length;
    for (let b = a + 1; b < points.length; b += 1) {
      const bNext = (b + 1) % points.length;
      if (a === b || aNext === b || bNext === a) continue;
      if (segmentsIntersect(points[a], points[aNext], points[b], points[bNext])) return true;
    }
  }
  return false;
}

function segmentsIntersect(a: Point, b: Point, c: Point, d: Point): boolean {
  const o1 = orientation(a, b, c);
  const o2 = orientation(a, b, d);
  const o3 = orientation(c, d, a);
  const o4 = orientation(c, d, b);
  return o1 * o2 < 0 && o3 * o4 < 0;
}

function orientation(a: Point, b: Point, c: Point): number {
  return Math.sign((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
}
