<script lang="ts">
  import { onMount } from "svelte";
  import { apiErrorMessage } from "./api/api-message";
  import { deviceApi } from "./api/device-api";
  import { deviceStorageQueue } from "./api/device-storage-queue";
  import CalibrationDialog from "./components/CalibrationDialog.svelte";
  import ProtectedZoneDialog from "./components/ProtectedZoneDialog.svelte";
  import ShrinkConfirmDialog from "./components/ShrinkConfirmDialog.svelte";
  import {
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus,
    saveFloorplanStorage,
    saveFloorplanStorageDocument
  } from "./floorplan/floorplan-storage-client";
  import { buildFloorplanRoomContext } from "./floorplan/floorplan-room-context";
  import { MAX_SOFTWARE_ZONES } from "../core/constants";
  import type { BackupFloorplanData } from "../core/config-backup";
  import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
  import {
    calibrationType,
    isEmptyZone,
    localizedZoneDisplayName,
    normalizeSoftwareConfig
  } from "../core/zones";
  import { createBackupRestore } from "./state/useBackupRestore.svelte";
  import { createCalibrationRun } from "./state/useCalibrationRun.svelte";
  import { createConfigHistory } from "./state/useConfigHistory.svelte";
  import { createConfigSave } from "./state/useConfigSave.svelte";
  import { createRadarInteraction } from "./state/useRadarInteraction.svelte";
  import { createRadarPolling } from "./state/useRadarPolling.svelte";
  import { createZoneEditor } from "./state/useZoneEditor.svelte";
  import { APP_SELECTION_PRESERVE_SELECTOR } from "./dom/selection-preserve";
  import { languageOptions } from "./i18n";
  import { createI18n } from "./i18n/useI18n.svelte";
  import BackupPage from "./pages/BackupPage.svelte";
  import DashboardPage from "./pages/DashboardPage.svelte";
  import FloorplanPage from "./pages/FloorplanPage.svelte";
  import StatsPage from "./pages/StatsPage.svelte";
  import ZonesPage from "./pages/ZonesPage.svelte";
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

  type AppTab = "dashboard" | "zones" | "floorplan" | "stats" | "backup";

  type AppProps = {
    api?: DeviceApi;
    demoMode?: boolean;
    mockMode?: boolean;
    deviceBaseUrl?: string;
    floorplanStorageFetcher?: typeof fetch;
    onResetDemoStorage?: () => void;
    haSetupHandoffRequired?: boolean;
  };

  const searchParams = new URLSearchParams(window.location.search);
  const defaultDeviceBaseUrl = searchParams.get("device")?.trim() || "";
  let {
    api = deviceApi,
    demoMode = false,
    mockMode = false,
    deviceBaseUrl = defaultDeviceBaseUrl,
    floorplanStorageFetcher,
    onResetDemoStorage,
    haSetupHandoffRequired = searchParams.get("setup") === "1"
  }: AppProps = $props();

  function requiresHaSetupHandoff(): boolean {
    return haSetupHandoffRequired && !mockMode;
  }

  const i18n = createI18n();
  type IntegrationMode = "unknown" | "edge" | "ha";
  type HaSetupGateMode = "select" | "edge" | "ha-setup" | "api-warmup" | "api-warning";

  const zoneTypeLabels = $derived<Record<WebZoneType, string>>({
    detection: i18n.msg.zones.typeLabels.detection,
    filter: i18n.msg.zones.typeLabels.filter,
    reduced: i18n.msg.zones.typeLabels.reduced,
    disabled: i18n.msg.zones.typeLabels.disabled,
    exit: i18n.msg.zones.typeLabels.exit
  });

  const calibrationTypeLabels = $derived<Record<Extract<WebZoneType, "filter" | "reduced" | "disabled">, string>>({
    filter: i18n.msg.zones.typeLabels.filter,
    reduced: i18n.msg.zones.typeLabels.reduced,
    disabled: i18n.msg.zones.typeLabels.disabled
  });

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
  let integrationModeActionBusy = $state(false);
  let selectedPointIndex = $state(-1);
  let statusText = $state(i18n.msg.app.statusWaiting);
  let statusTone = $state<"ok" | "warn" | "error">("warn");
  let errorText = $state("");
  let debugMode = $state(false);
  let activeTab = $state<AppTab>("dashboard");
  let floorplanHasUnsavedChanges = $state(false);
  let languageMenuOpen = $state(false);
  let activeZoneTool = $state<"" | "zones" | "calibration" | "staticRadar">("");
  let haSetupGateVisible = $state(requiresHaSetupHandoff());
  let haSetupGateMode = $state<HaSetupGateMode>(requiresHaSetupHandoff() ? "select" : "api-warmup");
  let haSetupGateBusy = $state(false);
  let haSetupGateMessage = $state(requiresHaSetupHandoff() ? i18n.msg.app.setupModeSelectMessage : i18n.msg.app.setupInitialChecking);
  let haSetupGateDismissed = $state(false);
  let haSetupGateAllowContinue = $state(false);

  function createAppConfigSave() {
    return createConfigSave({
      api,
      getConfig: () => config,
      setStatus,
      errorMessage,
      getSaveStatusLabels: () => i18n.msg.common.saveStatus,
      savedMessage: () => i18n.msg.app.configSaved,
      saveFailedMessage: (error) => i18n.msg.app.configSaveFailed(error)
    });
  }

  const configSave = createAppConfigSave();

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
    getMessages: () => i18n.msg,
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
      configSave.markPending();
      renderSceneNow();
    }
  });

  const calibration = createCalibrationRun({
    getConfig: () => config,
    getState: () => state,
    getCalibrationZoneCount: () => calibrationZones.length,
    getMessages: () => i18n.msg,
    updateConfig: (mutator) => updateZoneDraftConfig(mutator),
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
    getMessages: () => i18n.msg,
    updateConfig: (mutator) => updateZoneDraftConfig(mutator),
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
    getMessages: () => i18n.msg,
    setSelectedPointIndex: (pointIndex) => {
      selectedPointIndex = pointIndex;
    },
    updateConfig: updateZoneDraftConfig,
    pushHistory: configHistory.pushHistory,
    scheduleSave: configSave.markPending,
    selectZone,
    renderSceneNow,
    setStatus
  });

  const updatedText = $derived(state ? formatClock(state.updatedAt) : "-");
  const activeTargetCount = $derived(state?.targets.filter((target) => target.active).length ?? 0);
  let appRuntimeStarted = false;

  function formatClock(timestamp: number): string {
    return new Intl.DateTimeFormat(i18n.msg.language.code === "ko" ? "ko-KR" : "en-US", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hour12: false
    }).format(new Date(timestamp));
  }

  function displayZoneName(zone: WebZone): string {
    return localizedZoneDisplayName(zone, i18n.msg.zones);
  }

  const selectedLabel = $derived(
    selectedZone
      ? `${displayZoneName(selectedZone)} · ${zoneTypeLabels[selectedZone.type]}`
      : selectedCalibrationZone
        ? `${displayZoneName(selectedCalibrationZone)} · ${i18n.msg.zones.calibrationZone} · ${calibrationLabel(selectedCalibrationZone)}`
        : i18n.msg.zones.noneSelected
  );

  function hasUnsavedChanges(): boolean {
    return configSave.hasPendingChanges || floorplanHasUnsavedChanges;
  }

  function handleBeforeUnload(event: BeforeUnloadEvent): void {
    if (!hasUnsavedChanges()) return;
    event.preventDefault();
    event.returnValue = "";
  }

  function setActiveTab(tab: AppTab): void {
    if (tab === activeTab) return;
    if (hasUnsavedChanges() && !window.confirm(i18n.msg.common.unsavedChangesConfirm)) return;
    activeTab = tab;
  }

  function setFloorplanUnsavedChanges(dirty: boolean): void {
    floorplanHasUnsavedChanges = dirty;
  }

  function startAppRuntime(): void {
    if (appRuntimeStarted) return;
    appRuntimeStarted = true;
    radarPolling.start();

    window.addEventListener("pointermove", radarInteraction.handlePointerMove);
    window.addEventListener("pointerup", radarInteraction.handlePointerUp);
    window.addEventListener("pointercancel", radarInteraction.handlePointerUp);
    window.addEventListener("keydown", handleKeyDown);
    window.addEventListener("beforeunload", handleBeforeUnload);
    document.addEventListener("pointerdown", handleDocumentPointerDown, true);
  }

  function stopAppRuntime(): void {
    if (!appRuntimeStarted) return;
    appRuntimeStarted = false;
    radarPolling.destroy();
    configSave.destroy();
    window.removeEventListener("pointermove", radarInteraction.handlePointerMove);
    window.removeEventListener("pointerup", radarInteraction.handlePointerUp);
    window.removeEventListener("pointercancel", radarInteraction.handlePointerUp);
    window.removeEventListener("keydown", handleKeyDown);
    window.removeEventListener("beforeunload", handleBeforeUnload);
    document.removeEventListener("pointerdown", handleDocumentPointerDown, true);
  }

  onMount(() => {
    startAppRuntime();
    return stopAppRuntime;
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
      setStatus(i18n.msg.app.configLoaded, "ok");
    } catch (error) {
      setStatus(i18n.msg.app.configReadFailed(errorMessage(error)), "error");
    }
  }

  async function refreshState(): Promise<void> {
    try {
      state = await api.getState();
      calibration.update();
      renderSceneNow();
      setStatus(state.connected ? i18n.msg.app.stateConnected : i18n.msg.app.statusWaiting, state.connected ? "ok" : "warn");
    } catch (error) {
      setStatus(i18n.msg.app.stateReadFailed(errorMessage(error)), "error");
    }
  }

  async function refreshStats(): Promise<void> {
    statsLoading = true;
    try {
      stats = await api.getStats();
      statsError = "";
    } catch (error) {
      statsError = errorMessage(error);
      setStatus(i18n.msg.app.statsReadFailed(statsError), "error");
    } finally {
      statsLoading = false;
    }
  }

  async function refreshSystemStatus(): Promise<void> {
    if (!api.getSystemStatus) {
      systemStatusLoaded = true;
      systemStatusError = i18n.msg.app.systemStatusApiNotReady;
      return;
    }
    systemStatusLoading = true;
    try {
      const nextStatus = await api.getSystemStatus();
      systemStatus = nextStatus;
      systemStatusLoaded = true;
      systemStatusError = "";
      updateHaSetupGateFromStatus(nextStatus);
    } catch (error) {
      systemStatusLoaded = true;
      systemStatusError = errorMessage(error);
    } finally {
      systemStatusLoading = false;
    }
  }

  function wait(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  async function waitForHaApiReady(maxWaitMs = 30000): Promise<"ready" | "timeout"> {
    if (!api.getSystemStatus) return "timeout";

    const startedAt = Date.now();
    let attempt = 0;
    while (Date.now() - startedAt < maxWaitMs) {
      attempt += 1;
      try {
        const nextStatus = await api.getSystemStatus();
        systemStatus = nextStatus;
        systemStatusLoaded = true;
        systemStatusError = "";

        if (nextStatus.api?.connected) {
          return "ready";
        }

        const elapsedSeconds = Math.max(1, Math.round((Date.now() - startedAt) / 1000));
        haSetupGateMessage =
          elapsedSeconds < 8
            ? i18n.msg.app.haApiWarmupChecking
            : i18n.msg.app.haApiWaitingElapsed(elapsedSeconds);
      } catch (error) {
        systemStatusLoaded = true;
        systemStatusError = errorMessage(error);
        haSetupGateMessage =
          attempt < 4 ? i18n.msg.app.deviceNetworkPreparing : i18n.msg.app.deviceWaitingWifiSwitch;
      }

      await wait(1000);
    }

    return "timeout";
  }

  async function refreshControlStatus(): Promise<void> {
    if (!api.getControlStatus) {
      controlStatusLoaded = true;
      controlStatusError = i18n.msg.app.controlApiNotReady;
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
      controlStatusError = i18n.msg.app.statusLedApiNotReady;
      return;
    }
    controlActionBusy = true;
    try {
      await api.setStatusLed(enabled);
      await refreshControlStatus();
      setStatus(enabled ? i18n.msg.app.statusLedOn : i18n.msg.app.statusLedOff, "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.statusLedFailed(controlStatusError), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setLedBlinkDuration(seconds: number): Promise<void> {
    if (!api.setLedBlinkDuration) {
      controlStatusError = i18n.msg.app.ledDurationApiNotReady;
      return;
    }
    controlActionBusy = true;
    try {
      await api.setLedBlinkDuration(seconds);
      await refreshControlStatus();
      setStatus(i18n.msg.app.ledDurationChanged, "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.ledDurationFailed(controlStatusError), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setEnvironmentCorrection(enabled: boolean): Promise<void> {
    if (!api.setEnvironmentCorrection) {
      controlStatusError = i18n.msg.app.environmentCorrectionApiNotReady;
      return;
    }
    controlActionBusy = true;
    try {
      await api.setEnvironmentCorrection(enabled);
      await refreshControlStatus();
      setStatus(enabled ? i18n.msg.app.environmentCorrectionOn : i18n.msg.app.environmentCorrectionOff, "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.environmentCorrectionFailed(controlStatusError), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setLegacyPresenceFallback(enabled: boolean): Promise<void> {
    if (!config) return;
    controlActionBusy = true;
    const previousConfig = config;
    const nextConfig = normalizeSoftwareConfig({
      ...config,
      legacyPresenceFallback: enabled
    });
    config = nextConfig;
    try {
      await api.saveConfig(nextConfig);
      setStatus(enabled ? i18n.msg.app.legacyPresenceFallbackOn : i18n.msg.app.legacyPresenceFallbackOff, "ok");
    } catch (error) {
      config = previousConfig;
      setStatus(i18n.msg.app.legacyPresenceFallbackFailed(errorMessage(error)), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setTemperatureOffset(value: number): Promise<void> {
    if (!api.setTemperatureOffset) {
      controlStatusError = i18n.msg.app.temperatureOffsetApiNotReady;
      return;
    }
    controlActionBusy = true;
    try {
      await api.setTemperatureOffset(value);
      await refreshControlStatus();
      setStatus(i18n.msg.app.temperatureOffsetChanged, "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.temperatureOffsetFailed(controlStatusError), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setHumidityOffset(value: number): Promise<void> {
    if (!api.setHumidityOffset) {
      controlStatusError = i18n.msg.app.humidityOffsetApiNotReady;
      return;
    }
    controlActionBusy = true;
    try {
      await api.setHumidityOffset(value);
      await refreshControlStatus();
      setStatus(i18n.msg.app.humidityOffsetChanged, "ok");
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.humidityOffsetFailed(controlStatusError), "error");
    } finally {
      controlActionBusy = false;
    }
  }

  async function setTimezone(timezone: string): Promise<boolean> {
    if (!api.setTimezone || !api.getControlStatus) {
      controlStatusError = i18n.msg.app.timezoneApiNotReady;
      return false;
    }
    controlActionBusy = true;
    try {
      await api.setTimezone(timezone);
      let applied = false;
      for (let attempt = 0; attempt < 12; attempt += 1) {
        await wait(200);
        const nextStatus = await api.getControlStatus();
        controlStatus = nextStatus;
        controlStatusLoaded = true;
        controlStatusError = "";
        if (!nextStatus.timezoneApplyPending && nextStatus.timezone === timezone) {
          applied = true;
          break;
        }
      }
      if (!applied) throw new Error(i18n.msg.app.timezoneApplyTimeout);
      await refreshStats();
      setStatus(i18n.msg.app.timezoneChanged, "ok");
      return true;
    } catch (error) {
      controlStatusError = errorMessage(error);
      setStatus(i18n.msg.app.timezoneFailed(controlStatusError), "error");
      return false;
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
    return apiErrorMessage(i18n.msg, error);
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
    if (save) configSave.markPending();
  }

  function updateZoneDraftConfig(
    mutator: (current: WebDeviceConfig) => WebDeviceConfig,
    save: boolean | undefined = undefined,
    history: boolean | undefined = undefined
  ): void {
    const shouldRecordHistory = history ?? (save ?? true);
    updateConfig(mutator, false, shouldRecordHistory);
    configSave.markPending();
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

  function addExitZone(): void {
    activeZoneTool = "zones";
    zoneEditor.addZone("exit");
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
    return Boolean(target.closest(APP_SELECTION_PRESERVE_SELECTOR));
  }

  async function loadFloorplanBackup(): Promise<BackupFloorplanData | null> {
    let status;
    try {
      status = await loadFloorplanStorageStatus({ baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher });
    } catch {
      return null;
    }
    if (!status.hasConfig) return null;

    const document = await loadFloorplanStorageDocument({ baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher });
    if (!status.hasImage) return { document };

    const image = await loadFloorplanStorageImage({ baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher });
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
      await deviceStorageQueue.run("floorplan", () =>
        saveFloorplanStorage(
          {
            document: floorplan.document,
            image: base64ToBlob(floorplan.image.dataBase64, floorplan.image.mime)
          },
          { baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher }
        )
      );
      await markFloorplanRestored(true, floorplan.document);
      return;
    }
    await deviceStorageQueue.run("floorplan", () =>
      saveFloorplanStorageDocument(floorplan.document, { baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher })
    );
    await markFloorplanRestored(config?.floorplan?.hasImage === true, floorplan.document);
  }

  async function markFloorplanRestored(
    hasImage: boolean,
    document: FloorplanStorageDocument | undefined = undefined
  ): Promise<void> {
    if (!config) return;
    const room = document ? buildFloorplanRoomContext(document) : undefined;
    config = normalizeSoftwareConfig({
      ...config,
      floorplan: {
        ...(config.floorplan ?? {}),
        enabled: true,
        hasImage,
        room
      }
    });
    await configSave.saveConfigNow();
  }

  async function saveFloorplanFromPanel(
    ...[document, image]: Parameters<NonNullable<DeviceApi["saveFloorplan"]>>
  ): Promise<void> {
    if (!api.saveFloorplan) throw new Error(i18n.msg.app.floorplanSaveApiNotReady);
    await configSave.saveConfigNow();
    await api.saveFloorplan(document, image);
    await markFloorplanAvailable(true, document);
  }

  async function markFloorplanAvailable(
    hasImage: boolean,
    document: FloorplanStorageDocument | undefined = undefined
  ): Promise<void> {
    if (!config) return;
    const room = hasImage && document ? buildFloorplanRoomContext(document) : undefined;
    config = normalizeSoftwareConfig({
      ...config,
      floorplan: {
        ...(config.floorplan ?? {}),
        enabled: hasImage,
        hasImage,
        room
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

  function integrationMode(): IntegrationMode {
    const mode = config?.integrationMode;
    return mode === "edge" || mode === "ha" ? mode : "unknown";
  }

  function updateHaSetupGateFromStatus(status: WebSystemStatus): void {
    if (mockMode || requiresHaSetupHandoff() || haSetupGateDismissed || haSetupGateVisible || haSetupGateBusy) return;
    if (!status.api?.warning || status.api?.connected) return;
    if (!config) return;
    const mode = integrationMode();
    if (mode === "edge") return;
    if (mode === "unknown") {
      haSetupGateMode = "select";
      haSetupGateMessage = i18n.msg.app.setupModeSelectMessage;
      haSetupGateAllowContinue = false;
      haSetupGateVisible = true;
      return;
    }
    const uptimeSeconds = status.firmware?.uptimeSeconds ?? 0;
    haSetupGateMode = uptimeSeconds < 300 ? "api-warmup" : "api-warning";
    haSetupGateMessage =
      haSetupGateMode === "api-warmup"
        ? i18n.msg.app.haApiWarmupMessage
        : i18n.msg.app.haApiWarningMessage;
    haSetupGateAllowContinue = false;
    haSetupGateVisible = true;
  }

  function dismissHaSetupGate(): void {
    haSetupGateVisible = false;
    haSetupGateBusy = false;
    haSetupGateDismissed = true;
    haSetupGateAllowContinue = false;
  }

  function haSetupGateTitle(): string {
    if (haSetupGateMode === "select") return i18n.msg.app.setupTitles.select;
    if (haSetupGateMode === "edge") return i18n.msg.app.setupTitles.edge;
    if (haSetupGateMode === "ha-setup") return i18n.msg.app.setupTitles.haSetup;
    if (haSetupGateMode === "api-warmup") return i18n.msg.app.setupTitles.apiWarmup;
    return i18n.msg.app.setupTitles.apiWarning;
  }

  function haSetupGateBody(): string {
    if (haSetupGateMode === "select") {
      return i18n.msg.app.setupBodies.select;
    }
    if (haSetupGateMode === "edge") {
      return i18n.msg.app.setupBodies.edge;
    }
    if (haSetupGateMode === "ha-setup") {
      return i18n.msg.app.setupBodies.haSetup;
    }
    if (haSetupGateMode === "api-warmup") {
      return i18n.msg.app.setupBodies.apiWarmup;
    }
    return i18n.msg.app.setupBodies.apiWarning;
  }

  function haSetupGateButtonText(): string {
    if (haSetupGateAllowContinue) return i18n.msg.app.setupContinueDashboard;
    if (haSetupGateMode === "edge") return haSetupGateBusy ? i18n.msg.app.setupFinishingBusy : i18n.msg.app.setupContinueDashboard;
    return haSetupGateBusy ? i18n.msg.app.setupPreparingBusy : i18n.msg.app.setupConfirmContinue;
  }

  function cleanSetupUrl(): void {
    const cleanUrl = new URL(window.location.href);
    cleanUrl.searchParams.delete("setup");
    window.history.replaceState({}, "", `${cleanUrl.pathname}${cleanUrl.search}${cleanUrl.hash}`);
  }

  async function saveIntegrationMode(mode: Exclude<IntegrationMode, "unknown">): Promise<void> {
    const baseConfig =
      config ??
      normalizeSoftwareConfig(
        await api.getConfig().catch(() => ({
          version: 1,
          zones: [],
          calibrationZones: []
        }))
      );
    const nextConfig = normalizeSoftwareConfig({
      ...baseConfig,
      integrationMode: mode
    });
    config = nextConfig;
    await api.saveConfig(nextConfig);
  }

  async function chooseIntegrationMode(mode: Exclude<IntegrationMode, "unknown">): Promise<void> {
    if (haSetupGateBusy) return;
    haSetupGateBusy = true;
    haSetupGateAllowContinue = false;
    haSetupGateMode = mode === "edge" ? "edge" : "ha-setup";
    haSetupGateMessage =
      mode === "edge"
        ? i18n.msg.app.setupFinishingEdge
        : i18n.msg.app.haApiCheckingLong;

    try {
      await saveIntegrationMode(mode);
      await runHaSetupHandoff(mode);
    } catch (error) {
      haSetupGateMessage = i18n.msg.app.setupFinishFailed(errorMessage(error));
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
    }
  }

  async function changeIntegrationModeFromDashboard(): Promise<void> {
    if (integrationModeActionBusy) return;
    const currentMode = integrationMode();
    if (currentMode === "unknown") {
      haSetupGateMode = "select";
      haSetupGateMessage = i18n.msg.app.setupModeSelectMessage;
      haSetupGateAllowContinue = false;
      haSetupGateDismissed = false;
      haSetupGateVisible = true;
      return;
    }

    const nextMode: Exclude<IntegrationMode, "unknown"> = currentMode === "edge" ? "ha" : "edge";
    integrationModeActionBusy = true;
    try {
      await saveIntegrationMode(nextMode);
      if (nextMode === "edge") {
        dismissHaSetupGate();
        setStatus(i18n.msg.app.edgeModeSet, "ok");
        return;
      }

      if (systemStatus?.api?.connected && systemStatus.api.warning === false) {
        setStatus(i18n.msg.app.haModeSet, "ok");
        return;
      }

      haSetupGateMode = "ha-setup";
      haSetupGateMessage = i18n.msg.app.haSetupNeeded;
      haSetupGateAllowContinue = false;
      haSetupGateDismissed = false;
      haSetupGateVisible = true;
    } catch (error) {
      setStatus(i18n.msg.app.integrationModeChangeFailed(errorMessage(error)), "error");
    } finally {
      integrationModeActionBusy = false;
    }
  }

  async function runHaSetupHandoff(mode: Exclude<IntegrationMode, "unknown">): Promise<void> {
    if (!api.completeHaSetupHandoff) {
      dismissHaSetupGate();
      return;
    }

    await api.completeHaSetupHandoff();

    if (mode === "edge") {
      haSetupGateMessage = i18n.msg.app.setupRequestSent;
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
      cleanSetupUrl();
      void refreshSystemStatus();
      return;
    }

    haSetupGateMessage = i18n.msg.app.haApiCheckingLong;
    const readyState = await waitForHaApiReady(60000);

    if (readyState !== "ready") {
      haSetupGateMessage =
        i18n.msg.app.haSetupReadyNotice;
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
      return;
    }

    dismissHaSetupGate();
    haSetupGateMessage = "";
    cleanSetupUrl();
    void refreshSystemStatus();
  }

  async function completeHaSetupHandoff(): Promise<void> {
    if (haSetupGateBusy) return;
    if (haSetupGateAllowContinue) {
      dismissHaSetupGate();
      return;
    }
    if (haSetupGateMode === "select") return;
    const mode: Exclude<IntegrationMode, "unknown"> = haSetupGateMode === "edge" ? "edge" : "ha";
    if (!api.completeHaSetupHandoff) {
      dismissHaSetupGate();
      return;
    }

    haSetupGateBusy = true;
    haSetupGateAllowContinue = false;
    haSetupGateMessage =
      mode === "edge"
        ? i18n.msg.app.setupFinishingEdge
        : i18n.msg.app.setupFinishingHa;
    try {
      await saveIntegrationMode(mode);
      await runHaSetupHandoff(mode);
    } catch (error) {
      haSetupGateMessage = i18n.msg.app.haSetupCompleteFailed(errorMessage(error));
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
    }
  }
</script>

<main class="app-shell">
  <header class="top-bar">
    <div>
      <h1>Radar Zone Configurator</h1>
      <p>{demoMode ? i18n.msg.dashboard.demoMode : mockMode ? i18n.msg.dashboard.mockMode : i18n.msg.dashboard.liveMode}</p>
    </div>
    <div class="top-status-group">
      {#if demoMode}
        <div class="demo-pill">{i18n.msg.app.demo}</div>
        {#if onResetDemoStorage}
          <button class="demo-reset-button" type="button" onclick={onResetDemoStorage}>{i18n.msg.common.reset}</button>
        {/if}
      {/if}
      <div class="status-pill" data-status data-tone={statusTone}>{statusText}</div>
    </div>
  </header>
  <div class="toast" data-toast data-visible={errorText ? "true" : "false"}>{errorText}</div>

  {#if haSetupGateVisible}
    <div class="ha-setup-gate-backdrop" role="dialog" aria-modal="true" aria-labelledby="ha-setup-gate-title">
      <section class="ha-setup-gate-dialog">
        <div>
          <span>{i18n.msg.app.haIntegrationTitle}</span>
          <strong id="ha-setup-gate-title">{haSetupGateTitle()}</strong>
        </div>
        <p>{haSetupGateBody()}</p>
        <p>{haSetupGateMessage}</p>
        {#if haSetupGateMode === "api-warning"}
          <p class="ha-setup-gate-help">
            <a href="https://esphome.io/components/api/" target="_blank" rel="noreferrer">{i18n.msg.app.haDocsLabel}</a>
            {i18n.msg.app.haDocsSuffix}
          </p>
        {/if}
        {#if haSetupGateMode === "select"}
          <div class="ha-setup-gate-actions">
            <button type="button" disabled={haSetupGateBusy} onclick={() => chooseIntegrationMode("edge")}>
              {i18n.msg.app.edgeOnly}
            </button>
            <button type="button" disabled={haSetupGateBusy} onclick={() => chooseIntegrationMode("ha")}>
              {i18n.msg.app.haAlso}
            </button>
          </div>
        {:else}
          <button type="button" disabled={haSetupGateBusy} onclick={completeHaSetupHandoff}>
            {haSetupGateButtonText()}
          </button>
        {/if}
      </section>
    </div>
  {/if}

  <nav class="app-tabs" aria-label="Radar configurator sections">
    <div class="app-tab-list">
      <button class:active={activeTab === "dashboard"} type="button" onclick={() => setActiveTab("dashboard")}>{i18n.msg.tabs.dashboard}</button>
      <button class:active={activeTab === "zones"} type="button" onclick={() => setActiveTab("zones")}>{i18n.msg.tabs.zones}</button>
      <button class:active={activeTab === "floorplan"} type="button" onclick={() => setActiveTab("floorplan")}>{i18n.msg.tabs.floorplan}</button>
      <button class:active={activeTab === "stats"} type="button" onclick={() => setActiveTab("stats")}>{i18n.msg.tabs.stats}</button>
      <button class:active={activeTab === "backup"} type="button" onclick={() => setActiveTab("backup")}>{i18n.msg.tabs.backup}</button>
    </div>
    <div class="language-menu">
      <button
        class="language-menu-button"
        type="button"
        aria-haspopup="menu"
        aria-expanded={languageMenuOpen}
        onclick={() => (languageMenuOpen = !languageMenuOpen)}
      >
        {i18n.msg.language.name}
      </button>
      {#if languageMenuOpen}
        <div class="language-menu-list" role="menu">
          {#each languageOptions as option}
            <button
              class:active={i18n.language === option.code}
              type="button"
              role="menuitem"
              onclick={() => {
                i18n.setLanguage(option.code);
                languageMenuOpen = false;
              }}
            >
              {option.name}
            </button>
          {/each}
        </div>
      {/if}
    </div>
  </nav>

  {#if activeTab === "dashboard"}
    <DashboardPage
      {config}
      {state}
      {systemStatus}
      {systemStatusLoading}
      {systemStatusError}
      {controlStatus}
      {controlStatusLoading}
      {controlStatusError}
      {controlActionBusy}
      integrationMode={integrationMode()}
      {integrationModeActionBusy}
      messages={i18n.msg}
      {updatedText}
      floorplanStorageBaseUrl={deviceBaseUrl}
      {floorplanStorageFetcher}
      onNavigate={setActiveTab}
      onSetStatusLed={setStatusLed}
      onSetLedBlinkDuration={setLedBlinkDuration}
      onSetEnvironmentCorrection={setEnvironmentCorrection}
      onSetLegacyPresenceFallback={setLegacyPresenceFallback}
      onSetTemperatureOffset={setTemperatureOffset}
      onSetHumidityOffset={setHumidityOffset}
      onSetTimezone={setTimezone}
      onChangeIntegrationMode={changeIntegrationModeFromDashboard}
    />
  {:else if activeTab === "zones"}
    <ZonesPage
      {api}
      messages={i18n.msg}
      {activeZoneTool}
      {activeTargetCount}
      calibrationRun={Boolean(calibration.run)}
      calibrationStatusText={calibration.statusText()}
      {calibrationTypeLabels}
      {calibrationZones}
      canRedo={configHistory.canRedo}
      canUndo={configHistory.canUndo}
      {config}
      {debugMode}
      displayConfig={config ? displayConfig() : null}
      hasState={Boolean(state)}
      pirMotion={Boolean(state?.pirMotion)}
      saveState={configSave.saveState}
      saveStatusText={configSave.saveStatusText}
      {selectedCalibrationZone}
      {selectedLabel}
      {selectedPointIndex}
      {selectedZone}
      {selectedZoneId}
      {state}
      {updatedText}
      {zoneTypeLabels}
      {zones}
      onAddZone={addZone}
      onAddExitZone={addExitZone}
      onCalibrationInfoClick={(zoneId) => {
        selectZone(zoneId);
        radarInteraction.protectedZoneDialogOpen = true;
      }}
      onConvertToRect={radarInteraction.convertSelectedZoneToRect}
      onDeleteCalibrationZone={(zoneId) => {
        selectZone(zoneId);
        deleteSelected();
      }}
      onDeleteSelected={deleteSelected}
      onRedo={configHistory.redo}
      onSaveConfig={configSave.saveConfigNow}
      onSelectZone={selectZone}
      onSetActiveZoneTool={(tool) => (activeZoneTool = tool)}
      onSetCalibrationZoneType={setCalibrationZoneType}
      onSetDebugMode={(enabled) => (debugMode = enabled)}
      onSetZoneName={setSelectedZoneName}
      onSetZoneType={setSelectedZoneType}
      onStartCalibration={calibration.start}
      onStopCalibration={() => calibration.stop(i18n.msg.zones.calibrationStoppedByUser, "warn")}
      onUndo={configHistory.undo}
      onZoneEdgeClick={radarInteraction.handleRadarEdgeClick}
      onZonePointDoubleClick={radarInteraction.handlePointDoubleClick}
      onZonePointerDown={radarInteraction.handleRadarPointerDown}
      onCanvasClick={radarInteraction.handleRadarClick}
    />
  {:else if activeTab === "floorplan"}
    <FloorplanPage
      messages={i18n.msg}
      deviceConfig={displayConfig()}
      deviceState={state}
      floorplanStorageBaseUrl={deviceBaseUrl}
      {floorplanStorageFetcher}
      onUpdateDeviceConfig={(mutator) => updateConfig(mutator)}
      onSelectDeviceZone={(zoneId) => selectZone(zoneId)}
      onSaveDeviceConfig={configSave.saveConfigNow}
      onSaveFloorplan={saveFloorplanFromPanel}
      onFloorplanDeleted={() => void markFloorplanAvailable(false)}
      onUnsavedChange={setFloorplanUnsavedChanges}
    />
  {:else if activeTab === "stats"}
    <StatsPage messages={i18n.msg} {stats} error={statsError} loading={statsLoading} onRefresh={refreshStats} />
  {:else}
    <BackupPage
      messages={i18n.msg}
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
      {systemStatusLoading}
      {systemStatusError}
      {demoMode}
      issueText={backupRestore.issueText}
      onExport={backupRestore.exportBackup}
      onImportFile={backupRestore.readImportFile}
      onSetImportZones={backupRestore.setImportZones}
      onSetImportFloorplan={backupRestore.setImportFloorplan}
      onSetImportStats={backupRestore.setImportStats}
      onConfirmImport={backupRestore.confirmImport}
      onCancelImport={backupRestore.cancelImport}
      onGetSystemStatus={demoMode ? undefined : api.getSystemStatus}
      onUploadFirmware={demoMode ? undefined : api.uploadFirmware}
      onGetApiKey={demoMode ? undefined : api.getApiKey}
      onResetSystem={demoMode ? undefined : api.resetSystem}
      onRebootSystem={demoMode ? undefined : api.rebootSystem}
    />
  {/if}
</main>

<CalibrationDialog
  messages={i18n.msg}
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
  onStop={() => calibration.stop(i18n.msg.zones.calibrationStoppedByUser, "warn")}
/>

<ProtectedZoneDialog
  messages={i18n.msg}
  open={radarInteraction.protectedZoneDialogOpen}
  onClose={() => (radarInteraction.protectedZoneDialogOpen = false)}
/>

<ShrinkConfirmDialog
  messages={i18n.msg}
  zoneId={radarInteraction.shrinkConfirmZoneId}
  onCancel={radarInteraction.cancelCalibrationShrink}
  onConfirm={radarInteraction.unlockCalibrationMinSize}
/>
