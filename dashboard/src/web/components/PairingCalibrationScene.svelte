<script lang="ts">
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import type {
    PlatformCalibrationDisplayModel,
    PlatformCalibrationSample
  } from "../platform-calibration";
  import { calibrationSampleReferencePointPx } from "../platform-calibration";

  type ViewMode = "together" | "reference" | "peer";

  type Props = {
    floorplan: FloorplanStorageDocument;
    imageUrl: string;
    model: PlatformCalibrationDisplayModel;
    samples: PlatformCalibrationSample[];
    view: ViewMode;
    referenceName: string;
    peerName: string;
    ariaLabel: string;
    minimumBaselineLabel: string;
  };

  let {
    floorplan,
    imageUrl,
    model,
    samples,
    view,
    referenceName,
    peerName,
    ariaLabel,
    minimumBaselineLabel
  }: Props = $props();

  const markerSize = $derived(Math.max(8, Math.min(floorplan.image.width, floorplan.image.height) * 0.022));
  const radarRayLength = $derived(Math.max(32, Math.min(floorplan.image.width, floorplan.image.height) * 0.12));
  const baselineRadius = $derived(
    800 / ((floorplan.scale.mmPerPxX + floorplan.scale.mmPerPxY) / 2)
  );
  const firstSample = $derived(
    samples.length > 0 ? calibrationSampleReferencePointPx(floorplan, samples[0]) : null
  );

  function visible(role: "reference" | "peer"): boolean {
    return view === "together" || view === role;
  }

  function heading(radar: PlatformCalibrationDisplayModel["referenceRadar"]): { x: number; y: number } {
    const radians = radar.rotationDegrees * Math.PI / 180;
    return {
      x: radar.xPx + Math.sin(radians) * radarRayLength,
      y: radar.yPx - Math.cos(radians) * radarRayLength
    };
  }
</script>

