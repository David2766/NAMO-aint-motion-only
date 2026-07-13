import {
  CALIBRATION_MAX_MS,
  CALIBRATION_MIN_MS,
  CALIBRATION_MIN_SAMPLES,
  CALIBRATION_SCORE_THRESHOLD,
  MAX_CALIBRATION_ZONES
} from "../../core/constants";
import {
  calibrationMetrics,
  calibrationZoneFromSamples,
  type CalibrationMetrics,
  type CalibrationRun
} from "../../core/calibration";
import { clamp } from "../../core/geometry";
import type { Messages } from "../i18n/types";
import type { CalibrationResult, WebDeviceConfig, WebDeviceState } from "../types";

type StatusTone = "ok" | "warn" | "error";

type CalibrationStatusCode =
  | "calibration_start_requested"
  | "calibration_pir_passed"
  | "calibration_sample_collection_started"
  | "calibration_started"
  | "calibration_pir_active"
  | "calibration_no_target"
  | "calibration_multiple_targets"
  | "calibration_max_zones_reached"
  | "calibration_multiple_targets_cancelled"
  | "calibration_timeout"
  | "calibration_candidate_failed"
  | "calibration_score_accepted"
  | "calibration_saved"
  | "calibration_running"
  | "calibration_waiting"
  | "calibration_waiting_for_target_sample"
  | "calibration_collecting_samples"
  | "calibration_analyzing_stability"
  | "calibration_ready_to_create";

interface CalibrationStatusDetail {
  acceptedBy?: string;
  elapsedSeconds?: number;
  maxSeconds?: number;
  score?: number;
  zoneId?: string;
}

interface CalibrationRunOptions {
  getConfig: () => WebDeviceConfig | null;
  getState: () => WebDeviceState | null;
  getCalibrationZoneCount: () => number;
  getMessages: () => Messages;
  updateConfig: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
  selectZone: (zoneId: string) => void;
  setStatus: (message: string, tone: StatusTone) => void;
}

