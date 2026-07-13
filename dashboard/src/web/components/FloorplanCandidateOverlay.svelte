<script>
  import { roomCandidatePoints, roomCandidatePointString } from "../floorplan/room-candidate-points";

  let {
    transformStyle = "",
    candidates = [],
    selectedCandidateId = "",
    selectedCandidateVertexIndex = -1,
    focusSelectedCandidate = false,
    selectionLocked = false,
    imageWidth = 1,
    imageHeight = 1,
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
    onCandidateVertexMoveEnd
  } = $props();

  const VERTEX_DRAG_CLICK_TOLERANCE_PX = 4;
  const VERTEX_DRAG_DOUBLE_CLICK_SUPPRESS_MS = 350;
  const VERTEX_INSERT_DOUBLE_CLICK_SUPPRESS_MS = 700;

  let draggedVertex = null;
  let draggedSplitPoint = null;
  let suppressVertexDoubleClickUntil = 0;
  let suppressInsertedVertexDoubleClick = null;

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

  function labelX(candidate) {
    return candidate.rect.x + 8;
  }

  function labelY(candidate) {
    return candidate.rect.y + 18;
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
      x: ((event.clientX - bounds.left) / bounds.width) * imageWidth,
      y: ((event.clientY - bounds.top) / bounds.height) * imageHeight
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
  class={`floorplan-candidate-layer ${focusSelectedCandidate ? "focus-selected-candidate" : ""}`}
  viewBox={`0 0 ${imageWidth} ${imageHeight}`}
  preserveAspectRatio="none"
  style={transformStyle}
  role="presentation"
  onpointermove={handleVertexPointerMove}
  onpointerup={handleVertexPointerUp}
  onpointercancel={handleVertexPointerUp}
  aria-label={ariaLabels.candidateLayer}
>
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
      <text class="floorplan-candidate-label" x={labelX(candidate)} y={labelY(candidate)}>{candidate.name}</text>
    </g>
  {/each}

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
            cx={point[0]}
            cy={point[1]}
            r="7"
          data-active={selectedCandidateVertexIndex === index ? "true" : "false"}
          role="button"
          tabindex="0"
          onpointerdown={(event) => handleCandidateVertexPointerDown(event, candidate, index)}
          ondblclick={(event) => handleCandidateVertexDoubleClick(event, candidate.id, index)}
          onkeydown={(event) => handleCandidateVertexKeydown(event, candidate.id, index)}
        />
        <text x={point[0] + 8} y={point[1] - 8}>{index + 1}</text>
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
    {#if manualDraftPoints.length >= 3}
      <polygon class="floorplan-manual-draft-fill" points={pointList(manualDraftPoints)} />
    {/if}
    <polyline class="floorplan-manual-draft-line" points={pointList(manualDraftPoints)} />
    {#each manualDraftPoints as point, index}
      <g class="floorplan-manual-draft-point">
        <circle
          cx={point[0]}
          cy={point[1]}
          r="5"
          role="button"
          tabindex="0"
          ondblclick={(event) => handleManualDraftPointDoubleClick(event, index)}
          onkeydown={(event) => handleManualDraftPointKeydown(event, index)}
        />
        <text x={point[0] + 8} y={point[1] - 8}>{index + 1}</text>
      </g>
    {/each}
  {/if}

  {#if manualDraftActive && manualDraftHoverPoint}
    <g class="floorplan-manual-draft-hover">
      <circle cx={manualDraftHoverPoint.x} cy={manualDraftHoverPoint.y} r="6" />
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
