import type { FloorplanOcrResult, FloorplanOcrWord } from "../floorplan-ocr";

export type FloorplanScaleAxis = "horizontal" | "vertical";
export type FloorplanScaleSide = "top" | "bottom" | "left" | "right" | "inside";
export type FloorplanScaleLane = "outer" | "inner" | "unknown";

export type FloorplanScaleValue = {
  text: string;
  value: number;
  axis: FloorplanScaleAxis;
  side: FloorplanScaleSide;
  lane: FloorplanScaleLane;
  confidence: number;
  bbox: NonNullable<FloorplanOcrWord["bbox"]>;
  regionIndex?: number;
};

export type FloorplanScaleGroup = {
  axis: FloorplanScaleAxis;
  side: FloorplanScaleSide;
  lane: FloorplanScaleLane;
  values: FloorplanScaleValue[];
  discardedValues: Array<{
    value: FloorplanScaleValue;
    reason: string;
  }>;
  sum: number;
  confidence: number;
  label: string;
  note?: string;
};

export type FloorplanScaleEstimate = {
  widthMm: number | null;
  heightMm: number | null;
  widthGroup: FloorplanScaleGroup | null;
  heightGroup: FloorplanScaleGroup | null;
  groups: FloorplanScaleGroup[];
  ignored: Array<{
    text: string;
    reason: string;
    bbox: FloorplanOcrWord["bbox"];
  }>;
  warnings: string[];
};

const GROUP_AXIS_GAP_PX = 34;
const GROUP_RUN_GAP_PX = 260;
const SCALE_LANE_COLUMN_CLUSTER_GAP_PX = 24;
const SCALE_LANE_SPLIT_MIN_GAP_PX = 28;
const SCALE_LANE_ROW_CLUSTER_GAP_PX = 24;
const SCALE_LANE_ROW_SPLIT_MIN_GAP_PX = 24;
const MIN_DIMENSION_DIGITS = 3;
const MAX_DIMENSION_DIGITS = 6;
const MAX_REASONABLE_AXIS_MM = 30000;
const EDGE_SIDE_RATIO = 0.28;
const SCALE_RATIO_TOLERANCE_MIN = 0.45;
const SCALE_RATIO_TOLERANCE_MAX = 2.1;

export function estimateFloorplanScale(
  ocrResult: FloorplanOcrResult | null | undefined,
  imageWidth: number,
  imageHeight: number
): FloorplanScaleEstimate | null {
  if (!ocrResult || !Number.isFinite(imageWidth) || !Number.isFinite(imageHeight) || imageWidth <= 0 || imageHeight <= 0) {
    return null;
  }

  const { values, ignored } = collectScaleValues(ocrResult, imageWidth, imageHeight);
  const horizontalGroups = groupScaleValuesKeepingRegions(
    values.filter((value) => value.axis === "horizontal"),
    ignored,
    groupScaleValues
  );
  const verticalGroups = groupScaleValuesKeepingRegions(
    values.filter((value) => value.axis === "vertical"),
    ignored,
    groupVerticalScaleValues
  );
  const warnings: string[] = [];
  const reasonableHorizontalGroups = filterReasonableScaleGroups(horizontalGroups, ignored, warnings);
  const widthGroup = chooseScaleGroup(reasonableHorizontalGroups);
  const reasonableVerticalGroups = filterAspectReasonableScaleGroups(
    filterReasonableScaleGroups(verticalGroups, ignored, warnings),
    widthGroup,
    imageWidth,
    imageHeight,
    ignored,
    warnings
  );
  const heightGroup = chooseVerticalScaleGroup(reasonableVerticalGroups) ?? chooseScaleGroup(reasonableVerticalGroups);
  const groups = [...reasonableHorizontalGroups, ...reasonableVerticalGroups].sort((a, b) => {
    if (a.axis !== b.axis) return a.axis.localeCompare(b.axis);
    return b.sum - a.sum;
  });

  if (!widthGroup) warnings.push("가로 치수 후보를 찾지 못했습니다.");
  if (!heightGroup) warnings.push("세로 치수 후보를 찾지 못했습니다.");
  for (const group of groups) {
    if (group.values.length === 1 && group.sum < 1000) {
      warnings.push(`${group.label} 후보 ${group.sum}mm는 전체 치수로 보기에는 작을 수 있습니다.`);
    }
  }

  return {
    widthMm: widthGroup?.sum ?? null,
    heightMm: heightGroup?.sum ?? null,
    widthGroup,
    heightGroup,
    groups,
    ignored,
    warnings
  };
}

