<script>
  import { DEFAULT_CARD_CONFIG } from "../../core/defaults";
  import {
    floorplanPxToRadarPoint,
    radarPointToFloorplanPx,
    radarZoneToFloorplanPolygon
  } from "../../core/floorplan/radar-floorplan-transform";
  import { LD2450_FOV_DEGREES, radarViewportRangeX } from "../../core/radar-math";

  let {
    transformStyle = "",
    imageWidth = 1,
    imageHeight = 1,
    scaleEstimate = null,
    placement = null,
    scalePercent = 100,
    zones = [],
    calibrationZones = [],
    targets = [],
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
    onToggleOcclusionSegment
  } = $props();

  let drag = $state(null);
  const RADAR_WALL_CLEARANCE_PX = 10;
  const RADAR_WALL_DETECT_RADIUS_PX = 12;
  const RADAR_WALL_SEGMENT_MARGIN_PX = 8;
  const RADAR_ORIGIN_RADIUS_PX = 8;
  const RADAR_VISIBILITY_RAY_STEP_DEGREES = 0.5;
  const radarCoverageClipId = `radar-coverage-clip-${Math.random().toString(36).slice(2)}`;
  const radarReachableMaskId = `radar-reachable-mask-${Math.random().toString(36).slice(2)}`;

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

  function pxPerMmX() {
    return scaleEstimate ? (1 / scaleEstimate.mmPerPxX) * scaleFactor() : 0;
  }

  function pxPerMmY() {
    return scaleEstimate ? (1 / scaleEstimate.mmPerPxY) * scaleFactor() : 0;
  }

  function transformOptions() {
    if (!scaleEstimate) return null;
    return {
      placement: effectivePlacement(),
      scale: scaleEstimate,
      scaleFactor: scaleFactor()
    };
  }

  function radarRangeX() {
    return Math.max(DEFAULT_CARD_CONFIG.range_x, radarViewportRangeX(rangeY, fovDegrees));
  }

  function imagePointFromEvent(event) {
    const svg = event.currentTarget.closest("svg") ?? event.currentTarget;
    const rect = svg.getBoundingClientRect();
    if (!rect.width || !rect.height) return { x: 0, y: 0 };
    return {
      x: ((event.clientX - rect.left) / rect.width) * imageWidth,
      y: ((event.clientY - rect.top) / rect.height) * imageHeight
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

  function isZoneEditingSource(source) {
    return editableZoneSource === source;
  }

  function isZoneEditable(source) {
    return Boolean(editableZoneSource) && isZoneEditingSource(source);
  }

  function isSelectedZone(zone) {
    return selectedZoneId === zone.id;
  }

  function beginZoneMove(event, zone, source) {
    if (!isZoneEditable(source)) return;
    const options = transformOptions();
    if (!options) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    onSelectZone?.(zone.id, -1);
    drag = {
      type: "zone-move",
      pointerId: event.pointerId,
      source,
      zone,
      startRadarPoint: floorplanPxToRadarPoint(imagePointFromEvent(event), options)
    };
  }

  function beginZonePointMove(event, zone, source, pointIndex) {
    if (!isZoneEditable(source)) return;
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
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, -1);
    onZoneEdgeClick?.(source, zone, edgeIndex, floorplanPxToRadarPoint(imagePointFromEvent(event), options));
  }

  function handleZoneEdgeKeydown(event, zone, source, edge) {
    if (event.key !== "Enter" && event.key !== " ") return;
    if (!isZoneEditable(source)) return;
    const options = transformOptions();
    if (!options) return;
    event.preventDefault();
    event.stopPropagation();
    onSelectZone?.(zone.id, -1);
    onZoneEdgeClick?.(source, zone, edge.index, floorplanPxToRadarPoint({
      x: (edge.start.x + edge.end.x) / 2,
      y: (edge.start.y + edge.end.y) / 2
    }, options));
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
      const radarPoint = floorplanPxToRadarPoint(point, options);
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
    const current = effectivePlacement();
    const left = rotatedPoint(-110, 0);
    const right = rotatedPoint(110, 0);
    const front = rotatedPoint(0, 220);
    return `${front.x},${front.y} ${left.x},${left.y} ${right.x},${right.y}`;
  }

  function orientation(a, b, c) {
    return (b.y - a.y) * (c.x - b.x) - (b.x - a.x) * (c.y - b.y);
  }

  function onSegment(a, b, c) {
    return (
      Math.min(a.x, c.x) <= b.x + 0.001 &&
      b.x <= Math.max(a.x, c.x) + 0.001 &&
      Math.min(a.y, c.y) <= b.y + 0.001 &&
      b.y <= Math.max(a.y, c.y) + 0.001
    );
  }

  function segmentsIntersect(a, b, c, d) {
    const o1 = orientation(a, b, c);
    const o2 = orientation(a, b, d);
    const o3 = orientation(c, d, a);
    const o4 = orientation(c, d, b);
    if (Math.abs(o1) < 0.001 && onSegment(a, c, b)) return true;
    if (Math.abs(o2) < 0.001 && onSegment(a, d, b)) return true;
    if (Math.abs(o3) < 0.001 && onSegment(c, a, d)) return true;
    if (Math.abs(o4) < 0.001 && onSegment(c, b, d)) return true;
    return (o1 > 0) !== (o2 > 0) && (o3 > 0) !== (o4 > 0);
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

  function occlusionSegmentKey(segment) {
    return segment.occlusionKey ?? segment.id;
  }

  function isOcclusionSegmentIgnored(segment, ignored = new Set(ignoredOcclusionSegmentIds)) {
    if (segment.locked) return false;
    return ignored.has(segment.id) || ignored.has(occlusionSegmentKey(segment));
  }

  function visibleRayPolygon() {
    if (!scaleEstimate) return "";
    const current = effectivePlacement();
    const origin = { x: current.originX, y: current.originY };
    const ignored = new Set(ignoredOcclusionSegmentIds);
    const blockingWalls = occlusionSegments
      .filter((segment) => !isOcclusionSegmentIgnored(segment, ignored))
      .map(wallSegmentLine)
      .filter(Boolean);
    const halfFov = fovDegrees / 2;
    const rayCount = Math.max(1, Math.ceil(fovDegrees / RADAR_VISIBILITY_RAY_STEP_DEGREES));
    const points = [origin];

    for (let index = 0; index <= rayCount; index += 1) {
      const angle = -halfFov + (fovDegrees * index) / rayCount;
      const radians = (angle * Math.PI) / 180;
      const rayEnd = rotatedPoint(Math.sin(radians) * rangeY, Math.cos(radians) * rangeY);
      const hit = closestRayWallHit(origin, rayEnd, blockingWalls);
      points.push(hit ?? rayEnd);
    }

    return points.map((point) => `${point.x},${point.y}`).join(" ");
  }

  function closestRayWallHit(origin, rayEnd, walls) {
    let nearest = null;
    for (const wall of walls) {
      const hit = segmentIntersectionPoint(origin, rayEnd, { x: wall.x1, y: wall.y1 }, { x: wall.x2, y: wall.y2 });
      if (!hit) continue;
      const distance = Math.hypot(hit.x - origin.x, hit.y - origin.y);
      if (distance < 0.5) continue;
      if (!nearest || distance < nearest.distance) {
        nearest = { ...hit, distance };
      }
    }
    return nearest ? { x: nearest.x, y: nearest.y } : null;
  }

  function segmentIntersectionPoint(a, b, c, d) {
    const denominator = (a.x - b.x) * (c.y - d.y) - (a.y - b.y) * (c.x - d.x);
    if (Math.abs(denominator) < 0.001) return null;
    const determinantA = a.x * b.y - a.y * b.x;
    const determinantB = c.x * d.y - c.y * d.x;
    const point = {
      x: (determinantA * (c.x - d.x) - (a.x - b.x) * determinantB) / denominator,
      y: (determinantA * (c.y - d.y) - (a.y - b.y) * determinantB) / denominator
    };
    return onSegment(a, point, b) && onSegment(c, point, d) ? point : null;
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
  {@const occlusionMaskVersion = `${ignoredOcclusionSegmentIds.join("|")}:${current.originX}:${current.originY}:${current.rotation}`}
  <svg
    class="floorplan-radar-placement-layer"
    data-readonly={readOnly ? "true" : "false"}
    style={transformStyle}
    viewBox={`0 0 ${imageWidth} ${imageHeight}`}
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
        <mask id={radarReachableMaskId} maskUnits="userSpaceOnUse">
          <rect x="0" y="0" width={imageWidth} height={imageHeight} fill="white"></rect>
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
          <polygon
            class={`floorplan-radar-zone ${zone.type} ${isSelectedZone(zone) ? "selected" : ""}`}
            data-editable={isZoneEditable("zones") ? "true" : "false"}
            points={zonePoints(zone)}
            role="button"
            tabindex="0"
            aria-label="감지 구역 선택"
            onpointerdown={(event) => beginZoneMove(event, zone, "zones")}
            onkeydown={(event) => handleZoneKeydown(event, zone, "zones")}
          ></polygon>
          {#if isZoneEditable("zones") && isSelectedZone(zone)}
            {#each zonePolygonEdges(zone) as edge}
              <line
                class="floorplan-radar-zone-edge"
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
      {#each renderableTargets() as target (target.id)}
        {@const targetPoint = rotatedPoint(target.x, target.y)}
        <g class="floorplan-radar-target" style={`--target-color:${target.color || "#ff6b7a"}`}>
          <circle cx={targetPoint.x} cy={targetPoint.y} r="8"></circle>
          <text x={targetPoint.x + 10} y={targetPoint.y - 10}>{target.name}</text>
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
