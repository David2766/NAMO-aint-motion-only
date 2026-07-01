import { detectFloorplanTextBoxes, type FloorplanTextRegion } from "../../core/floorplan/text-box-detector";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import { normalizeExactRoomLabel, normalizeLabelComparableText } from "./room-labels";

const TESSERACT_SCRIPT_URL = "https://cdn.jsdelivr.net/npm/tesseract.js@6/dist/tesseract.min.js";
const TESSERACT_WORKER_PATH = "https://cdn.jsdelivr.net/npm/tesseract.js@6/dist/worker.min.js";
const TESSERACT_CORE_PATH = "https://cdn.jsdelivr.net/npm/tesseract.js-core@6";
const TESSERACT_LANG_PATH = "https://tessdata.projectnaptha.com/4.0.0";
const MAX_OCR_REGIONS = 48;
const OCR_CROP_SCALE = 3;
const OCR_PROGRESS_ENGINE_READY = 0.08;
const OCR_PROGRESS_TEXT_DETECTED = 0.14;
const OCR_PROGRESS_WORKER_READY = 0.2;
const OCR_PROGRESS_RECOGNIZE_DONE = 0.92;

let tesseractLoadPromise: Promise<any> | null = null;

export class FloorplanOcrCancelledError extends Error {
  constructor() {
    super("OCR 작업이 취소되었습니다.");
    this.name = "FloorplanOcrCancelledError";
  }
}

export type FloorplanOcrProgress = {
  status: string;
  progress: number;
  message: string;
};

export type FloorplanOcrWord = {
  text: string;
  confidence: number;
  kind?: "room-label" | "dimension" | "noise";
  dimensionSide?: "left" | "right";
  dimensionScaleSide?: "top" | "bottom" | "left" | "right";
  dimensionLane?: "outer" | "inner";
  dimensionDividerX?: number;
  bbox: {
    x0: number;
    y0: number;
    x1: number;
    y1: number;
  } | null;
  regionIndex?: number;
  rotation?: number;
};

export type FloorplanVerticalDividerDebug = {
  side: "top" | "bottom" | "left" | "right";
  x: number;
  y: number;
  width: number;
  height: number;
  dividerX: number | null;
  candidateColumns: number;
  bestScore: number;
  bestRun: number;
  bestCoverage: number;
  bestSlices: number;
  bestBandCoverage: number;
  reason: string;
};

export type FloorplanContentBounds = {
  x: number;
  y: number;
  maxX: number;
  maxY: number;
};

export type FloorplanDimensionBoundsDebug = FloorplanContentBounds & {
  source: "manual" | "room-candidates" | "wall-segments" | "fallback";
  x: number;
  y: number;
  maxX: number;
  maxY: number;
  padding: number;
  reason: string;
};

export type FloorplanOcrResult = {
  text: string;
  words: FloorplanOcrWord[];
  lines: FloorplanOcrWord[];
  blocks: FloorplanOcrWord[];
  rawBlockCount: number;
  rawTextBoxes: number;
  mergedTextBoxes: number;
  wallBlockedMerges: number;
  textCandidateBoxes: number;
  textRegions: number;
  dimensionRegions: number;
  dimensionGroupSplits: number;
  verticalStripRegions: number;
  verticalDividerDebug: FloorplanVerticalDividerDebug[];
  dimensionBoundsDebug: FloorplanDimensionBoundsDebug | null;
  removedGuideLines: number;
  ocrRegions: number;
  emptyRegions: number;
  rotatedRegions: number;
};

export type FloorplanOcrJob = {
  promise: Promise<FloorplanOcrResult>;
  cancel: () => Promise<void>;
};

export type FloorplanOcrOptions = {
  roomCandidates?: RoomCandidate[];
};

type TesseractWindow = Window & {
  Tesseract?: any;
};

