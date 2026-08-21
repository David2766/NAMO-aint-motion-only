export interface StaticRadarCalibrationSample {
  gate: number;
  sensitivity: number;
  moveEnergy: number;
  stillEnergy: number;
}

export interface StaticRadarCalibrationRecommendation {
  gate: number;
  sampleCount: number;
  currentSensitivity: number;
  recommendedSensitivity: number;
  falsePositiveEnergy: number;
}

export interface StaticRadarCalibrationOptions {
  minSamplesPerGate?: number;
  energyMargin?: number;
  minSensitivity?: number;
  maxGates?: number;
}

const DEFAULT_MIN_SAMPLES_PER_GATE = 5;
const DEFAULT_ENERGY_MARGIN = 5;
const DEFAULT_MIN_SENSITIVITY = 10;
const DEFAULT_MAX_GATES = 2;

export function staticRadarCalibrationRecommendations(
  samples: StaticRadarCalibrationSample[],
  options: StaticRadarCalibrationOptions = {}
): StaticRadarCalibrationRecommendation[] {
  const minSamples = Math.max(1, Math.round(options.minSamplesPerGate ?? DEFAULT_MIN_SAMPLES_PER_GATE));
  const margin = Math.max(1, Math.round(options.energyMargin ?? DEFAULT_ENERGY_MARGIN));
  const minSensitivity = clamp(Math.round(options.minSensitivity ?? DEFAULT_MIN_SENSITIVITY), 0, 100);
  const maxGates = Math.max(1, Math.round(options.maxGates ?? DEFAULT_MAX_GATES));
  const byGate = new Map<number, StaticRadarCalibrationSample[]>();

  for (const sample of samples) {
    if (!validSample(sample)) continue;
    const values = byGate.get(sample.gate) ?? [];
    values.push(sample);
    byGate.set(sample.gate, values);
  }

  return [...byGate.entries()]
    .filter(([, values]) => values.length >= minSamples)
    .map(([gate, values]) => recommendationForGate(gate, values, margin, minSensitivity))
    .filter((value): value is StaticRadarCalibrationRecommendation => value !== null)
    .sort((a, b) => b.sampleCount - a.sampleCount || b.falsePositiveEnergy - a.falsePositiveEnergy)
    .slice(0, maxGates)
    .sort((a, b) => a.gate - b.gate);
}

function recommendationForGate(
  gate: number,
  samples: StaticRadarCalibrationSample[],
  margin: number,
  minSensitivity: number
): StaticRadarCalibrationRecommendation | null {
  const latest = samples[samples.length - 1];
  if (!latest) return null;

  const currentSensitivity = clamp(Math.round(latest.sensitivity), 0, 100);
  if (currentSensitivity <= minSensitivity) return null;

  const energies = samples
    .map((sample) => Math.max(sample.moveEnergy, sample.stillEnergy))
    .sort((a, b) => a - b);
  const falsePositiveEnergy = percentile(energies, 0.9);
  const currentThreshold = 100 - currentSensitivity;
  if (falsePositiveEnergy < currentThreshold) return null;

  const targetThreshold = clamp(Math.ceil(falsePositiveEnergy) + margin, currentThreshold + 1, 100);
  const recommendedSensitivity = clamp(100 - targetThreshold, minSensitivity, currentSensitivity - 1);
  if (recommendedSensitivity >= currentSensitivity) return null;

  return {
    gate,
    sampleCount: samples.length,
    currentSensitivity,
    recommendedSensitivity,
    falsePositiveEnergy
  };
}

function validSample(sample: StaticRadarCalibrationSample): boolean {
  return (
    Number.isInteger(sample.gate) &&
    sample.gate >= 0 &&
    Number.isFinite(sample.sensitivity) &&
    Number.isFinite(sample.moveEnergy) &&
    Number.isFinite(sample.stillEnergy)
  );
}

function percentile(sortedValues: number[], ratio: number): number {
  if (!sortedValues.length) return 0;
  const index = clamp(Math.ceil((sortedValues.length - 1) * ratio), 0, sortedValues.length - 1);
  return sortedValues[index] ?? 0;
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}
