export interface FloorplanTextBox {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
  pixels: number;
}

export interface FloorplanTextRegion {
  x: number;
  y: number;
  width: number;
  height: number;
  boxes: FloorplanTextBox[];
  pixels: number;
  source?: "text" | "dimension" | "vertical-dimension-strip";
  dimensionSide?: "left" | "right";
  dimensionScaleSide?: "top" | "bottom" | "left" | "right";
  dimensionLane?: "outer" | "inner";
  dimensionDividerX?: number;
}

export interface FloorplanTextDetectionResult {
  mask: Uint8Array;
  boxes: FloorplanTextBox[];
  regions: FloorplanTextRegion[];
  debug: {
    darkPixels: number;
    rawBoxes: number;
    mergedBoxes: number;
    wallBlockedMerges: number;
    candidateBoxes: number;
    regions: number;
  };
}

export interface FloorplanTextDetectionOptions {
  relaxedAreas?: Array<{
    x: number;
    y: number;
    width: number;
    height: number;
  }>;
}

const DARK_THRESHOLD = 118;
const TEXT_MIN_PIXELS = 8;
const TEXT_MAX_PIXELS = 900;
const TEXT_MIN_WIDTH = 3;
const TEXT_MAX_WIDTH = 130;
const TEXT_MIN_HEIGHT = 5;
const TEXT_MAX_HEIGHT = 36;
const TEXT_MIN_DENSITY = 0.04;
const TEXT_MAX_DENSITY = 0.72;
const TEXT_PADDING = 2;
const TEXT_FRAGMENT_MIN_PIXELS = 2;
const TEXT_FRAGMENT_MAX_PIXELS = 900;
const TEXT_FRAGMENT_MAX_WIDTH = 150;
const TEXT_FRAGMENT_MAX_HEIGHT = 42;
const TEXT_FRAGMENT_MERGE_MAX_PASSES = 4;
const TEXT_REGION_PADDING = 5;
const TEXT_REGION_MIN_WIDTH = 8;
const TEXT_REGION_MIN_HEIGHT = 6;
const TEXT_REGION_MAX_HEIGHT = 52;

export function detectFloorplanTextBoxes(imageData: ImageData, options: FloorplanTextDetectionOptions = {}): FloorplanTextDetectionResult {
  const width = imageData.width;
  const height = imageData.height;
  const dark = new Uint8Array(width * height);
  const visited = new Uint8Array(width * height);
  const mask = new Uint8Array(width * height);
  const data = imageData.data;
  let darkPixels = 0;
  const mergeDebug = { wallBlockedMerges: 0 };

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      if (isDarkWallLikePixel(data[offset], data[offset + 1], data[offset + 2], data[offset + 3])) {
        dark[y * width + x] = 1;
        darkPixels += 1;
      }
    }
  }

  const rawBoxes: FloorplanTextBox[] = [];
  const queue: number[] = [];
  for (let index = 0; index < dark.length; index += 1) {
    if (!dark[index] || visited[index]) continue;
    const box = collectDarkPixelBox(index, dark, visited, width, height, queue);
    if (!isTextFragmentCandidate(box)) continue;
    rawBoxes.push(box);
  }

  const mergedBoxes = mergeNearbyTextFragments(rawBoxes, dark, width, height, mergeDebug, options.relaxedAreas ?? []);
  const boxes = mergedBoxes.filter(isConservativeTextBox);
  for (const box of boxes) {
    markTextBox(mask, box, width, height, TEXT_PADDING);
  }

  const regions = textBoxesToRegions(boxes, width, height);
  return {
    mask,
    boxes,
    regions,
    debug: {
      darkPixels,
      rawBoxes: rawBoxes.length,
      mergedBoxes: mergedBoxes.length,
      wallBlockedMerges: mergeDebug.wallBlockedMerges,
      candidateBoxes: boxes.length,
      regions: regions.length
    }
  };
}

