<script lang="ts">
  import { onDestroy, onMount } from "svelte";
  import { apiErrorMessage } from "../api/api-message";
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import { presenceDisplayTargets } from "../../core/presence-target-display";
  import {
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus,
    floorplanStorageRevision,
    hasCompleteFloorplanStorage,
    type FloorplanStorageStatus
  } from "../floorplan/floorplan-storage-client";
  import { floorplanImageFrameStyle } from "../floorplan/floorplan-image-frame";
  import { physicalWallOcclusionSegments } from "../floorplan/floorplan-physical-walls";
  import {
    storedRoomToCandidate,
    withLocalFloorplanRadarPlacement
  } from "../floorplan/stored-floorplan-document";
  import { storedFloorplanViewBounds } from "../floorplan/stored-floorplan-view";
  import FloorplanCandidateOverlay from "./FloorplanCandidateOverlay.svelte";
  import FloorplanImageFrame from "./floorplan/FloorplanImageFrame.svelte";
  import FloorplanPhysicalWallOverlay from "./floorplan/FloorplanPhysicalWallOverlay.svelte";
  import ManualFloorplanDoorOverlay from "./floorplan/ManualFloorplanDoorOverlay.svelte";
  import FloorplanRadarPlacementOverlay from "./FloorplanRadarPlacementOverlay.svelte";
  import DiagnosticDialog from "./DiagnosticDialog.svelte";
  import type { Messages } from "../i18n";
  import RadarScene from "./RadarScene.svelte";
  import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
  import { platformFloorplanTargets, platformPeerRadarMarkers } from "../platform-radar-display";
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
    platformSiteConfiguration: PlatformSiteConfigurationStatus | null;
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
    platformSiteConfiguration,
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
  let floorplanStatusLoading = false;
  let loadedFloorplanRevision = "";
  let floorplanStatusTimer = 0;

  const targetCount = $derived(deviceState?.targetCount ?? deviceState?.targets.filter((target) => target.active).length ?? 0);
  const movingTargetCount = $derived(deviceState?.movingTargetCount ?? 0);
  const stillTargetCount = $derived(deviceState?.stillTargetCount ?? 0);
  const displayTargets = $derived(presenceDisplayTargets(deviceState));
  const floorplanDisplayTargets = $derived(platformFloorplanTargets(
    deviceState,
    displayTargets,
    platformSiteConfiguration,
    floorplanDocument,
    systemStatus?.platform?.nodeId
  ));
  const configuredFloorplan = $derived(Boolean(config?.floorplan?.enabled && config.floorplan.hasImage));
  const hasFloorplan = $derived(configuredFloorplan);
  const canProjectFloorplanTargets = $derived(
    Boolean(config?.floorplan?.radar || config?.floorplan?.room)
  );
  const userZones = $derived(config?.zones ?? []);
  const calibrationZones = $derived(config?.calibrationZones ?? []);
  const userFilterZoneCount = $derived(userZones.filter((zone) => zone.type === "filter").length);
  const userReducedZoneCount = $derived(userZones.filter((zone) => zone.type === "reduced").length);
  const userDisabledZoneCount = $derived(userZones.filter((zone) => zone.type === "disabled").length);
  const calibrationFilterZoneCount = $derived(calibrationZones.filter((zone) => zone.type === "filter").length);
  const calibrationReducedZoneCount = $derived(calibrationZones.filter((zone) => zone.type === "reduced").length);
  const calibrationDisabledZoneCount = $derived(calibrationZones.filter((zone) => zone.type === "disabled").length);
  const platformConnected = $derived(
    systemStatus?.platform?.mode === "enrolled" ||
      (systemStatus?.platform?.connectedRemoteParticipantCount ??
        systemStatus?.platform?.activeRemoteParticipantCount ??
        0) > 0
  );
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
    if (!hasFloorplan) {
      clearDashboardFloorplan();
      return;
    }
    if (!floorplanDocument && !floorplanLoading) void refreshDashboardFloorplan();
  });

  onMount(() => {
    void refreshDashboardFloorplan();
    floorplanStatusTimer = window.setInterval(() => {
      void refreshDashboardFloorplan();
    }, 5000);
  });

  onDestroy(() => {
    window.clearInterval(floorplanStatusTimer);
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
  });

  function clearDashboardFloorplan(): void {
    floorplanDocument = null;
    floorplanLoading = false;
    floorplanError = "";
    loadedFloorplanRevision = "";
    if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
    floorplanImageUrl = "";
  }

  async function refreshDashboardFloorplan(): Promise<void> {
    if (!hasFloorplan || floorplanStatusLoading || floorplanLoading) return;
    floorplanStatusLoading = true;
    try {
      const status = await loadFloorplanStorageStatus({
        baseUrl: floorplanStorageBaseUrl,
        fetcher: floorplanStorageFetcher
      });
      const detected = hasCompleteFloorplanStorage(status);
      if (!detected) {
        clearDashboardFloorplan();
        floorplanError = messages.dashboard.noSavedFloorplan;
        return;
      }

      const revision = floorplanStorageRevision(status);
      if (floorplanDocument && floorplanImageUrl && revision === loadedFloorplanRevision) return;
      await loadDashboardFloorplan(status, revision);
    } catch (error) {
      floorplanError = apiErrorMessage(messages, error);
    } finally {
      floorplanStatusLoading = false;
    }
  }

  async function loadDashboardFloorplan(
    status: FloorplanStorageStatus,
    revision: string
  ): Promise<void> {
    floorplanLoading = true;
    floorplanError = "";
    try {
      const [document, image] = await Promise.all([
        loadFloorplanStorageDocument({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
        loadFloorplanStorageImage({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher })
      ]);
      if (floorplanImageUrl) URL.revokeObjectURL(floorplanImageUrl);
      floorplanDocument = withLocalFloorplanRadarPlacement(document, config?.floorplan?.radar);
      floorplanImageUrl = URL.createObjectURL(image);
      loadedFloorplanRevision = revision || floorplanStorageRevision(status);
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
    const bounds = storedFloorplanViewBounds(floorplanDocument);
    return floorplanImageFrameStyle(bounds.width, bounds.height, 72);
  }

  function floorplanViewBounds() {
    return storedFloorplanViewBounds(floorplanDocument);
  }

  function floorplanRoomCandidates() {
    return floorplanDocument?.rooms.map(storedRoomToCandidate) ?? [];
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
    return physicalWallOcclusionSegments(floorplanDocument);
  }

  function radarOcclusionSegments() {
    return roomBoundarySegments();
  }

  function structuredManualFloorplan() {
    return floorplanDocument?.source === "manual" &&
      Array.isArray(floorplanDocument.doors) &&
      Boolean(floorplanDocument.wallSegments?.length);
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
              viewBounds={floorplanViewBounds()}
              frameStyle={floorplanPreviewStyle()}
              ariaLabel={messages.dashboard.savedFloorplanAria}
              showImage={!structuredManualFloorplan()}
              backgroundFill={structuredManualFloorplan() ? "#f4f6f3" : ""}
            >
            <FloorplanCandidateOverlay
              candidates={floorplanRoomCandidates()}
              imageWidth={floorplanDocument.image.width}
              imageHeight={floorplanDocument.image.height}
              viewBounds={floorplanViewBounds()}
              ariaLabels={{
                candidateLayer: messages.floorplan.panel.roomCandidateLayerAria,
                addSplitPoint: messages.floorplan.panel.addRoomSplitPointAria,
                addManualPoint: messages.floorplan.panel.addManualRoomPointAria
              }}
              allowCandidateEditing={false}
              selectionLocked
              metricMmPerPx={(floorplanDocument.scale.mmPerPxX + floorplanDocument.scale.mmPerPxY) / 2}
              showMetricGrid={false}
            />
            {#if structuredManualFloorplan()}
              <FloorplanPhysicalWallOverlay
                segments={roomBoundarySegments()}
                imageWidth={floorplanDocument.image.width}
                imageHeight={floorplanDocument.image.height}
                viewBounds={floorplanViewBounds()}
              />
            {/if}
            {#if structuredManualFloorplan() && floorplanDocument.doors?.length}
              <ManualFloorplanDoorOverlay
                doors={floorplanDocument.doors}
                imageWidth={floorplanDocument.image.width}
                imageHeight={floorplanDocument.image.height}
                viewBounds={floorplanViewBounds()}
                mmPerPx={(floorplanDocument.scale.mmPerPxX + floorplanDocument.scale.mmPerPxY) / 2}
              />
            {/if}
            {#if canProjectFloorplanTargets}
              <FloorplanRadarPlacementOverlay
                imageWidth={floorplanDocument.image.width}
                imageHeight={floorplanDocument.image.height}
                viewBounds={floorplanViewBounds()}
                scaleEstimate={floorplanScaleEstimate()}
                placement={floorplanRadarPlacement()}
                scalePercent={(floorplanDocument.radar.scale ?? 1) * 100}
                zones={config?.zones ?? []}
                calibrationZones={config?.calibrationZones ?? []}
                targets={floorplanDisplayTargets}
                peerRadars={platformPeerRadarMarkers(
                  platformSiteConfiguration,
                  floorplanDocument,
                  systemStatus?.platform?.nodeId
                )}
                peerRadarLabel={messages.dashboard.connectedRadar}
                fusionLabel={messages.common.fused}
                wallSegments={roomBoundarySegments()}
                occlusionSegments={radarOcclusionSegments()}
                ignoredOcclusionSegmentIds={floorplanDocument.occlusion?.ignoredEdges ?? []}
                readOnly
                showPlacementLabel={false}
              />
            {/if}
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
        <div class="dashboard-zone-summary-grid">
          <div>
            <strong>{messages.dashboard.userZones(userZones.length)}</strong>
            <dl class="floorplan-stored-summary">
              <div>
                <dt>{messages.dashboard.filter}</dt>
                <dd>{messages.dashboard.countValue(userFilterZoneCount)}</dd>
              </div>
              <div>
                <dt>{messages.dashboard.reduced}</dt>
                <dd>{messages.dashboard.countValue(userReducedZoneCount)}</dd>
              </div>
              <div>
                <dt>{messages.dashboard.disabled}</dt>
                <dd>{messages.dashboard.countValue(userDisabledZoneCount)}</dd>
              </div>
            </dl>
          </div>
          <div>
            <strong>{messages.dashboard.calibrationZones(calibrationZones.length)}</strong>
            <dl class="floorplan-stored-summary">
              <div>
                <dt>{messages.dashboard.filter}</dt>
                <dd>{messages.dashboard.countValue(calibrationFilterZoneCount)}</dd>
              </div>
              <div>
                <dt>{messages.dashboard.reduced}</dt>
                <dd>{messages.dashboard.countValue(calibrationReducedZoneCount)}</dd>
              </div>
              <div>
                <dt>{messages.dashboard.disabled}</dt>
                <dd>{messages.dashboard.countValue(calibrationDisabledZoneCount)}</dd>
              </div>
            </dl>
          </div>
        </div>
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
          {#if systemStatus?.platform}
            <div>
              <dt>{messages.dashboard.platformStatus}</dt>
              <dd>
                {systemStatus.platform.identityReady
                  ? messages.dashboard.platformReady
                  : messages.dashboard.platformUnavailable}
              </dd>
            </div>
            <div>
              <dt>{messages.dashboard.platformMode}</dt>
              <dd>
                {platformConnected
                  ? messages.dashboard.platformEnrolled
                  : systemStatus.platform.mode === "standalone"
                    ? messages.dashboard.platformStandalone
                    : systemStatus.platform.mode ?? "-"}
              </dd>
            </div>
            {#if systemStatus.platform.identityReady}
              <div>
                <dt>{messages.dashboard.nodeId}</dt>
                <dd class="dashboard-platform-id">{systemStatus.platform.nodeId ?? "-"}</dd>
              </div>
              <div>
                <dt>{messages.dashboard.siteId}</dt>
                <dd class="dashboard-platform-id">{systemStatus.platform.siteId ?? "-"}</dd>
              </div>
            {/if}
          {/if}
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
