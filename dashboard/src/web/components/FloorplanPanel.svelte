<script>
  import { onDestroy, onMount } from "svelte";
  import FloorplanCandidateList from "./FloorplanCandidateList.svelte";
  import FloorplanCandidateOverlay from "./FloorplanCandidateOverlay.svelte";
  import EditorToolbar from "./EditorToolbar.svelte";
  import FinalStep from "./floorplan/FinalStep.svelte";
  import RadarStep from "./floorplan/RadarStep.svelte";
  import RoomEditStep from "./floorplan/RoomEditStep.svelte";
  import RoomEditTools from "./floorplan/RoomEditTools.svelte";
  import FloorplanOcrDialog from "./FloorplanOcrDialog.svelte";
  import FloorplanFurnitureOverlay from "./FloorplanFurnitureOverlay.svelte";
  import FloorplanRadarPlacementOverlay from "./FloorplanRadarPlacementOverlay.svelte";
  import { detectWallLineRoomCandidates } from "../../core/floorplan/wall-line-room-detector";
  import { cleanupOrthogonalRoomCandidate } from "../../core/floorplan/room-candidate-cleanup";
  import {
    getRoomCandidateSnapEdges,
    getVisibleSnapWallCandidates
  } from "../../core/floorplan/wall-snap";
  import { createFloorplanOcrJob, FloorplanOcrCancelledError } from "../floorplan/floorplan-ocr";
  import { applyOcrRoomLabels } from "../floorplan/ocr-room-matcher";
  import { FLOORPLAN_FURNITURE_ASSETS, findFloorplanFurnitureAsset } from "../floorplan/furniture-assets";
  import { createRoomCandidateState } from "../state/useRoomCandidates.svelte";
  import { zoneDisplayName } from "../../core/zones";
  import {
    addSoftwareZone,
    convertZoneToRectInConfig,
    deletePolygonObjectPoint,
    deleteZone,
    deleteZonePointInConfig,
    insertPolygonObjectPoint,
    insertZonePointInConfig,
    movePolygonObjectPoint,
    moveZone as moveRadarZone,
    renameZone,
    setCalibrationZoneType,
    setZoneType,
    updateZonePoint as updateRadarZonePoint
  } from "../zone-geometry";
  import {
    buildFloorplanStorageDocument,
  } from "../../core/floorplan/floorplan-storage";
  import {
    deleteFloorplanStorage,
    loadFloorplanStorageDocument,
    loadFloorplanStorageImage,
    loadFloorplanStorageStatus,
    saveFloorplanStorageDocument
  } from "../floorplan/floorplan-storage-client";

  const MAX_UPLOAD_BYTES = 5 * 1024 * 1024;
  const MAX_WEBP_BYTES = 80 * 1024;
  const MIN_LONG_SIDE = 800;
  const MIN_SHORT_SIDE = 500;
  const QUALITY_STEPS = [0.9, 0.85, 0.8, 0.75, 0.7];
  const ALLOWED_TYPES = new Set(["image/png", "image/jpeg", "image/webp"]);
  const MAX_CANDIDATE_HISTORY = 40;
  const OUTER_BOUNDS_TOUCH_TOLERANCE_PX = 8;
  const RADAR_OCCLUSION_GROUP_LINE_TOLERANCE_PX = 3;
  const RADAR_OCCLUSION_GROUP_TOUCH_TOLERANCE_PX = 6;
  const RADAR_SCALE_MIN_PERCENT = 95;
  const RADAR_SCALE_MAX_PERCENT = 105;
  const RADAR_SCALE_STEP_PERCENT = 1;

  let {
    deviceConfig = null,
    deviceState = null,
    floorplanStorageBaseUrl = "",
    floorplanStorageFetcher,
    onUpdateDeviceConfig,
    onSaveFloorplan,
    onFloorplanDeleted
  } = $props();

  let imageUrl = $state("");
  let imageName = $state("");
  let imageMeta = $state("");
  let statusTone = $state("warn");
  let statusText = $state("평면도 이미지를 선택해 주세요.");
  let debugInfo = $state(null);
  let busy = $state(false);
  let analysisBusy = $state(false);
  let analysisText = $state("평면도 업로드 후 방을 자동으로 인식할 수 있습니다.");
  let analysisDebug = $state(null);
  let analysisEngine = $state("");
  let analysisSteps = $state([]);
  let wallSegments = $state([]);
  let snapSegments = $state([]);
  let snapEdit = $state({ active: false, candidateId: "", edgeKey: "", original: null });
  let manualRoomDraft = $state({ active: false, points: [] });
  let manualRoomDraftHover = $state(null);
  let roomSplitDraft = $state({ active: false, candidateId: "", points: [] });
  let roomSplitDraftHover = $state(null);
  let roomMergeDraft = $state({ active: false, candidateIds: [] });
  let floorplanView = $state({ scale: 1, x: 0, y: 0, panning: false, lastX: 0, lastY: 0 });
  let candidateUndoStack = $state([]);
  let candidateRedoStack = $state([]);
  let floorplanWizardStep = $state("image");
  let floorplanWizardAnimating = $state(false);
  let floorplanWizardDirection = $state("forward");
  let floorplanTotalSize = $state({ width: "", height: "" });
  let roomSizeEstimates = $state({});
  let roomSizeCalculated = $state(false);
  let roomSizeError = $state("");
  let roomMeasurements = $state({});
  let preciseRoomSizeEditing = $state(false);
  let radarScalePercent = $state(100);
  let radarPlacement = $state(null);
  let radarOcclusionEditActive = $state(false);
  let radarOcclusionIgnoredEdges = $state([]);
  let showFinalRadarOverlay = $state(true);
  let floorplanSaveBusy = $state(false);
  let floorplanSaveStatus = $state("");
  let floorplanSaveTone = $state("idle");
  let ocrBusy = $state(false);
  let ocrDialogOpen = $state(false);
  let ocrProgress = $state(0);
  let ocrStatusText = $state("아직 OCR을 실행하지 않았습니다.");
  let ocrSteps = $state([]);
  let ocrResult = $state(null);
  let ocrError = $state("");
  let showOcrOverlay = $state(false);
  let showRoomCandidateDebug = $state(false);
  let showWallMaskOverlay = $state(false);
  let storedFloorplanCheckBusy = $state(false);
  let storedFloorplanDetected = $state(false);
  let storedFloorplanCheckError = $state("");
  let storedFloorplanMode = $state("view");
  let storedFloorplanDocument = $state(null);
  let storedFloorplanImageUrl = $state("");
  let storedFloorplanEditTool = $state("rooms");
  let showStoredRadarOverlay = $state(true);
  let storedFloorplanScaleSize = $state({ width: "", height: "" });
  let storedFloorplanDirty = $state(false);
  let storedFloorplanSaveBusy = $state(false);
  let storedFloorplanSaveStatus = $state("");
  let storedFloorplanSaveTone = $state("idle");
  let selectedFurnitureObjectId = $state("");
  let selectedCandidateVertexIndex = $state(-1);
  let selectedStoredRadarZoneId = $state("");
  let selectedStoredRadarZonePointIndex = $state(-1);
  let storedFloorplanDeleteOpen = $state(false);
  let storedFloorplanDeleteBusy = $state(false);
  let storedFloorplanDeleteError = $state("");
  let imageBlob = $state(null);
  let originalImageBlob = $state(null);
  let ocrJob = null;
  let floorplanWizardTimer = null;
  const roomCandidates = createRoomCandidateState();

  async function handleFileChange(event) {
    const file = event.currentTarget.files?.[0];
    event.currentTarget.value = "";
    if (!file) return;
    busy = true;
    clearImage();

    try {
      const result = await prepareFloorplanCandidate(file);
      imageUrl = URL.createObjectURL(result.blob);
      imageBlob = result.blob;
      originalImageBlob = file;
      imageName = file.name;
      imageMeta = `${formatBytes(result.blob.size)} · ${result.width} x ${result.height}px · WebP`;
      statusTone = result.ok ? "ok" : "error";
      statusText = result.ok ? "ESP32 저장 후보로 사용할 수 있습니다." : result.reason;
      debugInfo = result;
      analysisText = "방 자동 인식을 실행할 수 있습니다.";
      floorplanWizardStep = "image";
    } catch (error) {
      statusTone = "error";
      statusText = error instanceof Error ? error.message : String(error);
      debugInfo = null;
    } finally {
      busy = false;
    }
  }

  async function prepareFloorplanCandidate(file) {
    if (!ALLOWED_TYPES.has(file.type)) {
      throw new Error("PNG, JPG, WebP 이미지만 업로드할 수 있습니다.");
    }
    if (file.size > MAX_UPLOAD_BYTES) {
      throw new Error("이미지 파일이 너무 큽니다. 5MB 이하 파일을 사용해 주세요.");
    }

    const bitmap = await createImageBitmap(file);
    try {
      const longSide = Math.max(bitmap.width, bitmap.height);
      const shortSide = Math.min(bitmap.width, bitmap.height);
      const resolutionOk = longSide >= MIN_LONG_SIDE && shortSide >= MIN_SHORT_SIDE;
      const attempts = [];
      let best = null;

      for (const quality of QUALITY_STEPS) {
        const blob = await encodeWebp(bitmap, quality);
        const attempt = {
          quality,
          size: blob.size,
          ok: blob.size <= MAX_WEBP_BYTES
        };
        attempts.push(attempt);
        if (!best || blob.size < best.blob.size) {
          best = { blob, quality };
        }
        if (attempt.ok) {
          best = { blob, quality };
          break;
        }
      }

      const sizeOk = Boolean(best && best.blob.size <= MAX_WEBP_BYTES);
      const ok = resolutionOk && sizeOk;
      const reason = !resolutionOk
        ? "이미지가 너무 작습니다. 긴 변 800px, 짧은 변 500px 이상의 이미지를 사용해 주세요."
        : !sizeOk
          ? "글자 인식 품질을 유지하면서 80KB 이하 WebP로 변환하지 못했습니다."
          : "";

      return {
        ok,
        reason,
        blob: best.blob,
        originalName: file.name,
        originalType: file.type,
        originalSize: file.size,
        width: bitmap.width,
        height: bitmap.height,
        longSide,
        shortSide,
        outputType: "image/webp",
        outputSize: best.blob.size,
        quality: best.quality,
        resized: false,
        attempts
      };
    } finally {
      bitmap.close?.();
    }
  }

  async function encodeWebp(bitmap, quality) {
    const canvas = document.createElement("canvas");
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
    const context = canvas.getContext("2d", { alpha: false });
    if (!context) throw new Error("이미지를 변환할 수 없습니다. Canvas를 사용할 수 없습니다.");
    context.fillStyle = "#ffffff";
    context.fillRect(0, 0, canvas.width, canvas.height);
    context.drawImage(bitmap, 0, 0);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/webp", quality));
    if (!blob) throw new Error("WebP 변환에 실패했습니다.");
    return blob;
  }

  function clearImage() {
    void ocrJob?.cancel?.();
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    imageUrl = "";
    imageName = "";
    imageMeta = "";
    debugInfo = null;
    imageBlob = null;
    originalImageBlob = null;
    analysisText = "평면도 업로드 후 방을 자동으로 인식할 수 있습니다.";
    analysisDebug = null;
    analysisEngine = "";
    analysisSteps = [];
    wallSegments = [];
    snapSegments = [];
    snapEdit = { active: false, candidateId: "", edgeKey: "", original: null };
    manualRoomDraft = { active: false, points: [] };
    manualRoomDraftHover = null;
    roomSplitDraft = { active: false, candidateId: "", points: [] };
    roomSplitDraftHover = null;
    roomMergeDraft = { active: false, candidateIds: [] };
    candidateUndoStack = [];
    candidateRedoStack = [];
    floorplanWizardStep = "image";
    floorplanWizardAnimating = false;
    floorplanWizardDirection = "forward";
    floorplanTotalSize = { width: "", height: "" };
    roomSizeEstimates = {};
    roomSizeCalculated = false;
    roomSizeError = "";
    roomMeasurements = {};
    preciseRoomSizeEditing = false;
    radarScalePercent = 100;
    radarPlacement = null;
    radarOcclusionEditActive = false;
    radarOcclusionIgnoredEdges = [];
    showFinalRadarOverlay = true;
    floorplanSaveBusy = false;
    floorplanSaveStatus = "";
    floorplanSaveTone = "idle";
    resetOcrState();
    floorplanView = { scale: 1, x: 0, y: 0, panning: false, lastX: 0, lastY: 0 };
    roomCandidates.clear();
    statusTone = "warn";
    statusText = "평면도 이미지를 선택해 주세요.";
  }

  async function detectRoomCandidatesByWalls() {
    if (!imageBlob || !debugInfo) return;
    analysisBusy = true;
    analysisText = "벽선을 기준으로 방을 자동으로 인식하고 있습니다.";
    analysisSteps = ["방 자동 인식을 시작했습니다."];

    try {
      const bitmap = await createImageBitmap(imageBlob);
      try {
        const canvas = document.createElement("canvas");
        canvas.width = bitmap.width;
        canvas.height = bitmap.height;
        const context = canvas.getContext("2d", { willReadFrequently: true });
        if (!context) throw new Error("이미지 분석에 사용할 Canvas를 준비하지 못했습니다.");
        context.drawImage(bitmap, 0, 0);
        analysisSteps = [...analysisSteps, "이미지 픽셀 데이터를 준비했습니다."];
        const imageData = context.getImageData(0, 0, canvas.width, canvas.height);
        analysisSteps = [...analysisSteps, "벽선 분석을 실행하고 있습니다."];
        const result = detectWallLineRoomCandidates({
          imageData,
          applyCandidateFilters: true,
          preferThickWalls: true,
          excludeTextLikeNoise: true
        });
        const candidates = result.candidates;
        roomCandidates.setCandidates(candidates);
        wallSegments = result.wallSegments ?? [];
        snapSegments = result.snapSegments ?? result.wallSegments ?? [];
        snapEdit = { active: false, candidateId: "", edgeKey: "", original: null };
        manualRoomDraft = { active: false, points: [] };
        manualRoomDraftHover = null;
        candidateUndoStack = [];
        candidateRedoStack = [];
        analysisDebug = result.debug;
        analysisEngine = result.debug.engine;
        analysisText = candidates.length
          ? `방 후보 ${candidates.length}개를 찾았습니다.`
          : "방 후보를 찾지 못했습니다.";
        analysisSteps = [...analysisSteps, `방 자동 인식 완료: ${candidates.length}개`];
      } finally {
        bitmap.close?.();
      }
    } catch (error) {
      analysisText = error instanceof Error ? error.message : String(error);
      analysisSteps = [...analysisSteps, `오류: ${analysisText}`];
    } finally {
      analysisBusy = false;
    }
  }

  function startCandidateAnalysisStep() {
    if (!imageUrl || statusTone !== "ok" || analysisBusy) return;
    transitionWizardStep("rooms", "forward", detectRoomCandidatesByWalls);
  }

  function transitionWizardStep(nextStep, direction, afterTransition) {
    floorplanWizardDirection = direction;
    floorplanWizardAnimating = true;
    if (floorplanWizardTimer) window.clearTimeout(floorplanWizardTimer);
    floorplanWizardTimer = window.setTimeout(() => {
      floorplanWizardStep = nextStep;
      floorplanWizardAnimating = false;
      afterTransition?.();
    }, 220);
  }

  function goToImageStep() {
    finishSnapEdit();
    roomCandidates.select("");
    transitionWizardStep("image", "back");
  }

  function goToRoomsStep() {
    transitionWizardStep("rooms", "back");
  }

  function goToOcrStep() {
    finishSnapEdit();
    roomCandidates.select("");
    manualRoomDraft = { active: false, points: [] };
    manualRoomDraftHover = null;
    transitionWizardStep("ocr", "forward", () => {
      if (!ocrBusy) void runFloorplanOcr();
    });
  }

  function goBackToOcrStep() {
    roomCandidates.select("");
    transitionWizardStep("ocr", "back");
  }

  function goToRadarStep() {
    if (!canGoToRadarStep()) return;
    calculateRoomSizes({ keepError: true });
    initializeRadarPlacement();
    roomCandidates.select("");
    transitionWizardStep("radar", "forward");
  }

  function goBackToRadarStep() {
    transitionWizardStep("radar", "back");
  }

  function goToFinalStep() {
    initializeRadarPlacement();
    roomCandidates.select("");
    transitionWizardStep("final", "forward");
  }

  function workflowTitle() {
    if (floorplanWizardStep === "rooms") return "2단계 · 방 자동 인식";
    if (floorplanWizardStep === "ocr") return "3단계 · 방 이름/스케일";
    if (floorplanWizardStep === "radar") return "4단계 · 레이더 배치";
    if (floorplanWizardStep === "final") return "5단계 · 최종 확인";
    return "1단계 · 이미지 준비";
  }

  function workflowDescription() {
    if (floorplanWizardStep === "rooms") return "자동으로 찾은 방 후보를 확인하고, 필요한 방은 추가·분할·합치기로 정리합니다.";
    if (floorplanWizardStep === "ocr") return "방 이름을 확인하고 전체 평면도 가로/세로 길이를 입력합니다.";
    if (floorplanWizardStep === "radar") return "정리된 방과 전체 스케일을 기준으로 레이더 배치를 준비합니다.";
    if (floorplanWizardStep === "final") return "ESP32에 저장하기 전에 이미지, 방 후보, 레이더 배치 설정을 확인합니다.";
    return "PNG/JPG/WebP를 업로드하면 브라우저 메모리에서 WebP 저장 후보를 만듭니다. ESP32에는 아직 저장하지 않습니다.";
  }

  function updateRoomMeasurement(candidateId, field, value) {
    roomMeasurements = {
      ...roomMeasurements,
      [candidateId]: {
        ...(roomMeasurements[candidateId] ?? { width: "", height: "" }),
        [field]: normalizeDimensionInput(value)
      }
    };
    calculateRoomSizes({ keepError: true });
  }

  function updateFloorplanTotalSize(field, value) {
    floorplanTotalSize = {
      ...floorplanTotalSize,
      [field]: normalizeDimensionInput(value)
    };
    calculateRoomSizes({ keepError: true });
  }

  function togglePreciseRoomSizeEditing() {
    preciseRoomSizeEditing = !preciseRoomSizeEditing;
    calculateRoomSizes();
  }

  function normalizeDimensionInput(value) {
    return value.replace(/[^\d.]/g, "").replace(/(\..*)\./g, "$1");
  }

  function clampRadarScalePercent(value) {
    const numericValue = Number(value);
    if (!Number.isFinite(numericValue)) return 100;
    return Math.max(RADAR_SCALE_MIN_PERCENT, Math.min(RADAR_SCALE_MAX_PERCENT, Math.round(numericValue)));
  }

  function updateRadarScalePercent(value) {
    radarScalePercent = clampRadarScalePercent(value);
    commitRadarPlacement();
  }

  function nudgeRadarScalePercent(delta) {
    updateRadarScalePercent(radarScalePercent + delta);
  }

  function floorplanRadarConfig() {
    return deviceConfig?.floorplan?.radar ?? null;
  }

  function defaultRadarPlacement() {
    const bounds = floorplanScaleEstimate()?.outerBounds ?? getFloorplanOuterBounds();
    return {
      originX: bounds ? bounds.x + bounds.width / 2 : (debugInfo?.width ?? 1) / 2,
      originY: bounds ? bounds.y + bounds.height - 16 : (debugInfo?.height ?? 1) * 0.82,
      rotation: 0
    };
  }

  function initializeRadarPlacement() {
    radarOcclusionIgnoredEdges = deviceConfig?.floorplan?.radarOcclusionIgnoredEdges ?? [];
    const saved = floorplanRadarConfig();
    if (saved) {
      radarPlacement = {
        originX: saved.originX,
        originY: saved.originY,
        rotation: saved.rotation
      };
      radarScalePercent = clampRadarScalePercent((saved.scale || 1) * 100);
      return;
    }
    radarPlacement = defaultRadarPlacement();
  }

  function toggleRadarOcclusionEdit() {
    radarOcclusionEditActive = !radarOcclusionEditActive;
  }

  function toggleRadarOcclusionEdge(segment) {
    const segments = roomBoundaryWallSegments();
    const target = resolveOcclusionSegment(segment, segments);
    const keys = occlusionSegmentGroupKeys(target, segments);
    const ignored = new Set(radarOcclusionIgnoredEdges);
    const shouldIgnore = keys.some((key) => !ignored.has(key));

    for (const key of occlusionSegmentLegacyKeys(target, segments)) {
      ignored.delete(key);
    }

    if (shouldIgnore) {
      for (const key of keys) {
        ignored.add(key);
      }
    } else {
      for (const key of keys) {
        ignored.delete(key);
      }
    }

    radarOcclusionIgnoredEdges = [...ignored];
    commitRadarOcclusionEdges();
  }

  function resolveOcclusionSegment(segment, segments) {
    if (segment && typeof segment === "object") return segment;
    return segments.find((candidate) => candidate.id === segment || candidate.occlusionKey === segment) ?? { id: segment, occlusionKey: segment };
  }

  function occlusionSegmentGroupKeys(target, segments) {
    return uniqueStrings(
      occlusionSegmentGroup(target, segments)
        .map((segment) => segment.occlusionKey ?? segment.id)
        .filter(Boolean)
    );
  }

  function occlusionSegmentLegacyKeys(target, segments) {
    return uniqueStrings(
      occlusionSegmentGroup(target, segments)
        .flatMap((segment) => [segment.id, segment.occlusionKey])
        .filter(Boolean)
    );
  }

  function occlusionSegmentGroup(target, segments) {
    if (!target) return [];
    return segments.filter((segment) => areSameOcclusionWall(target, segment));
  }

  function areSameOcclusionWall(a, b) {
    if (!a || !b) return false;
    if ((a.occlusionKey ?? a.id) === (b.occlusionKey ?? b.id)) return true;
    const axis = occlusionSegmentAxis(a);
    if (axis !== occlusionSegmentAxis(b)) return false;
    if (axis === "horizontal") {
      return (
        Math.abs(segmentMidY(a) - segmentMidY(b)) <= RADAR_OCCLUSION_GROUP_LINE_TOLERANCE_PX &&
        rangesTouchOrOverlap(segmentRangeX(a), segmentRangeX(b), RADAR_OCCLUSION_GROUP_TOUCH_TOLERANCE_PX)
      );
    }
    if (axis === "vertical") {
      return (
        Math.abs(segmentMidX(a) - segmentMidX(b)) <= RADAR_OCCLUSION_GROUP_LINE_TOLERANCE_PX &&
        rangesTouchOrOverlap(segmentRangeY(a), segmentRangeY(b), RADAR_OCCLUSION_GROUP_TOUCH_TOLERANCE_PX)
      );
    }
    return false;
  }

  function occlusionSegmentAxis(segment) {
    if (segment.axis === "horizontal" || segment.axis === "vertical") return segment.axis;
    const dx = Math.abs(Number(segment.x2) - Number(segment.x1));
    const dy = Math.abs(Number(segment.y2) - Number(segment.y1));
    if (dy <= 1) return "horizontal";
    if (dx <= 1) return "vertical";
    return "diagonal";
  }

  function segmentMidX(segment) {
    return (Number(segment.x1) + Number(segment.x2)) / 2;
  }

  function segmentMidY(segment) {
    return (Number(segment.y1) + Number(segment.y2)) / 2;
  }

  function segmentRangeX(segment) {
    return [Math.min(Number(segment.x1), Number(segment.x2)), Math.max(Number(segment.x1), Number(segment.x2))];
  }

  function segmentRangeY(segment) {
    return [Math.min(Number(segment.y1), Number(segment.y2)), Math.max(Number(segment.y1), Number(segment.y2))];
  }

  function rangesTouchOrOverlap(a, b, tolerance) {
    return Math.max(a[0], b[0]) <= Math.min(a[1], b[1]) + tolerance;
  }

  function uniqueStrings(values) {
    return [...new Set(values)];
  }

  function commitRadarOcclusionEdges() {
    if (!onUpdateDeviceConfig) return;
    onUpdateDeviceConfig((current) => ({
      ...current,
      floorplan: {
        ...(current.floorplan ?? { enabled: true, hasImage: true }),
        enabled: true,
        hasImage: true,
        radarOcclusionIgnoredEdges: radarOcclusionIgnoredEdges
      }
    }));
  }

  function updateRadarPlacement(nextPlacement) {
    radarPlacement = {
      originX: nextPlacement.originX,
      originY: nextPlacement.originY,
      rotation: nextPlacement.rotation
    };
  }

  function commitRadarPlacement(nextPlacement = radarPlacement) {
    const scaleEstimate = floorplanScaleEstimate();
    if (!nextPlacement || !scaleEstimate || !onUpdateDeviceConfig) return;
    const scale = Math.round((radarScalePercent / 100) * 1000) / 1000;
    const radar = {
      originX: Math.round(nextPlacement.originX * 100) / 100,
      originY: Math.round(nextPlacement.originY * 100) / 100,
      rotation: Math.round(nextPlacement.rotation * 10) / 10,
      scale
    };
    onUpdateDeviceConfig((current) => ({
      ...current,
      floorplan: {
        ...(current.floorplan ?? { enabled: true, hasImage: true }),
        enabled: true,
        hasImage: true,
        scaleMmPerPx: (scaleEstimate.mmPerPxX + scaleEstimate.mmPerPxY) / 2,
        radar
      }
    }));
  }

  function parseDimensionInput(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
  }

  function candidatePointArray(candidate) {
    if (candidate.shape === "polygon" && candidate.points?.length) {
      return candidate.points;
    }
    return rectPointsFromBounds(candidate.rect);
  }

  function visibleRoomCandidates() {
    return roomCandidates.candidates.filter((candidate) => candidate.status !== "rejected");
  }

  function roomBoundaryWallSegments() {
    return visibleRoomCandidates().flatMap(candidateBoundarySegments);
  }

  function candidateBoundarySegments(candidate) {
      const points = candidatePointArray(candidate);
      if (points.length < 2) return [];
      return points.map((point, index) => {
        const next = points[(index + 1) % points.length];
        const [x1, y1] = point;
        const [x2, y2] = next;
        const dx = Math.abs(x2 - x1);
        const dy = Math.abs(y2 - y1);
        return {
          id: `room-${candidate.id}-${index}`,
          occlusionKey: wallSegmentGeometryKey(x1, y1, x2, y2),
          x1,
          y1,
          x2,
          y2,
          axis: dy <= 1 ? "horizontal" : dx <= 1 ? "vertical" : "diagonal"
        };
      });
  }

  function outerBoundsOcclusionSegments() {
    const bounds = getFloorplanOuterBounds();
    if (!bounds) return [];
    const x1 = bounds.x;
    const y1 = bounds.y;
    const x2 = bounds.x + bounds.width;
    const y2 = bounds.y + bounds.height;
    return [
      outerBoundsOcclusionSegment("top", x1, y1, x2, y1, "horizontal"),
      outerBoundsOcclusionSegment("right", x2, y1, x2, y2, "vertical"),
      outerBoundsOcclusionSegment("bottom", x2, y2, x1, y2, "horizontal"),
      outerBoundsOcclusionSegment("left", x1, y2, x1, y1, "vertical")
    ];
  }

  function outerBoundsOcclusionSegment(id, x1, y1, x2, y2, axis) {
    return {
      id: `outer-${id}`,
      occlusionKey: `outer-${id}`,
      locked: true,
      x1,
      y1,
      x2,
      y2,
      axis
    };
  }

  function radarOcclusionSegments() {
    return [...roomBoundaryWallSegments(), ...outerBoundsOcclusionSegments()];
  }

  function wallSegmentGeometryKey(x1, y1, x2, y2) {
    const first = [Math.round(x1), Math.round(y1)];
    const second = [Math.round(x2), Math.round(y2)];
    const [start, end] =
      first[0] < second[0] || (first[0] === second[0] && first[1] <= second[1])
        ? [first, second]
        : [second, first];
    return `wall:${start[0]},${start[1]}-${end[0]},${end[1]}`;
  }

  function getFloorplanOuterBounds() {
    const points = visibleRoomCandidates().flatMap(candidatePointArray);
    if (!points.length) return null;
    return rectFromPoints(points);
  }

  function floorplanScaleEstimate() {
    const outerBounds = getFloorplanOuterBounds();
    const widthMm = parseDimensionInput(floorplanTotalSize.width);
    const heightMm = parseDimensionInput(floorplanTotalSize.height);
    if (!outerBounds || outerBounds.width <= 0 || outerBounds.height <= 0 || !widthMm || !heightMm) return null;
    return {
      outerBounds,
      widthMm,
      heightMm,
      mmPerPxX: widthMm / outerBounds.width,
      mmPerPxY: heightMm / outerBounds.height
    };
  }

  function floorplanScaleSummary() {
    const scale = floorplanScaleEstimate();
    if (!scale) return null;
    return {
      widthMm: Math.round(scale.widthMm),
      heightMm: Math.round(scale.heightMm),
      widthPx: scale.outerBounds.width.toFixed(2),
      heightPx: scale.outerBounds.height.toFixed(2),
      mmPerPxX: scale.mmPerPxX.toFixed(2),
      mmPerPxY: scale.mmPerPxY.toFixed(2)
    };
  }

  function floorplanStorageDraft() {
    const scale = floorplanScaleEstimate();
    const width = debugInfo?.width;
    const height = debugInfo?.height;
    if (!scale || !width || !height) return null;
    const placement = radarPlacement ?? defaultRadarPlacement();
    const estimates = roomSizeCalculated
      ? roomSizeEstimates
      : Object.fromEntries(visibleRoomCandidates().map((candidate) => [candidate.id, estimateRoomSize(candidate, scale)]));

    return buildFloorplanStorageDocument({
      image: {
        width,
        height,
        bytes: imageBlob?.size,
        name: imageName,
        mime: imageBlob?.type || "image/webp"
      },
      scale,
      radar: {
        originX: placement.originX,
        originY: placement.originY,
        rotation: placement.rotation,
        scale: radarScalePercent / 100
      },
      rooms: visibleRoomCandidates(),
      roomMeasurements,
      roomSizeEstimates: estimates,
      ignoredOcclusionEdges: radarOcclusionIgnoredEdges,
      objects: storedFurnitureObjects()
    });
  }

  async function saveFloorplanToDevice() {
    const document = floorplanStorageDraft();
    if (!document || !imageBlob) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = "저장 후보를 만들 수 없습니다. 이미지와 전체 크기 입력을 확인하세요.";
      return;
    }
    if (!onSaveFloorplan) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = "평면도 저장 API가 준비되지 않았습니다.";
      return;
    }
    floorplanSaveBusy = true;
    floorplanSaveTone = "saving";
    floorplanSaveStatus = "ESP32에 평면도 이미지와 설정을 저장하는 중입니다.";
    try {
      await onSaveFloorplan(document, imageBlob);
      await loadStoredFloorplan();
      storedFloorplanDetected = true;
      storedFloorplanMode = "edit";
      storedFloorplanEditTool = "rooms";
      floorplanSaveTone = "ok";
      floorplanSaveStatus = "평면도 이미지와 설정을 저장했습니다.";
    } catch (error) {
      floorplanSaveTone = "error";
      floorplanSaveStatus = error instanceof Error ? error.message : String(error);
    } finally {
      floorplanSaveBusy = false;
    }
  }

  function canCalculateRoomSizes() {
    return Boolean(floorplanScaleEstimate());
  }

  function estimateRoomSize(candidate, scale) {
    const bounds = rectFromPoints(candidatePointArray(candidate));
    const measurement = roomMeasurements[candidate.id] ?? {};
    const manualWidthMm = parseDimensionInput(measurement.width ?? "");
    const manualHeightMm = parseDimensionInput(measurement.height ?? "");
    const touchesLeft = Math.abs(bounds.x - scale.outerBounds.x) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesRight = Math.abs(bounds.x + bounds.width - (scale.outerBounds.x + scale.outerBounds.width)) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesTop = Math.abs(bounds.y - scale.outerBounds.y) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesBottom = Math.abs(bounds.y + bounds.height - (scale.outerBounds.y + scale.outerBounds.height)) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const widthFromOuter = touchesLeft && touchesRight;
    const heightFromOuter = touchesTop && touchesBottom;
    const estimatedWidthMm = widthFromOuter ? scale.widthMm : bounds.width * scale.mmPerPxX;
    const estimatedHeightMm = heightFromOuter ? scale.heightMm : bounds.height * scale.mmPerPxY;
    return {
      widthPx: roundPoint(bounds.width).toFixed(2),
      heightPx: roundPoint(bounds.height).toFixed(2),
      widthMm: Math.round(manualWidthMm ?? estimatedWidthMm),
      heightMm: Math.round(manualHeightMm ?? estimatedHeightMm),
      estimatedWidthMm: Math.round(estimatedWidthMm),
      estimatedHeightMm: Math.round(estimatedHeightMm),
      widthFromOuter,
      heightFromOuter,
      widthManual: Boolean(manualWidthMm),
      heightManual: Boolean(manualHeightMm),
      manuallyEdited: Boolean(manualWidthMm || manualHeightMm)
    };
  }

  function calculateRoomSizes(options = {}) {
    const scale = floorplanScaleEstimate();
    if (!scale) {
      roomSizeEstimates = {};
      roomSizeCalculated = false;
      roomSizeError = options.keepError ? "" : "전체 평면도 가로/세로 길이를 입력해야 방 크기를 계산할 수 있습니다.";
      return;
    }

    roomSizeEstimates = Object.fromEntries(
      visibleRoomCandidates().map((candidate) => [candidate.id, estimateRoomSize(candidate, scale)])
    );
    roomSizeCalculated = true;
    roomSizeError = "";
  }

  function roomSizeCalculationMessage() {
    if (roomSizeError) return roomSizeError;
    if (roomSizeCalculated) return `방 ${Object.keys(roomSizeEstimates).length}개의 크기를 계산했습니다.`;
    if (!floorplanScaleEstimate()) return "전체 평면도 가로/세로 길이를 입력하면 방 크기를 대략 계산할 수 있습니다.";
    return "정밀 보정을 열면 방별 추정 크기와 직접 수정 입력란을 볼 수 있습니다.";
  }

  function canGoToRadarStep() {
    return Boolean(floorplanScaleEstimate());
  }

  async function runFloorplanOcr() {
    const ocrSource = originalImageBlob ?? imageBlob;
    if (!ocrSource || ocrBusy) return;
    ocrBusy = true;
    ocrDialogOpen = true;
    ocrProgress = 0.02;
    ocrStatusText = "OCR 분석을 준비하고 있습니다.";
    ocrSteps = ["OCR 분석을 시작했습니다."];
    ocrResult = null;
    ocrError = "";

    appendOcrStep(originalImageBlob ? "원본 업로드 이미지를 OCR 입력으로 사용합니다." : "WebP 저장 후보를 OCR 입력으로 사용합니다.");
    const job = createFloorplanOcrJob(
      ocrSource,
      (progress) => {
        ocrProgress = progress.progress;
        ocrStatusText = progress.message;
        if (shouldKeepOcrProgressLog(progress)) {
          appendOcrStep(progress.message);
        }
      },
      {
        roomCandidates: roomCandidates.candidates
      }
    );
    ocrJob = job;

    try {
      const result = await job.promise;
      ocrResult = result;
      const roomLabelResult = applyOcrRoomLabels(roomCandidates.candidates, result);
      if (roomLabelResult.matches.length) {
        roomCandidates.replace(roomLabelResult.candidates, roomCandidates.selectedCandidateId);
        appendOcrStep(`방 이름 후보 ${roomLabelResult.matches.length}개를 방 영역 기준으로 매칭했습니다.`);
      } else {
        appendOcrStep("방 영역 내부에서 적용할 수 있는 방 이름 후보를 찾지 못했습니다.");
      }
      ocrProgress = 1;
      ocrStatusText = "OCR 분석이 완료되었습니다.";
      appendOcrStep(
        `OCR 완료: 텍스트 후보 ${result.textCandidateBoxes}개, crop ${result.ocrRegions}개, 블록 ${result.rawBlockCount}개, 줄 ${result.lines.length}개, 단어 ${result.words.length}개를 찾았습니다.`
      );
    } catch (error) {
      if (error instanceof FloorplanOcrCancelledError) {
        ocrProgress = 0;
        ocrStatusText = "OCR 분석이 취소되었습니다.";
        appendOcrStep("사용자가 OCR 분석을 취소했습니다.");
      } else {
        ocrError = error instanceof Error ? error.message : String(error);
        ocrStatusText = "OCR 분석 중 오류가 발생했습니다.";
        appendOcrStep(`오류: ${ocrError}`);
      }
    } finally {
      ocrBusy = false;
      ocrJob = null;
    }
  }

  function appendOcrStep(message) {
    if (!message) return;
    const last = ocrSteps[ocrSteps.length - 1];
    if (last === message) return;
    ocrSteps = [...ocrSteps, message].slice(-80);
  }

  function shouldKeepOcrProgressLog(progress) {
    return progress.status !== "recognize" && progress.status !== "recognizing text";
  }

  async function cancelFloorplanOcr() {
    if (!ocrJob) {
      ocrDialogOpen = false;
      return;
    }
    ocrStatusText = "OCR 분석을 취소하는 중입니다.";
    appendOcrStep("OCR 작업 취소를 요청했습니다.");
    await ocrJob.cancel();
  }

  function closeFloorplanOcrDialog() {
    if (ocrBusy) return;
    ocrDialogOpen = false;
  }

  function resetOcrState() {
    ocrBusy = false;
    ocrDialogOpen = false;
    ocrProgress = 0;
    ocrStatusText = "아직 OCR을 실행하지 않았습니다.";
    ocrSteps = [];
    ocrResult = null;
    ocrError = "";
    showOcrOverlay = false;
    showWallMaskOverlay = false;
    ocrJob = null;
  }

  function ocrResultSummary() {
    return "";
  }

  function ocrPreviewWords() {
    return ocrResult?.words?.slice(0, 16) ?? [];
  }

  function ocrOverlayItems() {
    if (!ocrResult) return [];
    return ocrResult.lines.length ? ocrResult.lines : ocrResult.words;
  }

  function ocrKindCounts() {
    const items = ocrOverlayItems();
    return items.reduce(
      (counts, item) => {
        const kind = item.kind ?? "noise";
        counts[kind] = (counts[kind] ?? 0) + 1;
        return counts;
      },
      { "room-label": 0, dimension: 0, noise: 0 }
    );
  }

  function wallMaskReasonCounts() {
    const cells = analysisDebug?.wallMaskCells ?? [];
    return cells.reduce((counts, cell) => {
      const key = cell.source ?? "unknown";
      counts[key] = (counts[key] ?? 0) + 1;
      return counts;
    }, {});
  }

  function wallRejectedReasonCounts() {
    const cells = analysisDebug?.wallRejectedCells ?? [];
    return cells.reduce((counts, cell) => {
      const key = cell.source ?? "unknown";
      counts[key] = (counts[key] ?? 0) + 1;
      return counts;
    }, {});
  }

  function formatWallDebugCell(cell, state) {
    return [
      state,
      `x=${Math.round(cell.x)}`,
      `y=${Math.round(cell.y)}`,
      `grid=${cell.gridX},${cell.gridY}`,
      `source=${cell.source}`,
      `dark=${cell.darkPixels}`,
      cell.reason
    ].join(" · ");
  }

  function buildWallSelectionDebugText() {
    const selected = analysisDebug?.wallMaskCells ?? [];
    const rejected = analysisDebug?.wallRejectedCells ?? [];
    const lines = [
      "벽 선정 디버그",
      "",
      `이미지: ${debugInfo?.width ?? analysisDebug?.imageWidth ?? "-"} x ${debugInfo?.height ?? analysisDebug?.imageHeight ?? "-"}`,
      `격자: ${analysisDebug?.gridWidth ?? "-"} x ${analysisDebug?.gridHeight ?? "-"} / 셀 ${analysisDebug?.cellSize ?? "-"}px`,
      `선정된 벽 셀: ${selected.length}개`,
      `제외된 벽 후보 셀: ${rejected.length}개`,
      "",
      "[선정 이유별 개수]",
      ...Object.entries(wallMaskReasonCounts()).map(([reason, count]) => `${reason}: ${count}개`),
      "",
      "[제외 이유별 개수]",
      ...(rejected.length ? Object.entries(wallRejectedReasonCounts()).map(([reason, count]) => `${reason}: ${count}개`) : ["없음"]),
      "",
      "[선정된 벽 셀]",
      ...selected.map((cell) => formatWallDebugCell(cell, "선정")),
      "",
      "[제외된 벽 후보 셀]",
      ...(rejected.length ? rejected.map((cell) => formatWallDebugCell(cell, "제외")) : ["없음"])
    ];
    return lines.join("\n");
  }

  function downloadWallSelectionDebug() {
    if (!analysisDebug) return;
    const blob = new Blob([buildWallSelectionDebugText()], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    const baseName = imageName ? imageName.replace(/\.[^.]+$/, "") : "floorplan";
    link.href = url;
    link.download = `${baseName}-wall-debug.txt`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }


  function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return "-";
    if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
    return `${Math.round(bytes / 1024)} KB`;
  }

  function candidateSnapshot() {
    return {
      candidates: roomCandidates.candidates.map(cloneCandidate),
      selectedCandidateId: roomCandidates.selectedCandidateId,
      manualRoomDraft: {
        active: manualRoomDraft.active,
        points: manualRoomDraft.points.map(([x, y]) => [x, y])
      },
      manualRoomDraftHover: manualRoomDraftHover ? { ...manualRoomDraftHover } : null
    };
  }

  function pushCandidateHistory() {
    candidateUndoStack = [...candidateUndoStack, candidateSnapshot()].slice(-MAX_CANDIDATE_HISTORY);
    candidateRedoStack = [];
  }

  function restoreCandidateSnapshot(snapshot) {
    roomCandidates.replace(snapshot.candidates.map(cloneCandidate), snapshot.selectedCandidateId);
    snapEdit = { active: false, candidateId: "", edgeKey: "", original: null };
    manualRoomDraft = snapshot.manualRoomDraft
      ? {
          active: snapshot.manualRoomDraft.active,
          points: snapshot.manualRoomDraft.points.map(([x, y]) => [x, y])
        }
      : { active: false, points: [] };
    manualRoomDraftHover = snapshot.manualRoomDraftHover ? { ...snapshot.manualRoomDraftHover } : null;
  }

  function undoCandidateEdit() {
    if (!candidateUndoStack.length) return;
    const previous = candidateUndoStack[candidateUndoStack.length - 1];
    candidateUndoStack = candidateUndoStack.slice(0, -1);
    candidateRedoStack = [...candidateRedoStack, candidateSnapshot()].slice(-MAX_CANDIDATE_HISTORY);
    restoreCandidateSnapshot(previous);
  }

  function redoCandidateEdit() {
    if (!candidateRedoStack.length) return;
    const next = candidateRedoStack[candidateRedoStack.length - 1];
    candidateRedoStack = candidateRedoStack.slice(0, -1);
    candidateUndoStack = [...candidateUndoStack, candidateSnapshot()].slice(-MAX_CANDIDATE_HISTORY);
    restoreCandidateSnapshot(next);
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
    } else if (!event.ctrlKey && !event.metaKey && (event.key === "Delete" || event.key === "Backspace") && deleteSelectedStoredRadarZonePoint()) {
      event.preventDefault();
    } else if (!event.ctrlKey && !event.metaKey && key === "r" && storedFloorplanMode === "edit" && storedFloorplanEditTool === "furniture" && selectedFurnitureObjectId) {
      event.preventDefault();
      rotateStoredFurnitureObject(selectedFurnitureObjectId, 90);
    } else if ((event.ctrlKey || event.metaKey) && key === "z") {
      event.preventDefault();
      if (event.shiftKey) {
        redoCandidateEdit();
      } else {
        undoCandidateEdit();
      }
    } else if ((event.ctrlKey || event.metaKey) && key === "y") {
      event.preventDefault();
      redoCandidateEdit();
    }
  }

  function withCandidateHistory(action) {
    pushCandidateHistory();
    action();
  }

  function updateCandidatePoints(id, points, saveHistory = true) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate || points.length < 3) return;
    const nextCandidate = {
      ...candidate,
      shape: "polygon",
      points,
      rect: rectFromPoints(points),
      debug: {
        ...(candidate.debug ?? {}),
        finalPoints: points.length
      }
    };
    if (saveHistory) {
      withCandidateHistory(() => roomCandidates.update(id, nextCandidate));
    } else {
      roomCandidates.update(id, nextCandidate);
    }
  }

  function cleanupCandidate(id, saveHistory = true) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return false;
    const result = cleanupOrthogonalRoomCandidate(candidate);
    if (!result.changed) return false;
    if (saveHistory) {
      withCandidateHistory(() => roomCandidates.update(id, result.candidate));
    } else {
      roomCandidates.update(id, result.candidate);
    }
    return true;
  }

  function cleanupSelectedCandidate(saveHistory = true) {
    const id = roomCandidates.selectedCandidateId;
    return id ? cleanupCandidate(id, saveHistory) : false;
  }

  function clearFloorplanSelection(saveHistory = true) {
    cleanupSelectedCandidate(saveHistory);
    finishSnapEdit();
    selectedCandidateVertexIndex = -1;
    roomCandidates.select("");
  }

  function addCandidateVertex(id, edgeIndex, point) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points?.length || edgeIndex < 0) return;
    const editableCandidate = { ...candidate, points: candidate.points.map(([x, y]) => [x, y]) };
    const result = insertPolygonObjectPoint(editableCandidate, edgeIndex, point, {
      bounds: { width: debugInfo?.width, height: debugInfo?.height }
    });
    if (!result.changed) return;
    selectedCandidateVertexIndex = result.selectedPointIndex;
    updateCandidatePoints(id, result.item.points);
  }

  function deleteCandidateVertex(id, index) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points || candidate.points.length <= 3 || index < 0 || index >= candidate.points.length) return false;
    const editableCandidate = { ...candidate, points: candidate.points.map(([x, y]) => [x, y]) };
    const result = deletePolygonObjectPoint(editableCandidate, index);
    if (!result.changed) return false;
    selectedCandidateVertexIndex = result.selectedPointIndex;
    updateCandidatePoints(id, result.item.points);
    return true;
  }

  function deleteSelectedCandidateVertex() {
    if (!roomCandidates.selectedCandidateId || selectedCandidateVertexIndex < 0) return false;
    const editingInWizard = floorplanWizardStep === "rooms";
    const editingStoredRooms = storedFloorplanMode === "edit" && storedFloorplanEditTool === "rooms";
    if (!editingInWizard && !editingStoredRooms) return false;
    return deleteCandidateVertex(roomCandidates.selectedCandidateId, selectedCandidateVertexIndex);
  }

  function moveCandidateVertex(id, index, point) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate?.points || index < 0 || index >= candidate.points.length) return;
    const snapped = snapManualPoint(point);
    const editableCandidate = { ...candidate, points: candidate.points.map(([x, y]) => [x, y]) };
    const result = movePolygonObjectPoint(editableCandidate, index, snapped, {
      bounds: { width: debugInfo?.width, height: debugInfo?.height }
    });
    if (!result.changed) return;
    selectedCandidateVertexIndex = result.selectedPointIndex;
    updateCandidatePoints(id, result.item.points, false);
  }

  function selectCandidateVertex(id, index) {
    roomCandidates.select(id);
    selectedCandidateVertexIndex = index;
  }

  function beginCandidateVertexMove(id, index) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    selectCandidateVertex(id, index);
    pushCandidateHistory();
  }

  function endCandidateVertexMove(id) {
    cleanupCandidate(id, false);
  }

  function removeCandidateWithHistory(id) {
    withCandidateHistory(() => roomCandidates.remove(id));
  }

  function setCandidateStatusWithHistory(id, status) {
    withCandidateHistory(() => roomCandidates.setStatus(id, status));
  }

  function floorplanTransformStyle() {
    return `transform: translate(${floorplanView.x}px, ${floorplanView.y}px) scale(${floorplanView.scale}); transform-origin: 0 0;`;
  }

  function floorplanImageLayerStyle() {
    const width = debugInfo?.width ?? 1;
    const height = debugInfo?.height ?? 1;
    const maxWidthVh = height > 0 ? Math.round((width / height) * 82 * 10000) / 10000 : 82;
    return `width: min(100%, ${maxWidthVh}vh); aspect-ratio: ${width} / ${height};`;
  }

  function setFloorplanZoom(nextScale, origin = null) {
    const layer = document.querySelector(".floorplan-image-layer");
    const bounds = layer?.getBoundingClientRect?.();
    const width = bounds?.width ?? 0;
    const height = bounds?.height ?? 0;
    const scale = Math.min(5, Math.max(1, nextScale));
    if (!width || !height || scale === floorplanView.scale) {
      floorplanView = { ...floorplanView, scale, x: scale <= 1 ? 0 : floorplanView.x, y: scale <= 1 ? 0 : floorplanView.y };
      return;
    }
    const pointerX = origin?.x ?? width / 2;
    const pointerY = origin?.y ?? height / 2;
    const contentX = (pointerX - floorplanView.x) / floorplanView.scale;
    const contentY = (pointerY - floorplanView.y) / floorplanView.scale;
    const nextPan = clampFloorplanPan(
      pointerX - contentX * scale,
      pointerY - contentY * scale,
      scale,
      width,
      height
    );
    floorplanView = {
      ...floorplanView,
      scale,
      x: nextPan.x,
      y: nextPan.y
    };
  }

  function zoomFloorplan(direction) {
    setFloorplanZoom(floorplanView.scale * (direction > 0 ? 1.18 : 0.84));
  }

  function resetFloorplanZoom() {
    floorplanView = { ...floorplanView, scale: 1, x: 0, y: 0, panning: false };
  }

  function clampFloorplanPan(x, y, scale, width, height) {
    if (scale <= 1) return { x: 0, y: 0 };
    const minX = width * (1 - scale);
    const minY = height * (1 - scale);
    return {
      x: Math.min(0, Math.max(minX, x)),
      y: Math.min(0, Math.max(minY, y))
    };
  }

  function handleFloorplanWheel(event) {
    if (!imageUrl) return;
    event.preventDefault();
    const bounds = event.currentTarget.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    const pointerX = event.clientX - bounds.left;
    const pointerY = event.clientY - bounds.top;
    setFloorplanZoom(floorplanView.scale * (event.deltaY < 0 ? 1.12 : 0.89), { x: pointerX, y: pointerY });
  }

  function handleFloorplanPointerDown(event) {
    if (!imageUrl || event.button !== 1) return;
    event.preventDefault();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    floorplanView = {
      ...floorplanView,
      panning: true,
      lastX: event.clientX,
      lastY: event.clientY
    };
  }

  function handleFloorplanPointerMove(event) {
    if (!floorplanView.panning) return;
    const bounds = event.currentTarget.getBoundingClientRect();
    const nextPan = clampFloorplanPan(
      floorplanView.x + event.clientX - floorplanView.lastX,
      floorplanView.y + event.clientY - floorplanView.lastY,
      floorplanView.scale,
      bounds.width,
      bounds.height
    );
    floorplanView = {
      ...floorplanView,
      x: nextPan.x,
      y: nextPan.y,
      lastX: event.clientX,
      lastY: event.clientY
    };
  }

  function stopFloorplanPan(event) {
    if (!floorplanView.panning) return;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
    floorplanView = { ...floorplanView, panning: false };
  }

  function preventFloorplanAuxClick(event) {
    if (event.button === 1) event.preventDefault();
  }

  function shouldKeepFloorplanSelection(target) {
    return Boolean(target?.closest?.(
      ".floorplan-middle-column, .floorplan-candidates, .floorplan-candidate-card, .floorplan-candidate-select, .floorplan-candidate-editor, .floorplan-edit-tool-card, .floorplan-split-card, .floorplan-manual-card, .floorplan-candidate-group, .floorplan-candidate-edit-edge, .floorplan-candidate-vertex, .floorplan-snap-edge, .floorplan-snap-wall, .floorplan-split-draft-hit, .floorplan-split-draft-point, .floorplan-manual-draft-hit, .floorplan-manual-draft-point"
    ));
  }

  function handleFloorplanPanelClick(event) {
    if (shouldKeepFloorplanSelection(event.target)) return;
    if (!roomCandidates.selectedCandidateId && !snapEdit.active) return;
    clearFloorplanSelection();
  }

  function handleFloorplanPanelKeydown(event) {
    if (isEditableKeyTarget(event.target)) return;
    if (event.key !== "Delete" && event.key !== "Backspace") return;
    if (!deleteSelectedCandidateVertex()) return;
    event.preventDefault();
    event.stopPropagation();
  }

  function snapManualPoint(point) {
    const lineThreshold = 12;
    const cornerThreshold = 20;
    const cornerEndpointTolerance = 10;
    let cornerBest = null;
    const manualSnapSegments = snapSegments.length ? snapSegments : wallSegments;
    const horizontalSegments = manualSnapSegments.filter((segment) => segment.axis === "horizontal");
    const verticalSegments = manualSnapSegments.filter((segment) => segment.axis === "vertical");

    for (const horizontal of horizontalSegments) {
      const hMinX = Math.min(horizontal.x1, horizontal.x2);
      const hMaxX = Math.max(horizontal.x1, horizontal.x2);
      if (point.x < hMinX - cornerThreshold || point.x > hMaxX + cornerThreshold) continue;
      if (Math.abs(point.y - horizontal.y1) > cornerThreshold) continue;

      for (const vertical of verticalSegments) {
        const vMinY = Math.min(vertical.y1, vertical.y2);
        const vMaxY = Math.max(vertical.y1, vertical.y2);
        if (point.y < vMinY - cornerThreshold || point.y > vMaxY + cornerThreshold) continue;
        if (Math.abs(point.x - vertical.x1) > cornerThreshold) continue;

        const corner = { x: vertical.x1, y: horizontal.y1 };
        const touchesHorizontal = corner.x >= hMinX - cornerEndpointTolerance && corner.x <= hMaxX + cornerEndpointTolerance;
        const touchesVertical = corner.y >= vMinY - cornerEndpointTolerance && corner.y <= vMaxY + cornerEndpointTolerance;
        if (!touchesHorizontal || !touchesVertical) continue;

        const distance = Math.hypot(point.x - corner.x, point.y - corner.y);
        if (distance <= cornerThreshold && (!cornerBest || distance < cornerBest.distance)) {
          cornerBest = { point: corner, distance };
        }
      }
    }

    if (cornerBest) return cornerBest.point;

    let best = null;
    for (const segment of manualSnapSegments) {
      let snapped = null;
      if (segment.axis === "horizontal") {
        const minX = Math.min(segment.x1, segment.x2);
        const maxX = Math.max(segment.x1, segment.x2);
        if (point.x < minX - lineThreshold || point.x > maxX + lineThreshold) continue;
        const x = Math.min(maxX, Math.max(minX, point.x));
        snapped = { x, y: segment.y1 };
      } else if (segment.axis === "vertical") {
        const minY = Math.min(segment.y1, segment.y2);
        const maxY = Math.max(segment.y1, segment.y2);
        if (point.y < minY - lineThreshold || point.y > maxY + lineThreshold) continue;
        const y = Math.min(maxY, Math.max(minY, point.y));
        snapped = { x: segment.x1, y };
      }
      if (!snapped) continue;
      const distance = Math.hypot(point.x - snapped.x, point.y - snapped.y);
      if (distance <= lineThreshold && (!best || distance < best.distance)) {
        best = { point: snapped, distance };
      }
    }
    return best?.point ?? point;
  }

  function startRoomSplitDraft(id = roomCandidates.selectedCandidateId) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    finishSnapEdit();
    cancelManualRoomDraft();
    cancelRoomMergeDraft();
    roomCandidates.select(id);
    roomSplitDraft = { active: true, candidateId: id, points: [] };
    roomSplitDraftHover = null;
  }

  function cancelRoomSplitDraft() {
    roomSplitDraft = { active: false, candidateId: "", points: [] };
    roomSplitDraftHover = null;
  }

  function startRoomMergeDraft() {
    finishSnapEdit();
    cancelManualRoomDraft();
    cancelRoomSplitDraft();
    const initialIds = roomCandidates.selectedCandidateId ? [roomCandidates.selectedCandidateId] : [];
    roomMergeDraft = { active: true, candidateIds: initialIds };
  }

  function cancelRoomMergeDraft() {
    roomMergeDraft = { active: false, candidateIds: [] };
  }

  function selectRoomCandidate(id) {
    if (!roomMergeDraft.active) {
      if (roomCandidates.selectedCandidateId !== id) {
        selectedCandidateVertexIndex = -1;
      }
      roomCandidates.select(id);
      return;
    }
    const candidate = roomCandidates.candidates.find((item) => item.id === id && item.status !== "rejected");
    if (!candidate) return;
    const exists = roomMergeDraft.candidateIds.includes(id);
    const candidateIds = exists
      ? roomMergeDraft.candidateIds.filter((candidateId) => candidateId !== id)
      : [...roomMergeDraft.candidateIds, id].slice(-2);
    roomMergeDraft = { ...roomMergeDraft, candidateIds };
    selectedCandidateVertexIndex = -1;
    roomCandidates.select(id);
  }

  function canFinishRoomMergeDraft() {
    if (!roomMergeDraft.active || roomMergeDraft.candidateIds.length !== 2) return false;
    return roomMergeDraft.candidateIds.every((id) =>
      roomCandidates.candidates.some((candidate) => candidate.id === id && candidate.status !== "rejected")
    );
  }

  function finishRoomMergeDraft() {
    if (!canFinishRoomMergeDraft()) return;
    const [firstId, secondId] = roomMergeDraft.candidateIds;
    const first = roomCandidates.candidates.find((candidate) => candidate.id === firstId);
    const second = roomCandidates.candidates.find((candidate) => candidate.id === secondId);
    if (!first || !second) return;
    const firstIndex = roomCandidates.candidates.findIndex((candidate) => candidate.id === firstId);
    const secondIndex = roomCandidates.candidates.findIndex((candidate) => candidate.id === secondId);
    const insertIndex = Math.max(0, Math.min(firstIndex, secondIndex));
    const points = mergeRoomCandidatePoints(first, second);
    if (!points.length) return;
    const mergedCandidate = {
      ...first,
      id: `${first.id}_merge_${second.id}_${Date.now()}`,
      name: first.name || second.name || `방 ${insertIndex + 1}`,
      status: "candidate",
      confidence: Math.max(first.confidence ?? 0, second.confidence ?? 0),
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        ...(first.debug ?? {}),
        reason: "merge",
        finalPoints: points.length,
        closedLoop: true
      }
    };
    withCandidateHistory(() => {
      const candidates = roomCandidates.candidates.filter((candidate) => candidate.id !== firstId && candidate.id !== secondId);
      candidates.splice(insertIndex, 0, mergedCandidate);
      roomCandidates.replace(candidates, mergedCandidate.id);
    });
    cancelRoomMergeDraft();
  }

  function mergeRoomCandidatePoints(first, second) {
    const firstPoints = orientPolygonClockwise(normalizePolygonPoints(candidateSplitPoints(first)));
    const secondPoints = orientPolygonClockwise(normalizePolygonPoints(candidateSplitPoints(second)));
    if (firstPoints.length < 3 || secondPoints.length < 3) return [];
    const unionPoints = traceUnionBoundary(firstPoints, secondPoints);
    return unionPoints.length >= 3 ? unionPoints : [];
  }

  function getSplitCandidate() {
    return roomCandidates.candidates.find((candidate) => candidate.id === roomSplitDraft.candidateId) ?? null;
  }

  function splitDraftTargetRect() {
    return getSplitCandidate()?.rect ?? null;
  }

  function clampPointToRect(point, rect) {
    return {
      x: Math.min(rect.x + rect.width, Math.max(rect.x, point.x)),
      y: Math.min(rect.y + rect.height, Math.max(rect.y, point.y))
    };
  }

  function constrainSplitPoint(point) {
    const first = roomSplitDraft.points[0];
    if (!first) return point;
    const dx = Math.abs(point.x - first[0]);
    const dy = Math.abs(point.y - first[1]);
    return dx >= dy ? { x: point.x, y: first[1] } : { x: first[0], y: point.y };
  }

  function snapAndClampSplitPoint(point) {
    const rect = splitDraftTargetRect();
    const snapped = constrainSplitPoint(snapManualPoint(point));
    const clamped = rect ? clampPointToRect(snapped, rect) : snapped;
    return [
      Math.round(Math.min(debugInfo?.width ?? clamped.x, Math.max(0, clamped.x))),
      Math.round(Math.min(debugInfo?.height ?? clamped.y, Math.max(0, clamped.y)))
    ];
  }

  function addRoomSplitPoint(point) {
    if (!roomSplitDraft.active || !debugInfo || roomSplitDraft.points.length >= 2) return;
    const nextPoint = snapAndClampSplitPoint(point);
    roomSplitDraft = {
      ...roomSplitDraft,
      points: [...roomSplitDraft.points, nextPoint]
    };
    roomSplitDraftHover = null;
  }

  function previewRoomSplitPoint(point) {
    if (!roomSplitDraft.active || !debugInfo || roomSplitDraft.points.length !== 1) return;
    const [x, y] = snapAndClampSplitPoint(point);
    roomSplitDraftHover = { x, y };
  }

  function clearRoomSplitPreview() {
    roomSplitDraftHover = null;
  }

  function moveRoomSplitPoint(index, point) {
    if (!roomSplitDraft.active || !debugInfo || index < 0 || index >= roomSplitDraft.points.length) return;
    const points = roomSplitDraft.points.map(([x, y]) => [x, y]);
    points[index] = snapAndClampSplitPoint(point);
    roomSplitDraft = { ...roomSplitDraft, points };
    roomSplitDraftHover = null;
  }

  function roomSplitLine() {
    const first = roomSplitDraft.points[0];
    const second = roomSplitDraft.points[1] ?? (roomSplitDraftHover ? [roomSplitDraftHover.x, roomSplitDraftHover.y] : null);
    if (!first || !second) return null;
    return { x1: first[0], y1: first[1], x2: second[0], y2: second[1] };
  }

  function canFinishRoomSplitDraft() {
    const candidate = getSplitCandidate();
    const line = roomSplitLine();
    if (!candidate || !line || roomSplitDraft.points.length < 2) return false;
    return Boolean(splitRoomCandidateByLine(candidate, line));
  }

  function activeMapToolTitle() {
    if (snapEdit.active) return "벽에 맞추기";
    if (roomSplitDraft.active) return "방 나누기";
    if (roomMergeDraft.active) return "방 합치기";
    if (manualRoomDraft.active) return "방 추가";
    return "";
  }

  function activeMapToolMessage() {
    if (snapEdit.active) {
      return snapEdit.edgeKey
        ? "붙일 벽선을 선택한 뒤 완료를 누르세요."
        : "맞출 방의 변을 선택하세요.";
    }
    if (roomSplitDraft.active) {
      if (roomSplitDraft.points.length === 0) return "나눌 방 안에 분할선의 시작점을 찍으세요.";
      if (roomSplitDraft.points.length === 1) return "끝점을 찍으세요. 가로/세로 방향으로 자동 정렬합니다.";
      return canFinishRoomSplitDraft()
        ? "나누기 완료를 누르면 방 2개로 나뉩니다."
        : "분할선이 방을 제대로 가르지 못했습니다. 점을 다시 조정하세요.";
    }
    if (roomMergeDraft.active) {
      if (roomMergeDraft.candidateIds.length === 0) return "합칠 방 2개를 선택하세요.";
      if (roomMergeDraft.candidateIds.length === 1) return "합칠 다른 방을 하나 더 선택하세요.";
      return "완료를 누르면 선택한 방 2개가 하나로 합쳐집니다.";
    }
    if (manualRoomDraft.active) {
      if (manualRoomDraft.points.length < 3) return "평면도 위에 방 꼭짓점을 3개 이상 찍으세요.";
      return "완료를 누르면 새 방이 추가됩니다. 꼭짓점은 더 찍을 수 있습니다.";
    }
    return "";
  }

  function activeMapToolCanFinish() {
    if (snapEdit.active) return Boolean(snapEdit.edgeKey);
    if (roomSplitDraft.active) return canFinishRoomSplitDraft();
    if (roomMergeDraft.active) return canFinishRoomMergeDraft();
    if (manualRoomDraft.active) return manualRoomDraft.points.length >= 3;
    return false;
  }

  function finishActiveMapTool() {
    if (snapEdit.active) {
      finishSnapEdit();
    } else if (roomSplitDraft.active) {
      finishRoomSplitDraft();
    } else if (roomMergeDraft.active) {
      finishRoomMergeDraft();
    } else if (manualRoomDraft.active) {
      finishManualRoomDraft();
    }
  }

  function cancelActiveMapTool() {
    if (snapEdit.active) {
      cancelSnapEdit();
    } else if (roomSplitDraft.active) {
      cancelRoomSplitDraft();
    } else if (roomMergeDraft.active) {
      cancelRoomMergeDraft();
    } else if (manualRoomDraft.active) {
      cancelManualRoomDraft();
    }
  }

  function finishRoomSplitDraft() {
    const candidate = getSplitCandidate();
    const line = roomSplitLine();
    if (!candidate || !line || !canFinishRoomSplitDraft()) return;
    const splitResult = splitRoomCandidateByLine(candidate, line);
    if (!splitResult) return;
    const candidateIndex = roomCandidates.candidates.findIndex((item) => item.id === candidate.id);
    const splitStamp = Date.now();
    const baseName = `방 후보 ${candidateIndex + 1}`;
    const makeCandidate = (suffix, points) => ({
      ...candidate,
      id: `${candidate.id}_split_${splitStamp}_${suffix}`,
      name: `${baseName}-${suffix}`,
      status: "candidate",
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        ...(candidate.debug ?? {}),
        reason: "split",
        finalPoints: points.length,
        closedLoop: true
      }
    });
    const nextCandidates = [
      makeCandidate(1, splitResult[0]),
      makeCandidate(2, splitResult[1])
    ];
    withCandidateHistory(() => {
      const candidates = roomCandidates.candidates.flatMap((item) => item.id === candidate.id ? nextCandidates : [item]);
      roomCandidates.replace(candidates, nextCandidates[0].id);
    });
    cancelRoomSplitDraft();
  }

  function startManualRoomDraft() {
    finishSnapEdit();
    roomCandidates.select("");
    cancelRoomSplitDraft();
    cancelRoomMergeDraft();
    manualRoomDraft = { active: true, points: [] };
    manualRoomDraftHover = null;
  }

  function cancelManualRoomDraft() {
    manualRoomDraft = { active: false, points: [] };
    manualRoomDraftHover = null;
  }

  function previewManualRoomPoint(point) {
    if (!manualRoomDraft.active || !debugInfo) return;
    const snapped = snapAndConstrainManualPoint(point);
    manualRoomDraftHover = {
      x: Math.round(Math.min(debugInfo.width, Math.max(0, snapped.x))),
      y: Math.round(Math.min(debugInfo.height, Math.max(0, snapped.y)))
    };
  }

  function clearManualRoomPointPreview() {
    manualRoomDraftHover = null;
  }

  function constrainManualPointToStraightLine(point) {
    const lastPoint = manualRoomDraft.points.at(-1);
    if (!lastPoint) return point;
    const [lastX, lastY] = lastPoint;
    const dx = Math.abs(point.x - lastX);
    const dy = Math.abs(point.y - lastY);
    return dx >= dy
      ? { x: point.x, y: lastY }
      : { x: lastX, y: point.y };
  }

  function snapAndConstrainManualPoint(point) {
    return constrainManualPointToStraightLine(snapManualPoint(point));
  }

  function addManualRoomPoint(point) {
    if (!manualRoomDraft.active || !debugInfo) return;
    pushCandidateHistory();
    const snapped = snapAndConstrainManualPoint(point);
    const x = Math.round(Math.min(debugInfo.width, Math.max(0, snapped.x)));
    const y = Math.round(Math.min(debugInfo.height, Math.max(0, snapped.y)));
    manualRoomDraft = {
      active: true,
      points: [...manualRoomDraft.points, [x, y]]
    };
    manualRoomDraftHover = null;
  }

  function deleteManualRoomPoint(index) {
    if (!manualRoomDraft.active || index < 0 || index >= manualRoomDraft.points.length) return;
    pushCandidateHistory();
    manualRoomDraft = {
      active: true,
      points: manualRoomDraft.points.filter((_, pointIndex) => pointIndex !== index)
    };
    manualRoomDraftHover = null;
  }

  function finishManualRoomDraft() {
    if (!debugInfo || manualRoomDraft.points.length < 3) return;
    const points = manualRoomDraft.points.map(([x, y]) => [x, y]);
    const candidate = {
      id: `manual_room_${Date.now()}`,
      name: `방 ${roomCandidates.candidates.length + 1}`,
      kind: "unknown",
      confidence: 100,
      status: "candidate",
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        finalPoints: points.length,
        closedLoop: true,
        reason: "manual"
      }
    };
    const cleanedCandidate = cleanupOrthogonalRoomCandidate(candidate).candidate;
    withCandidateHistory(() => roomCandidates.add(cleanedCandidate));
    manualRoomDraft = { active: false, points: [] };
    manualRoomDraftHover = null;
  }

  function getSelectedCandidate() {
    return roomCandidates.candidates.find((candidate) => candidate.id === roomCandidates.selectedCandidateId) ?? null;
  }

  function startSnapEdit(id) {
    const candidate = roomCandidates.candidates.find((item) => item.id === id);
    if (!candidate) return;
    cancelManualRoomDraft();
    cancelRoomSplitDraft();
    cancelRoomMergeDraft();
    roomCandidates.select(id);
    snapEdit = {
      active: true,
      candidateId: id,
      edgeKey: "",
      original: cloneCandidate(candidate)
    };
  }

  function finishSnapEdit() {
    if (snapEdit.candidateId) cleanupCandidate(snapEdit.candidateId, false);
    snapEdit = { active: false, candidateId: "", edgeKey: "", original: null };
  }

  function cancelSnapEdit() {
    if (snapEdit.original) {
      roomCandidates.update(snapEdit.original.id, snapEdit.original);
    }
    finishSnapEdit();
  }

  function cloneCandidate(candidate) {
    return {
      ...candidate,
      rect: { ...candidate.rect },
      points: candidate.points?.map(([x, y]) => [x, y])
    };
  }

  function getSnapEdges() {
    if (!snapEdit.active) return [];
    const candidate = roomCandidates.candidates.find((item) => item.id === snapEdit.candidateId);
    if (!candidate) return [];
    return getCandidateEdges(candidate);
  }

  function getCandidateEdges(candidate) {
    return getRoomCandidateSnapEdges(candidate);
  }

  function selectSnapEdge(edgeKey) {
    snapEdit = { ...snapEdit, edgeKey };
  }

  function getSnapWallSegments() {
    if (!snapEdit.active || !snapEdit.edgeKey) return [];
    const edge = getSnapEdges().find((item) => item.key === snapEdit.edgeKey);
    if (!edge) return [];
    return getVisibleSnapWallCandidates(edge, snapSegments.length ? snapSegments : wallSegments, {
      imageWidth: debugInfo?.width,
      imageHeight: debugInfo?.height
    })
      .map(({ segment }) => segment);
  }

  function snapSelectedEdgeToWall(segmentId) {
    const candidate = getSelectedCandidate();
    const edge = getSnapEdges().find((item) => item.key === snapEdit.edgeKey);
    const segment = (snapSegments.length ? snapSegments : wallSegments).find((item) => item.id === segmentId);
    if (!candidate || !edge || !segment) return;
    const nextCandidate = candidate.shape === "polygon" && candidate.points?.length
      ? snapPolygonCandidate(candidate, edge, segment)
      : snapRectCandidate(candidate, edge, segment);
    withCandidateHistory(() => roomCandidates.update(candidate.id, nextCandidate));
  }

  function snapRectCandidate(candidate, edge, segment) {
    const rect = { ...candidate.rect };
    const minSize = 12;
    if (edge.key === "rect:top") {
      const bottom = rect.y + rect.height;
      rect.y = Math.min(segment.y1, bottom - minSize);
      rect.height = bottom - rect.y;
    } else if (edge.key === "rect:bottom") {
      rect.height = Math.max(minSize, segment.y1 - rect.y);
    } else if (edge.key === "rect:left") {
      const right = rect.x + rect.width;
      rect.x = Math.min(segment.x1, right - minSize);
      rect.width = right - rect.x;
    } else if (edge.key === "rect:right") {
      rect.width = Math.max(minSize, segment.x1 - rect.x);
    }
    return { ...candidate, rect: normalizeRect(rect) };
  }

  function snapPolygonCandidate(candidate, edge, segment) {
    const points = candidate.points.map(([x, y]) => [x, y]);
    const index = edge.index;
    const nextIndex = (index + 1) % points.length;
    if (edge.axis === "horizontal") {
      points[index] = [points[index][0], segment.y1];
      points[nextIndex] = [points[nextIndex][0], segment.y1];
    } else {
      points[index] = [segment.x1, points[index][1]];
      points[nextIndex] = [segment.x1, points[nextIndex][1]];
    }
    return {
      ...candidate,
      points,
      rect: rectFromPoints(points)
    };
  }

  function candidateSplitPoints(candidate) {
    if (candidate.shape === "polygon" && candidate.points?.length >= 3) {
      return candidate.points.map(([x, y]) => [x, y]);
    }
    const { x, y, width, height } = candidate.rect;
    return [
      [x, y],
      [x + width, y],
      [x + width, y + height],
      [x, y + height]
    ];
  }

  function splitRoomCandidateByLine(candidate, line) {
    const points = normalizePolygonPoints(candidateSplitPoints(candidate));
    if (points.length < 3) return null;
    const vertical = Math.abs(line.x2 - line.x1) < Math.abs(line.y2 - line.y1);
    const splitValue = vertical
      ? Math.round((line.x1 + line.x2) / 2)
      : Math.round((line.y1 + line.y2) / 2);
    const center = {
      x: (line.x1 + line.x2) / 2,
      y: (line.y1 + line.y2) / 2
    };
    const intersections = findAxisLineIntersections(points, vertical ? "vertical" : "horizontal", splitValue)
      .sort((a, b) => Math.hypot(a.point[0] - center.x, a.point[1] - center.y) - Math.hypot(b.point[0] - center.x, b.point[1] - center.y));
    if (intersections.length < 2) return null;

    const selected = intersections.slice(0, 2).sort((a, b) => a.position - b.position);
    const [first, second] = selected;
    if (first.edgeIndex === second.edgeIndex) return null;
    if (Math.hypot(first.point[0] - second.point[0], first.point[1] - second.point[1]) < 16) return null;

    const firstPolygon = normalizePolygonPoints([first.point, ...walkPolygonPoints(points, first.edgeIndex, second.edgeIndex), second.point]);
    const secondPolygon = normalizePolygonPoints([second.point, ...walkPolygonPoints(points, second.edgeIndex, first.edgeIndex), first.point]);
    if (!validSplitPolygon(firstPolygon) || !validSplitPolygon(secondPolygon)) return null;
    return [firstPolygon, secondPolygon];
  }

  function findAxisLineIntersections(points, axis, value) {
    const intersections = [];
    const epsilon = 0.001;
    for (let index = 0; index < points.length; index += 1) {
      const start = points[index];
      const end = points[(index + 1) % points.length];
      const startAxis = axis === "vertical" ? start[0] : start[1];
      const endAxis = axis === "vertical" ? end[0] : end[1];
      const delta = endAxis - startAxis;
      if (Math.abs(delta) <= epsilon) continue;
      const t = (value - startAxis) / delta;
      if (t < -epsilon || t > 1 + epsilon) continue;
      const clampedT = Math.min(1, Math.max(0, t));
      const point = [
        roundPoint(start[0] + (end[0] - start[0]) * clampedT),
        roundPoint(start[1] + (end[1] - start[1]) * clampedT)
      ];
      if (intersections.some((item) => samePoint(item.point, point))) continue;
      intersections.push({
        edgeIndex: index,
        position: index + clampedT,
        point
      });
    }
    return intersections;
  }

  function walkPolygonPoints(points, fromEdgeIndex, toEdgeIndex) {
    const result = [];
    let index = (fromEdgeIndex + 1) % points.length;
    const stop = (toEdgeIndex + 1) % points.length;
    while (index !== stop) {
      result.push(points[index]);
      index = (index + 1) % points.length;
    }
    return result;
  }

  function traceUnionBoundary(firstPoints, secondPoints) {
    const firstEdges = splitPolygonEdges(firstPoints, secondPoints);
    const secondEdges = splitPolygonEdges(secondPoints, firstPoints);
    const segments = [];
    for (const edge of firstEdges) {
      if (!pointInPolygon(edge.midpoint, secondPoints) && !edgeOnPolygonBoundary(edge, secondPoints)) segments.push(edge);
    }
    for (const edge of secondEdges) {
      if (!pointInPolygon(edge.midpoint, firstPoints) && !edgeOnPolygonBoundary(edge, firstPoints)) segments.push(edge);
    }
    const uniqueSegments = removeDuplicateSegments(segments);
    const loops = traceSegmentLoops(uniqueSegments);
    return loops.sort((a, b) => Math.abs(polygonArea(b)) - Math.abs(polygonArea(a)))[0] ?? [];
  }

  function splitPolygonEdges(points, otherPoints) {
    const edges = [];
    for (let index = 0; index < points.length; index += 1) {
      const start = points[index];
      const end = points[(index + 1) % points.length];
      const vertical = Math.abs(start[0] - end[0]) < 0.001;
      const horizontal = Math.abs(start[1] - end[1]) < 0.001;
      if (!vertical && !horizontal) continue;
      const cuts = [0, 1];
      const minX = Math.min(start[0], end[0]);
      const maxX = Math.max(start[0], end[0]);
      const minY = Math.min(start[1], end[1]);
      const maxY = Math.max(start[1], end[1]);
      for (let otherIndex = 0; otherIndex < otherPoints.length; otherIndex += 1) {
        const otherStart = otherPoints[otherIndex];
        const otherEnd = otherPoints[(otherIndex + 1) % otherPoints.length];
        if (vertical) {
          if (pointBetween(otherStart[1], minY, maxY) && Math.abs(otherStart[0] - start[0]) < 0.001) cuts.push((otherStart[1] - start[1]) / (end[1] - start[1]));
          if (segmentsIntersectVerticalHorizontal(start, end, otherStart, otherEnd)) cuts.push((otherStart[1] - start[1]) / (end[1] - start[1]));
        } else if (horizontal) {
          if (pointBetween(otherStart[0], minX, maxX) && Math.abs(otherStart[1] - start[1]) < 0.001) cuts.push((otherStart[0] - start[0]) / (end[0] - start[0]));
          if (segmentsIntersectVerticalHorizontal(otherStart, otherEnd, start, end)) cuts.push((otherStart[0] - start[0]) / (end[0] - start[0]));
        }
      }
      const sortedCuts = [...new Set(cuts.map((value) => Math.round(Math.min(1, Math.max(0, value)) * 100000) / 100000))]
        .sort((a, b) => a - b);
      for (let cutIndex = 0; cutIndex < sortedCuts.length - 1; cutIndex += 1) {
        const t1 = sortedCuts[cutIndex];
        const t2 = sortedCuts[cutIndex + 1];
        if (Math.abs(t2 - t1) < 0.00001) continue;
        const a = [
          roundPoint(start[0] + (end[0] - start[0]) * t1),
          roundPoint(start[1] + (end[1] - start[1]) * t1)
        ];
        const b = [
          roundPoint(start[0] + (end[0] - start[0]) * t2),
          roundPoint(start[1] + (end[1] - start[1]) * t2)
        ];
        edges.push({ start: a, end: b, midpoint: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2] });
      }
    }
    return edges;
  }

  function segmentsIntersectVerticalHorizontal(verticalStart, verticalEnd, horizontalStart, horizontalEnd) {
    if (Math.abs(verticalStart[0] - verticalEnd[0]) >= 0.001) return false;
    if (Math.abs(horizontalStart[1] - horizontalEnd[1]) >= 0.001) return false;
    return pointBetween(verticalStart[0], Math.min(horizontalStart[0], horizontalEnd[0]), Math.max(horizontalStart[0], horizontalEnd[0]))
      && pointBetween(horizontalStart[1], Math.min(verticalStart[1], verticalEnd[1]), Math.max(verticalStart[1], verticalEnd[1]));
  }

  function edgeOnPolygonBoundary(edge, points) {
    return points.some((start, index) => {
      const end = points[(index + 1) % points.length];
      return pointOnSegment(edge.midpoint, start, end);
    });
  }

  function removeDuplicateSegments(segments) {
    const map = new Map();
    for (const segment of segments) {
      const key = segmentKey(segment.start, segment.end);
      const existing = map.get(key);
      if (existing) {
        map.delete(key);
      } else {
        map.set(key, segment);
      }
    }
    return [...map.values()];
  }

  function traceSegmentLoops(segments) {
    const adjacency = new Map();
    for (const segment of segments) {
      const startKey = pointKey(segment.start);
      if (!adjacency.has(startKey)) adjacency.set(startKey, []);
      adjacency.get(startKey).push(segment);
    }
    const used = new Set();
    const loops = [];
    for (const segment of segments) {
      const startKey = directedSegmentKey(segment);
      if (used.has(startKey)) continue;
      const loop = [segment.start];
      let current = segment;
      while (current && !used.has(directedSegmentKey(current))) {
        used.add(directedSegmentKey(current));
        loop.push(current.end);
        if (samePoint(current.end, loop[0])) break;
        const candidates = adjacency.get(pointKey(current.end)) ?? [];
        current = candidates.find((item) => !used.has(directedSegmentKey(item))) ?? null;
      }
      const normalized = normalizePolygonPoints(loop);
      if (normalized.length >= 3) loops.push(normalized);
    }
    return loops;
  }

  function pointInPolygon(point, points) {
    if (points.some((start, index) => pointOnSegment(point, start, points[(index + 1) % points.length]))) return false;
    let inside = false;
    for (let index = 0, previous = points.length - 1; index < points.length; previous = index, index += 1) {
      const [xi, yi] = points[index];
      const [xj, yj] = points[previous];
      const intersects = yi > point[1] !== yj > point[1]
        && point[0] < ((xj - xi) * (point[1] - yi)) / (yj - yi) + xi;
      if (intersects) inside = !inside;
    }
    return inside;
  }

  function pointOnSegment(point, start, end) {
    const cross = (point[0] - start[0]) * (end[1] - start[1]) - (point[1] - start[1]) * (end[0] - start[0]);
    if (Math.abs(cross) > 0.001) return false;
    return pointBetween(point[0], Math.min(start[0], end[0]), Math.max(start[0], end[0]))
      && pointBetween(point[1], Math.min(start[1], end[1]), Math.max(start[1], end[1]));
  }

  function pointBetween(value, min, max) {
    return value >= min - 0.001 && value <= max + 0.001;
  }

  function segmentKey(a, b) {
    const first = pointKey(a);
    const second = pointKey(b);
    return first < second ? `${first}|${second}` : `${second}|${first}`;
  }

  function directedSegmentKey(segment) {
    return `${pointKey(segment.start)}>${pointKey(segment.end)}`;
  }

  function pointKey(point) {
    return `${roundPoint(point[0])},${roundPoint(point[1])}`;
  }

  function normalizePolygonPoints(points) {
    const result = [];
    for (const point of points) {
      const normalized = [roundPoint(point[0]), roundPoint(point[1])];
      if (!result.length || !samePoint(result[result.length - 1], normalized)) {
        result.push(normalized);
      }
    }
    if (result.length > 1 && samePoint(result[0], result[result.length - 1])) {
      result.pop();
    }
    return result;
  }

  function validSplitPolygon(points) {
    return points.length >= 3 && Math.abs(polygonArea(points)) >= 256;
  }

  function polygonArea(points) {
    let area = 0;
    for (let index = 0; index < points.length; index += 1) {
      const [x1, y1] = points[index];
      const [x2, y2] = points[(index + 1) % points.length];
      area += x1 * y2 - x2 * y1;
    }
    return area / 2;
  }

  function orientPolygonClockwise(points) {
    return polygonArea(points) < 0 ? [...points].reverse() : points;
  }

  function samePoint(a, b) {
    return Math.abs(a[0] - b[0]) < 0.001 && Math.abs(a[1] - b[1]) < 0.001;
  }

  function roundPoint(value) {
    return Math.round(value * 100) / 100;
  }

  function normalizeRect(rect) {
    const x = Math.min(rect.x, rect.x + rect.width);
    const y = Math.min(rect.y, rect.y + rect.height);
    return {
      x,
      y,
      width: Math.abs(rect.width),
      height: Math.abs(rect.height)
    };
  }

  function rectFromPoints(points) {
    const xs = points.map(([x]) => x);
    const ys = points.map(([, y]) => y);
    const minX = Math.min(...xs);
    const maxX = Math.max(...xs);
    const minY = Math.min(...ys);
    const maxY = Math.max(...ys);
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
  }

  function rectPointsFromBounds(rect) {
    const x1 = rect.x;
    const y1 = rect.y;
    const x2 = rect.x + rect.width;
    const y2 = rect.y + rect.height;
    return [[x1, y1], [x2, y1], [x2, y2], [x1, y2]];
  }

  async function checkStoredFloorplan() {
    storedFloorplanCheckBusy = true;
    storedFloorplanCheckError = "";
    try {
      const status = await loadFloorplanStorageStatus({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher });
      const hasStoredFloorplan = status.ok === true && status.hasConfig === true && status.hasImage === true;
      storedFloorplanDetected = hasStoredFloorplan;
      if (hasStoredFloorplan) {
        await loadStoredFloorplan();
      }
    } catch (error) {
      storedFloorplanDetected = false;
      storedFloorplanCheckError = error instanceof Error ? error.message : String(error);
    } finally {
      storedFloorplanCheckBusy = false;
    }
  }

  async function loadStoredFloorplan() {
    const [document, image] = await Promise.all([
      loadFloorplanStorageDocument({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher }),
      loadFloorplanStorageImage({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher })
    ]);
    if (storedFloorplanImageUrl) URL.revokeObjectURL(storedFloorplanImageUrl);
    storedFloorplanDocument = document;
    storedFloorplanScaleSize = {
      width: String(document.scale.widthMm ?? ""),
      height: String(document.scale.heightMm ?? "")
    };
    storedFloorplanDirty = false;
    storedFloorplanSaveStatus = "";
    storedFloorplanSaveTone = "idle";
    selectedFurnitureObjectId = "";
    storedFloorplanImageUrl = URL.createObjectURL(image);
    debugInfo = {
      width: document.image.width,
      height: document.image.height,
      size: document.image.bytes ?? image.size,
      type: document.image.mime,
      ok: true
    };
    const candidates = document.rooms.map(storedRoomToCandidate);
    roomCandidates.setCandidates(candidates);
    const storedSegments = document.rooms.flatMap((room) => storedRoomBoundarySegmentsFromRoom(room));
    wallSegments = storedSegments;
    snapSegments = storedSegments;
  }

  function storedRoomToCandidate(room) {
    const points = room.pointsPx.map(([x, y]) => [x, y]);
    return {
      id: room.id,
      name: room.name,
      kind: room.kind ?? "unknown",
      confidence: 100,
      status: "confirmed",
      shape: "polygon",
      rect: rectFromPoints(points),
      points,
      debug: {
        reason: "stored",
        finalPoints: points.length,
        closedLoop: true
      }
    };
  }

  function requestStoredFloorplanDelete() {
    storedFloorplanDeleteError = "";
    storedFloorplanDeleteOpen = true;
  }

  function cancelStoredFloorplanDelete() {
    if (storedFloorplanDeleteBusy) return;
    storedFloorplanDeleteOpen = false;
    storedFloorplanDeleteError = "";
  }

  async function confirmStoredFloorplanDelete() {
    storedFloorplanDeleteBusy = true;
    storedFloorplanDeleteError = "";
    try {
      await deleteFloorplanStorage({ baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher });
      if (storedFloorplanImageUrl) URL.revokeObjectURL(storedFloorplanImageUrl);
      storedFloorplanImageUrl = "";
      storedFloorplanDocument = null;
      storedFloorplanDetected = false;
      storedFloorplanDeleteOpen = false;
      storedFloorplanMode = "view";
      storedFloorplanEditTool = "rooms";
      storedFloorplanScaleSize = { width: "", height: "" };
      storedFloorplanDirty = false;
      storedFloorplanSaveStatus = "";
      storedFloorplanSaveTone = "idle";
      selectedFurnitureObjectId = "";
      roomCandidates.clear();
      onFloorplanDeleted?.();
    } catch (error) {
      storedFloorplanDeleteError = error instanceof Error ? error.message : String(error);
    } finally {
      storedFloorplanDeleteBusy = false;
    }
  }

  function storedRoomPoints(room) {
    return room.pointsPx.map(([x, y]) => `${x},${y}`).join(" ");
  }

  function storedRoomCenter(room) {
    const points = room.pointsPx;
    if (!points.length) return [0, 0];
    const sum = points.reduce(
      (acc, [x, y]) => {
        acc[0] += x;
        acc[1] += y;
        return acc;
      },
      [0, 0]
    );
    return [sum[0] / points.length, sum[1] / points.length];
  }

  function storedFloorplanScaleEstimate() {
    const document = storedFloorplanDocument;
    if (!document) return null;
    const [x, y, width, height] = document.scale.outerBoundsPx;
    return {
      outerBounds: { x, y, width, height },
      widthMm: document.scale.widthMm,
      heightMm: document.scale.heightMm,
      mmPerPxX: document.scale.mmPerPxX,
      mmPerPxY: document.scale.mmPerPxY
    };
  }

  function storedFloorplanScaleInputValid() {
    return Boolean(parseDimensionInput(storedFloorplanScaleSize.width) && parseDimensionInput(storedFloorplanScaleSize.height));
  }

  function updateStoredFloorplanTotalSize(field, value) {
    storedFloorplanScaleSize = {
      ...storedFloorplanScaleSize,
      [field]: normalizeDimensionInput(value)
    };
    applyStoredFloorplanScaleFromInput();
  }

  function applyStoredFloorplanScaleFromInput() {
    if (!storedFloorplanDocument) return;
    const widthMm = parseDimensionInput(storedFloorplanScaleSize.width);
    const heightMm = parseDimensionInput(storedFloorplanScaleSize.height);
    if (!widthMm || !heightMm) {
      storedFloorplanSaveStatus = "전체 가로/세로 길이를 모두 입력해야 저장할 수 있습니다.";
      storedFloorplanSaveTone = "error";
      return;
    }
    const [x, y, width, height] = storedFloorplanDocument.scale.outerBoundsPx;
    if (width <= 0 || height <= 0) return;
    const scale = {
      widthMm: Math.round(widthMm),
      heightMm: Math.round(heightMm),
      outerBoundsPx: [x, y, width, height],
      mmPerPxX: roundPoint(widthMm / width),
      mmPerPxY: roundPoint(heightMm / height)
    };
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      scale,
      rooms: currentStoredFloorplanRooms(scale)
    };
    storedFloorplanDirty = true;
    storedFloorplanSaveStatus = "전체 크기와 방별 추정 치수를 다시 계산했습니다.";
    storedFloorplanSaveTone = "ok";
  }

  function currentStoredFloorplanRooms(scale = storedFloorplanDocument?.scale) {
    if (!storedFloorplanDocument || !scale) return [];
    const previousRooms = new Map(storedFloorplanDocument.rooms.map((room) => [room.id, room]));
    return visibleRoomCandidates().map((candidate) => storedRoomFromCandidate(candidate, scale, previousRooms.get(candidate.id)));
  }

  function storedRoomFromCandidate(candidate, scale, previousRoom = null) {
    const points = candidatePointArray(candidate).map(([x, y]) => [roundPoint(x), roundPoint(y)]);
    const bounds = rectFromPoints(points);
    const outerBounds = {
      x: scale.outerBoundsPx[0],
      y: scale.outerBoundsPx[1],
      width: scale.outerBoundsPx[2],
      height: scale.outerBoundsPx[3]
    };
    const touchesLeft = Math.abs(bounds.x - outerBounds.x) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesRight = Math.abs(bounds.x + bounds.width - (outerBounds.x + outerBounds.width)) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesTop = Math.abs(bounds.y - outerBounds.y) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const touchesBottom = Math.abs(bounds.y + bounds.height - (outerBounds.y + outerBounds.height)) <= OUTER_BOUNDS_TOUCH_TOLERANCE_PX;
    const estimatedWidthMm = touchesLeft && touchesRight ? scale.widthMm : bounds.width * scale.mmPerPxX;
    const estimatedHeightMm = touchesTop && touchesBottom ? scale.heightMm : bounds.height * scale.mmPerPxY;
    const manualSize = Boolean(previousRoom?.manualSize);
    return {
      id: candidate.id,
      name: candidate.name,
      kind: candidate.kind ?? "unknown",
      pointsPx: points,
      widthMm: Math.round(manualSize && previousRoom?.widthMm ? previousRoom.widthMm : estimatedWidthMm),
      heightMm: Math.round(manualSize && previousRoom?.heightMm ? previousRoom.heightMm : estimatedHeightMm),
      manualSize
    };
  }

  function currentStoredFloorplanDocument() {
    if (!storedFloorplanDocument) return null;
    return {
      ...storedFloorplanDocument,
      rooms: currentStoredFloorplanRooms(storedFloorplanDocument.scale),
      occlusion: {
        ignoredEdges: storedFloorplanDocument.occlusion?.ignoredEdges ?? []
      },
      objects: storedFurnitureObjects().map(clampFurnitureObjectToOuterBounds)
    };
  }

  function canSaveStoredFloorplan() {
    return Boolean(storedFloorplanDocument && storedFloorplanScaleInputValid() && !storedFloorplanSaveBusy);
  }

  async function saveStoredFloorplanToDevice() {
    const document = currentStoredFloorplanDocument();
    if (!document || !storedFloorplanScaleInputValid()) {
      storedFloorplanSaveStatus = "저장할 수 없습니다. 전체 가로/세로 길이를 확인하세요.";
      storedFloorplanSaveTone = "error";
      return;
    }
    storedFloorplanSaveBusy = true;
    storedFloorplanSaveTone = "saving";
    storedFloorplanSaveStatus = "ESP32에 평면도 설정을 저장하는 중입니다.";
    try {
      await saveFloorplanStorageDocument(document, { baseUrl: floorplanStorageBaseUrl, fetcher: floorplanStorageFetcher });
      storedFloorplanDocument = document;
      storedFloorplanDirty = false;
      storedFloorplanSaveTone = "ok";
      storedFloorplanSaveStatus = "평면도 설정을 저장했습니다.";
    } catch (error) {
      storedFloorplanSaveTone = "error";
      storedFloorplanSaveStatus = error instanceof Error ? error.message : String(error);
    } finally {
      storedFloorplanSaveBusy = false;
    }
  }

  function storedFloorplanRadarPlacement() {
    const document = storedFloorplanDocument;
    if (!document) return null;
    return {
      originX: document.radar.originPx[0],
      originY: document.radar.originPx[1],
      rotation: document.radar.rotationDeg
    };
  }

  function updateStoredFloorplanRadarPlacement(nextPlacement) {
    if (!storedFloorplanDocument) return;
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      radar: {
        ...storedFloorplanDocument.radar,
        originPx: [Math.round(nextPlacement.originX * 100) / 100, Math.round(nextPlacement.originY * 100) / 100],
        rotationDeg: Math.round(nextPlacement.rotation * 10) / 10
      }
    };
    storedFloorplanDirty = true;
  }

  function storedRadarZoneEditSource() {
    if (storedFloorplanMode !== "edit") return "";
    if (storedFloorplanEditTool === "zones") return "zones";
    if (storedFloorplanEditTool === "calibration") return "calibration";
    return "";
  }

  function beginStoredRadarZoneTool(tool) {
    storedFloorplanEditTool = tool;
    showStoredRadarOverlay = true;
    selectedStoredRadarZonePointIndex = -1;
    const zones = tool === "calibration" ? (deviceConfig?.calibrationZones ?? []) : (deviceConfig?.zones ?? []);
    if (!zones.some((zone) => zone.id === selectedStoredRadarZoneId)) {
      selectedStoredRadarZoneId = zones[0]?.id ?? "";
    }
  }

  function selectedStoredRadarZone(source = storedRadarZoneEditSource()) {
    const zones = source === "calibration" ? (deviceConfig?.calibrationZones ?? []) : (deviceConfig?.zones ?? []);
    return zones.find((zone) => zone.id === selectedStoredRadarZoneId) ?? null;
  }

  function selectStoredRadarZone(zoneId, pointIndex = -1) {
    selectedStoredRadarZoneId = zoneId;
    selectedStoredRadarZonePointIndex = pointIndex;
  }

  function updateStoredRadarZone(source, zoneId, updater) {
    if (!onUpdateDeviceConfig) return;
    onUpdateDeviceConfig((current) => {
      if (source === "calibration") {
        return {
          ...current,
          calibrationZones: (current.calibrationZones ?? []).map((zone) => zone.id === zoneId ? updater(zone) : zone)
        };
      }
      return {
        ...current,
        zones: current.zones.map((zone) => zone.id === zoneId ? updater(zone) : zone)
      };
    });
  }

  function updateStoredRadarConfig(mutator) {
    if (!onUpdateDeviceConfig) return null;
    let result = null;
    onUpdateDeviceConfig((current) => {
      result = mutator(current);
      return result.config;
    });
    return result;
  }

  function addStoredRadarZone() {
    const result = updateStoredRadarConfig((current) => addSoftwareZone(current));
    if (!result) return;
    if (!result.changed) {
      storedFloorplanSaveTone = "error";
      storedFloorplanSaveStatus = result.message ?? "감지 구역을 추가할 수 없습니다.";
      return;
    }
    selectedStoredRadarZoneId = result.selectedZoneId ?? "";
    selectedStoredRadarZonePointIndex = -1;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = "감지 구역을 추가했습니다.";
  }

  function deleteSelectedStoredRadarZone() {
    if (!selectedStoredRadarZoneId) return;
    const result = updateStoredRadarConfig((current) => deleteZone(current, selectedStoredRadarZoneId));
    if (!result?.changed) return;
    selectedStoredRadarZoneId = result.selectedZoneId ?? "";
    selectedStoredRadarZonePointIndex = -1;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = "구역을 삭제했습니다.";
  }

  function renameSelectedStoredRadarZone(name) {
    if (!selectedStoredRadarZoneId || storedFloorplanEditTool !== "zones") return;
    updateStoredRadarConfig((current) => renameZone(current, selectedStoredRadarZoneId, name));
  }

  function setSelectedStoredRadarZoneType(type) {
    if (!selectedStoredRadarZoneId || storedFloorplanEditTool !== "zones") return;
    updateStoredRadarConfig((current) => setZoneType(current, selectedStoredRadarZoneId, type));
  }

  function setSelectedStoredCalibrationZoneType(type) {
    if (!selectedStoredRadarZoneId || storedFloorplanEditTool !== "calibration") return;
    updateStoredRadarConfig((current) => setCalibrationZoneType(current, selectedStoredRadarZoneId, type));
  }

  function convertSelectedStoredRadarZoneToRect() {
    if (!selectedStoredRadarZoneId || storedFloorplanEditTool !== "zones") return;
    const result = updateStoredRadarConfig((current) => convertZoneToRectInConfig(current, selectedStoredRadarZoneId));
    if (!result?.changed) return;
    selectedStoredRadarZonePointIndex = -1;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = "감지 구역을 사각형으로 정리했습니다.";
  }

  function moveStoredRadarZone(source, zone, startPoint, currentPoint) {
    const start = { x: startPoint[0], y: startPoint[1] };
    const current = { x: currentPoint[0], y: currentPoint[1] };
    updateStoredRadarZone(source, zone.id, () => moveRadarZone(zone, start, current));
  }

  function moveStoredRadarZonePoint(source, zone, pointIndex, point) {
    if (pointIndex < 0 || pointIndex >= zone.points.length) return;
    updateStoredRadarZone(source, zone.id, (current) => updateRadarZonePoint(current, pointIndex, { x: point[0], y: point[1] }));
  }

  function addStoredRadarZonePoint(source, zone, edgeIndex, point) {
    if (source !== "zones") return;
    const result = updateStoredRadarConfig((current) => insertZonePointInConfig(current, zone.id, edgeIndex, { x: point[0], y: point[1] }));
    if (!result) return;
    if (!result.changed) {
      storedFloorplanSaveTone = "error";
      storedFloorplanSaveStatus = result.message ?? "꼭짓점을 추가할 수 없습니다.";
      return;
    }
    selectedStoredRadarZoneId = result.selectedZoneId ?? zone.id;
    selectedStoredRadarZonePointIndex = result.selectedPointIndex ?? -1;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = "감지 구역 꼭짓점을 추가했습니다.";
  }

  function commitStoredRadarZoneEdit(source, zoneId) {
    selectedStoredRadarZoneId = zoneId;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = source === "calibration" ? "오탐 영역을 수정했습니다." : "감지 구역을 수정했습니다.";
  }

  function deleteSelectedStoredRadarZonePoint() {
    if (storedFloorplanMode !== "edit" || storedFloorplanEditTool !== "zones") return false;
    if (!selectedStoredRadarZoneId || selectedStoredRadarZonePointIndex < 0) return false;
    const result = updateStoredRadarConfig((current) => deleteZonePointInConfig(current, selectedStoredRadarZoneId, selectedStoredRadarZonePointIndex));
    if (!result?.changed) return false;
    selectedStoredRadarZonePointIndex = result.selectedPointIndex ?? -1;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = "감지 구역 꼭짓점을 삭제했습니다.";
    return true;
  }

  function storedFurnitureObjects() {
    return storedFloorplanDocument?.objects ?? [];
  }

  function selectedFurnitureObject() {
    return storedFurnitureObjects().find((object) => object.id === selectedFurnitureObjectId) ?? null;
  }

  function defaultFurniturePlacement(assetId) {
    const asset = findFloorplanFurnitureAsset(assetId);
    const scale = storedFloorplanScaleEstimate();
    if (!asset || !scale) return null;
    const selectedCandidate = visibleRoomCandidates().find((candidate) => candidate.id === roomCandidates.selectedCandidateId);
    const bounds = selectedCandidate ? rectFromPoints(candidatePointArray(selectedCandidate)) : scale.outerBounds;
    const widthPx = Math.max(36, Math.min(bounds.width * 0.9, bounds.width * asset.widthRatio));
    const heightPx = Math.max(28, Math.min(bounds.height * 0.9, bounds.height * asset.heightRatio));
    const centerX = bounds.x + bounds.width / 2;
    const centerY = bounds.y + bounds.height / 2;
    return clampFurnitureObjectToOuterBounds({
      id: `object_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`,
      asset: asset.id,
      xPx: roundPoint(centerX - widthPx / 2),
      yPx: roundPoint(centerY - heightPx / 2),
      widthPx: roundPoint(widthPx),
      heightPx: roundPoint(heightPx),
      rotationDeg: 0
    });
  }

  function clampFurnitureObjectToOuterBounds(object) {
    const scale = storedFloorplanScaleEstimate();
    if (!scale) return object;
    const bounds = scale.outerBounds;
    const widthPx = Math.min(Math.max(18, object.widthPx), bounds.width);
    const heightPx = Math.min(Math.max(18, object.heightPx), bounds.height);
    return {
      ...object,
      xPx: roundPoint(Math.max(bounds.x, Math.min(bounds.x + bounds.width - widthPx, object.xPx))),
      yPx: roundPoint(Math.max(bounds.y, Math.min(bounds.y + bounds.height - heightPx, object.yPx))),
      widthPx: roundPoint(widthPx),
      heightPx: roundPoint(heightPx)
    };
  }

  function markStoredFurnitureChanged(message = "가구 배치를 변경했습니다.") {
    storedFloorplanDirty = true;
    storedFloorplanSaveTone = "ok";
    storedFloorplanSaveStatus = message;
  }

  function addStoredFurnitureObject(assetId) {
    if (!storedFloorplanDocument) return;
    const object = defaultFurniturePlacement(assetId);
    if (!object) return;
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      objects: [...storedFurnitureObjects(), object]
    };
    selectedFurnitureObjectId = object.id;
    markStoredFurnitureChanged("가구를 추가했습니다.");
  }

  function updateStoredFurnitureObject(id, patch) {
    if (!storedFloorplanDocument) return;
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      objects: storedFurnitureObjects().map((object) => (
        object.id === id
          ? {
              ...object,
              ...clampFurnitureObjectToOuterBounds({ ...object, ...patch }),
              rotationDeg: roundPoint(patch.rotationDeg ?? object.rotationDeg ?? 0)
            }
          : object
      ))
    };
    markStoredFurnitureChanged();
  }

  function rotateStoredFurnitureObject(id, deltaDeg) {
    const object = storedFurnitureObjects().find((item) => item.id === id);
    if (!object) return;
    updateStoredFurnitureObject(id, {
      rotationDeg: (object.rotationDeg ?? 0) + deltaDeg
    });
    selectedFurnitureObjectId = id;
    markStoredFurnitureChanged("가구를 회전했습니다.");
  }

  function deleteStoredFurnitureObject(id) {
    if (!storedFloorplanDocument) return;
    storedFloorplanDocument = {
      ...storedFloorplanDocument,
      objects: storedFurnitureObjects().filter((object) => object.id !== id)
    };
    if (selectedFurnitureObjectId === id) selectedFurnitureObjectId = "";
    markStoredFurnitureChanged("가구를 삭제했습니다.");
  }

  function storedRoomBoundarySegments() {
    return visibleRoomCandidates().flatMap(candidateBoundarySegments);
  }

  function storedRoomBoundarySegmentsFromRoom(room) {
    const points = room.pointsPx;
    if (points.length < 2) return [];
    return points.map((point, index) => {
      const next = points[(index + 1) % points.length];
      const [x1, y1] = point;
      const [x2, y2] = next;
      const dx = Math.abs(x2 - x1);
      const dy = Math.abs(y2 - y1);
      return {
        id: `stored-room-${room.id}-${index}`,
        occlusionKey: wallSegmentGeometryKey(x1, y1, x2, y2),
        x1,
        y1,
        x2,
        y2,
        axis: dy <= 1 ? "horizontal" : dx <= 1 ? "vertical" : "diagonal"
      };
    });
  }

  function storedOuterBoundsOcclusionSegments() {
    const scale = storedFloorplanScaleEstimate();
    if (!scale) return [];
    const bounds = scale.outerBounds;
    const x1 = bounds.x;
    const y1 = bounds.y;
    const x2 = bounds.x + bounds.width;
    const y2 = bounds.y + bounds.height;
    return [
      outerBoundsOcclusionSegment("stored-top", x1, y1, x2, y1, "horizontal"),
      outerBoundsOcclusionSegment("stored-right", x2, y1, x2, y2, "vertical"),
      outerBoundsOcclusionSegment("stored-bottom", x2, y2, x1, y2, "horizontal"),
      outerBoundsOcclusionSegment("stored-left", x1, y2, x1, y1, "vertical")
    ];
  }

  function storedRadarOcclusionSegments() {
    return [...storedRoomBoundarySegments(), ...storedOuterBoundsOcclusionSegments()];
  }

  function storedFloorplanImageFrameStyle() {
    const width = storedFloorplanDocument?.image?.width ?? 1;
    const height = storedFloorplanDocument?.image?.height ?? 1;
    const maxWidthVh = height > 0 ? Math.round((width / height) * 72 * 10000) / 10000 : 72;
    return `width: min(100%, ${maxWidthVh}vh); aspect-ratio: ${width} / ${height};`;
  }

  onMount(() => {
    void checkStoredFloorplan();
    window.addEventListener("keydown", handleHistoryKeydown);
    return () => window.removeEventListener("keydown", handleHistoryKeydown);
  });

  onDestroy(() => {
    if (floorplanWizardTimer) window.clearTimeout(floorplanWizardTimer);
    void ocrJob?.cancel?.();
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    if (storedFloorplanImageUrl) URL.revokeObjectURL(storedFloorplanImageUrl);
  });
