<script>
  let {
    text,
    activeTool,
    onRooms,
    onScale,
    onRadar,
    onZones,
    onCalibration,
    onFurniture
  } = $props();
</script>

<div class="floorplan-stored-edit-mode-tools">
  <button
    type="button"
    data-active={activeTool === "rooms" ? "true" : "false"}
    onclick={onRooms}
  >
    {text.storedRoomEdit}
  </button>
  <button
    type="button"
    data-active={activeTool === "scale" ? "true" : "false"}
    onclick={onScale}
  >
    {text.totalFloorplanSize}
  </button>
  <button
    type="button"
    data-active={activeTool === "radar" ? "true" : "false"}
    onclick={onRadar}
  >
    {text.storedRadarPosition}
  </button>
  <button
    type="button"
    data-active={activeTool === "zones" ? "true" : "false"}
    onclick={onZones}
  >
    {text.storedZoneEdit}
  </button>
  <button
    type="button"
    data-active={activeTool === "calibration" ? "true" : "false"}
    onclick={onCalibration}
  >
    {text.storedCalibrationEdit}
  </button>
  <button
    type="button"
    data-active={activeTool === "furniture" ? "true" : "false"}
    onclick={onFurniture}
  >
    {text.storedFurniture}
  </button>
</div>
