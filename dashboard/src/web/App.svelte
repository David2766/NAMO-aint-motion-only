<script lang="ts">
  import { onMount } from "svelte";
  import { deviceApi } from "./api/device-api";
  import { mockApi } from "./api/mock-api";
  import BackupPanel from "./components/BackupPanel.svelte";
  import CalibrationDialog from "./components/CalibrationDialog.svelte";
  import CalibrationPanel from "./components/CalibrationPanel.svelte";
  import DashboardPanel from "./components/DashboardPanel.svelte";
  import DebugPanel from "./components/DebugPanel.svelte";
  import FloorplanPanel from "./components/FloorplanPanel.svelte";
  import MapToolbar from "./components/MapToolbar.svelte";
  import ProtectedZoneDialog from "./components/ProtectedZoneDialog.svelte";
  import RadarScene from "./components/RadarScene.svelte";
  import ShrinkConfirmDialog from "./components/ShrinkConfirmDialog.svelte";
  import StatsPanel from "./components/StatsPanel.svelte";
  import ZonePanel from "./components/ZonePanel.svelte";
  import {
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus,
    saveFloorplanStorage,
    saveFloorplanStorageDocument
  } from "./floorplan/floorplan-storage-client";
  import { MAX_SOFTWARE_ZONES } from "../core/constants";
  import type { BackupFloorplanData } from "../core/config-backup";
  import { calibrationType, isEmptyZone, normalizeSoftwareConfig, zoneDisplayName } from "../core/zones";
  import { createBackupRestore } from "./state/useBackupRestore.svelte";
  import { createCalibrationRun } from "./state/useCalibrationRun.svelte";
  import { createConfigHistory } from "./state/useConfigHistory.svelte";
  import { createConfigSave } from "./state/useConfigSave.svelte";
  import { createRadarInteraction } from "./state/useRadarInteraction.svelte";
  import { createRadarPolling } from "./state/useRadarPolling.svelte";
  import { createZoneEditor } from "./state/useZoneEditor.svelte";
  import type {
    DeviceApi,
    FirmwareUploadProgress,
    WebControlStatus,
    WebDeviceConfig,
    WebDeviceState,
    WebDeviceStats,
    WebSystemStatus,
    WebZone,
    WebZoneType
  } from "./types";

  const searchParams = new URLSearchParams(window.location.search);
  const deviceBaseUrl = searchParams.get("device")?.trim() || "";
  const useMockApi = searchParams.get("mock") === "1" || (!deviceBaseUrl && window.location.hostname === "localhost");
  const api: DeviceApi = useMockApi ? mockApi : deviceApi;

  const zoneTypeLabels: Record<WebZoneType, string> = {
    detection: "탐지",
    filter: "필터",
    reduced: "둔감",
    disabled: "제외"
  };

  const calibrationTypeLabels: Record<Extract<WebZoneType, "filter" | "reduced" | "disabled">, string> = {
    filter: "필터",
    reduced: "둔감",
    disabled: "제외"
  };

  let state = $state<WebDeviceState | null>(null);
  let config = $state<WebDeviceConfig | null>(null);
  let stats = $state<WebDeviceStats | null>(null);
  let statsLoading = $state(false);
  let statsError = $state("");
  let systemStatus = $state<WebSystemStatus | null>(null);
  let systemStatusLoading = $state(false);
  let systemStatusLoaded = $state(false);
  let systemStatusError = $state("");
  let controlStatus = $state<WebControlStatus | null>(null);
  let controlStatusLoading = $state(false);
  let controlStatusLoaded = $state(false);
  let controlStatusError = $state("");
  let controlActionBusy = $state(false);
  let selectedPointIndex = $state(-1);
  let statusText = $state("연결 대기");
  let statusTone = $state<"ok" | "warn" | "error">("warn");
  let errorText = $state("");
  let debugMode = $state(false);
  let activeTab = $state<"dashboard" | "zones" | "floorplan" | "stats" | "backup">("dashboard");
  let activeZoneTool = $state<"" | "zones" | "calibration">("");

  const configSave = createConfigSave({
    api,
    getConfig: () => config,
    setStatus,
    errorMessage
  });

  const backupRestore = createBackupRestore({
    getConfig: () => config,
    applyConfig: applyImportedConfig,
    loadFloorplanBackup,
    applyFloorplanBackup,
    loadStatsBackup,
    applyStatsBackup,
    getDeviceInfo: () => ({
      sourceUrl: deviceBaseUrl || window.location.origin,
      name: "Radar Zone Configurator"
    }),
    setStatus,
    errorMessage
  });

  const configHistory = createConfigHistory({
    getConfig: () => config,
    setConfig: (nextConfig) => {
      config = nextConfig;
    },
    onRestore: (nextConfig) => {
      selectZone(nextConfig.zones[0]?.id || nextConfig.calibrationZones?.[0]?.id || "");
      configSave.scheduleSave();
      renderSceneNow();
    }
  });

  const calibration = createCalibrationRun({
    getConfig: () => config,
    getState: () => state,
    getCalibrationZoneCount: () => calibrationZones.length,
    updateConfig: (mutator) => updateConfig(mutator),
    selectZone: (zoneId) => selectZone(zoneId),
    setStatus
  });

  const radarPolling = createRadarPolling({
    loadConfig,
    refreshState,
    refreshStats
  });

  const zones = $derived(config?.zones.filter((zone) => !isEmptyZone(zone)).slice(0, MAX_SOFTWARE_ZONES) ?? []);
  const calibrationZones = $derived(config?.calibrationZones ?? []);

  const zoneEditor = createZoneEditor({
    getConfig: () => config,
    getZones: () => zones,
    getCalibrationZones: () => calibrationZones,
    updateConfig: (mutator) => updateConfig(mutator),
    setStatus,
    onSelect: (_zoneId, resetPoint, render) => {
      radarInteraction.resetShrinkWarning();
      if (resetPoint) selectedPointIndex = -1;
      if (render) renderSceneNow();
    }
  });

  const selectedZoneId = $derived(zoneEditor.selectedZoneId);
  const selectedZone = $derived(zoneEditor.selectedZone);
  const selectedCalibrationZone = $derived(zoneEditor.selectedCalibrationZone);

  const radarInteraction = createRadarInteraction({
    getConfig: () => config,
    getZones: () => zones,
    getCalibrationZones: () => calibrationZones,
    getSelectedZone: () => selectedZone,
    getSelectedPointIndex: () => selectedPointIndex,
    setSelectedPointIndex: (pointIndex) => {
      selectedPointIndex = pointIndex;
    },
    updateConfig,
    pushHistory: configHistory.pushHistory,
    scheduleSave: configSave.scheduleSave,
    selectZone,
    renderSceneNow,
    setStatus
  });

  const updatedText = $derived(state ? new Date(state.updatedAt).toLocaleTimeString() : "-");
  const activeTargetCount = $derived(state?.targets.filter((target) => target.active).length ?? 0);
  const selectedLabel = $derived(
    selectedZone
      ? `${zoneDisplayName(selectedZone)} · ${zoneTypeLabels[selectedZone.type]}`
      : selectedCalibrationZone
        ? `${zoneDisplayName(selectedCalibrationZone)} · 보정 구역 · ${calibrationLabel(selectedCalibrationZone)}`
        : "선택 없음"
  );

  onMount(() => {
    radarPolling.start();

    window.addEventListener("pointermove", radarInteraction.handlePointerMove);
    window.addEventListener("pointerup", radarInteraction.handlePointerUp);
    window.addEventListener("pointercancel", radarInteraction.handlePointerUp);
    window.addEventListener("keydown", handleKeyDown);
    document.addEventListener("pointerdown", handleDocumentPointerDown, true);

    return () => {
      radarPolling.destroy();
      configSave.destroy();
      window.removeEventListener("pointermove", radarInteraction.handlePointerMove);
      window.removeEventListener("pointerup", radarInteraction.handlePointerUp);
      window.removeEventListener("pointercancel", radarInteraction.handlePointerUp);
      window.removeEventListener("keydown", handleKeyDown);
      document.removeEventListener("pointerdown", handleDocumentPointerDown, true);
    };
  });

  $effect(() => {
    if ((activeTab === "dashboard" || activeTab === "backup") && !systemStatusLoaded && !systemStatusLoading) {
      void refreshSystemStatus();
    }
  });

  $effect(() => {
    if (activeTab === "dashboard" && !controlStatusLoaded && !controlStatusLoading) {
      void refreshControlStatus();
    }
  });

  async function loadConfig(): Promise<void> {
    try {
      config = normalizeSoftwareConfig(await api.getConfig());
      zoneEditor.selectFirstAvailable();
      selectedPointIndex = -1;
      setStatus("설정 로드 완료", "ok");
    } catch (error) {
      setStatus(`설정을 읽지 못했습니다. ${errorMessage(error)}`, "error");
    }
  }

  async function refreshState(): Promise<void> {
    try {
      state = await api.getState();
      calibration.update();
      renderSceneNow();
      setStatus(state.connected ? "연결됨" : "연결 대기", state.connected ? "ok" : "warn");
    } catch (error) {
      setStatus(`상태를 읽지 못했습니다. ${errorMessage(error)}`, "error");
    }
  }

  async function refreshStats(): Promise<void> {
    statsLoading = true;
    try {
      stats = await api.getStats();
      statsError = "";
    } catch (error) {
      statsError = errorMessage(error);
      setStatus(`통계를 읽지 못했습니다: ${statsError}`, "error");
    } finally {
      statsLoading = false;
    }
  }

  async function refreshSystemStatus(): Promise<void> {
    if (!api.getSystemStatus) {
      systemStatusLoaded = true;
      systemStatusError = "시스템 정보 API가 준비되지 않았습니다.";
      return;
    }
    systemStatusLoading = true;
    try {
      systemStatus = await api.getSystemStatus();
      systemStatusLoaded = true;
      systemStatusError = "";
    } catch (error) {
      systemStatusLoaded = true;
      systemStatusError = errorMessage(error);
    } finally {
      systemStatusLoading = false;
    }
  }

  async function refreshControlStatus(): Promise<void> {
    if (!api.getControlStatus) {
      controlStatusLoaded = true;
      controlStatusError = "제어 API가 준비되지 않았습니다.";
      return;
    }
    controlStatusLoading = true;
    try {
      controlStatus = await api.getControlStatus();
      controlStatusLoaded = true;
      controlStatusError = "";
    } catch (error) {
      controlStatusLoaded = true;
      controlStatusError = errorMessage(error);
    } finally {
      controlStatusLoading = false;
    }
  }

  async function setStatusLed(enabled: boolean): Promise<void> {
    if (!api.setStatusLed) {
      controlStatusError = "Status LED 제어 API가 준비되지 않았습니다.";
      return;
    }
    controlActionBusy = true;
    try {
      await api.setStatusLed(enabled);
      await refreshControlStatus();
      setStatus(enabled ? "Status LED를 켰습니다." : "Status LED를 껐습니다.", "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(`Status LED 제어 실패: ${controlStatusError}`, "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setLedBlinkDuration(seconds: number): Promise<void> {
    if (!api.setLedBlinkDuration) {
      controlStatusError = "LED 시간 제어 API가 준비되지 않았습니다.";
      return;
    }
    controlActionBusy = true;
    try {
      await api.setLedBlinkDuration(seconds);
      await refreshControlStatus();
      setStatus("LED 자동 꺼짐 시간을 변경했습니다.", "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(`LED 시간 변경 실패: ${controlStatusError}`, "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setEnvironmentCorrection(enabled: boolean): Promise<void> {
    if (!api.setEnvironmentCorrection) {
      controlStatusError = "온습도 보정 API가 준비되지 않았습니다.";
      return;
    }
    controlActionBusy = true;
    try {
      await api.setEnvironmentCorrection(enabled);
      await refreshControlStatus();
      setStatus(enabled ? "온습도 보정을 켰습니다." : "온습도 보정을 껐습니다.", "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(`온습도 보정 변경 실패: ${controlStatusError}`, "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setTemperatureOffset(value: number): Promise<void> {
    if (!api.setTemperatureOffset) {
      controlStatusError = "온도 보정 API가 준비되지 않았습니다.";
      return;
    }
    controlActionBusy = true;
    try {
      await api.setTemperatureOffset(value);
      await refreshControlStatus();
      setStatus("온도 보정값을 변경했습니다.", "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(`온도 보정값 변경 실패: ${controlStatusError}`, "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setHumidityOffset(value: number): Promise<void> {
    if (!api.setHumidityOffset) {
      controlStatusError = "습도 보정 API가 준비되지 않았습니다.";
      return;
    }
    controlActionBusy = true;
    try {
      await api.setHumidityOffset(value);
      await refreshControlStatus();
      setStatus("습도 보정값을 변경했습니다.", "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(`습도 보정값 변경 실패: ${controlStatusError}`, "error");
    } finally {
      controlActionBusy = false;
    }
  }

  function displayConfig(): WebDeviceConfig {
    return config ? { ...config, zones, calibrationZones } : { version: 1, zones: [], calibrationZones: [] };
  }

  function setStatus(message: string, tone: "ok" | "warn" | "error"): void {
    statusText = message;
    statusTone = tone;
    if (tone === "error") {
      errorText = message;
      window.setTimeout(() => {
        if (errorText === message) errorText = "";
      }, 5000);
    }
  }

  function errorMessage(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
  }

  function calibrationLabel(zone: WebZone): string {
    return calibrationTypeLabels[calibrationType(zone.type)];
  }

  function selectZone(zoneId: string, resetPoint = true, render = true): void {
    zoneEditor.selectZone(zoneId, resetPoint, render);
    if (zoneId) {
      activeZoneTool = calibrationZones.some((zone) => zone.id === zoneId) ? "calibration" : "zones";
    }
  }

  function renderSceneNow(): void {
    // RadarScene.svelte is state-driven; keeping this hook preserves call sites while avoiding DOM replacement.
  }

  function updateConfig(mutator: (current: WebDeviceConfig) => WebDeviceConfig, save = true, history = save): void {
    if (!config) return;
    if (history) configHistory.pushHistory();
    config = normalizeSoftwareConfig(mutator(config));
    if (save) configSave.scheduleSave();
  }

  async function applyImportedConfig(nextConfig: WebDeviceConfig): Promise<void> {
    if (config) configHistory.pushHistory();
    const currentFloorplan = config?.floorplan;
    config = normalizeSoftwareConfig({
      ...nextConfig,
      floorplan: currentFloorplan ?? nextConfig.floorplan
    });
    zoneEditor.selectFirstAvailable();
    selectedPointIndex = -1;
    renderSceneNow();
    await configSave.saveConfigNow();
  }

  function addZone(): void {
    activeZoneTool = "zones";
    zoneEditor.addZone();
  }

  function deleteSelected(): void {
    zoneEditor.deleteSelected();
  }

  function setSelectedZoneType(type: WebZoneType): void {
    zoneEditor.setSelectedZoneType(type);
  }

  function setSelectedZoneName(name: string): void {
    zoneEditor.setSelectedZoneName(name);
  }

  function setCalibrationZoneType(zoneId: string, type: Extract<WebZoneType, "filter" | "reduced" | "disabled">): void {
    updateConfig((current) => ({
      ...current,
      calibrationZones: (current.calibrationZones || []).map((zone) => (zone.id === zoneId ? { ...zone, type } : zone))
    }));
    calibration.clearResult();
  }

  function handleKeyDown(event: KeyboardEvent): void {
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    const tag = document.activeElement?.tagName.toLowerCase();
    if (tag === "input" || tag === "textarea" || tag === "select") return;
    event.preventDefault();
    if (selectedPointIndex >= 0) {
      radarInteraction.deleteSelectedPoint();
      return;
    }
    deleteSelected();
  }

  function handleDocumentPointerDown(event: PointerEvent): void {
    if (!selectedZoneId && selectedPointIndex < 0) return;
    const target = event.target as Element | null;
    if (!target) return;
    if (isSelectionPreservingTarget(target)) return;
    selectZone("");
  }

  function isSelectionPreservingTarget(target: Element): boolean {
    return Boolean(
      target.closest(
        [
          "button",
          "input",
          "textarea",
          "select",
          "option",
          "a",
          "[data-zone-id]",
          "[data-calibration-select]",
          "[data-zone-drag]",
          "[data-zone-edge]",
          "[data-zone-point]",
          "[data-zone-select]",
          "[data-calibration-info]",
          "[data-calibration-dialog]",
          "[data-protected-zone-dialog]",
          "[data-shrink-confirm-dialog]",
          ".target"
        ].join(", ")
      )
    );
  }

  async function loadFloorplanBackup(): Promise<BackupFloorplanData | null> {
    let status;
    try {
      status = await loadFloorplanStorageStatus({ baseUrl: deviceBaseUrl });
    } catch {
      return null;
    }
    if (!status.hasConfig) return null;

    const document = await loadFloorplanStorageDocument({ baseUrl: deviceBaseUrl });
    if (!status.hasImage) return { document };

    const image = await loadFloorplanStorageImage({ baseUrl: deviceBaseUrl });
    return {
      document,
      image: {
        name: document.image.name,
        mime: image.type || document.image.mime || "image/webp",
        bytes: image.size,
        dataBase64: await blobToBase64(image)
      }
    };
  }

  async function applyFloorplanBackup(floorplan: BackupFloorplanData): Promise<void> {
    if (floorplan.image) {
      await saveFloorplanStorage(
        {
          document: floorplan.document,
          image: base64ToBlob(floorplan.image.dataBase64, floorplan.image.mime)
        },
        { baseUrl: deviceBaseUrl }
      );
      await markFloorplanRestored(true);
      return;
    }
    await saveFloorplanStorageDocument(floorplan.document, { baseUrl: deviceBaseUrl });
    await markFloorplanRestored(config?.floorplan?.hasImage === true);
  }

  async function markFloorplanRestored(hasImage: boolean): Promise<void> {
    if (!config) return;
    config = normalizeSoftwareConfig({
      ...config,
      floorplan: {
        ...(config.floorplan ?? {}),
        enabled: true,
        hasImage
      }
    });
    await configSave.saveConfigNow();
  }

  async function loadStatsBackup(): Promise<WebDeviceStats | null> {
    try {
      return stats ?? await api.getStats();
    } catch {
      return null;
    }
  }

  async function applyStatsBackup(
    nextStats: WebDeviceStats,
    onProgress: ((progress: FirmwareUploadProgress) => void) | undefined = undefined
  ): Promise<void> {
    await api.saveStats(nextStats, onProgress);
    stats = nextStats;
  }

  async function blobToBase64(blob: Blob): Promise<string> {
    const bytes = new Uint8Array(await blob.arrayBuffer());
    let binary = "";
    for (const byte of bytes) {
      binary += String.fromCharCode(byte);
    }
    return btoa(binary);
  }

  function base64ToBlob(dataBase64: string, mime: string): Blob {
    const binary = atob(dataBase64);
    const bytes = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; index += 1) {
      bytes[index] = binary.charCodeAt(index);
    }
    return new Blob([bytes], { type: mime });
  }
</script>

<main class="app-shell">
  <header class="top-bar">
    <div>
      <h1>Radar Zone Configurator</h1>
      <p>{useMockApi ? "Mock 데이터로 확인 중입니다." : "실시간 위치와 구역 설정을 한 화면에서 확인합니다."}</p>
    </div>
    <div class="status-pill" data-status data-tone={statusTone}>{statusText}</div>
  </header>
  <div class="toast" data-toast data-visible={errorText ? "true" : "false"}>{errorText}</div>

  <nav class="app-tabs" aria-label="Radar configurator sections">
    <button class:active={activeTab === "dashboard"} type="button" onclick={() => (activeTab = "dashboard")}>대시보드</button>
    <button class:active={activeTab === "zones"} type="button" onclick={() => (activeTab = "zones")}>구역 설정</button>
    <button class:active={activeTab === "floorplan"} type="button" onclick={() => (activeTab = "floorplan")}>평면도</button>
    <button class:active={activeTab === "stats"} type="button" onclick={() => (activeTab = "stats")}>감지 통계</button>
    <button class:active={activeTab === "backup"} type="button" onclick={() => (activeTab = "backup")}>관리 / 백업</button>
  </nav>

  {#if activeTab === "dashboard"}
    <section class="tab-page">
      <DashboardPanel
        {config}
        {state}
        {systemStatus}
        {systemStatusLoading}
        {systemStatusError}
        {controlStatus}
        {controlStatusLoading}
        {controlStatusError}
        {controlActionBusy}
        {updatedText}
        floorplanStorageBaseUrl={deviceBaseUrl}
        onNavigate={(tab) => (activeTab = tab)}
        onSetStatusLed={setStatusLed}
        onSetLedBlinkDuration={setLedBlinkDuration}
        onSetEnvironmentCorrection={setEnvironmentCorrection}
        onSetTemperatureOffset={setTemperatureOffset}
        onSetHumidityOffset={setHumidityOffset}
      />
    </section>
  {:else if activeTab === "zones"}
  <section class={`workspace zone-workspace ${activeZoneTool ? "edit-step" : ""}`}>
    <aside class="side-panel zone-workflow-panel">
      <div class="floorplan-workflow-card zone-summary-card">
        <div>
          <strong>구역 설정</strong>
          <span>레이더맵 위에서 감지 구역과 오탐 보정 구역을 편집합니다.</span>
        </div>
        <dl class="zone-summary-list">
          <div>
            <dt>상태</dt>
            <dd>{config ? "데이터 확인됨" : "로딩 중"}</dd>
          </div>
          <div>
            <dt>구역</dt>
            <dd>{zones.length}개</dd>
          </div>
          <div>
            <dt>오탐 보정</dt>
            <dd>{calibrationZones.length}개</dd>
          </div>
          <div>
            <dt>감지</dt>
            <dd>{activeTargetCount}개</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-stored-tools zone-mode-tools">
        <button
          type="button"
          data-active={activeZoneTool === "zones" ? "true" : "false"}
          onclick={() => (activeZoneTool = activeZoneTool === "zones" ? "" : "zones")}
        >
          구역 설정
        </button>
        <button
          type="button"
          data-active={activeZoneTool === "calibration" ? "true" : "false"}
          onclick={() => (activeZoneTool = activeZoneTool === "calibration" ? "" : "calibration")}
        >
          오탐 보정
        </button>
      </div>
    </aside>

    {#if activeZoneTool}
      <aside class="side-panel zone-detail-panel">
        {#if activeZoneTool === "zones"}
          <ZonePanel
            loaded={Boolean(config)}
            {zones}
            {selectedZone}
            {selectedZoneId}
            {zoneTypeLabels}
            onSelectZone={selectZone}
            onAddZone={addZone}
            onSetZoneName={setSelectedZoneName}
            onSetZoneType={setSelectedZoneType}
            onDeleteSelected={deleteSelected}
          />
        {:else}
          <CalibrationPanel
            loaded={Boolean(config)}
            hasState={Boolean(state)}
            pirMotion={Boolean(state?.pirMotion)}
            running={Boolean(calibration.run)}
            zones={calibrationZones}
            {selectedZoneId}
            statusText={calibration.statusText()}
            {calibrationTypeLabels}
            onStart={calibration.start}
            onStop={() => calibration.stop("사용자가 보정을 중지했습니다.", "warn")}
            onSelectZone={selectZone}
            onSetZoneType={setCalibrationZoneType}
            onDeleteZone={(zoneId) => {
              selectZone(zoneId);
              deleteSelected();
            }}
          />
        {/if}
      </aside>
    {/if}

    <section class="map-panel zone-map-panel">
      <div class="radar-host" data-radar-scene>
        <div class="radar-scene-frame">
          <MapToolbar
            canUndo={configHistory.canUndo}
            canRedo={configHistory.canRedo}
            {selectedZone}
            hasSelectedCalibrationZone={Boolean(selectedCalibrationZone)}
            {selectedLabel}
            saveState={configSave.saveState}
            saveStatusText={configSave.saveStatusText}
            {updatedText}
            {debugMode}
            onUndo={configHistory.undo}
            onRedo={configHistory.redo}
            onConvertToRect={radarInteraction.convertSelectedZoneToRect}
            onDeleteSelected={deleteSelected}
            onToggleDebug={() => (debugMode = !debugMode)}
          />
          <RadarScene
            {state}
            config={config ? displayConfig() : null}
            {selectedZoneId}
            editable
            {selectedPointIndex}
            {debugMode}
            onCanvasClick={radarInteraction.handleRadarClick}
            onZonePointerDown={radarInteraction.handleRadarPointerDown}
            onZoneEdgeClick={radarInteraction.handleRadarEdgeClick}
            onZonePointDoubleClick={radarInteraction.handlePointDoubleClick}
            onCalibrationInfoClick={(zoneId) => {
              selectZone(zoneId);
              radarInteraction.protectedZoneDialogOpen = true;
            }}
          />
          <p class="map-status-line">타깃 {activeTargetCount}개 감지 중</p>
        </div>
      </div>
      {#if debugMode}
        <DebugPanel targets={state?.targets ?? []} />
      {/if}
    </section>
  </section>
  {:else if activeTab === "floorplan"}
    <section class="tab-page">
      <FloorplanPanel
        deviceConfig={displayConfig()}
        deviceState={state}
        floorplanStorageBaseUrl={deviceBaseUrl}
        onUpdateDeviceConfig={(mutator) => updateConfig(mutator)}
        onSaveFloorplan={(document, image) => api.saveFloorplan?.(document, image) ?? Promise.reject(new Error("평면도 저장 API가 준비되지 않았습니다."))}
      />
    </section>
  {:else if activeTab === "stats"}
    <section class="tab-page">
      <StatsPanel {stats} error={statsError} loading={statsLoading} onRefresh={refreshStats} />
    </section>
  {:else}
    <section class="tab-page">
      <BackupPanel
        loaded={Boolean(config)}
        stage={backupRestore.stage}
        filename={backupRestore.filename}
        message={backupRestore.message}
        errors={backupRestore.errors}
        warnings={backupRestore.warnings}
        summary={backupRestore.summary}
        progressSteps={backupRestore.progressSteps}
        progressPercent={backupRestore.progressPercent}
        importZones={backupRestore.importZones}
        importFloorplan={backupRestore.importFloorplan}
        importStats={backupRestore.importStats}
        canImportFloorplan={backupRestore.canImportFloorplan}
        canImportStats={backupRestore.canImportStats}
        canConfirmImport={backupRestore.canConfirmImport}
        {systemStatus}
        systemStatusLoading={systemStatusLoading}
        systemStatusError={systemStatusError}
        issueText={backupRestore.issueText}
        onExport={backupRestore.exportBackup}
        onImportFile={backupRestore.readImportFile}
        onSetImportZones={backupRestore.setImportZones}
        onSetImportFloorplan={backupRestore.setImportFloorplan}
        onSetImportStats={backupRestore.setImportStats}
        onConfirmImport={backupRestore.confirmImport}
        onCancelImport={backupRestore.cancelImport}
        onUploadFirmware={api.uploadFirmware}
      />
    </section>
  {/if}
</main>

<CalibrationDialog
  open={calibration.dialogOpen}
  running={Boolean(calibration.run)}
  result={calibration.result}
  metrics={calibration.metrics}
  progress={calibration.progress}
  progressText={calibration.progressText(calibration.metrics)}
  workItems={calibration.workItems(calibration.metrics)}
  logs={calibration.dialogLogs}
  metricsLines={calibration.result?.metrics ? calibration.metricsLines(calibration.result.metrics) : []}
  onClose={() => (calibration.dialogOpen = false)}
  onStop={() => calibration.stop("사용자가 보정을 중지했습니다.", "warn")}
/>

<ProtectedZoneDialog
  open={radarInteraction.protectedZoneDialogOpen}
  onClose={() => (radarInteraction.protectedZoneDialogOpen = false)}
/>

<ShrinkConfirmDialog
  zoneId={radarInteraction.shrinkConfirmZoneId}
  onCancel={radarInteraction.cancelCalibrationShrink}
  onConfirm={radarInteraction.unlockCalibrationMinSize}
/>
