import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { WebDeviceConfig, WebZone } from "../types";

type SaveTone = "idle" | "saving" | "ok" | "error";
export type StoredFloorplanDirtyScope = "document" | "coordinateFrame" | "config" | "roomContext" | "radar" | "roomName" | "occlusion" | "objects" | "automationAreas";

interface StoredFloorplanSnapshot {
  document: FloorplanStorageDocument | null;
  zones: WebZone[];
  calibrationZones: WebZone[];
  selectedFurnitureObjectId: string;
  selectedRadarZoneId: string;
  selectedRadarZonePointIndex: number;
  editTool: string;
  showRadarOverlay: boolean;
}

interface StoredFloorplanEditSessionOptions {
  maxHistory?: number;
  getDocument: () => FloorplanStorageDocument | null;
  getConfig: () => WebDeviceConfig | null;
  restoreDocument: (document: FloorplanStorageDocument | null) => void;
  updateConfig?: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
  getMode: () => string;
  getEditTool: () => string;
  setEditTool: (tool: string) => void;
  getShowRadarOverlay: () => boolean;
  setShowRadarOverlay: (visible: boolean) => void;
}

export function createStoredFloorplanEditSession({
  maxHistory = 40,
  getDocument,
  getConfig,
  restoreDocument,
  updateConfig,
  getMode,
  getEditTool,
  setEditTool,
  getShowRadarOverlay,
  setShowRadarOverlay
}: StoredFloorplanEditSessionOptions) {
  let dirty = $state(false);
  let dirtyScopes = $state<StoredFloorplanDirtyScope[]>([]);
  let saveBusy = $state(false);
  let saveStatus = $state("");
  let saveTone = $state<SaveTone>("idle");
  let selectedFurnitureObjectId = $state("");
  let selectedRadarZoneId = $state("");
  let selectedRadarZonePointIndex = $state(-1);
  let roomNamePatches = $state<Record<string, string>>({});
  let undoStack = $state<StoredFloorplanSnapshot[]>([]);
  let redoStack = $state<StoredFloorplanSnapshot[]>([]);
  let historyActionKey = $state("");

  function snapshot(): StoredFloorplanSnapshot {
    const config = getConfig();
    return {
      document: clonePlain(getDocument()),
      zones: clonePlain(config?.zones ?? []),
      calibrationZones: clonePlain(config?.calibrationZones ?? []),
      selectedFurnitureObjectId,
      selectedRadarZoneId,
      selectedRadarZonePointIndex,
      editTool: getEditTool(),
      showRadarOverlay: getShowRadarOverlay()
    };
  }

  function restore(snapshotValue: StoredFloorplanSnapshot): void {
    restoreDocument(clonePlain(snapshotValue.document));
    updateConfig?.((current) => ({
      ...current,
      zones: clonePlain(snapshotValue.zones ?? []),
      calibrationZones: clonePlain(snapshotValue.calibrationZones ?? [])
    }));
    selectedFurnitureObjectId = snapshotValue.selectedFurnitureObjectId ?? "";
    selectedRadarZoneId = snapshotValue.selectedRadarZoneId ?? "";
    selectedRadarZonePointIndex = snapshotValue.selectedRadarZonePointIndex ?? -1;
    setEditTool(snapshotValue.editTool ?? getEditTool());
    setShowRadarOverlay(snapshotValue.showRadarOverlay ?? getShowRadarOverlay());
    markChanged("", dirtyScopesForTool(snapshotValue.editTool ?? getEditTool()));
    historyActionKey = "";
  }

  function pushHistory(): void {
    if (!getDocument() && !getConfig()) return;
    undoStack = [...undoStack, snapshot()].slice(-maxHistory);
    redoStack = [];
  }

  function beginHistoryAction(actionKey: string): void {
    if (historyActionKey === actionKey) return;
    pushHistory();
    historyActionKey = actionKey;
  }

  function finishHistoryAction(actionKey = historyActionKey): void {
    if (!actionKey || historyActionKey === actionKey) historyActionKey = "";
  }

  function undo(): void {
    if (!undoStack.length) return;
    const previous = undoStack[undoStack.length - 1];
    undoStack = undoStack.slice(0, -1);
    redoStack = [...redoStack, snapshot()].slice(-maxHistory);
    restore(previous);
  }

  function redo(): void {
    if (!redoStack.length) return;
    const next = redoStack[redoStack.length - 1];
    redoStack = redoStack.slice(0, -1);
    undoStack = [...undoStack, snapshot()].slice(-maxHistory);
    restore(next);
  }

  function resetHistory(): void {
    undoStack = [];
    redoStack = [];
    historyActionKey = "";
  }

  function resetAfterLoad(): void {
    dirty = false;
    dirtyScopes = [];
    roomNamePatches = {};
    saveStatus = "";
    saveTone = "idle";
    selectedFurnitureObjectId = "";
    resetHistory();
  }

  function resetAfterDelete(): void {
    dirty = false;
    dirtyScopes = [];
    roomNamePatches = {};
    saveStatus = "";
    saveTone = "idle";
    selectedFurnitureObjectId = "";
    selectedRadarZoneId = "";
    selectedRadarZonePointIndex = -1;
    resetHistory();
  }

  function markChanged(message = "", scopes: StoredFloorplanDirtyScope[] = ["document"]): void {
    dirty = true;
    dirtyScopes = [...new Set([...dirtyScopes, ...scopes])];
    if (message) {
      saveTone = "idle";
      saveStatus = message;
    }
  }

  function markRoomNameChanged(roomId: string, name: string): void {
    if (!roomId) return;
    roomNamePatches = {
      ...roomNamePatches,
      [roomId]: name
    };
    markChanged("", ["roomName", "roomContext"]);
  }

  function isDirty(scope: StoredFloorplanDirtyScope): boolean {
    return dirtyScopes.includes(scope);
  }

  function clearDirty(): void {
    dirty = false;
    dirtyScopes = [];
    roomNamePatches = {};
  }

  function dirtyScopesForTool(tool: string): StoredFloorplanDirtyScope[] {
    if (tool === "zones" || tool === "exit" || tool === "calibration") return ["config"];
    if (tool === "radar") return ["radar", "roomContext"];
    if (tool === "automationAreas") return ["automationAreas"];
    if (tool === "rooms") return ["document", "roomContext"];
    if (tool === "scale") return ["document", "coordinateFrame", "roomContext"];
    return ["document"];
  }

  function setSaveStatus(message: string, tone: SaveTone): void {
    saveStatus = message;
    saveTone = tone === "ok" ? "idle" : tone;
  }

  function usesStoredHistory(): boolean {
    return getMode() === "edit" && getEditTool() !== "rooms";
  }

  return {
    get dirty() {
      return dirty;
    },
    set dirty(value: boolean) {
      dirty = value;
      dirtyScopes = value ? ["document", "roomContext"] : [];
    },
    get dirtyScopes() {
      return dirtyScopes;
    },
    get roomNamePatches() {
      return roomNamePatches;
    },
    get saveBusy() {
      return saveBusy;
    },
    set saveBusy(value: boolean) {
      saveBusy = value;
    },
    get saveStatus() {
      return saveStatus;
    },
    set saveStatus(value: string) {
      saveStatus = value;
    },
    get saveTone() {
      return saveTone;
    },
    set saveTone(value: SaveTone) {
      saveTone = value;
    },
    get selectedFurnitureObjectId() {
      return selectedFurnitureObjectId;
    },
    set selectedFurnitureObjectId(value: string) {
      selectedFurnitureObjectId = value;
    },
    get selectedRadarZoneId() {
      return selectedRadarZoneId;
    },
    set selectedRadarZoneId(value: string) {
      selectedRadarZoneId = value;
    },
    get selectedRadarZonePointIndex() {
      return selectedRadarZonePointIndex;
    },
    set selectedRadarZonePointIndex(value: number) {
      selectedRadarZonePointIndex = value;
    },
    get canUndo() {
      return undoStack.length > 0;
    },
    get canRedo() {
      return redoStack.length > 0;
    },
    pushHistory,
    beginHistoryAction,
    finishHistoryAction,
    undo,
    redo,
    resetAfterLoad,
    resetAfterDelete,
    markChanged,
    markRoomNameChanged,
    isDirty,
    clearDirty,
    setSaveStatus,
    usesStoredHistory
  };
}

function clonePlain<T>(value: T): T {
  return value == null ? value : JSON.parse(JSON.stringify(value));
}