function collectScaleValues(
  ocrResult: FloorplanOcrResult,
  imageWidth: number,
  imageHeight: number
): { values: FloorplanScaleValue[]; ignored: FloorplanScaleEstimate["ignored"] } {
  const regionValues = collectRegionScaleValues(ocrResult, imageWidth, imageHeight);
  const consumedRegions = new Set(regionValues.values.map((value) => value.regionIndex).filter((value): value is number => value !== undefined));
  const source = dedupeOcrItems([...ocrResult.lines, ...ocrResult.words])
    .filter((item) => item.regionIndex === undefined || !consumedRegions.has(item.regionIndex));
  const values: FloorplanScaleValue[] = [];
  const ignored: FloorplanScaleEstimate["ignored"] = [...regionValues.ignored];

  for (const item of source) {
    const text = normalizeDimensionText(item.text);
    const invalidReason = invalidDimensionReason(text, item.text);
    if (invalidReason) {
      if (looksLikeDimensionNoise(item.text)) {
        ignored.push({ text: item.text, reason: invalidReason, bbox: item.bbox });
      }
      continue;
    }
    if (!item.bbox) {
      ignored.push({ text: item.text, reason: "좌표가 없어 치수 계산에서 제외했습니다.", bbox: item.bbox });
      continue;
    }

    const axis = detectDimensionAxis(item);
    values.push({
      text,
      value: Number(text),
      axis,
      side: detectDimensionSide(item.bbox, axis, imageWidth, imageHeight, item),
      lane: detectDimensionLane(item),
      confidence: item.confidence,
      bbox: item.bbox
    });
  }

  return { values: [...regionValues.values, ...values], ignored };
}

function collectRegionScaleValues(
  ocrResult: FloorplanOcrResult,
  imageWidth: number,
  imageHeight: number
): { values: FloorplanScaleValue[]; ignored: FloorplanScaleEstimate["ignored"] } {
  const lineItems = ocrResult.lines.filter(isRegionScaleItem);
  const wordItems = ocrResult.words.filter(isRegionScaleItem);
  const allItems = [...lineItems, ...wordItems];
  const regionIndexes = [...new Set(allItems.map((item) => item.regionIndex).filter((index): index is number => index !== undefined))];
  const values: FloorplanScaleValue[] = [];
  const ignored: FloorplanScaleEstimate["ignored"] = [];

  for (const regionIndex of regionIndexes) {
    const regionLines = lineItems.filter((item) => item.regionIndex === regionIndex);
    const regionWords = wordItems.filter((item) => item.regionIndex === regionIndex);
    const regionItems = [...regionLines, ...regionWords];
    const preferredItems = regionLines.some((item) => splitDimensionTokens(item.text).length) ? regionLines : regionWords;
    const first = regionItems[0];
    if (!first?.bbox) continue;
    const axis = detectDimensionAxis(first);
    const side = detectDimensionSide(first.bbox, axis, imageWidth, imageHeight, first);
    const lane = detectDimensionLane(first);
    const regionBox = mergeScaleItemBboxes(regionItems);

    for (const item of preferredItems) {
      for (const token of splitDimensionTokens(item.text)) {
        const text = normalizeDimensionText(token);
        const invalidReason = invalidDimensionReason(text, token);
        if (invalidReason) {
          if (looksLikeDimensionNoise(token)) {
            ignored.push({ text: token, reason: invalidReason, bbox: item.bbox });
          }
          continue;
        }
        values.push({
          text,
          value: Number(text),
          axis,
          side,
          lane,
          confidence: item.confidence,
          bbox: regionBox ?? item.bbox!,
          regionIndex
        });
      }
    }
  }

  return { values, ignored };
}

