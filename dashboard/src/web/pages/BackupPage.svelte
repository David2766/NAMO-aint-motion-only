<script lang="ts">
  import type { BackupIssue, BackupSummary } from "../../core/config-backup";
  import BackupPanel from "../components/BackupPanel.svelte";
  import type { Messages } from "../i18n/types";
  import type {
    FirmwareUploadProgress,
    WebApiKeyResult,
    WebFirmwareTrustStatus,
    WebSystemRebootResult,
    WebSystemResetOptions,
    WebSystemResetResult,
    WebSystemStatus
  } from "../types";

  type Props = {
    messages: Messages;
    loaded: boolean;
    stage: string;
    filename: string;
    message: string;
    errors: BackupIssue[];
    warnings: BackupIssue[];
    summary: BackupSummary | null;
    progressSteps: Array<{ label: string; status: string; detail: string }>;
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
    bootGuardRemainingSeconds: number;
    demoMode: boolean;
    issueText: (issue: BackupIssue) => string;
    onExport: () => void | Promise<void>;
    onImportFile: (file: File) => void | Promise<void>;
    onSetImportZones: (enabled: boolean) => void;
    onSetImportFloorplan: (enabled: boolean) => void;
    onSetImportStats: (enabled: boolean) => void;
    onConfirmImport: () => void | Promise<void>;
    onCancelImport: () => void;
    onGetSystemStatus?: () => Promise<WebSystemStatus>;
    onGetFirmwareTrust?: () => Promise<WebFirmwareTrustStatus>;
    onSetAlphaFirmwareInstallAllowed?: (enabled: boolean) => Promise<WebFirmwareTrustStatus>;
    onUploadFirmware?: (file: File, onProgress?: (progress: FirmwareUploadProgress) => void) => Promise<void>;
    onGetApiKey?: () => Promise<WebApiKeyResult>;
    onRotateApiKey?: () => Promise<WebApiKeyResult>;
    onResetSystem?: (settings: WebSystemResetOptions) => Promise<WebSystemResetResult>;
    onRebootSystem?: () => Promise<WebSystemRebootResult>;
  };

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
    bootGuardRemainingSeconds,
    demoMode,
    issueText,
    onExport,
    onImportFile,
    onSetImportZones,
    onSetImportFloorplan,
    onSetImportStats,
    onConfirmImport,
    onCancelImport,
    onGetSystemStatus,
    onGetFirmwareTrust,
    onSetAlphaFirmwareInstallAllowed,
    onUploadFirmware,
    onGetApiKey,
    onRotateApiKey,
    onResetSystem,
    onRebootSystem
  }: Props = $props();
</script>

<section class="tab-page">
  <BackupPanel
    {messages}
    {loaded}
    {stage}
    {filename}
    {message}
    {errors}
    {warnings}
    {summary}
    {progressSteps}
    {progressPercent}
    {importZones}
    {importFloorplan}
    {importStats}
    {canImportFloorplan}
    {canImportStats}
    {canConfirmImport}
    {systemStatus}
    {systemStatusLoading}
    {systemStatusError}
    {bootGuardRemainingSeconds}
    {demoMode}
    {issueText}
    {onExport}
    {onImportFile}
    {onSetImportZones}
    {onSetImportFloorplan}
    {onSetImportStats}
    {onConfirmImport}
    {onCancelImport}
    {onGetSystemStatus}
    {onGetFirmwareTrust}
    {onSetAlphaFirmwareInstallAllowed}
    {onUploadFirmware}
    {onGetApiKey}
    {onRotateApiKey}
    {onResetSystem}
    {onRebootSystem}
  />
</section>