function collectDarkPixelBox(
  startIndex: number,
  dark: Uint8Array,
  visited: Uint8Array,
  width: number,
  height: number,
  queue: number[]
): FloorplanTextBox {
  const startX = startIndex % width;
  const startY = Math.floor(startIndex / width);
  let minX = startX;
  let minY = startY;
  let maxX = startX;
  let maxY = startY;
  let pixels = 0;
  let head = 0;

  queue.length = 0;
  queue.push(startIndex);
  visited[startIndex] = 1;

  while (head < queue.length) {
    const current = queue[head];
    head += 1;
    const x = current % width;
    const y = Math.floor(current / width);
    pixels += 1;
    minX = Math.min(minX, x);
    minY = Math.min(minY, y);
    maxX = Math.max(maxX, x);
    maxY = Math.max(maxY, y);

    pushDarkPixelNeighbor(queue, dark, visited, width, height, x - 1, y);
    pushDarkPixelNeighbor(queue, dark, visited, width, height, x + 1, y);
    pushDarkPixelNeighbor(queue, dark, visited, width, height, x, y - 1);
    pushDarkPixelNeighbor(queue, dark, visited, width, height, x, y + 1);
  }

  return { minX, minY, maxX, maxY, pixels };
}

function pushDarkPixelNeighbor(
  queue: number[],
  dark: Uint8Array,
  visited: Uint8Array,
  width: number,
  height: number,
  x: number,
  y: number
): void {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  const index = y * width + x;
  if (!dark[index] || visited[index]) return;
  visited[index] = 1;
  queue.push(index);
}

function isConservativeTextBox(box: FloorplanTextBox): boolean {
  const width = box.maxX - box.minX + 1;
  const height = box.maxY - box.minY + 1;
  const density = box.pixels / Math.max(1, width * height);
  const aspect = width / Math.max(1, height);

  if (box.pixels < TEXT_MIN_PIXELS || box.pixels > TEXT_MAX_PIXELS) return false;
  if (width < TEXT_MIN_WIDTH || width > TEXT_MAX_WIDTH) return false;
  if (height < TEXT_MIN_HEIGHT || height > TEXT_MAX_HEIGHT) return false;
  if (density < TEXT_MIN_DENSITY || density > TEXT_MAX_DENSITY) return false;
  if (aspect > 9 || aspect < 0.12) return false;
  return true;
}

function isTextFragmentCandidate(box: FloorplanTextBox): boolean {
  const width = box.maxX - box.minX + 1;
  const height = box.maxY - box.minY + 1;
  const density = box.pixels / Math.max(1, width * height);
  const aspect = width / Math.max(1, height);

  if (box.pixels < TEXT_FRAGMENT_MIN_PIXELS || box.pixels > TEXT_FRAGMENT_MAX_PIXELS) return false;
  if (width > TEXT_FRAGMENT_MAX_WIDTH || height > TEXT_FRAGMENT_MAX_HEIGHT) return false;
  if (density < 0.015 || density > 0.88) return false;
  if (aspect > 18 || aspect < 0.05) return false;
  return true;
}

function mergeNearbyTextFragments(
  boxes: FloorplanTextBox[],
  dark: Uint8Array,
  imageWidth: number,
  imageHeight: number,
  debug: { wallBlockedMerges: number },
  relaxedAreas: NonNullable<FloorplanTextDetectionOptions["relaxedAreas"]>
): FloorplanTextBox[] {
  let current = [...boxes].sort((a, b) => centerY(a) - centerY(b) || a.minX - b.minX);

  for (let pass = 0; pass < TEXT_FRAGMENT_MERGE_MAX_PASSES; pass += 1) {
    const used = new Uint8Array(current.length);
    const next: FloorplanTextBox[] = [];
    let changed = false;

    for (let index = 0; index < current.length; index += 1) {
      if (used[index]) continue;
      let merged = current[index];
      used[index] = 1;

      for (let otherIndex = index + 1; otherIndex < current.length; otherIndex += 1) {
        if (used[otherIndex]) continue;
        const other = current[otherIndex];
        const relaxed = isRelaxedTextPair(merged, other, relaxedAreas);
        if (!shouldMergeTextFragments(merged, other, relaxed)) continue;
        if (!relaxed && hasDarkBarrierBetween(merged, other, dark, imageWidth, imageHeight)) {
          debug.wallBlockedMerges += 1;
          continue;
        }
        merged = mergeTextBoxes(merged, other);
        used[otherIndex] = 1;
        changed = true;
      }

      next.push(merged);
    }

    current = next.sort((a, b) => centerY(a) - centerY(b) || a.minX - b.minX);
    if (!changed) break;
  }

  return current;
}

