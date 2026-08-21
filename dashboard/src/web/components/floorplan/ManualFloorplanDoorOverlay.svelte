<script>
  let {
    doors = [],
    placementActive = false,
    selectedDoorId = "",
    imageWidth = 1,
    imageHeight = 1,
    viewBounds = null,
    mmPerPx = 10,
    transformStyle = "",
    ariaLabel = "",
    onPreview,
    onPlace,
    onSelect,
    onResize,
    onResizeCommit,
    onMove,
    onMoveCommit
  } = $props();

  let previewDoor = $state(null);
  let resizeState = $state(null);
  let moveState = $state(null);
  const displayedDoors = $derived(previewDoor
    ? [...doors, { ...previewDoor, id: "__door_preview" }]
    : doors);
  const handleRadius = $derived(Math.max(10, Math.min(imageWidth, imageHeight) * 0.008));
  const interactive = $derived(Boolean(placementActive || onPlace || onSelect || onResize || onMove));

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

  function pointerPoint(event) {
    const svg = event.currentTarget.ownerSVGElement ?? event.currentTarget;
    const bounds = svg?.getBoundingClientRect();
    if (!bounds || bounds.width <= 0 || bounds.height <= 0) return null;
    return {
      x: viewX() + ((event.clientX - bounds.left) / bounds.width) * viewWidth(),
      y: viewY() + ((event.clientY - bounds.top) / bounds.height) * viewHeight()
    };
  }

  function preview(event) {
    if (!placementActive || resizeState || moveState) return;
    const point = pointerPoint(event);
    previewDoor = point ? onPreview?.(point) ?? null : null;
  }

  function clearPreview() {
    if (!resizeState && !moveState) previewDoor = null;
  }

  function place(event) {
    const point = pointerPoint(event);
    if (!point) return;
    onPlace?.(point);
    previewDoor = onPreview?.(point) ?? null;
  }

  function handleKeydown(event) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onPlace?.({ x: imageWidth / 2, y: imageHeight / 2 });
  }

  function selectDoor(event, id) {
    if (id === "__door_preview") return;
    event.stopPropagation();
    onSelect?.(id);
  }

  function selectDoorKeydown(event, id) {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    onSelect?.(id);
  }

  function beginResize(event, id, handle) {
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    resizeState = { id, handle, pointerId: event.pointerId };
    previewDoor = null;
    onSelect?.(id);
  }

  function resize(event) {
    if (!resizeState || event.pointerId !== resizeState.pointerId) return;
    const point = pointerPoint(event);
    if (point) onResize?.(resizeState.id, resizeState.handle, point);
  }

  function finishResize(event) {
    if (!resizeState || event.pointerId !== resizeState.pointerId) return;
    const finished = resizeState;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
    resizeState = null;
    onResizeCommit?.(finished.id);
  }

  function beginMove(event, id) {
    if (id === "__door_preview" || event.button !== 0) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    moveState = { id, pointerId: event.pointerId };
    previewDoor = null;
    onSelect?.(id);
  }

  function moveSelectedDoor(event) {
    if (!moveState || event.pointerId !== moveState.pointerId) return;
    const point = pointerPoint(event);
    if (point) onMove?.(moveState.id, point);
  }

  function finishMove(event) {
    if (!moveState || event.pointerId !== moveState.pointerId) return;
    const finished = moveState;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
    moveState = null;
    onMoveCommit?.(finished.id);
  }

  function halfWidth(door) {
    return door.widthPx / 2;
  }

  function slidingOffset() {
    return Math.max(2, 80 / mmPerPx);
  }

  function hingedArc(door) {
    const half = halfWidth(door);
    return `M ${half} 0 A ${door.widthPx} ${door.widthPx} 0 0 0 ${-half} ${-door.widthPx}`;
  }
</script>

