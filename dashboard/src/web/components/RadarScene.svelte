<script lang="ts">
  import { toScreenPoint } from "../../core/radar-math";
  import { renderGrid } from "../../core/radar-svg";
  import { localizedZoneDisplayName } from "../../core/zones";
  import type { RadarViewport } from "../../core/types";
  import {
    RADAR_SCENE_HEIGHT,
    RADAR_SCENE_PAD,
    RADAR_SCENE_WIDTH,
    radarSceneViewport
  } from "../canvas/radar-view";
  import type { Messages } from "../i18n/types";
  import type { WebDeviceConfig, WebDeviceState, WebTarget, WebZone } from "../types";
  import { exitLineFromZone } from "../zone-geometry";

  interface Props {
    messages: Messages;
    state: WebDeviceState | null;
    config: WebDeviceConfig | null;
    selectedZoneId?: string;
    editable?: boolean;
    selectedPointIndex?: number;
    debugMode?: boolean;
    onCanvasClick?: (event: MouseEvent) => void;
    onZonePointerDown?: (event: PointerEvent) => void;
    onZoneEdgeClick?: (event: MouseEvent) => void;
    onZonePointDoubleClick?: (event: MouseEvent) => void;
    onCalibrationInfoClick?: (zoneId: string) => void;
  }

  let {
    messages,
    state,
    config,
    selectedZoneId = "",
    editable = false,
    selectedPointIndex = -1,
    debugMode = false,
    onCanvasClick,
    onZonePointerDown,
    onZoneEdgeClick,
    onZonePointDoubleClick,
    onCalibrationInfoClick
  }: Props = $props();

  const viewport = $derived(radarSceneViewport());
  const text = $derived(messages.zones);
  const centerX = RADAR_SCENE_WIDTH / 2;
  const bottomY = RADAR_SCENE_HEIGHT - RADAR_SCENE_PAD;

  function renderableTargets(): WebTarget[] {
    return state?.targets.filter((target) => isRenderableTarget(target, debugMode)) ?? [];
  }

  function screenPoint(x: number, y: number) {
    return toScreenPoint(x, y, viewport);
  }

  function zonePoints(zone: WebZone): string {
    return zone.points
      .map(([x, y]) => {
        const point = screenPoint(x, y);
        return `${point.x},${point.y}`;
      })
      .join(" ");
  }

  function zoneScreenPoints(zone: WebZone) {
    return zone.points.map(([x, y]) => screenPoint(x, y));
  }

  function exitLineScreenPoints(zone: WebZone) {
    const line = exitLineFromZone(zone);
    return {
      start: screenPoint(line.start[0], line.start[1]),
      end: screenPoint(line.end[0], line.end[1])
    };
  }

  function zoneClasses(zone: WebZone, calibration = false): string {
    return [
      "web-zone",
      zone.type,
      calibration ? "calibration" : "",
      zone.placeholder ? "placeholder" : "",
      zone.id === selectedZoneId ? "selected" : ""
    ]
      .filter(Boolean)
      .join(" ");
  }

  function zoneLabel(zoneId: string): string {
    const match = /^zone_(\d+)$/.exec(zoneId);
    return match ? text.zoneLabel(match[1]) : zoneId;
  }

  function displayZoneName(zone: WebZone): string {
    return localizedZoneDisplayName(zone, text);
  }

  function targetScreenPoint(target: WebTarget) {
    return screenPoint(targetRadarX(target), targetRadarY(target));
  }

  function targetDistanceM(target: WebTarget): string {
    return (Math.hypot(targetRadarX(target), targetRadarY(target)) / 1000).toFixed(2);
  }

  function targetRadarX(target: WebTarget): number {
    return debugMode && Number.isFinite(target.rawX) ? Number(target.rawX) : target.x;
  }

  function targetRadarY(target: WebTarget): number {
    return debugMode && Number.isFinite(target.rawY) ? Number(target.rawY) : target.y;
  }

  function isRenderableTarget(target: WebTarget, showDebug: boolean): boolean {
    const radarX = showDebug && Number.isFinite(target.rawX) ? Number(target.rawX) : target.x;
    const radarY = showDebug && Number.isFinite(target.rawY) ? Number(target.rawY) : target.y;
    const active = target.active || (showDebug && Boolean(target.rawActive || target.filtered));
    return active && Number.isFinite(radarX) && Number.isFinite(radarY) && Math.hypot(radarX, radarY) > 100;
  }

  function isFilteredTarget(target: WebTarget): boolean {
    return Boolean(target.filtered || (!target.active && target.rawActive));
  }

  function stopKey(event: KeyboardEvent): void {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    event.stopPropagation();
  }