export function createCalibrationRun({
  getConfig,
  getState,
  getCalibrationZoneCount,
  getMessages,
  updateConfig,
  selectZone,
  setStatus
}: CalibrationRunOptions) {
  let dialogOpen = $state(false);
  let run = $state<CalibrationRun | null>(null);
  let result = $state<CalibrationResult | null>(null);
  let logs = $state<string[]>([]);

  const metrics = $derived(run ? calibrationMetrics(run.samples) : result?.metrics);
  const progress = $derived(calibrationProgress(metrics));
  const dialogLogs = $derived(result?.logs ?? logs);

  function start(): void {
    const config = getConfig();
    const state = getState();
    if (!config || !state) return;
    dialogOpen = true;
    logs = [];
    result = null;
    addStatusLog("calibration_start_requested");

    if (state.pirMotion) {
      finishWithError("calibration_pir_active");
      return;
    }

    const activeTargets = state.targets.filter((target) => target.active);
    if (activeTargets.length === 0) {
      finishWithError("calibration_no_target");
      return;
    }
    if (activeTargets.length > 1) {
      finishWithError("calibration_multiple_targets");
      return;
    }
    if ((config.calibrationZones || []).length >= MAX_CALIBRATION_ZONES) {
      finishWithError("calibration_max_zones_reached");
      return;
    }

    run = { startedAt: Date.now(), samples: [] };
    addStatusLog("calibration_pir_passed");
    addStatusLog("calibration_sample_collection_started");
    setCalibrationStatus("calibration_started", "warn");
  }

  function stop(reason: string, tone: "warn" | "error"): void {
    const currentMetrics = run ? calibrationMetrics(run.samples) : undefined;
    addLog(reason);
    run = null;
    result = {
      title: tone === "error" ? statusMessages().failureTitle : statusMessages().stoppedTitle,
      tone,
      createdCount: 0,
      reason,
      metrics: currentMetrics && currentMetrics.samples > 0 ? currentMetrics : undefined,
      logs: [...logs]
    };
    setStatus(reason, tone);
  }

  function update(): void {
    const config = getConfig();
    const state = getState();
    if (!run || !state || !config) return;
    if (state.pirMotion) {
      stop(statusMessages().pirCancelled, "error");
      return;
    }

    const activeTargets = state.targets.filter((target) => target.active);
    if (activeTargets.length === 1) {
      const target = activeTargets[0];
      const last = run.samples[run.samples.length - 1];
      const speed = last ? Math.hypot(target.x - last.x, target.y - last.y) : 0;
      const samples = [...run.samples, { x: target.x, y: target.y, speed }];
      run = { ...run, samples };
      if (samples.length === 1 || samples.length % 10 === 0) {
        addLog(`samples=${samples.length}`);
      }
    } else if (activeTargets.length > 1) {
      stop(statusMessage("calibration_multiple_targets_cancelled"), "error");
      return;
    }

    const elapsed = Date.now() - run.startedAt;
    const currentMetrics = calibrationMetrics(run.samples);
    if (
      elapsed >= CALIBRATION_MIN_MS &&
      run.samples.length >= CALIBRATION_MIN_SAMPLES &&
      currentMetrics.acceptedBy !== "none"
    ) {
      addStatusLog("calibration_score_accepted", { acceptedBy: currentMetrics.acceptedBy });
      apply(currentMetrics);
      return;
    }
    if (elapsed >= CALIBRATION_MAX_MS) {
      stop(statusMessage("calibration_timeout"), "error");
    }
  }

  function apply(currentMetrics: CalibrationMetrics): void {
    const config = getConfig();
    if (!config || !run) return;
    const zone = calibrationZoneFromSamples(
      run.samples,
      config.calibrationZones || [],
      getMessages().zones.calibrationZoneLabel
    );
    run = null;
    if (!zone) {
      addLog(statusMessages().candidateCreateFailedLog);
      result = {
        title: statusMessages().failureTitle,
        tone: "error",
        createdCount: 0,
        reason: statusMessage("calibration_candidate_failed"),
        metrics: currentMetrics,
        logs: [...logs]
      };
      setCalibrationStatus("calibration_candidate_failed", "error");
      return;
    }

    updateConfig((current) => ({
      ...current,
      calibrationZones: [...(current.calibrationZones || []), zone]
    }));
    result = {
      title: statusMessages().successTitle,
      tone: "ok",
      createdCount: 1,
      reason:
        currentMetrics.acceptedBy === "score"
          ? statusMessages().successByScore
          : statusMessages().successByRepeat,
      metrics: currentMetrics,
      logs: [...logs, statusMessages().zoneCreated(zone.id)]
    };
    selectZone(zone.id);
    setCalibrationStatus("calibration_saved", "ok", { score: Math.round(currentMetrics.score), zoneId: zone.id });
  }

  function finishWithError(code: CalibrationStatusCode, detail: CalibrationStatusDetail = {}): void {
    const reason = statusMessage(code, detail);
    addLog(reason);
    run = null;
    result = {
      title: statusMessages().failureTitle,
      tone: "error",
      createdCount: 0,
      reason,
      logs: [...logs]
    };
    setStatus(reason, "error");
  }

  function setCalibrationStatus(code: CalibrationStatusCode, tone: StatusTone, detail: CalibrationStatusDetail = {}): void {
    setStatus(statusMessage(code, detail), tone);
  }

  function addStatusLog(code: CalibrationStatusCode, detail: CalibrationStatusDetail = {}): void {
    addLog(statusMessage(code, detail));
  }

  function addLog(message: string): void {
    const time = new Date().toLocaleTimeString();
    logs = [...logs, `[${time}] ${message}`].slice(-40);
  }

  function statusMessages() {
    return getMessages().zones.calibrationStatus;
  }

  function statusMessage(code: CalibrationStatusCode, detail: CalibrationStatusDetail = {}): string {
    const text = statusMessages();
    switch (code) {
      case "calibration_start_requested":
        return text.startRequested;
      case "calibration_pir_passed":
        return text.pirPassed;
      case "calibration_sample_collection_started":
        return text.sampleCollectionStarted;
      case "calibration_started":
        return text.started;
      case "calibration_pir_active":
        return getMessages().zones.calibrationPirActiveCannotStart;
      case "calibration_no_target":
        return text.noTarget;
      case "calibration_multiple_targets":
        return text.multipleTargets;
      case "calibration_max_zones_reached":
        return getMessages().zones.calibrationMaxZonesReached(MAX_CALIBRATION_ZONES);
      case "calibration_multiple_targets_cancelled":
        return text.multipleTargetsCancelled;
      case "calibration_timeout":
        return text.timeout;
      case "calibration_candidate_failed":
        return text.candidateCreateFailed;
      case "calibration_score_accepted":
        return text.scoreAccepted(detail.acceptedBy ?? "none");
      case "calibration_saved":
        return text.saved(detail.score ?? 0);
      case "calibration_running":
        return text.running(detail.elapsedSeconds ?? 0, detail.maxSeconds ?? Math.ceil(CALIBRATION_MAX_MS / 1000));
      case "calibration_waiting":
        return text.waiting;
      case "calibration_waiting_for_target_sample":
        return text.waitingForTargetSample;
      case "calibration_collecting_samples":
        return text.collectingSamples;
      case "calibration_analyzing_stability":
        return text.analyzingStability;
      case "calibration_ready_to_create":
        return text.readyToCreate;
    }
  }

  function statusText(): string {
    const config = getConfig();
    const state = getState();
    const calibrationZoneCount = getCalibrationZoneCount();
    if (!config) return getMessages().zones.calibrationUnavailableNoDevice;
    if (run) {
      const elapsed = Math.floor((Date.now() - run.startedAt) / 1000);
      return statusMessage("calibration_running", {
        elapsedSeconds: elapsed,
        maxSeconds: Math.ceil(CALIBRATION_MAX_MS / 1000)
      });
    }
    if (state?.pirMotion) return statusMessage("calibration_pir_active");
    if (calibrationZoneCount >= MAX_CALIBRATION_ZONES) {
      return getMessages().zones.calibrationMaxZonesReached(MAX_CALIBRATION_ZONES);
    }
    return getMessages().zones.calibrationSavedCount(calibrationZoneCount, MAX_CALIBRATION_ZONES);
  }

  function progressText(currentMetrics: CalibrationMetrics | undefined): string {
    if (result) return result.title;
    if (!run) return statusMessage("calibration_waiting");
    if (!currentMetrics || currentMetrics.samples === 0) return statusMessage("calibration_waiting_for_target_sample");
    if (currentMetrics.samples < CALIBRATION_MIN_SAMPLES) return statusMessage("calibration_collecting_samples");
    if (currentMetrics.acceptedBy === "none") return statusMessage("calibration_analyzing_stability");
    return statusMessage("calibration_ready_to_create");
  }

  function workItems(currentMetrics: CalibrationMetrics | undefined): string[] {
    const state = getState();
    const text = statusMessages();
    if (!run && !result) return [text.waitingWork];
    const elapsed = run ? Math.floor((Date.now() - run.startedAt) / 1000) : null;
    return [
      text.pirState(Boolean(state?.pirMotion)),
      elapsed === null
        ? text.collectionFinished
        : text.collectionTime(elapsed, Math.ceil(CALIBRATION_MIN_MS / 1000)),
      text.samples(currentMetrics?.samples ?? 0, CALIBRATION_MIN_SAMPLES),
      text.usedSamples(currentMetrics?.usedSamples ?? 0),
      text.outliers(currentMetrics?.outliers ?? 0),
      text.score(Math.round(currentMetrics?.score ?? 0), CALIBRATION_SCORE_THRESHOLD),
      text.acceptedBy(currentMetrics?.acceptedBy ?? "none")
    ];
  }

  function metricsLines(currentMetrics: CalibrationMetrics): string[] {
    return [
      `samples=${currentMetrics.samples}`,
      `usedSamples=${currentMetrics.usedSamples}`,
      `outliers=${currentMetrics.outliers}`,
      `score=${Math.round(currentMetrics.score)}`,
      `width=${Math.round(currentMetrics.width)}mm`,
      `height=${Math.round(currentMetrics.height)}mm`,
      `area=${Math.round(currentMetrics.area)}mm²`,
      `meanSpeed=${Math.round(currentMetrics.meanSpeed)}mm/sample`,
      `acceptedBy=${currentMetrics.acceptedBy}`
    ];
  }

  function clearResult(): void {
    result = null;
  }

  function calibrationProgress(currentMetrics: CalibrationMetrics | undefined): number {
    if (result) return 100;
    return Math.min(99, progressFromMetrics(currentMetrics));
  }

  function progressFromMetrics(currentMetrics: CalibrationMetrics | undefined): number {
    if (!run || !currentMetrics) return currentMetrics ? Math.round(clamp(currentMetrics.score, 0, 100)) : 0;
    const elapsedProgress = clamp((Date.now() - run.startedAt) / CALIBRATION_MIN_MS, 0, 1) * 35;
    const sampleProgress = clamp(currentMetrics.samples / CALIBRATION_MIN_SAMPLES, 0, 1) * 45;
    const scoreProgress = clamp(currentMetrics.score / CALIBRATION_SCORE_THRESHOLD, 0, 1) * 15;
    const acceptedProgress = currentMetrics.acceptedBy === "none" ? 0 : 4;
    return Math.round(sampleProgress + scoreProgress + elapsedProgress + acceptedProgress);
  }

  return {
    get dialogOpen() {
      return dialogOpen;
    },
    set dialogOpen(value: boolean) {
      dialogOpen = value;
    },
    get run() {
      return run;
    },
    get result() {
      return result;
    },
    get metrics() {
      return metrics;
    },
    get progress() {
      return progress;
    },
    get dialogLogs() {
      return dialogLogs;
    },
    start,
    stop,
    update,
    clearResult,
    statusText,
    progressText,
    workItems,
    metricsLines
  };
}