export function createFloorplanOcrJob(
  image: Blob,
  onProgress: (progress: FloorplanOcrProgress) => void,
  options: FloorplanOcrOptions = {}
): FloorplanOcrJob {
  let cancelled = false;
  let worker: any = null;
  let lastProgress = 0;
  let activeOcrUnit = -1;
  let totalOcrUnits = 1;

  const emitProgress = (progress: FloorplanOcrProgress) => {
    const nextProgress = Math.max(lastProgress, Math.min(1, Math.max(0, progress.progress)));
    lastProgress = nextProgress;
    onProgress({
      ...progress,
      progress: nextProgress
    });
  };

  const cancel = async () => {
    cancelled = true;
    if (worker?.terminate) {
      try {
        await worker.terminate();
      } catch {
        // Termination during WASM work can reject; the caller receives a cancelled result.
      }
    }
    worker = null;
  };

  const promise = (async () => {
    emitProgress({
      status: "engine",
      progress: 0.02,
      message: "OCR 엔진을 불러오는 중입니다."
    });
    const Tesseract = await loadTesseract();
    if (cancelled) throw new FloorplanOcrCancelledError();

    emitProgress({
      status: "text-detect",
      progress: OCR_PROGRESS_ENGINE_READY,
      message: "텍스트 후보 영역을 찾는 중입니다."
    });
    const imageData = await readImageData(image);
    const roomCandidates = options.roomCandidates ?? [];
    const textDetection = detectFloorplanTextBoxes(imageData, {
      relaxedAreas: roomCandidates
        .filter((candidate) => candidate.status !== "rejected")
        .map((candidate) => candidate.rect)
    });
    const regions = textDetection.regions.slice(0, MAX_OCR_REGIONS);
    if (!regions.length) {
      emitProgress({
        status: "done",
        progress: 1,
        message: "텍스트 후보 영역을 찾지 못했습니다."
      });
      return emptyOcrResult(
        textDetection.debug.rawBoxes,
        textDetection.debug.mergedBoxes,
        textDetection.debug.candidateBoxes,
        textDetection.debug.regions,
        0,
        0,
        0,
        [],
        null
      );
    }

    totalOcrUnits = regions.reduce((total, region) => total + getOcrRotations(region).length, 0) || 1;

    emitProgress({
      status: "worker",
      progress: OCR_PROGRESS_TEXT_DETECTED,
      message: `OCR Worker를 준비하는 중입니다. OCR 영역 ${regions.length}개를 인식합니다.`
    });
    worker = await Tesseract.createWorker("kor+eng", 1, {
      workerPath: TESSERACT_WORKER_PATH,
      corePath: TESSERACT_CORE_PATH,
      langPath: TESSERACT_LANG_PATH,
      logger: (message: any) => {
        emitProgress(normalizeProgressMessage(message, activeOcrUnit, totalOcrUnits));
      }
    });
    if (cancelled) throw new FloorplanOcrCancelledError();

    await worker.setParameters?.({
      preserve_interword_spaces: "1",
      user_defined_dpi: "300"
    });

    const cropResults: FloorplanOcrResult[] = [];
    let emptyRegions = 0;
    let rotatedRegions = 0;
    let removedGuideLines = 0;
    let completedOcrUnits = 0;
    for (let index = 0; index < regions.length; index += 1) {
      if (cancelled) throw new FloorplanOcrCancelledError();
      const region = regions[index];
      const rotations = getOcrRotations(region);
      activeOcrUnit = completedOcrUnits;
      emitProgress({
        status: "recognize",
        progress: ocrUnitProgress(completedOcrUnits, totalOcrUnits, 0),
        message: `텍스트 후보 ${index + 1}/${regions.length} 영역을 인식하는 중입니다.`
      });
      const normalized = await recognizeRegion(
        worker,
        imageData,
        region,
        index + 1,
        (rotationIndex) => {
          activeOcrUnit = completedOcrUnits + rotationIndex;
          emitProgress({
            status: "recognize",
            progress: ocrUnitProgress(activeOcrUnit, totalOcrUnits, 0),
            message: rotations.length > 1
              ? `텍스트 후보 ${index + 1}/${regions.length} 영역을 인식하는 중입니다. (${rotationIndex + 1}/${rotations.length})`
              : `텍스트 후보 ${index + 1}/${regions.length} 영역을 인식하는 중입니다.`
          });
        }
      );
      completedOcrUnits += rotations.length;
      activeOcrUnit = -1;
      if (normalized.words.some((item) => item.rotation && item.rotation !== 0) || normalized.lines.some((item) => item.rotation && item.rotation !== 0)) {
        rotatedRegions += 1;
      }
      if (!normalized.text && !normalized.words.length && !normalized.lines.length) {
        emptyRegions += 1;
      }
      removedGuideLines += normalized.removedGuideLines;
      cropResults.push(normalized);
    }

    emitProgress({
      status: "merge",
      progress: OCR_PROGRESS_RECOGNIZE_DONE,
      message: "OCR 결과를 정리하는 중입니다."
    });
    await worker.terminate();
    worker = null;
    emitProgress({
      status: "done",
      progress: 1,
      message: "OCR 분석이 완료되었습니다."
    });

    return mergeOcrResults(cropResults, {
      textCandidateBoxes: textDetection.debug.candidateBoxes,
      rawTextBoxes: textDetection.debug.rawBoxes,
      mergedTextBoxes: textDetection.debug.mergedBoxes,
      wallBlockedMerges: textDetection.debug.wallBlockedMerges,
      textRegions: textDetection.debug.regions,
      dimensionRegions: 0,
      dimensionGroupSplits: 0,
      verticalStripRegions: 0,
      verticalDividerDebug: [],
      dimensionBoundsDebug: null,
      removedGuideLines,
      ocrRegions: regions.length,
      emptyRegions,
      rotatedRegions
    });
  })().catch(async (error) => {
    if (cancelled) throw new FloorplanOcrCancelledError();
    if (worker?.terminate) {
      try {
        await worker.terminate();
      } catch {
        // Best-effort cleanup.
      }
    }
    worker = null;
    throw error;
  });

  return { promise, cancel };
}

