import type { RoomCandidate, RoomCandidateImageMeta } from "./floorplan-types";
import { sanitizeRoomCandidates } from "./room-candidate-validation";

export function createMockRoomCandidates(image: RoomCandidateImageMeta): RoomCandidate[] {
  const candidates: RoomCandidate[] = [
    {
      id: "candidate_living",
      name: "거실 후보",
      kind: "living",
      confidence: 74,
      status: "candidate",
      shape: "rect",
      rect: fromPercent(image, 30, 34, 38, 32)
    },
    {
      id: "candidate_room_1",
      name: "방 후보 1",
      kind: "room",
      confidence: 68,
      status: "candidate",
      shape: "rect",
      rect: fromPercent(image, 10, 16, 24, 24)
    },
    {
      id: "candidate_room_2",
      name: "방 후보 2",
      kind: "room",
      confidence: 61,
      status: "candidate",
      shape: "rect",
      rect: fromPercent(image, 68, 18, 22, 26)
    }
  ];

  return sanitizeRoomCandidates(candidates, image);
}

function fromPercent(
  image: RoomCandidateImageMeta,
  x: number,
  y: number,
  width: number,
  height: number
): RoomCandidate["rect"] {
  return {
    x: Math.round((image.width * x) / 100),
    y: Math.round((image.height * y) / 100),
    width: Math.round((image.width * width) / 100),
    height: Math.round((image.height * height) / 100)
  };
}
