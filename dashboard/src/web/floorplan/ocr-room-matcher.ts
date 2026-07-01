import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { FloorplanOcrResult, FloorplanOcrWord } from "./floorplan-ocr";
import { normalizeLabelComparableText, normalizePartialRoomLabel, normalizeUnambiguousRoomLabel, roomKindFromLabel } from "./room-labels";

export type FloorplanOcrRoomMatch = {
  candidateId: string;
  label: string;
  score: number;
  confidence: number;
  text: string;
};

const PARTIAL_ROOM_LABEL_MIN_CONFIDENCE = 65;

export function applyOcrRoomLabels(
  candidates: RoomCandidate[],
  ocrResult: FloorplanOcrResult
): { candidates: RoomCandidate[]; matches: FloorplanOcrRoomMatch[] } {
  const matches = matchOcrRoomLabels(candidates, ocrResult);
  if (!matches.length) return { candidates, matches };
  const matchByCandidate = new Map(matches.map((match) => [match.candidateId, match]));
  return {
    matches,
    candidates: candidates.map((candidate) => {
      const match = matchByCandidate.get(candidate.id);
      if (!match || !shouldApplyLabel(candidate)) return candidate;
      return {
        ...candidate,
        name: match.label,
        kind: roomKindFromLabel(match.label) ?? candidate.kind,
        debug: {
          ...(candidate.debug ?? {}),
          reason: candidate.debug?.reason,
          ocrLabel: match.text,
          ocrScore: Math.round(match.score)
        }
      };
    })
  };
}

export function matchOcrRoomLabels(candidates: RoomCandidate[], ocrResult: FloorplanOcrResult): FloorplanOcrRoomMatch[] {
  const textItems = collectRoomTextItems(ocrResult);
  const activeCandidates = candidates.filter((candidate) => candidate.status !== "rejected");
  const matchByCandidate = new Map<string, FloorplanOcrRoomMatch>();

  for (const item of textItems) {
    if (!item.bbox) continue;
    const partialLabel = item.confidence >= PARTIAL_ROOM_LABEL_MIN_CONFIDENCE ? normalizePartialRoomLabel(item.text) : "";
    const label = partialLabel || normalizeUnambiguousRoomLabel(item.text);
    if (!label) continue;

    const center = bboxCenter(item.bbox);
    const containingCandidates = activeCandidates.filter((candidate) => pointInCandidate(candidate, center.x, center.y));
    if (!containingCandidates.length) continue;

    const candidate = chooseContainingCandidate(containingCandidates, center.x, center.y);
    const score = scoreContainedRoomLabel(candidate, item, label, center.x, center.y);
    const current = matchByCandidate.get(candidate.id);
    if (!current || score > current.score) {
      matchByCandidate.set(candidate.id, {
        candidateId: candidate.id,
        label,
        score,
        confidence: item.confidence,
        text: item.text
      });
    }
  }

  for (const candidate of activeCandidates) {
    if (matchByCandidate.has(candidate.id)) continue;
    const containedItems = textItems
      .filter((item) => {
        if (!item.bbox) return false;
        const center = bboxCenter(item.bbox);
        return pointInCandidate(candidate, center.x, center.y);
      })
      .sort(compareOcrReadingOrder);
    const combined = bestCombinedRoomLabel(candidate, containedItems);
    if (combined) {
      matchByCandidate.set(candidate.id, combined);
    }
  }

  for (const candidate of activeCandidates) {
    if (matchByCandidate.has(candidate.id)) continue;
    const containedItems = textItems
      .filter((item) => {
        if (!item.bbox || item.confidence < PARTIAL_ROOM_LABEL_MIN_CONFIDENCE) return false;
        const center = bboxCenter(item.bbox);
        return pointInCandidate(candidate, center.x, center.y);
      })
      .sort(compareOcrReadingOrder);
    const partial = bestPartialRoomLabel(candidate, containedItems);
    if (partial) {
      matchByCandidate.set(candidate.id, partial);
    }
  }

  return [...matchByCandidate.values()].sort((a, b) => b.score - a.score);
}

function bestPartialRoomLabel(candidate: RoomCandidate, items: FloorplanOcrWord[]): FloorplanOcrRoomMatch | null {
  let best: FloorplanOcrRoomMatch | null = null;
  for (const item of items) {
    if (!item.bbox) continue;
    const label = normalizePartialRoomLabel(item.text);
    if (!label) continue;
    const center = bboxCenter(item.bbox);
    const score = scoreContainedRoomLabel(candidate, item, label, center.x, center.y) - 12;
    if (!best || score > best.score) {
      best = {
        candidateId: candidate.id,
        label,
        score,
        confidence: item.confidence,
        text: item.text
      };
    }
  }
  return best;
}