async function loadTesseract() {
  const existing = (window as TesseractWindow).Tesseract;
  if (existing) return existing;
  if (!tesseractLoadPromise) {
    tesseractLoadPromise = new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = TESSERACT_SCRIPT_URL;
      script.async = true;
      script.onload = () => {
        const loaded = (window as TesseractWindow).Tesseract;
        if (loaded) {
          resolve(loaded);
        } else {
          reject(new Error("Tesseract.js를 불러왔지만 OCR 객체를 찾지 못했습니다."));
        }
      };
      script.onerror = () => reject(new Error("Tesseract.js CDN 로딩에 실패했습니다."));
      document.head.appendChild(script);
    });
  }
  return tesseractLoadPromise;
}

function normalizeProgressMessage(
  message: any,
  activeOcrUnit = -1,
  totalOcrUnits = 1
): FloorplanOcrProgress {
  const rawStatus = String(message?.status ?? "working");
  const localProgress = Number.isFinite(message?.progress)
    ? Math.max(0, Math.min(1, Number(message.progress)))
    : 0;
  const isRecognizing = rawStatus === "recognizing text";
  const progress = isRecognizing && activeOcrUnit >= 0
    ? ocrUnitProgress(activeOcrUnit, totalOcrUnits, localProgress)
    : OCR_PROGRESS_TEXT_DETECTED + localProgress * (OCR_PROGRESS_WORKER_READY - OCR_PROGRESS_TEXT_DETECTED);
  return {
    status: rawStatus,
    progress,
    message: isRecognizing && activeOcrUnit >= 0
      ? translateTesseractStatus(rawStatus, localProgress)
      : translateTesseractStatus(rawStatus, progress)
  };
}

function ocrUnitProgress(unitIndex: number, totalUnits: number, localProgress: number): number {
  const safeTotalUnits = Math.max(1, totalUnits);
  const safeUnitIndex = Math.max(0, Math.min(safeTotalUnits, unitIndex));
  const safeLocalProgress = Math.max(0, Math.min(1, localProgress));
  return OCR_PROGRESS_WORKER_READY
    + ((safeUnitIndex + safeLocalProgress) / safeTotalUnits)
    * (OCR_PROGRESS_RECOGNIZE_DONE - OCR_PROGRESS_WORKER_READY);
}

function translateTesseractStatus(status: string, progress: number) {
  const percent = Math.round(progress * 100);
  const label =
    {
      "loading tesseract core": "OCR 코어를 불러오는 중입니다.",
      "initializing tesseract": "OCR 엔진을 초기화하는 중입니다.",
      "loading language traineddata": "한글/영문 학습 데이터를 불러오는 중입니다.",
      "initializing api": "OCR API를 초기화하는 중입니다.",
      "recognizing text": "텍스트를 인식하는 중입니다."
    }[status] ?? "OCR 작업을 처리하는 중입니다.";
  return `${label} (${percent}%)`;
}

function normalizeOcrResult(data: any, region?: FloorplanTextRegion, scale = 1, regionIndex?: number, rotation = 0): FloorplanOcrResult {
  const blocks = normalizeOcrItems(data?.blocks);
  const lines = normalizeOcrItems(data?.lines);
  const words = normalizeOcrItems(data?.words);
  const nestedLines = collectNestedOcrItems(data?.blocks, "lines");
  const nestedWords = collectNestedOcrItems(data?.blocks, "words");
  const normalizedWords = offsetOcrItems(words.length ? words : nestedWords, region, scale, regionIndex, rotation);
  const normalizedLines = offsetOcrItems(lines.length ? lines : nestedLines, region, scale, regionIndex, rotation);
  const normalizedBlocks = offsetOcrItems(blocks, region, scale, regionIndex, rotation);

  return {
    text: normalizedOcrText(data?.text, normalizedLines, normalizedWords),
    words: normalizedWords,
    lines: normalizedLines,
    blocks: normalizedBlocks,
    rawBlockCount: Array.isArray(data?.blocks) ? data.blocks.length : 0,
    textCandidateBoxes: 0,
    rawTextBoxes: 0,
    mergedTextBoxes: 0,
    wallBlockedMerges: 0,
    textRegions: 0,
    dimensionRegions: 0,
    dimensionGroupSplits: 0,
    verticalStripRegions: 0,
    verticalDividerDebug: [],
    dimensionBoundsDebug: null,
    removedGuideLines: 0,
    ocrRegions: 0,
    emptyRegions: 0,
    rotatedRegions: 0
  };
}

