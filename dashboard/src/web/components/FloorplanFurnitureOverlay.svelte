<script>
  let {
    objects = [],
    assets = [],
    selectedObjectId = "",
    imageWidth = 1,
    imageHeight = 1,
    bounds = null,
    editable = false,
    onSelect,
    onMove
  } = $props();

  let drag = null;
  const MIN_OBJECT_SIZE_PX = 18;
  const ROTATE_HANDLE_OFFSET_PX = 22;

  function assetFor(id) {
    return assets.find((asset) => asset.id === id) ?? null;
  }

  function objectCenter(object) {
    return {
      x: object.xPx + object.widthPx / 2,
      y: object.yPx + object.heightPx / 2
    };
  }

  function pointerPoint(event) {
    const svg = event.currentTarget.ownerSVGElement ?? event.currentTarget;
    const rect = svg.getBoundingClientRect();
    return {
      x: ((event.clientX - rect.left) / Math.max(1, rect.width)) * imageWidth,
      y: ((event.clientY - rect.top) / Math.max(1, rect.height)) * imageHeight
    };
  }

  function clampObject(object, x, y, width = object.widthPx, height = object.heightPx) {
    const limit = bounds ?? { x: 0, y: 0, width: imageWidth, height: imageHeight };
    const safeWidth = Math.min(Math.max(MIN_OBJECT_SIZE_PX, width), limit.width);
    const safeHeight = Math.min(Math.max(MIN_OBJECT_SIZE_PX, height), limit.height);
    return {
      xPx: Math.max(limit.x, Math.min(limit.x + limit.width - safeWidth, x)),
      yPx: Math.max(limit.y, Math.min(limit.y + limit.height - safeHeight, y)),
      widthPx: safeWidth,
      heightPx: safeHeight
    };
  }

  function beginDrag(event, object) {
    event.preventDefault();
    event.stopPropagation();
    onSelect?.(object.id);
    if (!editable) return;
    const point = pointerPoint(event);
    drag = {
      mode: "move",
      id: object.id,
      offsetX: point.x - object.xPx,
      offsetY: point.y - object.yPx,
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function beginResize(event, object, corner) {
    event.preventDefault();
    event.stopPropagation();
    onSelect?.(object.id);
    if (!editable) return;
    const point = pointerPoint(event);
    drag = {
      mode: "resize",
      id: object.id,
      corner,
      startX: point.x,
      startY: point.y,
      object: { ...object },
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function beginRotate(event, object) {
    event.preventDefault();
    event.stopPropagation();
    onSelect?.(object.id);
    if (!editable) return;
    const point = pointerPoint(event);
    drag = {
      mode: "rotate",
      id: object.id,
      center: objectCenter(object),
      startAngle: angleForPoint(objectCenter(object), point),
      startRotation: object.rotationDeg ?? 0,
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function updateDrag(event) {
    if (!drag) return;
    const object = objects.find((item) => item.id === drag.id);
    if (!object) return;
    const point = pointerPoint(event);
    if (drag.mode === "rotate") {
      onMove?.(object.id, rotateObject(point));
    } else if (drag.mode === "resize") {
      onMove?.(object.id, resizeObject(point));
    } else {
      onMove?.(object.id, clampObject(object, point.x - drag.offsetX, point.y - drag.offsetY));
    }
  }

  function angleForPoint(center, point) {
    return Math.atan2(point.y - center.y, point.x - center.x) * 180 / Math.PI;
  }

  function rotateObject(point) {
    const angle = angleForPoint(drag.center, point);
    return {
      rotationDeg: drag.startRotation + angle - drag.startAngle
    };
  }

  function resizeObject(point) {
    const original = drag.object;
    const dx = point.x - drag.startX;
    const dy = point.y - drag.startY;
    let x = original.xPx;
    let y = original.yPx;
    let width = original.widthPx;
    let height = original.heightPx;

    if (drag.corner.includes("e")) width = original.widthPx + dx;
    if (drag.corner.includes("s")) height = original.heightPx + dy;
    if (drag.corner.includes("w")) {
      x = original.xPx + dx;
      width = original.widthPx - dx;
    }
    if (drag.corner.includes("n")) {
      y = original.yPx + dy;
      height = original.heightPx - dy;
    }

    if (width < MIN_OBJECT_SIZE_PX) {
      if (drag.corner.includes("w")) x -= MIN_OBJECT_SIZE_PX - width;
      width = MIN_OBJECT_SIZE_PX;
    }
    if (height < MIN_OBJECT_SIZE_PX) {
      if (drag.corner.includes("n")) y -= MIN_OBJECT_SIZE_PX - height;
      height = MIN_OBJECT_SIZE_PX;
    }

    return clampObject(original, x, y, width, height);
  }

  function endDrag() {
    if (!drag) return;
    drag.target?.releasePointerCapture?.(drag.pointerId);
    drag = null;
  }

  function handleKeydown(event, object) {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      onSelect?.(object.id);
    }
  }

  function resizeHandles(object) {
    return [
      { key: "nw", x: object.xPx, y: object.yPx },
      { key: "ne", x: object.xPx + object.widthPx, y: object.yPx },
      { key: "se", x: object.xPx + object.widthPx, y: object.yPx + object.heightPx },
      { key: "sw", x: object.xPx, y: object.yPx + object.heightPx }
    ];
  }

  function rotateHandle(object) {
    return {
      x: object.xPx + object.widthPx / 2,
      y: object.yPx - ROTATE_HANDLE_OFFSET_PX
    };
  }
</script>

<svg
  class="floorplan-furniture-layer"
  data-editable={editable ? "true" : "false"}
  viewBox={`0 0 ${imageWidth} ${imageHeight}`}
  preserveAspectRatio="none"
  role="presentation"
  onpointermove={updateDrag}
  onpointerup={endDrag}
  onpointercancel={endDrag}
>
  {#each objects as object (object.id)}
    {@const asset = assetFor(object.asset)}
    {#if asset}
      {@const center = objectCenter(object)}
      <g
        class={`floorplan-furniture-object ${selectedObjectId === object.id ? "selected" : ""}`}
        transform={`rotate(${object.rotationDeg ?? 0} ${center.x} ${center.y})`}
        role="button"
        tabindex="0"
        aria-label={asset.label}
        onpointerdown={(event) => beginDrag(event, object)}
        onkeydown={(event) => handleKeydown(event, object)}
      >
        <rect
          class="floorplan-furniture-hitbox"
          x={object.xPx}
          y={object.yPx}
          width={object.widthPx}
          height={object.heightPx}
          rx="3"
        />
        <image
          href={asset.url}
          x={object.xPx}
          y={object.yPx}
          width={object.widthPx}
          height={object.heightPx}
          preserveAspectRatio="xMidYMid meet"
        />
        {#if selectedObjectId === object.id}
          <rect
            class="floorplan-furniture-selection"
            x={object.xPx}
            y={object.yPx}
            width={object.widthPx}
            height={object.heightPx}
            rx="3"
          />
          {#if editable}
            {@const rotate = rotateHandle(object)}
            <line
              class="floorplan-furniture-rotate-guide"
              x1={object.xPx + object.widthPx / 2}
              y1={object.yPx}
              x2={rotate.x}
              y2={rotate.y}
            />
            <circle
              class="floorplan-furniture-rotate-handle"
              cx={rotate.x}
              cy={rotate.y}
              r="6"
              role="button"
              tabindex="0"
              aria-label="가구 자유 회전"
              onpointerdown={(event) => beginRotate(event, object)}
            />
            {#each resizeHandles(object) as handle}
              <circle
                class={`floorplan-furniture-resize-handle ${handle.key}`}
                cx={handle.x}
                cy={handle.y}
                r="5"
                role="button"
                tabindex="0"
                aria-label="가구 크기 조절"
                onpointerdown={(event) => beginResize(event, object, handle.key)}
              />
            {/each}
          {/if}
        {/if}
      </g>
    {/if}
  {/each}
</svg>
