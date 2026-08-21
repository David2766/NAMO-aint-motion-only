<script>
  let {
    text,
    mode,
    floorplanName,
    roomCount = 0,
    dirty = false,
    saveBusy = false,
    canSave = false,
    onView,
    onEdit,
    onReplace,
    onDelete,
    onSave
  } = $props();
</script>

<div class="floorplan-workflow-card">
  <div>
    <strong>{text.storedTitle}</strong>
    <span>{text.storedDescription}</span>
  </div>
  <dl class="floorplan-stored-summary">
    <div>
      <dt>{text.storedState}</dt>
      <dd>{text.storedDataReady}</dd>
    </div>
    <div>
      <dt>{text.storedMode}</dt>
      <dd>{mode === "edit" ? text.storedModeEdit : text.storedModeView}</dd>
    </div>
    <div>
      <dt>{text.storedFloorplan}</dt>
      <dd>{floorplanName || text.storedReady}</dd>
    </div>
    <div>
      <dt>{text.storedRooms}</dt>
      <dd>{text.storedRoomCount(roomCount)}</dd>
    </div>
  </dl>
</div>

<div class="floorplan-stored-tools">
  <button
    type="button"
    data-active={mode === "view" ? "true" : "false"}
    onclick={onView}
  >
    {text.storedView}
  </button>
  <button
    type="button"
    data-active={mode === "edit" ? "true" : "false"}
    onclick={onEdit}
  >
    {text.storedEdit}
  </button>
  <button type="button" onclick={onReplace}>{text.storedReplace}</button>
  <button type="button" class="danger-button" onclick={onDelete}>{text.storedDelete}</button>
  <button type="button" onclick={onSave} disabled={!canSave}>
    {saveBusy ? text.storedSaving : dirty ? text.storedSaveChanges : text.storedSave}
  </button>
</div>
