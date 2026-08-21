import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import {
  moveManualFloorplanDoorOnWalls,
  placeManualFloorplanDoorOnWalls,
  resizeManualFloorplanDoorOnWalls,
  type ManualFloorplanDoor,
  type ManualFloorplanDoorKind,
  type ManualFloorplanDoorPlacementFailure,
  type ManualFloorplanDoorResizeHandle
} from "../floorplan/manual-floorplan-doors";
import type { StoredFloorplanDirtyScope } from "./useStoredFloorplanEditSession.svelte";

interface StoredFloorplanDoorEditSession {
  pushHistory(): void;
  beginHistoryAction(actionKey: string): void;
  finishHistoryAction(actionKey?: string): void;
  markChanged(message?: string, scopes?: StoredFloorplanDirtyScope[]): void;
}

interface StoredFloorplanDoorEditorOptions {
  getDocument: () => FloorplanStorageDocument | null;
  setDocument: (document: FloorplanStorageDocument) => void;
  getEditSession: () => StoredFloorplanDoorEditSession;
}

export function createStoredFloorplanDoorEditor({
  getDocument,
  setDocument,
  getEditSession
}: StoredFloorplanDoorEditorOptions) {
  let placementActive = $state(false);
  let placementKind = $state<ManualFloorplanDoorKind>("hinged");
  let placementError = $state<ManualFloorplanDoorPlacementFailure | null>(null);
  let placementDoors = $state<ManualFloorplanDoor[]>([]);
  let selectedDoorId = $state("");

  function editableDocument(): FloorplanStorageDocument | null {
    const document = getDocument();
    return document?.source === "manual" &&
      Array.isArray(document.doors) &&
      Boolean(document.wallSegments?.length)
      ? document
      : null;
  }

  function doors(): ManualFloorplanDoor[] {
    if (placementActive) return placementDoors;
    return editableDocument()?.doors ?? [];
  }

  function mmPerPx(): number {
    const scale = editableDocument()?.scale;
    return scale ? (scale.mmPerPxX + scale.mmPerPxY) / 2 : 10;
  }

  function startPlacement(): void {
    const document = editableDocument();
    if (!document || placementActive) return;
    placementDoors = cloneDoors(document.doors ?? []);
    placementKind = "hinged";
    placementError = null;
    selectedDoorId = "";
    placementActive = true;
  }

  function setPlacementKind(kind: ManualFloorplanDoorKind): void {
    if (!placementActive) return;
    placementKind = kind;
    placementError = null;
  }

  function previewDoor(point: { x: number; y: number }) {
    const document = editableDocument();
    if (!document || !placementActive) return null;
    return placeManualFloorplanDoorOnWalls(
      point,
      document.wallSegments ?? [],
      placementDoors,
      placementKind,
      mmPerPx()
    ).door;
  }

  function placeDoor(point: { x: number; y: number }): boolean {
    const document = editableDocument();
    if (!document || !placementActive) return false;
    const result = placeManualFloorplanDoorOnWalls(
      point,
      document.wallSegments ?? [],
      placementDoors,
      placementKind,
      mmPerPx()
    );
    if (!result.door) {
      placementError = result.reason;
      return false;
    }
    const door = { ...result.door, id: nextDoorId(placementDoors) };
    placementDoors = [...placementDoors, door];
    selectedDoorId = door.id;
    placementError = null;
    return true;
  }

  function finishPlacement(): void {
    const document = editableDocument();
    if (!document || !placementActive) return;
    if (!sameDoors(document.doors ?? [], placementDoors)) {
      getEditSession().pushHistory();
      setDoors(placementDoors);
    }
    placementActive = false;
    placementDoors = [];
    placementError = null;
  }

  function cancelPlacement(): void {
    if (!placementActive) return;
    placementActive = false;
    placementDoors = [];
    placementError = null;
    selectedDoorId = "";
  }

  function selectDoor(id: string): void {
    selectedDoorId = doors().some((door) => door.id === id) ? id : "";
    placementError = null;
  }

  function removeDoor(id: string): void {
    if (placementActive) {
      placementDoors = placementDoors.filter((door) => door.id !== id);
      if (selectedDoorId === id) selectedDoorId = "";
      return;
    }
    const current = doors();
    if (!current.some((door) => door.id === id)) return;
    getEditSession().pushHistory();
    setDoors(current.filter((door) => door.id !== id));
    if (selectedDoorId === id) selectedDoorId = "";
  }

  function resizeDoor(
    id: string,
    handle: ManualFloorplanDoorResizeHandle,
    point: { x: number; y: number }
  ): boolean {
    const document = editableDocument();
    const current = doors();
    const door = current.find((candidate) => candidate.id === id);
    if (!document || !door || placementActive) return false;
    const result = resizeManualFloorplanDoorOnWalls(
      door,
      handle,
      point,
      document.wallSegments ?? [],
      current,
      mmPerPx()
    );
    if (!result.door) {
      placementError = result.reason;
      return false;
    }
    getEditSession().beginHistoryAction(`door-resize:${id}`);
    setDoors(current.map((candidate) => candidate.id === id ? result.door : candidate));
    selectedDoorId = id;
    placementError = null;
    return true;
  }

  function moveDoor(id: string, point: { x: number; y: number }): boolean {
    const document = editableDocument();
    const current = doors();
    const door = current.find((candidate) => candidate.id === id);
    if (!document || !door || placementActive) return false;
    const result = moveManualFloorplanDoorOnWalls(
      door,
      point,
      document.wallSegments ?? [],
      current,
      mmPerPx()
    );
    if (!result.door) {
      placementError = result.reason;
      return false;
    }
    getEditSession().beginHistoryAction(`door-move:${id}`);
    setDoors(current.map((candidate) => candidate.id === id ? result.door : candidate));
    selectedDoorId = id;
    placementError = null;
    return true;
  }

  function finishDoorInteraction(id: string, operation: "move" | "resize"): void {
    getEditSession().finishHistoryAction(`door-${operation}:${id}`);
  }

  function reset(): void {
    placementActive = false;
    placementKind = "hinged";
    placementError = null;
    placementDoors = [];
    selectedDoorId = "";
  }

  function setDoors(nextDoors: ManualFloorplanDoor[]): void {
    const document = editableDocument();
    if (!document) return;
    setDocument({ ...document, doors: cloneDoors(nextDoors) });
    getEditSession().markChanged("", ["document", "config"]);
  }

  return {
    get placementActive() {
      return placementActive;
    },
    get placementKind() {
      return placementKind;
    },
    get placementError() {
      return placementError;
    },
    get selectedDoorId() {
      return selectedDoorId;
    },
    doors,
    startPlacement,
    setPlacementKind,
    previewDoor,
    placeDoor,
    finishPlacement,
    cancelPlacement,
    selectDoor,
    removeDoor,
    resizeDoor,
    moveDoor,
    finishDoorInteraction,
    reset
  };
}

function cloneDoors(doors: ManualFloorplanDoor[]): ManualFloorplanDoor[] {
  return doors.map((door) => ({ ...door, centerPx: [...door.centerPx] }));
}

function sameDoors(first: ManualFloorplanDoor[], second: ManualFloorplanDoor[]): boolean {
  return JSON.stringify(first) === JSON.stringify(second);
}

function nextDoorId(doors: ManualFloorplanDoor[]): string {
  const used = new Set(doors.map((door) => door.id));
  let index = 1;
  while (used.has(`manual_door_${index}`)) index += 1;
  return `manual_door_${index}`;
}
