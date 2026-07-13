<script lang="ts">
  import { onDestroy } from "svelte";
  import type { BackupIssue, BackupValidationResult } from "../../core/config-backup";
  import { apiErrorCode, apiErrorMessage } from "../api/api-message";
  import type { Messages } from "../i18n/types";
  import type { BackupRestoreProgressStep, BackupRestoreStage } from "../state/useBackupRestore.svelte";
  import { useDeviceStorageStatus } from "../state/useDeviceStorageStatus.svelte";
  import { formatCompactBytes as formatBytes } from "../utils/formatters";
  import type {
    FirmwareUploadProgress,
    WebApiKeyResult,
    WebSystemRebootResult,
    WebSystemResetOptions,
    WebSystemResetResult,
    WebSystemStatus
  } from "../types";

  type Props = {
    messages: Messages;
    loaded: boolean;
    stage: BackupRestoreStage;
    filename: string;
    message: string;
    errors: BackupIssue[];
    warnings: BackupIssue[];
    summary: BackupValidationResult["summary"] | null;
    progressSteps: BackupRestoreProgressStep[];
    progressPercent: number;
    importZones: boolean;
    importFloorplan: boolean;
    importStats: boolean;
    canImportFloorplan: boolean;
    canImportStats: boolean;
    canConfirmImport: boolean;
    systemStatus: WebSystemStatus | null;
    systemStatusLoading: boolean;
    systemStatusError: string;
    demoMode?: boolean;
    issueText: (issue: BackupIssue) => string;
    onExport: () => void;
    onImportFile: (file: File | null) => void;
    onSetImportZones: (enabled: boolean) => void;
    onSetImportFloorplan: (enabled: boolean) => void;
    onSetImportStats: (enabled: boolean) => void;
    onConfirmImport: () => void | Promise<void>;
    onCancelImport: () => void;
    onGetSystemStatus?: () => Promise<WebSystemStatus>;
    onUploadFirmware?: (file: File, onProgress: (progress: FirmwareUploadProgress) => void) => Promise<void>;
    onGetApiKey?: () => Promise<WebApiKeyResult>;
    onResetSystem?: (options: WebSystemResetOptions) => Promise<WebSystemResetResult>;
    onRebootSystem?: () => Promise<WebSystemRebootResult>;
  };

  type FirmwareStatusSnapshot = {
    version?: string;
    buildTime?: string;
  } | null;

  let {
    messages,
    loaded,
    stage,
    filename,
    message,
    errors,
    warnings,
    summary,
    progressSteps,
    progressPercent,
    importZones,
    importFloorplan,
    importStats,
    canImportFloorplan,
    canImportStats,
    canConfirmImport,
    systemStatus,
    systemStatusLoading,
    systemStatusError,
    demoMode = false,
    issueText,
    onExport,
    onImportFile,
    onSetImportZones,
    onSetImportFloorplan,
    onSetImportStats,
    onConfirmImport,
    onCancelImport,
    onGetSystemStatus,
    onUploadFirmware,
    onGetApiKey,
    onResetSystem,
    onRebootSystem
  }: Props = $props();

  const text = $derived(messages.backup);

  let fileInput: HTMLInputElement | null = $state(null);
  let firmwareFileInput: HTMLInputElement | null = $state(null);
  let firmwareFile: File | null = $state(null);
  let firmwareFileName = $state("");
  let firmwareFileSize = $state(0);
  let firmwareUploadStage = $state<"idle" | "selected" | "uploading" | "verifying" | "success" | "error" | "verify-error">("idle");
  let firmwareUploadPercent = $state(0);
  let firmwareUploadLoaded = $state(0);
  let firmwareUploadTotal = $state(0);
  let firmwareUploadMessage = $state("");
  let resetOptionsOpen = $state(false);
  let resetIncludeSettings = $state(true);
  let resetIncludeWifi = $state(false);
  let resetIncludeStats = $state(false);
  let resetConfirmOpen = $state(false);
  let resetExecuting = $state(false);
  let rebootConfirmOpen = $state(false);
  let rebootExecuting = $state(false);
  let apiKeyVisible = $state(false);
  let apiKeyLoading = $state(false);
  let apiKeyValue = $state("");
  let apiKeyMessage = $state("");
  let apiKeyHideTimer: ReturnType<typeof setTimeout> | null = null;
  let demoNoticeOpen = $state(false);
  let demoNoticeTitle = $state("");
  let demoNoticeMessage = $state("");
  let bootGuardNow = $state(Date.now());
  let bootGuardSnapshotMs = $state(Date.now());
  let bootGuardUptimeAtSnapshot = $state(0);
  const configStorageStatus = useDeviceStorageStatus("config", () => messages.common.saveStatus);
  let componentDestroyed = false;

  function showDemoNotice(title: string, message: string): void {
    demoNoticeTitle = title;
    demoNoticeMessage = message;
    demoNoticeOpen = true;
  }

  function showPreparedNotice(title: string, message: string): void {
    showDemoNotice(title, message);
  }

  function clearApiKeyTimer(): void {
    if (apiKeyHideTimer) {
      clearTimeout(apiKeyHideTimer);
      apiKeyHideTimer = null;
    }
  }

  function hideApiKey(message = ""): void {
    clearApiKeyTimer();
    apiKeyVisible = false;
    apiKeyValue = "";
    apiKeyMessage = message;
  }

  function scheduleApiKeyHide(seconds = 30): void {
    clearApiKeyTimer();
    apiKeyHideTimer = setTimeout(() => {
      hideApiKey(text.apiKeyHidden);
    }, Math.max(1, seconds) * 1000);
  }

  async function handleApiKeyReveal(): Promise<void> {
    if (apiKeyLoading) return;
    if (demoMode || !onGetApiKey) {
      showDemoNotice(text.apiKeyDemoTitle, text.apiKeyDemoMessage);
      return;
    }

    apiKeyLoading = true;
    apiKeyMessage = "";
    try {
      const result = await onGetApiKey();
      apiKeyValue = result.apiKey;
      apiKeyVisible = true;
      apiKeyMessage = text.apiKeyAutoHide(result.visibleSeconds ?? 30);
      scheduleApiKeyHide(result.visibleSeconds ?? 30);
    } catch (error) {
      const code = apiErrorCode(error);
      const message = apiErrorMessage(messages, error, text.apiKeyLoadFailed);
      hideApiKey(code === "api_key_not_set" ? text.apiKeyNotSet : message);
    } finally {
      apiKeyLoading = false;
    }
  }

  function legacyCopyText(value: string): boolean {
    const textarea = document.createElement("textarea");
    textarea.value = value;
    textarea.setAttribute("readonly", "");
    textarea.style.position = "fixed";
    textarea.style.left = "0";
    textarea.style.top = "0";
    textarea.style.width = "1px";
    textarea.style.height = "1px";
    textarea.style.opacity = "0";
    document.body.appendChild(textarea);
    textarea.focus();
    textarea.select();
    try {
      textarea.setSelectionRange(0, value.length);
    } catch {
      // Ignore selection range errors on older browsers.
    }
    let copied: boolean;
    try {
      copied = document.execCommand("copy");
    } catch {
      copied = false;
    }
    textarea.remove();
    return copied;
  }

  async function handleApiKeyCopy(): Promise<void> {
    if (!apiKeyValue) return;
    let copied = false;
    if (window.isSecureContext && navigator.clipboard?.writeText) {
      try {
        await navigator.clipboard.writeText(apiKeyValue);
        copied = true;
      } catch {
        copied = false;
      }
    }
    if (!copied) copied = legacyCopyText(apiKeyValue);
    hideApiKey(copied ? text.apiKeyCopied : text.apiKeyCopyFailed);
  }

  onDestroy(() => {
    componentDestroyed = true;
    configStorageStatus.destroy();
    clearApiKeyTimer();
  });

  $effect(() => {
    if (!systemStatus?.boot?.initialGuardActive) return;
    const timer = setInterval(() => {
      bootGuardNow = Date.now();
    }, 1000);
    return () => clearInterval(timer);
  });

  $effect(() => {
    bootGuardSnapshotMs = Date.now();
    bootGuardNow = bootGuardSnapshotMs;
    bootGuardUptimeAtSnapshot = Math.max(0, Number(systemStatus?.firmware?.uptimeSeconds ?? 0));
  });

  function wait(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  function snapshotFirmwareStatus(status: WebSystemStatus | null): FirmwareStatusSnapshot {
    if (!status?.firmware) return null;
    return {
      version: status.firmware.version,
      buildTime: status.firmware.buildTime
    };
  }

  async function readFirmwareStatusSnapshot(): Promise<FirmwareStatusSnapshot> {
    if (!onGetSystemStatus) return snapshotFirmwareStatus(systemStatus);
    try {
      return snapshotFirmwareStatus(await onGetSystemStatus());
    } catch {
      return null;
    }
  }

  function firmwareStatusChanged(before: FirmwareStatusSnapshot, after: FirmwareStatusSnapshot): boolean | null {
    if (!after) return null;
    if (!before) return null;
    return after.version !== before.version || after.buildTime !== before.buildTime;
  }

  async function waitForFirmwareReboot(before: FirmwareStatusSnapshot): Promise<"changed" | "same" | "unknown" | "timeout"> {
    if (!onGetSystemStatus) return "unknown";

    const deadline = Date.now() + 180000;
    let attempts = 0;

    await wait(3000);

    while (Date.now() < deadline && !componentDestroyed) {
      attempts += 1;
      firmwareUploadMessage =
        attempts <= 2 ? text.firmwareRebooting : attempts <= 8 ? text.firmwareWaitingResponse : text.firmwareCheckingApplied;

      try {
        const after = snapshotFirmwareStatus(await onGetSystemStatus());
        const changed = firmwareStatusChanged(before, after);
        if (changed === true) return "changed";
        if (changed === false) return "same";
        return "unknown";
      } catch {
        await wait(2000);
      }
    }

    return "timeout";
  }

  async function verifyFirmwareRebootAndReload(before: FirmwareStatusSnapshot): Promise<void> {
    firmwareUploadStage = "verifying";
    firmwareUploadPercent = 100;
    firmwareUploadLoaded = firmwareFileSize || firmwareUploadLoaded;
    firmwareUploadTotal = firmwareFileSize || firmwareUploadTotal;
    firmwareUploadMessage = text.firmwareRebooting;

    const result = await waitForFirmwareReboot(before);
    if (componentDestroyed) return;

    if (result === "timeout") {
      firmwareUploadStage = "verify-error";
      firmwareUploadMessage = text.firmwareVerifyTimeout;
      return;
    }

    firmwareUploadStage = "success";
    firmwareFile = null;
    firmwareUploadMessage =
      result === "changed"
        ? text.firmwareAppliedReload
        : result === "same"
          ? text.firmwareSameVersionReload
          : text.firmwareRespondedReload;

    await wait(1000);
    if (!componentDestroyed) window.location.reload();
  }

  function openFilePicker(): void {
    if (demoMode) {
      showDemoNotice(text.restoreDemoTitle, text.restoreDemoOpenMessage);
      return;
    }
    fileInput?.click();
  }

  function handleFileChange(event: Event): void {
    const input = event.currentTarget as HTMLInputElement;
    if (demoMode) {
      input.value = "";
      showDemoNotice(text.restoreDemoTitle, text.restoreDemoFileMessage);
      return;
    }
    onImportFile(input.files?.[0] || null);
    input.value = "";
  }

  function openFirmwareFilePicker(): void {
    if (firmwareUploadStage === "uploading" || firmwareUploadStage === "verifying") return;
    if (bootGuardActive) {
      showBootGuardNotice(text.firmwareUpdateTitle);
      return;
    }
    if (demoMode) {
      showDemoNotice(text.firmwareDemoTitle, text.firmwareDemoOpenMessage);
      return;
    }
    firmwareFileInput?.click();
  }

  function handleFirmwareAction(): void {
    if (firmwareUploadStage === "uploading" || firmwareUploadStage === "verifying") return;
    if (bootGuardActive) {
      showBootGuardNotice(text.firmwareUpdateTitle);
      return;
    }
    if (firmwareUploadStage === "selected" && firmwareFile) {
      void uploadSelectedFirmware();
      return;
    }
    openFirmwareFilePicker();
  }

  function handleFirmwareFileChange(event: Event): void {
    const input = event.currentTarget as HTMLInputElement;
    const file = input.files?.[0] || null;
    input.value = "";
    if (bootGuardActive) {
      showBootGuardNotice(text.firmwareUpdateTitle);
      return;
    }
    if (demoMode) {
      showDemoNotice(text.firmwareDemoTitle, text.firmwareDemoFileMessage);
      return;
    }
    if (!file) return;

    firmwareFile = null;
    firmwareFileName = file.name;
    firmwareFileSize = file.size;
    firmwareUploadPercent = 0;
    firmwareUploadLoaded = 0;
    firmwareUploadTotal = file.size;

    if (!file.name.toLowerCase().endsWith(".bin")) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = text.firmwareBinOnly;
      return;
    }

    if (Number.isFinite(systemStatus?.flash?.otaSlotBytes ?? NaN) && file.size > Number(systemStatus?.flash?.otaSlotBytes)) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = text.firmwareFileTooLarge(formatBytes(file.size), formatBytes(systemStatus?.flash?.otaSlotBytes));
      return;
    }

    if (!onUploadFirmware) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = text.firmwareApiNotReady;
      return;
    }

    firmwareFile = file;
    firmwareUploadStage = "selected";
    firmwareUploadMessage = text.firmwareFileReady;
  }

  async function uploadSelectedFirmware(): Promise<void> {
    if (!firmwareFile || !onUploadFirmware) return;
    const file = firmwareFile;
    const beforeStatus = await readFirmwareStatusSnapshot();
    firmwareUploadStage = "uploading";
    firmwareUploadMessage = text.firmwareUploadingMessage;
    try {
      await onUploadFirmware(file, (progress) => {
        firmwareUploadLoaded = progress.loaded;
        firmwareUploadTotal = progress.total;
        firmwareUploadPercent = progress.percent;
      });
      firmwareUploadPercent = 100;
      firmwareUploadLoaded = file.size;
      firmwareUploadTotal = file.size;
      await verifyFirmwareRebootAndReload(beforeStatus);
    } catch (error) {
      if (firmwareUploadPercent >= 95) {
        firmwareUploadPercent = 100;
        firmwareUploadLoaded = file.size;
        firmwareUploadTotal = file.size;
        firmwareUploadMessage = text.firmwareUploadResponseLost;
        await verifyFirmwareRebootAndReload(beforeStatus);
        return;
      }
      firmwareUploadStage = "error";
      firmwareUploadMessage = apiErrorMessage(messages, error, text.firmwareUpdateFailedMessage);
    }
  }

  function handleExport(): void {
    onExport();
  }

  function handleConfirmImport(): void {
    if (restoreStorageBusy) return;
    if (demoMode) {
      showDemoNotice(text.restoreDemoTitle, text.restoreDemoConfirmMessage);
      return;
    }
    onConfirmImport();
  }

  function handleCancelImport(): void {
    onCancelImport();
  }

  function handleImportZonesChange(event: Event): void {
    onSetImportZones((event.currentTarget as HTMLInputElement).checked);
  }

  function handleImportFloorplanChange(event: Event): void {
    onSetImportFloorplan((event.currentTarget as HTMLInputElement).checked);
  }

  function handleImportStatsChange(event: Event): void {
    onSetImportStats((event.currentTarget as HTMLInputElement).checked);
  }

  function handleResetOptionsOpen(): void {
    if (bootGuardActive) {
      showBootGuardNotice(text.resetButton);
      return;
    }
    resetOptionsOpen = true;
  }

  function handleRebootConfirmOpen(): void {
    if (bootGuardActive) {
      showBootGuardNotice(text.rebootButton);
      return;
    }
    rebootConfirmOpen = true;
  }

  async function executeSystemReboot(): Promise<void> {
    if (rebootExecuting) return;
    if (bootGuardActive) {
      rebootConfirmOpen = false;
      showBootGuardNotice(text.rebootButton);
      return;
    }
    if (demoMode || !onRebootSystem) {
      rebootConfirmOpen = false;
      showDemoNotice(text.rebootDemoTitle, text.rebootDemoMessage);
      return;
    }

    rebootExecuting = true;
    try {
      const result = await onRebootSystem();
      rebootConfirmOpen = false;
      const seconds = Math.max(1, Math.round((result.rebootInMs ?? 2500) / 1000));
      showDemoNotice(text.rebootRequestedTitle, text.rebootRequestedMessage(seconds));
    } catch (error) {
      rebootConfirmOpen = false;
      showDemoNotice(text.rebootFailedTitle, apiErrorMessage(messages, error, text.rebootFailedMessage));
    } finally {
      rebootExecuting = false;
    }
  }

  function handleResetSettingsChange(event: Event): void {
    resetIncludeSettings = (event.currentTarget as HTMLInputElement).checked;
  }

  function handleResetWifiChange(event: Event): void {
    resetIncludeWifi = (event.currentTarget as HTMLInputElement).checked;
  }

  function handleResetStatsChange(event: Event): void {
    resetIncludeStats = (event.currentTarget as HTMLInputElement).checked;
  }

  function handleResetConfirmRequest(): void {
    if (bootGuardActive) {
      showBootGuardNotice(text.resetButton);
      return;
    }
    if (!resetIncludeSettings && !resetIncludeWifi && !resetIncludeStats) {
      showPreparedNotice(text.resetSelectionRequiredTitle, text.resetSelectionRequiredMessage);
      return;
    }

    resetConfirmOpen = true;
  }

  async function executeSystemReset(): Promise<void> {
    if (resetExecuting) return;
    if (bootGuardActive) {
      resetConfirmOpen = false;
      showBootGuardNotice(text.resetButton);
      return;
    }
    if (demoMode || !onResetSystem) {
      resetConfirmOpen = false;
      showDemoNotice(text.resetDemoTitle, text.resetDemoMessage);
      return;
    }

    resetExecuting = true;
    try {
      const result = await onResetSystem({
        settings: resetIncludeSettings,
        wifi: resetIncludeWifi,
        stats: resetIncludeStats
      });
      resetConfirmOpen = false;
      resetOptionsOpen = false;
      const rebootText = result.rebootRequired
        ? text.resetRebootSuffix(Math.max(1, Math.round((result.rebootInMs ?? 2500) / 1000)))
        : "";
      showDemoNotice(text.resetRequestedTitle, text.resetRequestedMessage(rebootText));
    } catch (error) {
      resetConfirmOpen = false;
      showDemoNotice(text.resetFailedTitle, apiErrorMessage(messages, error, text.resetFailedMessage));
    } finally {
      resetExecuting = false;
    }
  }

  const importing = $derived(stage === "importing");
  const restoreStorageBusy = $derived(!importing && configStorageStatus.isBusy);
  const bootGuardSeconds = $derived(Math.max(1, Math.round(systemStatus?.boot?.guardSeconds ?? 60)));
  const bootGuardRemainingSeconds = $derived.by(() => {
    if (!systemStatus?.boot?.initialGuardActive) return 0;
    const elapsedSeconds = bootGuardUptimeAtSnapshot + Math.floor(Math.max(0, bootGuardNow - bootGuardSnapshotMs) / 1000);
    return Math.max(0, bootGuardSeconds - elapsedSeconds);
  });
  const bootGuardActive = $derived(bootGuardRemainingSeconds > 0);
  const bootGuardHint = $derived(
    bootGuardActive
      ? text.bootGuardHint(bootGuardRemainingSeconds)
      : ""
  );
  const canConfirmReset = $derived(resetIncludeSettings || resetIncludeWifi || resetIncludeStats);
  const showRestoreProgress = $derived(progressSteps.length > 0 && (stage === "importing" || stage === "imported" || stage === "error"));

  function showBootGuardNotice(action: string): void {
    showPreparedNotice(
      text.bootGuardTitle,
      text.bootGuardNotice(action, bootGuardSeconds)
    );
  }

  function formatPercent(used: number | null | undefined, total: number | null | undefined): string {
    if (!Number.isFinite(used ?? NaN) || !Number.isFinite(total ?? NaN) || Number(total) <= 0) return "-";
    return `${Math.round((Number(used) / Number(total)) * 100)}%`;
  }

  function subtractBytes(total: number | null | undefined, free: number | null | undefined): number | undefined {
    if (!Number.isFinite(total ?? NaN) || !Number.isFinite(free ?? NaN)) return undefined;
    return Math.max(0, Number(total) - Number(free));
  }

  function addBytes(...values: Array<number | null | undefined>): number | undefined {
    if (values.some((value) => !Number.isFinite(value ?? NaN))) return undefined;
    return values.reduce((sum, value) => sum + Number(value), 0);
  }

  function wifiSignalInfo(connected: boolean | undefined, rssi: number | null | undefined): { bars: number; label: string } {
    if (!connected || !Number.isFinite(rssi ?? NaN)) return { bars: 0, label: text.wifiNotConnected };
    const value = Number(rssi);
    if (value >= -50) return { bars: 4, label: text.wifiExcellent };
    if (value >= -60) return { bars: 3, label: text.wifiGood };
    if (value >= -70) return { bars: 2, label: text.wifiFair };
    if (value >= -80) return { bars: 1, label: text.wifiWeak };
    return { bars: 1, label: text.wifiVeryWeak };
  }

  function formatUptime(seconds: number | null | undefined): string {
    if (!Number.isFinite(seconds ?? NaN)) return "-";
    const total = Math.max(0, Math.round(Number(seconds)));
    const days = Math.floor(total / 86400);
    const hours = Math.floor((total % 86400) / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    if (days > 0) return text.uptimeDaysHours(days, hours);
    if (hours > 0) return text.uptimeHoursMinutes(hours, minutes);
    return text.uptimeMinutes(minutes);
  }

  const firmwareVersion = $derived(systemStatus?.firmware?.version || "-");
  const dashboardVersion = $derived(systemStatus?.dashboard?.version || "-");
  const dataStorageUsedBytes = $derived(systemStatus?.flash?.storageUsedBytes ?? systemStatus?.storage?.usedBytes);
  const dataStorageTotalBytes = $derived(systemStatus?.flash?.storageTotalBytes ?? systemStatus?.storage?.totalBytes);
  const dataStorageUsedText = $derived(
    `${formatBytes(dataStorageUsedBytes)} / ${formatBytes(dataStorageTotalBytes)}`
  );
  const dataStoragePercentText = $derived(formatPercent(dataStorageUsedBytes, dataStorageTotalBytes));
  const firmwareSpaceText = $derived(
    `${formatBytes(systemStatus?.flash?.firmwareUsedBytes)} / ${formatBytes(systemStatus?.flash?.firmwareSlotBytes)}`
  );
  const flashOccupiedBytes = $derived(
    addBytes(systemStatus?.flash?.firmwareUsedBytes, systemStatus?.flash?.otaSlotBytes, dataStorageUsedBytes)
  );
  const flashOccupancyText = $derived(
    `${formatBytes(flashOccupiedBytes)} / ${formatBytes(systemStatus?.flash?.totalBytes)}`
  );
  const flashOccupancyPercentText = $derived(formatPercent(flashOccupiedBytes, systemStatus?.flash?.totalBytes));
  const internalRamTotalBytes = $derived(systemStatus?.memory?.internalTotalBytes);
  const internalRamFreeBytes = $derived(systemStatus?.memory?.internalFreeBytes ?? systemStatus?.memory?.freeHeap);
  const internalRamUsedBytes = $derived(subtractBytes(internalRamTotalBytes, internalRamFreeBytes));
  const internalRamText = $derived(`${formatBytes(internalRamUsedBytes)} / ${formatBytes(internalRamTotalBytes)}`);
  const internalRamPercentText = $derived(formatPercent(internalRamUsedBytes, internalRamTotalBytes));
  const externalRamTotalBytes = $derived(systemStatus?.memory?.externalTotalBytes ?? systemStatus?.memory?.psramTotal);
  const externalRamFreeBytes = $derived(systemStatus?.memory?.externalFreeBytes ?? systemStatus?.memory?.psramFree);
  const externalRamUsedBytes = $derived(subtractBytes(externalRamTotalBytes, externalRamFreeBytes));
  const externalRamText = $derived(`${formatBytes(externalRamUsedBytes)} / ${formatBytes(externalRamTotalBytes)}`);
  const externalRamPercentText = $derived(formatPercent(externalRamUsedBytes, externalRamTotalBytes));
  const minInternalRamText = $derived(formatBytes(systemStatus?.memory?.internalMinFreeBytes ?? systemStatus?.memory?.minFreeHeap));
  const wifiInfo = $derived(wifiSignalInfo(systemStatus?.wifi?.connected, systemStatus?.wifi?.rssi));
  const firmwareFileText = $derived(firmwareFileName ? `${firmwareFileName} · ${formatBytes(firmwareFileSize)}` : text.notSelected);
  const firmwareActionText = $derived(
    firmwareUploadStage === "uploading"
      ? text.firmwareUploading
      : firmwareUploadStage === "verifying"
        ? text.firmwareVerifying
        : firmwareUploadStage === "selected"
          ? text.firmwareUpload
          : text.firmwareManualUpdate
  );
  const firmwareUploadProgressText = $derived(
    `${firmwareUploadPercent}% · ${formatBytes(firmwareUploadLoaded)} / ${formatBytes(firmwareUploadTotal || firmwareFileSize)}`
  );
  const firmwareUploadReadyText = $derived(
    systemStatus?.flash?.otaSlotBytes
      ? text.firmwareReadyWithOta(formatBytes(systemStatus.flash.otaSlotBytes))
      : text.firmwareReadyWithoutOta
  );
</script>

<section class="backup-workspace">
  <section class="backup-section-row">
    <aside class="backup-side-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>{text.title}</strong>
          <span>{text.description}</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{text.status}</dt>
            <dd>{loaded ? text.dataReady : text.loading}</dd>
          </div>
          <div>
            <dt>{text.file}</dt>
            <dd>{filename || text.notSelected}</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-stored-tools">
        <button type="button" disabled={!loaded || importing} onclick={handleExport}>{text.exportButton}</button>
        <button type="button" disabled={!loaded || importing} onclick={openFilePicker}>{text.importButton}</button>
        <input bind:this={fileInput} type="file" accept="application/json,.json" hidden onchange={handleFileChange} />
      </div>
    </aside>

    <section class="backup-result-panel" aria-live="polite">
      <div class="floorplan-workflow-card">
        {#if message}
          <div class={`floorplan-status backup-message ${stage}`}>
            <strong>{filename || text.backupFile}</strong>
            <span>{message}</span>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>{text.emptyTitle}</strong>
            <span>{text.emptyDescription}</span>
          </div>
        {/if}

        {#if summary && (stage === "ready" || stage === "warning" || stage === "imported" || stage === "exported")}
          <dl class="backup-summary floorplan-stored-summary">
            <div>
              <dt>{text.zones}</dt>
              <dd>{summary.softwareZones}</dd>
            </div>
            <div>
              <dt>{text.calibration}</dt>
              <dd>{summary.calibrationZones}</dd>
            </div>
            <div>
              <dt>{text.filterReducedDisabled}</dt>
              <dd>{summary.filterZones}/{summary.reducedZones}/{summary.disabledZones}</dd>
            </div>
            <div>
              <dt>{text.floorplan}</dt>
              <dd>{summary.hasFloorplan ? `${text.included}${summary.floorplanImageBytes ? ` · ${Math.round(summary.floorplanImageBytes / 1024)}KB` : ""}` : text.none}</dd>
            </div>
            <div>
              <dt>{text.statistics}</dt>
              <dd>
                {summary.hasStats
                  ? `${text.included} · ${text.dailyDays(summary.statsDailyDays)}${summary.hasHeatmap ? ` · ${text.heatmap}` : ""}`
                  : text.none}
              </dd>
            </div>
          </dl>
        {/if}

        {#if showRestoreProgress}
          <div class="backup-restore-progress floorplan-edit-tool-card" data-stage={stage}>
            <div class="backup-restore-progress-header">
              <strong>{text.restoreProgress}</strong>
              <span>{progressPercent}%</span>
            </div>
            <div class="firmware-progress-track" aria-label={text.restoreProgressAria}>
              <i style={`width: ${progressPercent}%`}></i>
            </div>
            <ol>
              {#each progressSteps as step}
                <li data-status={step.status}>
                  <span>{step.label}</span>
                  <small>{step.detail}</small>
                </li>
              {/each}
            </ol>
          </div>
        {/if}

        {#if summary && (stage === "ready" || stage === "warning")}
          <div class="floorplan-edit-tool-card backup-import-options">
            <strong>{text.importData}</strong>
            <label>
              <input type="checkbox" checked={importZones} disabled={importing} onchange={handleImportZonesChange} />
              <span>{text.importZonesAndCalibration}</span>
            </label>
            <label class:disabled={!canImportFloorplan}>
              <input
                type="checkbox"
                checked={importFloorplan}
                disabled={!canImportFloorplan || importing}
                onchange={handleImportFloorplanChange}
              />
              <span>{text.importFloorplanAndImage}</span>
            </label>
            <label class:disabled={!canImportStats}>
              <input
                type="checkbox"
                checked={importStats}
                disabled={!canImportStats || importing}
                onchange={handleImportStatsChange}
              />
              <span>{text.importStatsAndHeatmap}</span>
            </label>
          </div>
        {/if}

        {#if errors.length > 0}
          <div class="backup-issues error">
            <strong>{text.importErrorsTitle}</strong>
            <ul>
              {#each errors as error}
                <li>{issueText(error)}</li>
              {/each}
            </ul>
          </div>
        {/if}

        {#if warnings.length > 0}
          <div class="backup-issues warn">
            <strong>{text.importWarningsTitle}</strong>
            <ul>
              {#each warnings as warning}
                <li>{issueText(warning)}</li>
              {/each}
            </ul>
          </div>
        {/if}

        {#if stage === "ready" || stage === "warning" || stage === "importing"}
          <div class="floorplan-edit-tool-card backup-confirm">
            <p>{text.importConfirmDescription}</p>
            <div>
              <button type="button" class="danger-button" disabled={!canConfirmImport || importing || restoreStorageBusy} onclick={handleConfirmImport}>
                {importing ? text.importing : restoreStorageBusy ? text.importWaitingForStorage : text.importSelectedData}
              </button>
              <button type="button" disabled={importing} onclick={handleCancelImport}>{text.cancel}</button>
            </div>
            {#if restoreStorageBusy}
              <small>{text.importStorageBusyDescription}</small>
            {/if}
          </div>
        {/if}
      </div>
    </section>
  </section>

  <div class="backup-section-divider"></div>

  <section class="backup-section-row">
    <aside class="backup-side-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>{text.firmwareUpdateTitle}</strong>
          <span>{text.firmwareUpdateDescription}</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{text.firmware}</dt>
            <dd>{systemStatusLoading ? text.checking : firmwareVersion}</dd>
          </div>
          <div>
            <dt>{text.dashboard}</dt>
            <dd>{systemStatusLoading ? text.checking : dashboardVersion}</dd>
          </div>
          <div>
            <dt>{text.file}</dt>
            <dd>{firmwareFileText}</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-stored-tools">
        <button type="button" disabled>{text.firmwareUpdateButton}</button>
        <button
          type="button"
          class:danger-button={firmwareUploadStage === "selected"}
          disabled={bootGuardActive || firmwareUploadStage === "uploading" || firmwareUploadStage === "verifying"}
          title={bootGuardHint || undefined}
          onclick={handleFirmwareAction}
        >
          {firmwareActionText}
        </button>
        <input bind:this={firmwareFileInput} type="file" accept=".bin,application/octet-stream" hidden onchange={handleFirmwareFileChange} />
      </div>
    </aside>

    <section class="backup-result-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>{text.firmwareUpdateTitle}</strong>
          <span>{text.firmwareManualDescription}</span>
        </div>
        {#if firmwareUploadStage === "uploading"}
          <div class="firmware-update-status active">
            <strong>{text.firmwareUploadingTitle}</strong>
            <span>{firmwareUploadProgressText}</span>
            <div class="firmware-progress-track" aria-label={text.firmwareUploadProgressAria}>
              <i style={`width: ${firmwareUploadPercent}%`}></i>
            </div>
            <small>{text.firmwareDoNotClose}</small>
          </div>
        {:else if firmwareUploadStage === "verifying"}
          <div class="firmware-update-status active">
            <strong>{text.firmwareVerifyingTitle}</strong>
            <span>{firmwareUploadMessage}</span>
            <div class="firmware-progress-track" aria-label={text.firmwareVerifyProgressAria}>
              <i style="width: 100%"></i>
            </div>
            <small>{text.firmwareRebootDisconnectNote}</small>
          </div>
        {:else if firmwareUploadStage === "success"}
          <div class="firmware-update-status success">
            <strong>{text.firmwareSuccessTitle}</strong>
            <span>{firmwareUploadMessage}</span>
            <small>{text.firmwareRebootDisconnectNote}</small>
          </div>
        {:else if firmwareUploadStage === "verify-error"}
          <div class="firmware-update-status error">
            <strong>{text.firmwareVerifyFailedTitle}</strong>
            <span>{firmwareUploadMessage}</span>
            <small>{text.firmwareVerifyFailedHint}</small>
          </div>
        {:else if firmwareUploadStage === "error"}
          <div class="firmware-update-status error">
            <strong>{text.firmwareFailedTitle}</strong>
            <span>{firmwareUploadMessage}</span>
            <small>{text.firmwareFailedHint}</small>
          </div>
        {:else if firmwareUploadStage === "selected"}
          <div class="firmware-update-status active">
            <strong>{text.firmwareReadyTitle}</strong>
            <span>{firmwareUploadMessage}</span>
            <small>{text.firmwareReadyHint(firmwareFileText)}</small>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>{text.firmwareIdleTitle}</strong>
            <span>{firmwareUploadReadyText}</span>
          </div>
        {/if}
      </div>
    </section>
  </section>

  <div class="backup-section-divider"></div>

  <section class="backup-section-row danger-zone-row">
    <aside class="backup-side-panel">
      <div class="floorplan-workflow-card danger-zone-card">
        <div>
          <strong>{text.dangerZoneTitle}</strong>
          <span>{text.dangerZoneDescription}</span>
        </div>
      </div>

      <div class="floorplan-stored-tools danger-zone-tools">
        <button type="button" disabled={apiKeyLoading} onclick={handleApiKeyReveal}>
          {apiKeyLoading ? text.checking : text.apiKeyReveal}
        </button>
        <button type="button" disabled>{text.apiKeyRegenerate}</button>
        <button
          type="button"
          class="danger-button"
          disabled={bootGuardActive || rebootExecuting}
          title={bootGuardHint || undefined}
          onclick={handleRebootConfirmOpen}
        >
          {text.rebootDevice}
        </button>
        <button
          type="button"
          class="danger-button"
          disabled={bootGuardActive}
          title={bootGuardHint || undefined}
          onclick={handleResetOptionsOpen}
        >
          {text.resetButton}
        </button>
      </div>
    </aside>

    <section class="backup-result-panel">
      <div class="floorplan-workflow-card danger-zone-card">
        <div>
          <strong>{text.resetScopeTitle}</strong>
          <span>{text.resetScopeDescription}</span>
        </div>

        {#if apiKeyVisible}
          <div class="floorplan-edit-tool-card backup-import-options danger-reset-options">
            <strong>{text.apiKey}</strong>
            <code class="api-key-value">{apiKeyValue}</code>
            <span>{apiKeyMessage}</span>
            <button type="button" onclick={handleApiKeyCopy}>{text.copy}</button>
          </div>
        {:else if apiKeyMessage}
          <div class="floorplan-empty-note backup-empty-state danger-reset-empty">
            <strong>{text.apiKey}</strong>
            <span>{apiKeyMessage}</span>
          </div>
        {/if}

        {#if resetOptionsOpen}
          <div class="floorplan-edit-tool-card backup-import-options danger-reset-options">
            <strong>{text.resetItemsTitle}</strong>
            <label>
              <input type="checkbox" checked={resetIncludeSettings} onchange={handleResetSettingsChange} />
              <span>{text.resetSettingsData}</span>
            </label>
            <label>
              <input type="checkbox" checked={resetIncludeWifi} onchange={handleResetWifiChange} />
              <span>{text.resetWifiData}</span>
            </label>
            <label>
              <input type="checkbox" checked={resetIncludeStats} onchange={handleResetStatsChange} />
              <span>{text.resetStatsData}</span>
            </label>
            <button
              type="button"
              class="danger-button"
              disabled={bootGuardActive || !canConfirmReset}
              title={bootGuardHint || undefined}
              onclick={handleResetConfirmRequest}
            >
              {text.resetSelected}
            </button>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state danger-reset-empty">
            <strong>{text.resetIdleTitle}</strong>
            <span>{text.resetIdleDescription}</span>
          </div>
        {/if}
      </div>
    </section>
  </section>

  <div class="backup-section-divider"></div>

  <section class="backup-section-row">
    <aside class="backup-side-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>{text.systemInfoTitle}</strong>
          <span>{text.systemInfoDescription}</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>{text.status}</dt>
            <dd>{systemStatusLoading ? text.checking : systemStatusError ? text.checkFailed : systemStatus ? text.dataReady : text.waiting}</dd>
          </div>
          <div>
            <dt>{text.flashOccupancy}</dt>
            <dd>{systemStatus ? flashOccupancyPercentText : "-"}</dd>
          </div>
        </dl>
      </div>
    </aside>

    <section class="backup-result-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>{text.systemInfoTitle}</strong>
          <span>{systemStatusLoading ? text.systemCheckingDescription : text.systemSnapshotDescription}</span>
        </div>
        {#if systemStatusError}
          <div class="backup-issues error">
            <strong>{text.systemReadFailed}</strong>
            <ul>
              <li>{systemStatusError}</li>
            </ul>
          </div>
        {:else if systemStatus}
          <div class="backup-system-sections">
            <section class="backup-system-section">
              <strong>{text.systemSection}</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>{text.firmware}</dt>
                  <dd>{firmwareVersion}</dd>
                </div>
                <div>
                  <dt>{text.dashboard}</dt>
                  <dd>{dashboardVersion} · {formatBytes(systemStatus.dashboard?.gzipBytes)}</dd>
                </div>
                <div>
                  <dt>{text.uptime}</dt>
                  <dd>{formatUptime(systemStatus.firmware?.uptimeSeconds)}</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>{text.storageSection}</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>{text.flashOccupancy}</dt>
                  <dd>{text.usedWithPercent(flashOccupancyText, flashOccupancyPercentText)}</dd>
                </div>
                <div>
                  <dt>{text.firmwareSpace}</dt>
                  <dd>{firmwareSpaceText}</dd>
                </div>
                <div>
                  <dt>{text.otaSpace}</dt>
                  <dd>{formatBytes(systemStatus.flash?.otaSlotBytes)}</dd>
                </div>
                <div>
                  <dt>{text.storageData}</dt>
                  <dd>{dataStorageUsedText} · {dataStoragePercentText}</dd>
                </div>
                <div>
                  <dt>{text.floorplan}</dt>
                  <dd>{formatBytes(systemStatus.storage?.floorplanConfigBytes)} / {formatBytes(systemStatus.storage?.floorplanImageBytes)}</dd>
                </div>
                <div>
                  <dt>{text.settingsStats}</dt>
                  <dd>{formatBytes(systemStatus.storage?.deviceConfigBytes)} / {formatBytes(systemStatus.storage?.statsBytes)}</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>RAM</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>{text.internalRam}</dt>
                  <dd>{text.usedWithPercent(internalRamText, internalRamPercentText)}</dd>
                </div>
                <div>
                  <dt>{text.minInternalRamFree}</dt>
                  <dd>{minInternalRamText}</dd>
                </div>
                <div>
                  <dt>{text.externalRam}</dt>
                  <dd>{text.usedWithPercent(externalRamText, externalRamPercentText)}</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>{text.networkSection}</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>Wi-Fi</dt>
                  <dd>
                    <span class="wifi-strength" aria-label={`Wi-Fi ${wifiInfo.label}`}>
                      {#each [0, 1, 2, 3] as index}
                        <span class:active={index < wifiInfo.bars}></span>
                      {/each}
                    </span>
                    {wifiInfo.label}{systemStatus.wifi?.connected ? ` · ${systemStatus.wifi?.rssi ?? "-"}dBm` : ""}
                  </dd>
                </div>
                <div>
                  <dt>Bluetooth</dt>
                  <dd>{systemStatus.bluetooth?.enabled ? text.enabled : text.disabledState}</dd>
                </div>
              </dl>
            </section>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>{text.systemWaitingTitle}</strong>
            <span>{text.systemWaitingDescription}</span>
          </div>
        {/if}
      </div>
    </section>
  </section>
</section>

{#if rebootConfirmOpen}
  <div class="demo-blocker-backdrop" role="presentation">
    <div class="demo-blocker-dialog" role="dialog" aria-modal="true" aria-labelledby="reboot-confirm-title">
      <div>
        <strong id="reboot-confirm-title">{text.rebootConfirmTitle}</strong>
        <span>{text.rebootConfirmDescription}</span>
      </div>
      <div class="demo-blocker-actions">
        <button type="button" disabled={rebootExecuting} onclick={() => (rebootConfirmOpen = false)}>{text.cancel}</button>
        <button type="button" class="danger-button" disabled={rebootExecuting} onclick={executeSystemReboot}>
          {rebootExecuting ? text.rebooting : text.rebootButton}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if resetConfirmOpen}
  <div class="demo-blocker-backdrop" role="presentation">
    <div class="demo-blocker-dialog" role="dialog" aria-modal="true" aria-labelledby="reset-confirm-title">
      <div>
        <strong id="reset-confirm-title">{text.resetConfirmTitle}</strong>
        <span>
          {text.resetConfirmDescription}
        </span>
      </div>
      <div class="demo-blocker-actions">
        <button type="button" disabled={resetExecuting} onclick={() => (resetConfirmOpen = false)}>{text.cancel}</button>
        <button type="button" class="danger-button" disabled={resetExecuting} onclick={executeSystemReset}>
          {resetExecuting ? text.resetting : text.resetConfirmButton}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if demoNoticeOpen}
  <div class="demo-blocker-backdrop" role="presentation">
    <div class="demo-blocker-dialog" role="dialog" aria-modal="true" aria-labelledby="demo-blocker-title">
      <div>
        <strong id="demo-blocker-title">{demoNoticeTitle}</strong>
        <span>{demoNoticeMessage}</span>
      </div>
      <button type="button" class="danger-button" onclick={() => (demoNoticeOpen = false)}>{messages.common.close}</button>
    </div>
  </div>
{/if}