function isRegionScaleItem(item: FloorplanOcrWord): boolean {
  return Boolean(
    item.regionIndex !== undefined
    && item.bbox
    && item.dimensionLane
    && (item.dimensionScaleSide || item.dimensionSide)
  );
}

function splitDimensionTokens(text: string): string[] {
  return text.trim().split(/\s+/).map((token) => token.trim()).filter(Boolean);
}

function mergeScaleItemBboxes(items: FloorplanOcrWord[]): NonNullable<FloorplanOcrWord["bbox"]> | null {
  const bboxes = items.map((item) => item.bbox).filter((bbox): bbox is NonNullable<FloorplanOcrWord["bbox"]> => Boolean(bbox));
  if (!bboxes.length) return null;
  return {
    x0: Math.min(...bboxes.map((bbox) => bbox.x0)),
    y0: Math.min(...bboxes.map((bbox) => bbox.y0)),
    x1: Math.max(...bboxes.map((bbox) => bbox.x1)),
    y1: Math.max(...bboxes.map((bbox) => bbox.y1))
  };
}

function normalizeDimensionText(text: string): string {
  return text.replace(/\s+/g, "").replace(/,/g, "").replace(/[Oo]/g, "0").replace(/[Il|]/g, "1");
}

function invalidDimensionReason(normalizedText: string, rawText: string): string {
  if (!/^[0-9]+$/.test(normalizedText)) return `"${rawText}"는 숫자만 포함하지 않아 제외했습니다.`;
  if (normalizedText.length < MIN_DIMENSION_DIGITS) return `"${rawText}"는 ${MIN_DIMENSION_DIGITS}자리 미만이라 제외했습니다.`;
  if (normalizedText.length > MAX_DIMENSION_DIGITS) return `"${rawText}"는 ${MAX_DIMENSION_DIGITS}자리를 초과해 제외했습니다.`;
  if (/^0[0-9]+$/.test(normalizedText)) return `"${rawText}"는 0으로 시작해 제외했습니다.`;
  if (Number(normalizedText) > MAX_REASONABLE_AXIS_MM) {
    return `"${rawText}"는 ${MAX_REASONABLE_AXIS_MM}mm를 초과해 주거 평면도 치수로 보기 어려워 제외했습니다.`;
  }
  return "";
}

function looksLikeDimensionNoise(text: string): boolean {
  return /[0-9OoIl|]/.test(text);
}

function dedupeOcrItems(items: FloorplanOcrWord[]): FloorplanOcrWord[] {
  const sorted = [...items]
    .filter((item) => item.text && item.bbox)
    .sort((a, b) => b.confidence - a.confidence);
  const result: FloorplanOcrWord[] = [];

  for (const item of sorted) {
    if (result.some((existing) => isSameDimensionItem(existing, item))) continue;
    result.push(item);
  }

  return result;
}

function isSameDimensionItem(a: FloorplanOcrWord, b: FloorplanOcrWord): boolean {
  if (!a.bbox || !b.bbox) return false;
  if (normalizeDimensionText(a.text) !== normalizeDimensionText(b.text)) return false;
  const ax = (a.bbox.x0 + a.bbox.x1) / 2;
  const ay = (a.bbox.y0 + a.bbox.y1) / 2;
  const bx = (b.bbox.x0 + b.bbox.x1) / 2;
  const by = (b.bbox.y0 + b.bbox.y1) / 2;
  return Math.abs(ax - bx) <= 12 && Math.abs(ay - by) <= 12;
}

