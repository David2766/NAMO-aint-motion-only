<script>
  let {
    imageUrl = "",
    imageWidth = 1,
    imageHeight = 1,
    viewBounds = null,
    frameStyle = "",
    frameClass = "",
    ariaLabel = "",
    showImage = true,
    backgroundFill = "",
    children
  } = $props();

  const viewBox = $derived(viewBounds
    ? `${viewBounds.x} ${viewBounds.y} ${viewBounds.width} ${viewBounds.height}`
    : `0 0 ${imageWidth} ${imageHeight}`);
</script>

<div class={`floorplan-image-frame ${frameClass}`} style={frameStyle}>
  <svg
    class="floorplan-base-svg"
    {viewBox}
    preserveAspectRatio="none"
    aria-label={ariaLabel}
    role="img"
  >
    {#if backgroundFill}
      <rect
        x="0"
        y="0"
        width={imageWidth}
        height={imageHeight}
        fill={backgroundFill}
      />
    {/if}
    {#if showImage}
      <image
        href={imageUrl}
        x="0"
        y="0"
        width={imageWidth}
        height={imageHeight}
        preserveAspectRatio="none"
      />
    {/if}
  </svg>
  {@render children?.()}
</div>
