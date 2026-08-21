import type { RadarScreenPoint, WebDeviceConfig, WebZone, WebZoneType } from "../../core/types";
import type { Messages } from "../i18n/types";
import { zoneGeometryMessage } from "../i18n/zone-geometry";
import {
  addSoftwareZoneDraft,
  convertZoneToRectInConfig,
  deleteZone,
  deleteZonePointInConfig,
  insertZonePointInConfig,
  moveExitLineZone,
  moveZone,
  renameZone,
  setCalibrationZoneType,
  setZoneType,
  updateExitLineEndpoint,
  updateZonePoint,
  type ZoneGeometryMessageCode,
  type ZoneGeometryMessageParams,
  type ZoneMutationResult
} from "../zone-geometry";
import type { StoredFloorplanDirtyScope } from "./useStoredFloorplanEditSession.svelte";

type StoredRadarZoneTool = "rooms" | "scale" | "radar" | "zones" | "calibration" | "furniture" | string;
type StoredRadarZoneSource = "zones" | "calibration" | "";
type StoredCalibrationZoneType = Extract<WebZoneType, "filter" | "reduced" | "disabled">;

interface StoredRadarZoneEditorText {
  storedAddZoneUnavailable: string;
  storedAddPointUnavailable: string;
  storedCalibrationUpdated: string;
  storedZoneAdded: string;
  storedZoneDeleted: string;
  storedZonePointAdded: string;
  storedZonePointDeleted: string;
  storedZoneRectified: string;
  storedZoneUpdated: string;
}

interface StoredFloorplanEditSessionLike {
  selectedRadarZoneId: string;
  selectedRadarZonePointIndex: number;
  pushHistory(): void;
  beginHistoryAction(actionKey: string): void;
  finishHistoryAction(actionKey?: string): void;
  markChanged(message?: string, scopes?: StoredFloorplanDirtyScope[]): void;
  saveStatus: string;
  saveTone: "idle" | "saving" | "ok" | "error";
}

interface StoredRadarZoneEditorOptions {
  getConfig: () => WebDeviceConfig | null;
  updateConfig?: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
  getMode: () => string;
  getTool: () => StoredRadarZoneTool;
  setTool: (tool: StoredRadarZoneTool) => void;
  setShowRadarOverlay: (visible: boolean) => void;
  editSession: StoredFloorplanEditSessionLike;
  onSelectDeviceZone?: (zoneId: string) => void;
  getMessages: () => Messages;
  getText: () => StoredRadarZoneEditorText;
  canUseDetectionType?: () => boolean;
  onDetectionOwnerRequired?: () => void;
}

