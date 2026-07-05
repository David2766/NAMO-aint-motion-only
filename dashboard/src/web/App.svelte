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
  import SetupMockPanel from "./components/SetupMockPanel.svelte";
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
  import { mockFloorplanStorageFetch, resetMockFloorplanStorage } from "./floorplan/mock-floorplan-storage";
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
  const isDemoHost = window.location.hostname === "localhost" || window.location.hostname.endsWith(".github.io");
  const useSetupMock = (searchParams.get("setup") === "1" && isDemoHost) || window.location.hostname.endsWith(".github.io");
  const useDemoMode = useSetupMock || searchParams.get("demo") === "1" || window.location.hostname.endsWith(".github.io");
  const useMockApi = useDemoMode || searchParams.get("mock") === "1" || (!deviceBaseUrl && window.location.hostname === "localhost");
  const requiresHaSetupHandoff = searchParams.get("setup") === "1" && !useMockApi;
  const api: DeviceApi = useMockApi ? mockApi : deviceApi;
  const floorplanStorageFetcher = useMockApi ? mockFloorplanStorageFetch : undefined;
  const DEMO_SETUP_COMPLETE_KEY = "presence-sensor-demo-setup-complete";
  type IntegrationMode = "unknown" | "edge" | "ha";
  type HaSetupGateMode = "select" | "edge" | "ha-setup" | "api-warmup" | "api-warning";

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
  let setupMockCompleted = $state(useSetupMock && localStorage.getItem(DEMO_SETUP_COMPLETE_KEY) === "1");
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
  let statusText = $state("연결 대기");
  let statusTone = $state<"ok" | "warn" | "error">("warn");
  let errorText = $state("");
  let debugMode = $state(false);
  let activeTab = $state<"dashboard" | "zones" | "floorplan" | "stats" | "backup">("dashboard");
  let activeZoneTool = $state<"" | "zones" | "calibration">("");
  let haSetupGateVisible = $state(requiresHaSetupHandoff);
  let haSetupGateMode = $state<HaSetupGateMode>(requiresHaSetupHandoff ? "select" : "api-warmup");
  let haSetupGateBusy = $state(false);
  let haSetupGateMessage = $state(requiresHaSetupHandoff ? "사용할 연동 방식을 선택하세요." : "초기 연동 상태를 확인합니다.");
  let haSetupGateDismissed = $state(false);
  let haSetupGateAllowContinue = $state(false);

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
  let appRuntimeStarted = false;

  const selectedLabel = $derived(
    selectedZone
      ? `${zoneDisplayName(selectedZone)} · ${zoneTypeLabels[selectedZone.type]}`
      : selectedCalibrationZone
        ? `${zoneDisplayName(selectedCalibrationZone)} · 보정 구역 · ${calibrationLabel(selectedCalibrationZone)}`
        : "선택 없음"
  );

  function startAppRuntime(): void {
    if (appRuntimeStarted) return;
    appRuntimeStarted = true;
    radarPolling.start();

    window.addEventListener("pointermove", radarInteraction.handlePointerMove);
    window.addEventListener("pointerup", radarInteraction.handlePointerUp);
    window.addEventListener("pointercancel", radarInteraction.handlePointerUp);
    window.addEventListener("keydown", handleKeyDown);
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
    document.removeEventListener("pointerdown", handleDocumentPointerDown, true);
  }

  onMount(() => {
    if (!useSetupMock) startAppRuntime();
    return stopAppRuntime;
  });

  $effect(() => {
    if (useSetupMock && setupMockCompleted) {
      startAppRuntime();
    }
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
            ? "네트워크가 잠시 불안정할 수 있습니다. Home Assistant API 연결 상태를 확인하는 중입니다."
            : `Home Assistant API 연결을 기다리는 중입니다. ${elapsedSeconds}초 경과`;
      } catch (error) {
        systemStatusLoaded = true;
        systemStatusError = errorMessage(error);
        haSetupGateMessage =
          attempt < 4
            ? "기기가 네트워크를 정리하는 중입니다. 응답을 기다리고 있습니다."
            : "기기 응답을 기다리는 중입니다. Wi-Fi가 잠시 전환될 수 있습니다.";
      }

      await wait(1000);
    }

    return "timeout";
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
      await saveFloorplanStorage(
        {
          document: floorplan.document,
          image: base64ToBlob(floorplan.image.dataBase64, floorplan.image.mime)
        },
        { baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher }
      );
      await markFloorplanRestored(true);
      return;
    }
    await saveFloorplanStorageDocument(floorplan.document, { baseUrl: deviceBaseUrl, fetcher: floorplanStorageFetcher });
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

  function resetDemoStorage(): void {
    resetMockFloorplanStorage();
    localStorage.removeItem(DEMO_SETUP_COMPLETE_KEY);
    window.location.reload();
  }

  function completeSetupMock(): void {
    localStorage.setItem(DEMO_SETUP_COMPLETE_KEY, "1");
    setupMockCompleted = true;
  }

  function integrationMode(): IntegrationMode {
    const mode = config?.integrationMode;
    return mode === "edge" || mode === "ha" ? mode : "unknown";
  }

  function updateHaSetupGateFromStatus(status: WebSystemStatus): void {
    if (useMockApi || requiresHaSetupHandoff || haSetupGateDismissed || haSetupGateVisible || haSetupGateBusy) return;
    if (!status.api?.warning || status.api?.connected) return;
    if (!config) return;
    const mode = integrationMode();
    if (mode === "edge") return;
    if (mode === "unknown") {
      haSetupGateMode = "select";
      haSetupGateMessage = "사용할 연동 방식을 선택하세요.";
      haSetupGateAllowContinue = false;
      haSetupGateVisible = true;
      return;
    }
    const uptimeSeconds = status.firmware?.uptimeSeconds ?? 0;
    haSetupGateMode = uptimeSeconds < 300 ? "api-warmup" : "api-warning";
    haSetupGateMessage =
      haSetupGateMode === "api-warmup"
        ? "Home Assistant API 연결을 확인하는 중입니다. 이 과정은 약 1분 정도 걸릴 수 있습니다."
        : "Home Assistant 연결이 아직 확인되지 않습니다. 이미 기기를 추가했다면 네트워크와 Native API 설정을 확인하세요.";
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
    if (haSetupGateMode === "select") return "사용 환경을 선택하세요";
    if (haSetupGateMode === "edge") return "초기 설정을 마무리하는 중";
    if (haSetupGateMode === "ha-setup") return "Home Assistant 연동 준비";
    if (haSetupGateMode === "api-warmup") return "Home Assistant 연결 대기 중";
    return "Home Assistant 연결을 확인하세요";
  }

  function haSetupGateBody(): string {
    if (haSetupGateMode === "select") {
      return "SmartThings Edge만 사용할지, Home Assistant도 함께 사용할지 선택하세요. 나중에 Home Assistant를 사용하게 되면 다시 연동할 수 있습니다.";
    }
    if (haSetupGateMode === "edge") {
      return "초기 설정을 마무리하는 중입니다. Wi-Fi 연결이 잠시 불안정할 수 있습니다.";
    }
    if (haSetupGateMode === "ha-setup") {
      return "확인을 누르면 Home Assistant 연동 준비를 마무리합니다. 연결 확인에는 약 1분 정도 걸릴 수 있습니다.";
    }
    if (haSetupGateMode === "api-warmup") {
      return "Home Assistant 연동 준비가 아직 마무리되지 않았습니다. 확인을 누르면 API 연결 준비를 진행합니다.";
    }
    return "Home Assistant 연결이 아직 확인되지 않습니다. 확인을 눌러 API 연결 준비를 다시 진행한 뒤, Home Assistant에서 기기 추가 상태와 API 암호화 키를 확인하세요.";
  }

  function haSetupGateButtonText(): string {
    if (haSetupGateAllowContinue) return "대시보드 계속 사용";
    if (haSetupGateMode === "edge") return haSetupGateBusy ? "마무리 중..." : "대시보드 계속 사용";
    return haSetupGateBusy ? "연동 준비 중..." : "확인하고 계속";
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
        ? "초기 설정을 마무리하고 있습니다. Wi-Fi 연결이 잠시 불안정할 수 있습니다."
        : "Home Assistant 연동 준비를 시작합니다. 연결 확인에는 약 1분 정도 걸릴 수 있습니다.";

    try {
      await saveIntegrationMode(mode);
      await runHaSetupHandoff(mode);
    } catch (error) {
      haSetupGateMessage = `초기 설정을 마무리하지 못했습니다. ${errorMessage(error)}`;
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
    }
  }

  async function changeIntegrationModeFromDashboard(): Promise<void> {
    if (integrationModeActionBusy) return;
    const currentMode = integrationMode();
    if (currentMode === "unknown") {
      haSetupGateMode = "select";
      haSetupGateMessage = "사용할 연동 방식을 선택하세요.";
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
        setStatus("SmartThings Edge 중심으로 사용하도록 설정했습니다.", "ok");
        return;
      }

      if (systemStatus?.api?.connected && systemStatus.api.warning === false) {
        setStatus("Home Assistant도 함께 사용하도록 설정했습니다.", "ok");
        return;
      }

      haSetupGateMode = "ha-setup";
      haSetupGateMessage = "Home Assistant 연동 준비가 필요합니다.";
      haSetupGateAllowContinue = false;
      haSetupGateDismissed = false;
      haSetupGateVisible = true;
    } catch (error) {
      setStatus(`사용 환경을 변경하지 못했습니다. ${errorMessage(error)}`, "error");
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
      haSetupGateMessage = "초기 설정 요청을 보냈습니다. 대시보드는 계속 사용할 수 있습니다.";
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
      cleanSetupUrl();
      void refreshSystemStatus();
      return;
    }

    haSetupGateMessage = "Home Assistant API 연결 상태를 확인하는 중입니다. 약 1분 정도 걸릴 수 있습니다.";
    const readyState = await waitForHaApiReady(60000);

    if (readyState !== "ready") {
      haSetupGateMessage =
        "기기 설정은 완료되었습니다. Home Assistant 연결 확인에는 최대 1분 정도 걸릴 수 있습니다. 잠시 후 Home Assistant에서 기기 상태를 확인해 주세요.";
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
        ? "초기 설정을 마무리하고 있습니다. Wi-Fi 연결이 잠시 불안정할 수 있습니다."
        : "Home Assistant 연동을 위해 네트워크 설정을 마무리하는 중입니다.";
    try {
      await saveIntegrationMode(mode);
      await runHaSetupHandoff(mode);
    } catch (error) {
      haSetupGateMessage = `연동 준비를 완료하지 못했습니다. ${errorMessage(error)}`;
      haSetupGateBusy = false;
      haSetupGateAllowContinue = true;
    }
  }
