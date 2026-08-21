<script>
  import { onMount } from "svelte";
  import { roomCandidatePoints, roomCandidatePointString } from "../floorplan/room-candidate-points";
  import { manualFloorplanEdgeLabels } from "../floorplan/manual-floorplan";

  let {
    transformStyle = "",
    candidates = [],
    selectedCandidateId = "",
    selectedCandidateVertexIndex = -1,
    focusSelectedCandidate = false,
    selectionLocked = false,
    imageWidth = 1,
    imageHeight = 1,
    viewBounds = null,
    ariaLabels,
    allowCandidateEditing = true,
    manualDraftActive = false,
    manualDraftPoints = [],
    manualDraftHoverPoint = null,
    splitDraftActive = false,
    splitDraftPoints = [],
    splitDraftHoverPoint = null,
    mergeDraftActive = false,
    mergeDraftCandidateIds = [],
    snapEdges = [],
    selectedSnapEdgeKey = "",
    snapWallSegments = [],
    wallMaskCells = [],
    showWallMaskCells = false,
    ocrItems = [],
    showOcrItems = false,
    showRoomSizeBounds = false,
    metricMmPerPx = 0,
    showMetricGrid = true,
    metricOutlinePoints = [],
    manualOutlineEditable = false,
    onSelect,
    onManualDraftPoint,
    onManualDraftPointDelete,
    onManualDraftHover,
    onManualDraftLeave,
    onSplitDraftPoint,
    onSplitDraftHover,
    onSplitDraftLeave,
    onSplitDraftPointMove,
    onSelectSnapEdge,
    onSnapToWallSegment,
    onCandidateVertexAdd,
    onCandidateVertexSelect,
    onCandidateVertexDelete,
    onCandidateVertexMoveStart,
    onCandidateVertexMove,
    onCandidateVertexMoveEnd,
    onManualOutlineVertexAdd,
    onManualOutlineVertexDelete,
    onManualOutlineVertexMove
  } = $props();

  const VERTEX_DRAG_CLICK_TOLERANCE_PX = 4;
  const VERTEX_DRAG_DOUBLE_CLICK_SUPPRESS_MS = 350;
  const VERTEX_INSERT_DOUBLE_CLICK_SUPPRESS_MS = 700;

  let draggedVertex = null;
  let draggedOutlineVertex = null;
  let draggedSplitPoint = null;
  let suppressVertexDoubleClickUntil = 0;
  let suppressInsertedVertexDoubleClick = null;
  let candidateLayer = $state(null);
  let metricUnitScale = $state(1);
  let metricReady = $state(false);

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

  function updateMetricUnitScale() {
    if (!candidateLayer || metricMmPerPx <= 0) {
      metricUnitScale = 1;
      metricReady = false;
      return;
    }
    const bounds = candidateLayer.getBoundingClientRect();
    if (bounds.width <= 0 || bounds.height <= 0) return;
    metricUnitScale = Math.max(viewWidth() / bounds.width, viewHeight() / bounds.height);
    metricReady = true;
  }

  function metricSize(screenPixels) {
    return metricMmPerPx > 0 ? screenPixels * metricUnitScale : screenPixels;
  }

  function metricTextStyle(screenPixels) {
    return metricMmPerPx > 0 ? `font-size: ${metricSize(screenPixels)}px;` : "";
  }

  function metricGridStepPx() {
    return metricMmPerPx > 0 ? 100 / metricMmPerPx : 0;
  }

  function metricSnapStepPx() {
    return metricMmPerPx > 0 ? 500 / metricMmPerPx : 0;
  }

  function metricMajorStepPx() {
    return metricMmPerPx > 0 ? 1000 / metricMmPerPx : 0;
  }

  function showMetricMinorGrid() {
    return metricGridStepPx() / Math.max(0.001, metricUnitScale) >= 3;
  }

  function metricLabelPosition(label) {
    const radians = label.angleDeg * Math.PI / 180;
    let normalX = -Math.sin(radians);
    let normalY = Math.cos(radians);
    if (normalY > 0 || (Math.abs(normalY) < 0.001 && normalX > 0)) {
      normalX *= -1;
      normalY *= -1;
    }
    const offset = metricSize(14);
    return {
      x: label.x + normalX * offset,
      y: label.y + normalY * offset
    };
  }

  onMount(() => {
    updateMetricUnitScale();
    const observer = typeof ResizeObserver === "undefined"
      ? null
      : new ResizeObserver(updateMetricUnitScale);
    observer?.observe(candidateLayer);
    window.addEventListener("resize", updateMetricUnitScale);
    return () => {
      observer?.disconnect();
      window.removeEventListener("resize", updateMetricUnitScale);
    };
  });

  $effect(() => {
    const refreshKey = `${imageWidth}:${imageHeight}:${viewX()}:${viewY()}:${viewWidth()}:${viewHeight()}:${metricMmPerPx}:${transformStyle}`;
    queueMicrotask(() => {
      void refreshKey;
      updateMetricUnitScale();
    });
  });

  function candidateBounds(candidate) {
    const points = roomCandidatePoints(candidate);
    const minX = Math.max(0, Math.min(...points.map(([x]) => x)));
    const minY = Math.max(0, Math.min(...points.map(([, y]) => y)));
    const maxX = Math.min(imageWidth, Math.max(...points.map(([x]) => x)));
    const maxY = Math.min(imageHeight, Math.max(...points.map(([, y]) => y)));
    return {
      x: minX,
      y: minY,
      width: Math.max(0, maxX - minX),
      height: Math.max(0, maxY - minY)
    };
  }

  function candidateEditEdges(candidate) {
    const points = roomCandidatePoints(candidate);
    return points.map(([x1, y1], index) => {
      const [x2, y2] = points[(index + 1) % points.length];
      return { index, x1, y1, x2, y2 };
    });
  }

  function pointList(points) {
    return points.map(([x, y]) => `${x},${y}`).join(" ");
  }

  function metricLabels(points, closePolygon = false) {
    return metricMmPerPx > 0
      ? manualFloorplanEdgeLabels(points, closePolygon, metricMmPerPx)
      : [];
  }

  function labelX(candidate) {
    return metricMmPerPx > 0
      ? candidate.rect.x + candidate.rect.width / 2
      : candidate.rect.x + 8;
  }

  function labelY(candidate) {
    return metricMmPerPx > 0
      ? candidate.rect.y + candidate.rect.height / 2
      : candidate.rect.y + 18;
  }

  function candidateLabelAnchor() {
    return metricMmPerPx > 0 ? "middle" : "start";
  }

  function handleCandidateKeydown(event, id) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    if (manualDraftActive) return;
    if (splitDraftActive) return;
    if (selectionLocked && id !== selectedCandidateId) return;
    onSelect?.(id);
  }

  function handleCandidateSelect(id) {
    if (manualDraftActive) return;
    if (splitDraftActive) return;
    if (selectionLocked && id !== selectedCandidateId) return;
    onSelect?.(id);
  }

  function mergeSelected(id) {
    return mergeDraftCandidateIds.includes(id);
  }

  function svgPointFromEvent(event) {
    const svg = event.currentTarget.ownerSVGElement ?? event.currentTarget;
    const bounds = svg?.getBoundingClientRect();
    if (!bounds || bounds.width <= 0 || bounds.height <= 0) return null;
    return {
      x: viewX() + ((event.clientX - bounds.left) / bounds.width) * viewWidth(),
      y: viewY() + ((event.clientY - bounds.top) / bounds.height) * viewHeight()
    };
  }

  function handleCanvasClick(event) {
    if (splitDraftActive) {
      const point = svgPointFromEvent(event);
      if (!point) return;
      onSplitDraftPoint?.(point);
      return;
    }
    if (!manualDraftActive) return;
    const point = svgPointFromEvent(event);
    if (!point) return;
    onManualDraftPoint?.(point);
  }

  function handleCanvasPointerMove(event) {
    if (splitDraftActive) {
      const point = svgPointFromEvent(event);
      if (!point) return;
      onSplitDraftHover?.(point);
      return;
    }
    if (!manualDraftActive) return;
    const point = svgPointFromEvent(event);
    if (!point) return;
    onManualDraftHover?.(point);
  }

  function handleCanvasPointerLeave() {
    onSplitDraftLeave?.();
    onManualDraftLeave?.();
  }

  function handleCanvasKeydown(event) {
    if (splitDraftActive && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      onSplitDraftPoint?.({ x: imageWidth / 2, y: imageHeight / 2 });
      return;
    }
    if (!manualDraftActive || (event.key !== "Enter" && event.key !== " ")) return;
    event.preventDefault();
    onManualDraftPoint?.({ x: imageWidth / 2, y: imageHeight / 2 });
  }

  function handleManualDraftPointDoubleClick(event, index) {
    event.preventDefault();
    event.stopPropagation();
    onManualDraftPointDelete?.(index);
  }

  function handleManualDraftPointKeydown(event, index) {
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    event.preventDefault();
    event.stopPropagation();
    onManualDraftPointDelete?.(index);
  }

  function handleSnapEdgeKeydown(event, key) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onSelectSnapEdge?.(key);
  }

  function handleWallSegmentKeydown(event, id) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onSnapToWallSegment?.(id);
  }

  function handleCandidateEdgeClick(event, id, edgeIndex) {
    event.stopPropagation();
    const point = svgPointFromEvent(event);
    if (!point) return;
    onCandidateVertexAdd?.(id, edgeIndex, point);
    suppressInsertedVertexDoubleClick = {
      id,
      index: edgeIndex + 1,
      until: Date.now() + VERTEX_INSERT_DOUBLE_CLICK_SUPPRESS_MS
    };
  }

  function handleOutlineEdgeClick(event, edgeIndex) {
    event.stopPropagation();
    const point = svgPointFromEvent(event);
    if (!point) return;
    onManualOutlineVertexAdd?.(edgeIndex, point);
  }

  function handleOutlineEdgeKeydown(event, edge) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onManualOutlineVertexAdd?.(edge.index, {
      x: (edge.x1 + edge.x2) / 2,
      y: (edge.y1 + edge.y2) / 2
    });
  }

  function handleOutlineVertexPointerDown(event, index) {
    event.stopPropagation();
    draggedOutlineVertex = {
      index,
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function handleOutlineVertexDoubleClick(event, index) {
    event.preventDefault();
    event.stopPropagation();
    onManualOutlineVertexDelete?.(index);
  }

  function handleOutlineVertexKeydown(event, index) {
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    event.preventDefault();
    event.stopPropagation();
    onManualOutlineVertexDelete?.(index);
  }

  function handleCandidateEdgeKeydown(event, id, edge) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onCandidateVertexAdd?.(id, edge.index, {
      x: (edge.x1 + edge.x2) / 2,
      y: (edge.y1 + edge.y2) / 2
    });
    suppressInsertedVertexDoubleClick = {
      id,
      index: edge.index + 1,
      until: Date.now() + VERTEX_INSERT_DOUBLE_CLICK_SUPPRESS_MS
    };
  }

  function handleCandidateVertexDoubleClick(event, id, index) {
    if (
      Date.now() < suppressVertexDoubleClickUntil ||
      (
        suppressInsertedVertexDoubleClick &&
        Date.now() < suppressInsertedVertexDoubleClick.until &&
        suppressInsertedVertexDoubleClick.id === id &&
        suppressInsertedVertexDoubleClick.index === index
      )
    ) {
      event.preventDefault();
      event.stopPropagation();
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    onCandidateVertexDelete?.(id, index);
  }

  function handleCandidateVertexKeydown(event, id, index) {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      event.stopPropagation();
      onCandidateVertexSelect?.(id, index);
      return;
    }
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    event.preventDefault();
    event.stopPropagation();
    onCandidateVertexDelete?.(id, index);
  }

  function handleCandidateVertexPointerDown(event, candidate, index) {
    event.stopPropagation();
    draggedVertex = {
      id: candidate.id,
      index,
      pointerId: event.pointerId,
      target: event.currentTarget,
      startClientX: event.clientX,
      startClientY: event.clientY,
      moved: false
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
    onCandidateVertexMoveStart?.(candidate.id, index);
  }

  function handleSplitPointPointerDown(event, index) {
    event.stopPropagation();
    draggedSplitPoint = { index, pointerId: event.pointerId, target: event.currentTarget };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function handleVertexPointerMove(event) {
    if (draggedSplitPoint) {
      const point = svgPointFromEvent(event);
      if (!point) return;
      onSplitDraftPointMove?.(draggedSplitPoint.index, point);
      return;
    }
    if (draggedOutlineVertex) {
      const point = svgPointFromEvent(event);
      if (!point) return;
      onManualOutlineVertexMove?.(draggedOutlineVertex.index, point);
      return;
    }
    if (!draggedVertex) return;
    const point = svgPointFromEvent(event);
    if (!point) return;
    if (Math.hypot(event.clientX - draggedVertex.startClientX, event.clientY - draggedVertex.startClientY) > VERTEX_DRAG_CLICK_TOLERANCE_PX) {
      draggedVertex.moved = true;
    }
    onCandidateVertexMove?.(draggedVertex.id, draggedVertex.index, point);
  }

  function handleVertexPointerUp(_event) {
    if (draggedSplitPoint) {
      draggedSplitPoint.target?.releasePointerCapture?.(draggedSplitPoint.pointerId);
      draggedSplitPoint = null;
      return;
    }
    if (draggedOutlineVertex) {
      draggedOutlineVertex.target?.releasePointerCapture?.(draggedOutlineVertex.pointerId);
      draggedOutlineVertex = null;
      return;
    }
    if (!draggedVertex) return;
    const id = draggedVertex.id;
    const moved = draggedVertex.moved;
    draggedVertex.target?.releasePointerCapture?.(draggedVertex.pointerId);
    draggedVertex = null;
    if (moved) {
      suppressVertexDoubleClickUntil = Date.now() + VERTEX_DRAG_DOUBLE_CLICK_SUPPRESS_MS;
    }
    onCandidateVertexMoveEnd?.(id);
  }
</script>

<svg
  bind:this={candidateLayer}
  class={`floorplan-candidate-layer ${focusSelectedCandidate ? "focus-selected-candidate" : ""}`}
  viewBox={`${viewX()} ${viewY()} ${viewWidth()} ${viewHeight()}`}
  preserveAspectRatio="none"
  style={transformStyle}
  role="presentation"
  onpointermove={handleVertexPointerMove}
  onpointerup={handleVertexPointerUp}
  onpointercancel={handleVertexPointerUp}
  aria-label={ariaLabels.candidateLayer}
>
  {#if showMetricGrid && metricMmPerPx > 0 && metricReady}
    <defs>
      <pattern
        id="manual-floorplan-minor-grid"
        width={metricGridStepPx()}
        height={metricGridStepPx()}
        patternUnits="userSpaceOnUse"
      >
        <line
          x1="0" y1="0" x2={metricGridStepPx()} y2="0"
          class="floorplan-manual-grid-minor"
          style={`stroke-width: ${metricSize(0.5)}px;`}
        />
        <line
          x1="0" y1="0" x2="0" y2={metricGridStepPx()}
          class="floorplan-manual-grid-minor"
          style={`stroke-width: ${metricSize(0.5)}px;`}
        />
      </pattern>
      <pattern
        id="manual-floorplan-snap-grid"
        width={metricSnapStepPx()}
        height={metricSnapStepPx()}
        patternUnits="userSpaceOnUse"
      >
        <line
          x1="0" y1="0" x2={metricSnapStepPx()} y2="0"
          class="floorplan-manual-grid-snap"
          style={`stroke-width: ${metricSize(0.8)}px;`}
        />
        <line
          x1="0" y1="0" x2="0" y2={metricSnapStepPx()}
          class="floorplan-manual-grid-snap"
          style={`stroke-width: ${metricSize(0.8)}px;`}
        />
      </pattern>
      <pattern
        id="manual-floorplan-major-grid"
        width={metricMajorStepPx()}
        height={metricMajorStepPx()}
        patternUnits="userSpaceOnUse"
      >
        <line
          x1="0" y1="0" x2={metricMajorStepPx()} y2="0"
          class="floorplan-manual-grid-major"
          style={`stroke-width: ${metricSize(1.2)}px;`}
        />
        <line
          x1="0" y1="0" x2="0" y2={metricMajorStepPx()}
          class="floorplan-manual-grid-major"
          style={`stroke-width: ${metricSize(1.2)}px;`}
        />
      </pattern>
    </defs>
    <g class="floorplan-manual-grid" aria-hidden="true">
      {#if showMetricMinorGrid()}
        <rect x="0" y="0" width={imageWidth} height={imageHeight} fill="url(#manual-floorplan-minor-grid)" />
      {/if}
      <rect x="0" y="0" width={imageWidth} height={imageHeight} fill="url(#manual-floorplan-snap-grid)" />
      <rect x="0" y="0" width={imageWidth} height={imageHeight} fill="url(#manual-floorplan-major-grid)" />
    </g>
  {/if}

  {#if manualDraftActive || splitDraftActive}
    <rect
      class={splitDraftActive ? "floorplan-split-draft-hit" : "floorplan-manual-draft-hit"}
      x="0"
      y="0"
      width={imageWidth}
      height={imageHeight}
      role="button"
      tabindex="0"
      aria-label={splitDraftActive ? ariaLabels.addSplitPoint : ariaLabels.addManualPoint}
      onclick={handleCanvasClick}
      onpointermove={handleCanvasPointerMove}
      onpointerleave={handleCanvasPointerLeave}
      onkeydown={handleCanvasKeydown}
    />
  {/if}

  {#if showWallMaskCells}
    <g class="floorplan-wall-mask-cells">
      {#each wallMaskCells as cell}
        <rect class={`source-${cell.source}`} x={cell.x} y={cell.y} width={cell.width} height={cell.height}>
          <title>
            {`x=${Math.round(cell.x)}, y=${Math.round(cell.y)} / grid=${cell.gridX},${cell.gridY} / ${cell.reason} / dark=${cell.darkPixels}`}
          </title>
        </rect>
      {/each}
    </g>
  {/if}

  {#if showRoomSizeBounds}
    <g class="floorplan-room-size-bounds">
      {#each candidates.filter((candidate) => candidate.status !== "rejected" && candidate.id === selectedCandidateId) as candidate}
        {@const bounds = candidateBounds(candidate)}
        <rect x={bounds.x} y={bounds.y} width={bounds.width} height={bounds.height}>
          <title>{`${candidate.name || "room"} size bbox · ${Math.round(bounds.width)} x ${Math.round(bounds.height)}px`}</title>
        </rect>
      {/each}
    </g>
  {/if}

  {#each candidates.filter((candidate) => candidate.status !== "rejected") as candidate}
    <g
      class={`floorplan-candidate-group ${candidate.status} ${selectedCandidateId === candidate.id ? "selected" : ""} ${mergeSelected(candidate.id) ? "merge-selected" : ""}`}
      style={splitDraftActive || manualDraftActive ? "pointer-events: none;" : ""}
      role="button"
      tabindex="0"
      onclick={() => handleCandidateSelect(candidate.id)}
      onkeydown={(event) => handleCandidateKeydown(event, candidate.id)}
    >
      <polygon class="floorplan-candidate-shape" points={roomCandidatePointString(candidate)} />
      <text
        class="floorplan-candidate-label"
        x={labelX(candidate)}
        y={labelY(candidate)}
        text-anchor={candidateLabelAnchor()}
        dominant-baseline={metricMmPerPx > 0 ? "central" : "auto"}
        style={metricTextStyle(15)}
      >{candidate.name}</text>
    </g>
  {/each}

  {#if metricOutlinePoints.length >= 3}
    {#each candidates.filter((candidate) => candidate.status !== "rejected") as candidate}
      <polygon class="floorplan-manual-room-wall" points={roomCandidatePointString(candidate)} />
    {/each}
    <polygon class="floorplan-manual-outline" points={pointList(metricOutlinePoints)} />
    {#each metricLabels(metricOutlinePoints, true) as label}
      {@const position = metricLabelPosition(label)}
      <text
        class="floorplan-metric-edge-label"
        x={position.x}
        y={position.y}
        text-anchor="middle"
        dominant-baseline="central"
        style={metricTextStyle(13)}
      >{label.text}</text>
    {/each}
  {/if}

  {#if manualOutlineEditable && metricOutlinePoints.length >= 3}
    {#each candidateEditEdges({ shape: "polygon", points: metricOutlinePoints }) as edge}
      <line
        class="floorplan-candidate-edit-edge floorplan-outline-edit-edge"
        x1={edge.x1}
        y1={edge.y1}
        x2={edge.x2}
        y2={edge.y2}
        role="button"
        tabindex="0"
        onclick={(event) => handleOutlineEdgeClick(event, edge.index)}
        onkeydown={(event) => handleOutlineEdgeKeydown(event, edge)}
      />
    {/each}
    {#each metricOutlinePoints as point, index}
      <g class="floorplan-candidate-vertex manual floorplan-outline-edit-vertex">
        <circle
          class="floorplan-vertex-hit"
          cx={point[0]}
          cy={point[1]}
          r={metricSize(11)}
          role="button"
          tabindex="0"
          onpointerdown={(event) => handleOutlineVertexPointerDown(event, index)}
          ondblclick={(event) => handleOutlineVertexDoubleClick(event, index)}
          onkeydown={(event) => handleOutlineVertexKeydown(event, index)}
        />
        <circle class="floorplan-vertex-marker" cx={point[0]} cy={point[1]} r={metricSize(7)} />
        <text
          x={point[0] + metricSize(9)}
          y={point[1] - metricSize(9)}
          style={metricTextStyle(12)}
        >{index + 1}</text>
      </g>
    {/each}
  {/if}

  {#if allowCandidateEditing && !splitDraftActive && !mergeDraftActive}
  {#each candidates.filter((candidate) => candidate.status !== "rejected" && candidate.id === selectedCandidateId) as candidate}
    {#if !snapEdges.length}
      {#each candidateEditEdges(candidate) as edge}
        <line
          class="floorplan-candidate-edit-edge"
          x1={edge.x1}
          y1={edge.y1}
          x2={edge.x2}
          y2={edge.y2}
          role="button"
          tabindex="0"
          onclick={(event) => handleCandidateEdgeClick(event, candidate.id, edge.index)}
          onkeydown={(event) => handleCandidateEdgeKeydown(event, candidate.id, edge)}
        />
      {/each}
    {/if}
    {#each roomCandidatePoints(candidate) as point, index}
      <g class="floorplan-candidate-vertex manual">
        <circle
          class="floorplan-vertex-hit"
          cx={point[0]}
          cy={point[1]}
          r={metricSize(11)}
          role="button"
          tabindex="0"
          onpointerdown={(event) => handleCandidateVertexPointerDown(event, candidate, index)}
          ondblclick={(event) => handleCandidateVertexDoubleClick(event, candidate.id, index)}
          onkeydown={(event) => handleCandidateVertexKeydown(event, candidate.id, index)}
        />
        <circle
          class="floorplan-vertex-marker"
          cx={point[0]}
          cy={point[1]}
          r={metricSize(7)}
          data-active={selectedCandidateVertexIndex === index ? "true" : "false"}
        />
        <text
          x={point[0] + metricSize(9)}
          y={point[1] - metricSize(9)}
          style={metricTextStyle(12)}
        >{index + 1}</text>
      </g>
    {/each}
  {/each}
  {/if}

  {#if splitDraftActive}
    {@const splitPreviewPoints = splitDraftPoints.length === 1 && splitDraftHoverPoint ? [...splitDraftPoints, [splitDraftHoverPoint.x, splitDraftHoverPoint.y]] : splitDraftPoints}
    {#if splitPreviewPoints.length >= 2}
      <line
        class="floorplan-split-draft-line"
        x1={splitPreviewPoints[0][0]}
        y1={splitPreviewPoints[0][1]}
        x2={splitPreviewPoints[1][0]}
        y2={splitPreviewPoints[1][1]}
      />
    {/if}
    {#each splitDraftPoints as point, index}
      <g class="floorplan-split-draft-point">
        <circle
          cx={point[0]}
          cy={point[1]}
          r="6"
          role="button"
          tabindex="0"
          onpointerdown={(event) => handleSplitPointPointerDown(event, index)}
        />
        <text x={point[0] + 8} y={point[1] - 8}>{index + 1}</text>
      </g>
    {/each}
    {#if splitDraftPoints.length === 1 && splitDraftHoverPoint}
      <g class="floorplan-split-draft-hover">
        <circle cx={splitDraftHoverPoint.x} cy={splitDraftHoverPoint.y} r="5" />
      </g>
    {/if}
  {/if}

  {#if manualDraftPoints.length}
    {@const manualPreviewPoints = manualDraftActive && manualDraftHoverPoint
      ? [...manualDraftPoints, [manualDraftHoverPoint.x, manualDraftHoverPoint.y]]
      : manualDraftPoints}
    {#if manualDraftPoints.length >= 3}
      <polygon class="floorplan-manual-draft-fill" points={pointList(manualDraftPoints)} />
    {/if}
    <polyline class="floorplan-manual-draft-line" points={pointList(manualPreviewPoints)} />
    {#each metricLabels(manualPreviewPoints) as label}
      {@const position = metricLabelPosition(label)}
      <text
        class="floorplan-metric-edge-label"
        x={position.x}
        y={position.y}
        text-anchor="middle"
        dominant-baseline="central"
        style={metricTextStyle(13)}
      >{label.text}</text>
    {/each}
    {#each manualDraftPoints as point, index}
      <g class="floorplan-manual-draft-point">
        <circle
          class="floorplan-vertex-hit"
          cx={point[0]}
          cy={point[1]}
          r={metricSize(11)}
          role="button"
          tabindex="0"
          ondblclick={(event) => handleManualDraftPointDoubleClick(event, index)}
          onkeydown={(event) => handleManualDraftPointKeydown(event, index)}
        />
        <circle class="floorplan-vertex-marker" cx={point[0]} cy={point[1]} r={metricSize(6)} />
        <text
          x={point[0] + metricSize(8)}
          y={point[1] - metricSize(8)}
          style={metricTextStyle(12)}
        >{index + 1}</text>
      </g>
    {/each}
  {/if}

  {#if manualDraftActive && manualDraftHoverPoint}
    <g class="floorplan-manual-draft-hover">
      <circle cx={manualDraftHoverPoint.x} cy={manualDraftHoverPoint.y} r={metricSize(7)} />
    </g>
  {/if}

  {#if !splitDraftActive && !mergeDraftActive}
  {#each snapEdges as edge}
    <line
      class={`floorplan-snap-edge ${selectedSnapEdgeKey === edge.key ? "selected" : ""}`}
      x1={edge.x1}
      y1={edge.y1}
      x2={edge.x2}
      y2={edge.y2}
      role="button"
      tabindex="0"
      onclick={() => onSelectSnapEdge?.(edge.key)}
      onkeydown={(event) => handleSnapEdgeKeydown(event, edge.key)}
    />
  {/each}

  {#each snapWallSegments as segment}
    <line
      class="floorplan-snap-wall"
      x1={segment.x1}
      y1={segment.y1}
      x2={segment.x2}
      y2={segment.y2}
      role="button"
      tabindex="0"
      onclick={() => onSnapToWallSegment?.(segment.id)}
      onkeydown={(event) => handleWallSegmentKeydown(event, segment.id)}
    />
  {/each}
  {/if}

  {#if showRoomSizeBounds}
    <g class="floorplan-room-size-bounds top">
      {#each candidates.filter((candidate) => candidate.status !== "rejected" && candidate.id === selectedCandidateId) as candidate}
        {@const bounds = candidateBounds(candidate)}
        <rect x={bounds.x} y={bounds.y} width={bounds.width} height={bounds.height}>
          <title>{`${candidate.name || "room"} size bbox · ${Math.round(bounds.width)} x ${Math.round(bounds.height)}px`}</title>
        </rect>
        <text x={bounds.x + 6} y={Math.max(14, bounds.y - 8)}>
          {Math.round(bounds.width)} x {Math.round(bounds.height)}px
        </text>
      {/each}
    </g>
  {/if}

  {#if showOcrItems}
    {#each ocrItems.filter((item) => item.bbox) as item}
      <g class={`floorplan-ocr-item ${item.kind ?? "noise"}`}>
        <rect
          x={item.bbox.x0}
          y={item.bbox.y0}
          width={Math.max(1, item.bbox.x1 - item.bbox.x0)}
          height={Math.max(1, item.bbox.y1 - item.bbox.y0)}
          rx="3"
        />
        <text x={item.bbox.x0 + 3} y={Math.max(12, item.bbox.y0 - 4)}>
          {item.text} {item.confidence}%
        </text>
      </g>
    {/each}
  {/if}
</svg>
