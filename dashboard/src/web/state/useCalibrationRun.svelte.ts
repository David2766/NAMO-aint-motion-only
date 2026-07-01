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
import type { CalibrationResult, WebDeviceConfig, WebDeviceState } from "../types";

type StatusTone = "ok" | "warn" | "error";

interface CalibrationRunOptions {
  getConfig: () => WebDeviceConfig | null;
  getState: () => WebDeviceState | null;
  getCalibrationZoneCount: () => number;
  updateConfig: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
  selectZone: (zoneId: string) => void;
  setStatus: (message: string, tone: StatusTone) => void;
}

export function createCalibrationRun({
  getConfig,
  getState,
  getCalibrationZoneCount,
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
    addLog("보정 시작 요청");

    if (state.pirMotion) {
      finishWithError("PIR 움직임이 감지되어 보정을 시작할 수 없습니다.");
      return;
    }

    const activeTargets = state.targets.filter((target) => target.active);
    if (activeTargets.length === 0) {
      finishWithError("감지된 타겟이 없어 보정을 시작할 수 없습니다.");
      return;
    }
    if (activeTargets.length > 1) {
      finishWithError("타겟이 여러 개 감지되어 보정을 시작할 수 없습니다.");
      return;
    }
    if ((config.calibrationZones || []).length >= MAX_CALIBRATION_ZONES) {
      finishWithError(`오탐 보정 구역은 최대 ${MAX_CALIBRATION_ZONES}개까지 저장할 수 있습니다.`);
      return;
    }

    run = { startedAt: Date.now(), samples: [] };
    addLog("PIR 조건 통과");
    addLog("타겟 샘플 수집 시작");
    setStatus("오탐 보정을 시작했습니다.", "warn");
  }

  function stop(reason: string, tone: "warn" | "error"): void {
    const currentMetrics = run ? calibrationMetrics(run.samples) : undefined;
    addLog(reason);
    run = null;
    result = {
      title: tone === "error" ? "보정 실패" : "보정 중지",
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
      stop("PIR 움직임이 감지되어 보정을 취소했습니다.", "error");
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
        addLog(`샘플 수집: ${samples.length}개`);
      }
    } else if (activeTargets.length > 1) {
      stop("타겟이 여러 개 감지되어 보정을 취소했습니다.", "error");
      return;
    }

    const elapsed = Date.now() - run.startedAt;
    const currentMetrics = calibrationMetrics(run.samples);
    if (
      elapsed >= CALIBRATION_MIN_MS &&
      run.samples.length >= CALIBRATION_MIN_SAMPLES &&
      currentMetrics.acceptedBy !== "none"
    ) {
      addLog(`보정 기준 통과: ${currentMetrics.acceptedBy}`);
      apply(currentMetrics);
      return;
    }
    if (elapsed >= CALIBRATION_MAX_MS) {
      stop("반복 감지 영역이 너무 넓거나 불안정해 보정을 만들지 않았습니다.", "error");
    }
  }

  function apply(currentMetrics: CalibrationMetrics): void {
    const config = getConfig();
    if (!config || !run) return;
    const zone = calibrationZoneFromSamples(run.samples, config.calibrationZones || []);
    run = null;
    if (!zone) {
      addLog("오탐 보정 후보 구역 생성 실패");
      result = {
        title: "보정 실패",
        tone: "error",
        createdCount: 0,
        reason: "오탐 보정 후보를 만들지 못했습니다.",
        metrics: currentMetrics,
        logs: [...logs]
      };
      setStatus("오탐 보정 후보를 만들지 못했습니다.", "error");
      return;
    }

    updateConfig((current) => ({
      ...current,
      calibrationZones: [...(current.calibrationZones || []), zone]
    }));
    result = {
      title: "보정 완료",
      tone: "ok",
      createdCount: 1,
      reason:
        currentMetrics.acceptedBy === "score"
          ? "안정도 점수 기준을 통과했습니다."
          : "반복 감지 영역 기준을 통과했습니다.",
      metrics: currentMetrics,
      logs: [...logs, `${zone.id} 구역 생성`]
    };
    selectZone(zone.id);
    setStatus(`오탐 보정 구역을 저장했습니다. 안정도 ${Math.round(currentMetrics.score)}점`, "ok");
  }

  function finishWithError(reason: string): void {
    addLog(reason);
    run = null;
    result = {
      title: "보정 실패",
      tone: "error",
      createdCount: 0,
      reason,
      logs: [...logs]
    };
    setStatus(reason, "error");
  }

  function addLog(message: string): void {
    const time = new Date().toLocaleTimeString();
    logs = [...logs, `[${time}] ${message}`].slice(-40);
  }

  function statusText(): string {
    const config = getConfig();
    const state = getState();
    const calibrationZoneCount = getCalibrationZoneCount();
    if (!config) return "기기 연결 후 사용할 수 있습니다.";
    if (run) {
      const elapsed = Math.floor((Date.now() - run.startedAt) / 1000);
      return `안정도 분석 중입니다. ${elapsed}s / 최대 ${Math.ceil(CALIBRATION_MAX_MS / 1000)}s`;
    }
    if (state?.pirMotion) return "PIR 움직임이 감지되어 시작할 수 없습니다.";
    if (calibrationZoneCount >= MAX_CALIBRATION_ZONES) {
      return `오탐 보정 구역은 최대 ${MAX_CALIBRATION_ZONES}개까지 저장할 수 있습니다.`;
    }
    return `저장된 보정 구역 ${calibrationZoneCount}/${MAX_CALIBRATION_ZONES}`;
  }

  function progressText(currentMetrics: CalibrationMetrics | undefined): string {
    if (result) return result.title;
    if (!run) return "대기 중";
    if (!currentMetrics || currentMetrics.samples === 0) return "타겟 샘플 대기 중";
    if (currentMetrics.samples < CALIBRATION_MIN_SAMPLES) return "샘플 수집 중";
    if (currentMetrics.acceptedBy === "none") return "안정도 분석 중";
    return "보정 구역 생성 준비 완료";
  }

  function workItems(currentMetrics: CalibrationMetrics | undefined): string[] {
    const state = getState();
    if (!run && !result) return ["보정 작업 대기 중"];
    const elapsed = run ? Math.floor((Date.now() - run.startedAt) / 1000) : null;
    return [
      `PIR 상태: ${state?.pirMotion ? "움직임 감지됨" : "움직임 없음"}`,
      elapsed === null ? "수집 시간: 종료됨" : `수집 시간: ${elapsed}s / 최소 ${Math.ceil(CALIBRATION_MIN_MS / 1000)}s`,
      `샘플: ${currentMetrics?.samples ?? 0} / 최소 ${CALIBRATION_MIN_SAMPLES}`,
      `사용 샘플: ${currentMetrics?.usedSamples ?? 0}`,
      `제외 샘플: ${currentMetrics?.outliers ?? 0}`,
      `안정도 점수: ${Math.round(currentMetrics?.score ?? 0)} / ${CALIBRATION_SCORE_THRESHOLD}`,
      `판정 기준: ${currentMetrics?.acceptedBy ?? "none"}`
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
