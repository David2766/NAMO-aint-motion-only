<script lang="ts">
  import type { RadarViewport } from "../../core/types";
  import { staticRadarGateLabelPoint, staticRadarGatePath } from "../static-radar-tuning";
  import type { WebStaticRadarGateTuning } from "../types";

  type Props = {
    gates: WebStaticRadarGateTuning[];
    detectedGate: number | null;
    selectedGate: number;
    viewport: RadarViewport;
    gateAriaLabel: (gate: WebStaticRadarGateTuning) => string;
    onSelectGate: (gate: number) => void;
  };

  let { gates, detectedGate, selectedGate, viewport, gateAriaLabel, onSelectGate }: Props = $props();

  function energyLabel(gate: WebStaticRadarGateTuning): string {
    return `G${gate.gate}  M ${Math.round(gate.moveEnergy)}  S ${Math.round(gate.stillEnergy)}`;
  }

  function stopKey(event: KeyboardEvent, gate: number): void {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    event.stopPropagation();
    onSelectGate(gate);
  }
</script>

<g class="static-radar-tuning-layer">
  {#each gates as gate (gate.gate)}
    {@const path = staticRadarGatePath(gate, viewport)}
    {@const labelPoint = staticRadarGateLabelPoint(gate, viewport)}
    {#if path}
      <path
        d={path}
        class:detected={gate.gate === detectedGate}
        class:selected={gate.gate === selectedGate}
        style={`--gate-sensitivity:${Math.max(0, Math.min(100, gate.sensitivity)) / 100}`}
        role="button"
        tabindex="0"
        aria-label={gateAriaLabel(gate)}
        onclick={() => onSelectGate(gate.gate)}
        onkeydown={(event) => stopKey(event, gate.gate)}
      />
      <text x={labelPoint.x} y={labelPoint.y + 4}>{energyLabel(gate)}</text>
    {/if}
  {/each}
</g>