function normalizeOcrItems(items: any[]): FloorplanOcrWord[] {
  if (!Array.isArray(items)) return [];
  return items
    .map((item) => ({
      text: String(item?.text ?? "").trim(),
      confidence: Number.isFinite(item?.confidence) ? Math.round(item.confidence) : 0,
      bbox: normalizeBbox(item?.bbox)
    }))
    .filter((item) => isUsableOcrText(item.text))
    .map((item) => ({
      ...item,
      kind: classifyOcrText(item.text)
    }));
}

function collectNestedOcrItems(blocks: any[], key: "lines" | "words"): FloorplanOcrWord[] {
  if (!Array.isArray(blocks)) return [];
  const collected: any[] = [];
  for (const block of blocks) {
    if (Array.isArray(block?.[key])) {
      collected.push(...block[key]);
    }
    if (Array.isArray(block?.paragraphs)) {
      for (const paragraph of block.paragraphs) {
        if (Array.isArray(paragraph?.[key])) {
          collected.push(...paragraph[key]);
        }
        if (key === "words" && Array.isArray(paragraph?.lines)) {
          for (const line of paragraph.lines) {
            if (Array.isArray(line?.words)) collected.push(...line.words);
          }
        }
      }
    }
  }
  return normalizeOcrItems(collected);
}

function normalizeBbox(bbox: any) {
  if (!bbox) return null;
  const x0 = Number(bbox.x0);
  const y0 = Number(bbox.y0);
  const x1 = Number(bbox.x1);
  const y1 = Number(bbox.y1);
  if (![x0, y0, x1, y1].every(Number.isFinite)) return null;
  return { x0, y0, x1, y1 };
}

function offsetOcrItems(
  items: FloorplanOcrWord[],
  region: FloorplanTextRegion | undefined,
  scale: number,
  regionIndex: number | undefined,
  rotation: number
): FloorplanOcrWord[] {
  if (!region) return items;
  return items.map((item) => ({
    ...item,
    regionIndex,
    rotation,
    dimensionSide: region.dimensionSide,
    dimensionScaleSide: region.dimensionScaleSide,
    dimensionLane: region.dimensionLane,
    dimensionDividerX: region.dimensionDividerX,
    bbox: item.bbox
      ? {
          ...offsetOcrBbox(item.bbox, region, scale, rotation)
        }
      : null
  })).filter((item) => isWithinDimensionDivider(item, region));
}

function isWithinDimensionDivider(item: FloorplanOcrWord, region: FloorplanTextRegion): boolean {
  if (region.source !== "vertical-dimension-strip") return true;
  if (!Number.isFinite(region.dimensionDividerX) || !region.dimensionSide || !region.dimensionLane || !item.bbox) return true;
  const center = (item.bbox.x0 + item.bbox.x1) / 2;
  const dividerX = region.dimensionDividerX!;
  const tolerance = 2;
  if (region.dimensionSide === "left") {
    return region.dimensionLane === "outer" ? center <= dividerX + tolerance : center >= dividerX - tolerance;
  }
  return region.dimensionLane === "outer" ? center >= dividerX - tolerance : center <= dividerX + tolerance;
}

function offsetOcrBbox(
  bbox: NonNullable<FloorplanOcrWord["bbox"]>,
  region: FloorplanTextRegion,
  scale: number,
  rotation: number
) {
  const x0 = bbox.x0 / scale;
  const y0 = bbox.y0 / scale;
  const x1 = bbox.x1 / scale;
  const y1 = bbox.y1 / scale;
  if (rotation === 90) {
    return normalizeOffsetBbox(region, y0, region.height - x1, y1, region.height - x0);
  }
  if (rotation === -90) {
    return normalizeOffsetBbox(region, region.width - y1, x0, region.width - y0, x1);
  }
  return normalizeOffsetBbox(region, x0, y0, x1, y1);
}

