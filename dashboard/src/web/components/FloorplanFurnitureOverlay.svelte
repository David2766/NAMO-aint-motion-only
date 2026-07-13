<script>
  import {
    clampFurnitureObjectToBounds,
    constrainFurnitureObjectToRooms,
    furnitureCenter,
    isFurnitureObjectInsideRoom,
    resizeFurnitureObjectFromCorner
  } from "../floorplan/floorplan-furniture";

  let {
    objects = [],
    assets = [],
    selectedObjectId = "",
    imageWidth = 1,
    imageHeight = 1,
    bounds = null,
    rooms = [],
    editable = false,
    onSelect,
    onCommit
  } = $props();

  let drag = $state(null);
  let draftObject = $state(null);
  const MIN_OBJECT_SIZE_PX = 18;
  const ROTATE_HANDLE_OFFSET_PX = 22;

  function assetFor(id) {
    return assets.find((asset) => asset.id === id) ?? null;
  }

  function renderableObjects() {
    return (objects ?? []).filter((object) =>
      object &&
      typeof object === "object" &&
      typeof object.id === "string" &&
      typeof object.asset === "string"
    );
  }

  function pointerPoint(event) {
    const svg = event.currentTarget.ownerSVGElement ?? event.currentTarget;
    const rect = svg.getBoundingClientRect();
    return {
      x: ((event.clientX - rect.left) / Math.max(1, rect.width)) * imageWidth,
      y: ((event.clientY - rect.top) / Math.max(1, rect.height)) * imageHeight
    };
  }

  function constrainObject(proposed, previous) {
    const limit = bounds ?? { x: 0, y: 0, width: imageWidth, height: imageHeight };
    const safeProposed = {
      ...proposed,
      widthPx: Math.max(MIN_OBJECT_SIZE_PX, proposed.widthPx),
      heightPx: Math.max(MIN_OBJECT_SIZE_PX, proposed.heightPx)
    };
    if (drag?.constraintSuspended) {
      if (objectInsideAssignedRoom(safeProposed)) {
        drag = { ...drag, constraintSuspended: false };
        return constrainFurnitureObjectToRooms(safeProposed, previous, rooms, limit);
      }
      return clampFurnitureObjectToBounds(safeProposed, limit);
    }
    return constrainFurnitureObjectToRooms(safeProposed, previous, rooms, limit);
  }

  function objectInsideAssignedRoom(object) {
    const assignedRoom = rooms.find((room) => room.id === object.roomId);
    if (assignedRoom) return isFurnitureObjectInsideRoom(object, assignedRoom);
    return rooms.some((room) => isFurnitureObjectInsideRoom(object, room));
  }

  function moveObject(object, x, y) {
    return constrainObject({
      ...object,
      xPx: x,
      yPx: y
    }, draftObject ?? object);
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
      object: { ...object },
      offsetX: point.x - object.xPx,
      offsetY: point.y - object.yPx,
      pointerId: event.pointerId,
      target: event.currentTarget,
      constraintSuspended: rooms.length > 0 && !objectInsideAssignedRoom(object)
    };
    draftObject = { ...object };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function beginResize(event, object, corner) {
    event.preventDefault();
    event.stopPropagation();
    onSelect?.(object.id);
    if (!editable) return;
    drag = {
      mode: "resize",
      id: object.id,
      corner,
      object: { ...object },
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    draftObject = { ...object };
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
      center: furnitureCenter(object),
      startAngle: angleForPoint(furnitureCenter(object), point),
      startRotation: object.rotationDeg ?? 0,
      pointerId: event.pointerId,
      target: event.currentTarget
    };
    draftObject = { ...object };
    event.currentTarget.setPointerCapture?.(event.pointerId);
  }

  function updateDrag(event) {
    if (!drag) return;
    const object = draftObject ?? objects.find((item) => item.id === drag.id);
    if (!object) return;
    const point = pointerPoint(event);
    if (drag.mode === "rotate") {
      draftObject = constrainObject({ ...object, ...rotateObject(point) }, object);
    } else if (drag.mode === "resize") {
      draftObject = resizeObject(point);
    } else {
      draftObject = moveObject(object, point.x - drag.offsetX, point.y - drag.offsetY);
      drag = {
        ...drag,
        offsetX: point.x - draftObject.xPx,
        offsetY: point.y - draftObject.yPx
      };
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
    const resized = resizeFurnitureObjectFromCorner(drag.object, drag.corner, point, MIN_OBJECT_SIZE_PX);
    return constrainObject(resized, draftObject ?? drag.object);
  }

  function endDrag() {
    if (!drag) return;
    const finishedDrag = drag;
    const finalObject = draftObject;
    drag.target?.releasePointerCapture?.(drag.pointerId);
    drag = null;
    draftObject = null;
    if (finalObject) onCommit?.(finishedDrag.id, finalObject);
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
  {#each renderableObjects() as sourceObject (sourceObject.id)}
      {@const object = draftObject?.id === sourceObject.id ? draftObject : sourceObject}
      {@const asset = assetFor(object.asset)}
    {#if asset}
      {@const center = furnitureCenter(object)}
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
          preserveAspectRatio="none"
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
