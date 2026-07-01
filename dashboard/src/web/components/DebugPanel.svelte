<script lang="ts">
  import type { WebTarget } from "../types";

  type Props = {
    targets: WebTarget[];
  };

  let { targets }: Props = $props();

  function targetDebugStatus(target: WebTarget): string {
    if (target.filtered) return `filtered${target.filterReason ? `: ${target.filterReason}` : ""}`;
    if (target.reduced) return "reduced";
    if (target.rawActive && !target.active) return "raw active / display inactive";
    return target.active ? "active" : "inactive";
  }

  function targetDebugPoint(target: WebTarget): string {
    const raw =
      Number.isFinite(target.rawX) && Number.isFinite(target.rawY)
        ? `raw=(${Math.round(Number(target.rawX))}, ${Math.round(Number(target.rawY))})`
        : "raw=not provided";
    return `display=(${Math.round(target.x)}, ${Math.round(target.y)}) / ${raw}`;
  }
</script>

<section class="debug-panel">
  <div class="debug-panel-header">
    <strong>Radar Debug</strong>
    <span>현재 펌웨어가 제공하는 표시 좌표와 raw/filter 정보를 함께 보여줍니다.</span>
  </div>
  <div class="debug-grid">
    {#each targets as target}
      <article class:inactive={!target.active} class:filtered={Boolean(target.filtered || target.rawActive)}>
        <strong>{target.name}</strong>
        <span>{targetDebugStatus(target)}</span>
        <code>{targetDebugPoint(target)}</code>
        <small>
          active={String(target.active)}
          rawActive={String(Boolean(target.rawActive))}
          filtered={String(Boolean(target.filtered))}
          reason={target.filterReason || "none"}
        </small>
      </article>
    {/each}
  </div>
</section>