function normalizeOffsetBbox(region: FloorplanTextRegion, x0: number, y0: number, x1: number, y1: number) {
  return {
    x0: Math.round(region.x + Math.min(x0, x1)),
    y0: Math.round(region.y + Math.min(y0, y1)),
    x1: Math.round(region.x + Math.max(x0, x1)),
    y1: Math.round(region.y + Math.max(y0, y1))
  };
}

function mergeOcrResults(
  results: FloorplanOcrResult[],
  stats: Pick<
    FloorplanOcrResult,
    "rawTextBoxes" | "mergedTextBoxes" | "textCandidateBoxes" | "textRegions" | "ocrRegions" | "emptyRegions" | "rotatedRegions"
    | "wallBlockedMerges" | "dimensionRegions" | "dimensionGroupSplits" | "verticalStripRegions" | "verticalDividerDebug" | "dimensionBoundsDebug" | "removedGuideLines"
  >
): FloorplanOcrResult {
  const words = mergeRoomLabelOcrFragments(results.flatMap((result) => result.words));
  const lines = mergeRoomLabelOcrFragments(results.flatMap((result) => result.lines));
  return {
    text: mergeOcrText(results.map((result) => result.text).filter(Boolean).join("\n").trim(), lines),
    words,
    lines,
    blocks: results.flatMap((result) => result.blocks),
    rawBlockCount: results.reduce((total, result) => total + result.rawBlockCount, 0),
    ...stats
  };
}

function mergeRoomLabelOcrFragments(items: FloorplanOcrWord[]): FloorplanOcrWord[] {
  const sorted = [...items].sort((a, b) => {
    const ay = a.bbox ? (a.bbox.y0 + a.bbox.y1) / 2 : 0;
    const by = b.bbox ? (b.bbox.y0 + b.bbox.y1) / 2 : 0;
    return ay - by || ((a.bbox?.x0 ?? 0) - (b.bbox?.x0 ?? 0));
  });
  const used = new Uint8Array(sorted.length);
  const merged: FloorplanOcrWord[] = [];

  for (let index = 0; index < sorted.length; index += 1) {
    if (used[index]) continue;
    const candidate = findRoomLabelMerge(sorted, used, index);
    if (candidate) {
      for (const itemIndex of candidate.indexes) used[itemIndex] = 1;
      merged.push(candidate.item);
      continue;
    }
    used[index] = 1;
    merged.push(sorted[index]);
  }

  return merged.sort((a, b) => (a.bbox?.y0 ?? 0) - (b.bbox?.y0 ?? 0) || (a.bbox?.x0 ?? 0) - (b.bbox?.x0 ?? 0));
}

function findRoomLabelMerge(
  items: FloorplanOcrWord[],
  used: Uint8Array,
  startIndex: number
): { item: FloorplanOcrWord; indexes: number[] } | null {
  const base = items[startIndex];
  if (!isRoomLabelFragment(base)) return null;
  const nearby = items
    .map((item, index) => ({ item, index }))
    .filter(({ item, index }) => index !== startIndex && !used[index] && isRoomLabelFragment(item) && canMergeOcrFragments(base, item))
    .sort((a, b) => (a.item.bbox?.x0 ?? 0) - (b.item.bbox?.x0 ?? 0));
  const window = [{ item: base, index: startIndex }, ...nearby].sort((a, b) => (a.item.bbox?.x0 ?? 0) - (b.item.bbox?.x0 ?? 0));

  for (let size = Math.min(3, window.length); size >= 2; size -= 1) {
    for (let offset = 0; offset <= window.length - size; offset += 1) {
      const slice = window.slice(offset, offset + size);
      if (!slice.some(({ index }) => index === startIndex)) continue;
      const text = slice.map(({ item }) => item.text).join("");
      const label = normalizeExactRoomLabel(text);
      if (!label || normalizeLabelComparableText(text).length < 2) continue;
      return {
        indexes: slice.map(({ index }) => index),
        item: mergeOcrFragmentItems(slice.map(({ item }) => item), label)
      };
    }
  }

  return null;
}

function isRoomLabelFragment(item: FloorplanOcrWord): boolean {
  const text = item.text.replace(/\s+/g, "");
  return Boolean(item.bbox) && item.kind !== "dimension" && /[가-힣]/.test(text) && !/[0-9]/.test(text);
}

