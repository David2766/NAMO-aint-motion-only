import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import {
  manualFloorplanDoorsValid,
  moveManualFloorplanDoor,
  placeManualFloorplanDoor,
  resizeManualFloorplanDoor,
  type ManualFloorplanDoor,
  type ManualFloorplanDoorKind,
  type ManualFloorplanDoorPlacementFailure,
  type ManualFloorplanDoorResizeHandle
} from "../floorplan/manual-floorplan-doors";
import {
  MANUAL_FLOORPLAN_HEIGHT_PX,
  MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT,
  MANUAL_FLOORPLAN_MAX_SCALE_PERCENT,
  MANUAL_FLOORPLAN_MIN_SCALE_PERCENT,
  MANUAL_FLOORPLAN_SCALE_STEP_PERCENT,
  MANUAL_FLOORPLAN_WIDTH_PX,
  manualFloorplanBounds,
  manualFloorplanDraftPointInsideOutline,
  manualFloorplanMmPerPx,
  manualFloorplanPolygonInsideOutline,
  renderManualFloorplanImage,
  snapManualFloorplanPoint,
  validManualFloorplanPolygon,
  type ManualFloorplanPoint
} from "../floorplan/manual-floorplan";

export type ManualFloorplanPhase = "idle" | "outline" | "rooms";

