import type {
  FloorplanWallSegment,
  RoomCandidate,
  RoomCandidateStatus
} from "../../core/floorplan/floorplan-types";
import type { FloorplanSnapEdge } from "../../core/floorplan/wall-snap";
import {
  getRoomCandidateSnapEdges,
  getVisibleSnapWallCandidates
} from "../../core/floorplan/wall-snap";
import { cleanupOrthogonalRoomCandidate } from "../../core/floorplan/room-candidate-cleanup";
import {
  mergeRoomCandidatePoints,
  normalizeRect,
  rectFromPoints,
  splitRoomCandidateByLine
} from "../floorplan/floorplan-room-edit-geometry";

type PointTuple = [number, number];

interface RoomCandidateStateLike {
  candidates: RoomCandidate[];
  selectedCandidateId: string;
  replace(candidates: RoomCandidate[], selectedCandidateId?: string): void;
  update(id: string, candidate: RoomCandidate): void;
  select(id: string): void;
  add(candidate: RoomCandidate): void;
}

interface RoomCandidateEditSessionLike {
  pushHistory(): void;
  withHistory(action: () => void): void;
  cleanup(id: string, saveHistory?: boolean): boolean;
  clearSelectedVertex(): void;
}

interface ImageSize {
  width?: number;
  height?: number;
}

interface ManualRoomDraft {
  active: boolean;
  points: PointTuple[];
}

interface ManualRoomDraftHover {
  x: number;
  y: number;
}

interface RoomSplitDraft {
  active: boolean;
  candidateId: string;
  points: PointTuple[];
}

interface RoomMergeDraft {
  active: boolean;
  candidateIds: string[];
}

interface SnapEdit {
  active: boolean;
  candidateId: string;
  edgeKey: string;
  original: RoomCandidate | null;
}

interface RoomCandidateMapToolsText {
  defaultRoomName(index: number): string;
  defaultRoomCandidateName(index: number): string;
  toolTitleSnap: string;
  toolTitleSplit: string;
  toolTitleMerge: string;
  toolTitleManualRoom: string;
  snapSelectWall: string;
  snapSelectEdge: string;
  splitStart: string;
  splitEnd: string;
  splitReady: string;
  splitInvalid: string;
  mergeSelectTwo: string;
  mergeSelectOneMore: string;
  mergeReady: string;
  manualNeedPoints: string;
  manualReady: string;
  manualInvalid: string;
}

interface ManualDraftText {
  title: string;
  needPoints: string;
  ready: string;
  invalid: string;
}

interface RoomCandidateMapToolsOptions {
  roomCandidates: RoomCandidateStateLike;
  roomCandidateEdit: RoomCandidateEditSessionLike;
  getImageSize: () => ImageSize | null;
  getWallSegments: () => FloorplanWallSegment[];
  getText: () => RoomCandidateMapToolsText;
  getManualPointTransform?: () => ((points: PointTuple[], point: { x: number; y: number }) => { x: number; y: number }) | null;
  getManualPointValidator?: () => ((points: PointTuple[], point: { x: number; y: number }) => boolean) | null;
  getManualDraftValidator?: () => ((points: PointTuple[]) => boolean) | null;
  getManualDraftText?: () => ManualDraftText | null;
  onManualRoomFinished?: (candidate: RoomCandidate) => boolean | void;
}

