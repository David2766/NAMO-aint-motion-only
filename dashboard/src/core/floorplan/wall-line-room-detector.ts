import type { FloorplanWallSegment, RoomCandidate } from "./floorplan-types";
import { cleanupOrthogonalRoomCandidate } from "./room-candidate-cleanup";
import { detectFloorplanTextBoxes } from "./text-box-detector";
import { getPolygonSnapEdge, getVisibleSnapWallCandidates } from "./wall-snap";

export interface WallLineRoomDetectorDebug {
  engine: "wall-line";
  imageWidth: number;
  imageHeight: number;
  cellSize: number;
  gridWidth: number;
  gridHeight: number;
  totalCells: number;
  darkPixels: number;
  wallCells: number;
  expandedWallCells: number;
  freeComponents: number;
  acceptedBeforeSanitize: number;
  acceptedAfterSanitize: number;
  rejectedSmall: number;
  rejectedTooSmallArea: number;
  rejectedTooLargeArea: number;
  rejectedSparse: number;
  rejectedThin: number;
  filtersEnabled: boolean;
  polygonRawPoints: number;
  polygonSimplifiedPoints: number;
  polygonFinalPoints: number;
  polygonClosedLoops: number;
  polygonOpenLoops: number;
  thickWallMode: boolean;
  wallCellThreshold: number;
  gapClosedCells: number;
  attachedThinWallCells: number;
  externalFreeCells: number;
  rejectedExternal: number;
  removedSmallWallComponents: number;
  removedSmallWallCells: number;
  textExclusionEnabled: boolean;
  textCandidateBoxes: number;
  textExcludedPixels: number;
  wallMaskCells: WallMaskDebugCell[];
  wallRejectedCells: WallMaskDebugCell[];
  wallSnap: WallSnapDebug;
}

interface WallMaskDebugCell {
  x: number;
  y: number;
  width: number;
  height: number;
  gridX: number;
  gridY: number;
  source: string;
  reason: string;
  darkPixels: number;
}

interface WallSnapDebug {
  edgeChecks: number;
  noAxisEdges: number;
  candidateQueries: number;
  candidateNone: number;
  alreadyAligned: number;
  moveAttempts: number;
  applied: number;
  rejectedSelfIntersection: number;
  rejectedOverExpanded: number;
  cornerAttempts: number;
  cornerApplied: number;
  cornerNoIntersection: number;
  cornerExistingPoint: number;
  cornerSpike: number;
  cornerSelfIntersection: number;
  logs: string[];
}

export interface WallLineRoomDetectorResult {
  candidates: RoomCandidate[];
  debugCandidates: RoomCandidate[];
  wallSegments: FloorplanWallSegment[];
  snapSegments: FloorplanWallSegment[];
  debug: WallLineRoomDetectorDebug;
}

interface DetectWallLineRoomOptions {
  imageData: ImageData;
  applyCandidateFilters?: boolean;
  preferThickWalls?: boolean;
  excludeTextLikeNoise?: boolean;
}

interface GridCell {
  x: number;
  y: number;
}

interface GridComponent {
  cells: number;
  cellList: GridCell[];
  cellSet: Set<string>;
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

interface PolygonTraceResult {
  points: Array<[number, number]>;
  rawPoints: number;
  simplifiedPoints: number;
  finalPoints: number;
  closedLoop: boolean;
}

interface WallGridResult {
  mask: Uint8Array;
  counts: Uint16Array;
  sources: Uint8Array;
}

interface ThinWallSegment {
  axis: "horizontal" | "vertical";
  line: number;
  start: number;
  end: number;
  length: number;
}

interface ParallelThinWallBand {
  segments: ThinWallSegment[];
  start: number;
  end: number;
}

interface PolygonWallGuard {
  mask: Uint8Array;
  width: number;
  height: number;
  segments: FloorplanWallSegment[];
}

const CELL_SIZE = 8;
const MAX_CANDIDATES = 18;
const AUTO_HIDE_CONFIDENCE_MAX = 62;
const AUTO_HIDE_TINY_SIZE_PX = 41;
const AUTO_HIDE_TINY_CONFIDENCE_MAX = 77;
const MAX_POLYGON_POINTS = 48;
const POLYGON_POINT_CLUSTER_RADIUS_CELLS = 5;
const POLYGON_POINT_CLUSTER_MIN_POINTS = 3;
const POLYGON_AXIS_SNAP_RADIUS_CELLS = 2;
const POLYGON_SHORT_DIAGONAL_MAX_CELLS = 4;
const POLYGON_CURVE_NOISE_EDGE_MAX_CELLS = 3;
const POLYGON_CURVE_NOISE_MAX_PASSES = 2;
const POLYGON_WALL_TRIM_MAX_PASSES = 3;
const POLYGON_WALL_TRIM_MAX_AREA_SHRINK_RATIO = 0.2;
const POLYGON_CROWDED_POINT_RADIUS_PX = 24;
const POLYGON_CROWDED_POINT_MIN_COUNT = 3;
const POLYGON_FINAL_REFINE_MAX_PASSES = 8;
const POLYGON_AUTO_WALL_SNAP_TOUCH_DISTANCE_PX = 2;
const POLYGON_AUTO_WALL_SNAP_MIN_SIZE_GROWTH_RATIO = 0.6;
const POLYGON_AUTO_WALL_SNAP_MAX_SIZE_GROWTH_RATIO = 0.8;
const POLYGON_AUTO_WALL_SNAP_SMALL_SIZE_PX = 50;
const POLYGON_AUTO_WALL_SNAP_LARGE_SIZE_PX = 80;
const POLYGON_MISSING_CORNER_MAX_DISTANCE_PX = 128;
const POLYGON_EXISTING_CORNER_DISTANCE_PX = 6;
const POLYGON_COLLINEAR_ANGLE_DEGREES = 175;
const POLYGON_SPIKE_ANGLE_DEGREES = 150;
const DARK_THRESHOLD = 118;
const CLOSED_THIN_WALL_DARK_THRESHOLD = 200;
const DEFAULT_WALL_CELL_THRESHOLD = 5;
const THICK_WALL_CELL_THRESHOLD = 8;
const ATTACHED_THIN_WALL_MAX_CELLS = 18;
const PARALLEL_THIN_WALL_MIN_CELLS = 5;
const PARALLEL_THIN_WALL_MIN_GAP_CELLS = 1;
const PARALLEL_THIN_WALL_MAX_GAP_CELLS = 5;
const PARALLEL_THIN_WALL_MIN_OVERLAP_RATIO = 0.58;
const PARALLEL_THIN_WALL_MAX_REPEATS = 4;
const PARALLEL_THIN_WALL_ATTACH_RADIUS_CELLS = 2;
const PARALLEL_THIN_WALL_MAX_BAND_THICKNESS_PX = 20;
const CLOSED_THIN_WALL_MAX_THICKNESS_PX = 16;
const CLOSED_THIN_WALL_MIN_LENGTH_PX = 15;
const CLOSED_THIN_WALL_MAX_AREA_PX = 4800;
const CLOSED_THIN_WALL_MIN_ASPECT_RATIO = 3;
const CLOSED_THIN_WALL_END_TOUCH_RADIUS_PX = 50;
const WALL_SOURCE_BOUNDARY = 1;
const WALL_SOURCE_THRESHOLD = 2;
const WALL_SOURCE_ATTACHED_THIN = 3;
const WALL_SOURCE_PARALLEL_THIN = 4;
const EXTERNAL_OVERLAP_REJECT_RATIO = 0.5;
const WALL_SEGMENT_MIN_CELLS = 4;
const WALL_SNAP_DEBUG_MAX_LOGS = 40;

function createWallSnapDebug(): WallSnapDebug {
  return {
    edgeChecks: 0,
    noAxisEdges: 0,
    candidateQueries: 0,
    candidateNone: 0,
    alreadyAligned: 0,
    moveAttempts: 0,
    applied: 0,
    rejectedSelfIntersection: 0,
    rejectedOverExpanded: 0,
    cornerAttempts: 0,
    cornerApplied: 0,
    cornerNoIntersection: 0,
    cornerExistingPoint: 0,
    cornerSpike: 0,
    cornerSelfIntersection: 0,
    logs: []
  };
}

function pushWallSnapLog(debug: WallSnapDebug, message: string): void {
  if (debug.logs.length >= WALL_SNAP_DEBUG_MAX_LOGS) return;
  debug.logs.push(message);
}

export function detectWallLineRoomCandidates({
  imageData,
  applyCandidateFilters = true,
  preferThickWalls = false,
  excludeTextLikeNoise = false
}: DetectWallLineRoomOptions): WallLineRoomDetectorResult {
  const gridWidth = Math.ceil(imageData.width / CELL_SIZE);
  const gridHeight = Math.ceil(imageData.height / CELL_SIZE);
  const debug: WallLineRoomDetectorDebug = {
    engine: "wall-line",
    imageWidth: imageData.width,
    imageHeight: imageData.height,
    cellSize: CELL_SIZE,
    gridWidth,
    gridHeight,
    totalCells: gridWidth * gridHeight,
    darkPixels: 0,
    wallCells: 0,
    expandedWallCells: 0,
    freeComponents: 0,
    acceptedBeforeSanitize: 0,
    acceptedAfterSanitize: 0,
    rejectedSmall: 0,
    rejectedTooSmallArea: 0,
    rejectedTooLargeArea: 0,
    rejectedSparse: 0,
    rejectedThin: 0,
    filtersEnabled: applyCandidateFilters,
    polygonRawPoints: 0,
    polygonSimplifiedPoints: 0,
    polygonFinalPoints: 0,
    polygonClosedLoops: 0,
    polygonOpenLoops: 0,
    thickWallMode: preferThickWalls,
    wallCellThreshold: preferThickWalls ? THICK_WALL_CELL_THRESHOLD : DEFAULT_WALL_CELL_THRESHOLD,
    gapClosedCells: 0,
    attachedThinWallCells: 0,
    externalFreeCells: 0,
    rejectedExternal: 0,
    removedSmallWallComponents: 0,
    removedSmallWallCells: 0,
    textExclusionEnabled: excludeTextLikeNoise,
    textCandidateBoxes: 0,
    textExcludedPixels: 0,
    wallMaskCells: [],
    wallRejectedCells: [],
    wallSnap: createWallSnapDebug()
  };

  const textExclusionMask = excludeTextLikeNoise ? buildTextExclusionMask(imageData, debug) : null;
  const wallGrid = buildWallGrid(imageData, debug, textExclusionMask);
  const wallMask = wallGrid.mask;
  removeIsolatedThresholdWallCells(wallMask, wallGrid.counts, wallGrid.sources, gridWidth, gridHeight, debug);
  if (preferThickWalls) {
    restoreAttachedThinWalls(
      wallMask,
      wallGrid.counts,
      wallGrid.sources,
      gridWidth,
      gridHeight,
      DEFAULT_WALL_CELL_THRESHOLD,
      ATTACHED_THIN_WALL_MAX_CELLS,
      debug
    );
    promoteClosedThinWallGaps(imageData, wallMask, wallGrid.sources, gridWidth, gridHeight, textExclusionMask, debug);
    promoteParallelThinWalls(wallMask, wallGrid.counts, wallGrid.sources, gridWidth, gridHeight, debug);
    removeOverwideParallelThinWallComponents(wallMask, wallGrid.counts, wallGrid.sources, gridWidth, gridHeight, debug);
  }
  removeWeaklyConnectedThresholdWallCells(wallMask, wallGrid.counts, wallGrid.sources, gridWidth, gridHeight, debug);
  debug.wallMaskCells = maskToDebugCells(
    wallMask,
    wallGrid.counts,
    wallGrid.sources,
    gridWidth,
    gridHeight,
    imageData.width,
    imageData.height
  );
  const expandedWallMask = expandMask(wallMask, gridWidth, gridHeight, 1);
  debug.expandedWallCells = countMask(expandedWallMask);
  const wallSegments = extractWallSegments(wallMask, gridWidth, gridHeight);
  const snapSegments = wallSegments;

  // The expanded wall mask is useful for debugging, but final room candidates
  // are taken from the original wall mask so their borders stay closer to walls.
  const freeMask = invertMask(wallMask);
  const externalFreeMask = findExternalFreeMask(freeMask, gridWidth, gridHeight);
  debug.externalFreeCells = countMask(externalFreeMask);
  const debugCandidates: RoomCandidate[] = [];
  const components = collectComponents(freeMask, gridWidth, gridHeight)
    .filter((component, index) => {
      const externalRatio = getExternalComponentRatio(component, externalFreeMask, debug);
      const shouldReject = externalRatio >= EXTERNAL_OVERLAP_REJECT_RATIO;
      if (shouldReject) {
        debug.rejectedExternal += 1;
        const debugCandidate = componentToDebugCandidate(component, index, imageData, externalRatio);
        if (debugCandidate) debugCandidates.push(debugCandidate);
      }
      return !(applyCandidateFilters && shouldReject);
    });
  debug.freeComponents = components.length;

  const imageArea = imageData.width * imageData.height;
  const wallGuard = { mask: wallMask, width: gridWidth, height: gridHeight, segments: snapSegments.length ? snapSegments : wallSegments };
  const rawCandidates = components
    .map((component, index) =>
      componentToCandidate(component, index, imageArea, imageData, debug, applyCandidateFilters, wallGuard)
    )
    .filter((candidate): candidate is RoomCandidate => Boolean(candidate))
    .sort((a, b) => b.confidence - a.confidence)
    .slice(0, applyCandidateFilters ? MAX_CANDIDATES : components.length);

  debug.acceptedBeforeSanitize = rawCandidates.length;
  const sanitizedCandidates = sanitizeRoomCandidates(rawCandidates, imageData, applyCandidateFilters);
  const visibleCandidates = applyCandidateFilters ? autoHideWeakRoomCandidates(sanitizedCandidates) : sanitizedCandidates;
  const candidates = renameFinalWallRoomCandidates(visibleCandidates);
  relabelWallSnapDebugLogs(debug.wallSnap, visibleCandidates, candidates);
  debug.acceptedAfterSanitize = candidates.length;

  return { candidates, debugCandidates, wallSegments, snapSegments, debug };
}

function renameFinalWallRoomCandidates(candidates: RoomCandidate[]): RoomCandidate[] {
  return candidates.map((candidate, index) =>
    candidate.id.startsWith("wall_room_")
      ? { ...candidate, name: `방 후보 ${index + 1}` }
      : candidate
  );
}

function autoHideWeakRoomCandidates(candidates: RoomCandidate[]): RoomCandidate[] {
  return candidates.map((candidate) => {
    const tiny = candidate.rect.width <= AUTO_HIDE_TINY_SIZE_PX && candidate.rect.height <= AUTO_HIDE_TINY_SIZE_PX;
    const lowConfidence = candidate.confidence <= AUTO_HIDE_CONFIDENCE_MAX;
    const tinyAndNotStrong = tiny && candidate.confidence <= AUTO_HIDE_TINY_CONFIDENCE_MAX;
    if (!lowConfidence && !tinyAndNotStrong) return candidate;
    return {
      ...candidate,
      status: "rejected",
      debug: {
        ...(candidate.debug ?? {}),
        autoHidden: true,
        autoHiddenReason: lowConfidence ? "low-confidence" : "tiny-candidate"
      }
    };
  });
}

function relabelWallSnapDebugLogs(
  debug: WallSnapDebug,
  before: RoomCandidate[],
  after: RoomCandidate[]
): void {
  const labels = new Map<string, string>();
  for (let index = 0; index < before.length; index += 1) {
    const oldName = before[index]?.name;
    const newName = after[index]?.name;
    if (oldName && newName && oldName !== newName) labels.set(oldName, newName);
  }
  if (!labels.size) return;
  debug.logs = debug.logs.map((log) => {
    for (const [oldName, newName] of labels) {
      if (log.startsWith(`${oldName} - `)) return `${newName}${log.slice(oldName.length)}`;
    }
    return log;
  });
}

function buildWallGrid(
  imageData: ImageData,
  debug: WallLineRoomDetectorDebug,
  textExclusionMask: Uint8Array | null
): WallGridResult {
  const mask = new Uint8Array(debug.totalCells);
  const counts = new Uint16Array(debug.totalCells);
  const sources = new Uint8Array(debug.totalCells);
  const data = imageData.data;

  for (let y = 0; y < imageData.height; y += 1) {
    for (let x = 0; x < imageData.width; x += 1) {
      const offset = (y * imageData.width + x) * 4;
      if (!isDarkWallLikePixel(data[offset], data[offset + 1], data[offset + 2], data[offset + 3])) continue;
      if (textExclusionMask?.[y * imageData.width + x]) {
        debug.textExcludedPixels += 1;
        continue;
      }
      debug.darkPixels += 1;
      const gx = Math.floor(x / CELL_SIZE);
      const gy = Math.floor(y / CELL_SIZE);
      counts[gy * debug.gridWidth + gx] += 1;
    }
  }

  for (let index = 0; index < counts.length; index += 1) {
    const gx = index % debug.gridWidth;
    const gy = Math.floor(index / debug.gridWidth);
    const isBoundary = gx === 0 || gy === 0 || gx === debug.gridWidth - 1 || gy === debug.gridHeight - 1;
    if (isBoundary || counts[index] >= debug.wallCellThreshold) {
      mask[index] = 1;
      sources[index] = isBoundary ? WALL_SOURCE_BOUNDARY : WALL_SOURCE_THRESHOLD;
      debug.wallCells += 1;
    }
  }

  return { mask, counts, sources };
}

function maskToDebugCells(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  imageWidth: number,
  imageHeight: number
): WallMaskDebugCell[] {
  const cells: WallMaskDebugCell[] = [];
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      if (!mask[y * width + x]) continue;
      const px = x * CELL_SIZE;
      const py = y * CELL_SIZE;
      cells.push({
        x: px,
        y: py,
        gridX: x,
        gridY: y,
        width: Math.max(1, Math.min(CELL_SIZE, imageWidth - px)),
        height: Math.max(1, Math.min(CELL_SIZE, imageHeight - py)),
        source: wallSourceName(sources[y * width + x]),
        reason: wallSourceReason(sources[y * width + x]),
        darkPixels: counts[y * width + x] ?? 0
      });
    }
  }
  return cells;
}

