import type { RoomCandidate, RoomCandidateStatus } from "../../core/floorplan/floorplan-types";
import type { RadarScreenPoint } from "../../core/types";
import { cleanupOrthogonalRoomCandidate } from "../../core/floorplan/room-candidate-cleanup";
import {
  deletePolygonObjectPoint,
  insertPolygonObjectPoint,
  movePolygonObjectPoint
} from "../zone-geometry";
import { rectFromPoints } from "../floorplan/floorplan-room-edit-geometry";

interface RoomCandidateStateLike {
  candidates: RoomCandidate[];
  selectedCandidateId: string;
  replace(candidates: RoomCandidate[], selectedCandidateId?: string): void;
  update(id: string, candidate: RoomCandidate): void;
  remove(id: string): void;
  setStatus(id: string, status: RoomCandidateStatus): void;
  select(id: string): void;
  rename(id: string, name: string): void;
  add(candidate: RoomCandidate): void;
}

interface ManualRoomDraft {
  active: boolean;
  points: Array<[number, number]>;
}

interface ManualRoomDraftHover {
  x: number;
  y: number;
}

interface RoomCandidateEditSessionOptions {
  roomCandidates: RoomCandidateStateLike;
  maxHistory: number;
  getImageSize: () => { width?: number; height?: number } | null;
  getManualRoomDraft: () => ManualRoomDraft;
  setManualRoomDraft: (draft: ManualRoomDraft) => void;
  getManualRoomDraftHover: () => ManualRoomDraftHover | null;
  setManualRoomDraftHover: (hover: ManualRoomDraftHover | null) => void;
  snapPoint: (
    point: { x: number; y: number },
    points: Array<[number, number]>,
    index: number
  ) => { x: number; y: number };
  snapInsertedPoint?: (
    point: { x: number; y: number },
    points: Array<[number, number]>,
    index: number
  ) => { x: number; y: number };
  markStoredRoomsChanged: () => void;
  markStoredRoomNameChanged: (id: string, name: string) => void;
  canDeleteSelectedVertex: () => boolean;
  resetSnapEdit: () => void;
}

