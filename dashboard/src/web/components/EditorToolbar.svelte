<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    label?: string;
    ariaLabel?: string;
    children?: Snippet;
  };

  let { label = "", ariaLabel = "편집 도구", children }: Props = $props();
</script>

<div class="floorplan-map-toolbar" aria-label={ariaLabel}>
  {@render children?.()}
  {#if label}
    <span class="editor-toolbar-label">{label}</span>
  {/if}
</div>