function removeIsolatedThresholdWallCells(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  debug: WallLineRoomDetectorDebug
): void {
  const removals: number[] = [];

  for (let y = 1; y < height - 1; y += 1) {
    for (let x = 1; x < width - 1; x += 1) {
      const index = y * width + x;
      if (!mask[index] || sources[index] !== WALL_SOURCE_THRESHOLD) continue;
      if (hasNonBoundaryWallNeighbor(mask, sources, width, height, x, y)) continue;
      removals.push(index);
    }
  }

  for (const index of removals) {
    debug.wallRejectedCells.push(
      createWallDebugCell(
        index,
        counts[index] ?? 0,
        "excluded-isolated-threshold",
        "어두운 픽셀 기준은 넘었지만 주변 벽 셀과 1셀도 연결되지 않아 제외됨",
        width
      )
    );
    mask[index] = 0;
    sources[index] = 0;
    debug.removedSmallWallCells += 1;
    debug.wallCells = Math.max(0, debug.wallCells - 1);
  }
  debug.removedSmallWallComponents += removals.length;
}

function removeWeaklyConnectedThresholdWallCells(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  debug: WallLineRoomDetectorDebug
): void {
  const removals: number[] = [];

  for (let y = 1; y < height - 1; y += 1) {
    for (let x = 1; x < width - 1; x += 1) {
      const index = y * width + x;
      if (!mask[index] || sources[index] !== WALL_SOURCE_THRESHOLD) continue;
      if (countCardinalWallNeighbors(mask, sources, width, height, x, y) >= 2) continue;
      removals.push(index);
    }
  }

  for (const index of removals) {
    debug.wallRejectedCells.push(
      createWallDebugCell(
        index,
        counts[index] ?? 0,
        "excluded-weak-threshold",
        "상하좌우로 맞닿은 벽 셀이 2개 미만이라 제외됨",
        width
      )
    );
    mask[index] = 0;
    sources[index] = 0;
    debug.removedSmallWallCells += 1;
    debug.wallCells = Math.max(0, debug.wallCells - 1);
  }
  debug.removedSmallWallComponents += removals.length;
}

function removeOverwideParallelThinWallComponents(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  debug: WallLineRoomDetectorDebug
): void {
  const visited = new Uint8Array(mask.length);
  const maxThicknessCells = Math.ceil(PARALLEL_THIN_WALL_MAX_BAND_THICKNESS_PX / CELL_SIZE);

  for (let y = 1; y < height - 1; y += 1) {
    for (let x = 1; x < width - 1; x += 1) {
      const startIndex = y * width + x;
      if (visited[startIndex] || sources[startIndex] !== WALL_SOURCE_PARALLEL_THIN) continue;

      const queue = [startIndex];
      const cells: number[] = [];
      let head = 0;
      let minX = x;
      let maxX = x;
      let minY = y;
      let maxY = y;
      visited[startIndex] = 1;

      while (head < queue.length) {
        const index = queue[head];
        head += 1;
        cells.push(index);
        const cx = index % width;
        const cy = Math.floor(index / width);
        if (cx < minX) minX = cx;
        if (cx > maxX) maxX = cx;
        if (cy < minY) minY = cy;
        if (cy > maxY) maxY = cy;

        const neighbors = [index - 1, index + 1, index - width, index + width];
        for (const neighbor of neighbors) {
          if (neighbor < 0 || neighbor >= sources.length || visited[neighbor]) continue;
          if (sources[neighbor] !== WALL_SOURCE_PARALLEL_THIN) continue;
          visited[neighbor] = 1;
          queue.push(neighbor);
        }
      }

      const thicknessCells = Math.min(maxX - minX + 1, maxY - minY + 1);
      if (thicknessCells <= maxThicknessCells) continue;

      for (const index of cells) {
        debug.wallRejectedCells.push(
          createWallDebugCell(
            index,
            counts[index] ?? 0,
            "excluded-overwide-parallel-thin",
            "평행 얇은 선 묶음의 두께가 샷시로 보기에는 커서 제외됨",
            width
          )
        );
        mask[index] = 0;
        sources[index] = 0;
        debug.wallCells = Math.max(0, debug.wallCells - 1);
      }
      debug.removedSmallWallComponents += 1;
      debug.removedSmallWallCells += cells.length;
    }
  }
}

function createWallDebugCell(
  index: number,
  darkPixels: number,
  source: string,
  reason: string,
  gridWidth: number
): WallMaskDebugCell {
  const gridX = index % gridWidth;
  const gridY = Math.floor(index / gridWidth);
  return {
    x: gridX * CELL_SIZE,
    y: gridY * CELL_SIZE,
    width: CELL_SIZE,
    height: CELL_SIZE,
    gridX,
    gridY,
    source,
    reason,
    darkPixels
  };
}

function hasNonBoundaryWallNeighbor(
  mask: Uint8Array,
  sources: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number
): boolean {
  for (let dy = -1; dy <= 1; dy += 1) {
    for (let dx = -1; dx <= 1; dx += 1) {
      if (dx === 0 && dy === 0) continue;
      const nx = x + dx;
      const ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
      const index = ny * width + nx;
      if (mask[index] && sources[index] !== WALL_SOURCE_BOUNDARY) return true;
    }
  }
  return false;
}

