<script lang="ts">
  import type { BackupIssue, BackupValidationResult } from "../../core/config-backup";
  import type { BackupRestoreProgressStep, BackupRestoreStage } from "../state/useBackupRestore.svelte";
  import type { FirmwareUploadProgress, WebSystemStatus } from "../types";

  type Props = {
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
    issueText: (issue: BackupIssue) => string;
    onExport: () => void;
    onImportFile: (file: File | null) => void;
    onSetImportZones: (enabled: boolean) => void;
    onSetImportFloorplan: (enabled: boolean) => void;
    onSetImportStats: (enabled: boolean) => void;
    onConfirmImport: () => void | Promise<void>;
    onCancelImport: () => void;
    onUploadFirmware?: (file: File, onProgress: (progress: FirmwareUploadProgress) => void) => Promise<void>;
  };

  let {
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
    issueText,
    onExport,
    onImportFile,
    onSetImportZones,
    onSetImportFloorplan,
    onSetImportStats,
    onConfirmImport,
    onCancelImport,
    onUploadFirmware
  }: Props = $props();

  let fileInput: HTMLInputElement | null = $state(null);
  let firmwareFileInput: HTMLInputElement | null = $state(null);
  let firmwareFile: File | null = $state(null);
  let firmwareFileName = $state("");
  let firmwareFileSize = $state(0);
  let firmwareUploadStage = $state<"idle" | "selected" | "uploading" | "success" | "error">("idle");
  let firmwareUploadPercent = $state(0);
  let firmwareUploadLoaded = $state(0);
  let firmwareUploadTotal = $state(0);
  let firmwareUploadMessage = $state("");

  function openFilePicker(): void {
    fileInput?.click();
  }

  function handleFileChange(event: Event): void {
    const input = event.currentTarget as HTMLInputElement;
    onImportFile(input.files?.[0] || null);
    input.value = "";
  }

  function openFirmwareFilePicker(): void {
    if (firmwareUploadStage === "uploading") return;
    firmwareFileInput?.click();
  }

  function handleFirmwareAction(): void {
    if (firmwareUploadStage === "uploading") return;
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
    if (!file) return;

    firmwareFile = null;
    firmwareFileName = file.name;
    firmwareFileSize = file.size;
    firmwareUploadPercent = 0;
    firmwareUploadLoaded = 0;
    firmwareUploadTotal = file.size;

    if (!file.name.toLowerCase().endsWith(".bin")) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = "ESPHome에서 생성한 .bin 펌웨어 파일만 업로드할 수 있습니다.";
      return;
    }

    if (Number.isFinite(systemStatus?.flash?.otaSlotBytes ?? NaN) && file.size > Number(systemStatus?.flash?.otaSlotBytes)) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = `파일이 OTA 예비 공간보다 큽니다. ${formatBytes(file.size)} / ${formatBytes(systemStatus?.flash?.otaSlotBytes)}`;
      return;
    }

    if (!onUploadFirmware) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = "펌웨어 업로드 API가 준비되지 않았습니다.";
      return;
    }

    firmwareFile = file;
    firmwareUploadStage = "selected";
    firmwareUploadMessage = "파일을 확인했습니다. 업로드 버튼을 누르면 업데이트를 시작합니다.";
  }

  async function uploadSelectedFirmware(): Promise<void> {
    if (!firmwareFile || !onUploadFirmware) return;
    const file = firmwareFile;
    firmwareUploadStage = "uploading";
    firmwareUploadMessage = "펌웨어를 업로드하는 중입니다. 업데이트 중에는 전원을 끄지 마세요.";
    try {
      await onUploadFirmware(file, (progress) => {
        firmwareUploadLoaded = progress.loaded;
        firmwareUploadTotal = progress.total;
        firmwareUploadPercent = progress.percent;
      });
      firmwareUploadStage = "success";
      firmwareUploadPercent = 100;
      firmwareUploadLoaded = file.size;
      firmwareUploadTotal = file.size;
      firmwareFile = null;
      firmwareUploadMessage = "업데이트가 완료되었습니다. 장치가 곧 재부팅됩니다.";
    } catch (error) {
      firmwareUploadStage = "error";
      firmwareUploadMessage = error instanceof Error ? error.message : "펌웨어 업데이트에 실패했습니다.";
    }
  }

  function handleExport(): void {
    onExport();
  }

  function handleConfirmImport(): void {
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

  const importing = $derived(stage === "importing");
  const showRestoreProgress = $derived(progressSteps.length > 0 && (stage === "importing" || stage === "imported" || stage === "error"));

  function formatBytes(value: number | null | undefined): string {
    if (!Number.isFinite(value ?? NaN)) return "-";
    const bytes = Math.max(0, Number(value));
    if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)}MB`;
    if (bytes >= 1024) return `${Math.round(bytes / 1024)}KB`;
    return `${Math.round(bytes)}B`;
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
    if (!connected || !Number.isFinite(rssi ?? NaN)) return { bars: 0, label: "연결 안 됨" };
    const value = Number(rssi);
    if (value >= -50) return { bars: 4, label: "매우 좋음" };
    if (value >= -60) return { bars: 3, label: "좋음" };
    if (value >= -70) return { bars: 2, label: "보통" };
    if (value >= -80) return { bars: 1, label: "약함" };
    return { bars: 1, label: "매우 약함" };
  }

  function formatUptime(seconds: number | null | undefined): string {
    if (!Number.isFinite(seconds ?? NaN)) return "-";
    const total = Math.max(0, Math.round(Number(seconds)));
    const days = Math.floor(total / 86400);
    const hours = Math.floor((total % 86400) / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    if (days > 0) return `${days}일 ${hours}시간`;
    if (hours > 0) return `${hours}시간 ${minutes}분`;
    return `${minutes}분`;
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
  const firmwareFileText = $derived(firmwareFileName ? `${firmwareFileName} · ${formatBytes(firmwareFileSize)}` : "선택 안 됨");
  const firmwareActionText = $derived(
    firmwareUploadStage === "uploading" ? "업로드 중" : firmwareUploadStage === "selected" ? "업로드" : "수동으로 업데이트"
  );
  const firmwareUploadProgressText = $derived(
    `${firmwareUploadPercent}% · ${formatBytes(firmwareUploadLoaded)} / ${formatBytes(firmwareUploadTotal || firmwareFileSize)}`
  );
  const firmwareUploadReadyText = $derived(
    systemStatus?.flash?.otaSlotBytes
      ? `업로드할 .bin 파일을 선택하세요. OTA 예비 공간은 ${formatBytes(systemStatus.flash.otaSlotBytes)}입니다.`
      : "업로드할 .bin 파일을 선택하세요. OTA 예비 공간은 시스템 정보 조회 후 확인할 수 있습니다."
  );
</script>

<section class="backup-workspace">
  <section class="backup-section-row">
    <aside class="backup-side-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>백업 / 복원</strong>
          <span>구역 설정, 오탐 보정 구역, 저장된 평면도와 이미지를 파일로 저장하거나 복원합니다.</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>상태</dt>
            <dd>{loaded ? "데이터 확인됨" : "로딩 중"}</dd>
          </div>
          <div>
            <dt>파일</dt>
            <dd>{filename || "선택 안 됨"}</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-stored-tools">
        <button type="button" disabled={!loaded || importing} onclick={handleExport}>백업하기</button>
        <button type="button" disabled={!loaded || importing} onclick={openFilePicker}>복원하기</button>
        <input bind:this={fileInput} type="file" accept="application/json,.json" hidden onchange={handleFileChange} />
      </div>
    </aside>

    <section class="backup-result-panel" aria-live="polite">
      <div class="floorplan-workflow-card">
        {#if message}
          <div class={`floorplan-status backup-message ${stage}`}>
            <strong>{filename || "백업 파일"}</strong>
            <span>{message}</span>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>백업 파일을 만들거나 복원할 파일을 선택하세요.</strong>
            <span>작업 결과와 가져올 데이터 선택 항목이 여기에 표시됩니다.</span>
          </div>
        {/if}

        {#if summary && (stage === "ready" || stage === "warning" || stage === "imported" || stage === "exported")}
          <dl class="backup-summary floorplan-stored-summary">
            <div>
              <dt>구역</dt>
              <dd>{summary.softwareZones}</dd>
            </div>
            <div>
              <dt>오탐 보정</dt>
              <dd>{summary.calibrationZones}</dd>
            </div>
            <div>
              <dt>필터/둔감/제외</dt>
              <dd>{summary.filterZones}/{summary.reducedZones}/{summary.disabledZones}</dd>
            </div>
            <div>
              <dt>평면도</dt>
              <dd>{summary.hasFloorplan ? `포함${summary.floorplanImageBytes ? ` · ${Math.round(summary.floorplanImageBytes / 1024)}KB` : ""}` : "없음"}</dd>
            </div>
            <div>
              <dt>통계</dt>
              <dd>
                {summary.hasStats
                  ? `포함 · 일별 ${summary.statsDailyDays}일${summary.hasHeatmap ? " · 히트맵" : ""}`
                  : "없음"}
              </dd>
            </div>
          </dl>
        {/if}

        {#if showRestoreProgress}
          <div class="backup-restore-progress floorplan-edit-tool-card" data-stage={stage}>
            <div class="backup-restore-progress-header">
              <strong>복원 진행</strong>
              <span>{progressPercent}%</span>
            </div>
            <div class="firmware-progress-track" aria-label="백업 복원 진행률">
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
            <strong>가져올 데이터</strong>
            <label>
              <input type="checkbox" checked={importZones} disabled={importing} onchange={handleImportZonesChange} />
              <span>구역 설정과 오탐 보정 구역</span>
            </label>
            <label class:disabled={!canImportFloorplan}>
              <input
                type="checkbox"
                checked={importFloorplan}
                disabled={!canImportFloorplan || importing}
                onchange={handleImportFloorplanChange}
              />
              <span>평면도와 이미지</span>
            </label>
            <label class:disabled={!canImportStats}>
              <input
                type="checkbox"
                checked={importStats}
                disabled={!canImportStats || importing}
                onchange={handleImportStatsChange}
              />
              <span>통계 데이터와 히트맵</span>
            </label>
          </div>
        {/if}

        {#if errors.length > 0}
          <div class="backup-issues error">
            <strong>가져올 수 없는 이유</strong>
            <ul>
              {#each errors as error}
                <li>{issueText(error)}</li>
              {/each}
            </ul>
          </div>
        {/if}

        {#if warnings.length > 0}
          <div class="backup-issues warn">
            <strong>확인이 필요한 항목</strong>
            <ul>
              {#each warnings as warning}
                <li>{issueText(warning)}</li>
              {/each}
            </ul>
          </div>
        {/if}

        {#if stage === "ready" || stage === "warning" || stage === "importing"}
          <div class="floorplan-edit-tool-card backup-confirm">
            <p>가져오기를 진행하면 선택한 데이터만 현재 장치 설정에 덮어씁니다.</p>
            <div>
              <button type="button" class="danger-button" disabled={!canConfirmImport || importing} onclick={handleConfirmImport}>
                {importing ? "가져오는 중" : "선택한 데이터 가져오기"}
              </button>
              <button type="button" disabled={importing} onclick={handleCancelImport}>취소</button>
            </div>
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
          <strong>펌웨어 업데이트</strong>
          <span>장치 펌웨어를 업데이트합니다. 자동 업데이트는 배포 방식 확정 후 연결합니다.</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>펌웨어</dt>
            <dd>{systemStatusLoading ? "확인 중" : firmwareVersion}</dd>
          </div>
          <div>
            <dt>파일</dt>
            <dd>{firmwareFileText}</dd>
          </div>
          <div>
            <dt>상태</dt>
            <dd>{firmwareUploadStage === "uploading" ? "업로드 중" : firmwareUploadStage === "selected" ? "업로드 대기" : firmwareUploadStage === "success" ? "완료" : firmwareUploadStage === "error" ? "오류" : "대기 중"}</dd>
          </div>
        </dl>
      </div>

      <div class="floorplan-stored-tools">
        <button type="button" disabled>펌웨어 업데이트</button>
        <button
          type="button"
          class:danger-button={firmwareUploadStage === "selected"}
          disabled={firmwareUploadStage === "uploading"}
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
          <strong>펌웨어 업데이트</strong>
          <span>ESPHome에서 생성한 펌웨어 파일을 장치에 직접 업로드합니다.</span>
        </div>
        {#if firmwareUploadStage === "uploading"}
          <div class="firmware-update-status active">
            <strong>펌웨어를 업로드하는 중입니다</strong>
            <span>{firmwareUploadProgressText}</span>
            <div class="firmware-progress-track" aria-label="펌웨어 업로드 진행률">
              <i style={`width: ${firmwareUploadPercent}%`}></i>
            </div>
            <small>업데이트 중에는 전원을 끄거나 브라우저를 닫지 마세요.</small>
          </div>
        {:else if firmwareUploadStage === "success"}
          <div class="firmware-update-status success">
            <strong>업데이트 완료</strong>
            <span>{firmwareUploadMessage}</span>
            <small>재부팅 중에는 대시보드 연결이 잠시 끊길 수 있습니다.</small>
          </div>
        {:else if firmwareUploadStage === "error"}
          <div class="firmware-update-status error">
            <strong>업데이트 실패</strong>
            <span>{firmwareUploadMessage}</span>
            <small>파일과 OTA 예비 공간을 확인한 뒤 다시 시도하세요.</small>
          </div>
        {:else if firmwareUploadStage === "selected"}
          <div class="firmware-update-status active">
            <strong>업로드 준비됨</strong>
            <span>{firmwareUploadMessage}</span>
            <small>{firmwareFileText} · 업로드를 누르면 장치 업데이트가 시작됩니다.</small>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>수동 업데이트 대기 중</strong>
            <span>{firmwareUploadReadyText}</span>
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
          <strong>시스템 정보</strong>
          <span>ESP32의 메모리, 저장소, 무선 연결 상태를 확인합니다.</span>
        </div>
        <dl class="floorplan-stored-summary">
          <div>
            <dt>상태</dt>
            <dd>{systemStatusLoading ? "확인 중" : systemStatusError ? "확인 실패" : systemStatus ? "데이터 확인됨" : "대기 중"}</dd>
          </div>
          <div>
            <dt>저장 데이터</dt>
            <dd>{systemStatus ? dataStoragePercentText : "-"}</dd>
          </div>
        </dl>
      </div>
    </aside>

    <section class="backup-result-panel">
      <div class="floorplan-workflow-card">
        <div>
          <strong>시스템 정보</strong>
          <span>{systemStatusLoading ? "장치 상태를 확인하는 중입니다." : "탭 진입 시 한 번 조회한 값입니다."}</span>
        </div>
        {#if systemStatusError}
          <div class="backup-issues error">
            <strong>시스템 정보를 읽지 못했습니다</strong>
            <ul>
              <li>{systemStatusError}</li>
            </ul>
          </div>
        {:else if systemStatus}
          <div class="backup-system-sections">
            <section class="backup-system-section">
              <strong>시스템</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>펌웨어</dt>
                  <dd>{firmwareVersion}</dd>
                </div>
                <div>
                  <dt>대시보드</dt>
                  <dd>{dashboardVersion} · {formatBytes(systemStatus.dashboard?.gzipBytes)}</dd>
                </div>
                <div>
                  <dt>가동 시간</dt>
                  <dd>{formatUptime(systemStatus.firmware?.uptimeSeconds)}</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>저장 공간</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>Flash 점유량</dt>
                  <dd>{flashOccupancyText} · {flashOccupancyPercentText} 사용 중</dd>
                </div>
                <div>
                  <dt>펌웨어 공간</dt>
                  <dd>{firmwareSpaceText}</dd>
                </div>
                <div>
                  <dt>OTA 예비 공간</dt>
                  <dd>{formatBytes(systemStatus.flash?.otaSlotBytes)}</dd>
                </div>
                <div>
                  <dt>저장 데이터</dt>
                  <dd>{dataStorageUsedText} · {dataStoragePercentText}</dd>
                </div>
                <div>
                  <dt>평면도</dt>
                  <dd>{formatBytes(systemStatus.storage?.floorplanConfigBytes)} / {formatBytes(systemStatus.storage?.floorplanImageBytes)}</dd>
                </div>
                <div>
                  <dt>설정/통계</dt>
                  <dd>{formatBytes(systemStatus.storage?.deviceConfigBytes)} / {formatBytes(systemStatus.storage?.statsBytes)}</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>RAM</strong>
              <dl class="backup-system-list floorplan-stored-summary">
                <div>
                  <dt>내부 RAM</dt>
                  <dd>{internalRamText} · {internalRamPercentText} 사용 중</dd>
                </div>
                <div>
                  <dt>최저 내부 RAM 여유</dt>
                  <dd>{minInternalRamText}</dd>
                </div>
                <div>
                  <dt>외장 RAM</dt>
                  <dd>{externalRamText} · {externalRamPercentText} 사용 중</dd>
                </div>
              </dl>
            </section>

            <section class="backup-system-section">
              <strong>네트워크</strong>
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
                  <dd>{systemStatus.bluetooth?.enabled ? "활성" : "비활성"}</dd>
                </div>
              </dl>
            </section>
          </div>
        {:else}
          <div class="floorplan-empty-note backup-empty-state">
            <strong>시스템 정보 대기 중</strong>
            <span>관리 / 백업 탭에 진입하면 장치 상태를 한 번 확인합니다.</span>
          </div>
        {/if}
      </div>
    </section>
  </section>
</section>
