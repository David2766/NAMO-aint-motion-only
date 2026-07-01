<script>
  let {
    open = false,
    busy = false,
    progress = 0,
    statusText = "",
    logs = [],
    resultSummary = "",
    errorText = "",
    onCancel,
    onClose
  } = $props();

  function percent() {
    return Math.round(Math.max(0, Math.min(1, progress)) * 100);
  }
</script>

{#if open}
  <div class="floorplan-ocr-modal-backdrop" role="presentation">
    <div class="floorplan-ocr-modal" role="dialog" aria-modal="true" aria-labelledby="floorplan-ocr-title">
      <header>
        <div>
          <strong id="floorplan-ocr-title">{busy ? "OCR 분석 중" : errorText ? "OCR 분석 확인" : "OCR 분석 완료"}</strong>
          <span>{statusText}</span>
        </div>
      </header>

      <div class="floorplan-ocr-progress" aria-label="OCR 진행률">
        <span style={`width: ${percent()}%`}></span>
      </div>
      <div class="floorplan-ocr-progress-label">{percent()}%</div>

      {#if resultSummary}
        <div class="floorplan-ocr-result-summary">{resultSummary}</div>
      {/if}

      {#if errorText}
        <div class="floorplan-ocr-error">{errorText}</div>
      {/if}

      {#if logs.length}
        <ul class="floorplan-ocr-log-list">
          {#each logs.slice(-6) as log}
            <li>{log}</li>
          {/each}
        </ul>
      {/if}

      <footer>
        {#if busy}
          <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={onCancel}>
            <span>작업 취소</span>
            <strong>OCR 중단</strong>
          </button>
        {:else}
          <button type="button" class="floorplan-next-button floorplan-fixed-text" onclick={onClose}>
            <span>닫기</span>
            <strong>결과 확인</strong>
          </button>
        {/if}
      </footer>
    </div>
  </div>
{/if}