function countCardinalWallNeighbors(
  mask: Uint8Array,
  sources: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number
): number {
  let count = 0;
  const neighbors = [
    [x - 1, y],
    [x + 1, y],
    [x, y - 1],
    [x, y + 1]
  ];
  for (const [nx, ny] of neighbors) {
    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
    const index = ny * width + nx;
    if (mask[index] && sources[index] !== WALL_SOURCE_BOUNDARY) count += 1;
  }
  return count;
}

function wallSourceName(source: number): string {
  switch (source) {
    case WALL_SOURCE_BOUNDARY:
      return "boundary";
    case WALL_SOURCE_THRESHOLD:
      return "threshold";
    case WALL_SOURCE_ATTACHED_THIN:
      return "attached-thin";
    case WALL_SOURCE_PARALLEL_THIN:
      return "parallel-thin";
    default:
      return "unknown";
  }
}

function wallSourceReason(source: number): string {
  switch (source) {
    case WALL_SOURCE_BOUNDARY:
      return "분석 격자 외곽이라 벽으로 처리됨";
    case WALL_SOURCE_THRESHOLD:
      return "어두운 픽셀이 기준치 이상이라 벽으로 처리됨";
    case WALL_SOURCE_ATTACHED_THIN:
      return "양쪽 벽에 붙은 짧은 얇은 선이라 복원됨";
    case WALL_SOURCE_PARALLEL_THIN:
      return "벽에 붙은 평행 얇은 선 묶음이라 복원됨";
    default:
      return "출처를 알 수 없는 벽 셀";
  }
}

function extractWallSegments(mask: Uint8Array, width: number, height: number): FloorplanWallSegment[] {
  const segments: FloorplanWallSegment[] = [];
  let id = 1;

  for (let y = 0; y < height; y += 1) {
    let start = -1;
    for (let x = 0; x <= width; x += 1) {
      const isWall = x < width && Boolean(mask[y * width + x]);
      if (isWall && start < 0) {
        start = x;
        continue;
      }
      if (isWall || start < 0) continue;

      const end = x - 1;
      const cells = end - start + 1;
      if (cells >= WALL_SEGMENT_MIN_CELLS) {
        const yPx = (y + 0.5) * CELL_SIZE;
        segments.push({
          id: `wall_h_${id++}`,
          axis: "horizontal",
          x1: start * CELL_SIZE,
          y1: yPx,
          x2: (end + 1) * CELL_SIZE,
          y2: yPx,
          length: cells * CELL_SIZE
        });
      }
      start = -1;
    }
  }

  for (let x = 0; x < width; x += 1) {
    let start = -1;
    for (let y = 0; y <= height; y += 1) {
      const isWall = y < height && Boolean(mask[y * width + x]);
      if (isWall && start < 0) {
        start = y;
        continue;
      }
      if (isWall || start < 0) continue;

      const end = y - 1;
      const cells = end - start + 1;
      if (cells >= WALL_SEGMENT_MIN_CELLS) {
        const xPx = (x + 0.5) * CELL_SIZE;
        segments.push({
          id: `wall_v_${id++}`,
          axis: "vertical",
          x1: xPx,
          y1: start * CELL_SIZE,
          x2: xPx,
          y2: (end + 1) * CELL_SIZE,
          length: cells * CELL_SIZE
        });
      }
      start = -1;
    }
  }

  return segments.sort((a, b) => b.length - a.length).slice(0, 260);
}

function buildTextExclusionMask(imageData: ImageData, debug: WallLineRoomDetectorDebug): Uint8Array {
  const result = detectFloorplanTextBoxes(imageData);
  debug.textCandidateBoxes = result.boxes.length;
  return result.mask;
}

function promoteClosedThinWallGaps(
  imageData: ImageData,
  mask: Uint8Array,
  sources: Uint8Array,
  gridWidth: number,
  gridHeight: number,
  textExclusionMask: Uint8Array | null,
  debug: WallLineRoomDetectorDebug
): void {
  const width = imageData.width;
  const height = imageData.height;
  const total = width * height;
  const darkMask = new Uint8Array(total);
  const visited = new Uint8Array(total);
  const queue = new Int32Array(total);
  const data = imageData.data;

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const index = y * width + x;
      const offset = index * 4;
      if (textExclusionMask?.[index]) continue;
      if (isClosedThinWallBoundaryPixel(data[offset], data[offset + 1], data[offset + 2], data[offset + 3])) {
        darkMask[index] = 1;
      }
    }
  }

  for (let start = 0; start < total; start += 1) {
    if (visited[start] || darkMask[start]) continue;
    let head = 0;
    let tail = 0;
    let area = 0;
    let minX = width;
    let minY = height;
    let maxX = 0;
    let maxY = 0;
    let touchesBoundary = false;

    visited[start] = 1;
    queue[tail] = start;
    tail += 1;

    while (head < tail) {
      const index = queue[head];
      head += 1;
      const x = index % width;
      const y = Math.floor(index / width);
      area += 1;
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
      if (x === 0 || y === 0 || x === width - 1 || y === height - 1) touchesBoundary = true;

      const left = index - 1;
      const right = index + 1;
      const up = index - width;
      const down = index + width;
      if (x > 0 && !visited[left] && !darkMask[left]) {
        visited[left] = 1;
        queue[tail] = left;
        tail += 1;
      }
      if (x < width - 1 && !visited[right] && !darkMask[right]) {
        visited[right] = 1;
        queue[tail] = right;
        tail += 1;
      }
      if (y > 0 && !visited[up] && !darkMask[up]) {
        visited[up] = 1;
        queue[tail] = up;
        tail += 1;
      }
      if (y < height - 1 && !visited[down] && !darkMask[down]) {
        visited[down] = 1;
        queue[tail] = down;
        tail += 1;
      }
    }

    const boxWidth = maxX - minX + 1;
    const boxHeight = maxY - minY + 1;
    const thickness = Math.min(boxWidth, boxHeight);
    const length = Math.max(boxWidth, boxHeight);
    const aspectRatio = length / Math.max(1, thickness);
    if (
      touchesBoundary ||
      area > CLOSED_THIN_WALL_MAX_AREA_PX ||
      thickness > CLOSED_THIN_WALL_MAX_THICKNESS_PX ||
      length < CLOSED_THIN_WALL_MIN_LENGTH_PX ||
      aspectRatio < CLOSED_THIN_WALL_MIN_ASPECT_RATIO ||
      !closedThinWallEndsTouchWall(mask, sources, gridWidth, gridHeight, minX, minY, maxX, maxY) ||
      !closedThinWallTouchesThresholdNeighbor(queue, tail, width, mask, sources, gridWidth, gridHeight)
    ) {
      continue;
    }

    for (let queueIndex = 0; queueIndex < tail; queueIndex += 1) {
      const pixelIndex = queue[queueIndex];
      const x = pixelIndex % width;
      const y = Math.floor(pixelIndex / width);
      const gridX = Math.floor(x / CELL_SIZE);
      const gridY = Math.floor(y / CELL_SIZE);
      if (gridX < 0 || gridY < 0 || gridX >= gridWidth || gridY >= gridHeight) continue;
      const gridIndex = gridY * gridWidth + gridX;
      if (mask[gridIndex]) continue;
      mask[gridIndex] = 1;
      sources[gridIndex] = WALL_SOURCE_PARALLEL_THIN;
      debug.attachedThinWallCells += 1;
      debug.wallCells += 1;
    }
  }
}

function closedThinWallEndsTouchWall(
  mask: Uint8Array,
  sources: Uint8Array,
  gridWidth: number,
  gridHeight: number,
  minX: number,
  minY: number,
  maxX: number,
  maxY: number
): boolean {
  const radius = CLOSED_THIN_WALL_END_TOUCH_RADIUS_PX;
  const touchesLeftAndRight =
    wallCellExistsInPixelRect(mask, sources, gridWidth, gridHeight, minX - radius, minY - radius, minX + radius, maxY + radius) &&
    wallCellExistsInPixelRect(mask, sources, gridWidth, gridHeight, maxX - radius, minY - radius, maxX + radius, maxY + radius);
  const touchesTopAndBottom =
    wallCellExistsInPixelRect(mask, sources, gridWidth, gridHeight, minX - radius, minY - radius, maxX + radius, minY + radius) &&
    wallCellExistsInPixelRect(mask, sources, gridWidth, gridHeight, minX - radius, maxY - radius, maxX + radius, maxY + radius);
  return touchesLeftAndRight || touchesTopAndBottom;
}

function closedThinWallTouchesThresholdNeighbor(
  queue: Int32Array,
  queueLength: number,
  imageWidth: number,
  mask: Uint8Array,
  sources: Uint8Array,
  gridWidth: number,
  gridHeight: number
): boolean {
  for (let queueIndex = 0; queueIndex < queueLength; queueIndex += 1) {
    const pixelIndex = queue[queueIndex];
    const x = pixelIndex % imageWidth;
    const y = Math.floor(pixelIndex / imageWidth);
    const gridX = Math.floor(x / CELL_SIZE);
    const gridY = Math.floor(y / CELL_SIZE);
    if (gridX <= 0 || gridY <= 0 || gridX >= gridWidth - 1 || gridY >= gridHeight - 1) continue;
    const gridIndex = gridY * gridWidth + gridX;
    if (mask[gridIndex]) continue;
    const neighbors = [gridIndex - 1, gridIndex + 1, gridIndex - gridWidth, gridIndex + gridWidth];
    if (
      neighbors.some(
        (neighbor) =>
          sources[neighbor] === WALL_SOURCE_THRESHOLD ||
          sources[neighbor] === WALL_SOURCE_PARALLEL_THIN
      )
    ) {
      return true;
    }
  }
  return false;
}

function wallCellExistsInPixelRect(
  mask: Uint8Array,
  sources: Uint8Array,
  gridWidth: number,
  gridHeight: number,
  minX: number,
  minY: number,
  maxX: number,
  maxY: number
): boolean {
  const startX = Math.max(1, Math.floor(minX / CELL_SIZE));
  const endX = Math.min(gridWidth - 2, Math.floor(maxX / CELL_SIZE));
  const startY = Math.max(1, Math.floor(minY / CELL_SIZE));
  const endY = Math.min(gridHeight - 2, Math.floor(maxY / CELL_SIZE));
  for (let y = startY; y <= endY; y += 1) {
    for (let x = startX; x <= endX; x += 1) {
      const index = y * gridWidth + x;
      if (mask[index] && sources[index] !== WALL_SOURCE_BOUNDARY) return true;
    }
  }
  return false;
}

function promoteParallelThinWalls(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  debug: WallLineRoomDetectorDebug
): void {
  const additions = new Uint8Array(mask.length);
  const horizontalSegments = extractThinWallSegments(mask, counts, width, height, true);
  const verticalSegments = extractThinWallSegments(mask, counts, width, height, false);

  promoteParallelThinWallsOnAxis(horizontalSegments, mask, additions, width, height, true, debug);
  promoteParallelThinWallsOnAxis(verticalSegments, mask, additions, width, height, false, debug);

  for (let index = 0; index < additions.length; index += 1) {
    if (!additions[index] || mask[index]) continue;
    mask[index] = 1;
    sources[index] = WALL_SOURCE_PARALLEL_THIN;
    debug.attachedThinWallCells += 1;
    debug.wallCells += 1;
  }
}