export function createManualFloorplanSession() {
  let active = $state(false);
  let phase = $state<ManualFloorplanPhase>("idle");
  let outlinePoints = $state<ManualFloorplanPoint[]>([]);
  let outlineEditing = $state(false);
  let outlineEditBackup = $state<ManualFloorplanPoint[]>([]);
  let doors = $state<ManualFloorplanDoor[]>([]);
  let doorPlacementActive = $state(false);
  let doorPlacementKind = $state<ManualFloorplanDoorKind>("hinged");
  let doorPlacementError = $state<ManualFloorplanDoorPlacementFailure | null>(null);
  let doorPlacementBackup = $state<ManualFloorplanDoor[]>([]);
  let selectedDoorId = $state("");
  let nextDoorId = 1;
  let scalePercent = $state(MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT);

  function start(): void {
    active = true;
    phase = "outline";
    outlinePoints = [];
    outlineEditing = false;
    outlineEditBackup = [];
    doors = [];
    doorPlacementActive = false;
    doorPlacementKind = "hinged";
    doorPlacementError = null;
    doorPlacementBackup = [];
    selectedDoorId = "";
    nextDoorId = 1;
    scalePercent = MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT;
  }

  function reset(): void {
    active = false;
    phase = "idle";
    outlinePoints = [];
    outlineEditing = false;
    outlineEditBackup = [];
    doors = [];
    doorPlacementActive = false;
    doorPlacementKind = "hinged";
    doorPlacementError = null;
    doorPlacementBackup = [];
    selectedDoorId = "";
    nextDoorId = 1;
    scalePercent = MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT;
  }

  function setScalePercent(value: number): void {
    if (!active || phase !== "outline") return;
    const stepped = Math.round(value / MANUAL_FLOORPLAN_SCALE_STEP_PERCENT) *
      MANUAL_FLOORPLAN_SCALE_STEP_PERCENT;
    scalePercent = Math.min(
      MANUAL_FLOORPLAN_MAX_SCALE_PERCENT,
      Math.max(MANUAL_FLOORPLAN_MIN_SCALE_PERCENT, stepped)
    );
  }

  function zoomScale(direction: number): void {
    setScalePercent(scalePercent + (direction > 0 ? 1 : -1) * MANUAL_FLOORPLAN_SCALE_STEP_PERCENT);
  }

  function transformDraftPoint(points: ManualFloorplanPoint[], point: { x: number; y: number }) {
    return snapManualFloorplanPoint(points, point, MANUAL_FLOORPLAN_WIDTH_PX, MANUAL_FLOORPLAN_HEIGHT_PX, mmPerPx());
  }

  function snapEditPoint(
    points: ManualFloorplanPoint[],
    index: number,
    point: { x: number; y: number }
  ) {
    if (!points.length || index < 0 || index > points.length) {
      return snapManualFloorplanPoint([], point, MANUAL_FLOORPLAN_WIDTH_PX, MANUAL_FLOORPLAN_HEIGHT_PX, mmPerPx());
    }
    const previousIndex = index === points.length
      ? points.length - 1
      : (index - 1 + points.length) % points.length;
    return snapManualFloorplanPoint(
      [points[previousIndex]],
      point,
      MANUAL_FLOORPLAN_WIDTH_PX,
      MANUAL_FLOORPLAN_HEIGHT_PX,
      mmPerPx()
    );
  }

  function draftValid(points: ManualFloorplanPoint[]): boolean {
    if (!validManualFloorplanPolygon(points)) return false;
    return phase === "outline" || manualFloorplanPolygonInsideOutline(points, outlinePoints);
  }

  function draftPointAllowed(points: ManualFloorplanPoint[], point: { x: number; y: number }): boolean {
    return phase === "outline" || manualFloorplanDraftPointInsideOutline(
      points,
      [point.x, point.y],
      outlinePoints
    );
  }

  function captureOutline(candidate: RoomCandidate): boolean {
    if (!active || phase !== "outline" || candidate.shape !== "polygon" || !candidate.points?.length) return false;
    outlinePoints = candidate.points.map(([x, y]) => [x, y]);
    phase = "rooms";
    return true;
  }

  function beginOutlineEdit(): void {
    if (!active || phase !== "rooms" || outlinePoints.length < 3 || outlineEditing) return;
    outlineEditBackup = clonePoints(outlinePoints);
    outlineEditing = true;
  }

  function outlineEditValid(rooms: RoomCandidate[]): boolean {
    return validManualFloorplanPolygon(outlinePoints) && roomsValid(rooms);
  }

  function roomsValid(rooms: RoomCandidate[]): boolean {
    const roomGeometryValid = rooms.every((room) =>
      room.status === "rejected" || manualFloorplanPolygonInsideOutline(roomPoints(room), outlinePoints)
    );
    return roomGeometryValid && manualFloorplanDoorsValid(doors, outlinePoints, rooms, mmPerPx());
  }

  function startDoorPlacement(): void {
    if (!active || phase !== "rooms" || outlineEditing || doorPlacementActive) return;
    doorPlacementBackup = cloneDoors(doors);
    doorPlacementKind = "hinged";
    doorPlacementError = null;
    selectedDoorId = "";
    doorPlacementActive = true;
  }

  function setDoorPlacementKind(kind: ManualFloorplanDoorKind): void {
    if (!doorPlacementActive) return;
    doorPlacementKind = kind;
    doorPlacementError = null;
  }

  function placeDoor(point: { x: number; y: number }, rooms: RoomCandidate[]): boolean {
    if (!doorPlacementActive) return false;
    const result = placeManualFloorplanDoor(
      point,
      outlinePoints,
      rooms,
      doors,
      doorPlacementKind,
      mmPerPx()
    );
    if (!result.door) {
      doorPlacementError = result.reason;
      return false;
    }
    const door = { ...result.door, id: `manual_door_${nextDoorId++}` };
    doors = [...doors, door];
    selectedDoorId = door.id;
    doorPlacementError = null;
    return true;
  }

  function previewDoor(point: { x: number; y: number }, rooms: RoomCandidate[]) {
    if (!doorPlacementActive) return null;
    return placeManualFloorplanDoor(
      point,
      outlinePoints,
      rooms,
      doors,
      doorPlacementKind,
      mmPerPx()
    ).door;
  }

  function selectDoor(id: string): void {
    selectedDoorId = doors.some((door) => door.id === id) ? id : "";
    doorPlacementError = null;
  }

  function resizeDoor(
    id: string,
    handle: ManualFloorplanDoorResizeHandle,
    point: { x: number; y: number },
    rooms: RoomCandidate[]
  ): boolean {
    const door = doors.find((candidate) => candidate.id === id);
    if (!door) return false;
    const result = resizeManualFloorplanDoor(
      door,
      handle,
      point,
      outlinePoints,
      rooms,
      doors,
      mmPerPx()
    );
    if (!result.door) {
      doorPlacementError = result.reason;
      return false;
    }
    doors = doors.map((candidate) => candidate.id === id ? result.door : candidate);
    selectedDoorId = id;
    doorPlacementError = null;
    return true;
  }

  function moveDoor(
    id: string,
    point: { x: number; y: number },
    rooms: RoomCandidate[]
  ): boolean {
    const door = doors.find((candidate) => candidate.id === id);
    if (!door) return false;
    const result = moveManualFloorplanDoor(
      door,
      point,
      outlinePoints,
      rooms,
      doors,
      mmPerPx()
    );
    if (!result.door) {
      doorPlacementError = result.reason;
      return false;
    }
    doors = doors.map((candidate) => candidate.id === id ? result.door : candidate);
    selectedDoorId = id;
    doorPlacementError = null;
    return true;
  }

  function finishDoorPlacement(): void {
    if (!doorPlacementActive) return;
    doorPlacementActive = false;
    doorPlacementError = null;
    doorPlacementBackup = [];
  }

  function cancelDoorPlacement(): void {
    if (!doorPlacementActive) return;
    doors = cloneDoors(doorPlacementBackup);
    doorPlacementActive = false;
    doorPlacementError = null;
    doorPlacementBackup = [];
  }

  function removeDoor(id: string): void {
    doors = doors.filter((door) => door.id !== id);
    if (selectedDoorId === id) selectedDoorId = "";
    doorPlacementError = null;
  }

  function finishOutlineEdit(rooms: RoomCandidate[]): boolean {
    if (!outlineEditing || !outlineEditValid(rooms)) return false;
    outlineEditing = false;
    outlineEditBackup = [];
    return true;
  }

  function cancelOutlineEdit(): void {
    if (!outlineEditing) return;
    outlinePoints = clonePoints(outlineEditBackup);
    outlineEditing = false;
    outlineEditBackup = [];
  }

  function moveOutlinePoint(index: number, point: { x: number; y: number }): void {
    if (!outlineEditing || index < 0 || index >= outlinePoints.length) return;
    const previous = outlinePoints[(index - 1 + outlinePoints.length) % outlinePoints.length];
    const snapped = snapManualFloorplanPoint(
      [previous], point, MANUAL_FLOORPLAN_WIDTH_PX, MANUAL_FLOORPLAN_HEIGHT_PX, mmPerPx()
    );
    outlinePoints = outlinePoints.map((current, pointIndex) =>
      pointIndex === index ? [snapped.x, snapped.y] : current
    );
  }

  function addOutlinePoint(edgeIndex: number, point: { x: number; y: number }): void {
    if (!outlineEditing || edgeIndex < 0 || edgeIndex >= outlinePoints.length) return;
    const start = outlinePoints[edgeIndex];
    const snapped = snapManualFloorplanPoint(
      [start], point, MANUAL_FLOORPLAN_WIDTH_PX, MANUAL_FLOORPLAN_HEIGHT_PX, mmPerPx()
    );
    outlinePoints = [
      ...outlinePoints.slice(0, edgeIndex + 1),
      [snapped.x, snapped.y],
      ...outlinePoints.slice(edgeIndex + 1)
    ];
  }

  function deleteOutlinePoint(index: number): void {
    if (!outlineEditing || outlinePoints.length <= 3 || index < 0 || index >= outlinePoints.length) return;
    outlinePoints = outlinePoints.filter((_, pointIndex) => pointIndex !== index);
  }

  function scaleInput() {
    const bounds = manualFloorplanBounds(outlinePoints);
    if (!bounds) return null;
    return {
      widthMm: Math.round(bounds.width * mmPerPx()),
      heightMm: Math.round(bounds.height * mmPerPx()),
      bounds
    };
  }

  function render(rooms: RoomCandidate[], showGrid = false): Promise<Blob> {
    return renderManualFloorplanImage({
      rooms,
      outline: outlinePoints,
      doors,
      showGrid,
      width: MANUAL_FLOORPLAN_WIDTH_PX,
      height: MANUAL_FLOORPLAN_HEIGHT_PX,
      mmPerPx: mmPerPx()
    });
  }

  function mmPerPx(): number {
    return manualFloorplanMmPerPx(scalePercent);
  }

  return {
    get active() {
      return active;
    },
    get phase() {
      return phase;
    },
    get outlinePoints() {
      return outlinePoints;
    },
    get outlineEditing() {
      return outlineEditing;
    },
    get doors() {
      return doors;
    },
    get doorPlacementActive() {
      return doorPlacementActive;
    },
    get doorPlacementKind() {
      return doorPlacementKind;
    },
    get doorPlacementError() {
      return doorPlacementError;
    },
    get selectedDoorId() {
      return selectedDoorId;
    },
    get scalePercent() {
      return scalePercent;
    },
    get scaleAdjustable() {
      return active && phase === "outline";
    },
    get canZoomOut() {
      return active && phase === "outline" && scalePercent > MANUAL_FLOORPLAN_MIN_SCALE_PERCENT;
    },
    get width() {
      return MANUAL_FLOORPLAN_WIDTH_PX;
    },
    get height() {
      return MANUAL_FLOORPLAN_HEIGHT_PX;
    },
    get mmPerPx() {
      return mmPerPx();
    },
    start,
    reset,
    setScalePercent,
    zoomScale,
    transformDraftPoint,
    snapEditPoint,
    draftValid,
    draftPointAllowed,
    captureOutline,
    beginOutlineEdit,
    outlineEditValid,
    roomsValid,
    startDoorPlacement,
    setDoorPlacementKind,
    placeDoor,
    previewDoor,
    selectDoor,
    resizeDoor,
    moveDoor,
    finishDoorPlacement,
    cancelDoorPlacement,
    removeDoor,
    finishOutlineEdit,
    cancelOutlineEdit,
    moveOutlinePoint,
    addOutlinePoint,
    deleteOutlinePoint,
    scaleInput,
    render
  };
}

function roomPoints(room: RoomCandidate): ManualFloorplanPoint[] {
  if (room.shape === "polygon" && room.points?.length) return clonePoints(room.points);
  const { x, y, width, height } = room.rect;
  return [[x, y], [x + width, y], [x + width, y + height], [x, y + height]];
}

function clonePoints(points: ManualFloorplanPoint[]): ManualFloorplanPoint[] {
  return points.map(([x, y]) => [x, y]);
}

function cloneDoors(doors: ManualFloorplanDoor[]): ManualFloorplanDoor[] {
  return doors.map((door) => ({ ...door, centerPx: [...door.centerPx] as [number, number] }));
}
