<script>
  import { onDestroy, onMount } from "svelte";
  import { apiErrorMessage, fallbackErrorMessage } from "../api/api-message";
  import FloorplanCreationWizard from "./floorplan/FloorplanCreationWizard.svelte";
  import StoredFloorplanDeleteDialog from "./floorplan/StoredFloorplanDeleteDialog.svelte";
  import StoredFloorplanEditor from "./floorplan/StoredFloorplanEditor.svelte";
  import { FLOORPLAN_FURNITURE_ASSETS } from "../floorplan/furniture-assets";
  import {
    sanitizeStoredFloorplanDocument,
    storedRoomToCandidate,
    withLocalFloorplanRadarPlacement
  } from "../floorplan/stored-floorplan-document";
  import { createRoomCandidateState } from "../state/useRoomCandidates.svelte";
  import { createStoredFloorplanScaleEditor } from "../state/useStoredFloorplanScaleEditor.svelte";
  import {
    buildFloorplanStorageDocument,
  } from "../../core/floorplan/floorplan-storage";
  import { presenceDisplayTargets } from "../../core/presence-target-display";
  import { useDeviceStorageStatus } from "../state/useDeviceStorageStatus.svelte";
  import { createStoredFloorplanEditor } from "../state/useStoredFloorplanEditor.svelte";
  import { createStoredFloorplanEditSession } from "../state/useStoredFloorplanEditSession.svelte";
  import { createStoredRadarZoneEditor } from "../state/useStoredRadarZoneEditor.svelte";
  import { createStoredFurnitureEditor } from "../state/useStoredFurnitureEditor.svelte";
  import { createStoredRadarPlacementEditor } from "../state/useStoredRadarPlacementEditor.svelte";
  import { createFloorplanViewport } from "../state/useFloorplanViewport.svelte";
  import { createFloorplanWizardNavigation } from "../state/useFloorplanWizardNavigation.svelte";
  import { createFloorplanOcrSession } from "../state/useFloorplanOcrSession.svelte";
  import { createFloorplanImageSession } from "../state/useFloorplanImageSession.svelte";
  import { createManualFloorplanSession } from "../state/useManualFloorplanSession.svelte";
  import {
    addManualDoorExitZones,
    synchronizeManualDoorExitZones
  } from "../floorplan/manual-floorplan-exit-zones";
  import {
    buildManualPhysicalWallSegments,
    ensureStoredPhysicalWalls,
    occlusionSegmentsFromWalls,
    physicalWallOcclusionSegments
  } from "../floorplan/floorplan-physical-walls";
  import { nextIgnoredOcclusionEdges } from "../floorplan/floorplan-occlusion";
  import { createFloorplanWallAnalysisSession } from "../state/useFloorplanWallAnalysisSession.svelte";
  import { createRoomCandidateEditSession } from "../state/useRoomCandidateEditSession.svelte";
  import { createRoomCandidateMapTools } from "../state/useRoomCandidateMapTools.svelte";
  import { createFloorplanRoomScaleSession } from "../state/useFloorplanRoomScaleSession.svelte";
  import { createFloorplanRadarPlacementSession } from "../state/useFloorplanRadarPlacementSession.svelte";
  import { createStoredFloorplanSaveSession } from "../state/useStoredFloorplanSaveSession.svelte";
  import { createStoredFloorplanWorkspaceSession } from "../state/useStoredFloorplanWorkspaceSession.svelte";
  import { createStoredFloorplanDoorEditor } from "../state/useStoredFloorplanDoorEditor.svelte";
  import { platformFloorplanTargets, platformPeerRadarMarkers } from "../platform-radar-display";
  import {
    canonicalLocalRadarPlacement,
    floorplanMutationDecision
  } from "../floorplan/floorplan-replacement";

  const MAX_CANDIDATE_HISTORY = 40;
  const OUTER_BOUNDS_TOUCH_TOLERANCE_PX = 8;

  let {
    messages,
    deviceConfig = null,
    deviceState = null,
    localNodeId,
    siteConfiguration = null,
    floorplanStorageBaseUrl = "",
    floorplanStorageFetcher,
    onUpdateDeviceConfig,
    onSelectDeviceZone,
    onSaveDeviceConfig,
    onSaveFloorplan,
    onBeforeSharedFloorplanSave,
    onPublishSharedFloorplan,
    onFloorplanDeleted,
    onFloorplanOwnerRequired,
    onFloorplanDeleteBlocked,
    onUnsavedChange,
    canUseDetectionType = true,
    onDetectionOwnerRequired
  } = $props();

  const panelText = $derived(messages.floorplan.panel);
  const debugText = $derived(messages.floorplan.debug);
  const localizedFurnitureAssets = $derived(
    FLOORPLAN_FURNITURE_ASSETS.map((asset) => ({
      ...asset,
      label: panelText.storedFurnitureLabel(asset.id, asset.label)
    }))
  );

  // === Section: New floorplan wizard state ===
  let wallSegments = $state([]);
  let snapSegments = $state([]);
  let showFinalRadarOverlay = $state(true);
  let floorplanSaveBusy = $state(false);
  let floorplanSaveStatus = $state("");
  let floorplanSaveTone = $state("idle");
  let showWallMaskOverlay = $state(false);
  let replacingFloorplan = $state(false);

  // === Section: Stored floorplan state ===
  let storedFloorplanMode = $state("view");
  let storedFloorplanDocument = $state(null);
  let storedFloorplanEditTool = $state("rooms");
  let showStoredRadarOverlay = $state(true);
  let storedOcclusionEditActive = $state(false);
  let storedSavedDoors = $state([]);
  let storedSavedDoorDocument = $state(null);
  const floorplanStorageStatus = useDeviceStorageStatus("floorplan", () => messages.common.saveStatus);

  const floorplanImage = createFloorplanImageSession({
    getText: () => panelText,
    formatError: fallbackErrorMessage,
    onBeforeLoad: resetNewFloorplanStateAfterImageClear,
    onLoaded: () => {
      wallAnalysis.markReady();
      wizardNavigation.reset();
    }
  });
  const floorplanViewport = createFloorplanViewport({
    hasImage: () => !!floorplanImage.imageUrl,
    getImageSize: () => floorplanImage.debugInfo
  });
  const wizardNavigation = createFloorplanWizardNavigation();
  const manualFloorplan = createManualFloorplanSession();
  const roomCandidates = createRoomCandidateState();
  let roomCandidateMapTools;
  const roomCandidateEdit = createRoomCandidateEditSession({
    roomCandidates,
    maxHistory: MAX_CANDIDATE_HISTORY,
    getImageSize: () => floorplanImage.debugInfo,
    getManualRoomDraft: () => roomCandidateMapTools?.manualRoomDraft ?? { active: false, points: [] },
    setManualRoomDraft: (draft) => {
      roomCandidateMapTools?.setManualRoomDraft(draft);
    },
    getManualRoomDraftHover: () => roomCandidateMapTools?.manualRoomDraftHover ?? null,
    setManualRoomDraftHover: (hover) => {
      roomCandidateMapTools?.setManualRoomDraftHover(hover);
    },
    snapPoint: (point, points, index) => manualFloorplan.active
      ? manualFloorplan.snapEditPoint(points, index, point)
      : roomCandidateMapTools?.snapManualPoint(point) ?? point,
    snapInsertedPoint: (point, points, index) => manualFloorplan.active
      ? manualFloorplan.snapEditPoint(points, index, point)
      : point,
    markStoredRoomsChanged,
    markStoredRoomNameChanged: (id, name) => {
      if (storedRoomsEditActive()) storedEdit.markRoomNameChanged(id, name);
    },
    canDeleteSelectedVertex: () => {
      const editingInWizard = wizardNavigation.step === "rooms";
      const editingStoredRooms = storedFloorplanMode === "edit" && storedFloorplanEditTool === "rooms";
      return editingInWizard || editingStoredRooms;
    },
    resetSnapEdit: () => {
      roomCandidateMapTools?.resetSnapEdit();
    }
  });
  roomCandidateMapTools = createRoomCandidateMapTools({
    roomCandidates,
    roomCandidateEdit,
    getImageSize: () => floorplanImage.debugInfo,
    getWallSegments: () => floorplanWallCandidateSegments(),
    getText: () => panelText,
    getManualPointTransform: () => manualFloorplan.active ? manualFloorplan.transformDraftPoint : null,
    getManualPointValidator: () => manualFloorplan.active ? manualFloorplan.draftPointAllowed : null,
    getManualDraftValidator: () => manualFloorplan.active ? manualFloorplan.draftValid : null,
    getManualDraftText: () => {
      if (!manualFloorplan.active) return null;
      return manualFloorplan.phase === "outline"
        ? {
            title: panelText.manualOutlineTitle,
            needPoints: panelText.manualOutlineNeedPoints,
            ready: panelText.manualOutlineReady,
            invalid: panelText.manualOutlineInvalid
          }
        : {
            title: panelText.toolTitleManualRoom,
            needPoints: panelText.manualNeedPoints,
            ready: panelText.manualReady,
            invalid: panelText.manualRoomInvalid
          };
    },
    onManualRoomFinished: (candidate) => {
      if (!manualFloorplan.active || !manualFloorplan.captureOutline(candidate)) return true;
      syncManualFloorplanScale();
      return false;
    }
  });
  const roomScale = createFloorplanRoomScaleSession({
    getRoomCandidates: visibleRoomCandidates,
    getOuterBounds: () => manualFloorplan.active
      ? manualFloorplanBoundsForScale()
      : null,
    getText: () => panelText,
    outerBoundsTouchTolerancePx: OUTER_BOUNDS_TOUCH_TOLERANCE_PX
  });
  const floorplanRadar = createFloorplanRadarPlacementSession({
    getDeviceConfig: () => deviceConfig,
    getScaleEstimate: roomScale.scaleEstimate,
    getOuterBounds: roomScale.outerBounds,
    getImageSize: () => floorplanImage.debugInfo,
    getConfirmedRoomCandidates: confirmedRoomCandidates,
    getPhysicalWallSegments: () => occlusionSegmentsFromWalls(draftPhysicalWallSegments()),
    onUpdateDeviceConfig: (updater) => onUpdateDeviceConfig?.(updater)
  });
  const floorplanOcr = createFloorplanOcrSession({
    getSourceBlob: () => floorplanImage.originalImageBlob ?? floorplanImage.imageBlob,
    hasOriginalSource: () => Boolean(floorplanImage.originalImageBlob),
    getRoomCandidates: () => roomCandidates.candidates,
    getSelectedCandidateId: () => roomCandidates.selectedCandidateId,
    replaceRoomCandidates: (candidates, selectedCandidateId) => {
      roomCandidates.replace(candidates, selectedCandidateId);
    },
    getText: () => panelText,
    formatError: fallbackErrorMessage
  });
  const wallAnalysis = createFloorplanWallAnalysisSession({
    getImageBlob: () => floorplanImage.imageBlob,
    getImageSize: () => floorplanImage.debugInfo,
    getImageName: () => floorplanImage.imageName,
    getText: () => panelText,
    getDebugText: () => debugText,
    formatError: fallbackErrorMessage,
    onDetected: ({ candidates, wallSegments: detectedWallSegments, snapSegments: detectedSnapSegments }) => {
      roomCandidates.setCandidates(candidates);
      wallSegments = detectedWallSegments;
      snapSegments = detectedSnapSegments;
      roomCandidateMapTools.resetAll();
      roomCandidateEdit.resetHistory();
    }
  });

  $effect(() => {
    floorplanImage.ensureDefaultStatus();
    wallAnalysis.ensureDefaultText(
      Boolean(floorplanImage.imageUrl),
      floorplanImage.statusTone === "ok" && wizardNavigation.step === "image"
    );
    floorplanOcr.ensureDefaultStatus();
  });
  let storedEdit;
  let storedFurniture;
  let storedDoors;
  const storedSave = createStoredFloorplanSaveSession({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    getVisibleRoomCandidates: visibleRoomCandidates,
    getObjects: () => storedFurniture?.objects() ?? [],
    getWallSegments: () => wallSegments,
    getEditSession: () => storedEdit,
    getText: () => panelText,
    isScaleInputValid: () => storedFloorplanScaleInputValid(),
    isSaveBlocked: () => floorplanStorageSaveBusy(storedEdit?.saveBusy ?? false),
    getClientOptions: () => ({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
    updateDeviceConfig: (updater) => onUpdateDeviceConfig?.(updater),
    saveDeviceConfig: () => onSaveDeviceConfig?.() ?? Promise.resolve(),
    beforeSharedSave: () => onBeforeSharedFloorplanSave?.() ?? Promise.resolve(),
    publishSharedFloorplan: (document) => onPublishSharedFloorplan?.(document) ?? Promise.resolve(),
    errorMessage: (error) => apiErrorMessage(messages, error),
    touchTolerancePx: OUTER_BOUNDS_TOUCH_TOLERANCE_PX
  });
  const storedWorkspace = createStoredFloorplanWorkspaceSession({
    getDocument: () => storedFloorplanDocument,
    getVisibleRoomCandidates: visibleRoomCandidates,
    getSelectedRoomId: () => roomCandidates.selectedCandidateId,
    getRadarPlacement: () => storedRadarPlacement.placement(),
    getRoomWallSegments: () => physicalWallOcclusionSegments(storedFloorplanDocument)
  });
  const storedFloorplanScale = createStoredFloorplanScaleEditor({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    buildRooms: (scale) => storedSave.currentRooms(scale),
    markDirty: () => {
      storedEdit.markChanged("", ["document", "coordinateFrame", "roomContext"]);
    },
    setSaveStatus: (message, tone) => {
      storedEdit.setSaveStatus(message, tone);
    },
    getMessages: () => ({
      sizeRequired: panelText.storedSizeRequired,
      scaleRecalculated: panelText.storedScaleRecalculated
    })
  });
  storedEdit = createStoredFloorplanEditSession({
    getDocument: () => storedFloorplanDocument,
    getConfig: () => deviceConfig,
    restoreDocument: loadStoredFloorplanDocumentState,
    updateConfig: (...args) => onUpdateDeviceConfig?.(...args),
    getMode: () => storedFloorplanMode,
    getEditTool: () => storedFloorplanEditTool,
    setEditTool: (tool) => {
      storedFloorplanEditTool = tool;
    },
    getShowRadarOverlay: () => showStoredRadarOverlay,
    setShowRadarOverlay: (visible) => {
      showStoredRadarOverlay = visible;
    }
  });
  storedDoors = createStoredFloorplanDoorEditor({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    getEditSession: () => storedEdit
  });
  const storedRadarZones = createStoredRadarZoneEditor({
    getConfig: () => deviceConfig,
    updateConfig: (...args) => onUpdateDeviceConfig?.(...args),
    getMode: () => storedFloorplanMode,
    getTool: () => storedFloorplanEditTool,
    setTool: (tool) => {
      storedFloorplanEditTool = tool;
    },
    setShowRadarOverlay: (visible) => {
      showStoredRadarOverlay = visible;
    },
    editSession: storedEdit,
    onSelectDeviceZone: (...args) => onSelectDeviceZone?.(...args),
    getMessages: () => messages,
    getText: () => panelText,
    canUseDetectionType: () => canUseDetectionType,
    onDetectionOwnerRequired: () => onDetectionOwnerRequired?.()
  });
  storedFurniture = createStoredFurnitureEditor({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    getDefaultPlacementContext: storedWorkspace.defaultFurniturePlacementContext,
    getRooms: storedWorkspace.furnitureRooms,
    editSession: storedEdit,
    getText: () => panelText
  });
  const storedRadarPlacement = createStoredRadarPlacementEditor({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    editSession: storedEdit
  });
  const storedFloorplanStorage = createStoredFloorplanEditor({
    getBaseUrl: () => floorplanStorageBaseUrl,
    getFetcher: () => floorplanStorageFetcher,
    errorMessage: (error) => apiErrorMessage(messages, error),
    onLoaded: applyLoadedStoredFloorplan,
    onDeleted: clearStoredFloorplanAfterDelete
  });
  // === Section: Image preparation and validation ===
  async function handleFileChange(event) {
    manualFloorplan.reset();
    await floorplanImage.handleFileChange(event);
  }

  async function startManualFloorplan() {
    if (floorplanImage.busy) return;
    floorplanImage.clear();
    resetNewFloorplanStateAfterImageClear();
    manualFloorplan.start();
    try {
      const blob = await manualFloorplan.render([], false);
      floorplanImage.setGeneratedCandidate({
        blob,
        name: panelText.manualFloorplanName,
        width: manualFloorplan.width,
        height: manualFloorplan.height
      });
      wallAnalysis.markReady();
      wizardNavigation.replace("rooms", "forward", roomCandidateMapTools.startManualRoomDraft);
    } catch (error) {
      manualFloorplan.reset();
      floorplanSaveTone = "error";
      floorplanSaveStatus = fallbackErrorMessage(error);
    }
  }

  function zoomFloorplan(direction) {
    if (manualFloorplan.scaleAdjustable) {
      manualFloorplan.zoomScale(direction);
      return;
    }
    floorplanViewport.zoom(direction);
  }

  function resetFloorplanZoom() {
    if (manualFloorplan.scaleAdjustable) {
      manualFloorplan.setScalePercent(100);
      return;
    }
    floorplanViewport.reset();
  }

  let manualScaleWheelReadyAt = 0;

  function handleFloorplanWheel(event) {
    if (manualFloorplan.scaleAdjustable) {
      event.preventDefault();
      const now = performance.now();
      if (now < manualScaleWheelReadyAt) return;
      manualScaleWheelReadyAt = now + 160;
      manualFloorplan.zoomScale(event.deltaY < 0 ? 1 : -1);
      return;
    }
    floorplanViewport.handleWheel(event);
  }

  function startManualOutlineDraft() {
    if (!manualFloorplan.active || manualFloorplan.phase !== "outline") return;
    roomCandidateMapTools.startManualRoomDraft();
  }

  function beginManualOutlineEdit() {
    if (roomCandidateMapTools.isActive() || manualFloorplan.doorPlacementActive) return;
    roomCandidates.select("");
    manualFloorplan.beginOutlineEdit();
  }

  function startManualDoorPlacement() {
    if (!manualFloorplan.active || roomCandidateMapTools.isActive() || manualFloorplan.outlineEditing) return;
    roomCandidates.select("");
    manualFloorplan.startDoorPlacement();
  }

  function placeManualDoor(point) {
    manualFloorplan.placeDoor(point, visibleRoomCandidates());
  }

  function finishManualOutlineEdit() {
    if (!manualFloorplan.finishOutlineEdit(visibleRoomCandidates())) return;
    syncManualFloorplanScale();
  }

  function cancelManualOutlineEdit() {
    manualFloorplan.cancelOutlineEdit();
    syncManualFloorplanScale();
  }

  function finishFloorplanMapTool() {
    if (manualFloorplan.outlineEditing) {
      finishManualOutlineEdit();
      return;
    }
    if (manualFloorplan.doorPlacementActive) {
      manualFloorplan.finishDoorPlacement();
      return;
    }
    roomCandidateMapTools.finishActiveMapTool();
  }

  function cancelFloorplanMapTool() {
    if (manualFloorplan.outlineEditing) {
      cancelManualOutlineEdit();
      return;
    }
    if (manualFloorplan.doorPlacementActive) {
      manualFloorplan.cancelDoorPlacement();
      return;
    }
    roomCandidateMapTools.cancelActiveMapTool();
  }

  // === Section: Wizard reset and room detection ===
  function clearImage() {
    floorplanImage.clear();
    resetNewFloorplanStateAfterImageClear();
    manualFloorplan.reset();
  }

  function resetNewFloorplanStateAfterImageClear() {
    floorplanOcr.destroy();
    wallAnalysis.reset();
    wallSegments = [];
    snapSegments = [];
    roomCandidateMapTools.resetAll();
    roomCandidateEdit.resetHistory();
    wizardNavigation.reset();
    roomScale.reset();
    floorplanRadar.reset();
    showFinalRadarOverlay = true;
    floorplanSaveBusy = false;
    floorplanSaveStatus = "";
    floorplanSaveTone = "idle";
    floorplanOcr.reset();
    showWallMaskOverlay = false;
    floorplanViewport.reset();
    roomCandidates.clear();
  }

  // === Section: Wizard navigation ===
  function startCandidateAnalysisStep() {
    if (!floorplanImage.imageUrl || floorplanImage.statusTone !== "ok" || wallAnalysis.busy) return;
    wizardNavigation.transition("rooms", "forward", wallAnalysis.detect);
  }

  function goToImageStep() {
    roomCandidateMapTools.finishSnapEdit();
    roomCandidates.select("");
    wizardNavigation.transition("image", "back");
  }

  function goToRoomsStep() {
    wizardNavigation.transition("rooms", "back");
  }

  function goToOcrStep() {
    roomCandidateMapTools.finishSnapEdit();
    roomCandidates.select("");
    roomCandidateMapTools.cancelManualRoomDraft();
    wizardNavigation.transition("ocr", "forward", () => {
      if (!floorplanOcr.busy) void floorplanOcr.run();
    });
  }

  function goBackToOcrStep() {
    roomCandidates.select("");
    wizardNavigation.transition("ocr", "back");
  }

  function goToRadarStep() {
    if (manualFloorplan.active && manualFloorplan.outlinePoints.length < 3) return;
    if (manualFloorplan.active && visibleRoomCandidates().length < 1) return;
    if (manualFloorplan.active && !manualFloorplan.roomsValid(visibleRoomCandidates())) return;
    if (!roomScale.canCalculate()) return;
    roomScale.calculate({ keepError: true });
    floorplanRadar.initialize();
    roomCandidates.select("");
    wizardNavigation.transition("radar", "forward");
  }

  function goBackToRadarStep() {
    wizardNavigation.transition("radar", "back");
  }

  function goToFinalStep() {
    floorplanRadar.initialize();
    roomCandidates.select("");
    wizardNavigation.transition("final", "forward");
  }

  function workflowTitle() {
    if (manualFloorplan.active && wizardNavigation.step === "rooms") {
      return manualFloorplan.phase === "outline" ? panelText.manualOutlineTitle : panelText.manualRoomsTitle;
    }
    return wizardNavigation.title(panelText);
  }

  function workflowDescription() {
    if (manualFloorplan.active && wizardNavigation.step === "rooms") {
      return manualFloorplan.phase === "outline" ? panelText.manualOutlineHint : panelText.manualRoomsHint;
    }
    return wizardNavigation.description(panelText);
  }

  // === Section: Geometry helpers for storage and overlays ===
  function visibleRoomCandidates() {
    return roomCandidates.candidates.filter((candidate) => candidate.status !== "rejected");
  }

  function confirmedRoomCandidates() {
    return roomCandidates.candidates.filter((candidate) => candidate.status === "confirmed");
  }

  function manualFloorplanBoundsForScale() {
    if (!manualFloorplan.active) return null;
    const bounds = manualFloorplan.scaleInput()?.bounds;
    return bounds ? { ...bounds } : null;
  }

  function syncManualFloorplanScale() {
    const scale = manualFloorplan.scaleInput();
    if (!scale) return;
    roomScale.updateTotalSize("width", String(scale.widthMm));
    roomScale.updateTotalSize("height", String(scale.heightMm));
  }

  function floorplanWallCandidateSegments() {
    return snapSegments.length ? snapSegments : wallSegments;
  }

  function draftPhysicalWallSegments() {
    if (manualFloorplan.active) {
      return buildManualPhysicalWallSegments(manualFloorplan.outlinePoints, visibleRoomCandidates());
    }
    return floorplanWallCandidateSegments();
  }

  function floorplanStorageSaveBusy(localBusy) {
    return floorplanStorageStatus.saveBusy(localBusy ? "saving" : "idle");
  }

  function floorplanStorageSaveStatus(localBusy, status) {
    return floorplanStorageStatus.saveStatus(localBusy ? "saving" : "idle", status);
  }

  function floorplanStorageSaveTone(localBusy, tone) {
    return floorplanStorageStatus.saveTone(localBusy ? "saving" : "idle", tone);
  }

  // === Section: Floorplan save path ===
  function floorplanStorageDocument(imageBlob = floorplanImage.imageBlob) {
    const scale = roomScale.scaleEstimate();
    const width = floorplanImage.debugInfo?.width;
    const height = floorplanImage.debugInfo?.height;
    if (!scale || !width || !height) return null;
    const placement = floorplanRadar.placementOrDefault();

    return buildFloorplanStorageDocument({
      source: manualFloorplan.active ? "manual" : "image",
      outline: manualFloorplan.active ? manualFloorplan.outlinePoints : undefined,
      image: {
        width,
        height,
        bytes: imageBlob?.size,
        name: floorplanImage.imageName,
        mime: imageBlob?.type || "image/webp"
      },
      scale,
      radar: {
        originX: placement.originX,
        originY: placement.originY,
        rotation: placement.rotation,
        scale: floorplanRadar.scalePercent / 100
      },
      rooms: visibleRoomCandidates(),
      wallSegments: draftPhysicalWallSegments(),
      doors: manualFloorplan.active ? manualFloorplan.doors : undefined,
      roomMeasurements: roomScale.measurements,
      roomSizeEstimates: roomScale.storageEstimates(scale),
      ignoredOcclusionEdges: floorplanRadar.ignoredOcclusionEdges,
      objects: storedFurniture.objects()
    });
  }

  async function saveFloorplanToDevice() {
    if (floorplanStorageSaveBusy(floorplanSaveBusy)) return;
    if (!onSaveFloorplan) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = panelText.saveApiUnavailable;
      return;
    }
    floorplanSaveBusy = true;
    floorplanSaveTone = "saving";
    floorplanSaveStatus = panelText.saveInProgress;
    try {
      const imageBlob = manualFloorplan.active
        ? await manualFloorplan.render(visibleRoomCandidates())
        : floorplanImage.imageBlob;
      const document = floorplanStorageDocument(imageBlob);
      if (!document || !imageBlob) throw new Error(panelText.saveCannotBuild);
      if (manualFloorplan.active && !deviceConfig) throw new Error(panelText.manualDoorExitSaveUnavailable);
      const doorExitPreview = manualFloorplan.active
        ? addManualDoorExitZones(deviceConfig, document, manualFloorplan.doors, messages.zones)
        : null;
      if (doorExitPreview?.reason === "zone_limit") {
        throw new Error(panelText.manualDoorExitZoneLimit);
      }
      if (manualFloorplan.active && manualFloorplan.doors.length && (!onUpdateDeviceConfig || !onSaveDeviceConfig)) {
        throw new Error(panelText.manualDoorExitSaveUnavailable);
      }
      await onSaveFloorplan(document, imageBlob);
      if (doorExitPreview?.added) {
        onUpdateDeviceConfig((current) =>
          addManualDoorExitZones(current, document, manualFloorplan.doors, messages.zones).config
        );
        await onSaveDeviceConfig();
      }
      await loadStoredFloorplan();
      manualFloorplan.reset();
      replacingFloorplan = false;
      storedFloorplanMode = "edit";
      storedFloorplanEditTool = "rooms";
      floorplanSaveTone = "ok";
      floorplanSaveStatus = panelText.saveDone;
    } catch (error) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = apiErrorMessage(messages, error);
    } finally {
      floorplanSaveBusy = false;
    }
  }

  // === Section: Room candidate history and editing ===
  function undoCandidateEdit() {
    roomCandidateEdit.undo();
  }

  function redoCandidateEdit() {
    roomCandidateEdit.redo();
  }

  function canUndoFloorplanEdit() {
    return storedEdit.usesStoredHistory() ? storedEdit.canUndo : roomCandidateEdit.canUndo;
  }

  function canRedoFloorplanEdit() {
    return storedEdit.usesStoredHistory() ? storedEdit.canRedo : roomCandidateEdit.canRedo;
  }

  function undoFloorplanEdit() {
    if (storedEdit.usesStoredHistory()) {
      storedEdit.undo();
    } else {
      undoCandidateEdit();
    }
  }

  function redoFloorplanEdit() {
    if (storedEdit.usesStoredHistory()) {
      storedEdit.redo();
    } else {
      redoCandidateEdit();
    }
  }

  function isEditableKeyTarget(target) {
    const tagName = target?.tagName?.toLowerCase?.();
    return tagName === "input" || tagName === "textarea" || tagName === "select" || target?.isContentEditable;
  }

  function handleHistoryKeydown(event) {
    if (isEditableKeyTarget(event.target)) return;
    const key = event.key.toLowerCase();
    if (!event.ctrlKey && !event.metaKey && (event.key === "Delete" || event.key === "Backspace") && deleteSelectedCandidateVertex()) {
      event.preventDefault();
    } else if (!event.ctrlKey && !event.metaKey && (event.key === "Delete" || event.key === "Backspace") && storedRadarZones.deleteSelectedZonePoint()) {
      event.preventDefault();
    } else if (!event.ctrlKey && !event.metaKey && key === "r" && storedFloorplanMode === "edit" && storedFloorplanEditTool === "furniture" && storedEdit.selectedFurnitureObjectId) {
      event.preventDefault();
      storedFurniture.rotateObject(storedEdit.selectedFurnitureObjectId, 90);
    } else if ((event.ctrlKey || event.metaKey) && key === "z") {
      event.preventDefault();
      if (event.shiftKey) {
        redoFloorplanEdit();
      } else {
        undoFloorplanEdit();
      }
    } else if ((event.ctrlKey || event.metaKey) && key === "y") {
      event.preventDefault();
      redoFloorplanEdit();
    }
  }

  function storedRoomsEditActive() {
    return storedFloorplanStorage.detected && storedFloorplanMode === "edit" && storedFloorplanEditTool === "rooms";
  }

  function markStoredRoomsChanged() {
    if (!storedRoomsEditActive()) return;
    storedEdit.markChanged("", ["document", "roomContext"]);
  }

  function toggleStoredOcclusionEdit() {
    storedOcclusionEditActive = !storedOcclusionEditActive;
  }

  function updateStoredOcclusionEdges(nextEdges) {
    if (!storedFloorplanDocument) return;
    storedEdit.pushHistory();
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      occlusion: { ignoredEdges: nextEdges }
    };
    storedEdit.markChanged("", ["occlusion"]);
  }

  function toggleStoredOcclusionEdge(segment) {
    if (!storedFloorplanDocument) return;
    updateStoredOcclusionEdges(nextIgnoredOcclusionEdges(
      storedFloorplanDocument.occlusion?.ignoredEdges ?? [],
      segment,
      physicalWallOcclusionSegments(storedFloorplanDocument)
    ));
  }

  function resetStoredOcclusionEdges() {
    if (!storedFloorplanDocument?.occlusion?.ignoredEdges?.length) return;
    updateStoredOcclusionEdges([]);
  }

  function cleanupSelectedCandidate(saveHistory = true) {
    if (manualFloorplan.active) return false;
    return roomCandidateEdit.cleanupSelected(saveHistory);
  }

  function clearFloorplanSelection(saveHistory = true) {
    cleanupSelectedCandidate(saveHistory);
    roomCandidateMapTools.finishSnapEdit();
    roomCandidateEdit.clearSelectedVertex();
    roomCandidates.select("");
  }

  function addCandidateVertex(id, edgeIndex, point) {
    roomCandidateEdit.addVertex(id, edgeIndex, point);
  }

  function deleteCandidateVertex(id, index) {
    return roomCandidateEdit.deleteVertex(id, index);
  }

  function deleteSelectedCandidateVertex() {
    return roomCandidateEdit.deleteSelectedVertex();
  }

  function moveCandidateVertex(id, index, point) {
    roomCandidateEdit.moveVertex(id, index, point);
  }

  function selectCandidateVertex(id, index) {
    roomCandidateEdit.selectVertex(id, index);
  }

  function beginCandidateVertexMove(id, index) {
    roomCandidateEdit.beginVertexMove(id, index);
  }

  function endCandidateVertexMove(id) {
    roomCandidateEdit.endVertexMove(id, !manualFloorplan.active);
  }

  function renameRoomCandidate(id, name) {
    roomCandidateEdit.rename(id, name);
  }

  function removeCandidateWithHistory(id) {
    roomCandidateEdit.remove(id);
  }

  function setCandidateStatusWithHistory(id, status) {
    roomCandidateEdit.setStatus(id, status);
  }

  function shouldKeepFloorplanSelection(target) {
    return Boolean(target?.closest?.(
      ".floorplan-middle-column, .floorplan-candidates, .floorplan-candidate-card, .floorplan-candidate-select, .floorplan-candidate-editor, .floorplan-edit-tool-card, .floorplan-split-card, .floorplan-manual-card, .floorplan-candidate-group, .floorplan-candidate-edit-edge, .floorplan-candidate-vertex, .floorplan-snap-edge, .floorplan-snap-wall, .floorplan-split-draft-hit, .floorplan-split-draft-point, .floorplan-manual-draft-hit, .floorplan-manual-draft-point, .floorplan-door-card, .floorplan-manual-door"
    ));
  }

  function handleFloorplanPanelClick(event) {
    if (shouldKeepFloorplanSelection(event.target)) return;
    if (!roomCandidates.selectedCandidateId && !roomCandidateMapTools.snapEdit.active) return;
    clearFloorplanSelection();
  }

  function handleFloorplanPanelKeydown(event) {
    if (isEditableKeyTarget(event.target)) return;
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    if (!deleteSelectedCandidateVertex()) return;
    event.preventDefault();
    event.stopPropagation();
  }

  // === Section: Stored floorplan load and document editing ===
  async function checkStoredFloorplan() {
    await storedFloorplanStorage.check();
  }

  async function loadStoredFloorplan() {
    await storedFloorplanStorage.load();
  }

  function loadStoredFloorplanDocumentState(document) {
    if (!document) {
      storedFloorplanDocument = null;
      storedSavedDoors = [];
      storedSavedDoorDocument = null;
      storedOcclusionEditActive = false;
      storedFloorplanScale.reset();
      roomCandidates.clear();
      wallSegments = [];
      snapSegments = [];
      storedDoors?.reset();
      return;
    }
    const sanitized = ensureStoredPhysicalWalls(sanitizeStoredFloorplanDocument(document));
    const sanitizedDocument = withLocalFloorplanRadarPlacement(
      sanitized,
      canonicalLocalRadarPlacement(siteConfiguration, sanitized, localNodeId) ??
        deviceConfig?.floorplan?.radar
    );
    storedFloorplanDocument = sanitizedDocument;
    storedOcclusionEditActive = false;
    storedFloorplanScale.loadFromScale(sanitizedDocument.scale);
    const candidates = sanitizedDocument.rooms.map(storedRoomToCandidate);
    roomCandidates.setCandidates(candidates);
    wallSegments = sanitizedDocument.wallSegments ?? [];
    snapSegments = (sanitizedDocument.wallSegments ?? []).filter((segment) => segment.axis !== "diagonal");
    storedDoors?.reset();
    floorplanImage.setDebugInfo({
      width: sanitizedDocument.image.width,
      height: sanitizedDocument.image.height,
      size: sanitizedDocument.image.bytes ?? floorplanImage.debugInfo?.size ?? 0,
      type: sanitizedDocument.image.mime,
      ok: true
    });
  }

  function applyLoadedStoredFloorplan(document, image, _imageUrl) {
    loadStoredFloorplanDocumentState(document);
    storedSavedDoors = cloneFloorplanDoors(storedFloorplanDocument?.doors ?? []);
    storedSavedDoorDocument = cloneFloorplanDocument(storedFloorplanDocument);
    storedEdit.resetAfterLoad();
    floorplanImage.setDebugInfo({
      ...(floorplanImage.debugInfo ?? { width: document.image.width, height: document.image.height }),
      size: storedFloorplanDocument?.image.bytes ?? image.size
    });
  }

  async function saveStoredFloorplanEdits() {
    const currentDoors = storedFloorplanDocument?.doors ?? [];
    const doorsChanged = JSON.stringify(storedSavedDoors) !== JSON.stringify(currentDoors);
    if (doorsChanged) {
      if (!deviceConfig || !onUpdateDeviceConfig || !onSaveDeviceConfig || !storedFloorplanDocument || !storedSavedDoorDocument) {
        storedEdit.setSaveStatus(panelText.manualDoorExitSaveUnavailable, "error");
        return;
      }
      const synchronized = synchronizeManualDoorExitZones(
        deviceConfig,
        storedSavedDoorDocument,
        storedFloorplanDocument,
        storedSavedDoors,
        currentDoors,
        messages.zones
      );
      if (synchronized.reason === "zone_limit") {
        storedEdit.setSaveStatus(panelText.manualDoorExitZoneLimit, "error");
        return;
      }
      if (synchronized.config !== deviceConfig) {
        onUpdateDeviceConfig(() => synchronized.config);
        storedEdit.markChanged("", ["document", "config"]);
      }
    }
    await storedSave.save();
    if (storedEdit.saveTone === "ok") {
      storedSavedDoors = cloneFloorplanDoors(storedFloorplanDocument?.doors ?? []);
      storedSavedDoorDocument = cloneFloorplanDocument(storedFloorplanDocument);
    }
  }

  function requestStoredFloorplanDelete() {
    const decision = floorplanMutationDecision(siteConfiguration, localNodeId, "delete");
    if (decision === "owner_required") {
      onFloorplanOwnerRequired?.();
      return;
    }
    if (decision === "connected_delete_blocked") {
      onFloorplanDeleteBlocked?.();
      return;
    }
    storedFloorplanStorage.requestDelete();
  }

  function requestFloorplanReplacement() {
    if (
      floorplanMutationDecision(siteConfiguration, localNodeId, "replace") ===
      "owner_required"
    ) {
      onFloorplanOwnerRequired?.();
      return;
    }
    resetNewFloorplanStateAfterImageClear();
    manualFloorplan.reset();
    replacingFloorplan = true;
  }

  function cancelFloorplanReplacement() {
    if (floorplanSaveBusy) return;
    clearImage();
    replacingFloorplan = false;
  }

  function cancelStoredFloorplanDelete() {
    storedFloorplanStorage.cancelDelete();
  }

  async function confirmStoredFloorplanDelete() {
    await storedFloorplanStorage.confirmDelete();
  }

  async function clearStoredFloorplanAfterDelete() {
    storedFloorplanDocument = null;
    storedSavedDoors = [];
    storedSavedDoorDocument = null;
    storedDoors.reset();
    storedFloorplanMode = "view";
    storedFloorplanEditTool = "rooms";
    storedFloorplanScale.reset();
    storedEdit.resetAfterDelete();
    roomCandidates.clear();
    await onFloorplanDeleted?.();
  }

  function storedFloorplanScaleEstimate() {
    return storedFloorplanScale.estimate();
  }

  function storedFloorplanScaleInputValid() {
    return storedFloorplanScale.valid();
  }

  function updateStoredFloorplanTotalSize(field, value) {
    storedEdit.beginHistoryAction("scale");
    storedFloorplanScale.updateTotalSize(field, value);
  }

  function commitStoredFloorplanScaleEdit() {
    storedEdit.finishHistoryAction("scale");
  }

  function hasUnsavedFloorplanChanges() {
    if (storedFloorplanStorage.detected) return storedEdit.dirty;
    return Boolean(
      manualFloorplan.active ||
      floorplanImage.imageBlob ||
      roomCandidates.candidates.length ||
      roomScale.hasInput() ||
      floorplanRadar.hasPlacement()
    );
  }

  function floorplanCreationWizardModel() {
    const mapTools = roomCandidateMapTools;
    return {
      state: {
        step: wizardNavigation.step,
        animating: wizardNavigation.animating,
        direction: wizardNavigation.direction,
        imageUrl: floorplanImage.imageUrl,
        imageName: floorplanImage.imageName,
        imageMeta: floorplanImage.imageMeta,
        busy: floorplanImage.busy,
        statusTone: floorplanImage.statusTone,
        statusText: floorplanImage.statusText,
        analysisBusy: wallAnalysis.busy,
        analysisSteps: wallAnalysis.steps,
        analysisDebug: wallAnalysis.debug,
        analysisEngine: wallAnalysis.engine,
        floorplanView: floorplanViewport.view,
        debugInfo: floorplanImage.debugInfo,
        showFinalRadarOverlay,
        showOcrOverlay: floorplanOcr.showOverlay,
        showWallMaskOverlay,
        manualMode: manualFloorplan.active,
        manualPhase: manualFloorplan.phase,
        replacement: replacingFloorplan
      },
      sidebar: {
        title: workflowTitle(),
        description: workflowDescription(),
        totalSize: {
          value: roomScale.totalSize,
          preciseEditing: roomScale.preciseEditing,
          error: roomScale.error,
          calculated: roomScale.calculated,
          message: roomScale.message,
          canCalculate: roomScale.canCalculate
        },
        roomTools: {
          candidates: roomCandidates.candidates,
          selectedCandidateId: roomCandidates.selectedCandidateId,
          manualRoomDraft: mapTools.manualRoomDraft,
          roomSplitDraft: mapTools.roomSplitDraft,
          roomMergeDraft: mapTools.roomMergeDraft,
          snapEdit: mapTools.snapEdit
        },
        radarStep: {
          canGo: roomScale.canCalculate,
          scaleSummary: roomScale.scaleSummary,
          scalePercent: floorplanRadar.scalePercent,
          minScalePercent: floorplanRadar.minScalePercent,
          maxScalePercent: floorplanRadar.maxScalePercent,
          scaleStepPercent: floorplanRadar.scaleStepPercent,
          occlusionEditActive: floorplanRadar.occlusionEditActive,
          ignoredOcclusionEdges: floorplanRadar.ignoredOcclusionEdges
        },
        finalStep: {
          storageDocument: floorplanStorageDocument,
          scaleSummary: roomScale.scaleSummary,
          placement: floorplanRadar.placementOrDefault,
          roomCount: () => visibleRoomCandidates().length,
          ignoredOcclusionEdges: floorplanRadar.ignoredOcclusionEdges,
          hasImageBlob: !!floorplanImage.imageBlob,
          saveBusy: floorplanStorageSaveBusy(floorplanSaveBusy),
          saveStatus: floorplanStorageSaveStatus(floorplanSaveBusy, floorplanSaveStatus),
          saveTone: floorplanStorageSaveTone(floorplanSaveBusy, floorplanSaveTone)
        }
      },
      room: {
        candidates: roomCandidates.candidates,
        selectedCandidateId: roomCandidates.selectedCandidateId,
        selectedCandidateVertexIndex: roomCandidateEdit.selectedVertexIndex,
        measurements: roomScale.measurements,
        estimatedSizes: roomScale.estimates,
        showEstimatedSizes: roomScale.preciseEditing,
        manualRoomDraft: mapTools.manualRoomDraft,
        manualRoomDraftHover: mapTools.manualRoomDraftHover,
        roomSplitDraft: mapTools.roomSplitDraft,
        roomSplitDraftHover: mapTools.roomSplitDraftHover,
        roomMergeDraft: mapTools.roomMergeDraft,
        snapEdit: mapTools.snapEdit,
        roomSizeBoundsVisible: false,
        metricMmPerPx: manualFloorplan.mmPerPx,
        metricOutlinePoints: manualFloorplan.outlinePoints,
        manualOutlineEditing: manualFloorplan.outlineEditing,
        manualRoomsValid: manualFloorplan.roomsValid(visibleRoomCandidates()),
        doors: manualFloorplan.doors,
        doorPlacementActive: manualFloorplan.doorPlacementActive,
        doorPlacementKind: manualFloorplan.doorPlacementKind,
        doorPlacementError: manualFloorplan.doorPlacementError,
        selectedDoorId: manualFloorplan.selectedDoorId,
        wallMaskCells: wallAnalysis.debug?.wallMaskCells ?? [],
        snapEdges: mapTools.getSnapEdges(),
        snapWallSegments: mapTools.getSnapWallSegments(),
        canUndo: roomCandidateEdit.canUndo,
        canRedo: roomCandidateEdit.canRedo
      },
      workspace: {
        imageLayerStyle: floorplanViewport.imageLayerStyle(),
        transformStyle: floorplanViewport.transformStyle(),
        scaleEstimate: roomScale.scaleEstimate(),
        radarPlacement: floorplanRadar.placementOrDefault(),
        radarScalePercent: floorplanRadar.scalePercent,
        zones: deviceConfig?.zones ?? [],
        calibrationZones: deviceConfig?.calibrationZones ?? [],
        targets: presenceDisplayTargets(deviceState),
          wallSegments: floorplanRadar.boundarySegments(),
          occlusionSegments: floorplanRadar.occlusionSegments(),
          ignoredOcclusionSegmentIds: floorplanRadar.ignoredOcclusionEdges,
        mapToolActive: manualFloorplan.outlineEditing || manualFloorplan.doorPlacementActive || mapTools.isActive(),
        mapToolTitle: manualFloorplan.outlineEditing
          ? panelText.manualOutlineEditTitle
          : manualFloorplan.doorPlacementActive
            ? panelText.manualDoorPlacementTitle
          : mapTools.activeMapToolTitle(),
        mapToolMessage: manualFloorplan.outlineEditing
          ? (manualFloorplan.outlineEditValid(visibleRoomCandidates())
            ? panelText.manualOutlineEditReady
            : panelText.manualOutlineEditInvalid)
          : manualFloorplan.doorPlacementActive
            ? panelText.manualDoorPlacementHint
          : mapTools.activeMapToolMessage(),
        mapToolCanFinish: manualFloorplan.outlineEditing
          ? manualFloorplan.outlineEditValid(visibleRoomCandidates())
          : manualFloorplan.doorPlacementActive
            ? true
          : mapTools.activeMapToolCanFinish(),
        floorplanScale: manualFloorplan.scaleAdjustable
          ? manualFloorplan.scalePercent / 100
          : floorplanViewport.view.scale,
        canZoomOut: manualFloorplan.scaleAdjustable
          ? manualFloorplan.canZoomOut
          : floorplanViewport.view.scale > 1
      },
      ocr: {
        open: floorplanOcr.open,
        busy: floorplanOcr.busy,
        progress: floorplanOcr.progress,
        statusText: floorplanOcr.statusText,
        logs: floorplanOcr.logs,
        result: floorplanOcr.result,
        resultSummary: floorplanOcr.resultSummary(),
        errorText: floorplanOcr.errorText,
        overlayItems: floorplanOcr.overlayItems(),
        previewWords: floorplanOcr.previewWords(),
        kindCounts: floorplanOcr.kindCounts()
      },
      actions: {
        handleFloorplanPanelClick,
        handleFloorplanPanelKeydown,
        fileChange: handleFileChange,
        startManualFloorplan,
        startManualOutline: startManualOutlineDraft,
        editManualOutline: beginManualOutlineEdit,
        startDoorPlacement: startManualDoorPlacement,
        clearImage,
        cancelReplacement: cancelFloorplanReplacement,
        totalWidthInput: (value) => roomScale.updateTotalSize("width", value),
        totalHeightInput: (value) => roomScale.updateTotalSize("height", value),
        togglePreciseRoomSizeEditing: roomScale.togglePreciseEditing,
        startCandidateAnalysis: startCandidateAnalysisStep,
        goToImage: goToImageStep,
        goToOcr: goToOcrStep,
        goToRooms: goToRoomsStep,
        goToRadar: goToRadarStep,
        goBackToOcr: goBackToOcrStep,
        goToFinal: goToFinalStep,
        goBackToRadar: goBackToRadarStep,
        save: saveFloorplanToDevice,
        radarScaleInput: floorplanRadar.updateScalePercent,
        radarScaleNudge: floorplanRadar.nudgeScalePercent,
        toggleRadarOcclusionEdit: floorplanRadar.toggleOcclusionEdit,
        resetRadarOcclusionEdges: floorplanRadar.resetOcclusionEdges,
        selectRoom: mapTools.selectRoomCandidate,
        renameRoom: renameRoomCandidate,
        updateRoomMeasurement: roomScale.updateMeasurement,
        confirmRoom: (id) => setCandidateStatusWithHistory(id, "confirmed"),
        rejectRoom: (id) => setCandidateStatusWithHistory(id, "rejected"),
        removeRoom: removeCandidateWithHistory,
        startManualRoomDraft: mapTools.startManualRoomDraft,
        setDoorPlacementKind: manualFloorplan.setDoorPlacementKind,
        placeDoor: placeManualDoor,
        previewDoor: (point) => manualFloorplan.previewDoor(point, visibleRoomCandidates()),
        selectDoor: manualFloorplan.selectDoor,
        resizeDoor: (id, handle, point) => manualFloorplan.resizeDoor(
          id,
          handle,
          point,
          visibleRoomCandidates()
        ),
        moveDoor: (id, point) => manualFloorplan.moveDoor(
          id,
          point,
          visibleRoomCandidates()
        ),
        removeDoor: manualFloorplan.removeDoor,
        startSplitDraft: mapTools.startRoomSplitDraft,
        finishSplitDraft: mapTools.finishRoomSplitDraft,
        cancelSplitDraft: mapTools.cancelRoomSplitDraft,
        canFinishSplitDraft: mapTools.canFinishRoomSplitDraft,
        startMergeDraft: mapTools.startRoomMergeDraft,
        finishMergeDraft: mapTools.finishRoomMergeDraft,
        cancelMergeDraft: mapTools.cancelRoomMergeDraft,
        canFinishMergeDraft: mapTools.canFinishRoomMergeDraft,
        startSnapEdit: mapTools.startSnapEdit,
        finishSnapEdit: mapTools.finishSnapEdit,
        cancelSnapEdit: mapTools.cancelSnapEdit,
        undoCandidateEdit,
        redoCandidateEdit,
        toggleFinalRadarOverlay: () => (showFinalRadarOverlay = !showFinalRadarOverlay),
        finishMapTool: finishFloorplanMapTool,
        cancelMapTool: cancelFloorplanMapTool,
        addManualRoomPoint: mapTools.addManualRoomPoint,
        deleteManualRoomPoint: mapTools.deleteManualRoomPoint,
        previewManualRoomPoint: mapTools.previewManualRoomPoint,
        clearManualRoomPointPreview: mapTools.clearManualRoomPointPreview,
        addManualOutlineVertex: manualFloorplan.addOutlinePoint,
        deleteManualOutlineVertex: manualFloorplan.deleteOutlinePoint,
        moveManualOutlineVertex: manualFloorplan.moveOutlinePoint,
        addSplitPoint: mapTools.addRoomSplitPoint,
        previewSplitPoint: mapTools.previewRoomSplitPoint,
        clearSplitPreview: mapTools.clearRoomSplitPreview,
        moveSplitPoint: mapTools.moveRoomSplitPoint,
        selectSnapEdge: mapTools.selectSnapEdge,
        snapSelectedEdgeToWall: mapTools.snapSelectedEdgeToWall,
        addCandidateVertex,
        selectCandidateVertex,
        deleteCandidateVertex,
        beginCandidateVertexMove,
        moveCandidateVertex,
        endCandidateVertexMove,
        updateRadarPlacement: floorplanRadar.updatePlacement,
        commitRadarPlacement: floorplanRadar.commitPlacement,
        toggleRadarOcclusionEdge: floorplanRadar.toggleOcclusionEdge,
        zoomFloorplan,
        resetFloorplanZoom,
        handleFloorplanWheel,
        handleFloorplanPointerDown: floorplanViewport.handlePointerDown,
        handleFloorplanPointerMove: floorplanViewport.handlePointerMove,
        stopFloorplanPan: floorplanViewport.stopPan,
        preventFloorplanAuxClick: floorplanViewport.preventAuxClick,
        setShowOcrOverlay: floorplanOcr.setShowOverlay,
        setShowWallMaskOverlay: (checked) => (showWallMaskOverlay = checked),
        wallMaskReasonCounts: wallAnalysis.wallMaskReasonCounts,
        downloadWallSelectionDebug: wallAnalysis.downloadDebug,
        cancelFloorplanOcr: floorplanOcr.cancel,
        closeFloorplanOcrDialog: floorplanOcr.close
      }
    };
  }

  function storedFloorplanEditorModel() {
    const mapTools = roomCandidateMapTools;
    const storedZoneSource = storedRadarZones.editSource();
    const storedZoneToolZones = storedRadarZones.toolZones(storedFloorplanEditTool);
    const selectedStoredZone = storedRadarZones.selectedZone(storedZoneSource);
    return {
      state: {
        storage: {
          checkBusy: storedFloorplanStorage.checkBusy,
          imageUrl: storedFloorplanStorage.imageUrl
        },
        mode: storedFloorplanMode,
        editTool: storedFloorplanEditTool,
        document: storedFloorplanDocument,
        manualSource: storedFloorplanDocument?.source === "manual",
        structuredManualSource: storedFloorplanDocument?.source === "manual" &&
          Array.isArray(storedFloorplanDocument?.doors) &&
          Boolean(storedFloorplanDocument?.wallSegments?.length),
        summary: {
          dirty: storedEdit.dirty,
          saveBusy: floorplanStorageSaveBusy(storedEdit.saveBusy),
          canSave: storedSave.canSave(),
          saveTone: floorplanStorageSaveTone(storedEdit.saveBusy, storedEdit.saveTone),
          saveStatus: floorplanStorageSaveStatus(storedEdit.saveBusy, storedEdit.saveStatus)
        }
      },
      tools: {
        room: {
          candidates: roomCandidates.candidates,
          selectedCandidateId: roomCandidates.selectedCandidateId,
          selectedCandidateVertexIndex: roomCandidateEdit.selectedVertexIndex,
          manualRoomDraft: mapTools.manualRoomDraft,
          manualRoomDraftHover: mapTools.manualRoomDraftHover,
          roomSplitDraft: mapTools.roomSplitDraft,
          roomSplitDraftHover: mapTools.roomSplitDraftHover,
          roomMergeDraft: mapTools.roomMergeDraft,
          snapEdit: mapTools.snapEdit
        },
        doors: {
          doors: storedDoors.doors(),
          placementActive: storedDoors.placementActive,
          placementKind: storedDoors.placementKind,
          placementError: storedDoors.placementError,
          selectedDoorId: storedDoors.selectedDoorId
        },
        scale: {
          size: storedFloorplanScale.size,
          estimate: storedFloorplanScaleEstimate(),
          valid: storedFloorplanScaleInputValid()
        },
        zone: {
          source: storedZoneSource,
          zones: storedZoneToolZones,
          selection: {
            zone: selectedStoredZone,
            zoneId: storedEdit.selectedRadarZoneId,
            pointIndex: storedEdit.selectedRadarZonePointIndex
          },
          actions: {
            addZone: storedRadarZones.addZone,
            selectZone: storedRadarZones.selectZone,
            renameZone: storedRadarZones.renameSelectedZone,
            setZoneType: storedRadarZones.setSelectedZoneType,
            setCalibrationZoneType: storedRadarZones.setSelectedCalibrationZoneType,
            convertToRect: storedRadarZones.convertSelectedZoneToRect,
            deletePoint: storedRadarZones.deleteSelectedZonePoint,
            deleteZone: storedRadarZones.deleteSelectedZone
          }
        },
        furniture: {
          assets: localizedFurnitureAssets,
          objects: storedFurniture.objects(),
          selectedObjectId: storedEdit.selectedFurnitureObjectId,
          hasSelectedObject: !!storedFurniture.selectedObject(),
          targetRoomName: visibleRoomCandidates().find((candidate) => candidate.id === roomCandidates.selectedCandidateId)?.name ?? ""
        },
        radar: {
          occlusionEditActive: storedOcclusionEditActive,
          ignoredOcclusionEdges: storedFloorplanDocument?.occlusion?.ignoredEdges ?? []
        },
        workspace: {
          viewBounds: storedWorkspace.viewBounds(),
          imageFrameStyle: storedWorkspace.imageFrameStyle(),
          canUndo: canUndoFloorplanEdit(),
          canRedo: canRedoFloorplanEdit(),
          radarVisible: showStoredRadarOverlay,
          snapEdges: mapTools.getSnapEdges(),
          snapWallSegments: mapTools.getSnapWallSegments(),
          scaleEstimate: storedWorkspace.scaleEstimate(),
          radarPlacement: storedRadarPlacement.placement(),
          radarScalePercent: storedWorkspace.radarScalePercent(),
          zones: storedRadarZones.overlayZones(),
          calibrationZones: deviceConfig?.calibrationZones ?? [],
          targets: platformFloorplanTargets(
            deviceState,
            presenceDisplayTargets(deviceState),
            siteConfiguration,
            storedFloorplanDocument,
            localNodeId
          ),
          peerRadars: platformPeerRadarMarkers(siteConfiguration, storedFloorplanDocument, localNodeId),
          wallSegments: storedWorkspace.roomBoundarySegments(),
          occlusionSegments: storedWorkspace.radarOcclusionSegments(),
          ignoredOcclusionSegmentIds: storedWorkspace.ignoredOcclusionSegmentIds(),
          physicalWallSegments: physicalWallOcclusionSegments(storedFloorplanDocument),
          editableZoneSource: storedRadarZones.editSource(),
          zoneEditable: (zone) => zone.type !== "detection" || zone.placeholder || canUseDetectionType,
          selectedZoneId: storedEdit.selectedRadarZoneId,
          selectedZonePointIndex: storedEdit.selectedRadarZonePointIndex,
          furnitureBounds: storedWorkspace.furnitureBounds(),
          furnitureRooms: storedWorkspace.furnitureRooms()
        },
        mapTool: {
          active: mapTools.isActive() || storedDoors.placementActive,
          title: storedDoors.placementActive
            ? panelText.manualDoorPlacementTitle
            : mapTools.activeMapToolTitle(),
          message: storedDoors.placementActive
            ? panelText.manualDoorPlacementHint
            : mapTools.activeMapToolMessage(),
          canFinish: storedDoors.placementActive || mapTools.activeMapToolCanFinish()
        }
      },
      actions: {
        view: () => (storedFloorplanMode = "view"),
        edit: () => {
          storedFloorplanMode = "edit";
          storedFloorplanEditTool = "rooms";
        },
        replace: requestFloorplanReplacement,
        delete: requestStoredFloorplanDelete,
        save: saveStoredFloorplanEdits,
        selectRoomsTool: () => (storedFloorplanEditTool = "rooms"),
        selectScaleTool: () => (storedFloorplanEditTool = "scale"),
        selectRadarTool: () => (storedFloorplanEditTool = "radar"),
        selectZonesTool: () => storedRadarZones.beginTool("zones"),
        selectCalibrationTool: () => storedRadarZones.beginTool("calibration"),
        selectFurnitureTool: () => (storedFloorplanEditTool = "furniture"),
        startManualRoomDraft: mapTools.startManualRoomDraft,
        startDoorPlacement: storedDoors.startPlacement,
        setDoorPlacementKind: storedDoors.setPlacementKind,
        previewDoor: storedDoors.previewDoor,
        placeDoor: storedDoors.placeDoor,
        selectDoor: storedDoors.selectDoor,
        removeDoor: storedDoors.removeDoor,
        resizeDoor: storedDoors.resizeDoor,
        resizeDoorCommit: (id) => storedDoors.finishDoorInteraction(id, "resize"),
        moveDoor: storedDoors.moveDoor,
        moveDoorCommit: (id) => storedDoors.finishDoorInteraction(id, "move"),
        startSplitDraft: mapTools.startRoomSplitDraft,
        startMergeDraft: mapTools.startRoomMergeDraft,
        startSnapEdit: mapTools.startSnapEdit,
        selectRoom: mapTools.selectRoomCandidate,
        renameRoom: renameRoomCandidate,
        confirmRoom: (id) => setCandidateStatusWithHistory(id, "confirmed"),
        rejectRoom: (id) => setCandidateStatusWithHistory(id, "rejected"),
        removeRoom: removeCandidateWithHistory,
        finishSnapEdit: mapTools.finishSnapEdit,
        cancelSnapEdit: mapTools.cancelSnapEdit,
        finishSplitDraft: mapTools.finishRoomSplitDraft,
        cancelSplitDraft: mapTools.cancelRoomSplitDraft,
        canFinishSplitDraft: mapTools.canFinishRoomSplitDraft,
        finishMergeDraft: mapTools.finishRoomMergeDraft,
        cancelMergeDraft: mapTools.cancelRoomMergeDraft,
        canFinishMergeDraft: mapTools.canFinishRoomMergeDraft,
        scaleWidthInput: (value) => updateStoredFloorplanTotalSize("width", value),
        scaleHeightInput: (value) => updateStoredFloorplanTotalSize("height", value),
        scaleCommit: commitStoredFloorplanScaleEdit,
        addFurniture: storedFurniture.addObject,
        selectFurniture: (id) => (storedEdit.selectedFurnitureObjectId = id),
        deleteSelectedFurniture: () => storedFurniture.deleteObject(storedEdit.selectedFurnitureObjectId),
        undo: undoFloorplanEdit,
        redo: redoFloorplanEdit,
        rotateFurnitureCounterClockwise: () => storedFurniture.rotateObject(storedEdit.selectedFurnitureObjectId, -90),
        rotateFurnitureClockwise: () => storedFurniture.rotateObject(storedEdit.selectedFurnitureObjectId, 90),
        toggleRadar: () => (showStoredRadarOverlay = !showStoredRadarOverlay),
        finishMapTool: () => storedDoors.placementActive
          ? storedDoors.finishPlacement()
          : mapTools.finishActiveMapTool(),
        cancelMapTool: () => storedDoors.placementActive
          ? storedDoors.cancelPlacement()
          : mapTools.cancelActiveMapTool(),
        addManualRoomPoint: mapTools.addManualRoomPoint,
        deleteManualRoomPoint: mapTools.deleteManualRoomPoint,
        previewManualRoomPoint: mapTools.previewManualRoomPoint,
        clearManualRoomPointPreview: mapTools.clearManualRoomPointPreview,
        addSplitPoint: mapTools.addRoomSplitPoint,
        previewSplitPoint: mapTools.previewRoomSplitPoint,
        clearSplitPreview: mapTools.clearRoomSplitPreview,
        moveSplitPoint: mapTools.moveRoomSplitPoint,
        selectSnapEdge: mapTools.selectSnapEdge,
        snapSelectedEdgeToWall: mapTools.snapSelectedEdgeToWall,
        addCandidateVertex,
        selectCandidateVertex,
        deleteCandidateVertex,
        beginCandidateVertexMove,
        moveCandidateVertex,
        endCandidateVertexMove,
        changeRadarPlacement: (nextPlacement) => storedRadarPlacement.update(nextPlacement),
        commitRadarPlacement: (nextPlacement) => storedRadarPlacement.update(nextPlacement, true),
        toggleStoredOcclusionEdit,
        toggleStoredOcclusionEdge,
        resetStoredOcclusionEdges,
        selectZone: storedRadarZones.selectZone,
        moveZone: storedRadarZones.moveZone,
        moveZonePoint: storedRadarZones.moveZonePoint,
        addZonePoint: storedRadarZones.addZonePoint,
        commitZoneEdit: storedRadarZones.commitZoneEdit,
        commitFurniture: storedFurniture.commitObjectEdit
      }
    };
  }

  function cloneFloorplanDoors(doors) {
    return doors.map((door) => ({ ...door, centerPx: [...door.centerPx] }));
  }

  function cloneFloorplanDocument(document) {
    return document == null ? null : JSON.parse(JSON.stringify(document));
  }

  // === Section: Component lifecycle ===
  onMount(() => {
    void checkStoredFloorplan();
    window.addEventListener("keydown", handleHistoryKeydown);
    return () => window.removeEventListener("keydown", handleHistoryKeydown);
  });

  $effect(() => {
    onUnsavedChange?.(hasUnsavedFloorplanChanges());
  });

  onDestroy(() => {
    onUnsavedChange?.(false);
    floorplanStorageStatus.destroy();
    wizardNavigation.destroy();
    floorplanOcr.destroy();
    floorplanImage.destroy();
    storedFloorplanStorage.destroy();
  });
</script>

{#if !replacingFloorplan && (storedFloorplanStorage.checkBusy || storedFloorplanStorage.detected)}
  <StoredFloorplanEditor
    {messages}
    text={panelText}
    model={storedFloorplanEditorModel()}
  />
{:else}
  <FloorplanCreationWizard
    {messages}
    text={panelText}
    debugText={debugText}
    model={floorplanCreationWizardModel()}
  />
{/if}

{#if storedFloorplanStorage.deleteOpen}
  <StoredFloorplanDeleteDialog
    text={panelText}
    error={storedFloorplanStorage.deleteError}
    busy={storedFloorplanStorage.deleteBusy}
    onCancel={cancelStoredFloorplanDelete}
    onConfirm={confirmStoredFloorplanDelete}
  />
{/if}
