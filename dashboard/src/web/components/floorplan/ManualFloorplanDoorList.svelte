<script>
  let {
    text,
    doors = [],
    placementActive = false,
    placementKind = "hinged",
    placementError = null,
    selectedDoorId = "",
    onKindChange,
    onSelect,
    onRemove
  } = $props();

  function doorLabel(door, index) {
    const kind = door.kind === "sliding" ? text.manualSlidingDoor : text.manualHingedDoor;
    return `${kind} ${index + 1}`;
  }

  function errorMessage() {
    if (placementError === "wall_too_short") return text.manualDoorWallTooShort;
    if (placementError === "door_overlap") return text.manualDoorOverlap;
    if (placementError === "wall_not_found") return text.manualDoorWallRequired;
    return "";
  }
</script>

<div class="floorplan-door-card floorplan-fixed-text">
  <strong>{text.manualDoorsTitle}</strong>
  {#if placementActive}
    <div class="floorplan-door-kind-control" role="group" aria-label={text.manualDoorType}>
      <button
        type="button"
        data-active={placementKind === "hinged" ? "true" : "false"}
        onclick={() => onKindChange?.("hinged")}
      >{text.manualHingedDoor}</button>
      <button
        type="button"
        data-active={placementKind === "sliding" ? "true" : "false"}
        onclick={() => onKindChange?.("sliding")}
      >{text.manualSlidingDoor}</button>
    </div>
    <span data-tone={placementError ? "error" : "idle"}>
      {errorMessage() || text.manualDoorPlacementHint}
    </span>
  {/if}

  {#if doors.length}
    <ul>
      {#each doors as door, index (door.id)}
        <li data-selected={door.id === selectedDoorId ? "true" : "false"}>
          <button
            type="button"
            class="floorplan-door-select"
            onclick={() => onSelect?.(door.id)}
          >{doorLabel(door, index)}</button>
          <button
            type="button"
            class="floorplan-door-remove"
            title={text.manualDeleteDoor}
            aria-label={`${doorLabel(door, index)} ${text.manualDeleteDoor}`}
            onclick={() => onRemove?.(door.id)}
          >&times;</button>
        </li>
      {/each}
    </ul>
  {:else}
    <span>{text.manualNoDoors}</span>
  {/if}
</div>
