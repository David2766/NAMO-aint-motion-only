<script lang="ts">
  import CalibrationPanel from "../components/CalibrationPanel.svelte";
  import DebugPanel from "../components/DebugPanel.svelte";
  import MapToolbar from "../components/MapToolbar.svelte";
  import RadarScene from "../components/RadarScene.svelte";
  import StaticRadarTuningPanel from "../components/StaticRadarTuningPanel.svelte";
  import ZonePanel from "../components/ZonePanel.svelte";
  import type { Messages } from "../i18n/types";
  import { staticRadarDetectedGateNumber } from "../static-radar-tuning";
  import { isDeviceStorageSaveDisabled } from "../state/device-storage-status";
  import type {
    DeviceApi,
    SaveState,
    WebDeviceConfig,
    WebDeviceState,
    WebStaticRadarTuningStatus,
    WebZone,
    WebZoneType
  } from "../types";

  type ZoneTool = "" | "zones" | "calibration" | "staticRadar";
  type CalibrationActionType = Extract<WebZoneType, "filter" | "reduced" | "disabled">;

  type Props = {
    api: DeviceApi;
    messages: Messages;
    activeZoneTool: ZoneTool;
    activeTargetCount: number;
    calibrationCanStart: boolean;
    calibrationRun: boolean;
    calibrationStatusText: string;
    calibrationTypeLabels: Record<CalibrationActionType, string>;
    calibrationZones: WebZone[];
    canRedo: boolean;
    canUndo: boolean;
    config: WebDeviceConfig | null;
    debugMode: boolean;
    displayConfig: WebDeviceConfig | null;
    saveState: SaveState;
    saveStatusText: string;
    selectedCalibrationZone: WebZone | null;
    selectedLabel: string;
    selectedPointIndex: number;
    selectedZone: WebZone | null;
    selectedZoneId: string;
    state: WebDeviceState | null;
    updatedText: string;
    zoneTypeLabels: Record<WebZoneType, string>;
    zones: WebZone[];
    onAddZone: () => void;
    onCalibrationInfoClick: (zoneId: string) => void;
    onConvertToRect: () => void;
    onDeleteCalibrationZone: (zoneId: string) => void;
    onDeleteSelected: () => void;
    onRedo: () => void;
    onSaveConfig: () => void;
    onSelectZone: (zoneId: string) => void;
    onSetActiveZoneTool: (tool: ZoneTool) => void;
    onSetCalibrationZoneType: (zoneId: string, type: CalibrationActionType) => void;
    onSetDebugMode: (enabled: boolean) => void;
    onSetZoneName: (name: string) => void;
    onSetZoneType: (type: WebZoneType) => void;
    onStartCalibration: () => void;
    onStopCalibration: () => void;
    onUndo: () => void;
    onZoneEdgeClick: (event: MouseEvent) => void;
    onZonePointDoubleClick: (event: MouseEvent) => void;
    onZonePointerDown: (event: PointerEvent) => void;
    onCanvasClick: (event: MouseEvent) => void;
    onDetectionOwnerRequired: () => void;
    isZoneEditable: (zone: WebZone) => boolean;
  };

  let {
    api,
    messages,
    activeZoneTool,
    activeTargetCount,
    calibrationCanStart,
    calibrationRun,
    calibrationStatusText,
    calibrationTypeLabels,
    calibrationZones,
    canRedo,
    canUndo,
    config,
    debugMode,
    displayConfig,
    saveState,
    saveStatusText,
    selectedCalibrationZone,
    selectedLabel,
    selectedPointIndex,
    selectedZone,
    selectedZoneId,
    state: deviceState,
    updatedText,
    zoneTypeLabels,
    zones,
    onAddZone,
    onCalibrationInfoClick,
    onConvertToRect,
    onDeleteCalibrationZone,
    onDeleteSelected,
    onRedo,
    onSaveConfig,
    onSelectZone,
    onSetActiveZoneTool,
    onSetCalibrationZoneType,
    onSetDebugMode,
    onSetZoneName,
    onSetZoneType,
    onStartCalibration,
    onStopCalibration,
    onUndo,
    onZoneEdgeClick,
    onZonePointDoubleClick,
    onZonePointerDown,
    onCanvasClick,
    onDetectionOwnerRequired,
    isZoneEditable
  }: Props = $props();

  const text = $derived(messages.zones);
  const saveDisabled = $derived(isDeviceStorageSaveDisabled(Boolean(config), saveState));
  const staticRadarAvailable = $derived(Boolean(deviceState?.debug?.staticRadar?.available));
  const staticRadarApiAvailable = $derived(
    Boolean(api.getStaticRadarTuningStatus && api.setStaticRadarTuningSession && api.setStaticRadarGateSensitivity)
  );
  let staticRadarTuningStatus = $state<WebStaticRadarTuningStatus | null>(null);
  let selectedStaticRadarGate = $state(0);
  let staticRadarSensitivity = $state(50);
  let staticRadarSensitivityDirty = $state(false);
  let staticRadarTuningBusy = $state(false);
  let staticRadarTuningError = $state("");
  const staticRadarDetectedGate = $derived(
    staticRadarDetectedGateNumber(staticRadarTuningStatus?.gates ?? [], deviceState?.debug?.staticRadar)
  );

  function selectStaticRadarGate(gate: number): void {
    selectedStaticRadarGate = gate;
    const selected = staticRadarTuningStatus?.gates.find((item) => item.gate === gate);
    if (selected) staticRadarSensitivity = selected.sensitivity;
    staticRadarSensitivityDirty = false;
  }

  function setStaticRadarSensitivity(value: number): void {
    staticRadarSensitivity = Math.min(100, Math.max(0, Math.round(value)));
    staticRadarSensitivityDirty = true;
  }

  async function refreshStaticRadarTuning(): Promise<WebStaticRadarTuningStatus | null> {
    if (!api.getStaticRadarTuningStatus) return null;
    const next = await api.getStaticRadarTuningStatus();
    staticRadarTuningStatus = next;
    if (!next.available) {
      staticRadarTuningError = text.staticRadarUnavailable;
      return next;
    }
    if (next.phase === "failed") {
      staticRadarTuningError = text.staticRadarLoadFailed;
      return next;
    }
    staticRadarTuningError = "";
    if (!staticRadarSensitivityDirty) {
      const selected = next.gates.find((gate) => gate.gate === selectedStaticRadarGate) ?? next.gates[0];
      if (selected) {
        selectedStaticRadarGate = selected.gate;
        staticRadarSensitivity = selected.sensitivity;
      }
    }
    return next;
  }

  async function applyStaticRadarSensitivity(): Promise<void> {
    if (!api.setStaticRadarGateSensitivity || staticRadarTuningBusy) return;
    staticRadarTuningBusy = true;
    staticRadarTuningError = "";
    try {
      await api.setStaticRadarGateSensitivity(selectedStaticRadarGate, staticRadarSensitivity);
      staticRadarSensitivityDirty = false;
      await refreshStaticRadarTuning();
    } catch {
      staticRadarTuningError = text.staticRadarApplyFailed;
    } finally {
      staticRadarTuningBusy = false;
    }
  }

  async function resetStaticRadarFactoryDefaults(): Promise<void> {
    if (!api.resetStaticRadarFactoryDefaults || !api.setStaticRadarTuningSession ||
        !api.getStaticRadarTuningStatus || staticRadarTuningBusy) {
      return;
    }
    staticRadarTuningBusy = true;
    staticRadarTuningError = "";
    staticRadarTuningStatus = null;
    staticRadarSensitivityDirty = false;
    try {
      await api.resetStaticRadarFactoryDefaults();
      await new Promise((resolve) => window.setTimeout(resolve, 1200));
      await api.setStaticRadarTuningSession(true);
      for (let attempt = 0; attempt < 6; attempt++) {
        await new Promise((resolve) => window.setTimeout(resolve, 500));
        const next = await refreshStaticRadarTuning();
        if (next?.active && next.gates.length === 9) return;
      }
      throw new Error("static_radar_factory_reset_timeout");
    } catch (cause) {
      staticRadarTuningError = text.staticRadarFactoryResetFailed;
      throw cause;
    } finally {
      staticRadarTuningBusy = false;
    }
  }

  $effect(() => {
    if (activeZoneTool !== "staticRadar") return;
    if (!staticRadarAvailable || !api.setStaticRadarTuningSession || !api.getStaticRadarTuningStatus) {
      staticRadarTuningError = text.staticRadarUnavailable;
      return;
    }

    let disposed = false;
    let pollTimer = 0;
    let keepaliveTimer = 0;
    staticRadarTuningError = "";
    staticRadarTuningStatus = null;

    const poll = async () => {
      if (staticRadarTuningBusy) return;
      try {
        await refreshStaticRadarTuning();
      } catch {
        if (!disposed) staticRadarTuningError = text.staticRadarLoadFailed;
      }
    };

    void api.setStaticRadarTuningSession(true)
      .then(async () => {
        if (disposed) {
          await api.setStaticRadarTuningSession?.(false);
          return;
        }
        await poll();
        pollTimer = window.setInterval(() => void poll(), 1000);
        keepaliveTimer = window.setInterval(
          () => {
            if (!staticRadarTuningBusy) {
              void api.setStaticRadarTuningSession?.(true).catch(() => undefined);
            }
          },
          5000
        );
      })
      .catch(() => {
        if (!disposed) staticRadarTuningError = text.staticRadarLoadFailed;
      });

    return () => {
      disposed = true;
      window.clearInterval(pollTimer);
      window.clearInterval(keepaliveTimer);
      staticRadarTuningStatus = null;
      staticRadarSensitivityDirty = false;
      void api.setStaticRadarTuningSession?.(false).catch(() => undefined);
    };
  });
