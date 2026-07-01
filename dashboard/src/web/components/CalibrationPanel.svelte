<script lang="ts">
  import { MAX_CALIBRATION_ZONES } from "../../core/constants";
  import { calibrationType, zoneDisplayName } from "../../core/zones";
  import type { WebZone, WebZoneType } from "../types";

  type CalibrationActionType = Extract<WebZoneType, "filter" | "reduced" | "disabled">;

  type Props = {
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
</script>

<section>
  <h2>오탐 보정</h2>
  <div data-calibration-panel>
    <div class="calibration-card">
      <div>
        <strong>오탐 보정</strong>
        <span>반복적으로 감지되는 오탐 위치를 분석해 보정 구역으로 저장합니다.</span>
      </div>
      <button
        class="calibration-button"
        type="button"
        disabled={!running && (!loaded || !hasState || pirMotion || zones.length >= MAX_CALIBRATION_ZONES)}
        onclick={() => (running ? onStop() : onStart())}
      >
        {running ? "보정 중지" : "오탐 보정 시작"}
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
                {zoneDisplayName(zone)}
                <em>{calibrationTypeLabels[calibrationType(zone.type)]}</em>
              </span>
              <div class="calibration-list-actions">
                <select
                  aria-label="오탐 보정 동작"
                  value={calibrationType(zone.type)}
                  onchange={(event) =>
                    onSetZoneType(
                      zone.id,
                      event.currentTarget.value as CalibrationActionType
                    )}
                >
                  <option value="filter">필터</option>
                  <option value="reduced">둔감</option>
                  <option value="disabled">제외</option>
                </select>
                <button
                  type="button"
                  onclick={(event) => {
                    event.stopPropagation();
                    onSelectZone(zone.id);
                    onDeleteZone(zone.id);
                  }}
                >
                  삭제
                </button>
              </div>
            </div>
          {/each}
        </div>
      {/if}
    </div>
  </div>
</section>
