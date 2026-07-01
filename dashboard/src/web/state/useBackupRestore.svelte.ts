import {
  backupValidationMessage,
  createConfigBackup,
  validateConfigBackupText,
  type BackupFloorplanData,
  type BackupDeviceInfo,
  type BackupIssue,
  type BackupValidationResult
} from "../../core/config-backup";
import type { WebDeviceConfig, WebDeviceStats } from "../types";

export type BackupRestoreStage = "idle" | "blocked" | "warning" | "ready" | "importing" | "imported" | "exported" | "error";
export type BackupRestoreProgressStepId = "config" | "floorplan" | "stats";
export type BackupRestoreProgressStepStatus = "pending" | "active" | "done" | "error";

export interface BackupRestoreProgressStep {
  id: BackupRestoreProgressStepId;
  label: string;
  status: BackupRestoreProgressStepStatus;
  detail: string;
  percent: number;
}

interface BackupRestoreOptions {
  getConfig: () => WebDeviceConfig | null;
  applyConfig: (config: WebDeviceConfig) => Promise<void>;
  loadFloorplanBackup?: () => Promise<BackupFloorplanData | null>;
  applyFloorplanBackup?: (floorplan: BackupFloorplanData) => Promise<void>;
  loadStatsBackup?: () => Promise<WebDeviceStats | null>;
  applyStatsBackup?: (stats: WebDeviceStats, onProgress?: (progress: { loaded: number; total: number; percent: number }) => void) => Promise<void>;
  getDeviceInfo: () => BackupDeviceInfo;
  setStatus: (message: string, tone: "ok" | "warn" | "error") => void;
  errorMessage: (error: unknown) => string;
}