function extractThinWallSegments(
  mask: Uint8Array,
  counts: Uint16Array,
  width: number,
  height: number,
  horizontal: boolean
): ThinWallSegment[] {
  const segments: ThinWallSegment[] = [];
  const outer = horizontal ? height : width;
  const inner = horizontal ? width : height;

  for (let line = 0; line < outer; line += 1) {
    let start = -1;
    for (let position = 0; position <= inner; position += 1) {
      const inside = position < inner;
      const x = horizontal ? position : line;
      const y = horizontal ? line : position;
      const index = inside ? y * width + x : -1;
      const isThin = inside && !mask[index] && counts[index] >= DEFAULT_WALL_CELL_THRESHOLD;

      if (isThin && start < 0) {
        start = position;
        continue;
      }
      if (isThin || start < 0) continue;

      const end = position - 1;
      const length = end - start + 1;
      if (length >= PARALLEL_THIN_WALL_MIN_CELLS) {
        segments.push({
          axis: horizontal ? "horizontal" : "vertical",
          line,
          start,
          end,
          length
        });
      }
      start = -1;
    }
  }

  return segments;
}

function promoteParallelThinWallsOnAxis(
  segments: ThinWallSegment[],
  mask: Uint8Array,
  additions: Uint8Array,
  width: number,
  height: number,
  horizontal: boolean,
  debug: WallLineRoomDetectorDebug
): void {
  for (let index = 0; index < segments.length; index += 1) {
    const segment = segments[index];
    for (let otherIndex = index + 1; otherIndex < segments.length; otherIndex += 1) {
      const other = segments[otherIndex];
      const gap = Math.abs(other.line - segment.line);
      if (gap < PARALLEL_THIN_WALL_MIN_GAP_CELLS || gap > PARALLEL_THIN_WALL_MAX_GAP_CELLS) continue;
      const overlap = getSegmentOverlap(segment, other);
      if (overlap.ratio < PARALLEL_THIN_WALL_MIN_OVERLAP_RATIO) continue;
      if (isRepeatingGridPattern(segment, segments, other.line)) continue;
      if (isRepeatingGridPattern(other, segments, segment.line)) continue;
      const band = collectParallelThinWallBand(segment, other, overlap, segments);
      if (parallelThinWallBandThicknessPx(band) > PARALLEL_THIN_WALL_MAX_BAND_THICKNESS_PX) continue;
      if (!parallelThinWallBandTouchesWall(band, mask, width, height, horizontal)) {
        continue;
      }
      fillParallelThinWallBand(band, additions, width, height, horizontal, debug);
    }
  }
}

function collectParallelThinWallBand(
  a: ThinWallSegment,
  b: ThinWallSegment,
  overlap: { start: number; end: number; ratio: number },
  segments: ThinWallSegment[]
): ParallelThinWallBand {
  const group = [a, b];
  let start = overlap.start;
  let end = overlap.end;
  const minLine = Math.min(a.line, b.line);
  const maxLine = Math.max(a.line, b.line);

  for (const segment of segments) {
    if (segment === a || segment === b) continue;
    if (segment.line < minLine - PARALLEL_THIN_WALL_MAX_GAP_CELLS) continue;
    if (segment.line > maxLine + PARALLEL_THIN_WALL_MAX_GAP_CELLS) continue;
    const segmentOverlap = getRangeOverlap(start, end, segment.start, segment.end);
    if (segmentOverlap.ratio < PARALLEL_THIN_WALL_MIN_OVERLAP_RATIO) continue;
    if (isRepeatingGridPattern(segment, segments, a.line)) continue;
    group.push(segment);
    start = segmentOverlap.start;
    end = segmentOverlap.end;
    if (group.length >= 3) break;
  }

  return {
    segments: group.sort((left, right) => left.line - right.line),
    start,
    end
  };
}

function parallelThinWallBandThicknessPx(band: ParallelThinWallBand): number {
  const minLine = band.segments[0].line;
  const maxLine = band.segments[band.segments.length - 1].line;
  return (maxLine - minLine + 1) * CELL_SIZE;
}

function parallelThinWallBandTouchesWall(
  band: ParallelThinWallBand,
  mask: Uint8Array,
  width: number,
  height: number,
  horizontal: boolean
): boolean {
  const minLine = band.segments[0].line;
  const maxLine = band.segments[band.segments.length - 1].line;
  return (
    parallelThinWallEndTouchesWall(band.start, minLine, maxLine, mask, width, height, horizontal) &&
    parallelThinWallEndTouchesWall(band.end, minLine, maxLine, mask, width, height, horizontal)
  );
}

function parallelThinWallEndTouchesWall(
  position: number,
  minLine: number,
  maxLine: number,
  mask: Uint8Array,
  width: number,
  height: number,
  horizontal: boolean
): boolean {
  const radius = PARALLEL_THIN_WALL_ATTACH_RADIUS_CELLS;
  const minOuter = minLine - radius;
  const maxOuter = maxLine + radius;
  const minInner = position - radius;
  const maxInner = position + radius;

  for (let outer = minOuter; outer <= maxOuter; outer += 1) {
    for (let inner = minInner; inner <= maxInner; inner += 1) {
      const x = horizontal ? inner : outer;
      const y = horizontal ? outer : inner;
      if (x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1) continue;
      if (mask[y * width + x]) return true;
    }
  }
  return false;
}

function getSegmentOverlap(a: ThinWallSegment, b: ThinWallSegment): { start: number; end: number; ratio: number } {
  return getRangeOverlap(a.start, a.end, b.start, b.end, Math.min(a.length, b.length));
}

function getRangeOverlap(
  aStart: number,
  aEnd: number,
  bStart: number,
  bEnd: number,
  baseLength = Math.min(aEnd - aStart + 1, bEnd - bStart + 1)
): { start: number; end: number; ratio: number } {
  const start = Math.max(aStart, bStart);
  const end = Math.min(aEnd, bEnd);
  const overlap = Math.max(0, end - start + 1);
  return {
    start,
    end,
    ratio: overlap / Math.max(1, baseLength)
  };
}

function isRepeatingGridPattern(segment: ThinWallSegment, segments: ThinWallSegment[], pairedLine: number): boolean {
  let repeats = 0;
  for (const other of segments) {
    if (other === segment) continue;
    if (Math.abs(other.line - segment.line) > PARALLEL_THIN_WALL_MAX_GAP_CELLS * 3) continue;
    if (other.line === pairedLine) {
      repeats += 1;
      continue;
    }
    if (getSegmentOverlap(segment, other).ratio >= 0.45) repeats += 1;
  }
  return repeats > PARALLEL_THIN_WALL_MAX_REPEATS;
}

function fillParallelThinWallBand(
  band: ParallelThinWallBand,
  additions: Uint8Array,
  width: number,
  height: number,
  horizontal: boolean,
  debug: WallLineRoomDetectorDebug
): void {
  const minLine = band.segments[0].line;
  const maxLine = band.segments[band.segments.length - 1].line;
  let added = 0;

  for (let line = minLine; line <= maxLine; line += 1) {
    for (let position = band.start; position <= band.end; position += 1) {
      const x = horizontal ? position : line;
      const y = horizontal ? line : position;
      if (x < 0 || y < 0 || x >= width || y >= height) continue;
      const index = y * width + x;
      if (!additions[index]) added += 1;
      additions[index] = 1;
    }
  }

  debug.gapClosedCells += added;
}

function restoreAttachedThinWalls(
  mask: Uint8Array,
  counts: Uint16Array,
  sources: Uint8Array,
  width: number,
  height: number,
  minThinCount: number,
  maxRunCells: number,
  debug: WallLineRoomDetectorDebug
): void {
  const additions = new Uint8Array(mask.length);
  restoreAttachedThinWallsOnAxis(mask, counts, additions, width, height, minThinCount, maxRunCells, true);
  restoreAttachedThinWallsOnAxis(mask, counts, additions, width, height, minThinCount, maxRunCells, false);

  for (let index = 0; index < additions.length; index += 1) {
    if (!additions[index] || mask[index]) continue;
    mask[index] = 1;
    sources[index] = WALL_SOURCE_ATTACHED_THIN;
    debug.attachedThinWallCells += 1;
    debug.wallCells += 1;
  }
}

function restoreAttachedThinWallsOnAxis(
  mask: Uint8Array,
  counts: Uint16Array,
  additions: Uint8Array,
  width: number,
  height: number,
  minThinCount: number,
  maxRunCells: number,
  horizontal: boolean
): void {
  const outer = horizontal ? height : width;
  const inner = horizontal ? width : height;

  for (let outerIndex = 0; outerIndex < outer; outerIndex += 1) {
    let start = -1;
    for (let innerIndex = 0; innerIndex <= inner; innerIndex += 1) {
      const inside = innerIndex < inner;
      const x = horizontal ? innerIndex : outerIndex;
      const y = horizontal ? outerIndex : innerIndex;
      const index = inside ? y * width + x : -1;
      const isThin = inside && !mask[index] && counts[index] >= minThinCount;

      if (isThin && start < 0) {
        start = innerIndex;
        continue;
      }
      if (isThin) continue;
      if (start < 0) continue;

      const end = innerIndex - 1;
      const runLength = end - start + 1;
      const before = start - 1;
      const after = innerIndex;
      const hasWallBefore = before >= 0 && mask[(horizontal ? outerIndex : before) * width + (horizontal ? before : outerIndex)];
      const hasWallAfter = after < inner && mask[(horizontal ? outerIndex : after) * width + (horizontal ? after : outerIndex)];

      if (runLength <= maxRunCells && hasWallBefore && hasWallAfter) {
        for (let fillIndex = start; fillIndex <= end; fillIndex += 1) {
          const fillX = horizontal ? fillIndex : outerIndex;
          const fillY = horizontal ? outerIndex : fillIndex;
          additions[fillY * width + fillX] = 1;
        }
      }

      start = -1;
    }
  }
}

function isDarkWallLikePixel(r: number, g: number, b: number, alpha: number): boolean {
  if (alpha < 32) return false;
  const gray = (r * 77 + g * 150 + b * 29) >> 8;
  return gray < DARK_THRESHOLD;
}

function isClosedThinWallBoundaryPixel(r: number, g: number, b: number, alpha: number): boolean {
  if (alpha < 32) return false;
  const gray = (r * 77 + g * 150 + b * 29) >> 8;
  return gray < CLOSED_THIN_WALL_DARK_THRESHOLD;
}

function expandMask(mask: Uint8Array, width: number, height: number, radius: number): Uint8Array {
  const expanded = new Uint8Array(mask.length);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      if (!mask[y * width + x]) continue;
      for (let dy = -radius; dy <= radius; dy += 1) {
        for (let dx = -radius; dx <= radius; dx += 1) {
          const nx = x + dx;
          const ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
          expanded[ny * width + nx] = 1;
        }
      }
    }
  }
  return expanded;
}

function invertMask(mask: Uint8Array): Uint8Array {
  const result = new Uint8Array(mask.length);
  for (let index = 0; index < mask.length; index += 1) {
    result[index] = mask[index] ? 0 : 1;
  }
  return result;
}

function findExternalFreeMask(freeMask: Uint8Array, width: number, height: number): Uint8Array {
  const external = new Uint8Array(freeMask.length);
  const queue: number[] = [];
  let head = 0;

  for (let x = 1; x < Math.max(1, width - 1); x += 1) {
    seedExternalCell(queue, external, freeMask, width, height, x, 1);
    seedExternalCell(queue, external, freeMask, width, height, x, height - 2);
  }
  for (let y = 1; y < Math.max(1, height - 1); y += 1) {
    seedExternalCell(queue, external, freeMask, width, height, 1, y);
    seedExternalCell(queue, external, freeMask, width, height, width - 2, y);
  }

  while (head < queue.length) {
    const current = queue[head];
    head += 1;
    const x = current % width;
    const y = Math.floor(current / width);
    seedExternalCell(queue, external, freeMask, width, height, x - 1, y);
    seedExternalCell(queue, external, freeMask, width, height, x + 1, y);
    seedExternalCell(queue, external, freeMask, width, height, x, y - 1);
    seedExternalCell(queue, external, freeMask, width, height, x, y + 1);
  }

  return external;
}

