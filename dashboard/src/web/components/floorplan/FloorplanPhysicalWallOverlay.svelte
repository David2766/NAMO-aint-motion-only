<script>
  let {
    segments = [],
    imageWidth = 1,
    imageHeight = 1,
    viewBounds = null
  } = $props();

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
</script>

{#if segments.length}
  <svg
    class="floorplan-physical-wall-layer"
    viewBox={`${viewX()} ${viewY()} ${viewWidth()} ${viewHeight()}`}
    preserveAspectRatio="none"
    aria-hidden="true"
  >
    {#each segments as segment (segment.id)}
      <line
        x1={segment.x1}
        y1={segment.y1}
        x2={segment.x2}
        y2={segment.y2}
      />
    {/each}
  </svg>
{/if}
