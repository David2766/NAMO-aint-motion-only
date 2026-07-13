<script lang="ts">
  import { MAX_CALIBRATION_ZONES } from "../../core/constants";
  import { calibrationType, localizedZoneDisplayName } from "../../core/zones";
  import type { Messages } from "../i18n/types";
  import type { WebZone, WebZoneType } from "../types";

  type CalibrationActionType = Extract<WebZoneType, "filter" | "reduced" | "disabled">;

  type Props = {
    messages: Messages;
    loaded: boolean;
    hasState: boolean;
    pirMotion: boolean;
    running: boolean;
    zones: WebZone[];
    selectedZoneId: string;
    statusText: string;
    calibrationTypeLabels: Record<CalibrationActionType, string>;
    onStart: () => void;
    onStop: () => void;
    onSelectZone: (zoneId: string) => void;
    onSetZoneType: (zoneId: string, type: CalibrationActionType) => void;
    onDeleteZone: (zoneId: string) => void;
  };

  let {
    messages,
    loaded,
    hasState,
    pirMotion,
    running,
    zones,
    selectedZoneId,
    statusText,
    calibrationTypeLabels,
    onStart,
    onStop,
    onSelectZone,
    onSetZoneType,
    onDeleteZone
  }: Props = $props();

  const text = $derived(messages.zones);

  function displayZoneName(zone: WebZone): string {
    return localizedZoneDisplayName(zone, text);
  }
</script>

<section>
  <h2>{text.calibrationPanelTitle}</h2>
  <div data-calibration-panel>
    <div class="calibration-card">
      <div>
        <strong>{text.calibrationPanelTitle}</strong>
        <span>{text.calibrationDescription}</span>
      </div>
      <button
        class="calibration-button"
        type="button"
        disabled={!running && (!loaded || !hasState || pirMotion || zones.length >= MAX_CALIBRATION_ZONES)}
        onclick={() => (running ? onStop() : onStart())}
      >
        {running ? text.calibrationStop : text.calibrationStart}
      </button>
      <p>{statusText}</p>
      {#if zones.length}
        <div class="calibration-list">
          {#each zones as zone (zone.id)}
            <div
              class={`calibration-list-item ${calibrationType(zone.type)}${zone.id === selectedZoneId ? " selected" : ""}`}
              role="button"
              tabindex="0"
              onclick={() => onSelectZone(zone.id)}
              onkeydown={(event) => {
                if (event.key === "Enter") onSelectZone(zone.id);
              }}
            >
              <span>
                {displayZoneName(zone)}
                <em>{calibrationTypeLabels[calibrationType(zone.type)]}</em>
              </span>
              <div class="calibration-list-actions">
                <select
                  aria-label={text.calibrationActionAria}
                  value={calibrationType(zone.type)}
                  onchange={(event) =>
                    onSetZoneType(
                      zone.id,
                      event.currentTarget.value as CalibrationActionType
                    )}
                >
                  <option value="filter">{text.typeLabels.filter}</option>
                  <option value="reduced">{text.typeLabels.reduced}</option>
                  <option value="disabled">{text.typeLabels.disabled}</option>
                </select>
                <button
                  type="button"
                  onclick={(event) => {
                    event.stopPropagation();
                    onSelectZone(zone.id);
                    onDeleteZone(zone.id);
                  }}
                >
                  {text.delete}
                </button>
              </div>
            </div>
          {/each}
        </div>
      {/if}
    </div>
  </div>
</section>
