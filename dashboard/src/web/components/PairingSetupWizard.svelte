<script lang="ts">
  import { onDestroy } from "svelte";
  import {
    createPlatformPairingClient,
    PAIRING_INVITATION_DURATION_MS,
    pairingKeepsCanonicalFloorplan,
    pairingSetupRelationship,
    PlatformPairingError,
    shouldOfferCommunityFallbackPreparation,
    siteConvergenceExpectation,
    type PlatformDiscoveryCandidate,
    type PlatformPairingConnection
  } from "../api/platform-pairing";
  import { deviceStorageQueue } from "../api/device-storage-queue";
  import {
    hasCompleteFloorplanStorage,
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus,
    type FloorplanStorageStatus
  } from "../floorplan/floorplan-storage-client";
  import type { Messages } from "../i18n";
  import {
    copyPairingFloorplanToLocal,
    pairingFloorplanDecision,
    pairingFloorplansMatch,
    type PairingFloorplanSource
  } from "../pairing-floorplan";
  import { tracePairingPhase } from "../pairing-trace";
  import { runSharedFloorplanMutationAfterContinuity } from "../state/shared-floorplan-write";

  type WizardStep = "select" | "resume" | "wait" | "verify" | "floorplan" | "fallback" | "complete";

  type Props = {
    baseUrl: string;
    candidates: PlatformDiscoveryCandidate[];
    initialAddress?: string;
    resumeExistingSetup?: boolean;
    requesterAddress: string;
    discoveryScanning: boolean;
    fetcher?: typeof fetch;
    messages: Messages;
    mockMode?: boolean;
    pendingSetupCount?: number;
    onscan: () => void | Promise<void>;
    oncancel: () => void;
    oncomplete: () => void | Promise<void>;
    onlater: (
      participantId: string,
      prepareCommunityFallback: boolean
    ) => void | Promise<void>;
    oncalibrate: (
      participantId: string,
      address: string,
      prepareCommunityFallback: boolean
    ) => void | Promise<void>;
    onownerrequired: () => void | Promise<void>;
  };

  let {
    baseUrl,
    candidates,
    initialAddress = "",
    resumeExistingSetup = false,
    requesterAddress,
    discoveryScanning,
    fetcher,
    messages,
    mockMode = false,
    pendingSetupCount = 0,
    onscan,
    oncancel,
    oncomplete,
    onlater,
    oncalibrate,
    onownerrequired
  }: Props = $props();
  const initialSetupState = (() => ({
    address: initialAddress,
    resume: resumeExistingSetup && Boolean(initialAddress)
  }))();
  let step = $state<WizardStep>(initialSetupState.resume ? "resume" : "select");
  let selectedAddress = $state(initialSetupState.address);
  let manualAddress = $state(initialSetupState.address);
  let connection = $state<PlatformPairingConnection | null>(null);
  let localFloorplan = $state<FloorplanStorageStatus | null>(null);
  let peerFloorplan = $state<FloorplanStorageStatus | null>(null);
  let localFloorplanPreviewUrl = $state("");
  let peerFloorplanPreviewUrl = $state("");
  let floorplansShareIdentity = $state(false);
  let floorplanSource = $state<PairingFloorplanSource | null>(null);
  let canonicalFloorplanLocked = $state(false);
  let localParticipantId = $state("");
  let setupParticipantId = $state("");
  let prepareCommunityFallback = $state(false);
  let confirmWithoutFallback = $state(false);
  let busy = $state(initialSetupState.resume);
  let error = $state("");
  let errorCode = $state("");
  let waitSeconds = $state(60);
  let cancelled = false;
  let resumeStarted = false;

  const client = $derived(createPlatformPairingClient({ fetcher }));
  const pairingText = $derived(messages.dashboard.pairing);
  const text = $derived(messages.dashboard.pairing.setup);
  const selectedCandidate = $derived(
    candidates.find((candidate) => candidate.address === selectedAddress) ?? null
  );
  const selectedName = $derived(selectedCandidate?.hostname || selectedAddress || text.otherDevice);
  const canContinueFromFloorplan = $derived(floorplanSource !== null);

  $effect(() => {
    if (!resumeExistingSetup || !initialAddress || resumeStarted) return;
    resumeStarted = true;
    void startWaiting();
  });

  onDestroy(() => {
    cancelled = true;
    replaceFloorplanPreview("local", "");
    replaceFloorplanPreview("peer", "");
  });

  function normalizeDeviceUrl(value: string): string {
    const trimmed = value.trim().replace(/\/+$/, "");
    if (!trimmed) return window.location.origin;
    return /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`;
  }

  function dashboardUrl(value: string, returnTo = ""): string {
    const url = new URL("/dashboard", normalizeDeviceUrl(value));
    url.searchParams.set("tab", "pairing");
    if (returnTo) url.searchParams.set("returnTo", returnTo);
    return url.toString();
  }

  function localReturnUrl(): string {
    return dashboardUrl(baseUrl);
  }

  function peerDashboardUrl(): string {
    return dashboardUrl(selectedAddress, localReturnUrl());
  }

  function floorplanImageUrl(value: string): string {
    return new URL("/api/floorplan/image", normalizeDeviceUrl(value)).toString();
  }

  function replaceFloorplanPreview(side: "local" | "peer", nextUrl: string): void {
    const currentUrl = side === "local" ? localFloorplanPreviewUrl : peerFloorplanPreviewUrl;
    if (currentUrl.startsWith("blob:")) URL.revokeObjectURL(currentUrl);
    if (side === "local") localFloorplanPreviewUrl = nextUrl;
    else peerFloorplanPreviewUrl = nextUrl;
  }

  async function loadMockFloorplanPreview(
    side: "local" | "peer",
    status: FloorplanStorageStatus,
    deviceBaseUrl: string
  ): Promise<void> {
    if (!mockMode || !hasCompleteFloorplanStorage(status)) {
      replaceFloorplanPreview(side, "");
      return;
    }
    try {
      const image = await loadFloorplanStorageImage({ baseUrl: deviceBaseUrl, fetcher });
      const previewUrl = URL.createObjectURL(image);
      if (cancelled) {
        URL.revokeObjectURL(previewUrl);
        return;
      }
      replaceFloorplanPreview(side, previewUrl);
    } catch {
      replaceFloorplanPreview(side, "");
    }
  }

  function selectCandidate(candidate: PlatformDiscoveryCandidate): void {
    selectedAddress = candidate.address;
    manualAddress = candidate.address;
    error = "";
  }

  function selectManualAddress(): void {
    selectedAddress = manualAddress.trim();
    error = "";
  }

  async function startWaiting(): Promise<void> {
    const address = (selectedAddress || manualAddress).trim();
    if (!address) return;
    selectedAddress = address;
    busy = true;
    error = "";
    errorCode = "";
    try {
      const [
        localStatus,
        remoteStatus,
        localParticipants,
        remoteParticipants,
        localSite,
        localDescriptor,
        remoteDescriptor
      ] = await tracePairingPhase("setup.preflight", () => Promise.all([
          loadFloorplanStorageStatus({ baseUrl, fetcher }),
          loadFloorplanStorageStatus({ baseUrl: address, fetcher }),
          client.getParticipants(baseUrl),
          client.getParticipants(address),
          client.getSiteConfiguration(baseUrl),
          client.getDescriptor(baseUrl),
          client.getDescriptor(address)
        ]), { peer: normalizeDeviceUrl(address) });
      if (!localSite.localOwner) {
        throw new PlatformPairingError(
          "site_configuration_owner_required", 403,
          "site_configuration_owner_required"
        );
      }
      localParticipantId = localDescriptor.participantId;
      const relationship = pairingSetupRelationship(
        localParticipants,
        remoteParticipants,
        localDescriptor.participantId,
        remoteDescriptor.participantId
      );
      if (resumeExistingSetup && relationship !== "resume") {
        throw new PlatformPairingError("pairing_request_failed", 409, "pairing_request_failed");
      }
      if (relationship === "blocked") {
        throw new PlatformPairingError("peer_manages_existing_devices", 409, "peer_manages_existing_devices");
      }
      canonicalFloorplanLocked = pairingKeepsCanonicalFloorplan(localSite);
      if (!hasCompleteFloorplanStorage(localStatus) && !hasCompleteFloorplanStorage(remoteStatus)) {
        throw new PlatformPairingError("floorplan_required", 409, "floorplan_required");
      }
      localFloorplan = localStatus;
      peerFloorplan = remoteStatus;
      if (mockMode) {
        await Promise.all([
          loadMockFloorplanPreview("local", localStatus, baseUrl),
          loadMockFloorplanPreview("peer", remoteStatus, address)
        ]);
      }
      const [localDocument, remoteDocument] = await Promise.all([
        hasCompleteFloorplanStorage(localStatus)
          ? loadFloorplanStorageDocument({ baseUrl, fetcher })
          : Promise.resolve(null),
        hasCompleteFloorplanStorage(remoteStatus)
          ? loadFloorplanStorageDocument({ baseUrl: address, fetcher })
          : Promise.resolve(null)
      ]);
      const decision = canonicalFloorplanLocked
        ? "local"
        : pairingFloorplanDecision(
            localStatus,
            remoteStatus,
            localDocument,
            remoteDocument
          );
      floorplansShareIdentity = Boolean(
        localDocument &&
        remoteDocument &&
        localDocument.floorplanId === remoteDocument.floorplanId &&
        localDocument.geometryVersion === remoteDocument.geometryVersion
      );
      floorplanSource = decision === "local" || decision === "peer" ? decision : null;
      setupParticipantId = remoteDescriptor.participantId;
      if (relationship === "resume") {
        if (!sameFloorplan()) {
          step = "floorplan";
          return;
        }
        if (mockMode) {
          await tracePairingPhase(
            "pairing.resume_mock_sync",
            () => client.syncSiteConfiguration(address, localDescriptor.participantId),
            { peer: normalizeDeviceUrl(address) }
          );
          await advanceToCalibrationChoice(remoteDescriptor.participantId);
          return;
        }
        await tracePairingPhase(
          "pairing.resume_sync",
          () => requestSiteSync(localDescriptor.participantId),
          { peer: normalizeDeviceUrl(address) }
        );
        await tracePairingPhase(
          "pairing.resume_convergence",
          () => client.waitForSiteConvergence(
            address,
            siteConvergenceExpectation(localSite)
          ),
          { peer: normalizeDeviceUrl(address) }
        );
        await advanceToCalibrationChoice(remoteDescriptor.participantId);
        return;
      }
      await tracePairingPhase(
        "pairing.invitation",
        () => client.requestInvitation(address, requesterAddress),
        { peer: normalizeDeviceUrl(address) }
      );
      step = "wait";
      await tracePairingPhase(
        "pairing.permission_wait",
        () => waitForPermission(),
        { peer: normalizeDeviceUrl(address) }
      );
    } catch (cause) {
      await handleSetupError(cause);
    } finally {
      busy = false;
    }
  }

  async function waitForPermission(): Promise<void> {
    const deadline = Date.now() + PAIRING_INVITATION_DURATION_MS;
    while (!cancelled && Date.now() < deadline) {
      waitSeconds = Math.max(0, Math.ceil((deadline - Date.now()) / 1000));
      try {
        const status = await client.getStatus(selectedAddress);
        if (status.invitation?.state === "rejected") {
          throw new PlatformPairingError("peer_rejected", 409, "peer_rejected");
        }
        if (status.state === "prepared" && status.role === "joiner") {
          connection = await tracePairingPhase(
            "pairing.connect",
            () => client.connectLeader(baseUrl, selectedAddress),
            { peer: normalizeDeviceUrl(selectedAddress) }
          );
          step = "verify";
          return;
        }
      } catch (cause) {
        if (cause instanceof PlatformPairingError && cause.code === "peer_rejected") {
          throw cause;
        }
        // A transient Wi-Fi request failure during the bounded wait is not a
        // pairing decision. Keep polling until the visible timeout expires.
      }
      await new Promise((resolve) => setTimeout(resolve, 1000));
    }
    if (!cancelled) throw new PlatformPairingError("peer_not_ready", 408, "peer_not_ready");
  }

  async function confirmCode(): Promise<void> {
    if (!connection) return;
    busy = true;
    error = "";
    errorCode = "";
    try {
      await tracePairingPhase(
        "pairing.confirm",
        () => client.confirmPair(baseUrl, selectedAddress, connection!.confirmationCode),
        { peer: normalizeDeviceUrl(selectedAddress) }
      );
      step = "floorplan";
    } catch (cause) {
      await handleSetupError(cause);
    } finally {
      busy = false;
    }
  }

  async function applyFloorplan(): Promise<void> {
    if (!floorplanSource) return;
    busy = true;
    error = "";
    try {
      const participantId = connection?.local.peerParticipantId || setupParticipantId;
      if (!participantId) {
        throw new PlatformPairingError(
          "pairing_request_failed",
          409,
          "pairing_request_failed"
        );
      }
      if (mockMode) {
        const pairedSite = await client.getSiteConfiguration(baseUrl);
        if (!pairedSite.localOwner || !pairedSite.configurationOwnerNodeId) {
          throw new PlatformPairingError("site_owner_not_local", 409, "site_owner_not_local");
        }
        await tracePairingPhase(
          "floorplan.mock_sync",
          () => client.syncSiteConfiguration(selectedAddress, pairedSite.configurationOwnerNodeId),
          { peer: normalizeDeviceUrl(selectedAddress) }
        );
        await advanceToCalibrationChoice(participantId);
        return;
      }
      const pairedSite = await client.getSiteConfiguration(baseUrl);
      const pairedOwnerNodeId = pairedSite.configurationOwnerNodeId;
      if (!pairedSite.localOwner || !pairedOwnerNodeId) {
        throw new PlatformPairingError(
          "site_owner_not_local",
          409,
          "site_owner_not_local"
        );
      }
      const localSite = await deviceStorageQueue.run("floorplan", async () => {
        await tracePairingPhase(
          "floorplan.copy",
          () => copyPairingFloorplanToLocal(
            floorplanSource!,
            { baseUrl, fetcher },
            { baseUrl: selectedAddress, fetcher }
          ),
          { source: floorplanSource, peer: normalizeDeviceUrl(selectedAddress) }
        );
        return tracePairingPhase(
          "floorplan.commit",
          () => runSharedFloorplanMutationAfterContinuity({
            readSite: () => client.getSiteConfiguration(baseUrl),
            mutation: () => client.commitLocalFloorplan(baseUrl),
            timeoutMs: 60000
          }),
          { peer: normalizeDeviceUrl(selectedAddress) }
        );
      });
      const ownerNodeId = localSite.configurationOwnerNodeId;
      const floorplan = localSite.floorplan;
      if (!localSite.localOwner || !ownerNodeId || !floorplan.committed ||
          !floorplan.floorplanId) {
        throw new PlatformPairingError(
          "site_owner_not_local",
          409,
          "site_owner_not_local"
        );
      }
      await tracePairingPhase(
        "floorplan.sync_after_commit",
        () => requestSiteSync(ownerNodeId),
        { peer: normalizeDeviceUrl(selectedAddress) }
      );
      await tracePairingPhase(
        "floorplan.convergence",
        () => client.waitForSiteConvergence(
          selectedAddress,
          siteConvergenceExpectation(localSite)
        ),
        { peer: normalizeDeviceUrl(selectedAddress) }
      );
      await advanceToCalibrationChoice(participantId);
    } catch (cause) {
      await handleSetupError(cause);
    } finally {
      busy = false;
    }
  }

  async function advanceToCalibrationChoice(participantId: string): Promise<void> {
    const [site, participants] = await Promise.all([
      client.getSiteConfiguration(baseUrl),
      client.getParticipants(baseUrl)
    ]);
    setupParticipantId = participantId;
    prepareCommunityFallback = false;
    confirmWithoutFallback = false;
    step = shouldOfferCommunityFallbackPreparation(
      site,
      participants.participants,
      localParticipantId,
      participantId
    ) ? "fallback" : "complete";
    await oncomplete();
  }

  function acceptFallbackPreparation(): void {
    prepareCommunityFallback = true;
    confirmWithoutFallback = false;
    step = "complete";
  }

  function declineFallbackPreparation(): void {
    if (!confirmWithoutFallback) {
      confirmWithoutFallback = true;
      return;
    }
    prepareCommunityFallback = false;
    confirmWithoutFallback = false;
    step = "complete";
  }

  async function requestSiteSync(ownerNodeId: string): Promise<void> {
    try {
      await client.syncSiteConfiguration(selectedAddress, ownerNodeId);
    } catch (cause) {
      if (!(cause instanceof PlatformPairingError) ||
          (cause.code !== "owner_offline" && cause.code !== "busy")) {
        throw cause;
      }
      // Authenticated heartbeat discovery may finish just after pairing.
      // Automatic replication and the bounded continuity wait remain active.
    }
  }

  async function cancelWizard(): Promise<void> {
    cancelled = true;
    if (step === "wait" || step === "verify") {
      await Promise.allSettled([client.cancel(baseUrl), client.cancel(selectedAddress)]);
    }
    oncancel();
  }

  async function startCalibration(): Promise<void> {
    const participantId = setupParticipantId || connection?.local.peerParticipantId;
    if (!participantId || !selectedAddress) {
      error = text.error("pairing_request_failed");
      return;
    }
    busy = true;
    error = "";
    try {
      await oncalibrate(participantId, selectedAddress, prepareCommunityFallback);
    } catch (cause) {
      error = errorMessage(cause);
    } finally {
      busy = false;
    }
  }

  async function deferCalibration(): Promise<void> {
    const participantId = setupParticipantId || connection?.local.peerParticipantId;
    if (!participantId) {
      error = text.error("pairing_request_failed");
      return;
    }
    await onlater(participantId, prepareCommunityFallback);
  }

  function floorplanSummary(status: FloorplanStorageStatus | null): string {
    if (!status || !hasCompleteFloorplanStorage(status)) return text.noFloorplan;
    return text.floorplanSummary(status.configBytes + status.imageBytes);
  }

  function sameFloorplan(): boolean {
    return pairingFloorplansMatch(
      localFloorplan,
      peerFloorplan,
      floorplansShareIdentity
    );
  }

  function errorMessage(cause: unknown): string {
    const code = cause instanceof PlatformPairingError
      ? cause.code
      : cause instanceof Error
        ? cause.message
        : "pairing_request_failed";
    errorCode = code;
    if (mockMode && code === "floorplan_required") return text.mockFloorplanRequired;
    return text.error(code);
  }

  async function handleSetupError(cause: unknown): Promise<void> {
    const code = cause instanceof PlatformPairingError
      ? cause.code
      : cause instanceof Error
        ? cause.message
        : "pairing_request_failed";
    if (code === "site_configuration_owner_required" ||
        code === "site_owner_not_local") {
      await onownerrequired();
      return;
    }
    error = errorMessage(cause);
  }
</script>

<div class="dashboard-dialog-backdrop pairing-setup-backdrop" role="presentation">
  <div
    class="floorplan-panel pairing-setup-panel"
    role="dialog"
    aria-modal="true"
    aria-labelledby="pairing-setup-title"
  >
    <aside class="floorplan-controls">
      <div class="floorplan-workflow-card">
        <div>
          <strong id="pairing-setup-title">{text.title}</strong>
          <span>{text.description}</span>
        </div>

        <ol class="pairing-setup-progress">
          <li data-active={step === "select" || step === "wait"}>{text.steps.device}</li>
          <li data-active={step === "verify"}>{text.steps.verify}</li>
          <li data-active={step === "resume" || step === "floorplan"}>{text.steps.floorplan}</li>
          <li data-active={step === "fallback" || step === "complete"}>{text.steps.complete}</li>
        </ol>
      </div>

      <div class="floorplan-step-actions">
        <button
          type="button"
          class="floorplan-step-button floorplan-fixed-text"
          disabled={busy && step !== "wait"}
          onclick={cancelWizard}
        >
          <span>{messages.common.cancel}</span>
          <strong>{text.close}</strong>
        </button>
        <button type="button" class="floorplan-next-button floorplan-fixed-text" disabled>
          <span>{text.currentStep}</span>
          <strong>{text.steps[step === "wait"
            ? "device"
            : step === "resume" ? "floorplan"
            : step === "fallback" ? "complete" : step]}</strong>
        </button>
      </div>
    </aside>

    <div class="floorplan-workspace-column pairing-setup-workspace">
      {#if step === "resume"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.floorplanTitle}</strong>
            <span>{busy ? text.applying : text.floorplanDescription}</span>
          </div>
          <button type="button" disabled={busy} onclick={startWaiting}>
            {busy ? text.applying : text.selectContinue}
          </button>
        </div>
      {:else if step === "select"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.selectTitle}</strong>
            <span>{text.selectDescription}</span>
          </div>
          {#if pendingSetupCount > 0}
            <div class="floorplan-status" data-tone="warn" role="status">
              <strong>{pairingText.pendingSetupTitle(pendingSetupCount)}</strong>
              <span>{pairingText.pendingSetupAddDescription}</span>
            </div>
          {/if}
          <div class="pairing-setup-scan-row">
            <span>{discoveryScanning ? pairingText.searching : pairingText.nearbyDevices}</span>
            <button type="button" disabled={busy || discoveryScanning} onclick={onscan}>
              {pairingText.refreshDevices}
            </button>
          </div>
          {#if candidates.length > 0}
            <div class="pairing-setup-options">
              {#each candidates as candidate (candidate.participantId)}
                <button
                  type="button"
                  class="pairing-setup-option"
                  data-selected={selectedAddress === candidate.address}
                  onclick={() => selectCandidate(candidate)}
                >
                  <strong>{candidate.hostname || text.otherDevice}</strong>
                  <span>{candidate.address}</span>
                </button>
              {/each}
            </div>
          {:else if !discoveryScanning}
            <div class="floorplan-empty-note">
              <strong>{pairingText.noDevices}</strong>
              <span>{pairingText.noDevicesDescription}</span>
            </div>
          {/if}
          <label class="pairing-setup-address">
            <span>{text.manualAddress}</span>
            <input
              type="text"
              bind:value={manualAddress}
              placeholder="192.168.1.50"
              oninput={selectManualAddress}
            />
          </label>
          <button type="button" disabled={busy || !(selectedAddress || manualAddress).trim()} onclick={startWaiting}>
            {text.selectContinue}
          </button>
        </div>
      {:else if step === "wait"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.waitTitle(selectedName)}</strong>
            <span>{text.waitDescription(waitSeconds)}</span>
          </div>
          <a class="pairing-setup-link" href={peerDashboardUrl()} target="_blank" rel="noreferrer">
            <strong>{text.openOtherDevice}</strong>
            <span>{selectedAddress}</span>
          </a>
          <div class="floorplan-status" data-tone="warn">
            <strong>{text.waiting}</strong>
            <span>{text.waitingHelp}</span>
          </div>
        </div>
      {:else if step === "verify"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.verifyTitle}</strong>
            <span>{text.verifyDescription}</span>
          </div>
          <div class="floorplan-edit-tool-card pairing-setup-code">
            <span>{text.code}</span>
            <strong>{connection?.confirmationCode}</strong>
          </div>
          <button type="button" disabled={busy} onclick={confirmCode}>{text.codeMatches}</button>
        </div>
      {:else if step === "floorplan"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.floorplanTitle}</strong>
            <span>{canonicalFloorplanLocked
              ? text.existingSiteFloorplan
              : sameFloorplan() ? text.sameFloorplan : text.floorplanDescription}</span>
          </div>
          <div class="pairing-setup-options pairing-floorplan-options">
            <button
              type="button"
              class="pairing-setup-option"
              data-selected={floorplanSource === "local"}
              disabled={!localFloorplan || !hasCompleteFloorplanStorage(localFloorplan)}
              onclick={() => (floorplanSource = "local")}
            >
              {#if localFloorplan && hasCompleteFloorplanStorage(localFloorplan)}
                <img src={localFloorplanPreviewUrl || floorplanImageUrl(baseUrl)} alt="" />
              {/if}
              <strong>{text.thisFloorplan}</strong>
              <span>{floorplanSummary(localFloorplan)}</span>
            </button>
            <button
              type="button"
              class="pairing-setup-option"
              data-selected={floorplanSource === "peer"}
              disabled={!peerFloorplan || !hasCompleteFloorplanStorage(peerFloorplan)}
              onclick={() => (floorplanSource = "peer")}
              hidden={canonicalFloorplanLocked}
            >
              {#if peerFloorplan && hasCompleteFloorplanStorage(peerFloorplan)}
                <img src={peerFloorplanPreviewUrl || floorplanImageUrl(selectedAddress)} alt="" />
              {/if}
              <strong>{text.otherFloorplan(selectedName)}</strong>
              <span>{floorplanSummary(peerFloorplan)}</span>
            </button>
          </div>
          <div class="floorplan-empty-note">
            <strong>{text.radarPositionKept}</strong>
            <span>{text.radarPositionHelp}</span>
          </div>
          <button type="button" disabled={busy || !canContinueFromFloorplan} onclick={applyFloorplan}>
            {busy ? text.applying : text.apply}
          </button>
        </div>
      {:else if step === "fallback"}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.fallbackTitle}</strong>
            <span>{text.fallbackDescription}</span>
          </div>
          {#if confirmWithoutFallback}
            <div class="floorplan-status" data-tone="warn" role="alert">
              <strong>{text.fallbackDeclineTitle}</strong>
              <span>{text.fallbackDeclineDescription}</span>
            </div>
            <div class="pairing-setup-complete-actions">
              <button
                type="button"
                class="floorplan-step-button"
                disabled={busy}
                onclick={() => (confirmWithoutFallback = false)}
              >
                {text.fallbackGoBack}
              </button>
              <button type="button" disabled={busy} onclick={declineFallbackPreparation}>
                {text.fallbackContinueWithout}
              </button>
            </div>
          {:else}
            <div class="pairing-setup-complete-actions">
              <button
                type="button"
                class="floorplan-step-button"
                disabled={busy}
                onclick={declineFallbackPreparation}
              >
                {text.fallbackDecline}
              </button>
              <button type="button" disabled={busy} onclick={acceptFallbackPreparation}>
                {text.fallbackAccept}
              </button>
            </div>
          {/if}
        </div>
      {:else}
        <div class="floorplan-workflow-card pairing-setup-stage">
          <div>
            <strong>{text.completeTitle}</strong>
            <span>{text.completeDescription}</span>
          </div>
          <div class="pairing-setup-complete-actions">
            <button
              type="button"
              class="floorplan-step-button"
              disabled={busy}
              onclick={deferCalibration}
            >
              {text.later}
            </button>
            <button type="button" disabled={busy} onclick={startCalibration}>
              {text.startCalibration}
            </button>
          </div>
        </div>
      {/if}

      {#if error}
        <p class="floorplan-save-status" data-tone="error" role="alert">{error}</p>
        {#if errorCode === "peer_manages_existing_devices" || errorCode === "site_conflict"}
          <a class="pairing-setup-link" href={peerDashboardUrl()} target="_blank" rel="noreferrer">
            <strong>{text.manageExisting}</strong>
            <span>{selectedAddress}</span>
          </a>
        {/if}
      {/if}
    </div>
  </div>
</div>

<style>
  .pairing-setup-panel {
    grid-template-columns: minmax(300px, 380px) minmax(0, 760px);
    width: min(1160px, calc(100vw - 32px));
    max-height: min(860px, calc(100vh - 32px));
    overflow: auto;
    background: #111820;
    box-shadow: 0 28px 84px rgba(0, 0, 0, 0.62);
  }

  .pairing-setup-backdrop {
    background: rgba(2, 8, 12, 0.82);
  }

  .pairing-setup-workspace,
  .pairing-setup-stage,
  .pairing-setup-options,
  .pairing-setup-address {
    display: grid;
    gap: 10px;
  }

  .pairing-setup-scan-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
  }

  .pairing-setup-scan-row > span {
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .pairing-setup-progress {
    display: grid;
    gap: 6px;
    margin: 0;
    padding: 0;
    list-style: none;
  }

  .pairing-setup-progress li {
    padding: 7px 8px;
    border-left: 2px solid rgba(123, 184, 216, 0.2);
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .pairing-setup-progress li[data-active="true"] {
    border-color: #06d6a0;
    color: #d7eefc;
    font-weight: var(--rz-weight-label);
  }

  .pairing-setup-options {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .pairing-setup-option,
  .pairing-setup-link {
    display: grid;
    gap: 4px;
    min-width: 0;
    padding: 10px;
    border: 1px solid rgba(123, 184, 216, 0.22);
    border-radius: 6px;
    background: rgba(123, 184, 216, 0.06);
    color: #d7eefc;
    text-align: left;
    text-decoration: none;
  }

  .pairing-setup-option[data-selected="true"] {
    border-color: rgba(6, 214, 160, 0.62);
    background: rgba(6, 214, 160, 0.12);
  }

  .pairing-setup-option strong,
  .pairing-setup-link strong {
    overflow-wrap: anywhere;
    font-size: var(--rz-font-title-card);
  }

  .pairing-floorplan-options img {
    width: 100%;
    aspect-ratio: 16 / 10;
    object-fit: contain;
    border-radius: 4px;
    background: rgba(17, 24, 32, 0.36);
  }

  .pairing-setup-option span,
  .pairing-setup-link span,
  .pairing-setup-address span {
    overflow-wrap: anywhere;
    color: var(--rz-muted);
    font-size: var(--rz-font-help);
  }

  .pairing-setup-address input {
    width: 100%;
  }

  .pairing-setup-complete-actions {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    gap: 10px;
  }

  .pairing-setup-code {
    place-items: center;
    text-align: center;
  }

  .pairing-setup-code strong {
    font-size: 28px;
    letter-spacing: 0;
  }

  @media (max-width: 760px) {
    .pairing-setup-panel {
      grid-template-columns: 1fr;
    }

    .pairing-setup-options {
      grid-template-columns: 1fr;
    }

    .pairing-setup-complete-actions {
      grid-template-columns: 1fr;
    }
  }
</style>