export function createRoomCandidateMapTools({
  roomCandidates,
  roomCandidateEdit,
  getImageSize,
  getWallSegments,
  getText,
  getManualPointTransform = () => null,
  getManualPointValidator = () => null,
  getManualDraftValidator = () => null,
  getManualDraftText = () => null,
  onManualRoomFinished
}: RoomCandidateMapToolsOptions) {
  let snapEdit = $state<SnapEdit>(defaultSnapEdit());
  let manualRoomDraft = $state<ManualRoomDraft>(defaultManualRoomDraft());
  let manualRoomDraftHover = $state<ManualRoomDraftHover | null>(null);
  let roomSplitDraft = $state<RoomSplitDraft>(defaultRoomSplitDraft());
  let roomSplitDraftHover = $state<ManualRoomDraftHover | null>(null);
  let roomMergeDraft = $state<RoomMergeDraft>(defaultRoomMergeDraft());

  function resetAll(): void {
    resetSnapEdit();
    manualRoomDraft = defaultManualRoomDraft();
    manualRoomDraftHover = null;
    roomSplitDraft = defaultRoomSplitDraft();
    roomSplitDraftHover = null;
    roomMergeDraft = defaultRoomMergeDraft();
  }

  function resetSnapEdit(): void {
    snapEdit = defaultSnapEdit();
  }

  function setManualRoomDraft(draft: ManualRoomDraft): void {
    manualRoomDraft = {
      active: draft.active,
      points: clonePoints(draft.points)
    };
  }

  function setManualRoomDraftHover(hover: ManualRoomDraftHover | null): void {
    manualRoomDraftHover = hover ? { ...hover } : null;
  }

  function snapManualPoint(point: { x: number; y: number }): { x: number; y: number } {
    const lineThreshold = 12;
    const cornerThreshold = 20;
    const cornerEndpointTolerance = 10;
    let cornerBest: { point: { x: number; y: number }; distance: number } | null = null;
    const manualSnapSegments = getWallSegments();
    const horizontalSegments = manualSnapSegments.filter((segment) => segment.axis === "horizontal");
    const verticalSegments = manualSnapSegments.filter((segment) => segment.axis === "vertical");

    for (const horizontal of horizontalSegments) {
      const hMinX = Math.min(horizontal.x1, horizontal.x2);
      const hMaxX = Math.max(horizontal.x1, horizontal.x2);
      if (point.x < hMinX - cornerThreshold || point.x > hMaxX + cornerThreshold) continue;
      if (Math.abs(point.y - horizontal.y1) > cornerThreshold) continue;

      for (const vertical of verticalSegments) {
        const vMinY = Math.min(vertical.y1, vertical.y2);
        const vMaxY = Math.max(vertical.y1, vertical.y2);
        if (point.y < vMinY - cornerThreshold || point.y > vMaxY + cornerThreshold) continue;
        if (Math.abs(point.x - vertical.x1) > cornerThreshold) continue;

        const corner = { x: vertical.x1, y: horizontal.y1 };
        const touchesHorizontal = corner.x >= hMinX - cornerEndpointTolerance && corner.x <= hMaxX + cornerEndpointTolerance;
        const touchesVertical = corner.y >= vMinY - cornerEndpointTolerance && corner.y <= vMaxY + cornerEndpointTolerance;
        if (!touchesHorizontal || !touchesVertical) continue;

        const distance = Math.hypot(point.x - corner.x, point.y - corner.y);
        if (distance <= cornerThreshold && (!cornerBest || distance < cornerBest.distance)) {
          cornerBest = { point: corner, distance };
        }
      }
    }

    if (cornerBest) return cornerBest.point;

    let best: { point: { x: number; y: number }; distance: number } | null = null;
    for (const segment of manualSnapSegments) {
      let snapped: { x: number; y: number } | null = null;
      if (segment.axis === "horizontal") {
        const minX = Math.min(segment.x1, segment.x2);
        const maxX = Math.max(segment.x1, segment.x2);
        if (point.x < minX - lineThreshold || point.x > maxX + lineThreshold) continue;
        const x = Math.min(maxX, Math.max(minX, point.x));
        snapped = { x, y: segment.y1 };
      } else if (segment.axis === "vertical") {
        const minY = Math.min(segment.y1, segment.y2);
        const maxY = Math.max(segment.y1, segment.y2);
        if (point.y < minY - lineThreshold || point.y > maxY + lineThreshold) continue;
        const y = Math.min(maxY, Math.max(minY, point.y));
        snapped = { x: segment.x1, y };
      }
      if (!snapped) continue;
      const distance = Math.hypot(point.x - snapped.x, point.y - snapped.y);
      if (distance <= lineThreshold && (!best || distance < best.distance)) {
        best = { point: snapped, distance };
      }
    }
    return best?.point ?? point;
  }

  function startRoomSplitDraft(id = roomCandidates.selectedCandidateId): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    finishSnapEdit();
    cancelManualRoomDraft();
    cancelRoomMergeDraft();
    roomCandidates.select(id);
    roomSplitDraft = { active: true, candidateId: id, points: [] };
    roomSplitDraftHover = null;
  }

  function cancelRoomSplitDraft(): void {
    roomSplitDraft = defaultRoomSplitDraft();
    roomSplitDraftHover = null;
  }

  function startRoomMergeDraft(): void {
    finishSnapEdit();
    cancelManualRoomDraft();
    cancelRoomSplitDraft();
    const initialIds = roomCandidates.selectedCandidateId ? [roomCandidates.selectedCandidateId] : [];
    roomMergeDraft = { active: true, candidateIds: initialIds };
  }

  function cancelRoomMergeDraft(): void {
    roomMergeDraft = defaultRoomMergeDraft();
  }

  function selectRoomCandidate(id: string): void {
    if (!roomMergeDraft.active) {
      if (roomCandidates.selectedCandidateId !== id) {
        roomCandidateEdit.clearSelectedVertex();
      }
      roomCandidates.select(id);
      return;
    }
    const candidate = roomCandidates.candidates.find((item) => item.id === id && item.status !== "rejected");
    if (!candidate) return;
    const exists = roomMergeDraft.candidateIds.includes(id);
    const candidateIds = exists
      ? roomMergeDraft.candidateIds.filter((candidateId) => candidateId !== id)
      : [...roomMergeDraft.candidateIds, id].slice(-2);
    roomMergeDraft = { ...roomMergeDraft, candidateIds };
    roomCandidateEdit.clearSelectedVertex();
    roomCandidates.select(id);
  }

  function canFinishRoomMergeDraft(): boolean {
    if (!roomMergeDraft.active || roomMergeDraft.candidateIds.length !== 2) return false;
    return roomMergeDraft.candidateIds.every((id) =>
      roomCandidates.candidates.some((candidate) => candidate.id === id && candidate.status !== "rejected")
    );
  }

  function finishRoomMergeDraft(): void {
    if (!canFinishRoomMergeDraft()) return;
    const [firstId, secondId] = roomMergeDraft.candidateIds;
    const first = roomCandidates.candidates.find((candidate) => candidate.id === firstId);
    const second = roomCandidates.candidates.find((candidate) => candidate.id === secondId);
    if (!first || !second) return;
    const firstIndex = roomCandidates.candidates.findIndex((candidate) => candidate.id === firstId);
    const secondIndex = roomCandidates.candidates.findIndex((candidate) => candidate.id === secondId);
    const insertIndex = Math.max(0, Math.min(firstIndex, secondIndex));
    const points = mergeRoomCandidatePoints(first, second);
    if (!points.length) return;
    const text = getText();
    const mergedCandidate: RoomCandidate = {
      ...first,
      id: `${first.id}_merge_${second.id}_${Date.now()}`,
      name: first.name || second.name || text.defaultRoomName(insertIndex + 1),
      status: getManualPointTransform()
        ? first.status
        : "candidate" satisfies RoomCandidateStatus,
      confidence: Math.max(first.confidence ?? 0, second.confidence ?? 0),
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        ...(first.debug ?? {}),
        reason: "merge",
        finalPoints: points.length,
        closedLoop: true
      }
    };
    roomCandidateEdit.withHistory(() => {
      const candidates = roomCandidates.candidates.filter((candidate) => candidate.id !== firstId && candidate.id !== secondId);
      candidates.splice(insertIndex, 0, mergedCandidate);
      roomCandidates.replace(candidates, mergedCandidate.id);
    });
    cancelRoomMergeDraft();
  }

  function getSplitCandidate(): RoomCandidate | null {
    return roomCandidates.candidates.find((candidate) => candidate.id === roomSplitDraft.candidateId) ?? null;
  }

  function splitDraftTargetRect() {
    return getSplitCandidate()?.rect ?? null;
  }

  function clampPointToRect(point: { x: number; y: number }, rect: { x: number; y: number; width: number; height: number }) {
    return {
      x: Math.min(rect.x + rect.width, Math.max(rect.x, point.x)),
      y: Math.min(rect.y + rect.height, Math.max(rect.y, point.y))
    };
  }

  function constrainSplitPoint(point: { x: number; y: number }) {
    const first = roomSplitDraft.points[0];
    if (!first) return point;
    const dx = Math.abs(point.x - first[0]);
    const dy = Math.abs(point.y - first[1]);
    return dx >= dy ? { x: point.x, y: first[1] } : { x: first[0], y: point.y };
  }

  function snapAndClampSplitPoint(point: { x: number; y: number }): PointTuple {
    const rect = splitDraftTargetRect();
    const snapped = constrainSplitPoint(snapManualPoint(point));
    const clamped = rect ? clampPointToRect(snapped, rect) : snapped;
    const size = getImageSize();
    return [
      Math.round(Math.min(size?.width ?? clamped.x, Math.max(0, clamped.x))),
      Math.round(Math.min(size?.height ?? clamped.y, Math.max(0, clamped.y)))
    ];
  }

  function addRoomSplitPoint(point: { x: number; y: number }): void {
    if (!roomSplitDraft.active || !getImageSize() || roomSplitDraft.points.length >= 2) return;
    const nextPoint = snapAndClampSplitPoint(point);
    roomSplitDraft = {
      ...roomSplitDraft,
      points: [...roomSplitDraft.points, nextPoint]
    };
    roomSplitDraftHover = null;
  }

  function previewRoomSplitPoint(point: { x: number; y: number }): void {
    if (!roomSplitDraft.active || !getImageSize() || roomSplitDraft.points.length !== 1) return;
    const [x, y] = snapAndClampSplitPoint(point);
    roomSplitDraftHover = { x, y };
  }

  function clearRoomSplitPreview(): void {
    roomSplitDraftHover = null;
  }

  function moveRoomSplitPoint(index: number, point: { x: number; y: number }): void {
    if (!roomSplitDraft.active || !getImageSize() || index < 0 || index >= roomSplitDraft.points.length) return;
    const points = clonePoints(roomSplitDraft.points);
    points[index] = snapAndClampSplitPoint(point);
    roomSplitDraft = { ...roomSplitDraft, points };
    roomSplitDraftHover = null;
  }

  function roomSplitLine() {
    const first = roomSplitDraft.points[0];
    const second = roomSplitDraft.points[1] ?? (roomSplitDraftHover ? [roomSplitDraftHover.x, roomSplitDraftHover.y] as PointTuple : null);
    if (!first || !second) return null;
    return { x1: first[0], y1: first[1], x2: second[0], y2: second[1] };
  }

  function canFinishRoomSplitDraft(): boolean {
    const candidate = getSplitCandidate();
    const line = roomSplitLine();
    if (!candidate || !line || roomSplitDraft.points.length < 2) return false;
    return Boolean(splitRoomCandidateByLine(candidate, line));
  }

  function finishRoomSplitDraft(): void {
    const candidate = getSplitCandidate();
    const line = roomSplitLine();
    if (!candidate || !line || !canFinishRoomSplitDraft()) return;
    const splitResult = splitRoomCandidateByLine(candidate, line);
    if (!splitResult) return;
    const candidateIndex = roomCandidates.candidates.findIndex((item) => item.id === candidate.id);
    const splitStamp = Date.now();
    const baseName = getText().defaultRoomCandidateName(candidateIndex + 1);
    const makeCandidate = (suffix: number, points: PointTuple[]): RoomCandidate => ({
      ...candidate,
      id: `${candidate.id}_split_${splitStamp}_${suffix}`,
      name: `${baseName}-${suffix}`,
      status: getManualPointTransform() ? candidate.status : "candidate",
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        ...(candidate.debug ?? {}),
        reason: "split",
        finalPoints: points.length,
        closedLoop: true
      }
    });
    const nextCandidates = [
      makeCandidate(1, splitResult[0]),
      makeCandidate(2, splitResult[1])
    ];
    roomCandidateEdit.withHistory(() => {
      const candidates = roomCandidates.candidates.flatMap((item) => item.id === candidate.id ? nextCandidates : [item]);
      roomCandidates.replace(candidates, nextCandidates[0].id);
    });
    cancelRoomSplitDraft();
  }

  function activeMapToolTitle(): string {
    const text = getText();
    if (snapEdit.active) return text.toolTitleSnap;
    if (roomSplitDraft.active) return text.toolTitleSplit;
    if (roomMergeDraft.active) return text.toolTitleMerge;
    if (manualRoomDraft.active) return getManualDraftText()?.title ?? text.toolTitleManualRoom;
    return "";
  }

  function activeMapToolMessage(): string {
    const text = getText();
    if (snapEdit.active) {
      return snapEdit.edgeKey
        ? text.snapSelectWall
        : text.snapSelectEdge;
    }
    if (roomSplitDraft.active) {
      if (roomSplitDraft.points.length === 0) return text.splitStart;
      if (roomSplitDraft.points.length === 1) return text.splitEnd;
      return canFinishRoomSplitDraft()
        ? text.splitReady
        : text.splitInvalid;
    }
    if (roomMergeDraft.active) {
      if (roomMergeDraft.candidateIds.length === 0) return text.mergeSelectTwo;
      if (roomMergeDraft.candidateIds.length === 1) return text.mergeSelectOneMore;
      return text.mergeReady;
    }
    if (manualRoomDraft.active) {
      const manualText = getManualDraftText();
      if (manualRoomDraft.points.length < 3) return manualText?.needPoints ?? text.manualNeedPoints;
      const validator = getManualDraftValidator();
      if (validator && !validator(manualRoomDraft.points)) return manualText?.invalid ?? text.manualInvalid;
      return manualText?.ready ?? text.manualReady;
    }
    return "";
  }

  function activeMapToolCanFinish(): boolean {
    if (snapEdit.active) return Boolean(snapEdit.edgeKey);
    if (roomSplitDraft.active) return canFinishRoomSplitDraft();
    if (roomMergeDraft.active) return canFinishRoomMergeDraft();
    if (manualRoomDraft.active) {
      const validator = getManualDraftValidator();
      return validator ? validator(manualRoomDraft.points) : manualRoomDraft.points.length >= 3;
    }
    return false;
  }

  function finishActiveMapTool(): void {
    if (snapEdit.active) {
      finishSnapEdit();
    } else if (roomSplitDraft.active) {
      finishRoomSplitDraft();
    } else if (roomMergeDraft.active) {
      finishRoomMergeDraft();
    } else if (manualRoomDraft.active) {
      finishManualRoomDraft();
    }
  }

  function cancelActiveMapTool(): void {
    if (snapEdit.active) {
      cancelSnapEdit();
    } else if (roomSplitDraft.active) {
      cancelRoomSplitDraft();
    } else if (roomMergeDraft.active) {
      cancelRoomMergeDraft();
    } else if (manualRoomDraft.active) {
      cancelManualRoomDraft();
    }
  }

  function startManualRoomDraft(): void {
    finishSnapEdit();
    roomCandidates.select("");
    cancelRoomSplitDraft();
    cancelRoomMergeDraft();
    manualRoomDraft = { active: true, points: [] };
    manualRoomDraftHover = null;
  }

  function cancelManualRoomDraft(): void {
    manualRoomDraft = defaultManualRoomDraft();
    manualRoomDraftHover = null;
  }

  function previewManualRoomPoint(point: { x: number; y: number }): void {
    const size = getImageSize();
    if (!manualRoomDraft.active || !size) return;
    const snapped = snapAndConstrainManualPoint(point);
    const preservePrecision = Boolean(getManualPointTransform());
    const hover = {
      x: roundedManualCoordinate(Math.min(size.width ?? snapped.x, Math.max(0, snapped.x)), preservePrecision),
      y: roundedManualCoordinate(Math.min(size.height ?? snapped.y, Math.max(0, snapped.y)), preservePrecision)
    };
    const validator = getManualPointValidator();
    manualRoomDraftHover = validator && !validator(manualRoomDraft.points, hover) ? null : hover;
  }

  function clearManualRoomPointPreview(): void {
    manualRoomDraftHover = null;
  }

  function constrainManualPointToStraightLine(point: { x: number; y: number }) {
    const lastPoint = manualRoomDraft.points[manualRoomDraft.points.length - 1];
    if (!lastPoint) return point;
    const [lastX, lastY] = lastPoint;
    const dx = Math.abs(point.x - lastX);
    const dy = Math.abs(point.y - lastY);
    return dx >= dy
      ? { x: point.x, y: lastY }
      : { x: lastX, y: point.y };
  }

  function snapAndConstrainManualPoint(point: { x: number; y: number }) {
    const snapped = snapManualPoint(point);
    const transform = getManualPointTransform();
    return transform
      ? transform(manualRoomDraft.points, snapped)
      : constrainManualPointToStraightLine(snapped);
  }

  function addManualRoomPoint(point: { x: number; y: number }): void {
    const size = getImageSize();
    if (!manualRoomDraft.active || !size) return;
    const snapped = snapAndConstrainManualPoint(point);
    const preservePrecision = Boolean(getManualPointTransform());
    const x = roundedManualCoordinate(Math.min(size.width ?? snapped.x, Math.max(0, snapped.x)), preservePrecision);
    const y = roundedManualCoordinate(Math.min(size.height ?? snapped.y, Math.max(0, snapped.y)), preservePrecision);
    const validator = getManualPointValidator();
    if (validator && !validator(manualRoomDraft.points, { x, y })) {
      manualRoomDraftHover = null;
      return;
    }
    roomCandidateEdit.pushHistory();
    manualRoomDraft = {
      active: true,
      points: [...manualRoomDraft.points, [x, y]]
    };
    manualRoomDraftHover = null;
  }

  function deleteManualRoomPoint(index: number): void {
    if (!manualRoomDraft.active || index < 0 || index >= manualRoomDraft.points.length) return;
    roomCandidateEdit.pushHistory();
    manualRoomDraft = {
      active: true,
      points: manualRoomDraft.points.filter((_, pointIndex) => pointIndex !== index)
    };
    manualRoomDraftHover = null;
  }

  function finishManualRoomDraft(): void {
    if (!getImageSize() || manualRoomDraft.points.length < 3) return;
    const points = clonePoints(manualRoomDraft.points);
    const validator = getManualDraftValidator();
    if (validator && !validator(points)) return;
    const candidate: RoomCandidate = {
      id: `manual_room_${Date.now()}`,
      name: getText().defaultRoomName(roomCandidates.candidates.length + 1),
      kind: "unknown",
      confidence: 100,
      status: getManualPointTransform() ? "confirmed" : "candidate",
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        finalPoints: points.length,
        closedLoop: true,
        reason: "manual"
      }
    };
    const finishedCandidate = getManualPointTransform()
      ? candidate
      : cleanupOrthogonalRoomCandidate(candidate).candidate;
    const keepCandidate = onManualRoomFinished?.(finishedCandidate) !== false;
    if (keepCandidate) roomCandidateEdit.withHistory(() => roomCandidates.add(finishedCandidate));
    if (keepCandidate && getManualPointTransform()) roomCandidates.select("");
    manualRoomDraft = defaultManualRoomDraft();
    manualRoomDraftHover = null;
  }

  function getSelectedCandidate(): RoomCandidate | null {
    return roomCandidates.candidates.find((candidate) => candidate.id === roomCandidates.selectedCandidateId) ?? null;
  }

  function startSnapEdit(id: string): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    cancelManualRoomDraft();
    cancelRoomSplitDraft();
    cancelRoomMergeDraft();
    roomCandidates.select(id);
    snapEdit = {
      active: true,
      candidateId: id,
      edgeKey: "",
      original: cloneCandidate(candidate)
    };
  }

  function finishSnapEdit(): void {
    if (snapEdit.candidateId && !getManualPointTransform()) {
      roomCandidateEdit.cleanup(snapEdit.candidateId, false);
    }
    resetSnapEdit();
  }

  function cancelSnapEdit(): void {
    if (snapEdit.original) {
      roomCandidates.update(snapEdit.original.id, snapEdit.original);
    }
    finishSnapEdit();
  }

  function getSnapEdges(): FloorplanSnapEdge[] {
    if (!snapEdit.active) return [];
    const candidate = roomCandidates.candidates.find((item) => item.id === snapEdit.candidateId);
    if (!candidate) return [];
    return getRoomCandidateSnapEdges(candidate);
  }

  function selectSnapEdge(edgeKey: string): void {
    snapEdit = { ...snapEdit, edgeKey };
  }

  function getSnapWallSegments(): FloorplanWallSegment[] {
    if (!snapEdit.active || !snapEdit.edgeKey) return [];
    const edge = getSnapEdges().find((item) => item.key === snapEdit.edgeKey);
    if (!edge) return [];
    const size = getImageSize();
    return getVisibleSnapWallCandidates(edge, getWallSegments(), {
      imageWidth: size?.width,
      imageHeight: size?.height
    })
      .map(({ segment }) => segment);
  }

  function snapSelectedEdgeToWall(segmentId: string): void {
    const candidate = getSelectedCandidate();
    const edge = getSnapEdges().find((item) => item.key === snapEdit.edgeKey);
    const segment = getWallSegments().find((item) => item.id === segmentId);
    if (!candidate || !edge || !segment) return;
    const nextCandidate = candidate.shape === "polygon" && candidate.points?.length
      ? snapPolygonCandidate(candidate, edge, segment)
      : snapRectCandidate(candidate, edge, segment);
    roomCandidateEdit.withHistory(() => roomCandidates.update(candidate.id, nextCandidate));
  }

  function isActive(): boolean {
    return snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active;
  }

  return {
    get snapEdit() {
      return snapEdit;
    },
    get manualRoomDraft() {
      return manualRoomDraft;
    },
    get manualRoomDraftHover() {
      return manualRoomDraftHover;
    },
    get roomSplitDraft() {
      return roomSplitDraft;
    },
    get roomSplitDraftHover() {
      return roomSplitDraftHover;
    },
    get roomMergeDraft() {
      return roomMergeDraft;
    },
    resetAll,
    resetSnapEdit,
    setManualRoomDraft,
    setManualRoomDraftHover,
    snapManualPoint,
    startRoomSplitDraft,
    cancelRoomSplitDraft,
    canFinishRoomSplitDraft,
    finishRoomSplitDraft,
    addRoomSplitPoint,
    previewRoomSplitPoint,
    clearRoomSplitPreview,
    moveRoomSplitPoint,
    startRoomMergeDraft,
    cancelRoomMergeDraft,
    canFinishRoomMergeDraft,
    finishRoomMergeDraft,
    selectRoomCandidate,
    activeMapToolTitle,
    activeMapToolMessage,
    activeMapToolCanFinish,
    finishActiveMapTool,
    cancelActiveMapTool,
    startManualRoomDraft,
    cancelManualRoomDraft,
    previewManualRoomPoint,
    clearManualRoomPointPreview,
    addManualRoomPoint,
    deleteManualRoomPoint,
    finishManualRoomDraft,
    startSnapEdit,
    finishSnapEdit,
    cancelSnapEdit,
    getSnapEdges,
    selectSnapEdge,
    getSnapWallSegments,
    snapSelectedEdgeToWall,
    isActive
  };
}