<svg
  class="floorplan-manual-door-layer"
  data-active={placementActive ? "true" : "false"}
  data-snapped={previewDoor ? "true" : "false"}
  data-interactive={interactive ? "true" : "false"}
  viewBox={`${viewX()} ${viewY()} ${viewWidth()} ${viewHeight()}`}
  preserveAspectRatio="none"
  style={transformStyle}
  role="presentation"
  onpointermove={preview}
  onpointerleave={clearPreview}
>
  {#if placementActive}
    <rect
      class="floorplan-manual-door-hit"
      x="0"
      y="0"
      width={imageWidth}
      height={imageHeight}
      role="button"
      tabindex="0"
      aria-label={ariaLabel}
      onclick={place}
      onkeydown={handleKeydown}
    />
  {/if}

  {#each displayedDoors as door (door.id)}
    {@const half = halfWidth(door)}
    {@const isPreview = door.id === "__door_preview"}
    {@const isSelected = !isPreview && door.id === selectedDoorId}
    <g
      class={`floorplan-manual-door ${door.kind}${isPreview ? " preview" : ""}${isSelected ? " selected" : ""}`}
      transform={`translate(${door.centerPx[0]} ${door.centerPx[1]}) rotate(${door.rotationDeg})`}
      role="button"
      aria-hidden={interactive ? undefined : "true"}
      tabindex={interactive && !isPreview ? 0 : -1}
      aria-label={ariaLabel}
      onclick={(event) => selectDoor(event, door.id)}
      onkeydown={(event) => selectDoorKeydown(event, door.id)}
      onpointerdown={(event) => beginMove(event, door.id)}
      onpointermove={moveSelectedDoor}
      onpointerup={finishMove}
      onpointercancel={finishMove}
    >
      <line class="floorplan-manual-door-opening" x1={-half} y1="0" x2={half} y2="0" />
      {#if door.kind === "hinged"}
        <line class="floorplan-manual-door-panel" x1={-half} y1="0" x2={-half} y2={-door.widthPx} />
        <path class="floorplan-manual-door-swing" d={hingedArc(door)} />
      {:else}
        {@const offset = slidingOffset()}
        <line class="floorplan-manual-door-panel" x1={-half} y1={-offset} x2={door.widthPx * 0.08} y2={-offset} />
        <line class="floorplan-manual-door-panel" x1={-door.widthPx * 0.08} y1={offset} x2={half} y2={offset} />
        <line class="floorplan-manual-door-detail" x1={-door.widthPx * 0.08} y1={-offset * 1.7} x2={-door.widthPx * 0.08} y2={offset * 1.7} />
        <line class="floorplan-manual-door-detail" x1={door.widthPx * 0.08} y1={-offset * 1.7} x2={door.widthPx * 0.08} y2={offset * 1.7} />
      {/if}
      <circle class="floorplan-manual-door-post" cx={-half} cy="0" r="3.2" />
      <circle class="floorplan-manual-door-post" cx={half} cy="0" r="3.2" />
      {#if isSelected}
        <circle
          class="floorplan-manual-door-resize-handle"
          cx={-half}
          cy="0"
          r={handleRadius}
          role="slider"
          tabindex="0"
          aria-label={ariaLabel}
          aria-valuemin="0"
          aria-valuemax={imageWidth}
          aria-valuenow={door.widthPx}
          onpointerdown={(event) => beginResize(event, door.id, "start")}
          onpointermove={resize}
          onpointerup={finishResize}
          onpointercancel={finishResize}
        />
        <circle
          class="floorplan-manual-door-resize-handle"
          cx={half}
          cy="0"
          r={handleRadius}
          role="slider"
          tabindex="0"
          aria-label={ariaLabel}
          aria-valuemin="0"
          aria-valuemax={imageWidth}
          aria-valuenow={door.widthPx}
          onpointerdown={(event) => beginResize(event, door.id, "end")}
          onpointermove={resize}
          onpointerup={finishResize}
          onpointercancel={finishResize}
        />
      {/if}
    </g>
  {/each}
</svg>