function seedExternalCell(
  queue: number[],
  external: Uint8Array,
  freeMask: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number
): void {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  const index = y * width + x;
  if (!freeMask[index] || external[index]) return;
  external[index] = 1;
  queue.push(index);
}

function getExternalComponentRatio(
  component: GridComponent,
  externalFreeMask: Uint8Array,
  debug: WallLineRoomDetectorDebug
): number {
  let externalCells = 0;
  for (const { x, y } of component.cellList) {
    if (externalFreeMask[y * debug.gridWidth + x]) externalCells += 1;
  }
  return externalCells / Math.max(1, component.cells);
}

function collectComponents(mask: Uint8Array, width: number, height: number): GridComponent[] {
  const visited = new Uint8Array(mask.length);
  const components: GridComponent[] = [];
  const queue: number[] = [];
  let head = 0;

  for (let index = 0; index < mask.length; index += 1) {
    if (!mask[index] || visited[index]) continue;
    const startX = index % width;
    const startY = Math.floor(index / width);
    const component: GridComponent = {
      cells: 0,
      cellList: [],
      cellSet: new Set(),
      minX: startX,
      minY: startY,
      maxX: startX,
      maxY: startY
    };

    visited[index] = 1;
    queue.length = 0;
    queue.push(index);
    head = 0;

    while (head < queue.length) {
      const current = queue[head];
      head += 1;
      const x = current % width;
      const y = Math.floor(current / width);
      component.cells += 1;
      component.cellList.push({ x, y });
      component.cellSet.add(cellKey(x, y));
      component.minX = Math.min(component.minX, x);
      component.minY = Math.min(component.minY, y);
      component.maxX = Math.max(component.maxX, x);
      component.maxY = Math.max(component.maxY, y);

      pushNeighbor(queue, mask, visited, width, height, x - 1, y);
      pushNeighbor(queue, mask, visited, width, height, x + 1, y);
      pushNeighbor(queue, mask, visited, width, height, x, y - 1);
      pushNeighbor(queue, mask, visited, width, height, x, y + 1);
    }

    components.push(component);
  }

  return components;
}

function pushNeighbor(
  queue: number[],
  mask: Uint8Array,
  visited: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number
): void {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  const index = y * width + x;
  if (!mask[index] || visited[index]) return;
  visited[index] = 1;
  queue.push(index);
}

function componentToCandidate(
  component: GridComponent,
  index: number,
  imageArea: number,
  imageData: ImageData,
  debug: WallLineRoomDetectorDebug,
  applyCandidateFilters: boolean,
  wallGuard: PolygonWallGuard
): RoomCandidate | null {
  const polygon = componentToPolygon(component, imageData, wallGuard);
  debug.polygonRawPoints += polygon.rawPoints;
  debug.polygonSimplifiedPoints += polygon.simplifiedPoints;
  debug.polygonFinalPoints += polygon.finalPoints;
  if (polygon.closedLoop) {
    debug.polygonClosedLoops += 1;
  } else {
    debug.polygonOpenLoops += 1;
  }

  const points = polygon.points;
  const bounds = points.length >= 3 ? getPointBounds(points) : componentBounds(component);
  const area = bounds.width * bounds.height;
  const areaRatio = area / imageArea;
  const aspect = Math.max(bounds.width, bounds.height) / Math.max(1, Math.min(bounds.width, bounds.height));
  const fillRatio = component.cells / Math.max(1, ((component.maxX - component.minX + 1) * (component.maxY - component.minY + 1)));

  if (bounds.width < 32 || bounds.height < 32) {
    debug.rejectedSmall += 1;
    if (applyCandidateFilters) return null;
  }
  if (areaRatio < 0.0012) {
    debug.rejectedTooSmallArea += 1;
    if (applyCandidateFilters) return null;
  }
  if (areaRatio > 0.78) {
    debug.rejectedTooLargeArea += 1;
    if (applyCandidateFilters) return null;
  }
  if (fillRatio < 0.2) {
    debug.rejectedSparse += 1;
    if (applyCandidateFilters) return null;
  }
  if (aspect > 9) {
    debug.rejectedThin += 1;
    if (applyCandidateFilters) return null;
  }

  const candidate: RoomCandidate = {
    id: `wall_room_${index + 1}`,
    name: `방 후보 ${index + 1}`,
    kind: "unknown",
    confidence: Math.round(Math.min(94, 40 + fillRatio * 38 + Math.min(areaRatio, 0.25) * 64)),
    status: "candidate",
    shape: points.length >= 3 ? "polygon" : "rect",
    rect: bounds,
    debug: {
      rawPoints: polygon.rawPoints,
      simplifiedPoints: polygon.simplifiedPoints,
      finalPoints: polygon.finalPoints,
      closedLoop: polygon.closedLoop
    },
    ...(points.length >= 3 ? { points } : {})
  };
  const cleanedCandidate = cleanupOrthogonalRoomCandidate(candidate).candidate;
  return refineCandidateWithDetectedWalls(removeCrowdedCandidatePoints(cleanedCandidate), wallGuard, imageData, debug.wallSnap);
}

function componentToDebugCandidate(
  component: GridComponent,
  index: number,
  imageData: ImageData,
  externalRatio: number
): RoomCandidate | null {
  const polygon = componentToPolygon(component, imageData);
  const points = polygon.points;
  const bounds = points.length >= 3 ? getPointBounds(points) : componentBounds(component);
  if (bounds.width <= 0 || bounds.height <= 0) return null;

  return {
    id: `wall_external_${index + 1}`,
    name: `외부 제외 ${index + 1}`,
    kind: "unknown",
    confidence: Math.round(externalRatio * 100),
    status: "rejected",
    shape: points.length >= 3 ? "polygon" : "rect",
    rect: bounds,
    debug: {
      rawPoints: polygon.rawPoints,
      simplifiedPoints: polygon.simplifiedPoints,
      finalPoints: polygon.finalPoints,
      closedLoop: polygon.closedLoop,
      reason: "external",
      externalRatio
    },
    ...(points.length >= 3 ? { points } : {})
  };
}

function componentBounds(component: GridComponent): RoomCandidate["rect"] {
  return {
    x: component.minX * CELL_SIZE,
    y: component.minY * CELL_SIZE,
    width: (component.maxX - component.minX + 1) * CELL_SIZE,
    height: (component.maxY - component.minY + 1) * CELL_SIZE
  };
}

function componentToPolygon(
  component: GridComponent,
  imageData: ImageData,
  wallGuard?: PolygonWallGuard
): PolygonTraceResult {
  const edges = new Map<string, Array<[number, number]>>();

  for (const { x, y } of component.cellList) {
    addBoundaryEdgeIfNeeded(edges, component.cellSet, x, y, 0, -1, [x, y], [x + 1, y]);
    addBoundaryEdgeIfNeeded(edges, component.cellSet, x, y, 1, 0, [x + 1, y], [x + 1, y + 1]);
    addBoundaryEdgeIfNeeded(edges, component.cellSet, x, y, 0, 1, [x + 1, y + 1], [x, y + 1]);
    addBoundaryEdgeIfNeeded(edges, component.cellSet, x, y, -1, 0, [x, y + 1], [x, y]);
  }

  const loopResult = traceLongestLoop(edges);
  const loop = loopResult.points;
  const simplified = simplifyGridPolygon(loop);
  const normalized = normalizeNoisyPolygonPoints(simplified);
  const curveCleaned = removeCurveLikePolygonNoiseOutward(normalized, wallGuard);
  const axisSnapped = snapNearbyPolygonAxesOutward(curveCleaned, wallGuard);
  const orthogonalized = orthogonalizeShortDiagonalEdgesOutward(axisSnapped, wallGuard);
  const cleaned = simplifyGridPolygon(removeConsecutiveDuplicatePoints(orthogonalized));
  const limited = limitPolygonPoints(cleaned, MAX_POLYGON_POINTS);
  return {
    points: limited.map(([x, y]) => [
      clamp(x * CELL_SIZE, 0, imageData.width),
      clamp(y * CELL_SIZE, 0, imageData.height)
    ]),
    rawPoints: loop.length,
    simplifiedPoints: simplified.length,
    finalPoints: limited.length,
    closedLoop: loopResult.closed
  };
}

function addBoundaryEdgeIfNeeded(
  edges: Map<string, Array<[number, number]>>,
  cellSet: Set<string>,
  x: number,
  y: number,
  neighborDx: number,
  neighborDy: number,
  start: [number, number],
  end: [number, number]
): void {
  if (cellSet.has(cellKey(x + neighborDx, y + neighborDy))) return;
  const key = pointKey(start);
  const list = edges.get(key) ?? [];
  list.push(end);
  edges.set(key, list);
}

function traceLongestLoop(edges: Map<string, Array<[number, number]>>): { points: Array<[number, number]>; closed: boolean } {
  const used = new Set<string>();
  const loops: Array<{ points: Array<[number, number]>; closed: boolean }> = [];

  for (const [startKey, targets] of edges) {
    const start = parsePointKey(startKey);
    for (const target of targets) {
      const firstEdgeKey = edgeKey(start, target);
      if (used.has(firstEdgeKey)) continue;
      const loop: Array<[number, number]> = [start];
      let current = target;
      used.add(firstEdgeKey);

      for (let guard = 0; guard < edges.size + 8; guard += 1) {
        loop.push(current);
        if (pointKey(current) === startKey) break;
        const next = (edges.get(pointKey(current)) ?? []).find((candidate) => !used.has(edgeKey(current, candidate)));
        if (!next) break;
        used.add(edgeKey(current, next));
        current = next;
      }

      if (loop.length >= 4) {
        loops.push({
          points: loop,
          closed: samePoint(loop[0], loop[loop.length - 1])
        });
      }
    }
  }

  return loops.sort((a, b) => Math.abs(polygonArea(b.points)) - Math.abs(polygonArea(a.points)))[0] ?? { points: [], closed: false };
}

function simplifyGridPolygon(points: Array<[number, number]>): Array<[number, number]> {
  const unique = points.length > 1 && samePoint(points[0], points[points.length - 1]) ? points.slice(0, -1) : points.slice();
  let changed = true;
  let result = unique;

  while (changed && result.length > 3) {
    changed = false;
    const next: Array<[number, number]> = [];
    for (let index = 0; index < result.length; index += 1) {
      const previous = result[(index - 1 + result.length) % result.length];
      const current = result[index];
      const following = result[(index + 1) % result.length];
      if (isCollinear(previous, current, following)) {
        changed = true;
        continue;
      }
      next.push(current);
    }
    result = next;
  }

  return result;
}

function limitPolygonPoints(points: Array<[number, number]>, maxPoints: number): Array<[number, number]> {
  if (points.length <= maxPoints) return points;
  const step = points.length / maxPoints;
  const result: Array<[number, number]> = [];
  for (let index = 0; index < maxPoints; index += 1) {
    result.push(points[Math.floor(index * step)]);
  }
  return simplifyGridPolygon(result);
}