function canMergeOcrFragments(a: FloorplanOcrWord, b: FloorplanOcrWord): boolean {
  if (!a.bbox || !b.bbox) return false;
  const aHeight = a.bbox.y1 - a.bbox.y0;
  const bHeight = b.bbox.y1 - b.bbox.y0;
  const avgHeight = Math.max(1, (aHeight + bHeight) / 2);
  const centerGapY = Math.abs((a.bbox.y0 + a.bbox.y1) / 2 - (b.bbox.y0 + b.bbox.y1) / 2);
  const gapX = Math.max(0, Math.max(a.bbox.x0 - b.bbox.x1, b.bbox.x0 - a.bbox.x1));
  const verticalOverlap = bboxOverlapRatio(a.bbox.y0, a.bbox.y1, b.bbox.y0, b.bbox.y1);
  return centerGapY <= Math.max(8, avgHeight * 0.9)
    && verticalOverlap >= 0.12
    && gapX <= Math.max(18, avgHeight * 3.2);
}

function mergeOcrFragmentItems(items: FloorplanOcrWord[], label: string): FloorplanOcrWord {
  const bboxes = items.map((item) => item.bbox).filter((bbox): bbox is NonNullable<FloorplanOcrWord["bbox"]> => Boolean(bbox));
  return {
    text: label,
    confidence: Math.round(items.reduce((total, item) => total + item.confidence, 0) / Math.max(1, items.length)),
    kind: "room-label",
    regionIndex: items[0]?.regionIndex,
    rotation: items[0]?.rotation,
    bbox: {
      x0: Math.min(...bboxes.map((bbox) => bbox.x0)),
      y0: Math.min(...bboxes.map((bbox) => bbox.y0)),
      x1: Math.max(...bboxes.map((bbox) => bbox.x1)),
      y1: Math.max(...bboxes.map((bbox) => bbox.y1))
    }
  };
}

function mergeOcrText(rawText: string, lines: FloorplanOcrWord[]): string {
  const labelText = lines
    .filter((item) => item.kind === "room-label")
    .map((item) => item.text)
    .filter(Boolean)
    .join("\n");
  if (!labelText) return rawText;
  const rawParts = rawText ? [rawText] : [];
  return [...rawParts, labelText].join("\n").trim();
}

function bboxOverlapRatio(aMin: number, aMax: number, bMin: number, bMax: number): number {
  const overlap = Math.max(0, Math.min(aMax, bMax) - Math.max(aMin, bMin));
  const smaller = Math.max(1, Math.min(aMax - aMin, bMax - bMin));
  return overlap / smaller;
}

function emptyOcrResult(
  rawTextBoxes: number,
  mergedTextBoxes: number,
  textCandidateBoxes: number,
  textRegions: number,
  dimensionRegions: number,
  dimensionGroupSplits: number,
  verticalStripRegions: number
  , verticalDividerDebug: FloorplanVerticalDividerDebug[] = [],
  dimensionBoundsDebug: FloorplanDimensionBoundsDebug | null = null
): FloorplanOcrResult {
  return {
    text: "",
    words: [],
    lines: [],
    blocks: [],
    rawBlockCount: 0,
    rawTextBoxes,
    mergedTextBoxes,
    wallBlockedMerges: 0,
    textCandidateBoxes,
    textRegions,
    dimensionRegions,
    dimensionGroupSplits,
    verticalStripRegions,
    verticalDividerDebug,
    dimensionBoundsDebug,
    removedGuideLines: 0,
    ocrRegions: 0,
    emptyRegions: 0,
    rotatedRegions: 0
  };
}

async function recognizeRegion(
  worker: any,
  imageData: ImageData,
  region: FloorplanTextRegion,
  regionIndex: number,
  onRotationStart?: (rotationIndex: number) => void
): Promise<FloorplanOcrResult> {
  const rotations = getOcrRotations(region);
  const results: FloorplanOcrResult[] = [];

  for (let rotationIndex = 0; rotationIndex < rotations.length; rotationIndex += 1) {
    onRotationStart?.(rotationIndex);
    const rotation = rotations[rotationIndex];
    const crop = await cropTextRegion(imageData, region, OCR_CROP_SCALE, rotation);
    const result = await worker.recognize(crop.blob, undefined, {
      text: true,
      blocks: true
    });
    const normalized = normalizeOcrResult(result?.data, region, OCR_CROP_SCALE, regionIndex, rotation);
    normalized.removedGuideLines = crop.removedGuideLines;
    results.push(normalized);
  }

  return results.sort((a, b) => scoreOcrResult(b) - scoreOcrResult(a))[0];
}

function getOcrRotations(
  region: FloorplanTextRegion
): number[] {
  if (!shouldTryRotatedOcr(region)) return [0];
  if (region.source === "vertical-dimension-strip") {
    return [90];
  }
  return [0, -90, 90];
}