function detectDimensionAxis(item: FloorplanOcrWord): FloorplanScaleAxis {
  if (item.dimensionScaleSide === "left" || item.dimensionScaleSide === "right") return "vertical";
  if (item.dimensionScaleSide === "top" || item.dimensionScaleSide === "bottom") return "horizontal";
  if (item.rotation && item.rotation !== 0) return "vertical";
  if (!item.bbox) return "horizontal";
  const width = item.bbox.x1 - item.bbox.x0;
  const height = item.bbox.y1 - item.bbox.y0;
  return height > width * 1.18 ? "vertical" : "horizontal";
}

function detectDimensionSide(
  bbox: NonNullable<FloorplanOcrWord["bbox"]>,
  axis: FloorplanScaleAxis,
  imageWidth: number,
  imageHeight: number,
  item?: FloorplanOcrWord
): FloorplanScaleSide {
  if (item?.dimensionScaleSide) return item.dimensionScaleSide;
  if (axis === "vertical" && item?.dimensionSide) return item.dimensionSide;
  return detectDimensionSideFromBbox(bbox, axis, imageWidth, imageHeight);
}

function detectDimensionSideFromBbox(
  bbox: NonNullable<FloorplanOcrWord["bbox"]>,
  axis: FloorplanScaleAxis,
  imageWidth: number,
  imageHeight: number
): FloorplanScaleSide {
  const cx = (bbox.x0 + bbox.x1) / 2;
  const cy = (bbox.y0 + bbox.y1) / 2;
  if (axis === "vertical") {
    if (cx <= imageWidth * EDGE_SIDE_RATIO) return "left";
    if (cx >= imageWidth * (1 - EDGE_SIDE_RATIO)) return "right";
    return "inside";
  }
  if (cy <= imageHeight * EDGE_SIDE_RATIO) return "top";
  if (cy >= imageHeight * (1 - EDGE_SIDE_RATIO)) return "bottom";
  return "inside";
}

function detectDimensionLane(item: FloorplanOcrWord): FloorplanScaleLane {
  return item.dimensionLane ?? "unknown";
}

function refineVerticalDimensionLanes(values: FloorplanScaleValue[]): FloorplanScaleValue[] {
  const laneOverrides = new Map<FloorplanScaleValue, FloorplanScaleLane>();

  for (const side of ["left", "right"] as const) {
    const sideValues = values.filter((value) => value.axis === "vertical" && value.side === side);
    const columns = clusterVerticalScaleColumns(sideValues);
    if (columns.length < 2) continue;

    const sortedColumns = columns
      .map((column) => ({
        values: column,
        center: column.reduce((total, value) => total + centerX(value), 0) / column.length
      }))
      .sort((a, b) => a.center - b.center);
    const splitWidth = sortedColumns[sortedColumns.length - 1].center - sortedColumns[0].center;
    if (splitWidth < SCALE_LANE_SPLIT_MIN_GAP_PX) continue;

    const outerColumnIndex = side === "left" ? 0 : sortedColumns.length - 1;
    sortedColumns.forEach((column, index) => {
      const lane: FloorplanScaleLane = index === outerColumnIndex ? "outer" : "inner";
      for (const value of column.values) {
        laneOverrides.set(value, lane);
      }
    });
  }

  if (!laneOverrides.size) return values;
  return values.map((value) => {
    const lane = laneOverrides.get(value);
    return lane ? { ...value, lane } : value;
  });
}

function clusterVerticalScaleColumns(values: FloorplanScaleValue[]): FloorplanScaleValue[][] {
  const sorted = [...values].sort((a, b) => centerX(a) - centerX(b));
  const columns: FloorplanScaleValue[][] = [];

  for (const value of sorted) {
    const last = columns[columns.length - 1];
    const lastCenter = last
      ? last.reduce((total, item) => total + centerX(item), 0) / last.length
      : null;
    if (last && lastCenter !== null && Math.abs(centerX(value) - lastCenter) <= SCALE_LANE_COLUMN_CLUSTER_GAP_PX) {
      last.push(value);
    } else {
      columns.push([value]);
    }
  }

  return columns;
}

