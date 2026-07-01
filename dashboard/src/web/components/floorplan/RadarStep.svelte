<script>
  let {
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
    onBack,
    onNext
  } = $props();
</script>

<div class="floorplan-radar-placement-card">
  <strong>레이더 배치</strong>
  <span>입력한 전체 평면도 크기와 하늘색 최외곽 박스를 기준으로 레이더 배치를 준비합니다.</span>
  {#if scaleSummary}
    <dl>
      <div><dt>전체 크기</dt><dd>{scaleSummary.widthMm} x {scaleSummary.heightMm}mm</dd></div>
      <div><dt>기준 박스</dt><dd>{scaleSummary.widthPx} x {scaleSummary.heightPx}px</dd></div>
      <div><dt>스케일</dt><dd>{scaleSummary.mmPerPxX} / {scaleSummary.mmPerPxY} mm/px</dd></div>
    </dl>
  {/if}
  <div class="floorplan-radar-scale-control">
    <div>
      <span>스케일 미세 보정</span>
      <strong>{radarScalePercent}%</strong>
    </div>
    <div>
      <button
        type="button"
        aria-label="레이더맵 축소"
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
        aria-label="레이더맵 확대"
        onclick={() => onScaleNudge?.(scaleStepPercent)}
        disabled={radarScalePercent >= maxScalePercent}
      >
        +
      </button>
    </div>
    <em>자동 스케일 기준에서 95~105%까지만 조정됩니다.</em>
  </div>
  <em>다음 작업: 평면도 위에서 레이더 위치와 방향을 지정합니다.</em>
  <button
    type="button"
    class="floorplan-radar-occlusion-toggle"
    data-active={occlusionEditActive ? "true" : "false"}
    onclick={onToggleOcclusionEdit}
  >
    {occlusionEditActive ? "차폐 변 편집 끄기" : "차폐 변 편집"}
  </button>
  <em>
    {occlusionEditActive
      ? "평면도 위 방 경계선을 클릭하면 해당 변의 레이더 차폐를 무시합니다."
      : `무시한 차폐 변 ${ignoredOcclusionEdges.length}개`}
  </em>
</div>

<div class="floorplan-step-actions">
  <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={onBack}>
    <span>이전 단계</span>
    <strong>방 이름/스케일</strong>
  </button>
  <button type="button" class="floorplan-next-button floorplan-fixed-text" onclick={onNext}>
    <span>다음 단계</span>
    <strong>최종 확인</strong>
  </button>
</div>