export function createRoomCandidateEditSession({
  roomCandidates,
  maxHistory,
  getImageSize,
  getManualRoomDraft,
  setManualRoomDraft,
  getManualRoomDraftHover,
  setManualRoomDraftHover,
  snapPoint,
  snapInsertedPoint,
  markStoredRoomsChanged,
  markStoredRoomNameChanged,
  canDeleteSelectedVertex,
  resetSnapEdit
}: RoomCandidateEditSessionOptions) {
  let selectedVertexIndex = $state(-1);
  let undoStack = $state<RoomCandidateSnapshot[]>([]);
  let redoStack = $state<RoomCandidateSnapshot[]>([]);

  function snapshot(): RoomCandidateSnapshot {
    const manualRoomDraft = getManualRoomDraft();
    const manualRoomDraftHover = getManualRoomDraftHover();
    return {
      candidates: roomCandidates.candidates.map(cloneCandidate),
      selectedCandidateId: roomCandidates.selectedCandidateId,
      manualRoomDraft: {
        active: manualRoomDraft.active,
        points: manualRoomDraft.points.map(([x, y]) => [x, y])
      },
      manualRoomDraftHover: manualRoomDraftHover ? { ...manualRoomDraftHover } : null
    };
  }

  function pushHistory(): void {
    undoStack = [...undoStack, snapshot()].slice(-maxHistory);
    redoStack = [];
  }

  function restoreSnapshot(nextSnapshot: RoomCandidateSnapshot): void {
    roomCandidates.replace(nextSnapshot.candidates.map(cloneCandidate), nextSnapshot.selectedCandidateId);
    resetSnapEdit();
    setManualRoomDraft(nextSnapshot.manualRoomDraft
      ? {
          active: nextSnapshot.manualRoomDraft.active,
          points: nextSnapshot.manualRoomDraft.points.map(([x, y]) => [x, y])
        }
      : { active: false, points: [] });
    setManualRoomDraftHover(nextSnapshot.manualRoomDraftHover ? { ...nextSnapshot.manualRoomDraftHover } : null);
  }

  function undo(): void {
    if (!undoStack.length) return;
    const previous = undoStack[undoStack.length - 1];
    undoStack = undoStack.slice(0, -1);
    redoStack = [...redoStack, snapshot()].slice(-maxHistory);
    restoreSnapshot(previous);
  }

  function redo(): void {
    if (!redoStack.length) return;
    const next = redoStack[redoStack.length - 1];
    redoStack = redoStack.slice(0, -1);
    undoStack = [...undoStack, snapshot()].slice(-maxHistory);
    restoreSnapshot(next);
  }

  function withHistory(action: () => void): void {
    pushHistory();
    action();
    markStoredRoomsChanged();
  }

  function updatePoints(id: string, points: Array<[number, number]>, saveHistory = true): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate || points.length < 3) return;
    const nextCandidate: RoomCandidate = {
      ...candidate,
      shape: "polygon",
      points,
      rect: rectFromPoints(points),
      debug: {
        ...(candidate.debug ?? {}),
        finalPoints: points.length
      }
    };
    if (saveHistory) {
      withHistory(() => roomCandidates.update(id, nextCandidate));
    } else {
      roomCandidates.update(id, nextCandidate);
      markStoredRoomsChanged();
    }
  }

  function cleanup(id: string, saveHistory = true): boolean {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return false;
    const result = cleanupOrthogonalRoomCandidate(candidate);
    if (!result.changed) return false;
    if (saveHistory) {
      withHistory(() => roomCandidates.update(id, result.candidate));
    } else {
      roomCandidates.update(id, result.candidate);
    }
    return true;
  }

  function cleanupSelected(saveHistory = true): boolean {
    const id = roomCandidates.selectedCandidateId;
    return id ? cleanup(id, saveHistory) : false;
  }

  function clearSelectedVertex(): void {
    selectedVertexIndex = -1;
  }

  function addVertex(id: string, edgeIndex: number, point: RadarScreenPoint): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points?.length || edgeIndex < 0) return;
    const editableCandidate = { ...candidate, points: clonePoints(candidate.points) };
    const size = getImageSize();
    const snapped = snapInsertedPoint?.(point, editableCandidate.points, edgeIndex + 1) ?? point;
    const result = insertPolygonObjectPoint(editableCandidate, edgeIndex, snapped, {
      bounds: { width: size?.width, height: size?.height }
    });
    if (!result.changed) return;
    selectedVertexIndex = result.selectedPointIndex;
    updatePoints(id, result.item.points);
  }

  function deleteVertex(id: string, index: number): boolean {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points || candidate.points.length <= 3 || index < 0 || index >= candidate.points.length) return false;
    const editableCandidate = { ...candidate, points: clonePoints(candidate.points) };
    const result = deletePolygonObjectPoint(editableCandidate, index);
    if (!result.changed) return false;
    selectedVertexIndex = result.selectedPointIndex;
    updatePoints(id, result.item.points);
    return true;
  }

  function deleteSelectedVertex(): boolean {
    if (!roomCandidates.selectedCandidateId || selectedVertexIndex < 0) return false;
    if (!canDeleteSelectedVertex()) return false;
    return deleteVertex(roomCandidates.selectedCandidateId, selectedVertexIndex);
  }

  function moveVertex(id: string, index: number, point: { x: number; y: number }): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points || index < 0 || index >= candidate.points.length) return;
    const editableCandidate = { ...candidate, points: clonePoints(candidate.points) };
    const snapped = snapPoint(point, editableCandidate.points, index);
    const size = getImageSize();
    const result = movePolygonObjectPoint(editableCandidate, index, snapped, {
      bounds: { width: size?.width, height: size?.height }
    });
    if (!result.changed) return;
    selectedVertexIndex = result.selectedPointIndex;
    updatePoints(id, result.item.points, false);
  }

  function selectVertex(id: string, index: number): void {
    roomCandidates.select(id);
    selectedVertexIndex = index;
  }

  function beginVertexMove(id: string, index: number): void {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    selectVertex(id, index);
    pushHistory();
  }

  function endVertexMove(id: string, cleanupCandidate = true): void {
    if (cleanupCandidate) cleanup(id, false);
    markStoredRoomsChanged();
  }

  function rename(id: string, name: string): void {
    roomCandidates.rename(id, name);
    const nextName = roomCandidates.candidates.find((candidate) => candidate.id === id)?.name ?? name.slice(0, 16);
    markStoredRoomNameChanged(id, nextName);
  }

  function remove(id: string): void {
    withHistory(() => roomCandidates.remove(id));
  }

  function setStatus(id: string, status: RoomCandidateStatus): void {
    withHistory(() => roomCandidates.setStatus(id, status));
  }

  function resetHistory(): void {
    undoStack = [];
    redoStack = [];
  }

  return {
    get selectedVertexIndex() {
      return selectedVertexIndex;
    },
    set selectedVertexIndex(index: number) {
      selectedVertexIndex = index;
    },
    get canUndo() {
      return undoStack.length > 0;
    },
    get canRedo() {
      return redoStack.length > 0;
    },
    pushHistory,
    withHistory,
    undo,
    redo,
    resetHistory,
    updatePoints,
    cleanup,
    cleanupSelected,
    clearSelectedVertex,
    addVertex,
    deleteVertex,
    deleteSelectedVertex,
    moveVertex,
    selectVertex,
    beginVertexMove,
    endVertexMove,
    rename,
    remove,
    setStatus
  };
}

interface RoomCandidateSnapshot {
  candidates: RoomCandidate[];
  selectedCandidateId: string;
  manualRoomDraft: ManualRoomDraft;
  manualRoomDraftHover: ManualRoomDraftHover | null;
}

function cloneCandidate(candidate: RoomCandidate): RoomCandidate {
  return {
    ...candidate,
    rect: { ...candidate.rect },
    points: candidate.points ? clonePoints(candidate.points) : undefined
  };
}

function clonePoints(points: Array<[number, number]>): Array<[number, number]> {
  return points.map(([x, y]) => [x, y]);
}
