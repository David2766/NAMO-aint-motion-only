<script lang="ts">
  import type { CalibrationMetrics } from "../../core/calibration";
  import type { Messages } from "../i18n/types";
  import type { CalibrationResult } from "../types";

  type Props = {
    messages: Messages;
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
    messages,
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

  const text = $derived(messages.zones.calibrationDialog);
</script>

<div data-calibration-dialog>
  {#if open}
    <div class="calibration-dialog-backdrop" role="dialog" aria-modal="true" aria-label={text.aria}>
      <div class="calibration-dialog">
        <div class="calibration-dialog-header">
          <div>
            <strong>{text.title}</strong>
            <span>{running ? text.runningDescription : text.doneDescription}</span>
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
              <p>{text.createdZones(result.createdCount)}</p>
              {#if result.adjustedStaticRadarGates}
                <p>{text.adjustedStaticRadarGates(result.adjustedStaticRadarGates)}</p>
              {/if}
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
            <strong>{text.workItems}</strong>
            <ul>
              {#each workItems as item}
                <li>{item}</li>
              {/each}
            </ul>
          </div>
          <div class="calibration-log">
            <strong>{result?.tone === "error" ? text.errorLog : text.debugLog}</strong>
            <pre>{logs.length ? logs.join("\n") : text.noLogs}</pre>
          </div>
          <div class="calibration-dialog-actions">
            {#if running}
              <button class="calibration-button" type="button" onclick={onStop}>
                {text.stop}
              </button>
            {:else}
              <button type="button" onclick={onClose}>{text.close}</button>
            {/if}
          </div>
        </div>
      </div>
    </div>
  {/if}
</div>
