import type { RoomCandidate, RoomCandidateImageMeta } from "./floorplan-types";

const MIN_ROOM_SIDE_PX = 24;

export function sanitizeRoomCandidates(
  candidates: RoomCandidate[],
  image: RoomCandidateImageMeta
): RoomCandidate[] {
  return candidates
    .map((candidate) => sanitizeRoomCandidate(candidate, image))
    .filter((candidate): candidate is RoomCandidate => Boolean(candidate));
}

export function sanitizeRoomCandidate(
  candidate: RoomCandidate,
  image: RoomCandidateImageMeta
): RoomCandidate | null {
  const x = clamp(Math.round(candidate.rect.x), 0, image.width);
  const y = clamp(Math.round(candidate.rect.y), 0, image.height);
  const maxWidth = image.width - x;
  const maxHeight = image.height - y;
  const width = clamp(Math.round(candidate.rect.width), 0, maxWidth);
  const height = clamp(Math.round(candidate.rect.height), 0, maxHeight);

  if (width < MIN_ROOM_SIDE_PX || height < MIN_ROOM_SIDE_PX) return null;

  return {
    ...candidate,
    confidence: clamp(Math.round(candidate.confidence), 0, 100),
    rect: { x, y, width, height }
  };
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, value));
}
