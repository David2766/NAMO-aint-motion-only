<script>
  import { onDestroy, onMount } from "svelte";
  import { apiErrorMessage, fallbackErrorMessage } from "../api/api-message";
  import FloorplanCreationWizard from "./floorplan/FloorplanCreationWizard.svelte";
  import StoredFloorplanDeleteDialog from "./floorplan/StoredFloorplanDeleteDialog.svelte";
  import StoredFloorplanEditor from "./floorplan/StoredFloorplanEditor.svelte";
  import { FLOORPLAN_FURNITURE_ASSETS } from "../floorplan/furniture-assets";
  import {
    sanitizeStoredFloorplanDocument,
    storedRoomToCandidate
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
  import { createFloorplanWallAnalysisSession } from "../state/useFloorplanWallAnalysisSession.svelte";
  import { createRoomCandidateEditSession } from "../state/useRoomCandidateEditSession.svelte";
  import { createRoomCandidateMapTools } from "../state/useRoomCandidateMapTools.svelte";
  import { createFloorplanRoomScaleSession } from "../state/useFloorplanRoomScaleSession.svelte";
  import { createFloorplanRadarPlacementSession } from "../state/useFloorplanRadarPlacementSession.svelte";
  import { createStoredFloorplanSaveSession } from "../state/useStoredFloorplanSaveSession.svelte";
  import { createStoredFloorplanWorkspaceSession } from "../state/useStoredFloorplanWorkspaceSession.svelte";

  const MAX_CANDIDATE_HISTORY = 40;
  const OUTER_BOUNDS_TOUCH_TOLERANCE_PX = 8;

  let {
    messages,
    deviceConfig = null,
    deviceState = null,
    floorplanStorageBaseUrl = "",
    floorplanStorageFetcher,
    onUpdateDeviceConfig,
    onSelectDeviceZone,
    onSaveDeviceConfig,
    onSaveFloorplan,
    onFloorplanDeleted,
    onUnsavedChange
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

  // === Section: Stored floorplan state ===
  let storedFloorplanMode = $state("view");
  let storedFloorplanDocument = $state(null);
  let storedFloorplanEditTool = $state("rooms");
  let showStoredRadarOverlay = $state(true);
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
    snapPoint: (point) => roomCandidateMapTools?.snapManualPoint(point) ?? point,
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
    getText: () => panelText
  });
  const roomScale = createFloorplanRoomScaleSession({
    getRoomCandidates: visibleRoomCandidates,
    getText: () => panelText,
    outerBoundsTouchTolerancePx: OUTER_BOUNDS_TOUCH_TOLERANCE_PX
  });
  const floorplanRadar = createFloorplanRadarPlacementSession({
    getDeviceConfig: () => deviceConfig,
    getScaleEstimate: roomScale.scaleEstimate,
    getOuterBounds: roomScale.outerBounds,
    getImageSize: () => floorplanImage.debugInfo,
    getConfirmedRoomCandidates: confirmedRoomCandidates,
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
  const storedSave = createStoredFloorplanSaveSession({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    getVisibleRoomCandidates: visibleRoomCandidates,
    getObjects: () => storedFurniture?.objects() ?? [],
    getWallSegments: floorplanWallCandidateSegments,
    getEditSession: () => storedEdit,
    getText: () => panelText,
    isScaleInputValid: () => storedFloorplanScaleInputValid(),
    isSaveBlocked: () => floorplanStorageSaveBusy(storedEdit?.saveBusy ?? false),
    getClientOptions: () => ({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
    updateDeviceConfig: (updater) => onUpdateDeviceConfig?.(updater),
    saveDeviceConfig: () => onSaveDeviceConfig?.() ?? Promise.resolve(),
    errorMessage: (error) => apiErrorMessage(messages, error),
    touchTolerancePx: OUTER_BOUNDS_TOUCH_TOLERANCE_PX
  });
  const storedWorkspace = createStoredFloorplanWorkspaceSession({
    getDocument: () => storedFloorplanDocument,
    getVisibleRoomCandidates: visibleRoomCandidates,
    getSelectedRoomId: () => roomCandidates.selectedCandidateId,
    getRadarPlacement: () => storedRadarPlacement.placement(),
    getRoomWallSegments: (placement, segmentPrefix) =>
      floorplanRadar.visibleRoomWallSegmentsForPlacement(placement, segmentPrefix)
  });
  const storedFloorplanScale = createStoredFloorplanScaleEditor({
    getDocument: () => storedFloorplanDocument,
    setDocument: (document) => {
      storedFloorplanDocument = document;
    },
    buildRooms: (scale) => storedSave.currentRooms(scale),
    markDirty: () => {
      storedEdit.markChanged("", ["document", "roomContext"]);
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
    getText: () => panelText
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
    await floorplanImage.handleFileChange(event);
  }

  // === Section: Wizard reset and room detection ===
  function clearImage() {
    floorplanImage.clear();
    resetNewFloorplanStateAfterImageClear();
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
    return wizardNavigation.title(panelText);
  }

  function workflowDescription() {
    return wizardNavigation.description(panelText);
  }

  // === Section: Geometry helpers for storage and overlays ===
  function visibleRoomCandidates() {
    return roomCandidates.candidates.filter((candidate) => candidate.status !== "rejected");
  }

  function confirmedRoomCandidates() {
    return roomCandidates.candidates.filter((candidate) => candidate.status === "confirmed");
  }

  function floorplanWallCandidateSegments() {
    return snapSegments.length ? snapSegments : wallSegments;
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
  function floorplanStorageDocument() {
    const scale = roomScale.scaleEstimate();
    const width = floorplanImage.debugInfo?.width;
    const height = floorplanImage.debugInfo?.height;
    if (!scale || !width || !height) return null;
    const placement = floorplanRadar.placementOrDefault();

    return buildFloorplanStorageDocument({
      image: {
        width,
        height,
        bytes: floorplanImage.imageBlob?.size,
        name: floorplanImage.imageName,
        mime: floorplanImage.imageBlob?.type || "image/webp"
      },
      scale,
      radar: {
        originX: placement.originX,
        originY: placement.originY,
        rotation: placement.rotation,
        scale: floorplanRadar.scalePercent / 100
      },
      rooms: visibleRoomCandidates(),
      wallSegments: floorplanWallCandidateSegments(),
      roomMeasurements: roomScale.measurements,
      roomSizeEstimates: roomScale.storageEstimates(scale),
      ignoredOcclusionEdges: floorplanRadar.ignoredOcclusionEdges,
      objects: storedFurniture.objects()
    });
  }

  async function saveFloorplanToDevice() {
    if (floorplanStorageSaveBusy(floorplanSaveBusy)) return;
    const document = floorplanStorageDocument();
    if (!document || !floorplanImage.imageBlob) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = panelText.saveCannotBuild;
      return;
    }
    if (!onSaveFloorplan) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = panelText.saveApiUnavailable;
      return;
    }
    floorplanSaveBusy = true;
    floorplanSaveTone = "saving";
    floorplanSaveStatus = panelText.saveInProgress;
    try {
      await onSaveFloorplan(document, floorplanImage.imageBlob);
      await loadStoredFloorplan();
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

  function cleanupSelectedCandidate(saveHistory = true) {
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
    roomCandidateEdit.endVertexMove(id);
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
      ".floorplan-middle-column, .floorplan-candidates, .floorplan-candidate-card, .floorplan-candidate-select, .floorplan-candidate-editor, .floorplan-edit-tool-card, .floorplan-split-card, .floorplan-manual-card, .floorplan-candidate-group, .floorplan-candidate-edit-edge, .floorplan-candidate-vertex, .floorplan-snap-edge, .floorplan-snap-wall, .floorplan-split-draft-hit, .floorplan-split-draft-point, .floorplan-manual-draft-hit, .floorplan-manual-draft-point"
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
      storedFloorplanScale.reset();
      roomCandidates.clear();
      wallSegments = [];
      snapSegments = [];
      return;
    }
    const sanitizedDocument = sanitizeStoredFloorplanDocument(document);
    storedFloorplanDocument = sanitizedDocument;
    storedFloorplanScale.loadFromScale(sanitizedDocument.scale);
    const candidates = sanitizedDocument.rooms.map(storedRoomToCandidate);
    roomCandidates.setCandidates(candidates);
    wallSegments = sanitizedDocument.wallSegments ?? [];
    snapSegments = sanitizedDocument.wallSegments ?? [];
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
    storedEdit.resetAfterLoad();
    floorplanImage.setDebugInfo({
      ...(floorplanImage.debugInfo ?? { width: document.image.width, height: document.image.height }),
      size: storedFloorplanDocument?.image.bytes ?? image.size
    });
  }

  function requestStoredFloorplanDelete() {
    storedFloorplanStorage.requestDelete();
  }

  function cancelStoredFloorplanDelete() {
    storedFloorplanStorage.cancelDelete();
  }

  async function confirmStoredFloorplanDelete() {
    await storedFloorplanStorage.confirmDelete();
  }

  async function clearStoredFloorplanAfterDelete() {
    storedFloorplanDocument = null;
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
        showWallMaskOverlay
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
        mapToolActive: mapTools.isActive(),
        mapToolTitle: mapTools.activeMapToolTitle(),
        mapToolMessage: mapTools.activeMapToolMessage(),
        mapToolCanFinish: mapTools.activeMapToolCanFinish(),
        floorplanScale: floorplanViewport.view.scale
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
        clearImage,
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
        selectRoom: mapTools.selectRoomCandidate,
        renameRoom: renameRoomCandidate,
        updateRoomMeasurement: roomScale.updateMeasurement,
        confirmRoom: (id) => setCandidateStatusWithHistory(id, "confirmed"),
        rejectRoom: (id) => setCandidateStatusWithHistory(id, "rejected"),
        removeRoom: removeCandidateWithHistory,
        startManualRoomDraft: mapTools.startManualRoomDraft,
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
        finishMapTool: mapTools.finishActiveMapTool,
        cancelMapTool: mapTools.cancelActiveMapTool,
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
        updateRadarPlacement: floorplanRadar.updatePlacement,
        commitRadarPlacement: floorplanRadar.commitPlacement,
        toggleRadarOcclusionEdge: floorplanRadar.toggleOcclusionEdge,
        zoomFloorplan: floorplanViewport.zoom,
        resetFloorplanZoom: floorplanViewport.reset,
        handleFloorplanWheel: floorplanViewport.handleWheel,
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
        scale: {
          size: storedFloorplanScale.size,
          estimate: storedFloorplanScaleEstimate(),
          valid: storedFloorplanScaleInputValid()
        },
        zone: {
          source: storedFloorplanEditTool === "exit" ? "exit" : storedZoneSource,
          zones: storedZoneToolZones,
          selection: {
            zone: selectedStoredZone,
            zoneId: storedEdit.selectedRadarZoneId,
            pointIndex: storedEdit.selectedRadarZonePointIndex
          },
          actions: {
            addZone: storedFloorplanEditTool === "exit" ? storedRadarZones.addExitPoint : storedRadarZones.addZone,
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
        workspace: {
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
          targets: presenceDisplayTargets(deviceState),
          wallSegments: storedWorkspace.roomBoundarySegments(),
          occlusionSegments: storedWorkspace.radarOcclusionSegments(),
          ignoredOcclusionSegmentIds: storedWorkspace.ignoredOcclusionSegmentIds(),
          editableZoneSource: storedRadarZones.editSource(),
          selectedZoneId: storedEdit.selectedRadarZoneId,
          selectedZonePointIndex: storedEdit.selectedRadarZonePointIndex,
          furnitureBounds: storedWorkspace.furnitureBounds(),
          furnitureRooms: storedWorkspace.furnitureRooms()
        },
        mapTool: {
          active: mapTools.isActive(),
          title: mapTools.activeMapToolTitle(),
          message: mapTools.activeMapToolMessage(),
          canFinish: mapTools.activeMapToolCanFinish()
        }
      },
      actions: {
        view: () => (storedFloorplanMode = "view"),
        edit: () => {
          storedFloorplanMode = "edit";
          storedFloorplanEditTool = "rooms";
        },
        createNew: requestStoredFloorplanDelete,
        save: storedSave.save,
        selectRoomsTool: () => (storedFloorplanEditTool = "rooms"),
        selectScaleTool: () => (storedFloorplanEditTool = "scale"),
        selectRadarTool: () => (storedFloorplanEditTool = "radar"),
        selectZonesTool: () => storedRadarZones.beginTool("zones"),
        selectExitPointTool: storedRadarZones.beginExitPointTool,
        selectCalibrationTool: () => storedRadarZones.beginTool("calibration"),
        selectFurnitureTool: () => (storedFloorplanEditTool = "furniture"),
        startManualRoomDraft: mapTools.startManualRoomDraft,
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
        finishMapTool: mapTools.finishActiveMapTool,
        cancelMapTool: mapTools.cancelActiveMapTool,
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
        selectZone: storedRadarZones.selectZone,
        moveZone: storedRadarZones.moveZone,
        moveZonePoint: storedRadarZones.moveZonePoint,
        addZonePoint: storedRadarZones.addZonePoint,
        commitZoneEdit: storedRadarZones.commitZoneEdit,
        commitFurniture: storedFurniture.commitObjectEdit
      }
    };
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

{#if storedFloorplanStorage.checkBusy || storedFloorplanStorage.detected}
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
