import { DEFAULT_CARD_CONFIG } from "../../core/defaults";
import { LD2450_FOV_DEGREES, radarViewportRangeX, toRadarPoint } from "../../core/radar-math";
import type { RadarScreenPoint, RadarViewport } from "../../core/types";

export const RADAR_SCENE_WIDTH = 760;
export const RADAR_SCENE_HEIGHT = 540;
export const RADAR_SCENE_PAD = 34;

export function radarSceneViewport(): RadarViewport {
  const rangeY = DEFAULT_CARD_CONFIG.range_y;
  const specRangeX = radarViewportRangeX(rangeY, LD2450_FOV_DEGREES);
  return {
    width: RADAR_SCENE_WIDTH,
    height: RADAR_SCENE_HEIGHT,
    pad: RADAR_SCENE_PAD,
    rangeX: Math.max(DEFAULT_CARD_CONFIG.range_x, specRangeX),
    rangeY,
    fovDegrees: LD2450_FOV_DEGREES
  };
}

export function radarPointFromEvent(event: PointerEvent | MouseEvent, svg: SVGSVGElement): RadarScreenPoint {
  const rect = svg.getBoundingClientRect();
  const screenX = ((event.clientX - rect.left) / rect.width) * RADAR_SCENE_WIDTH;
  const screenY = ((event.clientY - rect.top) / rect.height) * RADAR_SCENE_HEIGHT;
  const viewport = radarSceneViewport();
  const point = toRadarPoint(screenX, screenY, viewport);
  return {
    x: clamp(Math.round(point.x), -viewport.rangeX, viewport.rangeX),
    y: clamp(Math.round(point.y), 0, viewport.rangeY)
  };
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}
