import {
  addSoftwareZone,
  deleteZone,
  renameZone,
  setZoneType
} from "../zone-geometry";
import type { WebDeviceConfig, WebZone, WebZoneType } from "../types";

type StatusTone = "ok" | "warn" | "error";

interface ZoneEditorOptions {
  getConfig: () => WebDeviceConfig | null;
  getZones: () => WebZone[];
  getCalibrationZones: () => WebZone[];
  updateConfig: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
  setStatus: (message: string, tone: StatusTone) => void;
  onSelect: (zoneId: string, resetPoint: boolean, render: boolean) => void;
}

export function createZoneEditor({
  getConfig,
  getZones,
  getCalibrationZones,
  updateConfig,
  setStatus,
  onSelect
}: ZoneEditorOptions) {
  let selectedZoneId = $state("");

  const selectedZone = $derived(getZones().find((zone) => zone.id === selectedZoneId) ?? null);
  const selectedCalibrationZone = $derived(
    getCalibrationZones().find((zone) => zone.id === selectedZoneId) ?? null
  );

  function selectZone(zoneId: string, resetPoint = true, render = true): void {
    selectedZoneId = zoneId;
    onSelect(zoneId, resetPoint, render);
  }

  function selectFirstAvailable(): void {
    selectZone(getZones()[0]?.id || getCalibrationZones()[0]?.id || "");
  }

  function addZone(): void {
    const config = getConfig();
    if (!config) return;
    const result = addSoftwareZone(config);
    if (!result.changed) {
      if (result.message) setStatus(result.message, "warn");
      return;
    }
    updateConfig(() => result.config);
    selectZone(result.selectedZoneId ?? "");
  }

  function deleteSelected(): void {
    const config = getConfig();
    if (!config || !selectedZoneId) return;
    const result = deleteZone(config, selectedZoneId);
    if (!result.changed) return;
    updateConfig(() => result.config);
    selectZone(result.selectedZoneId ?? "");
  }

  function setSelectedZoneType(type: WebZoneType): void {
    if (!selectedZone) return;
    updateConfig((current) => setZoneType(current, selectedZone.id, type).config);
  }

  function setSelectedZoneName(name: string): void {
    if (!selectedZone) return;
    updateConfig((current) => renameZone(current, selectedZone.id, name).config);
  }

  return {
    get selectedZoneId() {
      return selectedZoneId;
    },
    get selectedZone() {
      return selectedZone;
    },
    get selectedCalibrationZone() {
      return selectedCalibrationZone;
    },
    addZone,
    deleteSelected,
    selectFirstAvailable,
    selectZone,
    setSelectedZoneName,
    setSelectedZoneType
  };
}
