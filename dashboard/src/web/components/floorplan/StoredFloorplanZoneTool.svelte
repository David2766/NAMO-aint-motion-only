<script>
  import { localizedZoneDisplayName } from "../../../core/zones";

  let {
    messages,
    text,
    source = "zones",
    zones = [],
    selection = {},
    actions = {}
  } = $props();

  const isCalibration = $derived(source === "calibration");
  const isExitPoint = $derived(source === "exit");
  const editorKind = $derived(isCalibration ? "calibration" : isExitPoint ? "exit" : "zones");
  const selectedZone = $derived(selection.zone ?? null);
  const selectedZoneId = $derived(selection.zoneId ?? "");
  const selectedPointIndex = $derived(selection.pointIndex ?? -1);
  const zoneTypes = ["detection", "filter", "disabled", "exit"];
  const calibrationTypes = ["filter", "reduced", "disabled"];
  const canDeleteSelectedPoint = $derived(
    Boolean(!isExitPoint && selectedZone && selectedPointIndex >= 0 && selectedZone.shape === "polygon" && selectedZone.points.length > 3)
  );

  function zoneShapeText(zone) {
    if (zone.type === "exit") return text.storedExitLineShape;
    return zone.shape === "polygon" ? text.storedZonePointCount(zone.points.length) : text.storedRectShape;
  }

  function typeButtonClass(type) {
    return `zone-type-button ${type}${selectedZone?.type === type ? " selected" : ""}`;
  }

  function zoneTypeLabel(type) {
    if (type === "detection") return text.storedDetection;
    if (type === "filter") return text.storedFilter;
    if (type === "disabled") return text.storedDisabled;
    return text.storedExit;
  }

  function calibrationTypeLabel(type) {
    if (type === "filter") return text.storedBlock;
    if (type === "reduced") return text.storedReduced;
    return text.storedOff;
  }
</script>

<div class="floorplan-tool-card floorplan-zone-tool-card">
  <strong>{text.storedZoneEditorTitle(editorKind)}</strong>
  <span>{isExitPoint ? text.storedExitPointEditorDescription : text.storedZoneEditorDescription}</span>
  {#if !isCalibration}
    <button type="button" onclick={() => actions.addZone?.()}>{isExitPoint ? text.storedAddExitPoint : text.storedAddZone}</button>
  {/if}
  <div class="floorplan-tool-list floorplan-zone-tool-list">
    {#if !zones.length}
      <em>{text.storedNoZones(editorKind)}</em>
    {:else}
      {#each zones as zone (zone.id)}
        <button
          type="button"
          class="floorplan-tool-list-item"
          data-active={selectedZoneId === zone.id ? "true" : "false"}
          onclick={() => actions.selectZone?.(zone.id, -1)}
        >
          <span>{localizedZoneDisplayName(zone, messages.zones)}</span>
          <small>{zoneShapeText(zone)} &middot; {zone.type}</small>
        </button>
      {/each}
    {/if}
  </div>
  {#if selectedZone}
    {#if !isCalibration}
      <label class="zone-name-field">
        <span>{text.storedZoneName}</span>
        <input
          type="text"
          value={selectedZone.name || ""}
          placeholder={text.storedZoneNamePlaceholder}
          oninput={(event) => actions.renameZone?.(event.currentTarget.value)}
        />
      </label>
      {#if !isExitPoint}
        <div class="zone-type-buttons">
          {#each zoneTypes as type}
            <button class={typeButtonClass(type)} type="button" onclick={() => actions.setZoneType?.(type)}>
              {zoneTypeLabel(type)}
            </button>
          {/each}
        </div>
        <button type="button" disabled={selectedZone.shape === "rect"} onclick={() => actions.convertToRect?.()}>
          {text.storedConvertRect}
        </button>
        <button
          type="button"
          disabled={!canDeleteSelectedPoint}
          onclick={() => actions.deletePoint?.()}
        >
          {text.storedDeletePoint}
        </button>
      {/if}
    {:else}
      <div class="zone-type-buttons">
        {#each calibrationTypes as type}
          <button class={typeButtonClass(type)} type="button" onclick={() => actions.setCalibrationZoneType?.(type)}>
            {calibrationTypeLabel(type)}
          </button>
        {/each}
      </div>
    {/if}
    <button type="button" class="danger-button" onclick={() => actions.deleteZone?.()}>
      {text.storedDeleteZone}
    </button>
  {/if}
</div>