</script>

<section class={`workspace zone-workspace ${activeZoneTool ? "edit-step" : ""}`}>
  <aside class="side-panel zone-workflow-panel">
    <div class="floorplan-workflow-card zone-summary-card">
      <div>
        <strong>{text.title}</strong>
        <span>{text.description}</span>
      </div>
      <dl class="zone-summary-list">
        <div>
          <dt>{text.status}</dt>
          <dd>{config ? text.loaded : text.loading}</dd>
        </div>
        <div>
          <dt>{text.zones}</dt>
          <dd>{text.count(zones.length)}</dd>
        </div>
        <div>
          <dt>{text.calibration}</dt>
          <dd>{text.count(calibrationZones.length)}</dd>
        </div>
        <div>
          <dt>{text.detection}</dt>
          <dd>{text.count(activeTargetCount)}</dd>
        </div>
      </dl>
    </div>

    <div class="floorplan-stored-tools zone-mode-tools">
      <button
        type="button"
        data-active={activeZoneTool === "zones" ? "true" : "false"}
        onclick={() => onSetActiveZoneTool(activeZoneTool === "zones" ? "" : "zones")}
      >
        {text.zoneSettings}
      </button>
      <button
        type="button"
        data-active={activeZoneTool === "calibration" ? "true" : "false"}
        onclick={() => onSetActiveZoneTool(activeZoneTool === "calibration" ? "" : "calibration")}
      >
        {text.calibrationSettings}
      </button>
      <span
        class="zone-tool-hint"
        title={!staticRadarAvailable || !staticRadarApiAvailable ? text.staticRadarUnavailable : text.staticRadarTuning}
      >
        <button
          type="button"
          data-active={activeZoneTool === "staticRadar" ? "true" : "false"}
          disabled={!staticRadarAvailable || !staticRadarApiAvailable}
          onclick={() => onSetActiveZoneTool(activeZoneTool === "staticRadar" ? "" : "staticRadar")}
        >
          {text.staticRadarTuning}
        </button>
      </span>
      <button
        type="button"
        disabled={saveDisabled}
        onclick={onSaveConfig}
      >
        {messages.common.save}
      </button>
    </div>
  </aside>

  {#if activeZoneTool}
    <aside class="side-panel zone-detail-panel">
      {#if activeZoneTool === "zones"}
        <ZonePanel
          {messages}
          loaded={Boolean(config)}
          {zones}
          {selectedZone}
          {selectedZoneId}
          {zoneTypeLabels}
          onSelectZone={onSelectZone}
          onAddZone={onAddZone}
          onSetZoneName={onSetZoneName}
          onSetZoneType={onSetZoneType}
          onDeleteSelected={onDeleteSelected}
        />
      {:else if activeZoneTool === "calibration"}
        <CalibrationPanel
          {messages}
          canStart={calibrationCanStart}
          running={calibrationRun}
          zones={calibrationZones}
          {selectedZoneId}
          statusText={calibrationStatusText}
          {calibrationTypeLabels}
          onStart={onStartCalibration}
          onStop={onStopCalibration}
          onSelectZone={onSelectZone}
          onSetZoneType={onSetCalibrationZoneType}
          onDeleteZone={onDeleteCalibrationZone}
        />
      {:else}
        <StaticRadarTuningPanel
          {messages}
          status={staticRadarTuningStatus}
          detectedGate={staticRadarDetectedGate}
          selectedGate={selectedStaticRadarGate}
          sensitivity={staticRadarSensitivity}
          sensitivityDirty={staticRadarSensitivityDirty}
          busy={staticRadarTuningBusy}
          error={staticRadarTuningError}
          onSelectGate={selectStaticRadarGate}
          onSetSensitivity={setStaticRadarSensitivity}
          onApply={applyStaticRadarSensitivity}
          onFactoryReset={resetStaticRadarFactoryDefaults}
        />
      {/if}
    </aside>
  {/if}

  <section class="map-panel zone-map-panel">
    <div class="radar-host" data-radar-scene>
      <div class="radar-scene-frame">
        {#if activeZoneTool !== "staticRadar"}
          <MapToolbar
            {messages}
            {canUndo}
            {canRedo}
            {selectedZone}
            hasSelectedCalibrationZone={Boolean(selectedCalibrationZone)}
            {selectedLabel}
            {saveState}
            {saveStatusText}
            {updatedText}
            {debugMode}
            onUndo={onUndo}
            onRedo={onRedo}
            onConvertToRect={onConvertToRect}
            onDeleteSelected={onDeleteSelected}
            onToggleDebug={() => onSetDebugMode(!debugMode)}
          />
        {/if}
        <RadarScene
          {messages}
          state={deviceState}
          config={displayConfig}
          {selectedZoneId}
          editable={activeZoneTool !== "staticRadar"}
          {selectedPointIndex}
          {debugMode}
          staticRadarTuning={activeZoneTool === "staticRadar"}
          {staticRadarTuningStatus}
          {staticRadarDetectedGate}
          {selectedStaticRadarGate}
          onSelectStaticRadarGate={selectStaticRadarGate}
          onCanvasClick={onCanvasClick}
          onZonePointerDown={onZonePointerDown}
          onZoneEdgeClick={onZoneEdgeClick}
          onZonePointDoubleClick={onZonePointDoubleClick}
          onCalibrationInfoClick={onCalibrationInfoClick}
          {onDetectionOwnerRequired}
          {isZoneEditable}
        />
        <p class="map-status-line">{text.mapStatus(activeTargetCount)}</p>
      </div>
    </div>
    {#if debugMode}
      <DebugPanel targets={deviceState?.targets ?? []} />
    {/if}
  </section>
</section>
