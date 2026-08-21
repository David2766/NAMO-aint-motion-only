<script>
  let {
    text,
    occlusionEditActive = false,
    ignoredOcclusionEdges = [],
    onToggleOcclusionEdit,
    onResetOcclusion
  } = $props();

  let expanded = $state(false);

  $effect(() => {
    if (occlusionEditActive) expanded = true;
  });
</script>

<div class="floorplan-radar-advanced-options">
  <button
    type="button"
    class="floorplan-radar-advanced-toggle"
    aria-expanded={expanded}
    onclick={() => (expanded = !expanded)}
  >
    <span>{text.advancedOptions}</span>
    <strong>{ignoredOcclusionEdges.length ? text.occlusionIgnored(ignoredOcclusionEdges.length) : ""}</strong>
    <span aria-hidden="true">{expanded ? "−" : "+"}</span>
  </button>

  {#if expanded}
    <div class="floorplan-radar-advanced-content">
      <strong>{text.occlusionTitle}</strong>
      <span>{text.occlusionDescription}</span>
      <div class="floorplan-radar-advanced-actions">
        <button
          type="button"
          class="floorplan-radar-occlusion-toggle"
          data-active={occlusionEditActive ? "true" : "false"}
          onclick={onToggleOcclusionEdit}
        >
          {occlusionEditActive ? text.occlusionEditOff : text.occlusionEditOn}
        </button>
        <button
          type="button"
          onclick={onResetOcclusion}
          disabled={!ignoredOcclusionEdges.length}
        >
          {text.occlusionReset}
        </button>
      </div>
      <em>
        {occlusionEditActive
          ? text.occlusionActiveHint
          : text.occlusionIgnored(ignoredOcclusionEdges.length)}
      </em>
    </div>
  {/if}
</div>