export function createStoredRadarZoneEditor({
  getConfig,
  updateConfig,
  getMode,
  getTool,
  setTool,
  setShowRadarOverlay,
  editSession,
  onSelectDeviceZone,
  getMessages,
  getText,
  canUseDetectionType = () => true,
  onDetectionOwnerRequired = () => undefined
}: StoredRadarZoneEditorOptions) {
  function editSource(): StoredRadarZoneSource {
    if (getMode() !== "edit") return "";
    const tool = getTool();
    if (tool === "zones") return "zones";
    if (tool === "calibration") return "calibration";
    return "";
  }

  function toolZones(tool = getTool()): WebZone[] {
    const config = getConfig();
    if (tool === "calibration") return config?.calibrationZones ?? [];
    const zones = config?.zones ?? [];
    if (tool === "zones") return zones;
    return zones;
  }

  function overlayZones(): WebZone[] {
    const tool = getTool();
    if (getMode() === "edit" && tool === "zones") {
      return toolZones(tool);
    }
    return getConfig()?.zones ?? [];
  }

  function selectedZone(source = editSource()): WebZone | null {
    const config = getConfig();
    const zones = source === "calibration"
      ? (config?.calibrationZones ?? [])
      : getMode() === "edit" && getTool() === "zones"
        ? toolZones(getTool())
        : (config?.zones ?? []);
    return zones.find((zone) => zone.id === editSession.selectedRadarZoneId) ?? null;
  }

  function selectZone(zoneId: string, pointIndex = -1): void {
    editSession.selectedRadarZoneId = zoneId;
    editSession.selectedRadarZonePointIndex = pointIndex;
    onSelectDeviceZone?.(zoneId);
  }

  function beginTool(tool: StoredRadarZoneTool): void {
    setTool(tool);
    setShowRadarOverlay(true);
    editSession.selectedRadarZonePointIndex = -1;
    const zones = toolZones(tool);
    if (!zones.some((zone) => zone.id === editSession.selectedRadarZoneId)) {
      selectZone(zones[0]?.id ?? "", -1);
    }
  }

  function updateZone(source: StoredRadarZoneSource, zoneId: string, updater: (zone: WebZone) => WebZone): void {
    if (!updateConfig) return;
    updateConfig((current) => {
      if (source === "calibration") {
        return {
          ...current,
          calibrationZones: (current.calibrationZones ?? []).map((zone) => zone.id === zoneId ? updater(zone) : zone)
        };
      }
      return {
        ...current,
        zones: current.zones.map((zone) => zone.id === zoneId ? updater(zone) : zone)
      };
    });
    const text = getText();
    editSession.markChanged(source === "calibration" ? text.storedCalibrationUpdated : text.storedZoneUpdated, ["config"]);
  }

  function updateConfigWithResult(mutator: (current: WebDeviceConfig) => ZoneMutationResult): ZoneMutationResult | null {
    if (!updateConfig) return null;
    let result: ZoneMutationResult | null = null;
    updateConfig((current) => {
      result = mutator(current);
      return result.config;
    });
    return result as ZoneMutationResult | null;
  }

  function setMutationError(
    messageCode: ZoneGeometryMessageCode | undefined,
    messageParams: ZoneGeometryMessageParams | undefined,
    fallback: string
  ): void {
    editSession.saveTone = "error";
    editSession.saveStatus = zoneGeometryMessage(getMessages(), messageCode, messageParams) || fallback;
  }

  function addZone(): void {
    editSession.pushHistory();
    const result = updateConfigWithResult((current) => addSoftwareZoneDraft(current, getMessages().zones));
    if (!result) return;
    const text = getText();
    if (!result.changed) {
      setMutationError(result.messageCode, result.messageParams, text.storedAddZoneUnavailable);
      return;
    }
    selectZone(result.selectedZoneId ?? "", -1);
    editSession.markChanged(text.storedZoneAdded, ["config"]);
  }

  function deleteSelectedZone(): void {
    if (!editSession.selectedRadarZoneId) return;
    const current = selectedZone();
    if (current?.type === "detection" && !current.placeholder && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.pushHistory();
    const result = updateConfigWithResult((current) => deleteZone(current, editSession.selectedRadarZoneId));
    if (!result?.changed) return;
    const tool = getTool();
    if (tool === "zones") {
      selectZone(result.config.zones[0]?.id ?? "", -1);
    } else {
      selectZone(result.selectedZoneId ?? "", -1);
    }
    editSession.markChanged(getText().storedZoneDeleted, ["config"]);
  }

  function renameSelectedZone(name: string): void {
    const zoneId = editSession.selectedRadarZoneId;
    const tool = getTool();
    if (!zoneId || tool !== "zones") return;
    const current = selectedZone();
    if (current?.type === "detection" && !current.placeholder && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.beginHistoryAction(`zone-name:${zoneId}`);
    updateConfigWithResult((current) => renameZone(current, zoneId, name));
    editSession.markChanged(getText().storedZoneUpdated, ["config"]);
  }

  function setSelectedZoneType(type: WebZoneType): void {
    const zoneId = editSession.selectedRadarZoneId;
    if (!zoneId || getTool() !== "zones") return;
    const current = selectedZone();
    if ((!current?.placeholder && current?.type === "detection" && !canUseDetectionType()) ||
        (type === "detection" && !canUseDetectionType())) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.pushHistory();
    updateConfigWithResult((current) => setZoneType(current, zoneId, type));
    editSession.markChanged(getText().storedZoneUpdated, ["config"]);
  }

  function setSelectedCalibrationZoneType(type: StoredCalibrationZoneType): void {
    const zoneId = editSession.selectedRadarZoneId;
    if (!zoneId || getTool() !== "calibration") return;
    editSession.pushHistory();
    updateConfigWithResult((current) => setCalibrationZoneType(current, zoneId, type));
    editSession.markChanged(getText().storedCalibrationUpdated, ["config"]);
  }

  function convertSelectedZoneToRect(): void {
    const zoneId = editSession.selectedRadarZoneId;
    if (!zoneId || getTool() !== "zones") return;
    const current = selectedZone();
    if (current?.type === "detection" && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.pushHistory();
    const result = updateConfigWithResult((current) => convertZoneToRectInConfig(current, zoneId));
    if (!result?.changed) return;
    editSession.selectedRadarZonePointIndex = -1;
    editSession.markChanged(getText().storedZoneRectified, ["config"]);
  }

  function moveSelectedZone(source: StoredRadarZoneSource, zone: WebZone, startPoint: [number, number], currentPoint: [number, number]): void {
    if (source === "zones" && zone.type === "detection" && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.beginHistoryAction(`zone-move:${source}:${zone.id}`);
    const start = { x: startPoint[0], y: startPoint[1] };
    const current = { x: currentPoint[0], y: currentPoint[1] };
    updateZone(source, zone.id, () => (
      source === "zones" && zone.type === "exit"
        ? moveExitLineZone(zone, start, current)
        : moveZone(zone, start, current)
    ));
  }

  function moveSelectedZonePoint(source: StoredRadarZoneSource, zone: WebZone, pointIndex: number, point: [number, number]): void {
    if (source === "zones" && zone.type === "detection" && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    editSession.beginHistoryAction(`zone-point:${source}:${zone.id}:${pointIndex}`);
    const radarPoint: RadarScreenPoint = { x: point[0], y: point[1] };
    if (source === "zones" && zone.type === "exit") {
      if (pointIndex !== 0 && pointIndex !== 1) return;
      updateZone(source, zone.id, (current) => updateExitLineEndpoint(current, pointIndex, radarPoint));
      return;
    }
    if (pointIndex < 0 || pointIndex >= zone.points.length) return;
    updateZone(source, zone.id, (current) => updateZonePoint(current, pointIndex, radarPoint));
  }

  function addZonePoint(source: StoredRadarZoneSource, zone: WebZone, edgeIndex: number, point: [number, number]): void {
    if (source !== "zones") return;
    if (zone.type === "detection" && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return;
    }
    if (zone.type === "exit") return;
    editSession.pushHistory();
    const result = updateConfigWithResult((current) => insertZonePointInConfig(current, zone.id, edgeIndex, { x: point[0], y: point[1] }));
    if (!result) return;
    const text = getText();
    if (!result.changed) {
      setMutationError(result.messageCode, result.messageParams, text.storedAddPointUnavailable);
      return;
    }
    selectZone(result.selectedZoneId ?? zone.id, result.selectedPointIndex ?? -1);
    editSession.markChanged(text.storedZonePointAdded, ["config"]);
  }

  function commitZoneEdit(_source: StoredRadarZoneSource, zoneId: string): void {
    selectZone(zoneId, editSession.selectedRadarZonePointIndex);
    editSession.finishHistoryAction();
  }

  function deleteSelectedZonePoint(): boolean {
    if (getMode() !== "edit" || getTool() !== "zones") return false;
    const zoneId = editSession.selectedRadarZoneId;
    const pointIndex = editSession.selectedRadarZonePointIndex;
    if (!zoneId || pointIndex < 0) return false;
    const current = selectedZone();
    if (current?.type === "detection" && !canUseDetectionType()) {
      onDetectionOwnerRequired();
      return false;
    }
    editSession.pushHistory();
    const result = updateConfigWithResult((current) => deleteZonePointInConfig(current, zoneId, pointIndex));
    if (!result?.changed) return false;
    editSession.selectedRadarZonePointIndex = result.selectedPointIndex ?? -1;
    editSession.markChanged(getText().storedZonePointDeleted, ["config"]);
    return true;
  }

  return {
    editSource,
    toolZones,
    overlayZones,
    selectedZone,
    selectZone,
    beginTool,
    addZone,
    deleteSelectedZone,
    renameSelectedZone,
    setSelectedZoneType,
    setSelectedCalibrationZoneType,
    convertSelectedZoneToRect,
    moveZone: moveSelectedZone,
    moveZonePoint: moveSelectedZonePoint,
    addZonePoint,
    commitZoneEdit,
    deleteSelectedZonePoint
  };
}