function shouldMergeTextFragments(a: FloorplanTextBox, b: FloorplanTextBox, relaxed = false): boolean {
  const relax = relaxed ? 1.4 : 1;
  const aWidth = a.maxX - a.minX + 1;
  const bWidth = b.maxX - b.minX + 1;
  const aHeight = a.maxY - a.minY + 1;
  const bHeight = b.maxY - b.minY + 1;
  const avgHeight = (aHeight + bHeight) / 2;
  const centerGapY = Math.abs(centerY(a) - centerY(b));
  const verticalOverlap = overlapRatio(a.minY, a.maxY, b.minY, b.maxY);
  const horizontalGap = Math.max(0, Math.max(a.minX - b.maxX - 1, b.minX - a.maxX - 1));
  const horizontalOverlap = overlapRatio(a.minX, a.maxX, b.minX, b.maxX);
  const mergedWidth = Math.max(a.maxX, b.maxX) - Math.min(a.minX, b.minX) + 1;
  const mergedHeight = Math.max(a.maxY, b.maxY) - Math.min(a.minY, b.minY) + 1;
  const maxHorizontalGap = Math.max(8, Math.min(30, avgHeight * 1.65)) * relax;
  const sameTextLine = centerGapY <= Math.max(5, avgHeight * 0.62) * relax || verticalOverlap >= 0.22 / relax;
  const closeHorizontally = horizontalGap <= maxHorizontalGap || horizontalOverlap >= 0.15 / relax;
  const similarScale = Math.max(aHeight, bHeight) / Math.max(1, Math.min(aHeight, bHeight)) <= 2.6 * relax;

  if (!sameTextLine || !closeHorizontally || !similarScale) return false;
  if (mergedWidth > TEXT_MAX_WIDTH * 1.35 * relax || mergedHeight > TEXT_MAX_HEIGHT * 1.18 * relax) return false;
  if (aWidth <= 2 || bWidth <= 2) return horizontalGap <= maxHorizontalGap * 1.25;
  return true;
}

function isRelaxedTextPair(
  a: FloorplanTextBox,
  b: FloorplanTextBox,
  relaxedAreas: NonNullable<FloorplanTextDetectionOptions["relaxedAreas"]>
): boolean {
  if (!relaxedAreas.length) return false;
  const ax = centerX(a);
  const ay = centerY(a);
  const bx = centerX(b);
  const by = centerY(b);
  return relaxedAreas.some((area) =>
    pointInArea(ax, ay, area) && pointInArea(bx, by, area)
  );
}

function pointInArea(
  x: number,
  y: number,
  area: NonNullable<FloorplanTextDetectionOptions["relaxedAreas"]>[number]
): boolean {
  return x >= area.x && y >= area.y && x <= area.x + area.width && y <= area.y + area.height;
}

function mergeTextBoxes(a: FloorplanTextBox, b: FloorplanTextBox): FloorplanTextBox {
  return {
    minX: Math.min(a.minX, b.minX),
    minY: Math.min(a.minY, b.minY),
    maxX: Math.max(a.maxX, b.maxX),
    maxY: Math.max(a.maxY, b.maxY),
    pixels: a.pixels + b.pixels
  };
}

function hasDarkBarrierBetween(a: FloorplanTextBox, b: FloorplanTextBox, dark: Uint8Array, width: number, height: number): boolean {
  const horizontalGap = Math.max(0, Math.max(a.minX - b.maxX - 1, b.minX - a.maxX - 1));
  const verticalGap = Math.max(0, Math.max(a.minY - b.maxY - 1, b.minY - a.maxY - 1));

  if (horizontalGap >= 2) {
    const left = a.maxX < b.minX ? a : b;
    const right = left === a ? b : a;
    return hasVerticalBarrier(
      dark,
      width,
      height,
      left.maxX + 1,
      right.minX - 1,
      Math.min(left.minY, right.minY) - 2,
      Math.max(left.maxY, right.maxY) + 2
    );
  }

  if (verticalGap >= 2) {
    const top = a.maxY < b.minY ? a : b;
    const bottom = top === a ? b : a;
    return hasHorizontalBarrier(
      dark,
      width,
      height,
      Math.min(top.minX, bottom.minX) - 2,
      Math.max(top.maxX, bottom.maxX) + 2,
      top.maxY + 1,
      bottom.minY - 1
    );
  }

  return false;
}