</script>

{#if storedFloorplanCheckBusy || storedFloorplanDetected}
  <section
    class={`floorplan-stored-panel ${storedFloorplanMode === "edit" ? "edit-step" : ""}`}
    aria-label="저장된 평면도"
  >
    {#if storedFloorplanCheckBusy}
      <div class="floorplan-stored-loading" aria-live="polite">
        <div class="floorplan-loading-spinner" aria-hidden="true"></div>
        <strong>로딩중입니다</strong>
      </div>
    {:else}
      <aside class="floorplan-stored-controls">
        <div class="floorplan-workflow-card">
          <div>
            <strong>저장된 평면도</strong>
            <span>ESP32에 저장된 평면도 데이터를 확인했습니다.</span>
          </div>
          <dl class="floorplan-stored-summary">
            <div>
              <dt>상태</dt>
              <dd>데이터 확인됨</dd>
            </div>
            <div>
              <dt>모드</dt>
              <dd>{storedFloorplanMode === "edit" ? "편집" : "보기"}</dd>
            </div>
            <div>
              <dt>평면도</dt>
              <dd>{storedFloorplanDocument?.image?.name ?? "준비됨"}</dd>
            </div>
            <div>
              <dt>방</dt>
              <dd>{storedFloorplanDocument?.rooms?.length ?? 0}개</dd>
            </div>
          </dl>
        </div>

        <div class="floorplan-stored-tools">
          <button
            type="button"
            data-active={storedFloorplanMode === "view" ? "true" : "false"}
            onclick={() => (storedFloorplanMode = "view")}
          >
            보기
          </button>
          <button
            type="button"
            data-active={storedFloorplanMode === "edit" ? "true" : "false"}
            onclick={() => {
              storedFloorplanMode = "edit";
              storedFloorplanEditTool = "rooms";
            }}
          >
            편집
          </button>
          <button type="button" class="danger-button" onclick={requestStoredFloorplanDelete}>새로 만들기</button>
          <button type="button" onclick={saveStoredFloorplanToDevice} disabled={!canSaveStoredFloorplan()}>
            {storedFloorplanSaveBusy ? "저장 중" : storedFloorplanDirty ? "변경사항 저장" : "저장"}
          </button>
        </div>

        {#if storedFloorplanMode === "edit"}
          {#if storedFloorplanEditTool === "rooms"}
            <RoomEditTools
              candidates={roomCandidates.candidates}
              selectedCandidateId={roomCandidates.selectedCandidateId}
              manualRoomDraft={manualRoomDraft}
              roomSplitDraft={roomSplitDraft}
              roomMergeDraft={roomMergeDraft}
              snapEdit={snapEdit}
              onStartSplitDraft={startRoomSplitDraft}
              onStartMergeDraft={startRoomMergeDraft}
              onStartSnapEdit={startSnapEdit}
            />
          {/if}

          <div class="floorplan-stored-edit-mode-tools">
            <button
              type="button"
              data-active={storedFloorplanEditTool === "rooms" ? "true" : "false"}
              onclick={() => (storedFloorplanEditTool = "rooms")}
            >
              방 편집
            </button>
            <button
              type="button"
              data-active={storedFloorplanEditTool === "scale" ? "true" : "false"}
              onclick={() => (storedFloorplanEditTool = "scale")}
            >
              전체 평면도 크기
            </button>
            <button
              type="button"
              data-active={storedFloorplanEditTool === "radar" ? "true" : "false"}
              onclick={() => (storedFloorplanEditTool = "radar")}
            >
              레이더 위치 변경
            </button>
            <button
              type="button"
              data-active={storedFloorplanEditTool === "zones" ? "true" : "false"}
              onclick={() => beginStoredRadarZoneTool("zones")}
            >
              감지 구역 편집
            </button>
            <button
              type="button"
              data-active={storedFloorplanEditTool === "calibration" ? "true" : "false"}
              onclick={() => beginStoredRadarZoneTool("calibration")}
            >
              오탐 영역 편집
            </button>
            <button
              type="button"
              data-active={storedFloorplanEditTool === "furniture" ? "true" : "false"}
              onclick={() => (storedFloorplanEditTool = "furniture")}
            >
              가구 배치
            </button>
          </div>
        {/if}

        {#if storedFloorplanSaveStatus}
          <div class="floorplan-status floorplan-stored-save-status" data-tone={storedFloorplanSaveTone}>
            <strong>{storedFloorplanSaveTone === "ok" ? "저장 준비" : storedFloorplanSaveTone === "error" ? "확인 필요" : "저장 중"}</strong>
            <span>{storedFloorplanSaveStatus}</span>
          </div>
        {/if}
      </aside>

      {#if storedFloorplanMode === "edit"}
        <aside class="floorplan-middle-column floorplan-stored-middle-column">
          {#if storedFloorplanEditTool === "rooms"}
            <div class="floorplan-manual-card floorplan-fixed-text">
              <strong>수동 방 추가</strong>
              <button
                type="button"
                onclick={startManualRoomDraft}
                disabled={snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active}
              >
                방 추가
              </button>
            </div>
            <RoomEditStep
              candidates={roomCandidates.candidates}
              selectedCandidateId={roomCandidates.selectedCandidateId}
              roomSplitDraft={roomSplitDraft}
              roomMergeDraft={roomMergeDraft}
              snapEdit={snapEdit}
              onSelect={selectRoomCandidate}
              onRename={roomCandidates.rename}
              onConfirm={(id) => setCandidateStatusWithHistory(id, "confirmed")}
              onReject={(id) => setCandidateStatusWithHistory(id, "rejected")}
              onRemove={removeCandidateWithHistory}
              onStartSnapEdit={startSnapEdit}
              onFinishSnapEdit={finishSnapEdit}
              onCancelSnapEdit={cancelSnapEdit}
              onStartSplitDraft={startRoomSplitDraft}
              onFinishSplitDraft={finishRoomSplitDraft}
              onCancelSplitDraft={cancelRoomSplitDraft}
              canFinishSplitDraft={canFinishRoomSplitDraft}
              onStartMergeDraft={startRoomMergeDraft}
              onFinishMergeDraft={finishRoomMergeDraft}
              onCancelMergeDraft={cancelRoomMergeDraft}
              canFinishMergeDraft={canFinishRoomMergeDraft}
            />
          {:else if storedFloorplanEditTool === "scale"}
            <div class="floorplan-total-size-card">
              <strong>전체 평면도 크기</strong>
              <span>하늘색 최외곽 박스를 기준으로 전체 가로/세로 길이를 수정합니다.</span>
              <div class="floorplan-total-size-fields reference-room">
                <label>
                  <span>전체 가로</span>
                  <input
                    inputmode="decimal"
                    value={storedFloorplanScaleSize.width}
                    placeholder="mm"
                    oninput={(event) => updateStoredFloorplanTotalSize("width", event.currentTarget.value)}
                  />
                </label>
                <label>
                  <span>전체 세로</span>
                  <input
                    inputmode="decimal"
                    value={storedFloorplanScaleSize.height}
                    placeholder="mm"
                    oninput={(event) => updateStoredFloorplanTotalSize("height", event.currentTarget.value)}
                  />
                </label>
              </div>
              {#if storedFloorplanScaleEstimate()}
                {@const scale = storedFloorplanScaleEstimate()}
                <em data-tone={storedFloorplanScaleInputValid() ? "ok" : "error"}>
                  기준 박스 {scale.outerBounds.width.toFixed(2)} x {scale.outerBounds.height.toFixed(2)}px · {scale.mmPerPxX.toFixed(2)} / {scale.mmPerPxY.toFixed(2)} mm/px
                </em>
              {:else}
                <em data-tone="error">저장된 최외곽 박스를 확인할 수 없습니다.</em>
              {/if}
            </div>
          {:else if storedFloorplanEditTool === "zones" || storedFloorplanEditTool === "calibration"}
            {@const radarZoneSource = storedRadarZoneEditSource()}
            {@const radarZones = radarZoneSource === "calibration" ? (deviceConfig?.calibrationZones ?? []) : (deviceConfig?.zones ?? [])}
            {@const selectedRadarZone = selectedStoredRadarZone(radarZoneSource)}
            <div class="floorplan-furniture-card">
              <strong>{radarZoneSource === "calibration" ? "오탐 영역 편집" : "감지 구역 편집"}</strong>
              <span>평면도 위의 구역을 드래그하거나 꼭짓점을 움직여 수정합니다. 감지 구역의 변을 더블클릭하면 꼭짓점이 추가됩니다.</span>
              {#if radarZoneSource === "zones"}
                <button type="button" onclick={addStoredRadarZone}>구역 추가</button>
              {/if}
              <div class="floorplan-furniture-list">
                {#if !radarZones.length}
                  <em>{radarZoneSource === "calibration" ? "등록된 오탐 영역이 없습니다." : "등록된 감지 구역이 없습니다."}</em>
                {:else}
                  {#each radarZones as zone (zone.id)}
                    <button
                      type="button"
                      class="floorplan-furniture-list-item"
                      data-active={selectedStoredRadarZoneId === zone.id ? "true" : "false"}
                      onclick={() => selectStoredRadarZone(zone.id, -1)}
                    >
                      <span>{zoneDisplayName(zone)}</span>
                      <small>{zone.shape === "polygon" ? `${zone.points.length}점 다각형` : "사각형"} · {zone.type}</small>
                    </button>
                  {/each}
                {/if}
              </div>
              {#if selectedRadarZone}
                {#if radarZoneSource === "zones"}
                  <label class="zone-name-field">
                    <span>구역 이름</span>
                    <input
                      type="text"
                      value={selectedRadarZone.name || ""}
                      placeholder="예: 침대, 책상"
                      oninput={(event) => renameSelectedStoredRadarZone(event.currentTarget.value)}
                    />
                  </label>
                  <div class="zone-type-buttons">
                    <button
                      class={`zone-type-button detection${selectedRadarZone.type === "detection" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredRadarZoneType("detection")}
                    >
                      감지
                    </button>
                    <button
                      class={`zone-type-button filter${selectedRadarZone.type === "filter" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredRadarZoneType("filter")}
                    >
                      제외
                    </button>
                    <button
                      class={`zone-type-button disabled${selectedRadarZone.type === "disabled" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredRadarZoneType("disabled")}
                    >
                      비활성
                    </button>
                  </div>
                  <button type="button" disabled={selectedRadarZone.shape === "rect"} onclick={convertSelectedStoredRadarZoneToRect}>
                    사각형으로 정리
                  </button>
                  <button
                    type="button"
                    disabled={selectedStoredRadarZonePointIndex < 0 || selectedRadarZone.shape !== "polygon" || selectedRadarZone.points.length <= 3}
                    onclick={deleteSelectedStoredRadarZonePoint}
                  >
                    선택 꼭짓점 삭제
                  </button>
                {:else}
                  <div class="zone-type-buttons">
                    <button
                      class={`zone-type-button filter${selectedRadarZone.type === "filter" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredCalibrationZoneType("filter")}
                    >
                      차단
                    </button>
                    <button
                      class={`zone-type-button reduced${selectedRadarZone.type === "reduced" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredCalibrationZoneType("reduced")}
                    >
                      둔감
                    </button>
                    <button
                      class={`zone-type-button disabled${selectedRadarZone.type === "disabled" ? " selected" : ""}`}
                      type="button"
                      onclick={() => setSelectedStoredCalibrationZoneType("disabled")}
                    >
                      꺼짐
                    </button>
                  </div>
                {/if}
                <button type="button" class="danger-button" onclick={deleteSelectedStoredRadarZone}>
                  선택 구역 삭제
                </button>
              {/if}
            </div>
          {:else if storedFloorplanEditTool === "furniture"}
            <div class="floorplan-furniture-card">
              <strong>가구 배치</strong>
              <span>가구를 선택하면 선택한 방 중앙에 추가됩니다. 평면도 위에서 드래그해 위치를 조정하세요.</span>
              <div class="floorplan-furniture-palette">
                {#each FLOORPLAN_FURNITURE_ASSETS as asset}
                  <button type="button" onclick={() => addStoredFurnitureObject(asset.id)}>
                    <img src={asset.url} alt="" />
                    <span>{asset.label}</span>
                  </button>
                {/each}
              </div>
              <div class="floorplan-furniture-list">
                {#if !storedFurnitureObjects().length}
                  <em>배치된 가구가 없습니다.</em>
                {:else}
                  {#each storedFurnitureObjects() as object (object.id)}
                    {@const asset = findFloorplanFurnitureAsset(object.asset)}
                    <button
                      type="button"
                      class="floorplan-furniture-list-item"
                      data-active={selectedFurnitureObjectId === object.id ? "true" : "false"}
                      onclick={() => (selectedFurnitureObjectId = object.id)}
                    >
                      <span>{asset?.label ?? object.asset}</span>
                      <small>{Math.round(object.widthPx)} x {Math.round(object.heightPx)}px</small>
                    </button>
                  {/each}
                {/if}
              </div>
              {#if selectedFurnitureObject()}
                <button type="button" class="danger-button" onclick={() => deleteStoredFurnitureObject(selectedFurnitureObjectId)}>
                  선택한 가구 삭제
                </button>
              {/if}
            </div>
          {:else}
            <div class="floorplan-stored-tool-placeholder">
              <strong>레이더 위치 변경</strong>
              <span>오른쪽 평면도에서 레이더 본체를 드래그하거나 회전 핸들을 움직입니다.</span>
            </div>
          {/if}
        </aside>
      {/if}

      <section class="floorplan-stored-workspace" aria-label="저장된 평면도 미리보기">
        <div class="floorplan-stage">
          {#if storedFloorplanDocument && storedFloorplanImageUrl}
            <div class="floorplan-stored-preview">
              <div class="floorplan-stored-image-frame" style={storedFloorplanImageFrameStyle()}>
                {#if storedFloorplanMode === "edit"}
                  <EditorToolbar ariaLabel="저장된 평면도 편집 도구">
                    <button type="button" title="되돌리기 (Ctrl+Z)" onclick={undoCandidateEdit} disabled={!candidateUndoStack.length}>↶</button>
                    <button type="button" title="다시하기 (Ctrl+Y)" onclick={redoCandidateEdit} disabled={!candidateRedoStack.length}>↷</button>
                    {#if storedFloorplanEditTool === "furniture"}
                      <button
                        type="button"
                        title="선택한 가구를 반시계 방향으로 90도 회전"
                        onclick={() => rotateStoredFurnitureObject(selectedFurnitureObjectId, -90)}
                        disabled={!selectedFurnitureObjectId}
                      >
                        ⟲ 90°
                      </button>
                      <button
                        type="button"
                        title="선택한 가구를 시계 방향으로 90도 회전"
                        onclick={() => rotateStoredFurnitureObject(selectedFurnitureObjectId, 90)}
                        disabled={!selectedFurnitureObjectId}
                      >
                        ⟳ 90°
                      </button>
                    {/if}
                    <button
                      type="button"
                      title="레이더맵 표시"
                      data-active={showStoredRadarOverlay ? "true" : "false"}
                      onclick={() => (showStoredRadarOverlay = !showStoredRadarOverlay)}
                    >
                      레이더맵
                    </button>
                  </EditorToolbar>
                {/if}
                {#if snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active}
                  <div class="floorplan-map-tool-panel floorplan-fixed-text">
                    <strong>{activeMapToolTitle()}</strong>
                    <span>{activeMapToolMessage()}</span>
                    <div>
                      <button type="button" onclick={finishActiveMapTool} disabled={!activeMapToolCanFinish()}>완료</button>
                      <button type="button" onclick={cancelActiveMapTool}>취소</button>
                    </div>
                  </div>
                {/if}
                <svg
                  class="floorplan-base-svg"
                  viewBox={`0 0 ${storedFloorplanDocument.image.width} ${storedFloorplanDocument.image.height}`}
                  preserveAspectRatio="none"
                  aria-label="저장된 평면도"
                  role="img"
                >
                  <image
                    href={storedFloorplanImageUrl}
                    x="0"
                    y="0"
                    width={storedFloorplanDocument.image.width}
                    height={storedFloorplanDocument.image.height}
                    preserveAspectRatio="none"
                  />
                </svg>
                <FloorplanCandidateOverlay
                  candidates={roomCandidates.candidates}
                  selectedCandidateId={roomCandidates.selectedCandidateId}
                  selectedCandidateVertexIndex={selectedCandidateVertexIndex}
                  imageWidth={storedFloorplanDocument.image.width}
                  imageHeight={storedFloorplanDocument.image.height}
                  manualDraftActive={manualRoomDraft.active}
                  manualDraftPoints={manualRoomDraft.points}
                  manualDraftHoverPoint={manualRoomDraftHover}
                  splitDraftActive={roomSplitDraft.active}
                  splitDraftPoints={roomSplitDraft.points}
                  splitDraftHoverPoint={roomSplitDraftHover}
                  mergeDraftActive={roomMergeDraft.active}
                  mergeDraftCandidateIds={roomMergeDraft.candidateIds}
                  allowCandidateEditing={storedFloorplanMode === "edit" && storedFloorplanEditTool === "rooms"}
                  snapEdges={getSnapEdges()}
                  selectedSnapEdgeKey={snapEdit.edgeKey}
                  snapWallSegments={getSnapWallSegments()}
                  onSelect={storedFloorplanMode === "edit" ? selectRoomCandidate : undefined}
                  onManualDraftPoint={addManualRoomPoint}
                  onManualDraftPointDelete={deleteManualRoomPoint}
                  onManualDraftHover={previewManualRoomPoint}
                  onManualDraftLeave={clearManualRoomPointPreview}
                  onSplitDraftPoint={addRoomSplitPoint}
                  onSplitDraftHover={previewRoomSplitPoint}
                  onSplitDraftLeave={clearRoomSplitPreview}
                  onSplitDraftPointMove={moveRoomSplitPoint}
                  onSelectSnapEdge={selectSnapEdge}
                  onSnapToWallSegment={snapSelectedEdgeToWall}
                  onCandidateVertexAdd={addCandidateVertex}
                  onCandidateVertexSelect={selectCandidateVertex}
                  onCandidateVertexDelete={deleteCandidateVertex}
                  onCandidateVertexMoveStart={beginCandidateVertexMove}
                  onCandidateVertexMove={moveCandidateVertex}
                  onCandidateVertexMoveEnd={endCandidateVertexMove}
                />
                {#if showStoredRadarOverlay}
                  <FloorplanRadarPlacementOverlay
                    imageWidth={storedFloorplanDocument.image.width}
                    imageHeight={storedFloorplanDocument.image.height}
                    scaleEstimate={storedFloorplanScaleEstimate()}
                    placement={storedFloorplanRadarPlacement()}
                    scalePercent={(storedFloorplanDocument.radar.scale ?? 1) * 100}
                    zones={deviceConfig?.zones ?? []}
                    calibrationZones={deviceConfig?.calibrationZones ?? []}
                    targets={deviceState?.targets ?? []}
                    wallSegments={storedRoomBoundarySegments()}
                    occlusionSegments={storedRadarOcclusionSegments()}
                    ignoredOcclusionSegmentIds={storedFloorplanDocument.occlusion?.ignoredEdges ?? []}
                    editableZoneSource={storedRadarZoneEditSource()}
                    selectedZoneId={selectedStoredRadarZoneId}
                    selectedZonePointIndex={selectedStoredRadarZonePointIndex}
                    readOnly={storedFloorplanMode !== "edit" || storedFloorplanEditTool !== "radar"}
                    onChange={updateStoredFloorplanRadarPlacement}
                    onCommit={updateStoredFloorplanRadarPlacement}
                    onSelectZone={selectStoredRadarZone}
                    onZoneMove={moveStoredRadarZone}
                    onZonePointMove={moveStoredRadarZonePoint}
                    onZoneEdgeClick={addStoredRadarZonePoint}
                    onZoneEditCommit={commitStoredRadarZoneEdit}
                  />
                {/if}
                <FloorplanFurnitureOverlay
                  objects={storedFurnitureObjects()}
                  assets={FLOORPLAN_FURNITURE_ASSETS}
                  selectedObjectId={selectedFurnitureObjectId}
                  imageWidth={storedFloorplanDocument.image.width}
                  imageHeight={storedFloorplanDocument.image.height}
                  bounds={storedFloorplanScaleEstimate()?.outerBounds}
                  editable={storedFloorplanMode === "edit" && storedFloorplanEditTool === "furniture"}
                  onSelect={(id) => (selectedFurnitureObjectId = id)}
                  onMove={updateStoredFurnitureObject}
                />
              </div>
            </div>
          {:else}
            <div class="floorplan-stored-preview floorplan-stored-empty">
              <strong>평면도 데이터를 준비하는 중입니다</strong>
              <span>이미지와 설정을 불러온 뒤 여기에 표시합니다.</span>
            </div>
          {/if}
        </div>
      </section>
    {/if}
  </section>
{:else}
<section
  class={`floorplan-panel ${floorplanWizardStep === "rooms" || floorplanWizardStep === "ocr" ? "rooms-step" : "image-step"} ${floorplanWizardAnimating ? "is-transitioning" : ""} transition-${floorplanWizardDirection}`}
  role="presentation"
  onclick={handleFloorplanPanelClick}
  onkeydown={handleFloorplanPanelKeydown}
>
  <aside class="floorplan-controls">
    <div class="floorplan-workflow-card">
    <div>
      <strong>{workflowTitle()}</strong>
      <span>{workflowDescription()}</span>
    </div>

    {#if imageUrl}
      <div class="floorplan-file-info">
        <strong>{imageName}</strong>
        <span>{imageMeta}</span>
      </div>
      <button type="button" class="floorplan-clear-button" onclick={clearImage}>이미지 지우기</button>
    {/if}

    {#if !imageUrl}
    <label class="floorplan-upload-button">
      <input type="file" accept="image/png,image/jpeg,image/webp" onchange={handleFileChange} disabled={busy} />
      {busy ? "변환 중" : "이미지 선택"}
    </label>
    {/if}

    {#if floorplanWizardStep === "image"}
      <div class="floorplan-status" data-tone={statusTone}>
        <strong>{statusTone === "ok" ? "통과" : statusTone === "error" ? "확인 필요" : "대기"}</strong>
        <span>{statusText}</span>
      </div>
    {/if}

    {#if floorplanWizardStep === "ocr"}
      <div class="floorplan-total-size-card">
        <strong>전체 평면도 크기</strong>
        <span>하늘색 최외곽 박스를 기준으로 전체 가로/세로 길이를 입력하세요.</span>
        <div class="floorplan-total-size-fields">
          <label>
            <span>전체 가로</span>
            <input
              inputmode="decimal"
              value={floorplanTotalSize.width}
              placeholder="mm"
              oninput={(event) => updateFloorplanTotalSize("width", event.currentTarget.value)}
            />
          </label>
          <label>
            <span>전체 세로</span>
            <input
              inputmode="decimal"
              value={floorplanTotalSize.height}
              placeholder="mm"
              oninput={(event) => updateFloorplanTotalSize("height", event.currentTarget.value)}
            />
          </label>
        </div>
        <button type="button" onclick={togglePreciseRoomSizeEditing} disabled={!canCalculateRoomSizes()}>
          {preciseRoomSizeEditing ? "정밀 보정 닫기" : "정밀 보정"}
        </button>
        <em data-tone={roomSizeError ? "error" : roomSizeCalculated ? "ok" : "idle"}>{roomSizeCalculationMessage()}</em>
      </div>
    {/if}

    {#if !imageUrl}
      <div class="floorplan-empty-note">
        <strong>제한사항</strong>
        <span>최소 긴 변 800px, 짧은 변 500px 이상이며, 최종 WebP 후보는 80KB 이하여야 합니다.</span>
      </div>
    {/if}
    </div>

    {#if floorplanWizardStep === "image"}
      <div class="floorplan-step-actions">
        <button type="button" class="floorplan-step-button floorplan-fixed-text" disabled>
          <span>이전 단계</span>
          <strong>지금이 첫 단계입니다</strong>
        </button>
        <button
          type="button"
          class="floorplan-next-button floorplan-fixed-text"
          onclick={startCandidateAnalysisStep}
          disabled={!imageUrl || statusTone !== "ok" || analysisBusy}
        >
          <span>{analysisBusy ? "분석 중" : "다음 단계"}</span>
          <strong>방 자동 인식</strong>
        </button>
      </div>
    {:else if floorplanWizardStep === "rooms"}
      <RoomEditTools
        candidates={roomCandidates.candidates}
        selectedCandidateId={roomCandidates.selectedCandidateId}
        manualRoomDraft={manualRoomDraft}
        roomSplitDraft={roomSplitDraft}
        roomMergeDraft={roomMergeDraft}
        snapEdit={snapEdit}
        onStartSplitDraft={startRoomSplitDraft}
        onStartMergeDraft={startRoomMergeDraft}
        onStartSnapEdit={startSnapEdit}
      />
      <div class="floorplan-step-actions">
        <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={goToImageStep}>
          <span>이전 단계</span>
          <strong>이미지 준비</strong>
        </button>
        <button
          type="button"
          class="floorplan-next-button floorplan-fixed-text"
          onclick={goToOcrStep}
          title="방 이름 인식 단계로 이동합니다."
        >
          <span>다음 단계</span>
          <strong>방 이름 인식</strong>
        </button>
      </div>
    {:else if floorplanWizardStep === "ocr"}
      <div class="floorplan-step-actions">
        <button type="button" class="floorplan-step-button floorplan-fixed-text" onclick={goToRoomsStep}>
          <span>이전 단계</span>
          <strong>방 후보 정리</strong>
        </button>
        <button
          type="button"
          class="floorplan-next-button floorplan-fixed-text"
          onclick={goToRadarStep}
          title={canGoToRadarStep() ? "레이더 배치 단계로 이동합니다." : "전체 평면도 가로/세로 길이를 모두 입력하세요."}
          disabled={!canGoToRadarStep()}
        >
          <span>다음 단계</span>
          <strong>레이더 배치</strong>
        </button>
      </div>
    {:else if floorplanWizardStep === "radar"}
      <RadarStep
        scaleSummary={floorplanScaleSummary()}
        radarScalePercent={radarScalePercent}
        minScalePercent={RADAR_SCALE_MIN_PERCENT}
        maxScalePercent={RADAR_SCALE_MAX_PERCENT}
        scaleStepPercent={RADAR_SCALE_STEP_PERCENT}
        occlusionEditActive={radarOcclusionEditActive}
        ignoredOcclusionEdges={radarOcclusionIgnoredEdges}
        onScaleInput={updateRadarScalePercent}
        onScaleNudge={nudgeRadarScalePercent}
        onToggleOcclusionEdit={toggleRadarOcclusionEdit}
        onBack={goBackToOcrStep}
        onNext={goToFinalStep}
      />
    {:else if floorplanWizardStep === "final"}
      {@const storageDraft = floorplanStorageDraft()}
      <FinalStep
        storageDraft={storageDraft}
        scaleSummary={floorplanScaleSummary()}
        placement={radarPlacement ?? defaultRadarPlacement()}
        imageName={imageName}
        roomCount={visibleRoomCandidates().length}
        ignoredOcclusionEdges={radarOcclusionIgnoredEdges}
        canSave={Boolean(storageDraft && imageBlob)}
        saveBusy={floorplanSaveBusy}
        saveStatus={floorplanSaveStatus}
        saveTone={floorplanSaveTone}
        onBack={goBackToRadarStep}
        onSave={saveFloorplanToDevice}
      />
    {/if}
  </aside>

  {#if floorplanWizardStep === "rooms" || floorplanWizardStep === "ocr"}
    <aside class="floorplan-middle-column">
      {#if floorplanWizardStep === "rooms"}
        <div class="floorplan-manual-card floorplan-fixed-text">
          <strong>수동 방 추가</strong>
          <button
            type="button"
            onclick={startManualRoomDraft}
            disabled={snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active}
          >
            방 추가
          </button>
        </div>
        <RoomEditStep
          candidates={roomCandidates.candidates}
          selectedCandidateId={roomCandidates.selectedCandidateId}
          selectedCandidateVertexIndex={selectedCandidateVertexIndex}
          roomSplitDraft={roomSplitDraft}
          roomMergeDraft={roomMergeDraft}
          snapEdit={snapEdit}
          onSelect={selectRoomCandidate}
          onRename={roomCandidates.rename}
          onConfirm={(id) => setCandidateStatusWithHistory(id, "confirmed")}
          onReject={(id) => setCandidateStatusWithHistory(id, "rejected")}
          onRemove={removeCandidateWithHistory}
          onStartSnapEdit={startSnapEdit}
          onFinishSnapEdit={finishSnapEdit}
          onCancelSnapEdit={cancelSnapEdit}
          onStartSplitDraft={startRoomSplitDraft}
          onFinishSplitDraft={finishRoomSplitDraft}
          onCancelSplitDraft={cancelRoomSplitDraft}
          canFinishSplitDraft={canFinishRoomSplitDraft}
          onStartMergeDraft={startRoomMergeDraft}
          onFinishMergeDraft={finishRoomMergeDraft}
          onCancelMergeDraft={cancelRoomMergeDraft}
          canFinishMergeDraft={canFinishRoomMergeDraft}
        />
      {:else}
        <FloorplanCandidateList
          mode="ocr"
          candidates={roomCandidates.candidates}
          selectedCandidateId={roomCandidates.selectedCandidateId}
          measurements={roomMeasurements}
          estimatedSizes={roomSizeEstimates}
          showEstimatedSizes={preciseRoomSizeEditing}
          onSelect={selectRoomCandidate}
          onRename={roomCandidates.rename}
          onSizeChange={updateRoomMeasurement}
          onConfirm={(id) => setCandidateStatusWithHistory(id, "confirmed")}
          onReject={(id) => setCandidateStatusWithHistory(id, "rejected")}
          onRemove={removeCandidateWithHistory}
        />
      {/if}
    </aside>
  {/if}
  <section class="floorplan-workspace-column" aria-label="평면도 작업 영역">
  <div class="floorplan-stage">
    {#if imageUrl}
      <div
        class={`floorplan-image-layer ${floorplanView.panning ? "panning" : ""}`}
        style={floorplanImageLayerStyle()}
        role="presentation"
        onwheel={handleFloorplanWheel}
        onpointerdown={handleFloorplanPointerDown}
        onpointermove={handleFloorplanPointerMove}
        onpointerup={stopFloorplanPan}
        onpointercancel={stopFloorplanPan}
        onauxclick={preventFloorplanAuxClick}
      >
        {#if floorplanWizardStep === "rooms" || floorplanWizardStep === "final"}
          <EditorToolbar ariaLabel="평면도 편집 도구">
            {#if floorplanWizardStep === "rooms"}
              <button type="button" title="되돌리기 (Ctrl+Z)" onclick={undoCandidateEdit} disabled={!candidateUndoStack.length}>↶</button>
              <button type="button" title="다시하기 (Ctrl+Y)" onclick={redoCandidateEdit} disabled={!candidateRedoStack.length}>↷</button>
            {:else}
              <button
                type="button"
                title="스케일링된 레이더맵 표시"
                data-active={showFinalRadarOverlay ? "true" : "false"}
                onclick={() => (showFinalRadarOverlay = !showFinalRadarOverlay)}
              >
                레이더맵
              </button>
            {/if}
          </EditorToolbar>
        {/if}
        {#if snapEdit.active || roomSplitDraft.active || roomMergeDraft.active || manualRoomDraft.active}
          <div class="floorplan-map-tool-panel floorplan-fixed-text">
            <strong>{activeMapToolTitle()}</strong>
            <span>{activeMapToolMessage()}</span>
            <div>
              <button type="button" onclick={finishActiveMapTool} disabled={!activeMapToolCanFinish()}>완료</button>
              <button type="button" onclick={cancelActiveMapTool}>취소</button>
            </div>
          </div>
        {/if}
        <svg
          class="floorplan-base-svg"
          viewBox={`0 0 ${debugInfo?.width ?? 1} ${debugInfo?.height ?? 1}`}
          preserveAspectRatio="none"
          style={floorplanTransformStyle()}
          aria-label="업로드한 평면도 WebP 후보"
          role="img"
        >
          <image
            href={imageUrl}
            x="0"
            y="0"
            width={debugInfo?.width ?? 1}
            height={debugInfo?.height ?? 1}
            preserveAspectRatio="none"
          />
        </svg>
        <FloorplanCandidateOverlay
          transformStyle={floorplanTransformStyle()}
          candidates={roomCandidates.candidates}
          selectedCandidateId={roomCandidates.selectedCandidateId}
          selectionLocked={snapEdit.active}
          imageWidth={debugInfo?.width ?? 1}
          imageHeight={debugInfo?.height ?? 1}
          manualDraftActive={manualRoomDraft.active}
          manualDraftPoints={manualRoomDraft.points}
          manualDraftHoverPoint={manualRoomDraftHover}
          splitDraftActive={roomSplitDraft.active}
          splitDraftPoints={roomSplitDraft.points}
          splitDraftHoverPoint={roomSplitDraftHover}
          mergeDraftActive={roomMergeDraft.active}
          mergeDraftCandidateIds={roomMergeDraft.candidateIds}
          allowCandidateEditing={floorplanWizardStep === "rooms"}
          ocrItems={ocrOverlayItems()}
          showOcrItems={showOcrOverlay && floorplanWizardStep === "ocr"}
          showRoomSizeBounds={false}
          wallMaskCells={analysisDebug?.wallMaskCells ?? []}
          showWallMaskCells={showWallMaskOverlay && floorplanWizardStep === "rooms"}
          showCandidateDebug={showRoomCandidateDebug && floorplanWizardStep === "rooms"}
          snapEdges={getSnapEdges()}
          selectedSnapEdgeKey={snapEdit.edgeKey}
          snapWallSegments={getSnapWallSegments()}
          onSelect={selectRoomCandidate}
          onManualDraftPoint={addManualRoomPoint}
          onManualDraftPointDelete={deleteManualRoomPoint}
          onManualDraftHover={previewManualRoomPoint}
          onManualDraftLeave={clearManualRoomPointPreview}
          onSplitDraftPoint={addRoomSplitPoint}
          onSplitDraftHover={previewRoomSplitPoint}
          onSplitDraftLeave={clearRoomSplitPreview}
          onSplitDraftPointMove={moveRoomSplitPoint}
          onSelectSnapEdge={selectSnapEdge}
          onSnapToWallSegment={snapSelectedEdgeToWall}
          onCandidateVertexAdd={addCandidateVertex}
          onCandidateVertexSelect={selectCandidateVertex}
          onCandidateVertexDelete={deleteCandidateVertex}
          onCandidateVertexMoveStart={beginCandidateVertexMove}
          onCandidateVertexMove={moveCandidateVertex}
          onCandidateVertexMoveEnd={endCandidateVertexMove}
        />
        {#if floorplanWizardStep === "radar" || (floorplanWizardStep === "final" && showFinalRadarOverlay)}
          <FloorplanRadarPlacementOverlay
            transformStyle={floorplanTransformStyle()}
            imageWidth={debugInfo?.width ?? 1}
            imageHeight={debugInfo?.height ?? 1}
            scaleEstimate={floorplanScaleEstimate()}
            placement={radarPlacement ?? defaultRadarPlacement()}
            scalePercent={radarScalePercent}
            zones={deviceConfig?.zones ?? []}
            calibrationZones={deviceConfig?.calibrationZones ?? []}
            targets={deviceState?.targets ?? []}
            wallSegments={roomBoundaryWallSegments()}
            occlusionSegments={radarOcclusionSegments()}
            ignoredOcclusionSegmentIds={radarOcclusionIgnoredEdges}
            occlusionEditActive={radarOcclusionEditActive}
            readOnly={floorplanWizardStep === "final"}
            onChange={updateRadarPlacement}
            onCommit={commitRadarPlacement}
            onToggleOcclusionSegment={toggleRadarOcclusionEdge}
          />
        {/if}
        <div class="floorplan-zoom-controls" aria-label="Floorplan zoom controls">
          <button type="button" title="확대" onclick={() => zoomFloorplan(1)}>+</button>
          <button type="button" title="축소" onclick={() => zoomFloorplan(-1)} disabled={floorplanView.scale <= 1}>-</button>
          <button type="button" title="확대 초기화" onclick={resetFloorplanZoom}>{Math.round(floorplanView.scale * 100)}%</button>
        </div>
      </div>
    {:else}
      <div class="floorplan-preview">
        <div class="floorplan-room living">거실</div>
        <div class="floorplan-room room-a">방</div>
        <div class="floorplan-room room-b">방</div>
        <div class="floorplan-radar-dot"></div>
      </div>
    {/if}
  </div>

  <div class="floorplan-debug-row">
  {#if floorplanWizardStep === "ocr"}
    <details class="floorplan-debug floorplan-ocr-debug floorplan-fixed-text">
      <summary>OCR 디버그</summary>
      <label class="floorplan-switch-row">
        <input type="checkbox" bind:checked={showOcrOverlay} disabled={!ocrResult} />
        <span>OCR 영역 표시</span>
      </label>
      {#if ocrSteps.length}
        <ul>
          {#each ocrSteps as step}
            <li>{step}</li>
          {/each}
        </ul>
      {:else}
        <span>아직 OCR을 실행하지 않았습니다.</span>
      {/if}
      {#if ocrError}
        <div class="floorplan-ocr-error">{ocrError}</div>
      {/if}
      {#if ocrResult}
        <dl>
          <div><dt>인식 단어</dt><dd>{ocrResult.words.length}개</dd></div>
          <div><dt>인식 줄</dt><dd>{ocrResult.lines.length}개</dd></div>
          <div><dt>인식 블록</dt><dd>{ocrResult.rawBlockCount}개</dd></div>
          <div><dt>원시 조각</dt><dd>{ocrResult.rawTextBoxes}개</dd></div>
          <div><dt>병합 조각</dt><dd>{ocrResult.mergedTextBoxes}개</dd></div>
          <div><dt>벽 차단 병합</dt><dd>{ocrResult.wallBlockedMerges}회</dd></div>
          <div><dt>텍스트 후보</dt><dd>{ocrResult.textCandidateBoxes}개</dd></div>
          <div><dt>OCR crop</dt><dd>{ocrResult.ocrRegions}개</dd></div>
          <div><dt>제거한 가이드선</dt><dd>{ocrResult.removedGuideLines}개</dd></div>
          <div><dt>회전 OCR</dt><dd>{ocrResult.rotatedRegions}개</dd></div>
          <div><dt>빈 crop</dt><dd>{ocrResult.emptyRegions}개</dd></div>
          <div><dt>방 이름 후보</dt><dd>{ocrKindCounts()["room-label"]}개</dd></div>
          <div><dt>잡음 후보</dt><dd>{ocrKindCounts().noise}개</dd></div>
          <div><dt>전체 텍스트</dt><dd>{ocrResult.text ? `${ocrResult.text.length}자` : "없음"}</dd></div>
        </dl>
        {#if ocrResult.text}
          <strong class="floorplan-debug-section-title">전체 텍스트</strong>
          <p class="floorplan-ocr-text-preview">{ocrResult.text}</p>
        {/if}
        {#if ocrPreviewWords().length}
          <strong class="floorplan-debug-section-title">미리보기</strong>
          <ul>
            {#each ocrPreviewWords() as word}
              <li>{word.text} · 신뢰도 {word.confidence}%</li>
            {/each}
          </ul>
        {/if}
      {/if}
    </details>
  {:else if floorplanWizardStep === "rooms" && analysisSteps.length}
    <details class="floorplan-debug floorplan-analysis-log floorplan-fixed-text" open={analysisBusy}>
      <summary>분석 진행 로그</summary>
      <ul>
        {#each analysisSteps as step}
          <li>{step}</li>
        {/each}
      </ul>
    </details>
  {/if}

  {#if floorplanWizardStep === "rooms" && analysisDebug}
    <details class="floorplan-debug floorplan-fixed-text">
        <summary>방 후보 선정 디버그</summary>
        <label class="floorplan-switch-row">
          <input type="checkbox" bind:checked={showRoomCandidateDebug} />
          <span>평면도 디버그 표시</span>
        </label>
        <label class="floorplan-switch-row">
          <input
            type="checkbox"
            bind:checked={showWallMaskOverlay}
            disabled={!analysisDebug.wallMaskCells?.length}
          />
          <span>벽 인식 영역 표시</span>
        </label>
        <dl>
          <div><dt>분석 방식</dt><dd>{analysisEngine || analysisDebug.engine || "jsfeat-lite-worker"}</dd></div>
          <div><dt>정리 전 후보</dt><dd>{analysisDebug.acceptedBeforeSanitize}</dd></div>
          <div><dt>최종 후보</dt><dd>{analysisDebug.acceptedAfterSanitize}</dd></div>
          <div><dt>후보 필터</dt><dd>{analysisDebug.filtersEnabled === false ? "해제" : "적용"}</dd></div>
        </dl>
        {#if analysisDebug.edge}
          <strong class="floorplan-debug-section-title">edge 분석 상세</strong>
          <dl>
            <div><dt>셀 크기</dt><dd>{analysisDebug.edge.cellSize}px</dd></div>
            <div><dt>분석 격자</dt><dd>{analysisDebug.edge.gridWidth} x {analysisDebug.edge.gridHeight}</dd></div>
            <div><dt>전체 셀</dt><dd>{analysisDebug.edge.totalCells}</dd></div>
            <div><dt>edge 픽셀</dt><dd>{analysisDebug.edge.edgePixels}</dd></div>
            <div><dt>벽 후보 셀</dt><dd>{analysisDebug.edge.wallCells}</dd></div>
            <div><dt>확장 후 벽 셀</dt><dd>{analysisDebug.edge.expandedWallCells}</dd></div>
            <div><dt>자유공간 영역</dt><dd>{analysisDebug.edge.freeComponents}</dd></div>
            <div><dt>Canny 임계값</dt><dd>{analysisDebug.edge.lowThreshold} / {analysisDebug.edge.highThreshold}</dd></div>
          </dl>
        {/if}
        {#if analysisDebug.color}
          <strong class="floorplan-debug-section-title">색상/채움 분석 상세</strong>
          <dl>
            <div><dt>셀 크기</dt><dd>{analysisDebug.color.cellSize}px</dd></div>
            <div><dt>분석 격자</dt><dd>{analysisDebug.color.gridWidth} x {analysisDebug.color.gridHeight}</dd></div>
            <div><dt>전체 셀</dt><dd>{analysisDebug.color.totalCells}</dd></div>
            <div><dt>색상 픽셀</dt><dd>{analysisDebug.color.coloredPixels}</dd></div>
            <div><dt>색상 후보 셀</dt><dd>{analysisDebug.color.coloredCells}</dd></div>
            <div><dt>연결 영역</dt><dd>{analysisDebug.color.components}</dd></div>
          </dl>
        {/if}
        {#if analysisDebug.wall}
          <strong class="floorplan-debug-section-title">벽선 분석 상세</strong>
          <dl>
            <div><dt>셀 크기</dt><dd>{analysisDebug.wall.cellSize}px</dd></div>
            <div><dt>분석 격자</dt><dd>{analysisDebug.wall.gridWidth} x {analysisDebug.wall.gridHeight}</dd></div>
            <div><dt>전체 셀</dt><dd>{analysisDebug.wall.totalCells}</dd></div>
            <div><dt>어두운 픽셀</dt><dd>{analysisDebug.wall.darkPixels}</dd></div>
            <div><dt>벽 후보 셀</dt><dd>{analysisDebug.wall.wallCells}</dd></div>
            <div><dt>확장 후 벽 셀</dt><dd>{analysisDebug.wall.expandedWallCells}</dd></div>
            <div><dt>자유공간 영역</dt><dd>{analysisDebug.wall.freeComponents}</dd></div>
          </dl>
        {/if}
        {#if analysisDebug.wallMaskCells?.length}
          <strong class="floorplan-debug-section-title">벽 선정 디버그</strong>
          <dl>
            <div><dt>벽 셀 좌표</dt><dd>{analysisDebug.wallMaskCells.length}개</dd></div>
            <div><dt>제외된 후보</dt><dd>{analysisDebug.wallRejectedCells?.length ?? 0}개</dd></div>
            {#each Object.entries(wallMaskReasonCounts()) as [reason, count]}
              <div><dt>{reason}</dt><dd>{count}개</dd></div>
            {/each}
          </dl>
          <button type="button" class="floorplan-debug-download" onclick={downloadWallSelectionDebug}>
            벽 선정 디버그 txt 저장
          </button>
          <p class="floorplan-debug-note">선정된 좌표와 제외된 좌표는 txt 파일에서 확인합니다.</p>
        {/if}
        {#if analysisDebug.wallSnap}
          <strong class="floorplan-debug-section-title">벽 붙이기 디버그</strong>
          <dl>
            <div><dt>검사한 면</dt><dd>{analysisDebug.wallSnap.edgeChecks}</dd></div>
            <div><dt>축으로 인정 안 됨</dt><dd>{analysisDebug.wallSnap.noAxisEdges}</dd></div>
            <div><dt>후보 조회</dt><dd>{analysisDebug.wallSnap.candidateQueries}</dd></div>
            <div><dt>빨간 후보선 없음</dt><dd>{analysisDebug.wallSnap.candidateNone}</dd></div>
            <div><dt>이미 붙었다고 판단</dt><dd>{analysisDebug.wallSnap.alreadyAligned}</dd></div>
            <div><dt>이동 시도</dt><dd>{analysisDebug.wallSnap.moveAttempts}</dd></div>
            <div><dt>벽 붙이기 적용</dt><dd>{analysisDebug.wallSnap.applied}</dd></div>
            <div><dt>self-intersection 거부</dt><dd>{analysisDebug.wallSnap.rejectedSelfIntersection}</dd></div>
            <div><dt>과확장 거부</dt><dd>{analysisDebug.wallSnap.rejectedOverExpanded}</dd></div>
            <div><dt>코너 보강 시도</dt><dd>{analysisDebug.wallSnap.cornerAttempts}</dd></div>
            <div><dt>코너 보강 적용</dt><dd>{analysisDebug.wallSnap.cornerApplied}</dd></div>
            <div><dt>교차점 없음</dt><dd>{analysisDebug.wallSnap.cornerNoIntersection}</dd></div>
            <div><dt>기존 꼭짓점 근처</dt><dd>{analysisDebug.wallSnap.cornerExistingPoint}</dd></div>
            <div><dt>뾰족한 코너 거부</dt><dd>{analysisDebug.wallSnap.cornerSpike}</dd></div>
            <div><dt>코너 self-intersection</dt><dd>{analysisDebug.wallSnap.cornerSelfIntersection}</dd></div>
          </dl>
          {#if analysisDebug.wallSnap.logs?.length}
            <strong class="floorplan-debug-section-title">벽 붙이기 로그</strong>
            <ul>
              {#each analysisDebug.wallSnap.logs as log}
                <li>{log}</li>
              {/each}
            </ul>
          {/if}
        {/if}
        {#if !analysisDebug.edge && !analysisDebug.color}
          <dl>
            {#if analysisDebug.cellSize}
              <div><dt>셀 크기</dt><dd>{analysisDebug.cellSize}px</dd></div>
            {/if}
            {#if analysisDebug.gridWidth}
              <div><dt>분석 격자</dt><dd>{analysisDebug.gridWidth} x {analysisDebug.gridHeight}</dd></div>
              <div><dt>전체 셀</dt><dd>{analysisDebug.totalCells}</dd></div>
              {#if analysisDebug.seedCells !== undefined}
                <div><dt>초기 후보 셀</dt><dd>{analysisDebug.seedCells}</dd></div>
              {/if}
              {#if analysisDebug.expandedCells !== undefined}
                <div><dt>확장 후 셀</dt><dd>{analysisDebug.expandedCells}</dd></div>
              {/if}
              {#if analysisDebug.components !== undefined}
                <div><dt>연결 영역</dt><dd>{analysisDebug.components}</dd></div>
              {/if}
            {/if}
            {#if analysisDebug.edgePixels !== undefined}
              <div><dt>edge 픽셀</dt><dd>{analysisDebug.edgePixels}</dd></div>
              <div><dt>벽 후보 셀</dt><dd>{analysisDebug.wallCells}</dd></div>
              <div><dt>확장 후 벽 셀</dt><dd>{analysisDebug.expandedWallCells}</dd></div>
              <div><dt>자유공간 영역</dt><dd>{analysisDebug.freeComponents}</dd></div>
              <div><dt>Canny 임계값</dt><dd>{analysisDebug.lowThreshold} / {analysisDebug.highThreshold}</dd></div>
            {/if}
            {#if analysisDebug.coloredPixels !== undefined}
              <div><dt>색상 픽셀</dt><dd>{analysisDebug.coloredPixels}</dd></div>
              <div><dt>색상 후보 셀</dt><dd>{analysisDebug.coloredCells}</dd></div>
            {/if}
            {#if analysisDebug.darkPixels !== undefined}
              <div><dt>어두운 픽셀</dt><dd>{analysisDebug.darkPixels}</dd></div>
              <div><dt>벽 후보 셀</dt><dd>{analysisDebug.wallCells}</dd></div>
              <div><dt>확장 후 벽 셀</dt><dd>{analysisDebug.expandedWallCells}</dd></div>
              <div><dt>자유공간 영역</dt><dd>{analysisDebug.freeComponents}</dd></div>
            {/if}
          </dl>
        {/if}
        <ul>
          <li>너무 작은 덩어리 제외: {analysisDebug.rejectedSmall}</li>
          <li>면적이 너무 작아 제외: {analysisDebug.rejectedTooSmallArea}</li>
          <li>면적이 너무 커 제외: {analysisDebug.rejectedTooLargeArea}</li>
          <li>자유공간이 듬성듬성해 제외: {analysisDebug.rejectedSparse}</li>
          <li>지나치게 길쭉해 제외: {analysisDebug.rejectedThin}</li>
        </ul>
      </details>
    {/if}
  </div>
  </section>
</section>
{/if}

{#if storedFloorplanDeleteOpen}
  <div class="floorplan-delete-dialog-backdrop" role="presentation" onclick={cancelStoredFloorplanDelete}>
    <div
      class="floorplan-delete-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="floorplan-delete-title"
      tabindex="-1"
      onclick={(event) => event.stopPropagation()}
      onkeydown={(event) => event.stopPropagation()}
    >
      <strong id="floorplan-delete-title">저장된 평면도를 삭제합니다</strong>
      <p>모든 평면도 데이터가 삭제되며 복구할 수 없습니다. 진행하시겠습니까?</p>
      {#if storedFloorplanDeleteError}
        <span class="floorplan-delete-error">{storedFloorplanDeleteError}</span>
      {/if}
      <div class="floorplan-delete-dialog-actions">
        <button type="button" onclick={cancelStoredFloorplanDelete} disabled={storedFloorplanDeleteBusy}>아니오</button>
        <button type="button" class="danger-button" onclick={confirmStoredFloorplanDelete} disabled={storedFloorplanDeleteBusy}>
          {storedFloorplanDeleteBusy ? "삭제 중" : "네, 삭제합니다"}
        </button>
      </div>
    </div>
  </div>
{/if}

<FloorplanOcrDialog
  open={ocrDialogOpen}
  busy={ocrBusy}
  progress={ocrProgress}
  statusText={ocrStatusText}
  logs={ocrSteps}
  resultSummary={ocrResultSummary()}
  errorText={ocrError}
  onCancel={cancelFloorplanOcr}
  onClose={closeFloorplanOcrDialog}
/>
