import { boundsFromPoints, rectPoints } from "../../core/geometry";
import type { RadarScreenPoint } from "../../core/types";
import { clampZoneToHardwareBounds, isCalibrationZoneId } from "../../core/zones";
import { radarPointFromEvent } from "../canvas/radar-view";
import type { WebDeviceConfig, WebZone } from "../types";
import {
  convertZoneToRectInConfig,
  deleteZonePointInConfig,
  insertZonePointInConfig,
  moveZone,
  updateZonePoint
} from "../zone-geometry";

type DragState = {
  zoneId: string;
  source: "zone" | "calibration";
  mode: "move" | "resize";
  pointIndex?: number;
  pointerId: number;
  svg: SVGSVGElement;
  startPoint: RadarScreenPoint;
  startClientX: number;
  startClientY: number;
  startZone: WebZone;
  moved: boolean;
};

type PendingShrink = {
  zoneId: string;
  nextZone: WebZone;
  startZone: WebZone;
};

type StatusTone = "ok" | "warn" | "error";

interface RadarInteractionOptions {
  getConfig: () => WebDeviceConfig | null;
  getZones: () => WebZone[];
  getCalibrationZones: () => WebZone[];
  getSelectedZone: () => WebZone | null;
  getSelectedPointIndex: () => number;
  setSelectedPointIndex: (pointIndex: number) => void;
  updateConfig: (mutator: (current: WebDeviceConfig) => WebDeviceConfig, save?: boolean, history?: boolean) => void;
  pushHistory: () => void;
  scheduleSave: () => void;
  selectZone: (zoneId: string, resetPoint?: boolean, render?: boolean) => void;
  renderSceneNow: () => void;
  setStatus: (message: string, tone: StatusTone) => void;
}

const CALIBRATION_SHRINK_WARNING_MARGIN_MM = 50;
const POINT_DRAG_CLICK_TOLERANCE_PX = 4;
const POINT_DRAG_DOUBLE_CLICK_SUPPRESS_MS = 350;
const POINT_INSERT_DOUBLE_CLICK_SUPPRESS_MS = 700;

