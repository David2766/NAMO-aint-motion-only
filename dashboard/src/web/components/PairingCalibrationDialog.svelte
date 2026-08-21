<script lang="ts">
  import { onMount } from "svelte";
  import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
  import type { WebDeviceState } from "../../core/types";
  import {
    createPlatformPairingClient,
    type PlatformDescriptor,
    type PlatformParticipant,
    type PlatformParticipantAssignment
  } from "../api/platform-pairing";
  import { findRadarRoom } from "../floorplan/floorplan-room-context";
  import { loadFloorplanStorageImage } from "../floorplan/floorplan-storage-client";
  import type { Messages } from "../i18n";
  import {
    PlatformCalibrationError,
    approvePlatformCalibration,
    buildPlatformCalibrationDisplayModel,
    buildPlatformCalibrationProposal,
    capturePlatformCalibrationSample,
    waitForPlatformCalibrationBootstrap,
    type PlatformCalibrationProposal,
    type PlatformCalibrationSample
  } from "../platform-calibration";
  import {
    loadPairingFloorplanDocument,
    type PairingRadarPlacementDevice
  } from "../pairing-floorplan";
  import { tracePairingPhase } from "../pairing-trace";
  import PairingCalibrationScene from "./PairingCalibrationScene.svelte";

  type ViewMode = "together" | "reference" | "peer";

  type Props = {
    baseUrl: string;
    peerAddress: string;
    participant: PlatformParticipant;
    participantName: string;
    localNodeId: string;
    prepareCommunityFallback?: boolean;
    fetcher?: typeof fetch;
    messages: Messages;
    mockMode?: boolean;
    onclose: () => void;
    oncomplete: () => void | Promise<void>;
    onplacementmissing: (devices: PairingRadarPlacementDevice[]) => void;
  };

  let {
    baseUrl,
    peerAddress,
    participant,
    participantName,
    localNodeId,
    prepareCommunityFallback = false,
    fetcher,
    messages,
    mockMode = false,
    onclose,
    oncomplete,
    onplacementmissing
  }: Props = $props();
  let busy = $state(true);
  let stateRefreshBusy = $state(false);
  let error = $state("");
  let samples = $state<PlatformCalibrationSample[]>([]);
  let proposal = $state<PlatformCalibrationProposal | null>(null);
  let referenceDescriptor = $state<PlatformDescriptor | null>(null);
  let peerDescriptor = $state<PlatformDescriptor | null>(null);
  let referenceFloorplan = $state<FloorplanStorageDocument | null>(null);
  let peerFloorplan = $state<FloorplanStorageDocument | null>(null);
  let referenceState = $state<WebDeviceState | null>(null);
  let peerState = $state<WebDeviceState | null>(null);
  let calibrationVersion = $state(1);
  let view = $state<ViewMode>("together");
  let mockFloorplanImageUrl = $state("");

  const client = $derived(createPlatformPairingClient({ fetcher }));
  const text = $derived(messages.dashboard.pairing.calibration);
  const displayModel = $derived.by(() => {
    if (!referenceFloorplan || !peerFloorplan || !referenceState || !peerState) return null;
    return buildPlatformCalibrationDisplayModel(
      referenceFloorplan,
      peerFloorplan,
      referenceState,
      peerState
    );
  });

  onMount(() => {
    void prepare();
    const pollTimer = window.setInterval(() => {
      if (document.visibilityState === "visible") void refreshLiveState();
    }, 800);
    return () => {
      window.clearInterval(pollTimer);
      if (mockFloorplanImageUrl.startsWith("blob:")) URL.revokeObjectURL(mockFloorplanImageUrl);
    };
  });

  async function prepare(): Promise<void> {
    busy = true;
    error = "";
    try {
      const [localDescriptor, remoteDescriptor] = await Promise.all([
        client.getDescriptor(baseUrl),
        client.getDescriptor(peerAddress)
      ]);
      const [localFloorplanResult, remoteFloorplanResult] = await Promise.allSettled([
        loadPairingFloorplanDocument({
          baseUrl,
          fetcher,
          requireLocalRadarPlacement: true
        }),
        loadPairingFloorplanDocument({
          baseUrl: peerAddress,
          fetcher,
          requireLocalRadarPlacement: true
        })
      ]);
      const missingPlacements: PairingRadarPlacementDevice[] = [];
      if (localFloorplanResult.status === "rejected" && isMissingRadarPlacement(localFloorplanResult.reason)) {
        missingPlacements.push("local");
      }
      if (remoteFloorplanResult.status === "rejected" && isMissingRadarPlacement(remoteFloorplanResult.reason)) {
        missingPlacements.push("peer");
      }
      if (missingPlacements.length > 0) {
        onplacementmissing(missingPlacements);
        return;
      }
      if (localFloorplanResult.status === "rejected") throw localFloorplanResult.reason;
      if (remoteFloorplanResult.status === "rejected") throw remoteFloorplanResult.reason;
      const localFloorplan = localFloorplanResult.value;
      const remoteFloorplan = remoteFloorplanResult.value;
      if (remoteDescriptor.participantId !== participant.participantId || localDescriptor.participantId !== localNodeId) {
        throw new PlatformCalibrationError("calibration_identity_mismatch");
      }
      if (mockMode) {
        const [localState, remoteState, image] = await Promise.all([
          client.getDeviceState(baseUrl),
          client.getDeviceState(peerAddress),
          loadFloorplanStorageImage({ baseUrl, fetcher })
        ]);
        referenceDescriptor = localDescriptor;
        peerDescriptor = remoteDescriptor;
        referenceFloorplan = localFloorplan;
        peerFloorplan = remoteFloorplan;
        referenceState = localState;
        peerState = remoteState;
        calibrationVersion = 1;
        if (mockFloorplanImageUrl.startsWith("blob:")) URL.revokeObjectURL(mockFloorplanImageUrl);
        mockFloorplanImageUrl = URL.createObjectURL(image);
        return;
      }
      await waitForPlatformCalibrationBootstrap(
        client,
        baseUrl,
        peerAddress,
        localDescriptor.participantId,
        remoteDescriptor.participantId,
        localFloorplan.radar.poseVersion ?? 0,
        remoteFloorplan.radar.poseVersion ?? 0
      );
      const [localParticipants, remoteParticipants, localState, remoteState] = await Promise.all([
        client.getParticipants(baseUrl),
        client.getParticipants(peerAddress),
        client.getDeviceState(baseUrl),
        client.getDeviceState(peerAddress)
      ]);
      referenceDescriptor = localDescriptor;
      peerDescriptor = remoteDescriptor;
      referenceFloorplan = localFloorplan;
      peerFloorplan = remoteFloorplan;
      referenceState = localState;
      peerState = remoteState;
      const localVersion = localParticipants.participants.find(
        (item) => item.participantId === remoteDescriptor.participantId
      )?.assignment?.calibrationVersion ?? 0;
      const peerVersion = remoteParticipants.participants.find(
        (item) => item.participantId === localDescriptor.participantId
      )?.assignment?.calibrationVersion ?? 0;
      calibrationVersion = Math.max(localVersion, peerVersion) + 1;
      if (calibrationVersion > 0xffffffff) throw new PlatformCalibrationError("calibration_version_invalid");
    } catch (cause) {
      error = errorMessage(cause);
    } finally {
      busy = false;
    }
  }

  async function refreshLiveState(): Promise<[WebDeviceState, WebDeviceState] | null> {
    if (stateRefreshBusy || busy || !referenceFloorplan || !peerFloorplan) return null;
    stateRefreshBusy = true;
    try {
      const results = await Promise.allSettled([
        client.getDeviceState(baseUrl),
        client.getDeviceState(peerAddress)
      ]);
      if (results[0].status === "fulfilled") {
        referenceState = results[0].value;
      } else if (referenceState) {
        referenceState = { ...referenceState, connected: false };
      }
      if (results[1].status === "fulfilled") {
        peerState = results[1].value;
      } else if (peerState) {
        peerState = { ...peerState, connected: false };
      }
      return results[0].status === "fulfilled" && results[1].status === "fulfilled"
        ? [results[0].value, results[1].value]
        : null;
    } finally {
      stateRefreshBusy = false;
    }
  }

  async function capture(): Promise<void> {
    if (samples.length >= 2 || !referenceFloorplan || !peerFloorplan) return;
    busy = true;
    error = "";
    try {
      const states = await Promise.all([
        client.getDeviceState(baseUrl),
        client.getDeviceState(peerAddress)
      ]) as [WebDeviceState, WebDeviceState];
      [referenceState, peerState] = states;
      const nextSamples = [...samples, capturePlatformCalibrationSample(states[0], states[1])];
      if (mockMode) {
        samples = nextSamples;
        return;
      }
      proposal = buildPlatformCalibrationProposal(
        referenceFloorplan,
        peerFloorplan,
        nextSamples,
        calibrationVersion
      );
      samples = nextSamples;
    } catch (cause) {
      error = errorMessage(cause);
    } finally {
      busy = false;
    }
  }

  function reset(): void {
    if (busy) return;
    samples = [];
    proposal = null;
    error = "";
  }

  async function approve(): Promise<void> {
    if (!referenceDescriptor || !peerDescriptor) return;
    if (!mockMode && !proposal) return;
    busy = true;
    error = "";
    try {
      if (mockMode) {
        if (!referenceFloorplan || !peerFloorplan || samples.length < 2 ||
            !referenceDescriptor.capabilities || !peerDescriptor.capabilities) return;
        const referenceAssignment = mockAssignment(referenceFloorplan, calibrationVersion);
        const peerAssignment = mockAssignment(peerFloorplan, calibrationVersion);
        await client.configureParticipant(
          baseUrl,
          peerDescriptor.participantId,
          peerDescriptor.capabilities,
          peerAssignment
        );
        await client.configureParticipant(
          peerAddress,
          referenceDescriptor.participantId,
          referenceDescriptor.capabilities,
          referenceAssignment
        );
        await client.activateParticipant(baseUrl, peerDescriptor.participantId);
        await client.activateParticipant(peerAddress, referenceDescriptor.participantId);
        await oncomplete();
        return;
      }
      await tracePairingPhase(
        "calibration.approve",
        () => approvePlatformCalibration(
          client,
          baseUrl,
          peerAddress,
          referenceDescriptor!,
          peerDescriptor!,
          proposal!,
          { prepareCommunityFallback }
        ),
        { peer: peerAddress }
      );
      await oncomplete();
    } catch (cause) {
      error = errorMessage(cause);
      busy = false;
    }
  }

  function normalizeDeviceUrl(value: string): string {
    const trimmed = value.trim().replace(/\/+$/, "");
    if (trimmed) return /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`;
    return typeof window === "undefined" ? "http://localhost" : window.location.origin;
  }

  function floorplanImageUrl(value: string): string {
    return new URL("/api/floorplan/image", normalizeDeviceUrl(value)).toString();
  }

  function mockAssignment(
    floorplan: FloorplanStorageDocument,
    version: number
  ): PlatformParticipantAssignment {
    const room = findRadarRoom(floorplan) ?? floorplan.rooms[0];
    const spaceId = room?.id || "mock_room";
    const poseVersion = floorplan.radar.poseVersion ?? 1;
    return {
      floorplanId: floorplan.floorplanId || "mock-floorplan",
      spaceId,
      groupId: spaceId,
      geometryVersion: floorplan.geometryVersion ?? 1,
      localPoseVersion: poseVersion,
      poseVersion,
      calibrationVersion: version,
      transform: {
        originXMillimeters: Math.round(floorplan.radar.originPx[0] * floorplan.scale.mmPerPxX),
        originYMillimeters: Math.round(floorplan.radar.originPx[1] * floorplan.scale.mmPerPxY),
        rotationCentiDegrees: Math.round(floorplan.radar.rotationDeg * 100),
        residualMillimeters: 0,
        calibrationSamples: 2
      }
    };
  }

  function errorMessage(cause: unknown): string {
    const code = cause instanceof PlatformCalibrationError
      ? cause.code
      : cause instanceof Error
        ? cause.message
        : "calibration_failed";
    return text.error(code);
  }

  function isMissingRadarPlacement(cause: unknown): boolean {
    return cause instanceof PlatformCalibrationError
      ? cause.code === "calibration_pose_missing"
      : cause instanceof Error && cause.message === "calibration_pose_missing";
  }
</script>

<div class="dashboard-dialog-backdrop pairing-calibration-backdrop" role="presentation">
  <div
    class="floorplan-panel pairing-calibration-panel"
    role="dialog"
    aria-modal="true"
    aria-labelledby="pairing-calibration-title"
  >
    <aside class="floorplan-controls pairing-calibration-sidebar">
      <div class="floorplan-workflow-card">
        <div>
          <strong id="pairing-calibration-title">{text.title}</strong>
          <span>{text.description(participantName)}</span>
        </div>

        <ol class="pairing-calibration-progress">
          <li data-active={!displayModel?.captureReady && samples.length === 0}>{text.steps.check}</li>
          <li data-active={displayModel?.captureReady && samples.length === 0}>{text.steps.first}</li>
          <li data-active={samples.length === 1}>{text.steps.second}</li>
          <li data-active={samples.length === 2}>{text.steps.review}</li>
        </ol>
      </div>

      <div class="floorplan-workflow-card pairing-calibration-status">
        <div>
          <strong>{text.liveStatus}</strong>
          <span>{text.liveStatusHelp}</span>
        </div>
        <div class="calibration-device-status calibration-reference">
          <span class="calibration-swatch"></span>
          <div>
            <strong>{text.thisDevice}</strong>
            <span>{referenceState?.connected ? text.deviceOnline : text.deviceOffline} · {text.targetCount(displayModel?.referenceTargetCount ?? 0)}</span>
          </div>
        </div>
        <div class="calibration-device-status calibration-peer">
          <span class="calibration-swatch"></span>
          <div>
            <strong>{participantName}</strong>
            <span>{peerState?.connected ? text.deviceOnline : text.deviceOffline} · {text.targetCount(displayModel?.peerTargetCount ?? 0)}</span>
          </div>
        </div>
        {#if displayModel?.targetSeparationMillimeters !== null && displayModel?.targetSeparationMillimeters !== undefined}
          <div class="floorplan-status" data-tone={displayModel.captureReady ? "ok" : "warn"}>
            <strong>{text.targetSeparation(displayModel.targetSeparationMillimeters)}</strong>
          </div>
        {:else}
          <div class="floorplan-status" data-tone="warn">
            <strong>{text.waitingForTargets}</strong>
          </div>
        {/if}
      </div>
    </aside>

    <div class="floorplan-workspace-column pairing-calibration-workspace">
      <div class="floorplan-workflow-card pairing-calibration-stage">
        <div class="pairing-calibration-stage-heading">
          <div>
            <strong>{samples.length === 0 ? text.firstPosition : samples.length === 1 ? text.secondPosition : text.ready}</strong>
            <span>{samples.length === 0 ? text.firstPositionHelp : samples.length === 1 ? text.secondPositionHelp : text.readyHelp}</span>
          </div>
          <div class="pairing-calibration-view-tabs" role="group" aria-label={text.liveStatus}>
            <button type="button" data-selected={view === "together"} onclick={() => (view = "together")}>{text.views.together}</button>
            <button type="button" data-selected={view === "reference"} onclick={() => (view = "reference")}>{text.views.reference}</button>
            <button type="button" data-selected={view === "peer"} onclick={() => (view = "peer")}>{text.views.peer}</button>
          </div>
        </div>

        {#if referenceFloorplan && peerFloorplan && displayModel}
          <PairingCalibrationScene
            floorplan={referenceFloorplan}
            imageUrl={mockFloorplanImageUrl || floorplanImageUrl(baseUrl)}
            model={displayModel}
            {samples}
            {view}
            referenceName={text.thisDevice}
            peerName={participantName}
            ariaLabel={text.mapAria}
            minimumBaselineLabel={text.minimumBaseline}
          />
        {:else}
          <div class="floorplan-empty-note">
            <strong>{busy ? text.working : text.waitingForTargets}</strong>
          </div>
        {/if}

        <div class="pairing-calibration-mobile-status">
          <div class="calibration-device-status calibration-reference">
            <span class="calibration-swatch"></span>
            <div>
              <strong>{text.thisDevice}</strong>
              <span>{referenceState?.connected ? text.deviceOnline : text.deviceOffline} · {text.targetCount(displayModel?.referenceTargetCount ?? 0)}</span>
            </div>
          </div>
          <div class="calibration-device-status calibration-peer">
            <span class="calibration-swatch"></span>
            <div>
              <strong>{participantName}</strong>
              <span>{peerState?.connected ? text.deviceOnline : text.deviceOffline} · {text.targetCount(displayModel?.peerTargetCount ?? 0)}</span>
            </div>
          </div>
          <div class="floorplan-status" data-tone={displayModel?.captureReady ? "ok" : "warn"}>
            <strong>
              {displayModel?.targetSeparationMillimeters !== null &&
              displayModel?.targetSeparationMillimeters !== undefined
                ? text.targetSeparation(displayModel.targetSeparationMillimeters)
                : text.waitingForTargets}
            </strong>
          </div>
        </div>

        {#if proposal}
          <dl class="floorplan-stored-summary calibration-summary">
            <div><dt>{text.samples}</dt><dd>{proposal.samples}</dd></div>
            <div><dt>{text.translation}</dt><dd>{proposal.translationMillimeters} mm</dd></div>
            <div><dt>{text.rotation}</dt><dd>{proposal.rotationCorrectionDegrees.toFixed(2)} deg</dd></div>
            <div><dt>{text.residual}</dt><dd>{proposal.residualMillimeters} mm</dd></div>
            <div><dt>{text.room}</dt><dd>{proposal.spaceId}</dd></div>
          </dl>
        {/if}

        {#if error}<p class="floorplan-save-status" data-tone="error" role="alert">{error}</p>{/if}
      </div>

      <div class="floorplan-delete-dialog-actions pairing-calibration-actions">
        <button type="button" disabled={busy} onclick={onclose}>{messages.common.cancel}</button>
        {#if samples.length > 0}
          <button type="button" disabled={busy} onclick={reset}>{text.reset}</button>
        {/if}
        {#if samples.length < 2}
          <button
            type="button"
            disabled={busy || !displayModel?.captureReady}
            onclick={capture}
          >
            {busy ? text.working : samples.length === 0 ? text.captureFirst : text.captureSecond}
          </button>
        {/if}
        {#if proposal || (mockMode && samples.length === 2)}
          <button type="button" disabled={busy} onclick={approve}>
            {busy ? text.working : text.approveAndActivate}
          </button>
        {/if}
      </div>
    </div>
  </div>
</div>

<style>
  .pairing-calibration-backdrop {
    background: rgba(2, 8, 12, 0.84);
  }

  .pairing-calibration-panel {
    grid-template-columns: minmax(280px, 340px) minmax(0, 880px);
    width: min(1240px, calc(100vw - 32px));
    max-height: min(900px, calc(100vh - 32px));
    overflow: hidden;
    background: #111820;
    box-shadow: 0 28px 84px rgba(0, 0, 0, 0.62);
  }

  .pairing-calibration-sidebar,
  .pairing-calibration-workspace,
  .pairing-calibration-stage,
  .pairing-calibration-status {
    min-width: 0;
  }

  .pairing-calibration-sidebar,
  .pairing-calibration-workspace {
    overflow-y: auto;
  }

  .pairing-calibration-workspace,
  .pairing-calibration-stage,
  .pairing-calibration-status,
  .pairing-calibration-mobile-status {
    display: grid;
    gap: 10px;
  }

  .pairing-calibration-mobile-status {
    display: none;
  }

  .pairing-calibration-progress {
    display: grid;
    gap: 6px;
    margin: 0;
    padding: 0;
    list-style: none;
  }

  .pairing-calibration-progress li {
    padding: 7px 8px;
    border-left: 2px solid rgba(123, 184, 216, 0.2);
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .pairing-calibration-progress li[data-active="true"] {
    border-color: #06d6a0;
    color: #d7eefc;
    font-weight: var(--rz-weight-label);
  }

  .calibration-device-status {
    display: grid;
    grid-template-columns: 10px minmax(0, 1fr);
    align-items: center;
    gap: 8px;
    min-width: 0;
    padding: 8px;
    border: 1px solid rgba(123, 184, 216, 0.18);
    border-radius: 6px;
    background: rgba(123, 184, 216, 0.05);
  }

  .calibration-device-status > div {
    display: grid;
    min-width: 0;
    gap: 2px;
  }

  .calibration-device-status strong {
    overflow-wrap: anywhere;
    font-size: var(--rz-font-title-card);
  }

  .calibration-device-status span {
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .calibration-swatch {
    width: 10px;
    height: 10px;
    border-radius: 50%;
  }

  .calibration-reference .calibration-swatch {
    background: #25b8e6;
  }

  .calibration-peer .calibration-swatch {
    background: #ffb454;
  }

  .pairing-calibration-stage-heading {
    display: flex;
    align-items: end;
    justify-content: space-between;
    gap: 12px;
  }

  .pairing-calibration-stage-heading > div:first-child {
    display: grid;
    min-width: 0;
    gap: 4px;
  }

  .pairing-calibration-stage-heading strong {
    font-size: var(--rz-font-title-card);
  }

  .pairing-calibration-stage-heading span {
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
    line-height: 1.5;
  }

  .pairing-calibration-view-tabs {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    flex: 0 0 auto;
    min-width: 280px;
    padding: 3px;
    border: 1px solid rgba(123, 184, 216, 0.2);
    border-radius: 6px;
    background: rgba(2, 8, 12, 0.34);
  }

  .pairing-calibration-view-tabs button {
    min-height: 34px;
    padding: 6px 9px;
    border: 0;
    background: transparent;
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .pairing-calibration-view-tabs button[data-selected="true"] {
    background: rgba(123, 184, 216, 0.16);
    color: #d7eefc;
  }

  .calibration-summary {
    margin: 0;
  }

  .pairing-calibration-actions {
    position: sticky;
    bottom: 0;
    z-index: 2;
    margin-top: auto;
    padding: 12px;
    border-top: 1px solid rgba(123, 184, 216, 0.18);
    background: #111820;
  }

  @media (max-width: 760px) {
    .pairing-calibration-backdrop {
      padding: 0;
    }

    .pairing-calibration-panel {
      grid-template-columns: 1fr;
      width: 100vw;
      height: 100dvh;
      max-height: none;
      border-radius: 0;
      overflow-y: auto;
    }

    .pairing-calibration-sidebar,
    .pairing-calibration-workspace {
      overflow: visible;
    }

    .pairing-calibration-sidebar {
      padding-bottom: 0;
    }

    .pairing-calibration-status {
      display: none;
    }

    .pairing-calibration-mobile-status {
      display: grid;
    }

    .pairing-calibration-stage-heading {
      align-items: stretch;
      flex-direction: column;
    }

    .pairing-calibration-view-tabs {
      width: 100%;
      min-width: 0;
    }

    .pairing-calibration-actions {
      bottom: 0;
      flex-wrap: wrap;
    }

    .pairing-calibration-actions button {
      flex: 1 1 120px;
    }
  }
</style>
