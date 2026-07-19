<script lang="ts">
  import { onDestroy } from "svelte";
  import { apiErrorMessage } from "../api/api-message";
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import { presenceDisplayTargets } from "../../core/presence-target-display";
  import {
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus
  } from "../floorplan/floorplan-storage-client";
  import { buildBoundaryOcclusionSegments } from "../floorplan/floorplan-occlusion";
  import { floorplanImageFrameStyle } from "../floorplan/floorplan-image-frame";
  import { findRoomContainingPoint } from "../floorplan/floorplan-room-context";
  import FloorplanImageFrame from "./floorplan/FloorplanImageFrame.svelte";
  import FloorplanRadarPlacementOverlay from "./FloorplanRadarPlacementOverlay.svelte";
  import DiagnosticDialog from "./DiagnosticDialog.svelte";
  import type { Messages } from "../i18n";
  import RadarScene from "./RadarScene.svelte";
  import type { WebControlStatus, WebDeviceConfig, WebDeviceState, WebSystemStatus } from "../types";
  import {
    detectBrowserTimezone,
    normalizeSupportedTimezone,
    TIMEZONE_IDS,
    type TimezoneId
  } from "../timezone-options";
  import { formatCompactBytes } from "../utils/formatters";

  type TabTarget = "zones" | "floorplan" | "stats" | "backup";
  type IntegrationMode = "unknown" | "edge" | "ha";

  type Props = {
    config: WebDeviceConfig | null;
    state: WebDeviceState | null;
    systemStatus: WebSystemStatus | null;
    systemStatusLoading: boolean;
    systemStatusError: string;
    controlStatus: WebControlStatus | null;
    controlStatusLoading: boolean;
    controlStatusError: string;
    controlActionBusy: boolean;
    integrationMode: IntegrationMode;
    integrationModeActionBusy: boolean;
    messages: Messages;
    updatedText: string;
    floorplanStorageBaseUrl: string;
    floorplanStorageFetcher?: typeof fetch;
    onNavigate: (tab: TabTarget) => void;
    onSetStatusLed: (enabled: boolean) => void | Promise<void>;
    onSetLedBlinkDuration: (seconds: number) => void | Promise<void>;
    onSetEnvironmentCorrection: (enabled: boolean) => void | Promise<void>;
    onSetLegacyPresenceFallback: (enabled: boolean) => void | Promise<void>;
    onSetTemperatureOffset: (value: number) => void | Promise<void>;
    onSetHumidityOffset: (value: number) => void | Promise<void>;
    onSetTimezone: (timezone: string) => boolean | Promise<boolean>;
    onChangeIntegrationMode: () => void | Promise<void>;
  };

  let {
    config,
    state: deviceState,
    systemStatus,
    systemStatusLoading,
    systemStatusError,
    controlStatus,
    controlStatusLoading,
    controlStatusError,
    controlActionBusy,
    integrationMode,
    integrationModeActionBusy,
    messages,
    updatedText,
    floorplanStorageBaseUrl,
    floorplanStorageFetcher,
    onNavigate,
    onSetStatusLed,
    onSetLedBlinkDuration,
    onSetEnvironmentCorrection,
    onSetLegacyPresenceFallback,
    onSetTemperatureOffset,
    onSetHumidityOffset,
    onSetTimezone,
    onChangeIntegrationMode
  }: Props = $props();

  let tuningOpen = $state(false);
  let ledDialogOpen = $state(false);
  let environmentDialogOpen = $state(false);
  let timezoneDialogOpen = $state(false);
  let selectedTimezone = $state<TimezoneId>("Asia/Seoul");
  let timezoneDetectionText = $state("");
  let diagnosticDialogOpen = $state(false);
  let floorplanLoading = $state(false);
  let floorplanError = $state("");
  let floorplanDocument = $state<FloorplanStorageDocument | null>(null);
  let floorplanImageUrl = $state("");

  const targetCount = $derived(deviceState?.targetCount ?? deviceState?.targets.filter((target) => target.active).length ?? 0);
  const movingTargetCount = $derived(deviceState?.movingTargetCount ?? 0);
  const stillTargetCount = $derived(deviceState?.stillTargetCount ?? 0);
  const displayTargets = $derived(presenceDisplayTargets(deviceState));
  const hasFloorplan = $derived(Boolean(config?.floorplan?.enabled && config.floorplan.hasImage));
  const zoneCount = $derived(config?.zones.length ?? 0);
  const filterZoneCount = $derived(config?.zones.filter((zone) => zone.type === "filter").length ?? 0);
  const reducedZoneCount = $derived(config?.zones.filter((zone) => zone.type === "reduced").length ?? 0);
  const disabledZoneCount = $derived(config?.zones.filter((zone) => zone.type === "disabled").length ?? 0);
  const legacyPresenceFallback = $derived(config?.legacyPresenceFallback === true);
  const integrationModeButtonText = $derived(
    integrationMode === "edge"
      ? messages.dashboard.haMode
      : integrationMode === "ha"
        ? messages.dashboard.edgeMode
        : messages.dashboard.chooseMode
  );

  function yesNo(value: boolean | undefined): string {
    return value ? messages.dashboard.detected : messages.dashboard.notDetected;
  }

  function staticRadarSummary(): string {
    const staticRadar = deviceState?.debug?.staticRadar;
    if (!staticRadar?.available) return messages.dashboard.staticRadarUnavailable;
    return staticRadar.presence
      ? messages.dashboard.staticRadarDetected
      : messages.dashboard.staticRadarClear;
  }

  function formatNumber(value: number | null | undefined, suffix = ""): string {
    return typeof value === "number" && Number.isFinite(value) ? `${value}${suffix}` : "-";
  }

  function formatTemperature(value: number | null | undefined): string {
    return formatNumber(value, "°C");
  }

  function formatPercent(value: number | null | undefined): string {
    return formatNumber(value, "%");
  }

  function formatLux(value: number | null | undefined): string {
    return formatNumber(value, " lux");
  }

  function formatStoragePercent(used: number | null | undefined, total: number | null | undefined): string {
    if (!Number.isFinite(used ?? NaN) || !Number.isFinite(total ?? NaN) || Number(total) <= 0) return "-";
    return `${Math.round((Number(used) / Number(total)) * 100)}%`;
  }

  function addBytes(...values: Array<number | null | undefined>): number | undefined {
    if (values.some((value) => !Number.isFinite(value ?? NaN))) return undefined;
    return values.reduce((sum, value) => sum + Number(value), 0);
  }

  function formatUptime(seconds: number | undefined): string {
    if (!Number.isFinite(seconds)) return "-";
    const total = Math.max(0, Math.floor(Number(seconds)));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    if (hours > 0) return messages.dashboard.hoursMinutes(hours, minutes);
    return messages.dashboard.minutes(minutes);
  }

  function wifiSignalInfo(connected: boolean | undefined, rssi: number | null | undefined): { bars: number; label: string } {
    if (!connected || !Number.isFinite(rssi ?? NaN)) {
      return { bars: 0, label: systemStatusLoading ? messages.dashboard.checking : messages.dashboard.notConnected };
    }
    const value = Number(rssi);
    if (value >= -50) return { bars: 4, label: messages.dashboard.wifiExcellent };
    if (value >= -60) return { bars: 3, label: messages.dashboard.wifiGood };
    if (value >= -70) return { bars: 2, label: messages.dashboard.wifiFair };
    if (value >= -80) return { bars: 1, label: messages.dashboard.wifiWeak };
    return { bars: 1, label: messages.dashboard.wifiVeryWeak };
  }

  function formatDuration(seconds: number | null | undefined): string {
    if (typeof seconds !== "number" || !Number.isFinite(seconds)) return "-";
    if (seconds < 1) return messages.dashboard.always;
    if (seconds >= 60) return messages.dashboard.minutes(Math.round(seconds / 60));
    return messages.dashboard.seconds(Math.round(seconds));
  }

  function formatOffset(value: number | null | undefined, suffix: string): string {
    if (typeof value !== "number" || !Number.isFinite(value)) return "-";
    return `${value > 0 ? "+" : ""}${value.toFixed(1)}${suffix}`;
  }

  function clampTemperatureOffset(value: number): number {
    return Math.max(-2, Math.min(2, Math.round(value * 10) / 10));
  }

  function clampHumidityOffset(value: number): number {
    return Math.max(-5, Math.min(5, Math.round(value * 2) / 2));
  }

  $effect(() => {
    if (!hasFloorplan || floorplanDocument || floorplanLoading) return;
    void loadDashboardFloorplan();
  });

  $effect(() => {
    if (hasFloorplan) return;
    floorplanDocument = null;
    floorplanLoading = false;
    floorplanError = "";
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
    floorplanImageUrl = "";
  });

  onDestroy(() => {
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
  });

  async function loadDashboardFloorplan(): Promise<void> {
    floorplanLoading = true;
    floorplanError = "";
    try {
      const status = await loadFloorplanStorageStatus({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher });
      if (!status.hasConfig || !status.hasImage) {
        floorplanDocument = null;
        floorplanError = messages.dashboard.noSavedFloorplan;
        return;
      }
      const [document, image] = await Promise.all([
        loadFloorplanStorageDocument({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
        loadFloorplanStorageImage({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher })
      ]);
      if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
      floorplanDocument = document;
      floorplanImageUrl = URL.createObjectURL(image);
    } catch (error) {
      floorplanError = apiErrorMessage(messages, error);
      floorplanDocument = null;
      if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
      floorplanImageUrl = "";
    } finally {
      floorplanLoading = false;
    }
  }

  function floorplanPreviewStyle(): string {
    const width = floorplanDocument?.image?.width ?? 1;
    const height = floorplanDocument?.image?.height ?? 1;
    return floorplanImageFrameStyle(width, height, 62);
  }

  function roomPoints(points: Array<[number, number]>): string {
    return points.map(([x, y]) => `${x},${y}`).join(" ");
  }

  function roomCenter(points: Array<[number, number]>): [number, number] {
    if (!points.length) return [0, 0];
    const sum = points.reduce(
      (acc, [x, y]) => {
        acc[0] += x;
        acc[1] += y;
        return acc;
      },
      [0, 0]
    );
    return [sum[0] / points.length, sum[1] / points.length];
  }

  function floorplanScaleEstimate() {
    if (!floorplanDocument) return null;
    const [x, y, width, height] = floorplanDocument.scale.outerBoundsPx;
    return {
      outerBounds: { x, y, width, height },
      widthMm: floorplanDocument.scale.widthMm,
      heightMm: floorplanDocument.scale.heightMm,
      mmPerPxX: floorplanDocument.scale.mmPerPxX,
      mmPerPxY: floorplanDocument.scale.mmPerPxY
    };
  }

  function floorplanRadarPlacement() {
    if (!floorplanDocument) return null;
    return {
      originX: floorplanDocument.radar.originPx[0],
      originY: floorplanDocument.radar.originPx[1],
      rotation: floorplanDocument.radar.rotationDeg
    };
  }

  function roomBoundarySegments() {
    if (!floorplanDocument) return [];
    const placement = floorplanRadarPlacement();
    if (!placement) return [];
    const room = findRoomContainingPoint(
      floorplanDocument.rooms,
      { x: placement.originX, y: placement.originY },
      (candidate) => candidate.pointsPx
    );
    if (!room) return [];
    return buildBoundaryOcclusionSegments([{
      id: room.id,
      points: room.pointsPx,
      segmentPrefix: "stored-room"
    }]);
  }

  function radarOcclusionSegments() {
    return roomBoundarySegments();
  }

  async function chooseStatusLed(enabled: boolean): Promise<void> {
    await onSetStatusLed(enabled);
    ledDialogOpen = false;
  }

  async function chooseLedDuration(seconds: number): Promise<void> {
    await onSetLedBlinkDuration(seconds);
    ledDialogOpen = false;
  }

  async function chooseEnvironmentCorrection(enabled: boolean): Promise<void> {
    await onSetEnvironmentCorrection(enabled);
  }

  async function nudgeTemperatureOffset(delta: number): Promise<void> {
    await onSetTemperatureOffset(clampTemperatureOffset((controlStatus?.temperatureOffset ?? 0) + delta));
  }

  async function nudgeHumidityOffset(delta: number): Promise<void> {
    await onSetHumidityOffset(clampHumidityOffset((controlStatus?.humidityOffset ?? 0) + delta));
  }

  function closeLedDialogFromBackdrop(event: MouseEvent): void {
    if (event.target === event.currentTarget) ledDialogOpen = false;
  }

  function closeEnvironmentDialogFromBackdrop(event: MouseEvent): void {
    if (event.target === event.currentTarget) environmentDialogOpen = false;
  }

  function currentTimezoneLabel(): string {
    const timezone = normalizeSupportedTimezone(controlStatus?.timezone);
    if (timezone) return messages.dashboard.timezoneOptions[timezone];
    return controlStatus?.timezoneKnown && controlStatus.timezone ? controlStatus.timezone : "-";
  }

  function openTimezoneDialog(): void {
    selectedTimezone = normalizeSupportedTimezone(controlStatus?.timezone) ?? "Asia/Seoul";
    timezoneDetectionText = "";
    timezoneDialogOpen = true;
  }

  function closeTimezoneDialogFromBackdrop(event: MouseEvent): void {
    if (event.target === event.currentTarget && !controlActionBusy) timezoneDialogOpen = false;
  }

  function chooseBrowserTimezone(): void {
    const detected = detectBrowserTimezone();
    if (detected.supported) {
      selectedTimezone = detected.supported;
      timezoneDetectionText = messages.dashboard.timezoneBrowserDetected(detected.detected ?? detected.supported);
    } else if (detected.detected) {
      timezoneDetectionText = messages.dashboard.timezoneBrowserUnsupported(detected.detected);
    } else {
      timezoneDetectionText = messages.dashboard.timezoneBrowserUnavailable;
    }
  }

  function handleTimezoneSelection(event: Event): void {
    const timezone = normalizeSupportedTimezone((event.currentTarget as HTMLSelectElement).value);
    if (timezone) selectedTimezone = timezone;
  }

  async function applyTimezone(): Promise<void> {
    const currentTimezone = normalizeSupportedTimezone(controlStatus?.timezone);
    if (currentTimezone === selectedTimezone) {
      timezoneDialogOpen = false;
      return;
    }
    if (await onSetTimezone(selectedTimezone)) timezoneDialogOpen = false;
  }

  const wifiInfo = $derived(wifiSignalInfo(systemStatus?.wifi?.connected, systemStatus?.wifi?.rssi));
  const dataStorageUsedBytes = $derived(systemStatus?.flash?.storageUsedBytes ?? systemStatus?.storage?.usedBytes);
  const flashOccupiedBytes = $derived(
    addBytes(systemStatus?.flash?.firmwareUsedBytes, systemStatus?.flash?.otaSlotBytes, dataStorageUsedBytes)
  );
  const flashOccupancyText = $derived(
    `${formatCompactBytes(flashOccupiedBytes)} / ${formatCompactBytes(systemStatus?.flash?.totalBytes)}`
  );
  const flashOccupancyPercentText = $derived(formatStoragePercent(flashOccupiedBytes, systemStatus?.flash?.totalBytes));
</script>

<section class="dashboard-workspace">
  <div class="floorplan-workflow-card dashboard-intro-card">
    <div>
      <strong>{messages.dashboard.title}</strong>
      <span>{messages.dashboard.description}</span>
    </div>
    <dl class="floorplan-stored-summary dashboard-intro-summary">
      <div>
        <dt>{messages.dashboard.connection}</dt>
          <dd>{deviceState?.connected ? messages.dashboard.connected : messages.dashboard.waiting}</dd>
      </div>
      <div>
        <dt>{messages.dashboard.updated}</dt>
        <dd>{updatedText}</dd>
      </div>
    </dl>
  </div>

  <section class="dashboard-status-grid" aria-label={messages.dashboard.coreStatus}>
    <div class="floorplan-workflow-card dashboard-status-card" data-tone={deviceState?.presence ? "ok" : "idle"}>
      <div>
        <span>{messages.dashboard.presence}</span>
        <strong>{yesNo(deviceState?.presence)}</strong>
      </div>
      <small>{staticRadarSummary()}</small>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card" data-tone={deviceState?.motion ? "ok" : "idle"}>
      <div>
        <span>{messages.dashboard.motion}</span>
        <strong>{yesNo(deviceState?.motion)}</strong>
      </div>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card">
      <div>
        <span>{messages.dashboard.targetCount}</span>
        <strong>{messages.dashboard.targetCountValue(targetCount)}</strong>
      </div>
      <div>
        <small>{messages.dashboard.targetSummary(movingTargetCount, stillTargetCount)}</small>
      </div>
    </div>

    <div class="floorplan-workflow-card dashboard-status-card">
      <div>
        <span>{messages.dashboard.environment}</span>
        <strong>{formatTemperature(deviceState?.temperatureC)}</strong>
      </div>
      <small>{formatPercent(deviceState?.humidityPercent)} · {formatLux(deviceState?.illuminanceLux)}</small>
    </div>
  </section>

  <section class="dashboard-main-grid">
    <section class="dashboard-visual-panel">
      <div class="radar-scene-frame dashboard-visual-frame">
        <div class="dashboard-visual-status">
          <span>{hasFloorplan ? messages.dashboard.floorplanReady : messages.dashboard.radarReady}</span>
          <strong>{messages.dashboard.targetLabel(targetCount)}</strong>
        </div>
        {#if floorplanDocument && floorplanImageUrl}
          <div class="dashboard-floorplan-preview">
            <FloorplanImageFrame
              imageUrl={floorplanImageUrl}
              imageWidth={floorplanDocument.image.width}
              imageHeight={floorplanDocument.image.height}
              frameStyle={floorplanPreviewStyle()}
              ariaLabel={messages.dashboard.savedFloorplanAria}
            >
            <svg
              class="dashboard-floorplan-room-layer"
              viewBox={`0 0 ${floorplanDocument.image.width} ${floorplanDocument.image.height}`}
              preserveAspectRatio="none"
              role="presentation"
            >
              {#each floorplanDocument.rooms as room}
                {@const center = roomCenter(room.pointsPx)}
                <polygon class="dashboard-floorplan-room" points={roomPoints(room.pointsPx)} />
                <text class="dashboard-floorplan-room-label" x={center[0]} y={center[1]}>{room.name}</text>
              {/each}
            </svg>
            <FloorplanRadarPlacementOverlay
              imageWidth={floorplanDocument.image.width}
              imageHeight={floorplanDocument.image.height}
              scaleEstimate={floorplanScaleEstimate()}
              placement={floorplanRadarPlacement()}
              scalePercent={(floorplanDocument.radar.scale ?? 1) * 100}
              zones={config?.zones ?? []}
              calibrationZones={config?.calibrationZones ?? []}
              targets={displayTargets}
              wallSegments={roomBoundarySegments()}
              occlusionSegments={radarOcclusionSegments()}
              ignoredOcclusionSegmentIds={floorplanDocument.occlusion?.ignoredEdges ?? []}
              readOnly
              showPlacementLabel={false}
            />
            </FloorplanImageFrame>
          </div>
        {:else if hasFloorplan || floorplanLoading || floorplanError}
          <div class="dashboard-visual-placeholder">
            <strong>
              {floorplanLoading
                ? messages.dashboard.floorplanLoading
                : hasFloorplan
                  ? messages.dashboard.floorplanViewArea
                  : messages.dashboard.radarViewArea}
            </strong>
            <span>
              {floorplanError
                ? messages.dashboard.floorplanLoadFailed(floorplanError)
                : hasFloorplan
                  ? messages.dashboard.floorplanImageLoading
                  : messages.dashboard.radarFallback}
            </span>
          </div>
        {:else}
          <div class="dashboard-radar-preview">
            <RadarScene messages={messages} state={deviceState} {config} />
          </div>
        {/if}
      </div>
    </section>

    <aside class="dashboard-side-stack">
      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>{messages.dashboard.quickNav}</strong>
        <div class="floorplan-stored-tools dashboard-nav-tools">
          <button type="button" onclick={() => onNavigate("zones")}>{messages.tabs.zones}</button>
          <button type="button" onclick={() => onNavigate("floorplan")}>{messages.tabs.floorplan}</button>
          <button type="button" onclick={() => onNavigate("stats")}>{messages.tabs.stats}</button>
          <button type="button" onclick={() => onNavigate("backup")}>{messages.tabs.backup}</button>
        </div>
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>{messages.dashboard.zoneStatus}</strong>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{messages.dashboard.zone}</dt>
            <dd>{messages.dashboard.countValue(zoneCount)}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.filter}</dt>
            <dd>{messages.dashboard.countValue(filterZoneCount)}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.reduced}</dt>
            <dd>{messages.dashboard.countValue(reducedZoneCount)}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.disabled}</dt>
            <dd>{messages.dashboard.countValue(disabledZoneCount)}</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card">
        <strong>{messages.dashboard.deviceStatus}</strong>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{messages.dashboard.firmware}</dt>
            <dd>{systemStatus?.firmware?.version ?? (systemStatusLoading ? messages.dashboard.checking : "-")}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.dashboard}</dt>
            <dd>{systemStatus?.dashboard?.version ?? "-"}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.wifi}</dt>
            <dd>
              <span class="wifi-strength" aria-label={`Wi-Fi ${wifiInfo.label}`}>
                {#each [0, 1, 2, 3] as index}
                  <span class:active={index < wifiInfo.bars}></span>
                {/each}
              </span>
              {wifiInfo.label}{systemStatus?.wifi?.connected ? ` · ${systemStatus.wifi?.rssi ?? "-"}dBm` : ""}
            </dd>
          </div>
          <div>
            <dt>{messages.dashboard.storage}</dt>
            <dd>{messages.dashboard.storageUsed(flashOccupancyText, flashOccupancyPercentText)}</dd>
          </div>
          <div>
            <dt>{messages.dashboard.uptime}</dt>
            <dd>{formatUptime(systemStatus?.firmware?.uptimeSeconds)}</dd>
          </div>
        </dl>
        {#if systemStatusError}
          <p class="dashboard-note">{messages.dashboard.systemInfoFailed(systemStatusError)}</p>
        {/if}
      </div>

      <div class="floorplan-workflow-card dashboard-metric-card dashboard-tuning-control-card">
        <strong>{messages.dashboard.advancedSettings}</strong>
        <button
          class="dashboard-tuning-toggle"
          type="button"
          aria-expanded={tuningOpen}
          onclick={() => (tuningOpen = !tuningOpen)}
        >
          {tuningOpen ? messages.dashboard.collapse : messages.dashboard.expand}
        </button>
      </div>

      {#if tuningOpen}
        <div class="floorplan-workflow-card dashboard-tuning-card">
          <strong>{messages.dashboard.advancedTuning}</strong>
          <span>{messages.dashboard.advancedTuningDescription}</span>
          <dl class="floorplan-stored-summary">
            <div>
              <dt>{messages.dashboard.ledBehavior}</dt>
              <dd>
                {controlStatusLoading
                  ? messages.dashboard.checking
                  : controlStatus?.statusLedEnabled
                    ? messages.dashboard.enabled
                    : controlStatus?.statusLedKnown
                      ? messages.dashboard.disabledState
                      : "-"}
              </dd>
            </div>
            <div>
              <dt>{messages.dashboard.autoOff}</dt>
              <dd>{formatDuration(controlStatus?.ledBlinkDuration)}</dd>
            </div>
            <div>
              <dt>{messages.dashboard.environmentCorrection}</dt>
              <dd>
                {controlStatusLoading
                  ? messages.dashboard.checking
                  : controlStatus?.environmentCorrectionEnabled
                    ? messages.dashboard.enabled
                    : controlStatus?.environmentCorrectionKnown
                      ? messages.dashboard.disabledState
                  : "-"}
              </dd>
            </div>
            <div>
              <dt>{messages.dashboard.legacyPresenceFallback}</dt>
              <dd>{legacyPresenceFallback ? messages.dashboard.enabled : messages.dashboard.disabledState}</dd>
            </div>
            <div>
              <dt>{messages.dashboard.timezone}</dt>
              <dd>{controlStatusLoading ? messages.dashboard.checking : currentTimezoneLabel()}</dd>
            </div>
          </dl>
          {#if controlStatusError}
            <p class="dashboard-note">{messages.dashboard.controlStatusFailed(controlStatusError)}</p>
          {/if}
          <div class="dashboard-tuning-actions">
            <button type="button" disabled={controlActionBusy} onclick={() => (environmentDialogOpen = true)}>{messages.dashboard.environmentCorrection}</button>
            <button
              type="button"
              disabled={controlActionBusy || !config}
              title={messages.dashboard.legacyPresenceFallbackDescription}
              onclick={() => onSetLegacyPresenceFallback(!legacyPresenceFallback)}
            >
              {legacyPresenceFallback ? messages.dashboard.turnOff : messages.dashboard.turnOn}
            </button>
            <button type="button" disabled={controlActionBusy} onclick={() => (ledDialogOpen = true)}>{messages.dashboard.statusLedControl}</button>
            <button type="button" disabled={controlActionBusy} onclick={openTimezoneDialog}>{messages.dashboard.timezoneChange}</button>
            <button type="button" onclick={() => (diagnosticDialogOpen = true)}>{messages.dashboard.diagnostics}</button>
            <button type="button" disabled={integrationModeActionBusy} onclick={onChangeIntegrationMode}>
              {integrationModeActionBusy ? messages.dashboard.changing : integrationModeButtonText}
            </button>
          </div>
        </div>
      {/if}
    </aside>
  </section>
</section>

{#if ledDialogOpen}
  <div class="dashboard-dialog-backdrop" role="presentation" onclick={closeLedDialogFromBackdrop}>
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="dashboard-led-dialog-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="dashboard-led-dialog-title">{messages.dashboard.statusLedControl}</strong>
        <span>{messages.dashboard.ledDialogDescription}</span>
      </div>
      <dl class="floorplan-stored-summary">
        <div>
          <dt>{messages.dashboard.ledBehavior}</dt>
          <dd>{controlStatus?.statusLedEnabled ? messages.dashboard.enabled : controlStatus?.statusLedKnown ? messages.dashboard.disabledState : "-"}</dd>
        </div>
        <div>
          <dt>{messages.dashboard.autoOff}</dt>
          <dd>{formatDuration(controlStatus?.ledBlinkDuration)}</dd>
        </div>
      </dl>
      <div class="dashboard-dialog-section">
        <strong>{messages.dashboard.ledBehavior}</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseStatusLed(true)}>{messages.dashboard.turnOn}</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseStatusLed(false)}>{messages.dashboard.turnOff}</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>{messages.dashboard.autoOff}</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(0)}>{messages.dashboard.always}</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(30)}>{messages.dashboard.seconds(30)}</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(60)}>{messages.dashboard.seconds(60)}</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseLedDuration(300)}>{messages.dashboard.minutes(5)}</button>
        </div>
      </div>
      <div class="floorplan-delete-dialog-actions">
        <button
          type="button"
          class="danger-button dashboard-dialog-close-button"
          disabled={controlActionBusy}
          onclick={() => (ledDialogOpen = false)}
        >
          {messages.common.close}
        </button>
      </div>
    </div>
  </div>
{/if}