function refineHorizontalDimensionLanes(values: FloorplanScaleValue[]): FloorplanScaleValue[] {
  const laneOverrides = new Map<FloorplanScaleValue, FloorplanScaleLane>();

  for (const side of ["top", "bottom"] as const) {
    const sideValues = values.filter((value) => value.axis === "horizontal" && value.side === side);
    const rows = clusterHorizontalScaleRows(sideValues);
    if (rows.length < 2) continue;

    const sortedRows = rows
      .map((row) => ({
        values: row,
        center: row.reduce((total, value) => total + centerY(value), 0) / row.length
      }))
      .sort((a, b) => a.center - b.center);
    const splitHeight = sortedRows[sortedRows.length - 1].center - sortedRows[0].center;
    if (splitHeight < SCALE_LANE_ROW_SPLIT_MIN_GAP_PX) continue;

    const outerRowIndex = side === "top" ? 0 : sortedRows.length - 1;
    sortedRows.forEach((row, index) => {
      const lane: FloorplanScaleLane = index === outerRowIndex ? "outer" : "inner";
      for (const value of row.values) {
        laneOverrides.set(value, lane);
      }
    });
  }

  if (!laneOverrides.size) return values;
  return values.map((value) => {
    const lane = laneOverrides.get(value);
    return lane ? { ...value, lane } : value;
  });
}

function clusterHorizontalScaleRows(values: FloorplanScaleValue[]): FloorplanScaleValue[][] {
  const sorted = [...values].sort((a, b) => centerY(a) - centerY(b));
  const rows: FloorplanScaleValue[][] = [];

  for (const value of sorted) {
    const last = rows[rows.length - 1];
    const lastCenter = last
      ? last.reduce((total, item) => total + centerY(item), 0) / last.length
      : null;
    if (last && lastCenter !== null && Math.abs(centerY(value) - lastCenter) <= SCALE_LANE_ROW_CLUSTER_GAP_PX) {
      last.push(value);
    } else {
      rows.push([value]);
    }
  }

  return rows;
}

function groupVerticalScaleValues(values: FloorplanScaleValue[], ignored: FloorplanScaleEstimate["ignored"]): FloorplanScaleGroup[] {
  const groups: FloorplanScaleGroup[] = [];
  const used = new Set<FloorplanScaleValue>();

  for (const side of ["left", "right"] as const) {
    const sideValues = values.filter((value) => value.side === side);
    for (const column of clusterVerticalScaleColumns(sideValues)) {
      if (!column.length) continue;
      for (const value of column) used.add(value);
      groups.push(createScaleGroup(column, ignored));
    }
  }

  const remaining = values.filter((value) => !used.has(value));
  groups.push(...groupScaleValues(remaining, ignored));
  return groups.sort((a, b) => b.sum - a.sum);
}

function groupScaleValuesKeepingRegions(
  values: FloorplanScaleValue[],
  ignored: FloorplanScaleEstimate["ignored"],
  fallbackGroup: (values: FloorplanScaleValue[], ignored: FloorplanScaleEstimate["ignored"]) => FloorplanScaleGroup[]
): FloorplanScaleGroup[] {
  const groups: FloorplanScaleGroup[] = [];
  const used = new Set<FloorplanScaleValue>();
  const regionIndexes = [...new Set(values
    .map((value) => value.regionIndex)
    .filter((regionIndex): regionIndex is number => regionIndex !== undefined)
  )];

  for (const regionIndex of regionIndexes) {
    const regionValues = values.filter((value) => value.regionIndex === regionIndex);
    if (!regionValues.length) continue;
    for (const value of regionValues) used.add(value);
    groups.push(createScaleGroup(regionValues, ignored));
  }

  const remaining = values.filter((value) => !used.has(value));
  groups.push(...fallbackGroup(remaining, ignored));
  return groups.sort((a, b) => b.sum - a.sum);
}

