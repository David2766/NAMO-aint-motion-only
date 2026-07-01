<script lang="ts">
  import { MAX_SOFTWARE_ZONES, MAX_ZONE_NAME_LENGTH } from "../../core/constants";
  import { zoneDisplayName, zoneSlotLabel } from "../../core/zones";
  import type { WebZone, WebZoneType } from "../types";

  type Props = {
    loaded: boolean;
    zones: WebZone[];
    selectedZone: WebZone | null;
    selectedZoneId: string;
    zoneTypeLabels: Record<WebZoneType, string>;
    onSelectZone: (zoneId: string) => void;
    onAddZone: () => void;
    onSetZoneName: (name: string) => void;
    onSetZoneType: (type: WebZoneType) => void;
    onDeleteSelected: () => void;
  };

  let {
    loaded,
    zones,
    selectedZone,
    selectedZoneId,
    zoneTypeLabels,
    onSelectZone,
    onAddZone,
    onSetZoneName,
    onSetZoneType,
    onDeleteSelected
  }: Props = $props();
</script>

<section>
  <h2>구역</h2>
  <div class="zone-list" data-zone-list>
    {#if !loaded}
      <p class="empty-zone-message">구역 데이터를 불러오는 중입니다.</p>
    {:else if zones.length === 0}
      <p class="empty-zone-message">아직 설정된 구역이 없습니다. 구역을 추가해 감지 또는 제외 구역을 만들어보세요.</p>
    {:else}
      {#each zones as zone (zone.id)}
        <button
          class={`zone-list-item ${zone.type}${zone.id === selectedZoneId ? " selected" : ""}`}
          type="button"
          onclick={() => onSelectZone(zone.id)}
        >
          <div>
            <strong>{zoneDisplayName(zone)}</strong>
            <span>{zoneSlotLabel(zone.id)}</span>
          </div>
          <em>{zoneTypeLabels[zone.type]}</em>
        </button>
      {/each}
    {/if}
    <div class="zone-add-area">
      <button class="zone-add-button" type="button" disabled={zones.length >= MAX_SOFTWARE_ZONES} onclick={onAddZone}>
        구역 추가
      </button>
      <p>
        {zones.length >= MAX_SOFTWARE_ZONES
          ? "최대 6개까지 설정했습니다."
          : "탐지/제외 구역은 최대 6개까지 만들 수 있습니다."}
      </p>
    </div>
  </div>

  <div data-zone-type-controls>
    {#if selectedZone}
      <div class={`zone-type-card ${selectedZone.type}`}>
        <div>
          <strong>{zoneDisplayName(selectedZone)}</strong>
          <span>원하는 구역을 지정하고 이름을 붙이거나 감지 제외를 하도록 설정할 수 있습니다.</span>
        </div>
        <label class="zone-name-field">
          <span>구역 이름</span>
          <input
            type="text"
            value={selectedZone.name || ""}
            maxlength={MAX_ZONE_NAME_LENGTH}
            placeholder="예: 침대, 책상, 커튼"
            oninput={(event) => onSetZoneName(event.currentTarget.value)}
          />
        </label>
        <div class="zone-type-buttons">
          {#each ["detection", "filter", "disabled"] as type}
            <button
              class={`zone-type-button ${type}${selectedZone.type === type ? " selected" : ""}`}
              type="button"
              onclick={() => onSetZoneType(type as WebZoneType)}
            >
              {zoneTypeLabels[type as WebZoneType]}
            </button>
          {/each}
        </div>
        <button class="danger-button" type="button" onclick={onDeleteSelected}>구역 삭제</button>
      </div>
    {:else}
      <p class="panel-help">구역을 추가하거나 선택하세요.</p>
    {/if}
  </div>
</section>