<DiagnosticDialog open={diagnosticDialogOpen} state={deviceState} onClose={() => (diagnosticDialogOpen = false)} />

{#if timezoneDialogOpen}
  <div class="dashboard-dialog-backdrop" role="presentation" onclick={closeTimezoneDialogFromBackdrop}>
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="dashboard-timezone-dialog-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="dashboard-timezone-dialog-title">{messages.dashboard.timezoneChange}</strong>
        <span>{messages.dashboard.timezoneDialogDescription}</span>
      </div>
      <div class="dashboard-dialog-section">
        <label for="dashboard-timezone-select">{messages.dashboard.timezoneSelectLabel}</label>
        <select
          id="dashboard-timezone-select"
          class="dashboard-timezone-select"
          value={selectedTimezone}
          disabled={controlActionBusy}
          onchange={handleTimezoneSelection}
        >
          {#each TIMEZONE_IDS as timezone}
            <option value={timezone}>{messages.dashboard.timezoneOptions[timezone]}</option>
          {/each}
        </select>
        <button type="button" disabled={controlActionBusy} onclick={chooseBrowserTimezone}>
          {messages.dashboard.timezoneUseBrowser}
        </button>
        {#if timezoneDetectionText}
          <p class="dashboard-note">{timezoneDetectionText}</p>
        {/if}
      </div>
      <div class="dashboard-timezone-warning" role="alert">
        <strong>{messages.dashboard.timezoneWarningTitle}</strong>
        <span>{messages.dashboard.timezoneWarning}</span>
      </div>
      <div class="floorplan-delete-dialog-actions">
        <button type="button" disabled={controlActionBusy} onclick={() => (timezoneDialogOpen = false)}>
          {messages.common.cancel}
        </button>
        <button
          type="button"
          class="danger-button"
          disabled={controlActionBusy || normalizeSupportedTimezone(controlStatus?.timezone) === selectedTimezone}
          onclick={applyTimezone}
        >
          {controlActionBusy ? messages.dashboard.changing : messages.dashboard.timezoneApply}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if environmentDialogOpen}
  <div class="dashboard-dialog-backdrop" role="presentation" onclick={closeEnvironmentDialogFromBackdrop}>
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="dashboard-environment-dialog-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="dashboard-environment-dialog-title">{messages.dashboard.environmentCorrection}</strong>
        <span>{messages.dashboard.environmentDialogDescription}</span>
      </div>
      <dl class="floorplan-stored-summary">
        <div>
          <dt>{messages.dashboard.correctionBehavior}</dt>
          <dd>{controlStatus?.environmentCorrectionEnabled ? messages.dashboard.enabled : controlStatus?.environmentCorrectionKnown ? messages.dashboard.disabledState : "-"}</dd>
        </div>
        <div>
          <dt>{messages.dashboard.temperatureOffset}</dt>
          <dd>{formatOffset(controlStatus?.temperatureOffset, "°C")}</dd>
        </div>
        <div>
          <dt>{messages.dashboard.humidityOffset}</dt>
          <dd>{formatOffset(controlStatus?.humidityOffset, "%")}</dd>
        </div>
      </dl>
      <div class="dashboard-dialog-section">
        <strong>{messages.dashboard.correctionBehavior}</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => chooseEnvironmentCorrection(true)}>{messages.dashboard.turnOn}</button>
          <button type="button" disabled={controlActionBusy} onclick={() => chooseEnvironmentCorrection(false)}>{messages.dashboard.turnOff}</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>{messages.dashboard.temperatureOffset}</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeTemperatureOffset(-0.1)}>-0.1°C</button>
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeTemperatureOffset(0.1)}>+0.1°C</button>
        </div>
      </div>
      <div class="dashboard-dialog-section">
        <strong>{messages.dashboard.humidityOffset}</strong>
        <div class="dashboard-dialog-actions">
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeHumidityOffset(-0.5)}>-0.5%</button>
          <button type="button" disabled={controlActionBusy} onclick={() => nudgeHumidityOffset(0.5)}>+0.5%</button>
        </div>
      </div>
      <div class="floorplan-delete-dialog-actions">
        <button
          type="button"
          class="danger-button dashboard-dialog-close-button"
          disabled={controlActionBusy}
          onclick={() => (environmentDialogOpen = false)}
        >
          {messages.common.close}
        </button>
      </div>
    </div>
  </div>
{/if}
