<script>
  import FloorplanRadarAdvancedOptions from "./FloorplanRadarAdvancedOptions.svelte";

  let {
    messages,
    scaleSummary = null,
    radarScalePercent = 100,
    minScalePercent = 95,
    maxScalePercent = 105,
    scaleStepPercent = 1,
    occlusionEditActive = false,
    ignoredOcclusionEdges = [],
    onScaleInput,
    onScaleNudge,
    onToggleOcclusionEdit,
    onResetOcclusion,
    onBack,
    onNext
  } = $props();

  const text = $derived(messages.floorplan.radarStep);
</script>

<div class="floorplan-radar-placement-card">
  <strong>{text.title}</strong>
  <span>{text.description}</span>
  {#if scaleSummary}
    <dl>
      <div><dt>{text.totalSize}</dt><dd>{scaleSummary.widthMm} x {scaleSummary.heightMm}mm</dd></div>
      <div><dt>{text.referenceBox}</dt><dd>{scaleSummary.widthPx} x {scaleSummary.heightPx}px</dd></div>
      <div><dt>{text.scale}</dt><dd>{scaleSummary.mmPerPxX} / {scaleSummary.mmPerPxY} mm/px</dd></div>
    </dl>
  {/if}
  <div class="floorplan-radar-scale-control">
    <div>
      <span>{text.scaleFineTune}</span>
      <strong>{radarScalePercent}%</strong>
    </div>
    <div>
      <button
        type="button"
        aria-label={text.zoomOutAria}
        onclick={() => onScaleNudge?.(-scaleStepPercent)}
        disabled={radarScalePercent <= minScalePercent}
      >
        -
      </button>
      <input
        type="range"
        min={minScalePercent}
        max={maxScalePercent}
        step={scaleStepPercent}
        value={radarScalePercent}
        oninput={(event) => onScaleInput?.(event.currentTarget.value)}
      />
      <button
        type="button"
        aria-label={text.zoomInAria}
        onclick={() => onScaleNudge?.(scaleStepPercent)}
        disabled={radarScalePercent >= maxScalePercent}
      >
        +
      </button>
    </div>
    <em>{text.scaleHint}</em>
  </div>
  <em>{text.nextWork}</em>
  <FloorplanRadarAdvancedOptions
    {text}
    {occlusionEditActive}
    {ignoredOcclusionEdges}
    {onToggleOcclusionEdit}
    {onResetOcclusion}
  />
</div>

<div class="floorplan-step-actions">
  <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={onBack}>
    <span>{text.previousStep}</span>
    <strong>{text.roomsScale}</strong>
  </button>
  <button type="button" class="floorplan-next-button floorplan-fixed-text" onclick={onNext}>
    <span>{text.nextStep}</span>
    <strong>{text.finalReview}</strong>
  </button>
</div>