function collectRoomTextItems(ocrResult: FloorplanOcrResult): FloorplanOcrWord[] {
  const items = [...ocrResult.lines, ...ocrResult.words];
  const seen = new Set<string>();
  return items.filter((item) => {
    if (!item.bbox || item.kind === "dimension") return false;
    const comparableText = normalizeLabelComparableText(item.text);
    if (!comparableText || /[0-9]/.test(comparableText)) return false;
    const text = item.text.replace(/\s+/g, "");
    const key = `${text}:${item.bbox.x0}:${item.bbox.y0}:${item.bbox.x1}:${item.bbox.y1}`;
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

function bestCombinedRoomLabel(candidate: RoomCandidate, items: FloorplanOcrWord[]): FloorplanOcrRoomMatch | null {
  if (items.length < 2) return null;
  let best: FloorplanOcrRoomMatch | null = null;
  for (let start = 0; start < items.length; start += 1) {
    for (let end = start + 2; end <= Math.min(items.length, start + 3); end += 1) {
      const group = items.slice(start, end);
      if (!group.every((item) => item.bbox)) continue;
      if (!isCompactOcrGroup(group)) continue;
      const text = group.map((item) => item.text).join("");
      const label = normalizeUnambiguousRoomLabel(text);
      if (!label) continue;
      const confidence = group.reduce((sum, item) => sum + item.confidence, 0) / group.length;
      const bbox = mergedBbox(group);
      const center = bboxCenter(bbox);
      const score = scoreContainedRoomLabel(candidate, { ...group[0], text, confidence, bbox }, label, center.x, center.y) - 6;
      if (!best || score > best.score) {
        best = {
          candidateId: candidate.id,
          label,
          score,
          confidence,
          text
        };
      }
    }
  }
  return best;
}

function isCompactOcrGroup(items: FloorplanOcrWord[]): boolean {
  const bbox = mergedBbox(items);
  const width = bbox.x1 - bbox.x0;
  const height = bbox.y1 - bbox.y0;
  const maxItemHeight = Math.max(...items.map((item) => item.bbox ? item.bbox.y1 - item.bbox.y0 : 0), 1);
  const maxItemWidth = Math.max(...items.map((item) => item.bbox ? item.bbox.x1 - item.bbox.x0 : 0), 1);
  return width <= maxItemWidth * 4.5 && height <= maxItemHeight * 3.5;
}

function mergedBbox(items: FloorplanOcrWord[]): NonNullable<FloorplanOcrWord["bbox"]> {
  const boxes = items.map((item) => item.bbox).filter(Boolean) as Array<NonNullable<FloorplanOcrWord["bbox"]>>;
  return {
    x0: Math.min(...boxes.map((bbox) => bbox.x0)),
    y0: Math.min(...boxes.map((bbox) => bbox.y0)),
    x1: Math.max(...boxes.map((bbox) => bbox.x1)),
    y1: Math.max(...boxes.map((bbox) => bbox.y1))
  };
}

function compareOcrReadingOrder(a: FloorplanOcrWord, b: FloorplanOcrWord): number {
  if (!a.bbox || !b.bbox) return 0;
  const aCenter = bboxCenter(a.bbox);
  const bCenter = bboxCenter(b.bbox);
  const rowThreshold = Math.max(a.bbox.y1 - a.bbox.y0, b.bbox.y1 - b.bbox.y0, 1) * 0.7;
  if (Math.abs(aCenter.y - bCenter.y) <= rowThreshold) return aCenter.x - bCenter.x;
  return aCenter.y - bCenter.y;
}

function shouldApplyLabel(candidate: RoomCandidate): boolean {
  const normalized = candidate.name.trim();
  return (
    Boolean(candidate.debug?.ocrLabel) ||
    !normalized ||
    /^\uBCBD\uC120 \uD6C4\uBCF4 \d+$/.test(normalized) ||
    /^\uBC29 \uD6C4\uBCF4 \d+(?:-\d+)?$/.test(normalized) ||
    /^\uBC29 \d+$/.test(normalized) ||
    /^\uC218\uB3D9 \d+$/.test(normalized)
  );
}

function chooseContainingCandidate(candidates: RoomCandidate[], x: number, y: number): RoomCandidate {
  return [...candidates].sort((a, b) => {
    const areaDiff = candidateArea(a) - candidateArea(b);
    if (Math.abs(areaDiff) > 1) return areaDiff;
    return distanceToCandidateCenter(a, x, y) - distanceToCandidateCenter(b, x, y);
  })[0];
}

function scoreContainedRoomLabel(candidate: RoomCandidate, item: FloorplanOcrWord, label: string, x: number, y: number): number {
  const confidenceScore = Math.max(0, Math.min(100, item.confidence));
  const labelScore = label.length >= 2 ? 20 : 8;
  const centerScore = Math.max(0, 20 * (1 - distanceToCandidateCenter(candidate, x, y) / Math.max(candidate.rect.width, candidate.rect.height, 1)));
  const lineBonus = item.kind === "room-label" ? 10 : 0;
  return confidenceScore + labelScore + centerScore + lineBonus;
}

function bboxCenter(bbox: NonNullable<FloorplanOcrWord["bbox"]>) {
  return {
    x: (bbox.x0 + bbox.x1) / 2,
    y: (bbox.y0 + bbox.y1) / 2
  };
}

function pointInCandidate(candidate: RoomCandidate, x: number, y: number): boolean {
  const points = candidate.shape === "polygon" && candidate.points?.length
    ? candidate.points
    : rectPoints(candidate.rect);
  return pointInPolygon(points, x, y);
}

function rectPoints(rect: RoomCandidate["rect"]): Array<[number, number]> {
  return [
    [rect.x, rect.y],
    [rect.x + rect.width, rect.y],
    [rect.x + rect.width, rect.y + rect.height],
    [rect.x, rect.y + rect.height]
  ];
}

function pointInPolygon(points: Array<[number, number]>, x: number, y: number): boolean {
  let inside = false;
  for (let i = 0, j = points.length - 1; i < points.length; j = i, i += 1) {
    const [xi, yi] = points[i];
    const [xj, yj] = points[j];
    const intersects = yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi || 1) + xi;
    if (intersects) inside = !inside;
  }
  return inside;
}

function distanceToCandidateCenter(candidate: RoomCandidate, x: number, y: number): number {
  const candidateCenter = {
    x: candidate.rect.x + candidate.rect.width / 2,
    y: candidate.rect.y + candidate.rect.height / 2
  };
  return Math.hypot(x - candidateCenter.x, y - candidateCenter.y);
}

function candidateArea(candidate: RoomCandidate): number {
  return Math.max(1, candidate.rect.width * candidate.rect.height);
}