function groupScaleValues(values: FloorplanScaleValue[], ignored: FloorplanScaleEstimate["ignored"]): FloorplanScaleGroup[] {
  const sorted = [...values].sort((a, b) =>
    a.axis === "vertical"
      ? centerX(a) - centerX(b) || centerY(a) - centerY(b)
      : centerY(a) - centerY(b) || centerX(a) - centerX(b)
  );
  const used = new Uint8Array(sorted.length);
  const groups: FloorplanScaleGroup[] = [];

  for (let index = 0; index < sorted.length; index += 1) {
    if (used[index]) continue;
    const group = [sorted[index]];
    used[index] = 1;
    let changed = true;

    while (changed) {
      changed = false;
      for (let otherIndex = 0; otherIndex < sorted.length; otherIndex += 1) {
        if (used[otherIndex]) continue;
      if (!isNearScaleGroup(sorted[otherIndex], group)) continue;
        group.push(sorted[otherIndex]);
        used[otherIndex] = 1;
        changed = true;
      }
    }

    groups.push(createScaleGroup(group, ignored));
  }

  return groups.sort((a, b) => b.sum - a.sum);
}

function isNearScaleGroup(value: FloorplanScaleValue, group: FloorplanScaleValue[]): boolean {
  const axis = group[0].axis;
  if (value.axis !== axis) return false;
  if (!isLaneCompatible(value, group[0])) return false;
  const groupAxis = group.reduce((total, item) => total + (axis === "vertical" ? centerX(item) : centerY(item)), 0) / group.length;
  const valueAxis = axis === "vertical" ? centerX(value) : centerY(value);
  if (Math.abs(valueAxis - groupAxis) > GROUP_AXIS_GAP_PX) return false;

  const runGap = Math.min(...group.map((item) => axis === "vertical"
    ? gapBetween(value.bbox.y0, value.bbox.y1, item.bbox.y0, item.bbox.y1)
    : gapBetween(value.bbox.x0, value.bbox.x1, item.bbox.x0, item.bbox.x1)
  ));
  return runGap <= GROUP_RUN_GAP_PX;
}

function isLaneCompatible(a: FloorplanScaleValue, b: FloorplanScaleValue): boolean {
  if (a.side !== b.side) return false;
  if (a.lane === "unknown" || b.lane === "unknown") return true;
  return a.lane === b.lane;
}

function createScaleGroup(values: FloorplanScaleValue[], ignored: FloorplanScaleEstimate["ignored"]): FloorplanScaleGroup {
  const normalized = normalizeScaleGroupValues(values, ignored);
  const sorted = [...normalized.values].sort((a, b) =>
    a.axis === "vertical"
      ? centerY(a) - centerY(b)
      : centerX(a) - centerX(b)
  );
  const side = mostCommonSide(sorted);
  const lane = mostCommonLane(sorted);
  const axis = sorted[0]?.axis ?? values[0].axis;
  const sum = sorted.reduce((total, value) => total + value.value, 0);
  return {
    axis,
    side,
    lane,
    values: sorted,
    discardedValues: normalized.discardedValues,
    sum,
    confidence: Math.round(sorted.reduce((total, value) => total + value.confidence, 0) / sorted.length),
    label: `${axisLabel(axis)} ${sideLabel(side)}${laneLabel(lane)}`,
    note: normalized.note
  };
}

