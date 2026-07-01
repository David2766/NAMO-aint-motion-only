import { distanceLabel, isPointInRadarCoverage, pointAtPolar, toScreenPoint } from "./radar-math";
import { escapeHtml } from "./html";
import { targetColor } from "./defaults";
import type { HeldRadarTarget, RadarCardConfig, RadarViewport, RadarZoneDisplay } from "./types";

const SAFE_CSS_HEX_COLOR = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

export function renderGrid(viewport: RadarViewport): string {
  const centerX = viewport.width / 2;
  const bottomY = viewport.height - viewport.pad;
  const halfFov = viewport.fovDegrees / 2;
  const beamArc = sampledArcPath(viewport.rangeY, -halfFov, halfFov, viewport, false);
  const angleLines = [-60, -30, 0, 30, 60]
    .filter((angle) => Math.abs(angle) <= halfFov)
    .map((angle) => {
      const end = toScreenPoint(...polarTuple(viewport.rangeY, angle), viewport);
      const labelPoint = toScreenPoint(...polarTuple(viewport.rangeY * 0.92, angle), viewport);
      return `
        <line x1="${centerX}" y1="${bottomY}" x2="${end.x}" y2="${end.y}" />
        <text class="angle-label" x="${labelPoint.x}" y="${labelPoint.y}">${angle}°</text>
      `;
    })
    .join("");
  const distanceArcs = distanceTicks(viewport.rangeY)
    .map((distance) => {
      const labelPoint = toScreenPoint(0, distance, viewport);
      return `
        <path d="${sampledArcPath(distance, -halfFov, halfFov, viewport)}" />
        <text class="distance-label" x="${labelPoint.x + 5}" y="${labelPoint.y - 5}">${distance / 1000}m</text>
      `;
    })
    .join("");

  return `
    <path class="beam" d="M ${centerX} ${bottomY} L ${beamArc} Z" />
    <g class="grid">${angleLines}${distanceArcs}</g>
  `;
}

export function renderTargets(
  targets: HeldRadarTarget[],
  config: RadarCardConfig,
  viewport: RadarViewport,
  now = Date.now()
): string {
  return targets
    .map((target) => {
      const age = now - target.lastSeen;
      const point = toScreenPoint(target.x, target.y, viewport);
      const inCoverage = isPointInRadarCoverage(target.x, target.y, viewport);
      const opacity = (target.active ? 1 : Math.max(0, 1 - age / config.hold_ms)) * (inCoverage ? 1 : 0.35);
      const distance = distanceLabel(
        target.x,
        target.y,
        config.show_distance,
        config.distance_decimals
      );
      const label = escapeHtml(target.name);
      const color = safeTargetColor(target.color);

      return `
        <g class="target${inCoverage ? "" : " out-of-coverage"}" style="--target-color:${color}; opacity:${opacity}">
          <circle cx="${point.x}" cy="${point.y}" r="9"></circle>
          <text x="${point.x}" y="${point.y - 18}">
            <tspan x="${point.x}" dy="0">${label}</tspan>
            ${distance ? `<tspan x="${point.x}" dy="14">${distance}</tspan>` : ""}
          </text>
        </g>
      `;
    })
    .join("");
}

export function safeTargetColor(value: string): string {
  const color = value.trim();
  return SAFE_CSS_HEX_COLOR.test(color) ? color : targetColor(0);
}

function distanceTicks(rangeY: number): number[] {
  const ticks: number[] = [];
  for (let distance = 1000; distance <= rangeY; distance += 1000) {
    ticks.push(distance);
  }
  return ticks;
}

function sampledArcPath(
  distance: number,
  startAngle: number,
  endAngle: number,
  viewport: RadarViewport,
  includeMove = true
): string {
  const points: string[] = [];
  const steps = 36;
  for (let index = 0; index <= steps; index += 1) {
    const angle = startAngle + ((endAngle - startAngle) * index) / steps;
    const point = toScreenPoint(...polarTuple(distance, angle), viewport);
    const command = index === 0 ? (includeMove ? "M " : "") : "L ";
    points.push(`${command}${point.x} ${point.y}`);
  }
  return points.join(" ");
}

function polarTuple(distance: number, angleDegrees: number): [number, number] {
  const point = pointAtPolar(distance, angleDegrees);
  return [point.x, point.y];
}

export function renderZoneRects(
  zones: RadarZoneDisplay[],
  viewport: RadarViewport,
  editable = false
): string {
  return zones.map((zone) => renderZoneRect(zone, viewport, editable)).join("");
}

function renderZoneRect(zone: RadarZoneDisplay, viewport: RadarViewport, editable: boolean): string {
  const left = Math.min(zone.x1, zone.x2);
  const right = Math.max(zone.x1, zone.x2);
  const near = Math.min(zone.y1, zone.y2);
  const far = Math.max(zone.y1, zone.y2);
  if (left === right || near === far) return "";

  const topLeft = toScreenPoint(left, far, viewport);
  const bottomRight = toScreenPoint(right, near, viewport);
  const width = bottomRight.x - topLeft.x;
  const height = bottomRight.y - topLeft.y;
  const labelX = topLeft.x + width / 2;
  const labelY = topLeft.y + Math.max(18, Math.min(height / 2, 34));
  const customName = zone.customName?.trim();
  const handles = editable && zone.selected ? renderZoneHandles(zone, viewport) : "";

  const zoneType = zone.type === "Filter" ? "filter" : zone.type === "Disabled" ? "disabled" : "detection";

  return `
    <g class="zone-rect ${zoneType}${zone.selected ? " selected" : ""}${zone.placeholder ? " placeholder" : ""}">
      <rect
        x="${topLeft.x}"
        y="${topLeft.y}"
        width="${width}"
        height="${height}"
        ${editable ? `data-zone-drag="move" data-zone-id="${zone.zoneId}"` : ""}
      />
      <text x="${labelX}" y="${labelY}">
        <tspan x="${labelX}" dy="0">${escapeHtml(zone.label)}</tspan>
        ${customName ? `<tspan x="${labelX}" dy="14">${escapeHtml(customName)}</tspan>` : ""}
      </text>
      ${handles}
    </g>
  `;
}

function renderZoneHandles(zone: RadarZoneDisplay, viewport: RadarViewport): string {
  const corners = [
    ["x1y1", zone.x1, zone.y1],
    ["x1y2", zone.x1, zone.y2],
    ["x2y1", zone.x2, zone.y1],
    ["x2y2", zone.x2, zone.y2]
  ] as const;

  return corners
    .map(([corner, x, y]) => {
      const point = toScreenPoint(x, y, viewport);
      return `
        <circle
          class="zone-handle"
          cx="${point.x}"
          cy="${point.y}"
          r="7"
          data-zone-drag="resize"
          data-zone-id="${zone.zoneId}"
          data-zone-corner="${corner}"
        />
      `;
    })
    .join("");
}