function shouldTryRotatedOcr(region: FloorplanTextRegion): boolean {
  const aspect = region.height / Math.max(1, region.width);
  return region.height >= 14 && aspect >= 1.02;
}

function scoreOcrResult(result: FloorplanOcrResult): number {
  const text = result.text.replace(/\s+/g, "");
  const comparableText = normalizeOcrNumericText(text);
  if (!text) return -200;
  const confidenceItems = result.words.length ? result.words : result.lines;
  const avgConfidence = confidenceItems.length
    ? confidenceItems.reduce((total, item) => total + item.confidence, 0) / confidenceItems.length
    : 0;
  const digits = (comparableText.match(/[0-9]/g) ?? []).length;
  const korean = (text.match(/[가-힣]/g) ?? []).length;
  const likelyDimension = /^[0-9]{3,6}$/.test(comparableText);
  const leadingZeroPenalty = /^0[0-9]{2,}/.test(comparableText) ? 70 : 0;
  const overMergedNumberPenalty = /^[0-9]{6,}$/.test(comparableText) ? 90 : /^[0-9]{5}$/.test(comparableText) ? 12 : 0;
  const shortNumberPenalty = /^[0-9]{1,2}$/.test(comparableText) ? 45 : 0;
  const singleKoreanPenalty = /^[가-힣]$/.test(text) && !["방", "욕"].includes(text) ? 55 : 0;
  const rotatedBonus = confidenceItems.some((item) => item.rotation && item.rotation !== 0) ? 8 : 0;
  return avgConfidence + digits * 12 + korean * 10 + (likelyDimension ? 34 : 0) + rotatedBonus
    - leadingZeroPenalty - overMergedNumberPenalty - shortNumberPenalty - singleKoreanPenalty - Math.max(0, text.length - 10) * 3;
}

function normalizedOcrText(rawText: unknown, lines: FloorplanOcrWord[], words: FloorplanOcrWord[]): string {
  const rawLines = String(rawText ?? "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(isUsableOcrText);
  if (rawLines.length) return rawLines.join("\n");
  const source = lines.length ? lines : words;
  return source.map((item) => item.text).filter(isUsableOcrText).join(" ").trim();
}

function isUsableOcrText(text: string): boolean {
  const normalized = text.trim();
  if (!normalized) return false;
  return /^[0-9A-Za-z가-힣\s,]+$/.test(normalized);
}

function classifyOcrText(text: string): FloorplanOcrWord["kind"] {
  const normalized = normalizeOcrNumericText(text.replace(/\s+/g, ""));
  if (/^[0-9]{3,6}$/.test(normalized)) return "noise";
  if (/^[0-9]{1,2}$/.test(normalized)) return "noise";
  if (/^[가-힣]$/.test(normalized) && !["방", "욕"].includes(normalized)) return "noise";
  if (/[가-힣]/.test(normalized)) return "room-label";
  return "noise";
}

function normalizeOcrNumericText(text: string): string {
  return text.replace(/,/g, "");
}

async function readImageData(image: Blob): Promise<ImageData> {
  const bitmap = await createImageBitmap(image);
  try {
    const canvas = document.createElement("canvas");
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) throw new Error("OCR용 이미지 데이터를 준비하지 못했습니다.");
    context.drawImage(bitmap, 0, 0);
    return context.getImageData(0, 0, canvas.width, canvas.height);
  } finally {
    bitmap.close?.();
  }
}

async function cropTextRegion(
  imageData: ImageData,
  region: FloorplanTextRegion,
  scale: number,
  rotation = 0
): Promise<{ blob: Blob; removedGuideLines: number }> {
  const cropCanvas = document.createElement("canvas");
  cropCanvas.width = region.width;
  cropCanvas.height = region.height;
  const cropContext = cropCanvas.getContext("2d", { willReadFrequently: true });
  if (!cropContext) throw new Error("OCR crop 이미지를 만들 수 없습니다.");
  cropContext.fillStyle = "#ffffff";
  cropContext.fillRect(0, 0, cropCanvas.width, cropCanvas.height);
  cropContext.putImageData(imageData, -region.x, -region.y);
  const removedGuideLines = shouldCleanDimensionCrop(region) ? eraseLongGuideLinesFromCrop(cropContext, cropCanvas.width, cropCanvas.height) : 0;

  const sourceCanvas = rotation === 0 ? cropCanvas : rotateCanvas(cropCanvas, rotation);
  const scaledCanvas = document.createElement("canvas");
  scaledCanvas.width = Math.max(1, sourceCanvas.width * scale);
  scaledCanvas.height = Math.max(1, sourceCanvas.height * scale);
  const scaledContext = scaledCanvas.getContext("2d", { alpha: false });
  if (!scaledContext) throw new Error("OCR crop 확대 이미지를 만들 수 없습니다.");
  scaledContext.fillStyle = "#ffffff";
  scaledContext.fillRect(0, 0, scaledCanvas.width, scaledCanvas.height);
  scaledContext.imageSmoothingEnabled = false;
  scaledContext.drawImage(sourceCanvas, 0, 0, scaledCanvas.width, scaledCanvas.height);

  const blob = await new Promise<Blob | null>((resolve) => scaledCanvas.toBlob(resolve, "image/png"));
  if (!blob) throw new Error("OCR crop PNG 생성에 실패했습니다.");
  return { blob, removedGuideLines };
}

