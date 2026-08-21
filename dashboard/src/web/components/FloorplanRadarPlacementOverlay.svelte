<script>
  import { onMount } from "svelte";
  import { DEFAULT_CARD_CONFIG } from "../../core/defaults";
  import {
    floorplanPxToRadarPoint,
    radarPointToFloorplanPx,
    radarZoneToFloorplanPolygon
  } from "../../core/floorplan/radar-floorplan-transform";
  import { LD2450_FOV_DEGREES, radarViewportRangeX } from "../../core/radar-math";
  import { clampRadarEditZonePoint } from "../canvas/radar-view";
  import {
    isFloorplanOcclusionSegmentIgnored,
    radarVisiblePolygonPoints
  } from "../floorplan/floorplan-occlusion";
  import { exitLineFromZone } from "../zone-geometry";

  let {
    transformStyle = "",
    imageWidth = 1,
    imageHeight = 1,
    viewBounds = null,
    scaleEstimate = null,
    placement = null,
    scalePercent = 100,
    zones = [],
    calibrationZones = [],
    targets = [],
    peerRadars = [],
    peerRadarLabel = "",
    fusionLabel = "",
    wallSegments = [],
    occlusionSegments = [],
    ignoredOcclusionSegmentIds = [],
    occlusionEditActive = false,
    editableZoneSource = "",
    selectedZoneId = "",
    selectedZonePointIndex = -1,
    readOnly = false,
    showPlacementLabel = true,
    rangeY = DEFAULT_CARD_CONFIG.range_y,
    fovDegrees = LD2450_FOV_DEGREES,
    onChange,
    onCommit,
    onSelectZone,
    onZoneMove,
    onZonePointMove,
    onZoneEdgeClick,
    onZoneEditCommit,
    onToggleOcclusionSegment,
    zoneEditable = () => true
  } = $props();

  let drag = $state(null);
  const REMOTE_RANGE_START_MM = 6000;
  const RADAR_WALL_CLEARANCE_PX = 10;
  const RADAR_WALL_DETECT_RADIUS_PX = 12;
  const RADAR_WALL_SEGMENT_MARGIN_PX = 8;
  const RADAR_ORIGIN_RADIUS_PX = 8;
  const RADAR_VISIBILITY_RAY_STEP_DEGREES = 0.5;
  const radarCoverageClipId = `radar-coverage-clip-${Math.random().toString(36).slice(2)}`;
  const radarReachableMaskId = `radar-reachable-mask-${Math.random().toString(36).slice(2)}`;
  let radarLayer = $state(null);
  let screenUnitScale = $state(1);

  function viewX() {
    return viewBounds?.x ?? 0;
  }

  function viewY() {
    return viewBounds?.y ?? 0;
  }

  function viewWidth() {
    return viewBounds?.width ?? imageWidth;
  }

  function viewHeight() {
    return viewBounds?.height ?? imageHeight;
  }

  function updateScreenUnitScale() {
    if (!radarLayer) {
      screenUnitScale = 1;
      return;
    }
    const bounds = radarLayer.getBoundingClientRect();
    if (bounds.width <= 0 || bounds.height <= 0) return;
    screenUnitScale = Math.max(viewWidth() / bounds.width, viewHeight() / bounds.height);
  }

  function screenSize(screenPixels) {
    return screenPixels * screenUnitScale;
  }

  onMount(() => {
    updateScreenUnitScale();
    const observer = typeof ResizeObserver === "undefined"
      ? null
      : new ResizeObserver(updateScreenUnitScale);
    observer?.observe(radarLayer);
    window.addEventListener("resize", updateScreenUnitScale);
    return () => {
      observer?.disconnect();
      window.removeEventListener("resize", updateScreenUnitScale);
    };
  });

  $effect(() => {
    const refreshKey = `${imageWidth}:${imageHeight}:${viewX()}:${viewY()}:${viewWidth()}:${viewHeight()}:${transformStyle}`;
    queueMicrotask(() => {
      void refreshKey;
      updateScreenUnitScale();
    });
  });

  function effectivePlacement() {
    if (placement) return placement;
    const bounds = scaleEstimate?.outerBounds;
    return {
      originX: bounds ? bounds.x + bounds.width / 2 : imageWidth / 2,
      originY: bounds ? bounds.y + bounds.height - 16 : imageHeight * 0.82,
      rotation: 0
    };
  }

  function scaleFactor() {
    return Math.max(0.95, Math.min(1.05, Number(scalePercent) / 100 || 1));
  }

  function transformOptions() {
    if (!scaleEstimate) return null;
    return {
      placement: effectivePlacement(),
      scale: scaleEstimate,
      scaleFactor: scaleFactor()
    };
  }

  function radarEditBounds() {
    const editRangeY = Number.isFinite(rangeY) && rangeY > 0 ? Number(rangeY) : DEFAULT_CARD_CONFIG.range_y;
    const editFovDegrees = Number.isFinite(fovDegrees) && fovDegrees > 0 ? Number(fovDegrees) : LD2450_FOV_DEGREES;
    return {
      rangeX: Math.max(DEFAULT_CARD_CONFIG.range_x, radarViewportRangeX(editRangeY, editFovDegrees)),
      rangeY: editRangeY
    };
  }

  function radarEditPointFromImagePoint(point, options) {
    return clampRadarEditZonePoint(floorplanPxToRadarPoint(point, options), radarEditBounds());
  }

  function imagePointFromEvent(event) {
    const svg = event.currentTarget.closest("svg") ?? event.currentTarget;
    const rect = svg.getBoundingClientRect();
    if (!rect.width || !rect.height) return { x: 0, y: 0 };
    return {
      x: viewX() + ((event.clientX - rect.left) / rect.width) * viewWidth(),
      y: viewY() + ((event.clientY - rect.top) / rect.height) * viewHeight()
    };
  }

  function rotatedPoint(radarX, radarY) {
    const options = transformOptions();
    if (!options) {
      const current = effectivePlacement();
      return { x: current.originX, y: current.originY };
    }
    return radarPointToFloorplanPx([radarX, radarY], options);
  }

  function coveragePath() {
    const points = coveragePolygon();
    if (!points.length) return "";
    const [current, ...arcPoints] = points;
    return [
      `M ${current.originX} ${current.originY}`,
      ...arcPoints.map((point) => `L ${point.x} ${point.y}`),
      "Z"
    ].join(" ");
  }

  function coveragePolygon() {
    const current = effectivePlacement();
    if (!scaleEstimate) return [];
    const halfFov = fovDegrees / 2;
    const steps = 24;
    const points = [{ x: current.originX, y: current.originY, originX: current.originX, originY: current.originY }];
    for (let index = 0; index <= steps; index += 1) {
      const angle = -halfFov + (halfFov * 2 * index) / steps;
      const radians = (angle * Math.PI) / 180;
      points.push(rotatedPoint(Math.sin(radians) * rangeY, Math.cos(radians) * rangeY));
    }
    return points;
  }

  function remoteRangeBandPoints() {
    if (!scaleEstimate || rangeY <= REMOTE_RANGE_START_MM) return "";
    const halfFov = fovDegrees / 2;
    const steps = 24;
    const points = [];
    for (let index = 0; index <= steps; index += 1) {
      const angle = -halfFov + (halfFov * 2 * index) / steps;
      const radians = (angle * Math.PI) / 180;
      points.push(rotatedPoint(Math.sin(radians) * rangeY, Math.cos(radians) * rangeY));
    }
    for (let index = steps; index >= 0; index -= 1) {
      const angle = -halfFov + (halfFov * 2 * index) / steps;
      const radians = (angle * Math.PI) / 180;
      points.push(rotatedPoint(Math.sin(radians) * REMOTE_RANGE_START_MM, Math.cos(radians) * REMOTE_RANGE_START_MM));
    }
    return points.map((point) => `${point.x},${point.y}`).join(" ");
  }

  function distanceArcs() {
    const maxDistance = rangeY;
    const step = 1000;
    const arcs = [];
    for (let distance = step; distance <= maxDistance; distance += step) {
      const halfFov = fovDegrees / 2;
      const points = [];
      for (let index = 0; index <= 20; index += 1) {
        const angle = -halfFov + (halfFov * 2 * index) / 20;
        const radians = (angle * Math.PI) / 180;
        points.push(rotatedPoint(Math.sin(radians) * distance, Math.cos(radians) * distance));
      }
      arcs.push(points.map((point) => `${point.x},${point.y}`).join(" "));
    }
    return arcs;
  }

  function angleGuidePoints() {
    return [-60, -30, 0, 30, 60]
      .filter((angle) => Math.abs(angle) <= fovDegrees / 2)
      .map((angle) => {
        const radians = (angle * Math.PI) / 180;
        return {
          angle,
          end: rotatedPoint(Math.sin(radians) * rangeY, Math.cos(radians) * rangeY)
        };
      });
  }

  function zonePoints(zone) {
    const options = transformOptions();
    if (!options) return "";
    return radarZoneToFloorplanPolygon(zone, options)
      .map((point) => `${point.x},${point.y}`)
      .join(" ");
  }

  function zonePolygonPoints(zone) {
    const options = transformOptions();
    if (!options) return [];
    return radarZoneToFloorplanPolygon(zone, options);
  }

  function zonePolygonEdges(zone) {
    const points = zonePolygonPoints(zone);
    if (points.length < 2) return [];
    return points.map((point, index) => ({
      index,
      start: point,
      end: points[(index + 1) % points.length]
    }));
  }

  function exitLinePoints(zone) {
    const options = transformOptions();
    if (!options) return null;
    const line = exitLineFromZone(zone);
    return {
      start: radarPointToFloorplanPx(line.start, options),
      end: radarPointToFloorplanPx(line.end, options)
    };
  }

  function isZoneEditingSource(source) {
    return editableZoneSource === source;
  }

  function isZoneEditable(source, zone = null) {
    return Boolean(editableZoneSource) && isZoneEditingSource(source) && (!zone || source !== "zones" || zoneEditable(zone));
  }

  function isSelectedZone(zone) {
    return selectedZoneId === zone.id;
  }

  function beginZoneMove(event, zone, source) {
    if (!isZoneEditable(source, zone)) return;
    const options = transformOptions();
    if (!options) return;
    const startRadarPoint = radarEditPointFromImagePoint(imagePointFromEvent(event), options);
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    onSelectZone?.(zone.id, -1);
    drag = {
      type: "zone-move",
      pointerId: event.pointerId,
      source,
      zone,
      startRadarPoint
    };
  }

  function beginZonePointMove(event, zone, source, pointIndex) {
    if (!isZoneEditable(source, zone)) return;
    const options = transformOptions();
    if (!options) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    onSelectZone?.(zone.id, pointIndex);
    drag = {
      type: "zone-point",
      pointerId: event.pointerId,
      source,
      zone,
      pointIndex
    };
  }

  function handleZoneKeydown(event, zone, source) {
    if (event.key !== "Enter" && event.key !== " ") return;
    if (!isZoneEditable(source)) return;
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, -1);
  }

  function handleZonePointKeydown(event, zone, source, pointIndex) {
    if (event.key !== "Enter" && event.key !== " ") return;
    if (!isZoneEditable(source)) return;
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, pointIndex);
  }

  function handleZoneEdgeClick(event, zone, source, edgeIndex) {
    if (!isZoneEditable(source)) return;
    const options = transformOptions();
    if (!options) return;
    const radarPoint = radarEditPointFromImagePoint(imagePointFromEvent(event), options);
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, -1);
    onZoneEdgeClick?.(source, zone, edgeIndex, radarPoint);
  }

  function handleZoneEdgeKeydown(event, zone, source, edge) {
    if (event.key !== "Enter" && event.key !== " ") return;
    if (!isZoneEditable(source)) return;
    const options = transformOptions();
    if (!options) return;
    const radarPoint = radarEditPointFromImagePoint({
      x: (edge.start.x + edge.end.x) / 2,
      y: (edge.start.y + edge.end.y) / 2
    }, options);
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, -1);
    onZoneEdgeClick?.(source, zone, edge.index, radarPoint);
  }

  function renderableTargets() {
    return targets.filter((target) => target.active && Number.isFinite(target.x) && Number.isFinite(target.y));
  }

  function rotateHandlePoint() {
    return rotatedPoint(0, Math.min(rangeY, 1300));
  }

  function setPlacement(nextPlacement, commit = false, options = {}) {
    if (readOnly) return;
    const adjusted = options.keepOriginOutOfWalls === false ? nextPlacement : keepOriginOutOfWalls(nextPlacement);
    onChange?.(adjusted);
    if (commit) onCommit?.(adjusted);
  }

  function keepOriginOutOfWalls(nextPlacement) {
    if (!wallSegments.length) return clampOriginToOuterBounds(nextPlacement);
    let originX = nextPlacement.originX;
    let originY = nextPlacement.originY;
    let nearest = null;
    const origin = { x: originX, y: originY };

    for (const segment of wallSegments) {
      const line = wallSegmentLine(segment);
      if (!line) continue;
      const start = { x: line.x1, y: line.y1 };
      const end = { x: line.x2, y: line.y2 };
      const closest = closestPointOnSegment(origin, start, end);
      const segmentLength = Math.hypot(end.x - start.x, end.y - start.y);
      if (segmentLength < 1) continue;
      if (closest.t < -RADAR_WALL_SEGMENT_MARGIN_PX / segmentLength || closest.t > 1 + RADAR_WALL_SEGMENT_MARGIN_PX / segmentLength) {
        continue;
      }
      const distance = Math.hypot(origin.x - closest.x, origin.y - closest.y);
      if (distance <= RADAR_WALL_DETECT_RADIUS_PX && (!nearest || distance < nearest.distance)) {
        nearest = { closest, distance, start, end };
      }
    }

    if (nearest && nearest.distance < RADAR_WALL_CLEARANCE_PX) {
      let dx = originX - nearest.closest.x;
      let dy = originY - nearest.closest.y;
      let distance = Math.hypot(dx, dy);
      if (distance < 0.5) {
        const centerX = (scaleEstimate?.outerBounds?.x ?? 0) + (scaleEstimate?.outerBounds?.width ?? imageWidth) / 2;
        const centerY = (scaleEstimate?.outerBounds?.y ?? 0) + (scaleEstimate?.outerBounds?.height ?? imageHeight) / 2;
        dx = originX - centerX;
        dy = originY - centerY;
        distance = Math.hypot(dx, dy);
      }
      if (distance < 0.5) {
        dx = -(nearest.end.y - nearest.start.y);
        dy = nearest.end.x - nearest.start.x;
        distance = Math.hypot(dx, dy);
      }
      originX = nearest.closest.x + (dx / distance) * RADAR_WALL_CLEARANCE_PX;
      originY = nearest.closest.y + (dy / distance) * RADAR_WALL_CLEARANCE_PX;
    }

    return clampOriginToOuterBounds({
      ...nextPlacement,
      originX,
      originY
    });
  }

  function clampOriginToOuterBounds(nextPlacement) {
    const bounds = scaleEstimate?.outerBounds;
    if (!bounds) return nextPlacement;
    const minX = bounds.x + RADAR_ORIGIN_RADIUS_PX;
    const maxX = bounds.x + bounds.width - RADAR_ORIGIN_RADIUS_PX;
    const minY = bounds.y + RADAR_ORIGIN_RADIUS_PX;
    const maxY = bounds.y + bounds.height - RADAR_ORIGIN_RADIUS_PX;
    if (minX > maxX || minY > maxY) return nextPlacement;
    return {
      ...nextPlacement,
      originX: Math.max(minX, Math.min(maxX, nextPlacement.originX)),
      originY: Math.max(minY, Math.min(maxY, nextPlacement.originY))
    };
  }

  function beginMove(event) {
    if (readOnly) return;
    if (!scaleEstimate) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    drag = {
      type: "move",
      pointerId: event.pointerId,
      startPoint: imagePointFromEvent(event),
      startPlacement: effectivePlacement()
    };
  }

  function beginRotate(event) {
    if (readOnly) return;
    if (!scaleEstimate) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    drag = {
      type: "rotate",
      pointerId: event.pointerId,
      startPlacement: effectivePlacement()
    };
    updateDrag(event);
  }

  function updateDrag(event) {
    if (!drag || event.pointerId !== drag.pointerId) return;
    if (readOnly && drag.type !== "zone-move" && drag.type !== "zone-point") return;
    event.preventDefault();
    event.stopPropagation();
    const point = imagePointFromEvent(event);
    if (drag.type === "zone-move" || drag.type === "zone-point") {
      const options = transformOptions();
      if (!options) return;
      const radarPoint = radarEditPointFromImagePoint(point, options);
      if (drag.type === "zone-move") {
        onZoneMove?.(drag.source, drag.zone, drag.startRadarPoint, radarPoint);
      } else {
        onZonePointMove?.(drag.source, drag.zone, drag.pointIndex, radarPoint);
      }
      return;
    }
    if (readOnly) return;
    if (drag.type === "move") {
      const dx = point.x - drag.startPoint.x;
      const dy = point.y - drag.startPoint.y;
      setPlacement({
        ...drag.startPlacement,
        originX: drag.startPlacement.originX + dx,
        originY: drag.startPlacement.originY + dy
      });
      return;
    }

    const current = effectivePlacement();
    const angle = Math.atan2(point.x - current.originX, -(point.y - current.originY)) * (180 / Math.PI);
    setPlacement({
      ...current,
      rotation: Math.round(angle * 10) / 10
    }, false, { keepOriginOutOfWalls: false });
  }

  function endDrag(event) {
    if (!drag || event.pointerId !== drag.pointerId) return;
    if (readOnly && drag.type !== "zone-move" && drag.type !== "zone-point") return;
    event.preventDefault();
    event.stopPropagation();
    const finishedDrag = drag;
    drag = null;
    if (finishedDrag.type === "zone-move" || finishedDrag.type === "zone-point") {
      onZoneEditCommit?.(finishedDrag.source, finishedDrag.zone.id);
      return;
    }
    onCommit?.(finishedDrag.type === "rotate" ? effectivePlacement() : keepOriginOutOfWalls(effectivePlacement()));
  }

  function sensorPoints() {
    const left = rotatedPoint(-110, 0);
    const right = rotatedPoint(110, 0);
    const front = rotatedPoint(0, 220);
    return `${front.x},${front.y} ${left.x},${left.y} ${right.x},${right.y}`;
  }

  function targetLabel(target) {
    return target.fused && fusionLabel ? `${target.name} · ${fusionLabel}` : target.name;
  }

  function wallSegmentLine(segment) {
    const line = {
      x1: Number(segment.x1),
      y1: Number(segment.y1),
      x2: Number(segment.x2),
      y2: Number(segment.y2)
    };
    return Object.values(line).every(Number.isFinite) ? line : null;
  }

  function closestPointOnSegment(point, start, end) {
    const dx = end.x - start.x;
    const dy = end.y - start.y;
    const lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.001) return { x: start.x, y: start.y, t: 0 };
    const t = ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSquared;
    return {
      x: start.x + dx * t,
      y: start.y + dy * t,
      t
    };
  }

  function isOcclusionSegmentIgnored(segment, ignored = new Set(ignoredOcclusionSegmentIds)) {
    return isFloorplanOcclusionSegmentIgnored(segment, ignored);
  }

  function visibleRayPolygon() {
    return pointList(visibleRayPoints());
  }

  function visibleRayPoints() {
    if (!scaleEstimate) return [];
    const current = effectivePlacement();
    const origin = { x: current.originX, y: current.originY };
    const halfFov = fovDegrees / 2;
    const rayCount = Math.max(1, Math.ceil(fovDegrees / RADAR_VISIBILITY_RAY_STEP_DEGREES));
    const rayEndpoints = [];

    for (let index = 0; index <= rayCount; index += 1) {
      const angle = -halfFov + (fovDegrees * index) / rayCount;
      const radians = (angle * Math.PI) / 180;
      rayEndpoints.push(rotatedPoint(Math.sin(radians) * rangeY, Math.cos(radians) * rangeY));
    }

    return radarVisiblePolygonPoints(origin, rayEndpoints, occlusionSegments, ignoredOcclusionSegmentIds);
  }

  function pointList(points) {
    return points.map((point) => `${point.x},${point.y}`).join(" ");
  }

  function toggleOcclusionSegment(event, segment) {
    if (readOnly) return;
    event.preventDefault();
    event.stopPropagation();
    onToggleOcclusionSegment?.(segment);
  }

  function handleOcclusionSegmentKeydown(event, segment) {
    if (event.key !== "Enter" && event.key !== " ") return;
    toggleOcclusionSegment(event, segment);
  }