function normalizeScaleGroupValues(
  values: FloorplanScaleValue[],
  ignored: FloorplanScaleEstimate["ignored"]
): {
  values: FloorplanScaleValue[];
  discardedValues: FloorplanScaleGroup["discardedValues"];
  note?: string;
} {
  const discardedValues: FloorplanScaleGroup["discardedValues"] = [];
  let nextValues = removeTinyDimensionOutliers(values, discardedValues);
  const wholeDimension = chooseWholeDimensionIfDuplicated(nextValues);

  if (wholeDimension) {
    const kept = wholeDimension.value;
    const discarded = nextValues.filter((value) => value !== kept);
    for (const value of discarded) {
      discardedValues.push({
        value,
        reason: `${kept.value}mm가 나머지 합계 ${wholeDimension.sumWithoutLargest}mm와 비슷해 세부 치수로 보고 제외했습니다.`
      });
    }
    nextValues = [kept];
  }

  for (const discarded of discardedValues) {
    ignored.push({
      text: discarded.value.text,
      reason: `"${discarded.value.text}"는 ${discarded.reason}`,
      bbox: discarded.value.bbox
    });
  }

  return {
    values: nextValues,
    discardedValues,
    note: wholeDimension
      ? `전체 치수 ${wholeDimension.value.value}mm가 세부 합계 ${wholeDimension.sumWithoutLargest}mm와 ±10% 이내라 전체값만 사용했습니다.`
      : undefined
  };
}

function removeTinyDimensionOutliers(
  values: FloorplanScaleValue[],
  discardedValues: FloorplanScaleGroup["discardedValues"]
): FloorplanScaleValue[] {
  if (values.length < 4) return values;
  const sortedNumbers = values.map((value) => value.value).sort((a, b) => a - b);
  const median = sortedNumbers[Math.floor(sortedNumbers.length / 2)];
  if (median < 1000) return values;

  const filtered = values.filter((value) => {
    const tooSmallForGroup = value.value < 500 && value.value < median * 0.25;
    if (!tooSmallForGroup) return true;
    discardedValues.push({
      value,
      reason: `같은 치수 묶음의 중앙값 ${median}mm 대비 너무 작아 OCR 잡음으로 제외했습니다.`
    });
    return false;
  });

  return filtered.length ? filtered : values;
}

function chooseWholeDimensionIfDuplicated(values: FloorplanScaleValue[]): { value: FloorplanScaleValue; sumWithoutLargest: number } | null {
  if (values.length < 2) return null;
  const sorted = [...values].sort((a, b) => b.value - a.value);
  const largest = sorted[0];
  const secondLargest = sorted[1];
  if (secondLargest && largest.value < secondLargest.value * 1.55) return null;
  const sumWithoutLargest = sorted.slice(1).reduce((total, value) => total + value.value, 0);
  if (!sumWithoutLargest) return null;
  const differenceRatio = Math.abs(largest.value - sumWithoutLargest) / largest.value;
  if (differenceRatio > 0.1) return null;
  return { value: largest, sumWithoutLargest };
}

function chooseScaleGroup(groups: FloorplanScaleGroup[]): FloorplanScaleGroup | null {
  if (!groups.length) return null;
  const exterior = groups.filter((group) => group.side !== "inside");
  const source = exterior.length ? exterior : groups;
  return [...source].sort((a, b) => {
    const sumDelta = b.sum - a.sum;
    if (Math.abs(sumDelta) > 120) return sumDelta;
    return b.values.length - a.values.length || b.confidence - a.confidence;
  })[0] ?? null;
}

function chooseVerticalScaleGroup(groups: FloorplanScaleGroup[]): FloorplanScaleGroup | null {
  const outerGroups = groups
    .filter((group) => group.axis === "vertical" && group.lane === "outer" && group.sum >= 1000)
    .sort((a, b) => b.sum - a.sum);
  for (const outer of outerGroups) {
    const matchedInner = groups.find((group) =>
      group !== outer
      && group.axis === "vertical"
      && group.side === outer.side
      && group.lane === "inner"
      && Math.abs(group.sum - outer.sum) / Math.max(1, outer.sum) <= 0.1
    );
    if (!matchedInner) continue;
    return {
      ...outer,
      note: `${outer.label} 전체 치수가 ${matchedInner.label} 세부 합계 ${matchedInner.sum}mm와 ±10% 이내라 바깥 전체값을 사용했습니다.`
    };
  }
  return null;
}