function normalizeNoisyPolygonPoints(points: Array<[number, number]>): Array<[number, number]> {
  if (points.length < POLYGON_POINT_CLUSTER_MIN_POINTS) return points;

  const result: Array<[number, number]> = [];
  let index = 0;
  while (index < points.length) {
    const run: Array<[number, number]> = [points[index]];
    let nextIndex = index + 1;
    while (
      nextIndex < points.length &&
      isNearPoint(points[nextIndex], run[0], POLYGON_POINT_CLUSTER_RADIUS_CELLS)
    ) {
      run.push(points[nextIndex]);
      nextIndex += 1;
    }

    if (run.length >= POLYGON_POINT_CLUSTER_MIN_POINTS) {
      result.push(getClusterRepresentativePoint(run));
    } else {
      result.push(...run);
    }
    index = nextIndex;
  }

  return simplifyGridPolygon(removeConsecutiveDuplicatePoints(result));
}

function removeCurveLikePolygonNoiseOutward(
  points: Array<[number, number]>,
  wallGuard?: PolygonWallGuard
): Array<[number, number]> {
  if (points.length < 4) return points;

  let result = points.slice();
  for (let pass = 0; pass < POLYGON_CURVE_NOISE_MAX_PASSES; pass += 1) {
    let changed = false;
    for (let index = 0; index < result.length && result.length >= 4; index += 1) {
      const previous = result[(index - 1 + result.length) % result.length];
      const current = result[index];
      const next = result[(index + 1) % result.length];
      const prevLength = pointDistance(previous, current);
      const nextLength = pointDistance(current, next);
      if (prevLength > POLYGON_CURVE_NOISE_EDGE_MAX_CELLS || nextLength > POLYGON_CURVE_NOISE_EDGE_MAX_CELLS) {
        continue;
      }
      if (isCollinear(previous, current, next)) continue;

      const candidate = result.filter((_, candidateIndex) => candidateIndex !== index);
      if (Math.abs(polygonArea(candidate)) < Math.abs(polygonArea(result))) continue;
      if (!isWallSafePolygonExpansion(result, candidate, wallGuard)) continue;

      result = candidate;
      changed = true;
      index -= 1;
    }
    if (!changed) break;
  }

  return simplifyGridPolygon(removeConsecutiveDuplicatePoints(result));
}

function pointDistance(a: [number, number], b: [number, number]): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1]);
}

function snapNearbyPolygonAxesOutward(
  points: Array<[number, number]>,
  wallGuard?: PolygonWallGuard
): Array<[number, number]> {
  if (points.length < 3) return points;
  const snappedX = snapPolygonAxisOutward(points, 0, POLYGON_AXIS_SNAP_RADIUS_CELLS, wallGuard);
  const snappedY = snapPolygonAxisOutward(snappedX, 1, POLYGON_AXIS_SNAP_RADIUS_CELLS, wallGuard);
  return simplifyGridPolygon(removeConsecutiveDuplicatePoints(snappedY));
}

function snapPolygonAxisOutward(
  points: Array<[number, number]>,
  axisIndex: 0 | 1,
  radius: number,
  wallGuard?: PolygonWallGuard
): Array<[number, number]> {
  if (radius <= 0 || points.length < 3) return points;

  let result = points.slice();
  const sorted = points
    .map((point, index) => ({ index, value: point[axisIndex] }))
    .sort((a, b) => a.value - b.value);

  let start = 0;
  while (start < sorted.length) {
    let end = start + 1;
    while (end < sorted.length && sorted[end].value - sorted[end - 1].value <= radius) {
      end += 1;
    }

    const cluster = sorted.slice(start, end);
    if (cluster.length >= 2) {
      const values = cluster.map(({ value }) => value);
        const minValue = Math.min(...values);
        const maxValue = Math.max(...values);
        if (minValue !== maxValue) {
          const minCandidate = replaceAxisValues(result, cluster.map(({ index }) => index), axisIndex, minValue);
          const maxCandidate = replaceAxisValues(result, cluster.map(({ index }) => index), axisIndex, maxValue);
          result = chooseLargerPolygon(result, minCandidate, maxCandidate, wallGuard);
        }
      }

    start = end;
  }

  return result;
}

function replaceAxisValues(
  points: Array<[number, number]>,
  targetIndexes: number[],
  axisIndex: 0 | 1,
  value: number
): Array<[number, number]> {
  const targetSet = new Set(targetIndexes);
  return points.map(([x, y], index) => {
    if (!targetSet.has(index)) return [x, y];
    return axisIndex === 0 ? [value, y] : [x, value];
  });
}

function orthogonalizeShortDiagonalEdgesOutward(
  points: Array<[number, number]>,
  wallGuard?: PolygonWallGuard
): Array<[number, number]> {
  if (points.length < 3) return points;

  const result: Array<[number, number]> = [];
  for (let index = 0; index < points.length; index += 1) {
    const current = points[index];
    const next = points[(index + 1) % points.length];
    result.push(current);

    const dx = Math.abs(next[0] - current[0]);
    const dy = Math.abs(next[1] - current[1]);
    if (dx === 0 || dy === 0) continue;
    if (Math.min(dx, dy) > POLYGON_SHORT_DIAGONAL_MAX_CELLS) continue;

    const cornerA: [number, number] = [next[0], current[1]];
    const cornerB: [number, number] = [current[0], next[1]];
    const currentPolygon = [...result, ...points.slice(index + 1)];
    const corner = chooseLargerOrthogonalCorner(currentPolygon, result, points, index, cornerA, cornerB, wallGuard);
    if (!corner) continue;
    if (!samePoint(corner, current) && !samePoint(corner, next)) {
      result.push(corner);
    }
  }

  return result;
}

function chooseLargerOrthogonalCorner(
  currentPolygon: Array<[number, number]>,
  prefix: Array<[number, number]>,
  original: Array<[number, number]>,
  index: number,
  cornerA: [number, number],
  cornerB: [number, number],
  wallGuard?: PolygonWallGuard
): [number, number] | null {
  const candidateA = insertPointAfter(prefix, original, index, cornerA);
  const candidateB = insertPointAfter(prefix, original, index, cornerB);
  const currentArea = Math.abs(polygonArea(currentPolygon));
  const areaA = Math.abs(polygonArea(candidateA));
  const areaB = Math.abs(polygonArea(candidateB));
  const bestArea = Math.max(areaA, areaB);
  if (bestArea < currentArea) return null;
  const safeA = areaA >= currentArea && isWallSafePolygonExpansion(currentPolygon, candidateA, wallGuard);
  const safeB = areaB >= currentArea && isWallSafePolygonExpansion(currentPolygon, candidateB, wallGuard);
  if (!safeA && !safeB) return null;
  if (safeA && !safeB) return cornerA;
  if (safeB && !safeA) return cornerB;
  return areaA >= areaB ? cornerA : cornerB;
}

function insertPointAfter(
  prefix: Array<[number, number]>,
  original: Array<[number, number]>,
  index: number,
  point: [number, number]
): Array<[number, number]> {
  return [...prefix, point, ...original.slice(index + 1)];
}

function chooseLargerPolygon(
  current: Array<[number, number]>,
  candidateA: Array<[number, number]>,
  candidateB: Array<[number, number]>,
  wallGuard?: PolygonWallGuard
): Array<[number, number]> {
  const currentArea = Math.abs(polygonArea(current));
  const areaA = Math.abs(polygonArea(candidateA));
  const areaB = Math.abs(polygonArea(candidateB));
  const safeA = areaA >= currentArea && isWallSafePolygonExpansion(current, candidateA, wallGuard);
  const safeB = areaB >= currentArea && isWallSafePolygonExpansion(current, candidateB, wallGuard);
  if (!safeA && !safeB) return current;
  if (safeA && !safeB) return candidateA;
  if (safeB && !safeA) return candidateB;
  return areaA >= areaB ? candidateA : candidateB;
}

function isWallSafePolygonExpansion(
  current: Array<[number, number]>,
  candidate: Array<[number, number]>,
  wallGuard?: PolygonWallGuard
): boolean {
  if (!wallGuard || current.length < 3 || candidate.length < 3) return true;
  return countWallCellsInsidePolygon(candidate, wallGuard) <= countWallCellsInsidePolygon(current, wallGuard);
}

function countWallCellsInsidePolygon(points: Array<[number, number]>, wallGuard: PolygonWallGuard): number {
  const bounds = getGridPointBounds(points, wallGuard.width, wallGuard.height);
  let count = 0;
  for (let y = bounds.minY; y <= bounds.maxY; y += 1) {
    for (let x = bounds.minX; x <= bounds.maxX; x += 1) {
      const index = y * wallGuard.width + x;
      if (!wallGuard.mask[index]) continue;
      if (isPointInsidePolygon(x + 0.5, y + 0.5, points)) count += 1;
    }
  }
  return count;
}

function getGridPointBounds(
  points: Array<[number, number]>,
  width: number,
  height: number
): { minX: number; minY: number; maxX: number; maxY: number } {
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  return {
    minX: clamp(Math.floor(Math.min(...xs)), 0, width - 1),
    minY: clamp(Math.floor(Math.min(...ys)), 0, height - 1),
    maxX: clamp(Math.ceil(Math.max(...xs)) - 1, 0, width - 1),
    maxY: clamp(Math.ceil(Math.max(...ys)) - 1, 0, height - 1)
  };
}

function isPointInsidePolygon(x: number, y: number, points: Array<[number, number]>): boolean {
  let inside = false;
  for (let index = 0, previousIndex = points.length - 1; index < points.length; previousIndex = index++) {
    const [x1, y1] = points[index];
    const [x2, y2] = points[previousIndex];
    const intersects = y1 > y !== y2 > y && x < ((x2 - x1) * (y - y1)) / Math.max(0.0001, y2 - y1) + x1;
    if (intersects) inside = !inside;
  }
  return inside;
}

function isNearPoint(a: [number, number], b: [number, number], radius: number): boolean {
  const dx = a[0] - b[0];
  const dy = a[1] - b[1];
  return dx * dx + dy * dy <= radius * radius;
}

function getClusterRepresentativePoint(points: Array<[number, number]>): [number, number] {
  const centerX = points.reduce((sum, [x]) => sum + x, 0) / points.length;
  const centerY = points.reduce((sum, [, y]) => sum + y, 0) / points.length;
  return points.reduce((best, point) => {
    const bestDistance = squaredDistance(best, [centerX, centerY]);
    const pointDistance = squaredDistance(point, [centerX, centerY]);
    return pointDistance < bestDistance ? point : best;
  }, points[0]);
}

function squaredDistance(point: [number, number], center: [number, number]): number {
  const dx = point[0] - center[0];
  const dy = point[1] - center[1];
  return dx * dx + dy * dy;
}

function removeConsecutiveDuplicatePoints(points: Array<[number, number]>): Array<[number, number]> {
  return points.filter((point, index) => index === 0 || !samePoint(point, points[index - 1]));
}

function getPointBounds(points: Array<[number, number]>): RoomCandidate["rect"] {
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  return {
    x: minX,
    y: minY,
    width: maxX - minX,
    height: maxY - minY
  };
}

function polygonArea(points: Array<[number, number]>): number {
  let area = 0;
  for (let index = 0; index < points.length; index += 1) {
    const [x1, y1] = points[index];
    const [x2, y2] = points[(index + 1) % points.length];
    area += x1 * y2 - x2 * y1;
  }
  return area / 2;
}

function isCollinear(a: [number, number], b: [number, number], c: [number, number]): boolean {
  return (a[0] === b[0] && b[0] === c[0]) || (a[1] === b[1] && b[1] === c[1]);
}

function samePoint(a: [number, number], b: [number, number]): boolean {
  return a[0] === b[0] && a[1] === b[1];
}

function samePoints(a: Array<[number, number]>, b: Array<[number, number]>): boolean {
  return a.length === b.length && a.every(([x, y], index) => x === b[index][0] && y === b[index][1]);
}

function cellKey(x: number, y: number): string {
  return `${x},${y}`;
}

function pointKey(point: [number, number]): string {
  return `${point[0]},${point[1]}`;
}