</script>

{#if scaleEstimate}
  {@const current = effectivePlacement()}
  {@const handle = rotateHandlePoint()}
  {@const visiblePolygon = visibleRayPolygon()}
  {@const remoteBand = remoteRangeBandPoints()}
  {@const occlusionMaskVersion = `${ignoredOcclusionSegmentIds.join("|")}:${current.originX}:${current.originY}:${current.rotation}:${visiblePolygon}`}
  <svg
    bind:this={radarLayer}
    class="floorplan-radar-placement-layer"
    data-readonly={readOnly ? "true" : "false"}
    style={transformStyle}
    viewBox={`${viewX()} ${viewY()} ${viewWidth()} ${viewHeight()}`}
    preserveAspectRatio="none"
    aria-label="레이더 배치 오버레이"
    role="img"
    onpointermove={updateDrag}
    onpointerup={endDrag}
    onpointercancel={endDrag}
  >
    <defs>
      <clipPath id={radarCoverageClipId}>
        <path d={coveragePath()}></path>
      </clipPath>
      {#key occlusionMaskVersion}
        <mask
          id={radarReachableMaskId}
          maskUnits="userSpaceOnUse"
          maskContentUnits="userSpaceOnUse"
          x={viewX()}
          y={viewY()}
          width={viewWidth()}
          height={viewHeight()}
        >
          <rect x={viewX()} y={viewY()} width={viewWidth()} height={viewHeight()} fill="white"></rect>
          <polygon points={visiblePolygon} fill="black"></polygon>
        </mask>
      {/key}
    </defs>
    <g class="floorplan-radar-placement">
      <path
        class="floorplan-radar-coverage"
        d={coveragePath()}
        role="button"
        tabindex="0"
        aria-label="레이더맵 이동"
        onpointerdown={beginMove}
      ></path>
      {#if remoteBand}
        <polygon class="floorplan-radar-remote-band" points={remoteBand}></polygon>
      {/if}
      {#key occlusionMaskVersion}
        <path
          class="floorplan-radar-wall-occlusion"
          d={coveragePath()}
          mask={`url(#${radarReachableMaskId})`}
        ></path>
      {/key}
      {#if occlusionEditActive}
        {#each occlusionSegments.filter((segment) => !segment.locked) as segment (segment.id)}
          <line
            class={`floorplan-radar-occlusion-edge ${isOcclusionSegmentIgnored(segment) ? "ignored" : "active"}`}
            x1={segment.x1}
            y1={segment.y1}
            x2={segment.x2}
            y2={segment.y2}
            role="button"
            tabindex="0"
            onclick={(event) => toggleOcclusionSegment(event, segment)}
            onkeydown={(event) => handleOcclusionSegmentKeydown(event, segment)}
          ></line>
        {/each}
      {/if}
      {#each calibrationZones as zone (zone.id)}
        {#if zone.points?.length}
          <polygon
            class={`floorplan-radar-zone calibration ${isSelectedZone(zone) ? "selected" : ""}`}
            data-editable={isZoneEditable("calibration") ? "true" : "false"}
            data-zone-id={zone.id}
            data-zone-drag={isZoneEditable("calibration") ? "move" : undefined}
            points={zonePoints(zone)}
            role="button"
            tabindex="0"
            aria-label="오탐 영역 선택"
            onpointerdown={(event) => beginZoneMove(event, zone, "calibration")}
            onkeydown={(event) => handleZoneKeydown(event, zone, "calibration")}
          ></polygon>
          {#if isZoneEditable("calibration") && isSelectedZone(zone)}
            {#each zonePolygonPoints(zone) as point, index}
              <circle
                class="floorplan-radar-zone-point"
                data-active={selectedZonePointIndex === index ? "true" : "false"}
                data-zone-id={zone.id}
                data-zone-drag="resize"
                data-zone-point={index}
                cx={point.x}
                cy={point.y}
                r="7"
                role="button"
                tabindex="0"
                onpointerdown={(event) => beginZonePointMove(event, zone, "calibration", index)}
                onkeydown={(event) => handleZonePointKeydown(event, zone, "calibration", index)}
              ></circle>
            {/each}
          {/if}
        {/if}
      {/each}
      {#each zones as zone (zone.id)}
        {#if zone.points?.length}
          {@const exitLine = zone.type === "exit" ? exitLinePoints(zone) : null}
          {#if exitLine}
            <line
              class={`floorplan-radar-exit-line-display ${isSelectedZone(zone) ? "selected" : ""}`}
              x1={exitLine.start.x}
              y1={exitLine.start.y}
              x2={exitLine.end.x}
              y2={exitLine.end.y}
            ></line>
            <line
              class="floorplan-radar-exit-line-hit"
              data-editable={isZoneEditable("zones", zone) ? "true" : "false"}
              data-zone-id={zone.id}
              data-zone-drag={isZoneEditable("zones", zone) ? "move" : undefined}
              x1={exitLine.start.x}
              y1={exitLine.start.y}
              x2={exitLine.end.x}
              y2={exitLine.end.y}
              role="button"
              tabindex="0"
              aria-label="퇴실 지점 선택"
              onpointerdown={(event) => beginZoneMove(event, zone, "zones")}
              onkeydown={(event) => handleZoneKeydown(event, zone, "zones")}
            ></line>
            {#if isZoneEditable("zones", zone) && isSelectedZone(zone)}
              <line
                class="floorplan-radar-exit-line-edit"
                x1={exitLine.start.x}
                y1={exitLine.start.y}
                x2={exitLine.end.x}
                y2={exitLine.end.y}
              ></line>
              <circle
                class="floorplan-radar-zone-point floorplan-radar-exit-line-handle"
                data-active={selectedZonePointIndex === 0 ? "true" : "false"}
                data-zone-id={zone.id}
                data-zone-drag="resize"
                data-zone-point={0}
                cx={exitLine.start.x}
                cy={exitLine.start.y}
                r="8"
                role="button"
                tabindex="0"
                onpointerdown={(event) => beginZonePointMove(event, zone, "zones", 0)}
                onkeydown={(event) => handleZonePointKeydown(event, zone, "zones", 0)}
              ></circle>
              <circle
                class="floorplan-radar-zone-point floorplan-radar-exit-line-handle"
                data-active={selectedZonePointIndex === 1 ? "true" : "false"}
                data-zone-id={zone.id}
                data-zone-drag="resize"
                data-zone-point={1}
                cx={exitLine.end.x}
                cy={exitLine.end.y}
                r="8"
                role="button"
                tabindex="0"
                onpointerdown={(event) => beginZonePointMove(event, zone, "zones", 1)}
                onkeydown={(event) => handleZonePointKeydown(event, zone, "zones", 1)}
              ></circle>
            {/if}
          {:else}
            <polygon
              class={`floorplan-radar-zone ${zone.placeholder ? "placeholder" : zone.type} ${isSelectedZone(zone) ? "selected" : ""}`}
              data-editable={isZoneEditable("zones", zone) ? "true" : "false"}
              data-zone-id={zone.id}
              data-zone-drag={isZoneEditable("zones", zone) ? "move" : undefined}
              points={zonePoints(zone)}
              role="button"
              tabindex="0"
              aria-label="감지 구역 선택"
              onpointerdown={(event) => beginZoneMove(event, zone, "zones")}
              onkeydown={(event) => handleZoneKeydown(event, zone, "zones")}
            ></polygon>
            {#if isZoneEditable("zones", zone) && isSelectedZone(zone)}
              {#each zonePolygonEdges(zone) as edge}
                <line
                  class="floorplan-radar-zone-edge"
                  data-zone-id={zone.id}
                  data-zone-edge={edge.index}
                  x1={edge.start.x}
                  y1={edge.start.y}
                  x2={edge.end.x}
                  y2={edge.end.y}
                  role="button"
                  tabindex="0"
                  aria-label="감지 구역 꼭짓점 추가"
                  onclick={(event) => handleZoneEdgeClick(event, zone, "zones", edge.index)}
                  onkeydown={(event) => handleZoneEdgeKeydown(event, zone, "zones", edge)}
                ></line>
              {/each}
              {#each zonePolygonPoints(zone) as point, index}
                <circle
                  class="floorplan-radar-zone-point"
                  data-active={selectedZonePointIndex === index ? "true" : "false"}
                  data-zone-id={zone.id}
                  data-zone-drag="resize"
                  data-zone-point={index}
                  cx={point.x}
                  cy={point.y}
                  r="7"
                  role="button"
                  tabindex="0"
                  onpointerdown={(event) => beginZonePointMove(event, zone, "zones", index)}
                  onkeydown={(event) => handleZonePointKeydown(event, zone, "zones", index)}
                ></circle>
              {/each}
            {/if}
          {/if}
        {/if}
      {/each}
      {#each distanceArcs() as arc}
        <polyline class="floorplan-radar-arc" points={arc}></polyline>
      {/each}
      {#each angleGuidePoints() as guide}
        <line
          class="floorplan-radar-angle"
          x1={current.originX}
          y1={current.originY}
          x2={guide.end.x}
          y2={guide.end.y}
        ></line>
      {/each}
      <line
        class="floorplan-radar-rotation-line"
        x1={current.originX}
        y1={current.originY}
        x2={handle.x}
        y2={handle.y}
      ></line>
      <polygon
        class="floorplan-radar-sensor"
        points={sensorPoints()}
        role="button"
        tabindex="0"
        aria-label="레이더 센서 이동"
        onpointerdown={beginMove}
      ></polygon>
      {#each peerRadars as radar, index (radar.participantId)}
        <g
          class="floorplan-peer-radar"
          transform={`translate(${radar.xPx} ${radar.yPx}) rotate(${radar.rotationDegrees})`}
        >
          <title>{`${peerRadarLabel} ${index + 1}`}</title>
          <line x1="0" y1="0" x2="0" y2="-18"></line>
          <polygon points="0,-11 -7,7 7,7"></polygon>
        </g>
      {/each}
      {#each renderableTargets() as target (target.id)}
        {@const targetPoint = rotatedPoint(target.x, target.y)}
        <g
          class={`floorplan-radar-target${target.fused ? " fused" : ""}${target.displayMode ? ` ${target.displayMode}` : ""}`}
          style={`--target-color:${target.color || "#ff6b7a"};transform:translate(${targetPoint.x}px, ${targetPoint.y}px)`}
        >
          {#if target.fused}
            <circle class="target-fusion-ring" cx="0" cy="0" r={screenSize(13)}></circle>
          {/if}
          <circle cx="0" cy="0" r={screenSize(8)}></circle>
          <text
            x={screenSize(11)}
            y={-screenSize(11)}
            style={`font-size: ${screenSize(14)}px;`}
          >{targetLabel(target)}</text>
        </g>
      {/each}
      <circle
        class="floorplan-radar-origin"
        cx={current.originX}
        cy={current.originY}
        r="8"
        role="button"
        tabindex="0"
        aria-label="레이더 원점 이동"
        onpointerdown={beginMove}
      ></circle>
      <circle
        class="floorplan-radar-rotate-handle"
        cx={handle.x}
        cy={handle.y}
        r="10"
        role="button"
        tabindex="0"
        aria-label="레이더 방향 회전"
        onpointerdown={beginRotate}
      ></circle>
      {#if showPlacementLabel}
        <text class="floorplan-radar-placement-label" x={current.originX + 12} y={current.originY - 14}>
          <tspan x={current.originX + 12} dy="0">origin {Math.round(current.originX)}, {Math.round(current.originY)}</tspan>
          <tspan x={current.originX + 12} dy="15">rotation {Math.round(current.rotation)}° · scale {scalePercent}%</tspan>
        </text>
      {/if}
    </g>
  </svg>
{/if}
