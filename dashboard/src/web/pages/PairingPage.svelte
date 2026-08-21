<script lang="ts">
  import { onDestroy, untrack } from "svelte";
  import {
    availablePairingCandidates,
    communityFallbackControlAvailable,
    createPlatformPairingClient,
    onlineLeaderEligibleCount,
    pairingCalibrationAvailability,
    pairingConnectionGuard,
    pairingFloorplanUsage,
    pairingDisplay,
    pairingReturnFlowActive,
    pairingSetupResumeAvailability,
    participantIsOnline,
    participantOnlineAddress,
    participantUsesSiteMemberRemoval,
    PlatformPairingError,
    rememberDiscoveryCandidates,
    type PlatformDiscoveryCandidate,
    type PlatformPairingStatus,
    type PlatformParticipant,
    type PlatformParticipantsStatus,
    type PlatformSiteConfigurationStatus
  } from "../api/platform-pairing";
  import {
    hasCompleteFloorplanStorage,
    loadFloorplanStorageStatus,
    type FloorplanStorageStatus
  } from "../floorplan/floorplan-storage-client";
  import {
    mockPlatformRemovalDelayMs,
    removeMockPlatformParticipant
  } from "../api/mock-platform";
  import type { Messages } from "../i18n";
  import {
    pairingCalibrationPlacementGate,
    type PairingRadarPlacementDevice
  } from "../pairing-floorplan";
  import { pairingDeviceAddressLabel, pairingDeviceShortName } from "../pairing-device-label";
  import {
    emitPairingTrace,
    pairingTraceErrorDetail,
    tracePairingPhase
  } from "../pairing-trace";
  import type { WebSystemStatus } from "../types";
  import PairingCalibrationDialog from "../components/PairingCalibrationDialog.svelte";
  import PairingSetupWizard from "../components/PairingSetupWizard.svelte";

  type Props = {
    available: boolean;
    baseUrl: string;
    fetcher?: typeof fetch;
    messages: Messages;
    mockMode?: boolean;
    systemStatus: WebSystemStatus | null;
  };

  let { available, baseUrl, fetcher, messages, mockMode = false, systemStatus }: Props = $props();
  let status = $state<PlatformPairingStatus | null>(null);
  let participantsStatus = $state<PlatformParticipantsStatus | null>(null);
  let siteConfiguration = $state<PlatformSiteConfigurationStatus | null>(null);
  let localFloorplanStatus = $state<FloorplanStorageStatus | null>(null);
  let candidates = $state<PlatformDiscoveryCandidate[]>([]);
  let rememberedCandidates = $state<PlatformDiscoveryCandidate[]>([]);
  let manualPairAddress = $state("");
  let discoveryScanning = $state(false);
  let busy = $state(false);
  let polling = false;
  let error = $state("");
  let notice = $state("");
  let pollTimer: ReturnType<typeof setTimeout> | undefined;
  let pollGeneration = 0;
  let disconnectParticipant = $state<PlatformParticipant | null>(null);
  let disconnectAddress = $state("");
  let calibrationParticipant = $state<PlatformParticipant | null>(null);
  let calibrationAddress = $state("");
  let calibrationFallbackParticipantId = $state("");
  let pendingFallbackParticipantId = $state("");
  let setupWizardOpen = $state(false);
  let setupResumeRequested = $state(false);
  let connectionGuard = $state<"owner" | "continuity" | "">("");
  let connectionGuardOwnerUrl = $state("");
  let calibrationPlacementTargets = $state<Array<{ name: string; url: string }>>([]);
  let returnTo = $state(
    safeDashboardReturnUrl(new URLSearchParams(window.location.search).get("returnTo"))
  );

  const client = $derived(createPlatformPairingClient({ fetcher }));
  const text = $derived(messages.dashboard.pairing);
  const display = $derived(pairingDisplay(status, participantsStatus));
  const registeredParticipants = $derived(
    (participantsStatus?.participants ?? []).filter((participant) =>
      participant.canonicalMember === true ||
      participant.credential?.provisioned === true
    )
  );
  const managementAllowed = $derived(
    participantsStatus?.managementAllowed === true &&
      siteConfiguration?.localOwner === true
  );
  const disconnectIsSiteLeave = $derived(
    disconnectParticipant !== null &&
      !managementAllowed &&
      siteConfiguration?.localOwner === false &&
      disconnectParticipant.participantId === siteConfiguration.configurationOwnerNodeId
  );
  const disconnectIsCanonicalRemoval = $derived(
    disconnectParticipant !== null &&
      managementAllowed &&
      participantUsesSiteMemberRemoval(disconnectParticipant)
  );
  const connectedParticipants = $derived(
    registeredParticipants.filter((participant) => participant.membership !== "revoked")
  );
  const pendingSetupParticipants = $derived(
    connectedParticipants.filter((participant) => participant.membership === "quarantined")
  );
  const pendingDisconnectParticipants = $derived(
    registeredParticipants.filter((participant) => participant.membership === "revoked")
  );
  const localNodeId = $derived(systemStatus?.platform?.nodeId ?? "");
  const communityFallbackParticipants = $derived(
    connectedParticipants.filter((participant) =>
      communityFallbackControlAvailable(participant, siteConfiguration?.localOwner === true)
    )
  );
  const onlineFallbackDeviceCount = $derived(
    onlineLeaderEligibleCount(
      siteConfiguration,
      connectedParticipants,
      localNodeId
    )
  );
  const showSingleFallbackWarning = $derived(
    Boolean(siteConfiguration?.members?.length) &&
      connectedParticipants.some((participant) => participant.membership === "active") &&
      onlineFallbackDeviceCount <= 1
  );
  const nearbyCandidates = $derived(availablePairingCandidates(candidates, registeredParticipants));
  const localFloorplanUsage = $derived(
    pairingFloorplanUsage(
      localFloorplanStatus ? hasCompleteFloorplanStorage(localFloorplanStatus) : false,
      connectedParticipants.length,
      siteConfiguration?.localOwner === true,
      siteConfiguration?.configurationOwnerNodeId
    )
  );
  const returnFlowActive = $derived(
    pairingReturnFlowActive(returnTo, status, participantsStatus)
  );

  $effect(() => {
    const deviceBaseUrl = baseUrl;
    if (!available) {
      stopPolling();
      return;
    }
    untrack(() => {
      const generation = pollGeneration;
      void refreshAll(true, deviceBaseUrl);
      schedulePoll(deviceBaseUrl, generation);
    });
    return stopPolling;
  });

  onDestroy(stopPolling);

  function stopPolling(): void {
    pollGeneration += 1;
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = undefined;
  }

  function schedulePoll(deviceBaseUrl: string, generation = pollGeneration): void {
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = undefined;
    if (!available || generation !== pollGeneration) return;
    pollTimer = setTimeout(async () => {
      pollTimer = undefined;
      await refreshAll(false, deviceBaseUrl);
      if (generation === pollGeneration) schedulePoll(deviceBaseUrl, generation);
    }, 1500);
  }

  function messageForError(cause: unknown): string {
    const code = cause instanceof PlatformPairingError ? cause.code : "pairing_request_failed";
    return text.error(code);
  }

  async function refreshAll(reportError = true, deviceBaseUrl = baseUrl): Promise<void> {
    if (!available || polling) return;
    polling = true;
    try {
      const [nextStatus, nextParticipants, nextDiscovery, nextSiteConfiguration, nextFloorplanStatus] = await Promise.all([
        client.getStatus(deviceBaseUrl),
        client.getParticipants(deviceBaseUrl),
        client.getDiscovery(deviceBaseUrl),
        client.getSiteConfiguration(deviceBaseUrl),
        loadFloorplanStorageStatus({ baseUrl: deviceBaseUrl, fetcher })
      ]);
      status = nextStatus;
      participantsStatus = nextParticipants;
      candidates = nextDiscovery.candidates;
      rememberedCandidates = rememberDiscoveryCandidates(rememberedCandidates, nextDiscovery.candidates);
      siteConfiguration = nextSiteConfiguration;
      localFloorplanStatus = nextFloorplanStatus;
      if (returnTo && !pairingReturnFlowActive(returnTo, nextStatus, nextParticipants)) {
        clearReturnTo();
      }
      if (reportError) error = "";
    } catch (cause) {
      if (reportError && !busy) error = messageForError(cause);
    } finally {
      polling = false;
    }
  }

  async function scanDevices(deviceBaseUrl = baseUrl): Promise<void> {
    if (!available || discoveryScanning) return;
    discoveryScanning = true;
    error = "";
    try {
      const discovered = (await client.scan(deviceBaseUrl)).candidates;
      candidates = discovered;
      rememberedCandidates = rememberDiscoveryCandidates(rememberedCandidates, discovered);
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      discoveryScanning = false;
    }
  }

  function deviceDashboardUrl(address: string, tab: "floorplan" | "pairing"): string {
    const value = address.trim();
    const origin = !value
      ? window.location.origin
      : /^https?:\/\//i.test(value) ? value : `http://${value}`;
    const url = new URL("/dashboard", origin);
    url.searchParams.set("tab", tab);
    url.searchParams.set("returnTo", window.location.href);
    return url.toString();
  }

  function devicePairingUrl(address: string): string {
    return address ? deviceDashboardUrl(address, "pairing") : "";
  }

  function applyConnectionGuard(
    nextSite: PlatformSiteConfigurationStatus,
    nextParticipants: PlatformParticipantsStatus
  ): boolean {
    const guard = pairingConnectionGuard(nextSite, nextParticipants);
    if (guard.allowed) {
      connectionGuardOwnerUrl = "";
      connectionGuard = "";
      return true;
    }
    connectionGuardOwnerUrl = devicePairingUrl(guard.ownerAddress);
    connectionGuard = guard.reason;
    return false;
  }

  async function openSetupWizard(address = "", resumeExistingSetup = false): Promise<void> {
    if (busy) return;
    setupResumeRequested = false;
    busy = true;
    error = "";
    notice = "";
    try {
      const [nextSite, nextParticipants] = await Promise.all([
        client.getSiteConfiguration(baseUrl),
        client.getParticipants(baseUrl)
      ]);
      siteConfiguration = nextSite;
      participantsStatus = nextParticipants;
      if (!applyConnectionGuard(nextSite, nextParticipants)) return;
      manualPairAddress = address.trim();
      setupResumeRequested = resumeExistingSetup;
      setupWizardOpen = true;
      if (!resumeExistingSetup) void scanDevices();
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  function closeSetupWizard(): void {
    setupWizardOpen = false;
    setupResumeRequested = false;
  }

  function deferSetupCalibration(
    participantId: string,
    prepareCommunityFallback: boolean
  ): void {
    pendingFallbackParticipantId = prepareCommunityFallback ? participantId : "";
    setupWizardOpen = false;
    setupResumeRequested = false;
  }

  function setupResumeAvailability(participant: PlatformParticipant) {
    return pairingSetupResumeAvailability(
      siteConfiguration,
      participant,
      setupResumeAddress(participant)
    );
  }

  async function resumeSetup(participant: PlatformParticipant): Promise<void> {
    if (setupResumeAvailability(participant) !== "ready") return;
    const address = setupResumeAddress(participant);
    if (!address) return;
    await openSetupWizard(address, true);
  }

  async function setupComplete(): Promise<void> {
    await refreshAll(true);
    notice = text.confirmed;
  }

  async function showCurrentOwnerGuard(): Promise<void> {
    setupWizardOpen = false;
    try {
      const [nextSite, nextParticipants] = await Promise.all([
        client.getSiteConfiguration(baseUrl),
        client.getParticipants(baseUrl)
      ]);
      siteConfiguration = nextSite;
      participantsStatus = nextParticipants;
      if (applyConnectionGuard(nextSite, nextParticipants)) {
        connectionGuardOwnerUrl = "";
        connectionGuard = "continuity";
      }
    } catch {
      const currentSite = siteConfiguration;
      const currentParticipants = participantsStatus;
      if (currentSite && currentParticipants &&
          !applyConnectionGuard(currentSite, currentParticipants)) return;
      connectionGuardOwnerUrl = "";
      connectionGuard = "continuity";
    }
  }

  async function startSetupCalibration(
    participantId: string,
    address: string,
    prepareCommunityFallback: boolean
  ): Promise<void> {
    await refreshAll(true);
    const participant = participantsStatus?.participants.find(
      (value) => value.participantId === participantId
    );
    if (!participant) {
      throw new PlatformPairingError("pairing_request_failed", 409, "pairing_request_failed");
    }
    pendingFallbackParticipantId = prepareCommunityFallback ? participantId : "";
    setupWizardOpen = false;
    await openCalibration(participant, address, prepareCommunityFallback);
  }

  async function cancelPairing(): Promise<void> {
    busy = true;
    error = "";
    try {
      status = await client.cancel(baseUrl);
      clearReturnTo();
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  function candidateFor(participantId: string): PlatformDiscoveryCandidate | undefined {
    return rememberedCandidates.find((candidate) => candidate.participantId === participantId);
  }

  function openDisconnect(participant: PlatformParticipant): void {
    disconnectParticipant = participant;
    disconnectAddress = participantAddress(participant);
    error = "";
    notice = "";
  }

  function closeDisconnect(): void {
    if (busy) return;
    disconnectParticipant = null;
    disconnectAddress = "";
  }

  async function disconnectSelected(): Promise<void> {
    if (!disconnectParticipant || !disconnectAddress.trim() || !localNodeId) return;
    const canonicalRemoval = disconnectIsCanonicalRemoval;
    busy = true;
    error = "";
    try {
      if (mockMode) {
        const participantId = disconnectParticipant.participantId;
        await new Promise<void>((resolve) => {
          window.setTimeout(resolve, mockPlatformRemovalDelayMs());
        });
        if (!removeMockPlatformParticipant(participantId)) {
          throw new PlatformPairingError("participant_not_found", 404, "participant_not_found");
        }
        [participantsStatus, siteConfiguration] = await Promise.all([
          client.getParticipants(baseUrl),
          client.getSiteConfiguration(baseUrl)
        ]);
        clearReturnTo();
        notice = canonicalRemoval ? text.siteMemberRemoved : text.disconnected;
        disconnectParticipant = null;
        disconnectAddress = "";
        return;
      }
      if (disconnectIsSiteLeave) {
        const result = await client.leaveSite(
          baseUrl,
          disconnectAddress,
          localNodeId,
          disconnectParticipant.participantId
        );
        notice = text.leftSite;
        disconnectParticipant = null;
        disconnectAddress = "";
        window.setTimeout(() => window.location.reload(), Math.max(result.reset.rebootInMs + 1000, 2500));
        return;
      }
      if (canonicalRemoval) {
        const result = await tracePairingPhase(
          "removal.transaction",
          () => client.removeSiteMember(
            baseUrl,
            disconnectAddress,
            localNodeId,
            disconnectParticipant!.participantId
          ),
          {
            ownerParticipantId: localNodeId,
            peerParticipantId: disconnectParticipant.participantId
          }
        );
        participantsStatus = result.owner;
        clearReturnTo();
        notice = text.siteMemberRemoved;
        disconnectParticipant = null;
        disconnectAddress = "";
        return;
      }
      const result = await client.disconnectPair(
        baseUrl,
        disconnectAddress,
        localNodeId,
        disconnectParticipant.participantId
      );
      participantsStatus = result.local;
      clearReturnTo();
      notice = text.disconnected;
      disconnectParticipant = null;
      disconnectAddress = "";
    } catch (cause) {
      error = messageForError(cause);
      if (canonicalRemoval && disconnectParticipant) {
        const failedParticipantId = disconnectParticipant.participantId;
        try {
          participantsStatus = await client.getParticipants(baseUrl);
          disconnectParticipant = participantsStatus.participants.find(
            (participant) => participant.participantId === failedParticipantId
          ) ?? disconnectParticipant;
          const participant = participantsStatus.participants.find(
            (candidate) => candidate.participantId === failedParticipantId
          );
          emitPairingTrace("removal.ui.error_readback", "event", {
            peerParticipantId: failedParticipantId,
            error: pairingTraceErrorDetail(cause),
            targetPresent: participant !== undefined,
            targetMembership: participant?.membership ?? null,
            targetCanonicalMembership: participant?.canonicalMembership ?? null,
            targetDirectMembership: participant?.directMembership ?? null,
            targetCanonicalMember: participant?.canonicalMember ?? false,
            targetDirectPair: participant?.directPair ?? false,
            targetCredentialEnabled: participant?.credential.enabled ?? false
          });
        } catch {
          // Keep the actionable transaction error when status refresh also fails.
          emitPairingTrace("removal.ui.error_readback", "error", {
            peerParticipantId: failedParticipantId,
            error: pairingTraceErrorDetail(cause),
            readback: "failed"
          });
        }
      }
    } finally {
      busy = false;
    }
  }

  async function markDisconnectPending(): Promise<void> {
    if (!disconnectParticipant) return;
    busy = true;
    error = "";
    try {
      participantsStatus = await client.revokeParticipant(baseUrl, disconnectParticipant.participantId);
      clearReturnTo();
      notice = text.disconnectPendingNotice;
      disconnectParticipant = null;
      disconnectAddress = "";
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  async function forceForgetLocalParticipant(): Promise<void> {
    if (!disconnectParticipant || disconnectParticipant.membership !== "revoked") return;
    busy = true;
    error = "";
    try {
      participantsStatus = await client.forgetParticipant(baseUrl, disconnectParticipant.participantId);
      clearReturnTo();
      notice = disconnectIsCanonicalRemoval
        ? text.siteMemberForceRemoved
        : text.localRecordDeleted;
      disconnectParticipant = null;
      disconnectAddress = "";
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  async function toggleCommunityFallback(participant: PlatformParticipant): Promise<void> {
    if (!communityFallbackControlAvailable(
      participant,
      siteConfiguration?.localOwner === true
    )) return;
    const eligible = participant.leaderEligible !== true;
    busy = true;
    error = "";
    notice = "";
    try {
      participantsStatus = await client.setCommunityFallback(
        baseUrl,
        participant.participantId,
        eligible
      );
      siteConfiguration = await client.getSiteConfiguration(baseUrl);
      notice = eligible
        ? text.automaticFallbackEnabledNotice
        : text.automaticFallbackDisabledNotice;
    } catch (cause) {
      error = messageForError(cause);
    } finally {
      busy = false;
    }
  }

  function networkLabel(participant: PlatformParticipant): string {
    return participantIsOnline(participant) ? text.networkOnline : text.networkUnavailable;
  }

  function participantAddress(participant: PlatformParticipant): string {
    const heartbeatAddress = participantOnlineAddress(participant);
    if (heartbeatAddress) return heartbeatAddress;
    if (!participantIsOnline(participant)) return "";
    return candidateFor(participant.participantId)?.address || "";
  }

  function setupResumeAddress(participant: PlatformParticipant): string {
    return participant.endpoint?.address ||
      candidateFor(participant.participantId)?.address || "";
  }

  function membershipLabel(participant: PlatformParticipant): string {
    if (participant.membership === "active") return text.memberships.active;
    if (participant.membership === "revoked") return text.memberships.revoked;
    return text.memberships.quarantined;
  }

  function participantStatusLabel(participant: PlatformParticipant): string {
    return membershipLabel(participant);
  }

  function readinessLabel(participant: PlatformParticipant): string {
    return participant.readiness?.ready ? text.ready : text.notReady;
  }

  function deviceName(participant: PlatformParticipant): string {
    return pairingDeviceShortName(candidateFor(participant.participantId)?.hostname, text.setup.otherDevice);
  }

  function deviceDisplayAddress(participant: PlatformParticipant): string {
    const address = participant.endpoint?.address || candidateFor(participant.participantId)?.address || "";
    return pairingDeviceAddressLabel(address) || "-";
  }

  function connectionSummary(): string {
    if (display.state !== "idle" && display.state !== "committed") return text.states[display.state];
    if (connectedParticipants.length === 0) return text.noConnectedDevices;
    return text.connectedCount(connectedParticipants.length);
  }

  function floorplanUsageLabel(): string {
    return text.floorplanUsageStates[localFloorplanUsage];
  }

  function localPairingAddress(): string {
    const value = baseUrl.trim() || window.location.origin;
    try {
      const normalized = /^https?:\/\//i.test(value) ? value : `http://${value}`;
      return new URL(normalized).hostname;
    } catch {
      return window.location.hostname;
    }
  }

  function deviceAddressLabel(address: string): string {
    return pairingDeviceAddressLabel(address);
  }

  function showCalibrationPlacementTargets(
    participant: PlatformParticipant,
    address: string,
    devices: PairingRadarPlacementDevice[]
  ): void {
    calibrationPlacementTargets = devices.map((device) => {
      const local = device === "local";
      const deviceAddress = local ? localPairingAddress() : deviceAddressLabel(address);
      const deviceLabel = local ? text.calibration.thisDevice : deviceName(participant);
      return {
        name: `${deviceLabel} (${deviceAddress})`,
        url: deviceDashboardUrl(local ? baseUrl : address, "floorplan")
      };
    });
  }

  function canCalibrate(participant: PlatformParticipant): boolean {
    return calibrationAvailability(participant) === "ready";
  }

  function calibrationAvailability(participant: PlatformParticipant) {
    return pairingCalibrationAvailability(
      siteConfiguration,
      participant
    );
  }

  async function openCalibration(
    participant: PlatformParticipant,
    preferredAddress = "",
    prepareCommunityFallback = false
  ): Promise<void> {
    if (busy) return;
    const address = preferredAddress || participantAddress(participant);
    if (!address) return;
    busy = true;
    error = "";
    notice = "";
    calibrationFallbackParticipantId = "";
    calibrationPlacementTargets = [];
    try {
      const placementGate = await pairingCalibrationPlacementGate(
        { baseUrl, fetcher },
        { baseUrl: address, fetcher }
      );
      if (placementGate.state === "unavailable") {
        error = text.calibration.placementCheckFailed;
        return;
      }
      if (placementGate.state === "missing") {
        showCalibrationPlacementTargets(participant, address, placementGate.devices);
        return;
      }

      stopPolling();
      calibrationParticipant = participant;
      calibrationAddress = address;
      calibrationFallbackParticipantId = prepareCommunityFallback ||
          pendingFallbackParticipantId === participant.participantId
        ? participant.participantId
        : "";
    } catch {
      error = text.calibration.placementCheckFailed;
    } finally {
      busy = false;
    }
  }

  function calibrationPlacementMissing(devices: PairingRadarPlacementDevice[]): void {
    if (!calibrationParticipant || !calibrationAddress) return;
    const participant = calibrationParticipant;
    const address = calibrationAddress;
    calibrationParticipant = null;
    calibrationAddress = "";
    calibrationFallbackParticipantId = "";
    showCalibrationPlacementTargets(participant, address, devices);
    schedulePoll(baseUrl);
  }

  function closeCalibration(): void {
    calibrationParticipant = null;
    calibrationAddress = "";
    calibrationFallbackParticipantId = "";
    schedulePoll(baseUrl);
  }

  async function calibrationComplete(): Promise<void> {
    const fallbackParticipantId = calibrationFallbackParticipantId;
    calibrationParticipant = null;
    calibrationAddress = "";
    calibrationFallbackParticipantId = "";
    if (pendingFallbackParticipantId === fallbackParticipantId) {
      pendingFallbackParticipantId = "";
    }
    await refreshAll(true);
    notice = text.calibration.activated;
    if (fallbackParticipantId) {
      const participant = participantsStatus?.participants.find(
        (candidate) => candidate.participantId === fallbackParticipantId
      );
      if (participant?.leaderEligible === true) {
        notice = text.automaticFallbackPreparedNotice;
      } else if (participant && communityFallbackControlAvailable(
        participant,
        siteConfiguration?.localOwner === true
      )) {
        try {
          participantsStatus = await client.setCommunityFallback(
            baseUrl,
            fallbackParticipantId,
            true
          );
          siteConfiguration = await client.getSiteConfiguration(baseUrl);
          const updated = participantsStatus.participants.find(
            (candidate) => candidate.participantId === fallbackParticipantId
          );
          if (updated?.leaderEligible !== true) {
            throw new PlatformPairingError(
              "participant_mutation_not_applied",
              409,
              "participant_mutation_not_applied"
            );
          }
          notice = text.automaticFallbackPreparedNotice;
        } catch {
          error = text.automaticFallbackPreparationFailed;
        }
      } else {
        error = text.automaticFallbackPreparationFailed;
      }
    }
    schedulePoll(baseUrl);
  }

  function safeDashboardReturnUrl(value: string | null): string {
    if (!value) return "";
    try {
      const url = new URL(value);
      if (url.protocol !== "http:" && url.protocol !== "https:") return "";
      url.searchParams.set("tab", "pairing");
      return url.toString();
    } catch {
      return "";
    }
  }

  function clearReturnTo(): void {
    if (!returnTo) return;
    const url = new URL(window.location.href);
    url.searchParams.delete("returnTo");
    window.history.replaceState(window.history.state, "", url);
    returnTo = "";
  }
</script>

<section class="pairing-workspace">
  <div class="floorplan-workflow-card pairing-intro-card">
    <div>
      <strong>{text.title}</strong>
      <span>{text.description}</span>
    </div>
  </div>

  {#if !available}
    <div class="floorplan-workflow-card pairing-unavailable">
      <strong>{text.unavailable}</strong>
    </div>
  {:else}
    <div class="pairing-layout">
      <aside class="pairing-side-column">
        <div class="floorplan-workflow-card">
          <div>
            <strong>{text.localDevice}</strong>
            <span>{text.localDeviceDescription}</span>
          </div>
          <dl class="floorplan-stored-summary">
            <div>
              <dt>{text.state}</dt>
              <dd>{connectionSummary()}</dd>
            </div>
            <div>
              <dt>{text.floorplanUsage}</dt>
              <dd>{floorplanUsageLabel()}</dd>
            </div>
          </dl>
          {#if returnTo && returnFlowActive}
            <a class="pairing-return-link" href={returnTo}>{text.returnToMainDevice}</a>
          {/if}
          <button type="button" disabled={busy} onclick={() => openSetupWizard()}>{text.connectTitle}</button>
        </div>

        {#if status && status.state !== "idle" && status.state !== "committed"}
          <button class="pairing-cancel-button" type="button" disabled={busy} onclick={cancelPairing}>
            {text.cancel}
          </button>
        {/if}
      </aside>

      <section class="pairing-main-column">
        <div class="floorplan-workflow-card pairing-list-section">
          <div class="pairing-section-heading">
            <div>
              <strong>{text.connectedDevices}</strong>
              <span>{text.connectedDescription}</span>
            </div>
            <span>{connectedParticipants.length} / {participantsStatus?.registry?.maxRemoteParticipants ?? 19}</span>
          </div>

          {#if connectedParticipants.length === 0}
            <div class="floorplan-empty-note pairing-empty">
              <strong>{text.noConnectedDevices}</strong>
              <span>{text.noConnectedDescription}</span>
            </div>
          {:else}
            {#if communityFallbackParticipants.length > 0}
              <p class="pairing-fallback-help">{text.automaticFallbackDescription}</p>
            {/if}
            {#if showSingleFallbackWarning}
              <p class="floorplan-save-status" data-tone="saving" role="status">
                {text.singleFallbackWarning}
              </p>
            {/if}
            {#if pendingSetupParticipants.length > 0}
              <p class="floorplan-save-status" data-tone="saving" role="status">
                {text.pendingSetupDescription(pendingSetupParticipants.length)}
              </p>
            {/if}
            <div class="pairing-device-list">
              {#each connectedParticipants as participant (participant.participantId)}
                <div class="pairing-device-row" class:pairing-device-row-offline={!participantIsOnline(participant)}>
                  <div class="pairing-device-copy">
                    <strong>{deviceDisplayAddress(participant)}</strong>
                  </div>
                  <dl class="pairing-device-meta">
                    <div>
                      <dt>{text.membership}</dt>
                      <dd>{participantStatusLabel(participant)}</dd>
                    </div>
                    <div>
                      <dt>{text.readiness}</dt>
                      <dd>{readinessLabel(participant)}</dd>
                    </div>
                    <div>
                      <dt>{text.networkState}</dt>
                      <dd>{networkLabel(participant)}</dd>
                    </div>
                    <div>
                      <dt>{text.deviceName}</dt>
                      <dd>{deviceName(participant)}</dd>
                    </div>
                    {#if communityFallbackControlAvailable(participant, siteConfiguration?.localOwner === true)}
                      <div>
                        <dt>{text.automaticFallback}</dt>
                        <dd>
                          {participant.leaderEligible === true
                            ? text.automaticFallbackAllowed
                            : text.automaticFallbackNotAllowed}
                        </dd>
                      </div>
                    {/if}
                  </dl>
                  <div class="pairing-device-actions">
                    {#if communityFallbackControlAvailable(participant, siteConfiguration?.localOwner === true)}
                      <button
                        type="button"
                        disabled={busy}
                        onclick={() => toggleCommunityFallback(participant)}
                      >
                        {participant.leaderEligible === true
                          ? text.automaticFallbackDisable
                          : text.automaticFallbackAllow}
                      </button>
                    {/if}
                    {#if managementAllowed && setupResumeAvailability(participant) !== "hidden"}
                      <button
                        type="button"
                        disabled={busy || setupResumeAvailability(participant) !== "ready"}
                        onclick={() => resumeSetup(participant)}
                      >
                        {text.resumeSetup}
                      </button>
                    {:else if managementAllowed && calibrationAvailability(participant) !== "hidden"}
                      <button type="button" disabled={busy || !canCalibrate(participant)} onclick={() => openCalibration(participant)}>
                        {participant.membership === "active"
                          ? text.calibration.recalibrate
                          : calibrationAvailability(participant) === "waiting"
                            ? text.calibration.preparing
                            : text.calibration.start}
                      </button>
                    {/if}
                    {#if managementAllowed || participant.participantId === siteConfiguration?.configurationOwnerNodeId}
                      <button class="danger-button" type="button" disabled={busy} onclick={() => openDisconnect(participant)}>
                        {managementAllowed ? text.disconnect : text.leaveSite}
                      </button>
                    {/if}
                  </div>
                </div>
              {/each}
            </div>
          {/if}

          {#if pendingDisconnectParticipants.length > 0}
            <div class="pairing-pending-section">
              <div class="pairing-section-heading">
                <div>
                  <strong>{text.pendingDisconnects}</strong>
                  <span>{text.pendingDisconnectsDescription}</span>
                </div>
              </div>
              <div class="pairing-device-list">
                {#each pendingDisconnectParticipants as participant (participant.participantId)}
                  <div class="pairing-device-row">
                    <div class="pairing-device-copy">
                      <strong>{deviceDisplayAddress(participant)}</strong>
                    </div>
                    <dl class="pairing-device-meta">
                      <div>
                        <dt>{text.membership}</dt>
                        <dd>{membershipLabel(participant)}</dd>
                      </div>
                      <div>
                        <dt>{text.networkState}</dt>
                        <dd>{networkLabel(participant)}</dd>
                      </div>
                      <div>
                        <dt>{text.deviceName}</dt>
                        <dd>{deviceName(participant)}</dd>
                      </div>
                    </dl>
                    {#if managementAllowed || participant.participantId === siteConfiguration?.configurationOwnerNodeId}
                      <button type="button" disabled={busy} onclick={() => openDisconnect(participant)}>
                        {managementAllowed ? text.finishDisconnect : text.leaveSite}
                      </button>
                    {/if}
                  </div>
                {/each}
              </div>
            </div>
          {/if}
        </div>

        {#if notice}
          <p class="floorplan-save-status" data-tone="ok" role="status">{notice}</p>
        {/if}
        {#if error}
          <p class="floorplan-save-status" data-tone="error" role="alert">{error}</p>
        {/if}
      </section>
    </div>
  {/if}
</section>

{#if setupWizardOpen}
  <PairingSetupWizard
    {baseUrl}
    candidates={nearbyCandidates}
    initialAddress={manualPairAddress}
    resumeExistingSetup={setupResumeRequested}
    requesterAddress={localPairingAddress()}
    {discoveryScanning}
    {fetcher}
    {messages}
    {mockMode}
    pendingSetupCount={pendingSetupParticipants.length}
    onscan={() => scanDevices()}
    oncancel={closeSetupWizard}
    oncomplete={setupComplete}
    onlater={deferSetupCalibration}
    oncalibrate={startSetupCalibration}
    onownerrequired={showCurrentOwnerGuard}
  />
{/if}

{#if connectionGuard}
  <div class="dashboard-dialog-backdrop" role="presentation">
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="pairing-owner-required-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="pairing-owner-required-title">
          {connectionGuard === "owner"
            ? text.ownerRequiredTitle
            : text.connectionNotReadyTitle}
        </strong>
        <span>
          {connectionGuard === "owner"
            ? text.ownerRequiredDescription
            : text.connectionNotReadyDescription}
        </span>
      </div>
      {#if connectionGuard === "owner" && !connectionGuardOwnerUrl}
        <div class="dashboard-dialog-section">
          <span>{text.ownerUnavailable}</span>
        </div>
      {/if}
      <div class="dashboard-dialog-actions">
        {#if connectionGuardOwnerUrl}
          <button type="button" onclick={() => window.location.assign(connectionGuardOwnerUrl)}>
            {text.openOwnerDevice}
          </button>
        {/if}
        <button
          type="button"
          class="dashboard-dialog-close-button"
          onclick={() => (connectionGuard = "")}
        >
          {messages.common.close}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if calibrationPlacementTargets.length > 0}
  <div class="dashboard-dialog-backdrop" role="presentation">
    <div
      class="floorplan-delete-dialog dashboard-control-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="pairing-radar-placement-required-title"
    >
      <div class="dashboard-dialog-heading">
        <strong id="pairing-radar-placement-required-title">
          {text.calibration.placementRequiredTitle}
        </strong>
        <span>
          {text.calibration.placementRequiredDescription(
            calibrationPlacementTargets.map((target) => target.name).join(", ")
          )}
        </span>
      </div>
      <div class="dashboard-dialog-actions">
        {#each calibrationPlacementTargets as target (target.url)}
          <button type="button" onclick={() => window.location.assign(target.url)}>
            {text.calibration.openRadarPlacement(target.name)}
          </button>
        {/each}
        <button
          type="button"
          class="dashboard-dialog-close-button"
          onclick={() => (calibrationPlacementTargets = [])}
        >
          {messages.common.close}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if disconnectParticipant}
  <div class="dashboard-dialog-backdrop" role="presentation">
    <div class="floorplan-delete-dialog dashboard-control-dialog" role="dialog" aria-modal="true" aria-labelledby="pairing-disconnect-title">
      <div class="dashboard-dialog-heading">
        <strong id="pairing-disconnect-title">
          {disconnectIsSiteLeave
            ? text.leaveSiteTitle
            : disconnectIsCanonicalRemoval
              ? text.siteMemberRemoveTitle
              : text.disconnectTitle}
        </strong>
        <span>
          {disconnectIsSiteLeave
            ? text.leaveSiteDescription
            : disconnectParticipant.membership === "revoked"
            ? text.pendingDisconnectDescription(deviceName(disconnectParticipant))
            : disconnectIsCanonicalRemoval
            ? text.siteMemberRemoveDescription(deviceName(disconnectParticipant))
            : text.disconnectDescription(deviceName(disconnectParticipant))}
        </span>
      </div>
      <div class="dashboard-dialog-section pairing-disconnect-address">
        <label for="pairing-disconnect-address">{text.address}</label>
        <input
          id="pairing-disconnect-address"
          type="text"
          bind:value={disconnectAddress}
          placeholder="192.168.1.50"
          disabled={busy}
        />
        <span>
          {disconnectIsSiteLeave
            ? text.leaveSiteAddressHelp
            : disconnectIsCanonicalRemoval
              ? text.siteMemberRemoveAddressHelp
              : text.disconnectAddressHelp}
        </span>
      </div>
      {#if !disconnectIsSiteLeave && disconnectParticipant.membership === "revoked"}
        <p class="pairing-local-delete-warning">
          {disconnectIsCanonicalRemoval ? text.siteMemberForceRemoveWarning : text.localDeleteWarning}
        </p>
      {/if}
      {#if error}<p class="floorplan-delete-error" role="alert">{error}</p>{/if}
      <div class="floorplan-delete-dialog-actions">
        <button type="button" disabled={busy} onclick={closeDisconnect}>{messages.common.cancel}</button>
        {#if !disconnectIsSiteLeave && disconnectParticipant.membership === "revoked"}
          <button class="danger-button" type="button" disabled={busy} onclick={forceForgetLocalParticipant}>
            {disconnectIsCanonicalRemoval ? text.siteMemberForceRemove : text.deleteLocalRecord}
          </button>
        {:else if !disconnectIsSiteLeave}
          <button type="button" disabled={busy} onclick={markDisconnectPending}>
            {text.blockAndWait}
          </button>
        {/if}
        <button class="danger-button" type="button" disabled={busy || !disconnectAddress.trim() || !localNodeId} onclick={disconnectSelected}>
          {busy
            ? (disconnectIsSiteLeave
              ? text.leavingSite
              : disconnectIsCanonicalRemoval
                ? text.siteMemberRemoving
                : text.disconnecting)
            : disconnectIsSiteLeave
              ? text.leaveSiteConfirm
              : disconnectIsCanonicalRemoval
              ? text.siteMemberRemoveConfirm
              : disconnectParticipant.membership === "revoked"
              ? text.retryDisconnect
              : text.disconnectConfirm}
        </button>
      </div>
    </div>
  </div>
{/if}

{#if calibrationParticipant && calibrationAddress}
  <PairingCalibrationDialog
    {baseUrl}
    peerAddress={calibrationAddress}
    participant={calibrationParticipant}
    participantName={deviceName(calibrationParticipant)}
    {localNodeId}
    prepareCommunityFallback={calibrationFallbackParticipantId === calibrationParticipant.participantId}
    {fetcher}
    {messages}
    {mockMode}
    onclose={closeCalibration}
    oncomplete={calibrationComplete}
    onplacementmissing={calibrationPlacementMissing}
  />
{/if}

<style>
  .pairing-workspace {
    --pairing-main-width: 680px;
    display: grid;
    gap: 12px;
    width: min(100%, calc(var(--rz-side-column-width) + var(--rz-page-gap) + var(--pairing-main-width)));
    margin: 0 auto;
  }

  .pairing-section-heading {
    display: flex;
    align-items: start;
    justify-content: space-between;
    gap: 12px;
  }

  .pairing-intro-card {
    grid-template-columns: 1fr;
  }

  .pairing-intro-card > div {
    display: grid;
    gap: 8px;
  }

  .pairing-intro-card strong {
    font-size: var(--rz-font-title-section);
    font-weight: var(--rz-weight-title);
  }

  .pairing-intro-card span,
  .pairing-section-heading span,
  .pairing-disconnect-address > span {
    color: rgba(215, 238, 252, 0.68);
    font-size: var(--rz-font-help);
    line-height: 1.45;
  }

  .pairing-layout {
    display: grid;
    grid-template-columns: minmax(260px, var(--rz-side-column-width)) minmax(0, var(--pairing-main-width));
    gap: var(--rz-page-gap);
    align-items: start;
    min-width: 0;
  }

  .pairing-side-column,
  .pairing-main-column,
  .pairing-device-list,
  .pairing-list-section,
  .pairing-disconnect-address {
    display: grid;
    gap: 10px;
    min-width: 0;
  }

  .pairing-disconnect-address label {
    color: rgba(215, 238, 252, 0.72);
    font-size: var(--rz-font-label);
    font-weight: var(--rz-weight-label);
  }

  .pairing-disconnect-address input {
    width: 100%;
    min-height: 34px;
    box-sizing: border-box;
  }

  .pairing-return-link {
    display: grid;
    place-items: center;
    min-height: 34px;
    padding: 0 10px;
    border: 1px solid rgba(6, 214, 160, 0.32);
    border-radius: 6px;
    background: rgba(6, 214, 160, 0.1);
    color: #d7eefc;
    font-size: var(--rz-font-button);
    font-weight: var(--rz-weight-button);
    text-decoration: none;
  }

  .pairing-cancel-button {
    width: 100%;
  }

  .pairing-section-heading > div {
    display: grid;
    gap: 4px;
  }

  .pairing-pending-section {
    display: grid;
    gap: 10px;
    padding-top: 12px;
    border-top: 1px solid rgba(123, 184, 216, 0.14);
  }

  .pairing-local-delete-warning {
    margin: 0;
    color: rgba(255, 204, 153, 0.88);
    font-size: var(--rz-font-help);
    line-height: 1.45;
  }

  .pairing-fallback-help {
    margin: 0;
    color: rgba(215, 238, 252, 0.68);
    font-size: var(--rz-font-help);
    line-height: 1.45;
  }

  .pairing-section-heading strong {
    font-size: var(--rz-font-title-card);
    font-weight: var(--rz-weight-title);
  }

  .pairing-device-row {
    min-width: 0;
    padding: 11px 12px;
    border: 1px solid rgba(123, 184, 216, 0.16);
    border-radius: 6px;
    background: rgba(17, 24, 32, 0.4);
  }

  .pairing-device-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(240px, 0.9fr) auto;
    align-items: center;
    gap: 12px;
  }

  .pairing-device-row-offline {
    border-color: rgba(255, 90, 96, 0.86);
    background: rgba(86, 22, 28, 0.38);
    animation: pairing-device-offline-pulse 1.35s ease-in-out infinite;
  }

  @keyframes pairing-device-offline-pulse {
    0%,
    100% {
      border-color: rgba(255, 90, 96, 0.58);
      box-shadow: 0 0 0 1px rgba(255, 90, 96, 0.08);
    }

    50% {
      border-color: rgba(255, 90, 96, 1);
      box-shadow: 0 0 0 1px rgba(255, 90, 96, 0.34), 0 0 18px rgba(255, 54, 64, 0.32);
    }
  }

  .pairing-device-copy {
    display: grid;
    gap: 3px;
    min-width: 0;
  }

  .pairing-device-copy strong {
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .pairing-device-copy strong {
    font-size: var(--rz-font-title-card);
    font-weight: var(--rz-weight-title);
  }

  .pairing-device-meta {
    display: grid;
    gap: 4px;
    margin: 0;
  }

  .pairing-device-meta div {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    font-size: var(--rz-font-help);
  }

  .pairing-device-meta dt {
    color: rgba(215, 238, 252, 0.58);
  }

  .pairing-device-meta dd {
    margin: 0;
    color: rgba(230, 244, 255, 0.9);
    font-weight: var(--rz-weight-label);
  }

  .pairing-device-actions {
    display: grid;
    grid-template-columns: minmax(152px, 176px);
    justify-content: end;
    gap: 6px;
  }

  .pairing-device-actions > button {
    width: 100%;
    min-height: 34px;
    padding: 6px 11px;
    white-space: normal;
  }

  .pairing-empty,
  .pairing-unavailable {
    min-height: 96px;
    align-content: center;
  }

  @media (max-width: 980px) {
    .pairing-workspace,
    .pairing-layout {
      width: 100%;
    }

    .pairing-layout {
      grid-template-columns: 1fr;
    }
  }

  @media (max-width: 680px) {
    .pairing-section-heading {
      align-items: stretch;
      flex-direction: column;
    }

    .pairing-device-row {
      grid-template-columns: 1fr;
    }

    .pairing-device-actions {
      width: min(100%, 176px);
      justify-self: end;
    }
  }

  @media (prefers-reduced-motion: reduce) {
    .pairing-device-row-offline {
      animation: none;
      border-color: rgba(255, 90, 96, 1);
      box-shadow: 0 0 0 1px rgba(255, 90, 96, 0.28);
    }
  }
</style>