</script>

{#if state && config}
  <svg
    class="radar-scene"
    viewBox={`0 0 ${RADAR_SCENE_WIDTH} ${RADAR_SCENE_HEIGHT}`}
    role="img"
    aria-label={text.radarAria}
  >
    <rect
      class="radar-scene-hit"
      x="0"
      y="0"
      width={RADAR_SCENE_WIDTH}
      height={RADAR_SCENE_HEIGHT}
      role="button"
      tabindex="0"
      aria-label={text.clearSelectionAria}
      onclick={(event) => onCanvasClick?.(event)}
      onkeydown={stopKey}
    />
    {@html renderGrid(viewport as RadarViewport)}

    {#each config.calibrationZones || [] as zone (zone.id)}
      {#if zone.points.length && (!zone.placeholder || zone.id === selectedZoneId)}
        {@const points = zoneScreenPoints(zone)}
        {@const labelPoint = points[0]}
        <g class={zoneClasses(zone, true)}>
          <polygon
            points={zonePoints(zone)}
            data-zone-id={zone.id}
            data-calibration-info={zone.id}
            role="button"
            tabindex="0"
            aria-label={text.calibrationInfoAria}
            onclick={() => onCalibrationInfoClick?.(zone.id)}
            onkeydown={stopKey}
          ></polygon>
          <text x={labelPoint.x + 8} y={labelPoint.y - 8}>
            <tspan x={labelPoint.x + 8} dy="0">{zoneLabel(zone.id)}</tspan>
            {#if !zone.placeholder && displayZoneName(zone)}
              <tspan x={labelPoint.x + 8} dy="14">{displayZoneName(zone)}</tspan>
            {/if}
          </text>
          {#if editable && zone.id === selectedZoneId}
            {#each points as point, index}
              <circle
                class:selected={index === selectedPointIndex}
                class="zone-handle"
                cx={point.x}
                cy={point.y}
                r="7"
                data-zone-drag="resize"
                data-zone-id={zone.id}
                data-zone-point={index}
                role="button"
                tabindex="0"
                aria-label={text.calibrationPointAria}
                onpointerdown={(event) => onZonePointerDown?.(event)}
                ondblclick={(event) => onZonePointDoubleClick?.(event)}
                onkeydown={stopKey}
              />
            {/each}
          {/if}
        </g>
      {/if}
    {/each}

    {#each config.zones as zone (zone.id)}
      {#if zone.points.length && (!zone.placeholder || zone.id === selectedZoneId)}
        {@const points = zoneScreenPoints(zone)}
        {@const exitLine = zone.type === "exit" ? exitLineScreenPoints(zone) : null}
        {@const labelPoint = points[0]}
        <g class={zoneClasses(zone)}>
          {#if exitLine}
            <line
              class="exit-line-display"
              x1={exitLine.start.x}
              y1={exitLine.start.y}
              x2={exitLine.end.x}
              y2={exitLine.end.y}
              data-zone-drag={editable ? "move" : undefined}
              data-zone-id={zone.id}
              role="button"
              tabindex="0"
              aria-label={text.zoneMoveAria}
              onpointerdown={(event) => onZonePointerDown?.(event)}
              onkeydown={stopKey}
            />
            <line
              class="exit-line-hit"
              x1={exitLine.start.x}
              y1={exitLine.start.y}
              x2={exitLine.end.x}
              y2={exitLine.end.y}
              data-zone-drag={editable ? "move" : undefined}
              data-zone-id={zone.id}
              role="button"
              tabindex="0"
              aria-label={text.zoneMoveAria}
              onpointerdown={(event) => onZonePointerDown?.(event)}
              onkeydown={stopKey}
            />
          {:else}
            <polygon
              points={zonePoints(zone)}
              data-zone-drag={editable ? "move" : undefined}
              data-zone-id={zone.id}
              role="button"
              tabindex="0"
              aria-label={text.zoneMoveAria}
              onpointerdown={(event) => onZonePointerDown?.(event)}
              onkeydown={stopKey}
            ></polygon>
          {/if}
          {#if editable && zone.id === selectedZoneId && !exitLine}
            {#each points as point, index}
              {@const next = points[(index + 1) % points.length]}
              <line
                class="zone-edge-hit"
                x1={point.x}
                y1={point.y}
                x2={next.x}
                y2={next.y}
                data-zone-id={zone.id}
                data-zone-edge={index}
                role="button"
                tabindex="0"
                aria-label={text.zoneEdgeAddAria}
                onclick={(event) => onZoneEdgeClick?.(event)}
                onkeydown={stopKey}
              />
            {/each}
          {/if}
          <text x={(exitLine?.start.x ?? labelPoint.x) + 8} y={(exitLine?.start.y ?? labelPoint.y) - 8}>
            <tspan x={(exitLine?.start.x ?? labelPoint.x) + 8} dy="0">{zoneLabel(zone.id)}</tspan>
            {#if !zone.placeholder && displayZoneName(zone)}
              <tspan x={(exitLine?.start.x ?? labelPoint.x) + 8} dy="14">{displayZoneName(zone)}</tspan>
            {/if}
          </text>
          {#if editable && zone.id === selectedZoneId && exitLine}
            <circle
              class:selected={selectedPointIndex === 0}
              class="zone-handle exit-line-handle"
              cx={exitLine.start.x}
              cy={exitLine.start.y}
              r="8"
              data-zone-drag="resize"
              data-zone-id={zone.id}
              data-zone-point={0}
              role="button"
              tabindex="0"
              aria-label={text.zonePointAria}
              onpointerdown={(event) => onZonePointerDown?.(event)}
              onkeydown={stopKey}
            />
            <circle
              class:selected={selectedPointIndex === 1}
              class="zone-handle exit-line-handle"
              cx={exitLine.end.x}
              cy={exitLine.end.y}
              r="8"
              data-zone-drag="resize"
              data-zone-id={zone.id}
              data-zone-point={1}
              role="button"
              tabindex="0"
              aria-label={text.zonePointAria}
              onpointerdown={(event) => onZonePointerDown?.(event)}
              onkeydown={stopKey}
            />
          {:else if editable && zone.id === selectedZoneId}
            {#each points as point, index}
              <circle
                class:selected={index === selectedPointIndex}
                class="zone-handle"
                cx={point.x}
                cy={point.y}
                r="7"
                data-zone-drag="resize"
                data-zone-id={zone.id}
                data-zone-point={index}
                role="button"
                tabindex="0"
                aria-label={text.zonePointAria}
                onpointerdown={(event) => onZonePointerDown?.(event)}
                ondblclick={(event) => onZonePointDoubleClick?.(event)}
                onkeydown={stopKey}
              />
            {/each}
          {/if}
        </g>
      {/if}
    {/each}

    <polygon
      class="sensor"
      points={`${centerX},${bottomY - 12} ${centerX - 10},${bottomY + 8} ${centerX + 10},${bottomY + 8}`}
    />

    {#each renderableTargets() as target (target.id)}
      {@const point = targetScreenPoint(target)}
      <g
        class={`target${isFilteredTarget(target) ? " filtered" : ""}${target.reduced ? " reduced" : ""}`}
        style={`--target-color:${target.color}`}
      >
        <circle cx={point.x} cy={point.y} r="9"></circle>
        <text x={point.x} y={point.y - 30}>
          <tspan x={point.x} dy="0">{target.name}</tspan>
          <tspan x={point.x} dy="14">{targetDistanceM(target)} m</tspan>
          {#if debugMode && isFilteredTarget(target)}
            <tspan x={point.x} dy="14">FILTER {target.filterReason || ""}</tspan>
          {/if}
        </text>
      </g>
    {/each}
  </svg>
{:else}
  <div class="svelte-shell-placeholder">
    <strong>{text.sceneLoadingTitle}</strong>
    <span>{text.sceneLoadingDescription}</span>
  </div>
{/if}