export function createRadarInteraction({
  getConfig,
  getZones,
  getCalibrationZones,
  getSelectedZone,
  getSelectedPointIndex,
  setSelectedPointIndex,
  updateConfig,
  pushHistory,
  scheduleSave,
  selectZone,
  renderSceneNow,
  setStatus
}: RadarInteractionOptions) {
  let drag: DragState | null = null;
  let suppressNextRadarClick = false;
  let protectedZoneDialogOpen = $state(false);
  let shrinkConfirmZoneId = $state("");
  let shrinkWarningShownZoneId = "";
  let pendingShrink: PendingShrink | null = null;
  let suppressPointDoubleClickUntil = 0;
  let suppressInsertedPointDoubleClick:
    | {
        zoneId: string;
        pointIndex: number;
        until: number;
      }
    | null = null;

  function resetShrinkWarning(): void {
    shrinkWarningShownZoneId = "";
  }

  function convertSelectedZoneToRect(): void {
    const selectedZone = getSelectedZone();
    if (!selectedZone || selectedZone.points.length < 3) return;
    updateConfig((current) => convertZoneToRectInConfig(current, selectedZone.id).config);
    setSelectedPointIndex(-1);
    renderSceneNow();
  }

  function handleRadarPointerDown(event: PointerEvent): void {
    const config = getConfig();
    if (!config) return;
    const target = event.target as SVGElement | null;
    const dragMode = target?.dataset.zoneDrag as "move" | "resize" | undefined;
    const zoneId = target?.dataset.zoneId;
    const svg = target?.closest("svg") as SVGSVGElement | null;
    if (!target || !dragMode || !zoneId || !svg) return;

    const source: "zone" | "calibration" = isCalibrationZoneId(zoneId) ? "calibration" : "zone";
    if (source === "calibration" && dragMode !== "resize") return;
    const zone =
      source === "calibration"
        ? getCalibrationZones().find((item) => item.id === zoneId)
        : getZones().find((item) => item.id === zoneId);
    if (!zone) return;

    event.preventDefault();
    pushHistory();
    target.setPointerCapture?.(event.pointerId);
    selectZone(zoneId, false, false);
    const pointIndex =
      dragMode === "resize" && target.dataset.zonePoint !== undefined ? Number(target.dataset.zonePoint) : -1;
    setSelectedPointIndex(pointIndex);
    drag = {
      zoneId,
      source,
      mode: dragMode,
      pointIndex: pointIndex >= 0 ? pointIndex : undefined,
      pointerId: event.pointerId,
      svg,
      startPoint: radarPointFromEvent(event, svg),
      startClientX: event.clientX,
      startClientY: event.clientY,
      startZone: cloneZone(zone),
      moved: false
    };
  }

  function handlePointerMove(event: PointerEvent): void {
    const config = getConfig();
    if (!drag || !config || event.pointerId !== drag.pointerId) return;
    event.preventDefault();
    const clientDx = event.clientX - drag.startClientX;
    const clientDy = event.clientY - drag.startClientY;
    if (Math.hypot(clientDx, clientDy) > POINT_DRAG_CLICK_TOLERANCE_PX) {
      drag.moved = true;
    }
    const point = radarPointFromEvent(event, drag.svg);
    const nextZone =
      drag.source === "calibration"
        ? resizeCalibrationZone(drag.startZone, drag.pointIndex, point, Boolean(drag.startZone.minSizeUnlocked))
        : drag.mode === "move"
          ? moveZone(drag.startZone, drag.startPoint, point)
          : updateZonePoint(drag.startZone, drag.pointIndex, point);

    if (
      drag.source === "calibration" &&
      !drag.startZone.minSizeUnlocked &&
      calibrationResizeShrinks(drag.startZone, drag.pointIndex, point) &&
      shrinkWarningShownZoneId !== drag.zoneId
    ) {
      shrinkWarningShownZoneId = drag.zoneId;
      shrinkConfirmZoneId = drag.zoneId;
      pendingShrink = {
        zoneId: drag.zoneId,
        nextZone: resizeCalibrationZone(drag.startZone, drag.pointIndex, point, true),
        startZone: cloneZone(drag.startZone)
      };
      restoreCalibrationZone(drag.startZone);
      drag = null;
      renderSceneNow();
      return;
    }

    updateConfig(
      (current) =>
        drag?.source === "calibration"
          ? {
              ...current,
              calibrationZones: (current.calibrationZones || []).map((zone) =>
                zone.id === nextZone.id ? nextZone : zone
              )
            }
          : {
              ...current,
              zones: current.zones.map((zone) => (zone.id === nextZone.id ? nextZone : zone))
            },
      false
    );
  }

  function handlePointDoubleClick(event: MouseEvent): void {
    if (Date.now() < suppressPointDoubleClickUntil) {
      event.preventDefault();
      event.stopPropagation();
      return;
    }
    const target = event.target as SVGElement | null;
    const zoneId = target?.dataset.zoneId;
    const pointIndex = Number(target?.dataset.zonePoint);
    if (!zoneId || !Number.isInteger(pointIndex) || isCalibrationZoneId(zoneId)) return;
    if (
      suppressInsertedPointDoubleClick &&
      Date.now() < suppressInsertedPointDoubleClick.until &&
      suppressInsertedPointDoubleClick.zoneId === zoneId &&
      suppressInsertedPointDoubleClick.pointIndex === pointIndex
    ) {
      event.preventDefault();
      event.stopPropagation();
      return;
    }
    event.preventDefault();
    selectZone(zoneId, false);
    setSelectedPointIndex(pointIndex);
    deleteSelectedPoint();
  }

  function handlePointerUp(): void {
    if (!drag) return;
    const finishedDrag = drag;
    suppressNextRadarClick = true;
    if (finishedDrag.mode === "resize" && finishedDrag.moved) {
      suppressPointDoubleClickUntil = Date.now() + POINT_DRAG_DOUBLE_CLICK_SUPPRESS_MS;
    }
    drag = null;
    renderSceneNow();
    scheduleSave();
    window.setTimeout(() => {
      suppressNextRadarClick = false;
    }, 0);
  }

  function handleRadarClick(event: MouseEvent): void {
    if (suppressNextRadarClick) {
      event.preventDefault();
      return;
    }
    const target = event.target as SVGElement | null;
    const zoneId = target?.dataset.zoneId;
    const calibrationId = target?.dataset.calibrationInfo;
    if (drag) return;
    if (target?.dataset.zoneEdge !== undefined || target?.dataset.zonePoint !== undefined) {
      return;
    }
    if (zoneId && target?.dataset.zoneDrag === "move") {
      selectZone(zoneId);
      return;
    }
    if (calibrationId) {
      protectedZoneDialogOpen = true;
      return;
    }
    if (target?.closest("svg")) {
      selectZone("");
    }
  }

  function handleRadarEdgeClick(event: MouseEvent): void {
    const config = getConfig();
    if (!config) return;
    const target = event.target as SVGElement | null;
    const zoneId = target?.dataset.zoneId;
    const edgeIndex = Number(target?.dataset.zoneEdge);
    const svg = target?.closest("svg") as SVGSVGElement | null;
    if (!zoneId || !Number.isInteger(edgeIndex) || !svg) return;

    event.preventDefault();
    event.stopPropagation();
    const point = radarPointFromEvent(event, svg);
    let nextPointIndex = -1;
    updateConfig((current) => {
      const result = insertZonePointInConfig(current, zoneId, edgeIndex, point);
      if (!result.changed && result.message) setStatus(result.message, "error");
      nextPointIndex = result.selectedPointIndex ?? -1;
      return result.config;
    });
    if (nextPointIndex >= 0) {
      setSelectedPointIndex(nextPointIndex);
      suppressInsertedPointDoubleClick = {
        zoneId,
        pointIndex: nextPointIndex,
        until: Date.now() + POINT_INSERT_DOUBLE_CLICK_SUPPRESS_MS
      };
    }
    renderSceneNow();
  }

  function deleteSelectedPoint(): void {
    const selectedZone = getSelectedZone();
    const selectedPointIndex = getSelectedPointIndex();
    if (!selectedZone) return;
    if (selectedPointIndex < 0) return;
    let nextPointIndex = selectedPointIndex;
    updateConfig((current) => {
      const result = deleteZonePointInConfig(current, selectedZone.id, selectedPointIndex);
      if (!result.changed && result.message) setStatus(result.message, "warn");
      nextPointIndex = result.selectedPointIndex ?? nextPointIndex;
      return result.config;
    });
    setSelectedPointIndex(nextPointIndex);
    renderSceneNow();
  }

  function resizeCalibrationZone(
    zone: WebZone,
    pointIndex: number | undefined,
    point: RadarScreenPoint,
    allowShrink: boolean
  ): WebZone {
    if (pointIndex === undefined || pointIndex < 0 || zone.points.length < 4) return zone;
    const bounds = boundsFromPoints(zone.points);
    const minBounds = boundsFromPoints(zone.minPoints && zone.minPoints.length >= 4 ? zone.minPoints : zone.points);
    let { minX, maxX, minY, maxY } = bounds;

    if (pointIndex === 0) {
      minX = allowShrink ? point.x : Math.min(point.x, minBounds.minX);
      minY = allowShrink ? point.y : Math.min(point.y, minBounds.minY);
    } else if (pointIndex === 1) {
      maxX = allowShrink ? point.x : Math.max(point.x, minBounds.maxX);
      minY = allowShrink ? point.y : Math.min(point.y, minBounds.minY);
    } else if (pointIndex === 2) {
      maxX = allowShrink ? point.x : Math.max(point.x, minBounds.maxX);
      maxY = allowShrink ? point.y : Math.max(point.y, minBounds.maxY);
    } else if (pointIndex === 3) {
      minX = allowShrink ? point.x : Math.min(point.x, minBounds.minX);
      maxY = allowShrink ? point.y : Math.max(point.y, minBounds.maxY);
    }

    return clampZoneToHardwareBounds({
      ...zone,
      shape: "rect",
      points: rectPoints(minX, minY, maxX, maxY)
    });
  }

  function calibrationResizeShrinks(
    zone: WebZone,
    pointIndex: number | undefined,
    point: RadarScreenPoint
  ): boolean {
    if (pointIndex === undefined || pointIndex < 0 || zone.points.length < 4) return false;
    const minimum = boundsFromPoints(zone.minPoints && zone.minPoints.length >= 4 ? zone.minPoints : zone.points);
    const resized = boundsFromPoints(resizeCalibrationZone(zone, pointIndex, point, true).points);
    return (
      resized.width < minimum.width - CALIBRATION_SHRINK_WARNING_MARGIN_MM ||
      resized.height < minimum.height - CALIBRATION_SHRINK_WARNING_MARGIN_MM
    );
  }

  function unlockCalibrationMinSize(zoneId: string): void {
    if (!zoneId) return;
    const pending = pendingShrink?.zoneId === zoneId ? pendingShrink : null;
    updateConfig((current) => ({
      ...current,
      calibrationZones: (current.calibrationZones || []).map((zone) =>
        zone.id === zoneId
          ? {
              ...(pending ? pending.nextZone : zone),
              minSizeUnlocked: true
            }
          : zone
      )
    }));
    pendingShrink = null;
    shrinkConfirmZoneId = "";
    renderSceneNow();
  }

  function cancelCalibrationShrink(): void {
    if (pendingShrink) restoreCalibrationZone(pendingShrink.startZone);
    pendingShrink = null;
    shrinkConfirmZoneId = "";
    renderSceneNow();
  }

  function restoreCalibrationZone(zone: WebZone): void {
    updateConfig(
      (current) => ({
        ...current,
        calibrationZones: (current.calibrationZones || []).map((item) =>
          item.id === zone.id ? cloneZone(zone) : item
        )
      }),
      false
    );
  }

  function cloneZone(zone: WebZone): WebZone {
    const points = zone.points.map(([x, y]): [number, number] => [x, y]);
    return {
      ...zone,
      points,
      minPoints:
        zone.minPoints?.map(([x, y]): [number, number] => [x, y]) ?? (isCalibrationZoneId(zone.id) ? points : undefined)
    };
  }

  return {
    get isDragging() {
      return Boolean(drag);
    },
    get protectedZoneDialogOpen() {
      return protectedZoneDialogOpen;
    },
    set protectedZoneDialogOpen(value: boolean) {
      protectedZoneDialogOpen = value;
    },
    get shrinkConfirmZoneId() {
      return shrinkConfirmZoneId;
    },
    cancelCalibrationShrink,
    convertSelectedZoneToRect,
    deleteSelectedPoint,
    handlePointDoubleClick,
    handlePointerMove,
    handlePointerUp,
    handleRadarClick,
    handleRadarEdgeClick,
    handleRadarPointerDown,
    unlockCalibrationMinSize,
    resetShrinkWarning
  };
}
