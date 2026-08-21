import {
  staticRadarCalibrationRecommendations,
  type StaticRadarCalibrationRecommendation,
  type StaticRadarCalibrationSample
} from "../../core/static-radar-calibration";
import { staticRadarDetectedGateNumber } from "../static-radar-tuning";
import type { DeviceApi, WebDeviceState, WebStaticRadarTuningStatus } from "../types";

interface StaticRadarCalibrationSessionOptions {
  getApi: () => DeviceApi;
  getState: () => WebDeviceState | null;
  onStarted: () => void;
  onFailed: () => void;
  onSample: (count: number) => void;
  onAdjusted: (recommendation: StaticRadarCalibrationRecommendation) => void;
}

export interface StaticRadarCalibrationApplyResult {
  adjusted: number;
  failed: boolean;
  stillDetected: boolean;
}

export function createStaticRadarCalibrationSession({
  getApi,
  getState,
  onStarted,
  onFailed,
  onSample,
  onAdjusted
}: StaticRadarCalibrationSessionOptions) {
  let samples: StaticRadarCalibrationSample[] = [];
  let active = false;
  let pollPending = false;
  let lastPollAt = 0;
  let lastKeepaliveAt = 0;
  let token = 0;

  async function start(): Promise<boolean> {
    stop();
    samples = [];
    const currentToken = ++token;
    const api = getApi();
    if (!api.setStaticRadarTuningSession || !api.getStaticRadarTuningStatus) return false;
    try {
      await api.setStaticRadarTuningSession(true);
      if (currentToken !== token) {
        await api.setStaticRadarTuningSession(false);
        return false;
      }
      active = true;
      lastKeepaliveAt = Date.now();
      onStarted();
      await poll(currentToken);
      return true;
    } catch {
      if (currentToken === token) onFailed();
      return false;
    }
  }

  function update(now: number): void {
    if (!active) return;
    const api = getApi();
    if (now - lastKeepaliveAt >= 5000 && api.setStaticRadarTuningSession) {
      lastKeepaliveAt = now;
      void api.setStaticRadarTuningSession(true).catch(onFailed);
    }
    if (now - lastPollAt >= 1000) {
      lastPollAt = now;
      void poll(token);
    }
  }

  function stop(): void {
    token += 1;
    const shouldClose = active;
    active = false;
    pollPending = false;
    lastPollAt = 0;
    lastKeepaliveAt = 0;
    if (shouldClose) void getApi().setStaticRadarTuningSession?.(false).catch(() => undefined);
  }

  function ready(): boolean {
    return samples.length >= 5 || getState()?.debug?.staticRadar?.presence === false;
  }

  function recommendations(): StaticRadarCalibrationRecommendation[] {
    return staticRadarCalibrationRecommendations(samples);
  }

  async function apply(): Promise<StaticRadarCalibrationApplyResult> {
    const api = getApi();
    try {
      const initial = recommendations();
      const adjustedGates = new Set(await applyRecommendations(initial));
      if (initial.length === 0) return { adjusted: 0, failed: false, stillDetected: false };

      await delay(2500);
      const state = await api.getState();
      if (state.pirMotion || !state.debug?.staticRadar?.presence) {
        return { adjusted: adjustedGates.size, failed: false, stillDetected: false };
      }

      await api.setStaticRadarTuningSession?.(true);
      const status = await api.getStaticRadarTuningStatus?.();
      const retrySample = status ? sampleFromStatus(status, state) : null;
      if (!retrySample) return { adjusted: adjustedGates.size, failed: false, stillDetected: true };
      const retry = staticRadarCalibrationRecommendations([retrySample], {
        minSamplesPerGate: 1,
        energyMargin: 8,
        maxGates: 1
      });
      for (const gate of await applyRecommendations(retry)) adjustedGates.add(gate);
      if (retry.length === 0) return { adjusted: adjustedGates.size, failed: false, stillDetected: true };

      await delay(2500);
      const finalState = await api.getState();
      return {
        adjusted: adjustedGates.size,
        failed: false,
        stillDetected: !finalState.pirMotion && Boolean(finalState.debug?.staticRadar?.presence)
      };
    } catch {
      onFailed();
      return { adjusted: 0, failed: true, stillDetected: false };
    }
  }

  async function poll(currentToken: number): Promise<void> {
    const api = getApi();
    if (pollPending || !api.getStaticRadarTuningStatus) return;
    pollPending = true;
    try {
      const status = await api.getStaticRadarTuningStatus();
      if (!active || currentToken !== token) return;
      const sample = sampleFromStatus(status, getState());
      if (!sample) return;
      samples = [...samples, sample].slice(-120);
      if (samples.length === 1 || samples.length % 5 === 0) onSample(samples.length);
    } catch {
      if (active && currentToken === token) onFailed();
    } finally {
      pollPending = false;
    }
  }

  function sampleFromStatus(
    status: WebStaticRadarTuningStatus,
    state: WebDeviceState | null
  ): StaticRadarCalibrationSample | null {
    if (!status.active || !state?.debug?.staticRadar?.presence) return null;
    const gateNumber = staticRadarDetectedGateNumber(status.gates, state.debug.staticRadar);
    const gate = status.gates.find((value) => value.gate === gateNumber);
    return gate
      ? {
          gate: gate.gate,
          sensitivity: gate.sensitivity,
          moveEnergy: gate.moveEnergy,
          stillEnergy: gate.stillEnergy
        }
      : null;
  }

  async function applyRecommendations(recommendations: StaticRadarCalibrationRecommendation[]): Promise<number[]> {
    const api = getApi();
    if (!api.setStaticRadarGateSensitivity) return [];
    const adjusted: number[] = [];
    for (const recommendation of recommendations) {
      if (getState()?.pirMotion) break;
      await api.setStaticRadarGateSensitivity(recommendation.gate, recommendation.recommendedSensitivity);
      adjusted.push(recommendation.gate);
      onAdjusted(recommendation);
    }
    return adjusted;
  }

  function delay(ms: number): Promise<void> {
    return new Promise((resolve) => window.setTimeout(resolve, ms));
  }

  return {
    get sampleCount() {
      return samples.length;
    },
    start,
    update,
    stop,
    ready,
    recommendations,
    apply
  };
}
