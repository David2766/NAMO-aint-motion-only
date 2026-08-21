import { describe, expect, it } from "vitest";
import appSource from "./App.svelte?raw";
import dashboardPanelSource from "./components/DashboardPanel.svelte?raw";
import floorplanRadarPlacementOverlaySource from "./components/FloorplanRadarPlacementOverlay.svelte?raw";
import floorplanImageFrameSource from "./components/floorplan/FloorplanImageFrame.svelte?raw";
import storedFloorplanEditorSource from "./components/floorplan/StoredFloorplanEditor.svelte?raw";
import incomingPairingDialogSource from "./components/IncomingPairingDialog.svelte?raw";
import pairingSetupWizardSource from "./components/PairingSetupWizard.svelte?raw";
import pairingCalibrationDialogSource from "./components/PairingCalibrationDialog.svelte?raw";
import pairingPageSource from "./pages/PairingPage.svelte?raw";
import platformCalibrationSource from "./platform-calibration.ts?raw";

describe("dashboard layout regressions", () => {
  it("keeps the compact fusion status beside the global connection pill", () => {
    const fusionPill = appSource.indexOf('class="status-pill fusion-status-pill"');
    const connectionPill = appSource.indexOf('class="status-pill" data-status');

    expect(fusionPill).toBeGreaterThan(-1);
    expect(connectionPill).toBeGreaterThan(fusionPill);
    expect(appSource).toContain("{#if fusionSummary.visible}");
    expect(dashboardPanelSource).not.toContain("fusionStatusText");
  });

  it("keeps device operations visible in one compact vertical column", () => {
    expect(pairingPageSource).not.toContain("PairingDeviceActions");
    expect(pairingPageSource).toContain('class="pairing-device-actions"');
    expect(pairingPageSource).toContain("grid-template-columns: minmax(152px, 176px)");
    expect(pairingPageSource).toContain("text.automaticFallbackDisable");
    expect(pairingPageSource).toContain("text.calibration.recalibrate");
    expect(pairingPageSource).toContain("text.disconnect");
    expect(pairingPageSource).toContain("deviceDisplayAddress(participant)");
    expect(pairingPageSource).toContain("text.deviceName");
    expect(pairingPageSource).not.toContain("deviceSettings");
  });

  it("explains pending device setup without blocking another connection", () => {
    expect(pairingPageSource).toContain("const pendingSetupParticipants = $derived(");
    expect(pairingPageSource).toContain('participant.membership === "quarantined"');
    expect(pairingPageSource).toContain("text.pendingSetupDescription(pendingSetupParticipants.length)");
    expect(pairingPageSource).toContain("pendingSetupCount={pendingSetupParticipants.length}");
    expect(pairingPageSource).toContain("text.calibration.preparing");
    expect(pairingSetupWizardSource).toContain("pendingSetupCount = 0");
    expect(pairingSetupWizardSource).toContain("pairingText.pendingSetupTitle(pendingSetupCount)");
    expect(pairingSetupWizardSource).toContain("pairingText.pendingSetupAddDescription");
  });

  it("keeps fallback consent and applies it after activation convergence", () => {
    const fallbackPrompt = pairingSetupWizardSource.indexOf('step === "fallback"');
    const calibrationChoice = pairingSetupWizardSource.indexOf("text.completeTitle");
    const canonicalActivation = platformCalibrationSource.indexOf("() => client.activateParticipant(");
    const activationConvergence = platformCalibrationSource.indexOf(
      '"calibration.convergence_after_activation"',
      canonicalActivation
    );
    const fallbackMutation = platformCalibrationSource.indexOf("await enableCommunityFallback(", activationConvergence);
    const finalConvergence = platformCalibrationSource.indexOf("await convergeCalibrationSite(", fallbackMutation);

    expect(fallbackPrompt).toBeGreaterThan(-1);
    expect(calibrationChoice).toBeGreaterThan(fallbackPrompt);
    expect(pairingSetupWizardSource).not.toContain("setCommunityFallback");
    expect(canonicalActivation).toBeGreaterThan(-1);
    expect(activationConvergence).toBeGreaterThan(canonicalActivation);
    expect(fallbackMutation).toBeGreaterThan(activationConvergence);
    expect(finalConvergence).toBeGreaterThan(fallbackMutation);
    expect(pairingSetupWizardSource).toContain("text.fallbackDeclineDescription");
    expect(pairingSetupWizardSource).toContain("await onlater(participantId, prepareCommunityFallback)");
    expect(pairingPageSource).toContain("pendingFallbackParticipantId === participant.participantId");
  });

  it("resumes an incomplete pairing at Floorplan selection before synchronization", () => {
    const resumeBranch = pairingSetupWizardSource.indexOf('relationship === "resume"');
    const floorplanGate = pairingSetupWizardSource.indexOf("if (!sameFloorplan())", resumeBranch);
    const resumeSync = pairingSetupWizardSource.indexOf('"pairing.resume_sync"', resumeBranch);
    const floorplanCommit = pairingSetupWizardSource.indexOf('"floorplan.commit"');
    const floorplanSync = pairingSetupWizardSource.indexOf('"floorplan.sync_after_commit"');
    const applyFloorplan = pairingSetupWizardSource.indexOf("async function applyFloorplan()");
    const resumedParticipant = pairingSetupWizardSource.indexOf(
      "const participantId = connection?.local.peerParticipantId || setupParticipantId;",
      applyFloorplan
    );
    const floorplanCopy = pairingSetupWizardSource.indexOf('"floorplan.copy"', applyFloorplan);

    expect(resumeBranch).toBeGreaterThan(-1);
    expect(floorplanGate).toBeGreaterThan(resumeBranch);
    expect(resumeSync).toBeGreaterThan(floorplanGate);
    expect(floorplanCommit).toBeGreaterThan(-1);
    expect(floorplanSync).toBeGreaterThan(floorplanCommit);
    expect(pairingSetupWizardSource).not.toContain("floorplan.sync_before_commit");
    expect(pairingPageSource).toContain("await openSetupWizard(address, true)");
    expect(pairingPageSource).toContain("resumeExistingSetup={setupResumeRequested}");
    expect(pairingSetupWizardSource).toContain('resumeExistingSetup && relationship !== "resume"');
    expect(pairingSetupWizardSource).toContain("void startWaiting()");
    expect(appSource).toContain("{mockMode}");
    expect(pairingPageSource).toContain("{mockMode}");
    expect(pairingSetupWizardSource).toContain("if (mockMode) {");
    expect(pairingSetupWizardSource).toContain('"floorplan.mock_sync"');
    expect(pairingSetupWizardSource).toContain("await advanceToCalibrationChoice(participantId)");
    expect(pairingSetupWizardSource).toContain("loadFloorplanStorageImage");
    expect(pairingSetupWizardSource).toContain("URL.createObjectURL(image)");
    expect(pairingSetupWizardSource).toContain('mockMode && code === "floorplan_required"');
    expect(pairingSetupWizardSource).toContain('"pairing.resume_mock_sync"');
    expect(applyFloorplan).toBeGreaterThan(-1);
    expect(resumedParticipant).toBeGreaterThan(applyFloorplan);
    expect(floorplanCopy).toBeGreaterThan(resumedParticipant);
    expect(
      pairingSetupWizardSource.match(
        /const participantId = connection\?\.local\.peerParticipantId \|\| setupParticipantId;/g
      )
    ).toHaveLength(1);
    expect(pairingPageSource).toContain("<PairingCalibrationDialog");
    expect(pairingPageSource).toContain("{mockMode}");
    expect(pairingCalibrationDialogSource).toContain("if (mockMode) {");
    expect(pairingCalibrationDialogSource).toContain("mockAssignment(referenceFloorplan, calibrationVersion)");
    expect(pairingCalibrationDialogSource).toContain(
      "await client.activateParticipant(baseUrl, peerDescriptor.participantId)"
    );
    expect(pairingCalibrationDialogSource).toContain("proposal || (mockMode && samples.length === 2)");
    expect(pairingPageSource).not.toContain("await client.syncSiteConfiguration(address, ownerNodeId)");
    const resumeAction = pairingPageSource.indexOf(
      '{#if managementAllowed && setupResumeAvailability(participant) !== "hidden"}'
    );
    const calibrationAction = pairingPageSource.indexOf(
      '{:else if managementAllowed && calibrationAvailability(participant) !== "hidden"}',
      resumeAction
    );
    expect(resumeAction).toBeGreaterThan(-1);
    expect(calibrationAction).toBeGreaterThan(resumeAction);
    expect(pairingPageSource).toContain('if (setupResumeAvailability(participant) !== "ready") return;');
    expect(pairingPageSource).toContain('disabled={busy || setupResumeAvailability(participant) !== "ready"}');
    expect(pairingPageSource).toContain("generation !== pollGeneration");
    expect(pairingPageSource).toContain("generation === pollGeneration");
    expect(incomingPairingDialogSource).toContain("generation !== pollGeneration");
    expect(incomingPairingDialogSource).toContain("generation === pollGeneration");
  });

  it("removes mock participants before entering the production removal transaction", () => {
    const disconnectStart = pairingPageSource.indexOf("async function disconnectSelected");
    const mockRemoval = pairingPageSource.indexOf("if (mockMode) {", disconnectStart);
    const productionRemoval = pairingPageSource.indexOf("client.removeSiteMember", disconnectStart);

    expect(disconnectStart).toBeGreaterThanOrEqual(0);
    expect(mockRemoval).toBeGreaterThan(disconnectStart);
    expect(productionRemoval).toBeGreaterThan(mockRemoval);
    expect(pairingPageSource).toContain("mockPlatformRemovalDelayMs()");
    expect(pairingPageSource).toContain("removeMockPlatformParticipant(participantId)");
  });

  it("mounts one incoming pairing dialog above every dashboard tab", () => {
    const globalDialog = appSource.indexOf("<IncomingPairingDialog");
    const tabNavigation = appSource.indexOf('<nav class="app-tabs"');
    const tabPages = appSource.indexOf('{#if activeTab === "dashboard"}');

    expect(globalDialog).toBeGreaterThan(-1);
    expect(globalDialog).toBeLessThan(tabNavigation);
    expect(globalDialog).toBeLessThan(tabPages);
    expect(pairingPageSource).not.toContain("incomingPairingOpen");
    expect(pairingPageSource).not.toContain('aria-labelledby="pairing-incoming-title"');
    expect(appSource.match(/available=\{systemStatus\?\.platform\?\.pairingSupported === true\}/gu)).toHaveLength(2);
    expect(appSource).not.toContain("available={!demoMode && systemStatus?.platform?.pairingSupported === true}");
  });

  it("keeps the incoming permission action on standard button typography", () => {
    expect(incomingPairingDialogSource).toContain('class="pairing-incoming-action"');
    expect(incomingPairingDialogSource).toContain("font-size: var(--rz-font-button)");
    expect(incomingPairingDialogSource).toContain("font-weight: var(--rz-weight-button)");
    expect(incomingPairingDialogSource).toContain("letter-spacing: 0");
    expect(incomingPairingDialogSource).toContain("client.getStatus(deviceBaseUrl)");
  });

  it("keeps the radar occlusion mask in the cropped floorplan coordinate space", () => {
    expect(floorplanRadarPlacementOverlaySource).toContain('maskUnits="userSpaceOnUse"');
    expect(floorplanRadarPlacementOverlaySource).toContain('maskContentUnits="userSpaceOnUse"');
    expect(floorplanRadarPlacementOverlaySource).toContain("x={viewX()}");
    expect(floorplanRadarPlacementOverlaySource).toContain("y={viewY()}");
    expect(floorplanRadarPlacementOverlaySource).toContain("width={viewWidth()}");
    expect(floorplanRadarPlacementOverlaySource).toContain("height={viewHeight()}");
  });

  it("keeps the manual floorplan background without replaying its frozen image", () => {
    expect(floorplanImageFrameSource).toContain("{#if backgroundFill}");
    expect(floorplanImageFrameSource).toContain("fill={backgroundFill}");
    expect(dashboardPanelSource).toContain('backgroundFill={structuredManualFloorplan() ? "#f4f6f3" : ""}');
    expect(storedFloorplanEditorSource).toContain('backgroundFill={state.structuredManualSource ? "#f4f6f3" : ""}');
    expect(dashboardPanelSource).toContain("showImage={!structuredManualFloorplan()}");
    expect(storedFloorplanEditorSource).toContain("showImage={!state.structuredManualSource}");
  });
});