function hasVerticalBarrier(
  dark: Uint8Array,
  width: number,
  height: number,
  rawMinX: number,
  rawMaxX: number,
  rawMinY: number,
  rawMaxY: number
): boolean {
  const minX = clamp(rawMinX, 0, width - 1);
  const maxX = clamp(rawMaxX, 0, width - 1);
  const minY = clamp(rawMinY, 0, height - 1);
  const maxY = clamp(rawMaxY, 0, height - 1);
  const span = Math.max(1, maxY - minY + 1);
  let consecutiveColumns = 0;

  for (let x = minX; x <= maxX; x += 1) {
    let darkCount = 0;
    for (let y = minY; y <= maxY; y += 1) {
      if (dark[y * width + x]) darkCount += 1;
    }
    if (darkCount >= Math.max(4, span * 0.62)) {
      consecutiveColumns += 1;
      if (consecutiveColumns >= 2) return true;
    } else {
      consecutiveColumns = 0;
    }
  }

  return false;
}

function hasHorizontalBarrier(
  dark: Uint8Array,
  width: number,
  height: number,
  rawMinX: number,
  rawMaxX: number,
  rawMinY: number,
  rawMaxY: number
): boolean {
  const minX = clamp(rawMinX, 0, width - 1);
  const maxX = clamp(rawMaxX, 0, width - 1);
  const minY = clamp(rawMinY, 0, height - 1);
  const maxY = clamp(rawMaxY, 0, height - 1);
  const span = Math.max(1, maxX - minX + 1);
  let consecutiveRows = 0;

  for (let y = minY; y <= maxY; y += 1) {
    let darkCount = 0;
    for (let x = minX; x <= maxX; x += 1) {
      if (dark[y * width + x]) darkCount += 1;
    }
    if (darkCount >= Math.max(4, span * 0.62)) {
      consecutiveRows += 1;
      if (consecutiveRows >= 2) return true;
    } else {
      consecutiveRows = 0;
    }
  }

  return false;
}

function overlapRatio(aMin: number, aMax: number, bMin: number, bMax: number): number {
  const overlap = Math.max(0, Math.min(aMax, bMax) - Math.max(aMin, bMin) + 1);
  const smaller = Math.max(1, Math.min(aMax - aMin + 1, bMax - bMin + 1));
  return overlap / smaller;
}

function markTextBox(mask: Uint8Array, box: FloorplanTextBox, width: number, height: number, padding: number): void {
  const minX = clamp(box.minX - padding, 0, width - 1);
  const minY = clamp(box.minY - padding, 0, height - 1);
  const maxX = clamp(box.maxX + padding, 0, width - 1);
  const maxY = clamp(box.maxY + padding, 0, height - 1);

  for (let y = minY; y <= maxY; y += 1) {
    for (let x = minX; x <= maxX; x += 1) {
      mask[y * width + x] = 1;
    }
  }
}

function textBoxesToRegions(boxes: FloorplanTextBox[], imageWidth: number, imageHeight: number): FloorplanTextRegion[] {
  return boxes
    .map((box) =>
      padRegion(
        {
          x: box.minX,
          y: box.minY,
          width: box.maxX - box.minX + 1,
          height: box.maxY - box.minY + 1,
          boxes: [box],
          pixels: box.pixels
        },
        imageWidth,
        imageHeight
      )
    )
    .filter((region) => region.width >= TEXT_REGION_MIN_WIDTH && region.height >= TEXT_REGION_MIN_HEIGHT)
    .filter((region) => region.height <= TEXT_REGION_MAX_HEIGHT)
    .sort((a, b) => b.pixels - a.pixels);
}

function padRegion(region: FloorplanTextRegion, imageWidth: number, imageHeight: number): FloorplanTextRegion {
  const x = clamp(region.x - TEXT_REGION_PADDING, 0, imageWidth - 1);
  const y = clamp(region.y - TEXT_REGION_PADDING, 0, imageHeight - 1);
  const maxX = clamp(region.x + region.width - 1 + TEXT_REGION_PADDING, 0, imageWidth - 1);
  const maxY = clamp(region.y + region.height - 1 + TEXT_REGION_PADDING, 0, imageHeight - 1);
  return {
    ...region,
    x,
    y,
    width: maxX - x + 1,
    height: maxY - y + 1
  };
}

function centerY(box: FloorplanTextBox): number {
  return (box.minY + box.maxY) / 2;
}

function centerX(box: FloorplanTextBox): number {
  return (box.minX + box.maxX) / 2;
}

export function isDarkWallLikePixel(r: number, g: number, b: number, alpha: number): boolean {
  if (alpha < 32) return false;
  const gray = (r * 77 + g * 150 + b * 29) >> 8;
  return gray < DARK_THRESHOLD;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, value));
}
