<script lang="ts">
  import type { CalibrationMetrics } from "../../core/calibration";
  import type { CalibrationResult } from "../types";

  type Props = {
    open: boolean;
    running: boolean;
    result: CalibrationResult | null;
    metrics: CalibrationMetrics | undefined;
    progress: number;
    progressText: string;
    workItems: string[];
    logs: string[];
    metricsLines: string[];
    onClose: () => void;
    onStop: () => void;
  };

  let {
    open,
    running,
    result,
    metrics,
    progress,
    progressText,
    workItems,
    logs,
    metricsLines,
    onClose,
    onStop
  }: Props = $props();
</script>

<div data-calibration-dialog>
  {#if open}
    <div class="calibration-dialog-backdrop" role="dialog" aria-modal="true" aria-label="오탐 보정">
      <div class="calibration-dialog">
        <div class="calibration-dialog-header">
          <div>
            <strong>오탐 보정</strong>
            <span>{running ? "보정 데이터를 수집하고 있습니다." : "보정 작업이 종료되었습니다."}</span>
          </div>
          <button class="calibration-dialog-close" type="button" onclick={() => !running && onClose()}>
            ×
          </button>
        </div>
        <div class="calibration-dialog-body">
          {#if result}
            <div class={`calibration-result ${result.tone}`}>
              <strong>{result.title}</strong>
              <p>{result.reason}</p>
              <p>생성된 보정 구역: {result.createdCount}개</p>
              {#if metrics && metrics.samples > 0}
                <pre>{metricsLines.join("\n")}</pre>
              {/if}
            </div>
          {/if}
          <div class="calibration-progress">
            <div class="calibration-progress-header">
              <span>{progressText}</span>
              <strong>{progress}%</strong>
            </div>
            <div class="calibration-progress-track">
              <div class="calibration-progress-fill" style={`width:${progress}%`}></div>
            </div>
          </div>
          <div class="calibration-work">
            <strong>작업 내역</strong>
            <ul>
              {#each workItems as item}
                <li>{item}</li>
              {/each}
            </ul>
          </div>
          <div class="calibration-log">
            <strong>{result?.tone === "error" ? "오류 로그" : "디버그 로그"}</strong>
            <pre>{logs.length ? logs.join("\n") : "아직 기록된 로그가 없습니다."}</pre>
          </div>
          <div class="calibration-dialog-actions">
            {#if running}
              <button class="calibration-button" type="button" onclick={onStop}>
                보정 중지
              </button>
            {:else}
              <button type="button" onclick={onClose}>닫기</button>
            {/if}
          </div>
        </div>
      </div>
    </div>
  {/if}
</div>