</script>

{#if useSetupMock && !setupMockCompleted}
  <SetupMockPanel onComplete={completeSetupMock} />
{:else}
<main class="app-shell">
  <header class="top-bar">
    <div>
      <h1>Radar Zone Configurator</h1>
      <p>{useDemoMode ? "데모 모드입니다. 실제 기기에 저장되지 않습니다." : useMockApi ? "Mock 데이터로 확인 중입니다." : "실시간 위치와 구역 설정을 한 화면에서 확인합니다."}</p>
    </div>
    <div class="top-status-group">
      {#if useDemoMode}
        <div class="demo-pill">데모</div>
        <button class="demo-reset-button" type="button" onclick={resetDemoStorage}>초기화</button>
      {/if}
      <div class="status-pill" data-status data-tone={statusTone}>{statusText}</div>
    </div>
  </header>
  <div class="toast" data-toast data-visible={errorText ? "true" : "false"}>{errorText}</div>

  {#if haSetupGateVisible}
    <div class="ha-setup-gate-backdrop" role="dialog" aria-modal="true" aria-labelledby="ha-setup-gate-title">
      <section class="ha-setup-gate-dialog">
        <div>
          <span>Home Assistant 연동</span>
          <strong id="ha-setup-gate-title">{haSetupGateTitle()}</strong>
        </div>
        <p>{haSetupGateBody()}</p>
        <p>{haSetupGateMessage}</p>
        {#if haSetupGateMode === "api-warning"}
          <p class="ha-setup-gate-help">
            <a href="https://esphome.io/components/api/" target="_blank" rel="noreferrer">ESPHome Native API 문서</a>
            를 참고해 Home Assistant 연결 상태를 확인하세요.
          </p>
        {/if}
        {#if haSetupGateMode === "select"}
          <div class="ha-setup-gate-actions">
            <button type="button" disabled={haSetupGateBusy} onclick={() => chooseIntegrationMode("edge")}>
              SmartThings Edge만 사용
            </button>
            <button type="button" disabled={haSetupGateBusy} onclick={() => chooseIntegrationMode("ha")}>
              Home Assistant도 사용
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
        integrationMode={integrationMode()}
        {integrationModeActionBusy}
        {updatedText}
        floorplanStorageBaseUrl={deviceBaseUrl}
        {floorplanStorageFetcher}
        onNavigate={(tab) => (activeTab = tab)}
        onSetStatusLed={setStatusLed}
        onSetLedBlinkDuration={setLedBlinkDuration}
        onSetEnvironmentCorrection={setEnvironmentCorrection}
        onSetTemperatureOffset={setTemperatureOffset}
        onSetHumidityOffset={setHumidityOffset}
        onChangeIntegrationMode={changeIntegrationModeFromDashboard}
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
        {floorplanStorageFetcher}
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
        demoMode={useDemoMode}
        issueText={backupRestore.issueText}
        onExport={backupRestore.exportBackup}
        onImportFile={backupRestore.readImportFile}
        onSetImportZones={backupRestore.setImportZones}
        onSetImportFloorplan={backupRestore.setImportFloorplan}
        onSetImportStats={backupRestore.setImportStats}
        onConfirmImport={backupRestore.confirmImport}
        onCancelImport={backupRestore.cancelImport}
        onGetSystemStatus={useDemoMode ? undefined : api.getSystemStatus}
        onUploadFirmware={useDemoMode ? undefined : api.uploadFirmware}
        onGetApiKey={useDemoMode ? undefined : api.getApiKey}
        onResetSystem={useDemoMode ? undefined : api.resetSystem}
        onRebootSystem={useDemoMode ? undefined : api.rebootSystem}
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
{/if}