function shouldCleanDimensionCrop(region: FloorplanTextRegion): boolean {
  return region.source === "dimension" || region.source === "vertical-dimension-strip";
}

function eraseLongGuideLinesFromCrop(context: CanvasRenderingContext2D, width: number, height: number): number {
  if (width < 10 || height < 10) return 0;
  const image = context.getImageData(0, 0, width, height);
  const data = image.data;
  let removed = 0;
  removed += eraseHorizontalRuns(data, width, height);
  removed += eraseVerticalRuns(data, width, height);
  if (removed > 0) {
    context.putImageData(image, 0, 0);
  }
  return removed;
}

function eraseHorizontalRuns(data: Uint8ClampedArray, width: number, height: number): number {
  const minRun = Math.max(24, Math.floor(width * 0.32));
  let removed = 0;
  for (let y = 0; y < height; y += 1) {
    let start = -1;
    for (let x = 0; x <= width; x += 1) {
      const dark = x < width && isDarkGuidePixel(data, (y * width + x) * 4);
      if (dark && start < 0) start = x;
      if ((!dark || x === width) && start >= 0) {
        const end = x - 1;
        if (end - start + 1 >= minRun) {
          paintWhiteRun(data, width, height, start, y, end, y, 1);
          removed += 1;
        }
        start = -1;
      }
    }
  }
  return removed;
}

function eraseVerticalRuns(data: Uint8ClampedArray, width: number, height: number): number {
  const minRun = Math.max(30, Math.floor(height * 0.36));
  let removed = 0;
  for (let x = 0; x < width; x += 1) {
    let start = -1;
    for (let y = 0; y <= height; y += 1) {
      const dark = y < height && isDarkGuidePixel(data, (y * width + x) * 4);
      if (dark && start < 0) start = y;
      if ((!dark || y === height) && start >= 0) {
        const end = y - 1;
        if (end - start + 1 >= minRun) {
          paintWhiteRun(data, width, height, x, start, x, end, 1);
          removed += 1;
        }
        start = -1;
      }
    }
  }
  return removed;
}

function isDarkGuidePixel(data: Uint8ClampedArray, offset: number): boolean {
  if (data[offset + 3] < 32) return false;
  const gray = (data[offset] * 77 + data[offset + 1] * 150 + data[offset + 2] * 29) >> 8;
  return gray < 190;
}

function paintWhiteRun(
  data: Uint8ClampedArray,
  width: number,
  height: number,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  radius: number
) {
  const minX = Math.max(0, Math.min(x0, x1) - radius);
  const maxX = Math.min(width - 1, Math.max(x0, x1) + radius);
  const minY = Math.max(0, Math.min(y0, y1) - radius);
  const maxY = Math.min(height - 1, Math.max(y0, y1) + radius);
  for (let y = minY; y <= maxY; y += 1) {
    for (let x = minX; x <= maxX; x += 1) {
      const offset = (y * width + x) * 4;
      data[offset] = 255;
      data[offset + 1] = 255;
      data[offset + 2] = 255;
      data[offset + 3] = 255;
    }
  }
}

function rotateCanvas(canvas: HTMLCanvasElement, rotation: number): HTMLCanvasElement {
  const rotated = document.createElement("canvas");
  const quarterTurn = Math.abs(rotation) === 90;
  rotated.width = quarterTurn ? canvas.height : canvas.width;
  rotated.height = quarterTurn ? canvas.width : canvas.height;
  const context = rotated.getContext("2d", { alpha: false });
  if (!context) throw new Error("OCR crop 회전 이미지를 만들 수 없습니다.");
  context.fillStyle = "#ffffff";
  context.fillRect(0, 0, rotated.width, rotated.height);
  context.translate(rotated.width / 2, rotated.height / 2);
  context.rotate((rotation * Math.PI) / 180);
  context.drawImage(canvas, -canvas.width / 2, -canvas.height / 2);
  return rotated;
}