function defaultSnapEdit(): SnapEdit {
  return { active: false, candidateId: "", edgeKey: "", original: null };
}

function roundedManualCoordinate(value: number, preservePrecision: boolean): number {
  return preservePrecision ? Math.round(value * 100) / 100 : Math.round(value);
}

function defaultManualRoomDraft(): ManualRoomDraft {
  return { active: false, points: [] };
}

function defaultRoomSplitDraft(): RoomSplitDraft {
  return { active: false, candidateId: "", points: [] };
}

function defaultRoomMergeDraft(): RoomMergeDraft {
  return { active: false, candidateIds: [] };
}

function snapRectCandidate(candidate: RoomCandidate, edge: FloorplanSnapEdge, segment: FloorplanWallSegment): RoomCandidate {
  const rect = { ...candidate.rect };
  const minSize = 12;
  if (edge.key === "rect:top") {
    const bottom = rect.y + rect.height;
    rect.y = Math.min(segment.y1, bottom - minSize);
    rect.height = bottom - rect.y;
  } else if (edge.key === "rect:bottom") {
    rect.height = Math.max(minSize, segment.y1 - rect.y);
  } else if (edge.key === "rect:left") {
    const right = rect.x + rect.width;
    rect.x = Math.min(segment.x1, right - minSize);
    rect.width = right - rect.x;
  } else if (edge.key === "rect:right") {
    rect.width = Math.max(minSize, segment.x1 - rect.x);
  }
  return { ...candidate, rect: normalizeRect(rect) };
}

function snapPolygonCandidate(candidate: RoomCandidate, edge: FloorplanSnapEdge, segment: FloorplanWallSegment): RoomCandidate {
  const points = clonePoints(candidate.points ?? []);
  if (!points.length || edge.index === undefined) return candidate;
  const index = edge.index;
  const nextIndex = (index + 1) % points.length;
  if (edge.axis === "horizontal") {
    points[index] = [points[index][0], segment.y1];
    points[nextIndex] = [points[nextIndex][0], segment.y1];
  } else {
    points[index] = [segment.x1, points[index][1]];
    points[nextIndex] = [segment.x1, points[nextIndex][1]];
  }
  return {
    ...candidate,
    points,
    rect: rectFromPoints(points)
  };
}

function cloneCandidate(candidate: RoomCandidate): RoomCandidate {
  return {
    ...candidate,
    rect: { ...candidate.rect },
    points: candidate.points ? clonePoints(candidate.points) : undefined
  };
}

function clonePoints(points: PointTuple[]): PointTuple[] {
  return points.map(([x, y]) => [x, y]);
}
