<script>
  let {
    text,
    phase = "idle",
    activeTool = false,
    outlineEditing = false,
    roomCount = 0,
    roomsValid = true,
    onStartOutline,
    onStartRoom,
    onEditOutline
  } = $props();
</script>

<div class="floorplan-manual-guide floorplan-fixed-text" data-phase={phase}>
  {#if phase === "outline"}
    <strong>{text.manualOutlineTitle}</strong>
    <span>{activeTool ? text.manualOutlineHint : text.manualOutlineCancelled}</span>
    {#if !activeTool}
      <button type="button" onclick={onStartOutline}>{text.manualStartOutline}</button>
    {/if}
  {:else if phase === "rooms"}
    <strong>{outlineEditing ? text.manualOutlineEditTitle : text.manualRoomsTitle}</strong>
    <span>
      {outlineEditing
        ? text.manualOutlineEditHint
        : roomCount > 0 && !roomsValid
          ? text.manualRoomInvalid
          : roomCount > 0
          ? text.manualRoomsReady(roomCount)
          : text.manualRoomsHint}
    </span>
    {#if !activeTool}
      <div class="floorplan-manual-guide-actions">
        <button type="button" onclick={onStartRoom}>{text.manualAddRoom}</button>
        <button type="button" onclick={onEditOutline}>{text.manualEditOutline}</button>
      </div>
    {/if}
  {/if}
</div>