export function createBackupRestore({
  getConfig,
  applyConfig,
  loadFloorplanBackup,
  applyFloorplanBackup,
  loadStatsBackup,
  applyStatsBackup,
  getDeviceInfo,
  setStatus,
  errorMessage
}: BackupRestoreOptions) {
  let stage = $state<BackupRestoreStage>("idle");
  let filename = $state("");
  let message = $state("");
  let errors = $state<BackupIssue[]>([]);
  let warnings = $state<BackupIssue[]>([]);
  let summary = $state<BackupValidationResult["summary"] | null>(null);
  let pendingConfig = $state<WebDeviceConfig | null>(null);
  let pendingFloorplan = $state<BackupFloorplanData | null>(null);
  let pendingStats = $state<WebDeviceStats | null>(null);
  let importZones = $state(true);
  let importFloorplan = $state(false);
  let importStats = $state(false);
  let progressSteps = $state<BackupRestoreProgressStep[]>([]);
  let progressPercent = $state(0);
  let currentProgressStep = $state<BackupRestoreProgressStepId | "import">("import");

  async function exportBackup(): Promise<void> {
    const config = getConfig();
    if (!config) {
      setStatus("내보낼 설정이 아직 준비되지 않았습니다.", "warn");
      return;
    }

    try {
      const floorplan = loadFloorplanBackup ? await loadFloorplanBackup() : null;
      const stats = loadStatsBackup ? await loadStatsBackup() : null;
      const backup = await createConfigBackup(config, getDeviceInfo(), floorplan ?? undefined, stats ?? undefined);
      const backupText = `${JSON.stringify(backup, null, 2)}\n`;
      const blob = new Blob([backupText], { type: "application/json" });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = `radar-zone-backup-${new Date().toISOString().slice(0, 10)}.json`;
      document.body.append(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
      stage = "exported";
      filename = link.download;
      message = "백업 파일을 내보냈습니다.";
      errors = [];
      warnings = [];
      summary = summarizeBackupConfig(config, floorplan, stats);
      pendingConfig = null;
      pendingFloorplan = null;
      pendingStats = null;
      importZones = true;
      importFloorplan = Boolean(floorplan);
      importStats = Boolean(stats);
      setStatus("백업 파일을 만들었습니다.", "ok");
    } catch (error) {
      stage = "error";
      filename = "";
      message = `백업을 만들지 못했습니다. ${errorMessage(error)}`;
      errors = [{ path: "export", message }];
      warnings = [];
      summary = null;
      setStatus(message, "error");
    }
  }

  async function readImportFile(file: File | null): Promise<void> {
    resetImportState();
    if (!file) return;

    filename = file.name;
    if (!file.name.toLowerCase().endsWith(".json")) {
      stage = "blocked";
      message = "JSON 파일만 가져올 수 있습니다.";
      errors = [
        {
          path: "$file",
          message: "선택한 파일의 확장자가 .json이 아닙니다.",
          detail: file.name
        }
      ];
      setStatus(message, "error");
      return;
    }

    try {
      const text = await file.text();
      const result = await validateConfigBackupText(text);
      errors = result.errors;
      warnings = result.warnings;
      summary = result.summary;
      pendingConfig = result.config;
      pendingFloorplan = result.floorplan;
      pendingStats = result.stats;
      importZones = Boolean(pendingConfig);
      importFloorplan = Boolean(pendingFloorplan && applyFloorplanBackup);
      importStats = false;

      if (errors.length > 0 || !pendingConfig) {
        stage = "blocked";
        message = "백업 파일을 가져올 수 없습니다. 아래 항목을 확인해 주세요.";
        setStatus("백업 검증 실패", "error");
        return;
      }

      if (warnings.length > 0) {
        stage = "warning";
        message = "백업 파일에 확인이 필요한 경고가 있습니다.";
        setStatus("백업 경고 확인 필요", "warn");
        return;
      }

      stage = "ready";
      message = "백업 파일 검증이 완료되었습니다. 현재 설정을 덮어쓸 수 있습니다.";
      setStatus("백업 검증 완료", "warn");
    } catch (error) {
      stage = "error";
      message = `백업 파일을 읽지 못했습니다. ${errorMessage(error)}`;
      errors = [{ path: "$file", message }];
      setStatus(message, "error");
    }
  }

  async function confirmImport(): Promise<void> {
    if (!pendingConfig || (!importZones && !importFloorplan && !importStats)) return;
    try {
      stage = "importing";
      errors = [];
      warnings = [];
      message = "백업 복원을 준비하고 있습니다.";
      currentProgressStep = "import";
      initializeProgressSteps();
      setStatus("백업 복원 중", "warn");

      if (importZones) {
        message = "구역 설정과 오탐 보정 구역을 장치에 저장하는 중입니다.";
        activateProgressStep("config", message);
        await applyConfig(pendingConfig);
        completeProgressStep("config", "설정 저장 완료");
      }
      if (importFloorplan && pendingFloorplan && applyFloorplanBackup) {
        message = pendingFloorplan.image
          ? "평면도 설정과 이미지를 장치에 복원하는 중입니다."
          : "평면도 설정을 장치에 복원하는 중입니다.";
        activateProgressStep("floorplan", message);
        await applyFloorplanBackup(pendingFloorplan);
        completeProgressStep("floorplan", "평면도 복원 완료");
      }
      if (importStats && pendingStats && applyStatsBackup) {
        message = "통계 데이터와 히트맵 기록을 장치에 복원하는 중입니다.";
        activateProgressStep("stats", message);
        await applyStatsBackup(pendingStats, (progress) => {
          updateProgressStep("stats", "active", `업로드 중 ${progress.percent}%`, progress.percent);
          updateOverallProgress("stats", progress.percent);
        });
        completeProgressStep("stats", "통계 복원 완료");
      }
      stage = "imported";
      message =
        [importZones ? "설정" : "", importFloorplan ? "평면도" : "", importStats ? "통계" : ""]
          .filter(Boolean)
          .join(", ") + " 데이터를 가져왔습니다.";
      errors = [];
      warnings = [];
      pendingConfig = null;
      pendingFloorplan = null;
      pendingStats = null;
      importZones = true;
      importFloorplan = false;
      importStats = false;
      setStatus("백업 복원 완료", "ok");
    } catch (error) {
      stage = "error";
      message = `백업 복원 중 오류가 발생했습니다. ${errorMessage(error)}`;
      if (currentProgressStep !== "import") {
        updateProgressStep(currentProgressStep, "error", message, progressSteps.find((step) => step.id === currentProgressStep)?.percent ?? 0);
      }
      errors = [{ path: currentProgressStep, message }];
      setStatus(message, "error");
    }
  }

  function cancelImport(): void {
    resetImportState();
    setStatus("백업 가져오기를 취소했습니다.", "warn");
  }

  function resetImportState(): void {
    stage = "idle";
    filename = "";
    message = "";
    errors = [];
    warnings = [];
    summary = null;
    pendingConfig = null;
    pendingFloorplan = null;
    pendingStats = null;
    importZones = true;
    importFloorplan = false;
    importStats = false;
    progressSteps = [];
    progressPercent = 0;
    currentProgressStep = "import";
  }

  function setImportZones(enabled: boolean): void {
    importZones = enabled;
  }

  function setImportFloorplan(enabled: boolean): void {
    importFloorplan = enabled && Boolean(pendingFloorplan && applyFloorplanBackup);
  }

  function setImportStats(enabled: boolean): void {
    importStats = enabled && Boolean(pendingStats && applyStatsBackup);
  }

  function issueText(issue: BackupIssue): string {
    return backupValidationMessage(issue);
  }

  function initializeProgressSteps(): void {
    progressSteps = [
      importZones
        ? {
            id: "config",
            label: "설정",
            status: "pending",
            detail: "대기 중",
            percent: 0
          }
        : null,
      importFloorplan
        ? {
            id: "floorplan",
            label: "평면도",
            status: "pending",
            detail: "대기 중",
            percent: 0
          }
        : null,
      importStats
        ? {
            id: "stats",
            label: "통계",
            status: "pending",
            detail: "대기 중",
            percent: 0
          }
        : null
    ].filter((step): step is BackupRestoreProgressStep => Boolean(step));
    progressPercent = 0;
  }

  function activateProgressStep(id: BackupRestoreProgressStepId, detail: string): void {
    currentProgressStep = id;
    updateProgressStep(id, "active", detail, 0);
    updateOverallProgress(id, 0);
  }

  function completeProgressStep(id: BackupRestoreProgressStepId, detail: string): void {
    updateProgressStep(id, "done", detail, 100);
    updateOverallProgress(id, 100);
  }

  function updateProgressStep(
    id: BackupRestoreProgressStepId,
    status: BackupRestoreProgressStepStatus,
    detail: string,
    percent: number
  ): void {
    progressSteps = progressSteps.map((step) =>
      step.id === id
        ? {
            ...step,
            status,
            detail,
            percent: Math.min(100, Math.max(0, Math.round(percent)))
          }
        : step
    );
  }

  function updateOverallProgress(id: BackupRestoreProgressStepId, stepPercent: number): void {
    const index = progressSteps.findIndex((step) => step.id === id);
    if (index < 0 || progressSteps.length === 0) return;
    progressPercent = Math.min(
      100,
      Math.max(0, Math.round(((index + Math.min(100, Math.max(0, stepPercent)) / 100) / progressSteps.length) * 100))
    );
  }

  return {
    get stage() {
      return stage;
    },
    get filename() {
      return filename;
    },
    get message() {
      return message;
    },
    get errors() {
      return errors;
    },
    get warnings() {
      return warnings;
    },
    get summary() {
      return summary;
    },
    get importZones() {
      return importZones;
    },
    get importFloorplan() {
      return importFloorplan;
    },
    get importStats() {
      return importStats;
    },
    get canImportFloorplan() {
      return Boolean(pendingFloorplan && applyFloorplanBackup);
    },
    get canImportStats() {
      return Boolean(pendingStats && applyStatsBackup);
    },
    get canConfirmImport() {
      return Boolean(pendingConfig) && (stage === "ready" || stage === "warning") && (importZones || importFloorplan || importStats);
    },
    get importing() {
      return stage === "importing";
    },
    get progressSteps() {
      return progressSteps;
    },
    get progressPercent() {
      return progressPercent;
    },
    exportBackup,
    readImportFile,
    confirmImport,
    cancelImport,
    setImportZones,
    setImportFloorplan,
    setImportStats,
    issueText
  };
}

function summarizeBackupConfig(
  config: WebDeviceConfig,
  floorplan: BackupFloorplanData | null,
  stats: WebDeviceStats | null
): BackupValidationResult["summary"] {
  const allZones = [...config.zones, ...(config.calibrationZones || [])];
  return {
    softwareZones: config.zones.length,
    calibrationZones: config.calibrationZones?.length || 0,
    filterZones: allZones.filter((zone) => zone.type === "filter").length,
    reducedZones: allZones.filter((zone) => zone.type === "reduced").length,
    disabledZones: allZones.filter((zone) => zone.type === "disabled").length,
    hasFloorplan: Boolean(floorplan?.document),
    floorplanImageBytes: floorplan?.image?.bytes ?? 0,
    hasStats: Boolean(stats),
    statsDailyDays: stats?.daily?.filter(Boolean).length ?? 0,
    hasHeatmap: Boolean(stats?.heatmap)
  };
}