function parsePointKey(key: string): [number, number] {
  const [x, y] = key.split(",").map(Number);
  return [x, y];
}

function edgeKey(start: [number, number], end: [number, number]): string {
  return `${pointKey(start)}>${pointKey(end)}`;
}

function removeCrowdedCandidatePoints(candidate: RoomCandidate): RoomCandidate {
  if (candidate.shape !== "polygon" || !candidate.points || candidate.points.length <= 4) return candidate;

  const points = candidate.points.map(([x, y]) => [x, y] as [number, number]);
  const center = getPointCenter(points);
  const removeIndexes = new Set<number>();

  for (let index = 0; index < points.length; index += 1) {
    if (removeIndexes.has(index)) continue;
    const clusterIndexes = points
      .map((point, pointIndex) => ({ point, pointIndex }))
      .filter(({ point, pointIndex }) => !removeIndexes.has(pointIndex) && pointDistance(point, points[index]) <= POLYGON_CROWDED_POINT_RADIUS_PX)
      .map(({ pointIndex }) => pointIndex);

    if (clusterIndexes.length < POLYGON_CROWDED_POINT_MIN_COUNT) continue;

    const keepIndex = clusterIndexes.reduce((bestIndex, pointIndex) => {
      return pointDistance(points[pointIndex], center) < pointDistance(points[bestIndex], center) ? pointIndex : bestIndex;
    }, clusterIndexes[0]);

    for (const pointIndex of clusterIndexes) {
      if (pointIndex !== keepIndex) removeIndexes.add(pointIndex);
    }
  }

  if (!removeIndexes.size) return candidate;

  const nextPoints = points.filter((_, index) => !removeIndexes.has(index));
  if (nextPoints.length < 3 || hasSelfIntersection(nextPoints)) return candidate;

  return {
    ...candidate,
    points: nextPoints,
    rect: getPointBounds(nextPoints),
    debug: {
      ...(candidate.debug ?? {}),
      finalPoints: nextPoints.length
    }
  };
}

function getPointCenter(points: Array<[number, number]>): [number, number] {
  return [
    points.reduce((sum, [x]) => sum + x, 0) / points.length,
    points.reduce((sum, [, y]) => sum + y, 0) / points.length
  ];
}

function refineCandidateWithDetectedWalls(
  candidate: RoomCandidate,
  wallGuard: PolygonWallGuard,
  imageData: ImageData,
  debug: WallSnapDebug
): RoomCandidate {
  let current = candidate;
  for (let pass = 0; pass < POLYGON_FINAL_REFINE_MAX_PASSES; pass += 1) {
    const before = current.points?.map(([x, y]) => [x, y] as [number, number]) ?? [];
    const trimmed = trimCandidateInsideDetectedWalls(current, wallGuard, imageData);
    const snapped = snapCandidateEdgesToDetectedWalls(trimmed, wallGuard, imageData, debug);
    const after = snapped.points ?? [];
    current = snapped;
    if (!after.length || samePoints(before, after)) break;
  }
  const cornerEnhanced = addMissingCornersFromWallIntersections(current, wallGuard, imageData, debug);
  const snappedCornerEnhanced = snapCandidateEdgesToDetectedWalls(cornerEnhanced, wallGuard, imageData, debug);
  const finalTrimmed = trimCandidateInsideDetectedWalls(snappedCornerEnhanced, wallGuard, imageData);
  return snapCandidateEdgesToDetectedWalls(finalTrimmed, wallGuard, imageData, debug);
}

function trimCandidateInsideDetectedWalls(
  candidate: RoomCandidate,
  wallGuard: PolygonWallGuard,
  imageData: ImageData
): RoomCandidate {
  if (candidate.shape !== "polygon" || !candidate.points || candidate.points.length < 3) return candidate;

  let points = candidate.points.map(([x, y]) => [x, y] as [number, number]);
  const originalArea = Math.abs(polygonArea(points));
  let wallCells = countWallCellsInsidePixelPolygon(points, wallGuard);
  if (wallCells === 0) return candidate;

  for (let pass = 0; pass < POLYGON_WALL_TRIM_MAX_PASSES; pass += 1) {
    let bestPoints: Array<[number, number]> | null = null;
    let bestWallCells = wallCells;
    let bestArea = 0;

    for (let edgeIndex = 0; edgeIndex < points.length; edgeIndex += 1) {
      const moved = movePolygonEdgeInward(points, edgeIndex, imageData);
      if (!moved || moved.length < 3) continue;
      const movedArea = Math.abs(polygonArea(moved));
      if (movedArea <= 0 || movedArea < originalArea * (1 - POLYGON_WALL_TRIM_MAX_AREA_SHRINK_RATIO)) continue;
      if (hasSelfIntersection(moved)) continue;

      const movedWallCells = countWallCellsInsidePixelPolygon(moved, wallGuard);
      if (movedWallCells < bestWallCells || (movedWallCells === bestWallCells && movedArea > bestArea)) {
        bestPoints = moved;
        bestWallCells = movedWallCells;
        bestArea = movedArea;
      }
    }

    if (!bestPoints || bestWallCells >= wallCells) break;
    points = bestPoints;
    wallCells = bestWallCells;
    if (wallCells === 0) break;
  }

  if (samePoints(candidate.points, points)) return candidate;

  return {
    ...candidate,
    points,
    rect: getPointBounds(points),
    debug: {
      ...(candidate.debug ?? {}),
      finalPoints: points.length,
      wallTrim: true
    }
  };
}

function snapCandidateEdgesToDetectedWalls(
  candidate: RoomCandidate,
  wallGuard: PolygonWallGuard,
  imageData: ImageData,
  debug: WallSnapDebug
): RoomCandidate {
  if (candidate.shape !== "polygon" || !candidate.points || candidate.points.length < 3 || !wallGuard.segments.length) {
    return candidate;
  }

  let points = candidate.points.map(([x, y]) => [x, y] as [number, number]);
  for (let edgeIndex = 0; edgeIndex < points.length; edgeIndex += 1) {
    const label = wallSnapEdgeDebugLabel(candidate, points, edgeIndex);
    debug.edgeChecks += 1;
    const edge = getPolygonSnapEdge(points, edgeIndex);
    if (!edge) {
      debug.noAxisEdges += 1;
      continue;
    }

    debug.candidateQueries += 1;
    const snapCandidates = getWallSnapCandidates(edge, wallGuard, imageData);
    if (!snapCandidates.length) {
      debug.candidateNone += 1;
      pushWallSnapLog(debug, `${label}: 빨간 후보선 없음 (${edge.axis})`);
      continue;
    }
    const snapCandidate = chooseClosestUnalignedSnapCandidate(snapCandidates, debug);
    if (!snapCandidate) {
      pushWallSnapLog(debug, `${label}: 이미 붙은 후보선 있음 (${edge.axis})`);
      continue;
    }
    const segment = snapCandidate.segment;

    debug.moveAttempts += 1;
    const moved = movePolygonEdgeToSegment(points, edgeIndex, edge, segment, imageData);
    if (!moved || moved.length < 3 || hasSelfIntersection(moved)) {
      debug.rejectedSelfIntersection += 1;
      pushWallSnapLog(debug, `${label}: self-intersection 거부 (${edge.axis}, dist ${Math.round(snapCandidate.distance)}px)`);
      continue;
    }
    if (isPolygonSizeOverExpanded(points, moved)) {
      debug.rejectedOverExpanded += 1;
      pushWallSnapLog(debug, `${label}: 과확장 거부 (${edge.axis}, dist ${Math.round(snapCandidate.distance)}px)`);
      continue;
    }

    debug.applied += 1;
    pushWallSnapLog(debug, `${label}: 적용 (${edge.axis}, dist ${Math.round(snapCandidate.distance)}px, overlap ${Math.round(snapCandidate.overlap * 100)}%)`);
    points = moved;
  }

  points = simplifyPixelPolygon(removeConsecutiveDuplicatePoints(points));
  if (samePoints(candidate.points, points)) return candidate;

  return {
    ...candidate,
    points,
    rect: getPointBounds(points),
    debug: {
      ...(candidate.debug ?? {}),
      finalPoints: points.length
    }
  };
}

function chooseClosestUnalignedSnapCandidate(
  candidates: ReturnType<typeof getVisibleSnapWallCandidates>,
  debug: WallSnapDebug
): ReturnType<typeof getVisibleSnapWallCandidates>[number] | null {
  if (!candidates.length) return null;
  if (candidates.some((candidate) => candidate.distance <= POLYGON_AUTO_WALL_SNAP_TOUCH_DISTANCE_PX)) {
    debug.alreadyAligned += 1;
    return null;
  }
  return [...candidates].sort((a, b) =>
    a.distance - b.distance ||
    b.overlap - a.overlap ||
    b.segment.length - a.segment.length
  )[0] ?? null;
}

function getWallSnapCandidates(
  edge: ReturnType<typeof getPolygonSnapEdge> extends infer T ? Exclude<T, null> : never,
  wallGuard: PolygonWallGuard,
  imageData: ImageData,
  options: Parameters<typeof getVisibleSnapWallCandidates>[2] = {}
): ReturnType<typeof getVisibleSnapWallCandidates> {
  return getVisibleSnapWallCandidates(edge, wallGuard.segments, {
    ...options,
    imageWidth: imageData.width,
    imageHeight: imageData.height
  });
}

function wallSnapEdgeDebugLabel(candidate: RoomCandidate, points: Array<[number, number]>, edgeIndex: number): string {
  const nextIndex = (edgeIndex + 1) % points.length;
  const name = candidate.name || candidate.id;
  return `${name} - ${edgeIndex + 1}-${nextIndex + 1}`;
}

function addMissingCornersFromWallIntersections(
  candidate: RoomCandidate,
  wallGuard: PolygonWallGuard,
  imageData: ImageData,
  debug: WallSnapDebug
): RoomCandidate {
  if (candidate.shape !== "polygon" || !candidate.points || candidate.points.length < 3 || !wallGuard.segments.length) {
    return candidate;
  }

  let points = candidate.points.map(([x, y]) => [x, y] as [number, number]);
  for (let edgeIndex = 0; edgeIndex < points.length; edgeIndex += 1) {
    const label = wallSnapEdgeDebugLabel(candidate, points, edgeIndex);
    const edge = getPolygonSnapEdge(points, edgeIndex);
    if (!edge) continue;
    const snapCandidates = getWallSnapCandidates(edge, wallGuard, imageData, { minLength: null });
    if (!snapCandidates.length) continue;
    if (snapCandidates.some((snapCandidate) => snapCandidate.distance <= POLYGON_AUTO_WALL_SNAP_TOUCH_DISTANCE_PX)) continue;

    debug.cornerAttempts += 1;
    const closestWall = [...snapCandidates].sort((a, b) => a.distance - b.distance)[0].segment;
    const intersection = findClosestWallIntersection(edge, closestWall, wallGuard.segments);
    if (!intersection) {
      debug.cornerNoIntersection += 1;
      pushWallSnapLog(debug, `${label}: 코너 교차점 없음 (${edge.axis})`);
      continue;
    }
    if (isNearExistingPoint(points, intersection, POLYGON_EXISTING_CORNER_DISTANCE_PX)) {
      debug.cornerExistingPoint += 1;
      pushWallSnapLog(debug, `${label}: 코너가 기존 꼭짓점 근처라 스킵`);
      continue;
    }

    const nextIndex = (edgeIndex + 1) % points.length;
    const moved = [
      ...points.slice(0, nextIndex),
      [clamp(intersection[0], 0, imageData.width), clamp(intersection[1], 0, imageData.height)] as [number, number],
      ...points.slice(nextIndex)
    ];
    if (isSpikePoint(moved, nextIndex)) {
      debug.cornerSpike += 1;
      pushWallSnapLog(debug, `${label}: 코너가 뾰족해서 스킵`);
      continue;
    }
    const cleaned = removeNearlyCollinearPoints(simplifyPixelPolygon(removeConsecutiveDuplicatePoints(moved)));
    if (cleaned.length < 3 || hasSelfIntersection(cleaned)) {
      debug.cornerSelfIntersection += 1;
      pushWallSnapLog(debug, `${label}: 코너 self-intersection 거부`);
      continue;
    }
    debug.cornerApplied += 1;
    pushWallSnapLog(debug, `${label}: 코너 보강 적용`);
    points = cleaned;
    edgeIndex += 1;
  }

  points = removeNearlyCollinearPoints(simplifyPixelPolygon(removeConsecutiveDuplicatePoints(points)));
  if (samePoints(candidate.points, points)) return candidate;

  return {
    ...candidate,
    points,
    rect: getPointBounds(points),
    debug: {
      ...(candidate.debug ?? {}),
      finalPoints: points.length
    }
  };
}