<div class="pairing-calibration-scene">
  <svg
    viewBox={`0 0 ${floorplan.image.width} ${floorplan.image.height}`}
    preserveAspectRatio="xMidYMid meet"
    role="img"
    aria-label={ariaLabel}
  >
    <image
      href={imageUrl}
      x="0"
      y="0"
      width={floorplan.image.width}
      height={floorplan.image.height}
      preserveAspectRatio="none"
    />

    {#if firstSample && samples.length === 1}
      <circle
        class="calibration-baseline"
        cx={firstSample.xPx}
        cy={firstSample.yPx}
        r={baselineRadius}
      />
      <text
        class="calibration-baseline-label"
        x={firstSample.xPx}
        y={firstSample.yPx - baselineRadius - markerSize}
        font-size={markerSize * 0.9}
        text-anchor="middle"
      >{minimumBaselineLabel}</text>
    {/if}

    {#each samples as sample, index}
      {@const point = calibrationSampleReferencePointPx(floorplan, sample)}
      <g class="calibration-sample">
        <circle cx={point.xPx} cy={point.yPx} r={markerSize * 0.76} />
        <text
          x={point.xPx}
          y={point.yPx}
          font-size={markerSize * 0.9}
          text-anchor="middle"
          dominant-baseline="central"
        >{index + 1}</text>
      </g>
    {/each}

    {#if view === "together" && model.referenceTarget && model.peerTarget}
      <line
        class="calibration-gap"
        x1={model.referenceTarget.xPx}
        y1={model.referenceTarget.yPx}
        x2={model.peerTarget.xPx}
        y2={model.peerTarget.yPx}
      />
    {/if}

    {#if visible("reference")}
      {@const end = heading(model.referenceRadar)}
      <g class="calibration-radar calibration-reference">
        <line
          x1={model.referenceRadar.xPx}
          y1={model.referenceRadar.yPx}
          x2={end.x}
          y2={end.y}
        />
        <circle cx={model.referenceRadar.xPx} cy={model.referenceRadar.yPx} r={markerSize * 0.58} />
        <text
          x={model.referenceRadar.xPx}
          y={model.referenceRadar.yPx + markerSize * 1.7}
          font-size={markerSize * 0.9}
          text-anchor="middle"
        >{referenceName}</text>
      </g>
      {#if model.referenceTarget}
        <g class="calibration-target calibration-reference">
          <circle cx={model.referenceTarget.xPx} cy={model.referenceTarget.yPx} r={markerSize} />
          <circle cx={model.referenceTarget.xPx} cy={model.referenceTarget.yPx} r={markerSize * 0.35} />
        </g>
      {/if}
    {/if}

    {#if visible("peer")}
      {@const end = heading(model.peerRadar)}
      <g class="calibration-radar calibration-peer">
        <line
          x1={model.peerRadar.xPx}
          y1={model.peerRadar.yPx}
          x2={end.x}
          y2={end.y}
        />
        <circle cx={model.peerRadar.xPx} cy={model.peerRadar.yPx} r={markerSize * 0.58} />
        <text
          x={model.peerRadar.xPx}
          y={model.peerRadar.yPx + markerSize * 1.7}
          font-size={markerSize * 0.9}
          text-anchor="middle"
        >{peerName}</text>
      </g>
      {#if model.peerTarget}
        <g class="calibration-target calibration-peer">
          <circle cx={model.peerTarget.xPx} cy={model.peerTarget.yPx} r={markerSize} />
          <circle cx={model.peerTarget.xPx} cy={model.peerTarget.yPx} r={markerSize * 0.35} />
        </g>
      {/if}
    {/if}
  </svg>
</div>

<style>
  .pairing-calibration-scene {
    position: relative;
    overflow: hidden;
    width: 100%;
    aspect-ratio: 16 / 10;
    min-height: 280px;
    border: 1px solid rgba(123, 184, 216, 0.2);
    border-radius: 6px;
    background: #0b1118;
  }

  svg {
    display: block;
    width: 100%;
    height: 100%;
  }

  .calibration-gap {
    stroke: rgba(255, 255, 255, 0.8);
    stroke-width: 2;
    stroke-dasharray: 7 6;
    vector-effect: non-scaling-stroke;
  }

  .calibration-radar line {
    stroke-width: 3;
    vector-effect: non-scaling-stroke;
  }

  .calibration-radar circle {
    fill: #111820;
    stroke-width: 3;
    vector-effect: non-scaling-stroke;
  }

  .calibration-radar text,
  .calibration-baseline-label {
    fill: #f2f7fb;
    font-weight: 700;
    paint-order: stroke;
    stroke: rgba(6, 11, 16, 0.88);
    stroke-width: 3px;
    stroke-linejoin: round;
    letter-spacing: 0;
  }

  .calibration-target > circle:first-child {
    fill-opacity: 0.22;
    stroke-width: 4;
    vector-effect: non-scaling-stroke;
  }

  .calibration-target > circle:last-child {
    stroke: #f2f7fb;
    stroke-width: 2;
    vector-effect: non-scaling-stroke;
  }

  .calibration-reference line,
  .calibration-reference circle {
    stroke: #25b8e6;
  }

  .calibration-reference > circle:last-child {
    fill: #25b8e6;
  }

  .calibration-peer line,
  .calibration-peer circle {
    stroke: #ffb454;
  }

  .calibration-peer > circle:last-child {
    fill: #ffb454;
  }

  .calibration-baseline {
    fill: rgba(255, 180, 84, 0.08);
    stroke: rgba(255, 180, 84, 0.9);
    stroke-width: 2;
    stroke-dasharray: 8 6;
    vector-effect: non-scaling-stroke;
  }

  .calibration-sample circle {
    fill: rgba(17, 24, 32, 0.92);
    stroke: #f2f7fb;
    stroke-width: 2;
    vector-effect: non-scaling-stroke;
  }

  .calibration-sample text {
    fill: #f2f7fb;
    font-weight: 700;
    letter-spacing: 0;
  }

  @media (max-width: 760px) {
    .pairing-calibration-scene {
      min-height: 0;
      max-height: 52vh;
      aspect-ratio: 4 / 3;
    }
  }
</style>
