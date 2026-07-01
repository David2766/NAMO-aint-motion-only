import type { RoomCandidateKind } from "../../core/floorplan/floorplan-types";

export type RoomLabelEntry = {
  label: string;
  kind: RoomCandidateKind;
  aliases: string[];
};

export const ROOM_LABELS: RoomLabelEntry[] = [
  { label: "거실", kind: "living", aliases: ["거실", "리빙", "living"] },
  { label: "침실", kind: "room", aliases: ["침실", "안방", "작은방", "큰방", "자녀방", "침실1", "침실2", "침실3", "방", "룸", "room"] },
  { label: "주방/식당", kind: "kitchen", aliases: ["주방/식당", "주방식당", "주방및식당", "주방", "식당", "다이닝", "dining"] },
  { label: "욕실", kind: "bath", aliases: ["욕실", "화장실", "공용욕실", "부부욕실", "욕", "bath"] },
  { label: "발코니", kind: "balcony", aliases: ["발코니", "베란다", "실외기실"] },
  { label: "대피공간", kind: "balcony", aliases: ["대피공간"] },
  { label: "테라스", kind: "balcony", aliases: ["테라스"] },
  { label: "현관", kind: "unknown", aliases: ["현관"] },
  { label: "창고", kind: "unknown", aliases: ["창고", "수납"] },
  { label: "다용도실", kind: "unknown", aliases: ["다용도실", "다용도", "세탁실"] },
  { label: "드레스룸", kind: "unknown", aliases: ["드레스룸", "드레스"] },
  { label: "파우더룸", kind: "unknown", aliases: ["파우더룸", "파우더", "파우더실"] },
  { label: "A/C룸", kind: "unknown", aliases: ["A/C룸", "AC룸", "A C룸", "에이씨룸", "에어컨실"] },
  { label: "팬트리", kind: "unknown", aliases: ["팬트리", "펜트리"] },
  { label: "알파룸", kind: "unknown", aliases: ["알파룸"] },
  { label: "복도", kind: "unknown", aliases: ["복도"] }
];

const ROOM_LABEL_PARTIAL_ALIASES: RoomLabelEntry[] = [
  { label: "드레스룸", kind: "unknown", aliases: ["드레", "스룸", "레스룸"] },
  { label: "파우더룸", kind: "unknown", aliases: ["파우", "더룸"] },
  { label: "다용도실", kind: "unknown", aliases: ["다용", "도실"] },
  { label: "대피공간", kind: "balcony", aliases: ["대피"] }
];

export function normalizeLabelComparableText(text: string): string {
  return text
    .toLowerCase()
    .replace(/[\s/ㆍ·.,_-]+/g, "");
}

export function normalizeRoomLabel(text: string): string {
  const normalized = normalizeLabelComparableText(text);
  for (const entry of ROOM_LABELS) {
    if (entry.aliases.some((alias) => normalized.includes(normalizeLabelComparableText(alias)))) return entry.label;
  }
  return "";
}

export function normalizeExactRoomLabel(text: string): string {
  const normalized = normalizeLabelComparableText(text);
  for (const entry of ROOM_LABELS) {
    if (entry.aliases.some((alias) => normalized === normalizeLabelComparableText(alias))) return entry.label;
  }
  return "";
}

export function normalizeUnambiguousRoomLabel(text: string): string {
  const exact = normalizeExactRoomLabel(text);
  if (exact) return exact;

  const normalized = normalizeLabelComparableText(text);
  const matches = new Set<string>();
  for (const entry of ROOM_LABELS) {
    if (entry.aliases.some((alias) => normalized.includes(normalizeLabelComparableText(alias)))) {
      matches.add(entry.label);
    }
  }
  return matches.size === 1 ? [...matches][0] : "";
}

export function normalizePartialRoomLabel(text: string): string {
  const normalized = normalizeLabelComparableText(text);
  if (normalized.length < 2) return "";
  for (const entry of ROOM_LABEL_PARTIAL_ALIASES) {
    if (entry.aliases.some((alias) => normalized === normalizeLabelComparableText(alias))) return entry.label;
  }
  return "";
}

export function roomKindFromLabel(label: string): RoomCandidateKind | null {
  return ROOM_LABELS.find((entry) => entry.label === label)?.kind ?? null;
}