function findClosestWallIntersection(
  edge: { axis: "horizontal" | "vertical"; x1: number; y1: number; x2: number; y2: number },
  wall: FloorplanWallSegment,
  segments: FloorplanWallSegment[]
): [number, number] | null {
  const midpoint: [number, number] = [(edge.x1 + edge.x2) / 2, (edge.y1 + edge.y2) / 2];
  let best: { point: [number, number]; distance: number } | null = null;
  for (const segment of segments) {
    if (segment.axis === wall.axis) continue;
    const point = getSegmentIntersectionPoint(wall, segment);
    if (!point) continue;
    const distance = pointDistance(midpoint, point);
    if (distance > POLYGON_MISSING_CORNER_MAX_DISTANCE_PX) continue;
    if (!best || distance < best.distance) best = { point, distance };
  }
  return best?.point ?? null;
}

function getSegmentIntersectionPoint(a: FloorplanWallSegment, b: FloorplanWallSegment): [number, number] | null {
  const horizontal = a.axis === "horizontal" ? a : b.axis === "horizontal" ? b : null;
  const vertical = a.axis === "vertical" ? a : b.axis === "vertical" ? b : null;
  if (!horizontal || !vertical) return null;
  const x = vertical.x1;
  const y = horizontal.y1;
  if (!valueBetween(x, horizontal.x1, horizontal.x2) || !valueBetween(y, vertical.y1, vertical.y2)) return null;
  return [x, y];
}

function valueBetween(value: number, a: number, b: number): boolean {
  return value >= Math.min(a, b) - 0.001 && value <= Math.max(a, b) + 0.001;
}

function isNearExistingPoint(points: Array<[number, number]>, point: [number, number], distance: number): boolean {
  return points.some((candidate) => pointDistance(candidate, point) <= distance);
}

function removeNearlyCollinearPoints(points: Array<[number, number]>): Array<[number, number]> {
  if (points.length <= 3) return points;
  const result: Array<[number, number]> = [];
  let removed = 0;
  const maxRemove = points.length - 3;
  for (let index = 0; index < points.length; index += 1) {
    const previous = points[(index - 1 + points.length) % points.length];
    const current = points[index];
    const next = points[(index + 1) % points.length];
    if (removed < maxRemove && getPointAngleDegrees(previous, current, next) >= POLYGON_COLLINEAR_ANGLE_DEGREES) {
      removed += 1;
      continue;
    }
    result.push(current);
  }
  return result.length >= 3 ? result : points;
}

function isSpikePoint(points: Array<[number, number]>, index: number): boolean {
  if (points.length < 3) return false;
  const previous = points[(index - 1 + points.length) % points.length];
  const current = points[index];
  const next = points[(index + 1) % points.length];
  return getPointAngleDegrees(previous, current, next) <= POLYGON_SPIKE_ANGLE_DEGREES;
}

function getPointAngleDegrees(a: [number, number], b: [number, number], c: [number, number]): number {
  const abx = a[0] - b[0];
  const aby = a[1] - b[1];
  const cbx = c[0] - b[0];
  const cby = c[1] - b[1];
  const abLength = Math.hypot(abx, aby);
  const cbLength = Math.hypot(cbx, cby);
  if (abLength <= 0.001 || cbLength <= 0.001) return 180;
  const cosine = clamp((abx * cbx + aby * cby) / (abLength * cbLength), -1, 1);
  return Math.acos(cosine) * 180 / Math.PI;
}

function isPolygonSizeOverExpanded(
  current: Array<[number, number]>,
  candidate: Array<[number, number]>
): boolean {
  const currentBounds = getPointBounds(current);
  const candidateBounds = getPointBounds(candidate);
  const maxGrowthRatio = getPolygonSizeGrowthRatio(currentBounds);
  return (
    candidateBounds.width > currentBounds.width * (1 + maxGrowthRatio) ||
    candidateBounds.height > currentBounds.height * (1 + maxGrowthRatio)
  );
}

function getPolygonSizeGrowthRatio(bounds: RoomCandidate["rect"]): number {
  const size = Math.sqrt(Math.max(1, bounds.width * bounds.height));
  const t = clamp(
    (size - POLYGON_AUTO_WALL_SNAP_SMALL_SIZE_PX) /
      Math.max(1, POLYGON_AUTO_WALL_SNAP_LARGE_SIZE_PX - POLYGON_AUTO_WALL_SNAP_SMALL_SIZE_PX),
    0,
    1
  );
  return POLYGON_AUTO_WALL_SNAP_MIN_SIZE_GROWTH_RATIO +
    (POLYGON_AUTO_WALL_SNAP_MAX_SIZE_GROWTH_RATIO - POLYGON_AUTO_WALL_SNAP_MIN_SIZE_GROWTH_RATIO) * t;
}

function movePolygonEdgeToSegment(
  points: Array<[number, number]>,
  edgeIndex: number,
  edge: { axis: "horizontal" | "vertical"; x1: number; y1: number; x2: number; y2: number },
  segment: FloorplanWallSegment,
  imageData: ImageData
): Array<[number, number]> {
  const nextIndex = (edgeIndex + 1) % points.length;
  const moved = points.map(([x, y]) => [x, y] as [number, number]);
  if (edge.axis === "horizontal") {
    moved[edgeIndex] = [edge.x1, clamp(segment.y1, 0, imageData.height)];
    moved[nextIndex] = [edge.x2, clamp(segment.y1, 0, imageData.height)];
  } else {
    moved[edgeIndex] = [clamp(segment.x1, 0, imageData.width), edge.y1];
    moved[nextIndex] = [clamp(segment.x1, 0, imageData.width), edge.y2];
  }
  return simplifyPixelPolygon(removeConsecutiveDuplicatePoints(moved));
}

function movePolygonEdgeInward(
  points: Array<[number, number]>,
  edgeIndex: number,
  imageData: ImageData
): Array<[number, number]> | null {
  const current = points[edgeIndex];
  const nextIndex = (edgeIndex + 1) % points.length;
  const next = points[nextIndex];
  const dx = Math.abs(next[0] - current[0]);
  const dy = Math.abs(next[1] - current[1]);
  if (dx > 0.001 && dy > 0.001) return null;

  const bounds = getPointBounds(points);
  const centerX = bounds.x + bounds.width / 2;
  const centerY = bounds.y + bounds.height / 2;
  const moved = points.map(([x, y]) => [x, y] as [number, number]);

  if (dy <= 0.001) {
    const edgeY = (current[1] + next[1]) / 2;
    const delta = edgeY < centerY ? CELL_SIZE : -CELL_SIZE;
    moved[edgeIndex] = [current[0], clamp(current[1] + delta, 0, imageData.height)];
    moved[nextIndex] = [next[0], clamp(next[1] + delta, 0, imageData.height)];
  } else {
    const edgeX = (current[0] + next[0]) / 2;
    const delta = edgeX < centerX ? CELL_SIZE : -CELL_SIZE;
    moved[edgeIndex] = [clamp(current[0] + delta, 0, imageData.width), current[1]];
    moved[nextIndex] = [clamp(next[0] + delta, 0, imageData.width), next[1]];
  }

  return simplifyPixelPolygon(removeConsecutiveDuplicatePoints(moved));
}

function simplifyPixelPolygon(points: Array<[number, number]>): Array<[number, number]> {
  if (points.length < 3) return points;
  let result = points;
  let changed = true;
  while (changed && result.length > 3) {
    changed = false;
    const next: Array<[number, number]> = [];
    for (let index = 0; index < result.length; index += 1) {
      const previous = result[(index - 1 + result.length) % result.length];
      const current = result[index];
      const following = result[(index + 1) % result.length];
      if (isPixelCollinear(previous, current, following)) {
        changed = true;
        continue;
      }
      next.push(current);
    }
    result = next;
  }
  return result;
}

function isPixelCollinear(a: [number, number], b: [number, number], c: [number, number]): boolean {
  return (Math.abs(a[0] - b[0]) <= 0.001 && Math.abs(b[0] - c[0]) <= 0.001) ||
    (Math.abs(a[1] - b[1]) <= 0.001 && Math.abs(b[1] - c[1]) <= 0.001);
}

function countWallCellsInsidePixelPolygon(points: Array<[number, number]>, wallGuard: PolygonWallGuard): number {
  return countWallCellsInsidePolygon(
    points.map(([x, y]) => [x / CELL_SIZE, y / CELL_SIZE]),
    wallGuard
  );
}

function hasSelfIntersection(points: Array<[number, number]>): boolean {
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

function segmentsIntersect(a: [number, number], b: [number, number], c: [number, number], d: [number, number]): boolean {
  const o1 = orientation(a, b, c);
  const o2 = orientation(a, b, d);
  const o3 = orientation(c, d, a);
  const o4 = orientation(c, d, b);
  return o1 * o2 < 0 && o3 * o4 < 0;
}

function orientation(a: [number, number], b: [number, number], c: [number, number]): number {
  return Math.sign((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
}

function sanitizeRoomCandidates(
  candidates: RoomCandidate[],
  imageData: ImageData,
  applyCandidateFilters: boolean
): RoomCandidate[] {
  return candidates
    .map((candidate) => sanitizeRoomCandidate(candidate, imageData, applyCandidateFilters))
    .filter((candidate): candidate is RoomCandidate => Boolean(candidate));
}

function sanitizeRoomCandidate(
  candidate: RoomCandidate,
  imageData: ImageData,
  applyCandidateFilters: boolean
): RoomCandidate | null {
  const x = clamp(Math.round(candidate.rect.x), 0, imageData.width);
  const y = clamp(Math.round(candidate.rect.y), 0, imageData.height);
  const maxWidth = imageData.width - x;
  const maxHeight = imageData.height - y;
  const width = clamp(Math.round(candidate.rect.width), 0, maxWidth);
  const height = clamp(Math.round(candidate.rect.height), 0, maxHeight);
  if (applyCandidateFilters && (width < 24 || height < 24)) return null;
  if (width <= 0 || height <= 0) return null;

  const points = candidate.points
    ?.map(([px, py]) => [clamp(Math.round(px), 0, imageData.width), clamp(Math.round(py), 0, imageData.height)] as [number, number])
    .filter(([px, py]) => px >= x && py >= y && px <= x + width && py <= y + height);

  return {
    ...candidate,
    confidence: clamp(Math.round(candidate.confidence), 0, 100),
    shape: points && points.length >= 3 ? "polygon" : "rect",
    rect: { x, y, width, height },
    ...(points && points.length >= 3 ? { points } : { points: undefined })
  };
}

function countMask(mask: Uint8Array): number {
  let total = 0;
  for (const value of mask) total += value;
  return total;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, value));
}
