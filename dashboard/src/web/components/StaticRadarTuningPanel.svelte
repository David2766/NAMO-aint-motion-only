<script lang="ts">
  import type { Messages } from "../i18n/types";
  import type { WebStaticRadarGateTuning, WebStaticRadarTuningStatus } from "../types";

  type Props = {
    messages: Messages;
    status: WebStaticRadarTuningStatus | null;
    detectedGate: number | null;
    selectedGate: number;
    sensitivity: number;
    sensitivityDirty: boolean;
    busy: boolean;
    error: string;
    onSelectGate: (gate: number) => void;
    onSetSensitivity: (value: number) => void;
    onApply: () => void;
  };

  let {
    messages,
    status,
    detectedGate,
    selectedGate,
    sensitivity,
    sensitivityDirty,
    busy,
    error,
    onSelectGate,
    onSetSensitivity,
    onApply
  }: Props = $props();

  const text = $derived(messages.zones);
  const selected = $derived(status?.gates.find((gate) => gate.gate === selectedGate) ?? null);

  function distanceMeters(value: number): string {
    return (value / 1000).toFixed(2).replace(/\.00$/, "").replace(/(\.\d)0$/, "$1");
  }

  function gateRange(gate: WebStaticRadarGateTuning): string {
    return text.staticRadarGateRange(distanceMeters(gate.startMm), distanceMeters(gate.endMm));
  }
</script>

<section class="static-radar-tuning-panel">
  <h2>{text.staticRadarTuningTitle}</h2>
  <p class="panel-help">{text.staticRadarTuningDescription}</p>

  {#if error}
    <p class="static-radar-tuning-error" role="alert">{error}</p>
  {/if}

  {#if !status?.active || status.gates.length === 0}
    <p class="static-radar-tuning-loading">{text.staticRadarTuningStarting}</p>
  {:else}
    <div class="static-radar-gate-list" aria-label={text.staticRadarGateListAria}>
      {#each status.gates as gate (gate.gate)}
        <button
          type="button"
          class:detected={gate.gate === detectedGate}
          data-active={gate.gate === selectedGate ? "true" : "false"}
          onclick={() => onSelectGate(gate.gate)}
        >
          <strong>G{gate.gate}</strong>
          <span>{gateRange(gate)}</span>
          <small>M {Math.round(gate.moveEnergy)} · S {Math.round(gate.stillEnergy)}</small>
        </button>
      {/each}
    </div>

    {#if selected}
      <dl class="static-radar-gate-detail">
        <div>
          <dt>{text.staticRadarDistance}</dt>
          <dd>{gateRange(selected)}</dd>
        </div>
        <div>
          <dt>{text.staticRadarMovingEnergy}</dt>
          <dd>{Math.round(selected.moveEnergy)}</dd>
        </div>
        <div>
          <dt>{text.staticRadarStillEnergy}</dt>
          <dd>{Math.round(selected.stillEnergy)}</dd>
        </div>
        <div>
          <dt>{text.staticRadarDetection}</dt>
          <dd class:detected={selected.gate === detectedGate}>
            {selected.gate === detectedGate ? text.staticRadarDetected : text.staticRadarNotDetected}
          </dd>
        </div>
      </dl>

      <label class="static-radar-sensitivity-control">
        <span>
          <strong>{text.staticRadarSensitivity}</strong>
          <output>{Math.round(sensitivity)}%</output>
        </span>
        <input
          type="range"
          min="0"
          max="100"
          step="1"
          value={sensitivity}
          oninput={(event) => onSetSensitivity(Number(event.currentTarget.value))}
        />
      </label>
      <button type="button" disabled={busy || !sensitivityDirty} onclick={onApply}>
        {busy ? text.staticRadarApplying : text.staticRadarApply}
      </button>
    {/if}
  {/if}
</section>
