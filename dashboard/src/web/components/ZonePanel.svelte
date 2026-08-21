<script lang="ts">
  import { MAX_SOFTWARE_ZONES, MAX_ZONE_NAME_LENGTH } from "../../core/constants";
  import { localizedZoneDisplayName } from "../../core/zones";
  import type { Messages } from "../i18n/types";
  import type { WebZone, WebZoneType } from "../types";

  type Props = {
    messages: Messages;
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
    messages,
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

  const text = $derived(messages.zones);

  function displayZoneName(zone: WebZone): string {
    return localizedZoneDisplayName(zone, text);
  }

  function zoneSlotName(zoneId: string): string {
    const match = /^zone_(\d+)$/.exec(zoneId);
    return match ? text.zoneLabel(match[1]) : zoneId;
  }

  function zoneNameInputValue(zone: WebZone): string {
    return localizedZoneDisplayName(zone, text);
  }

  function zoneTypeText(zone: WebZone): string {
    return zone.placeholder ? text.zoneTypePlaceholder : zoneTypeLabels[zone.type];
  }
</script>

<section>
  <h2>{text.zonePanelTitle}</h2>
  <div class="zone-list" data-zone-list>
    {#if !loaded}
      <p class="empty-zone-message">{text.zonesLoading}</p>
    {:else if zones.length === 0}
      <p class="empty-zone-message">{text.zonesEmpty}</p>
    {:else}
      {#each zones as zone (zone.id)}
        <button
          class={`zone-list-item ${zone.placeholder ? "placeholder" : zone.type}${zone.id === selectedZoneId ? " selected" : ""}`}
          type="button"
          onclick={() => onSelectZone(zone.id)}
        >
          <div>
            <strong>{displayZoneName(zone)}</strong>
            <span>{zoneSlotName(zone.id)}</span>
          </div>
          <em>{zoneTypeText(zone)}</em>
        </button>
      {/each}
    {/if}
    <div class="zone-add-area">
      <button class="zone-add-button" type="button" disabled={zones.length >= MAX_SOFTWARE_ZONES} onclick={onAddZone}>
        {text.addZone}
      </button>
      <p>
        {zones.length >= MAX_SOFTWARE_ZONES
          ? text.maxZonesReached
          : text.maxZonesHint}
      </p>
    </div>
  </div>

  <div data-zone-type-controls>
    {#if selectedZone}
      <div class={`zone-type-card ${selectedZone.placeholder ? "unselected" : selectedZone.type}`}>
        <div>
          <strong>{displayZoneName(selectedZone)}</strong>
          <span>{text.zoneDescription}</span>
        </div>
        <label class="zone-name-field">
          <span>{text.zoneName}</span>
          <input
            type="text"
            value={zoneNameInputValue(selectedZone)}
            maxlength={MAX_ZONE_NAME_LENGTH}
            placeholder={text.zoneNamePlaceholder}
            oninput={(event) => onSetZoneName(event.currentTarget.value)}
          />
        </label>
        <div class="zone-type-buttons">
          {#if selectedZone.placeholder}
            <span class="zone-type-placeholder">{text.zoneTypePlaceholder}</span>
          {/if}
          {#each ["detection", "filter", "reduced", "disabled", "exit"] as type}
            <button
              class={`zone-type-button ${type}${!selectedZone.placeholder && selectedZone.type === type ? " selected" : ""}`}
              type="button"
              onclick={() => onSetZoneType(type as WebZoneType)}
            >
              {zoneTypeLabels[type as WebZoneType]}
            </button>
          {/each}
        </div>
        <button class="danger-button" type="button" onclick={onDeleteSelected}>{text.deleteZone}</button>
      </div>
    {:else}
      <p class="panel-help">{text.selectOrAddZone}</p>
    {/if}
  </div>
</section>