function filterReasonableScaleGroups(
  groups: FloorplanScaleGroup[],
  ignored: FloorplanScaleEstimate["ignored"],
  warnings: string[]
): FloorplanScaleGroup[] {
  return groups.filter((group) => {
    if (group.sum <= MAX_REASONABLE_AXIS_MM) return true;
    const reason = `${group.label} 합계 ${group.sum}mm가 ${MAX_REASONABLE_AXIS_MM}mm를 초과해 제외했습니다.`;
    warnings.push(reason);
    for (const value of group.values) {
      ignored.push({
        text: value.text,
        reason: `"${value.text}"는 ${reason}`,
        bbox: value.bbox
      });
    }
    return false;
  });
}

function filterAspectReasonableScaleGroups(
  groups: FloorplanScaleGroup[],
  widthGroup: FloorplanScaleGroup | null,
  imageWidth: number,
  imageHeight: number,
  ignored: FloorplanScaleEstimate["ignored"],
  warnings: string[]
): FloorplanScaleGroup[] {
  if (!widthGroup || !imageWidth || !imageHeight) return groups;
  const imageRatio = imageHeight / imageWidth;
  return groups.filter((group) => {
    const scaleRatio = group.sum / widthGroup.sum;
    const ratioDelta = scaleRatio / imageRatio;
    if (ratioDelta >= SCALE_RATIO_TOLERANCE_MIN && ratioDelta <= SCALE_RATIO_TOLERANCE_MAX) return true;
    const reason = `${group.label} ${group.sum}mm는 가로 후보 ${widthGroup.sum}mm와 이미지 비율 기준으로 맞지 않아 제외했습니다.`;
    warnings.push(reason);
    for (const value of group.values) {
      ignored.push({
        text: value.text,
        reason: `"${value.text}"는 ${reason}`,
        bbox: value.bbox
      });
    }
    return false;
  });
}

function mostCommonSide(values: FloorplanScaleValue[]): FloorplanScaleSide {
  const counts = new Map<FloorplanScaleSide, number>();
  for (const value of values) {
    counts.set(value.side, (counts.get(value.side) ?? 0) + 1);
  }
  return [...counts.entries()].sort((a, b) => b[1] - a[1])[0]?.[0] ?? "inside";
}

function mostCommonLane(values: FloorplanScaleValue[]): FloorplanScaleLane {
  const counts = new Map<FloorplanScaleLane, number>();
  for (const value of values) {
    counts.set(value.lane, (counts.get(value.lane) ?? 0) + 1);
  }
  return [...counts.entries()].sort((a, b) => b[1] - a[1])[0]?.[0] ?? "unknown";
}

function centerX(value: FloorplanScaleValue): number {
  return (value.bbox.x0 + value.bbox.x1) / 2;
}

function centerY(value: FloorplanScaleValue): number {
  return (value.bbox.y0 + value.bbox.y1) / 2;
}

function gapBetween(aMin: number, aMax: number, bMin: number, bMax: number): number {
  if (aMax < bMin) return bMin - aMax;
  if (bMax < aMin) return aMin - bMax;
  return 0;
}

function axisLabel(axis: FloorplanScaleAxis): string {
  return axis === "horizontal" ? "가로" : "세로";
}

function sideLabel(side: FloorplanScaleSide): string {
  return {
    top: "상단",
    bottom: "하단",
    left: "왼쪽",
    right: "오른쪽",
    inside: "내부"
  }[side];
}

function laneLabel(lane: FloorplanScaleLane): string {
  return {
    outer: " 바깥",
    inner: " 안쪽",
    unknown: ""
  }[lane];
}
