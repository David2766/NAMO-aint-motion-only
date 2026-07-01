export type RoomCandidateKind =
  | "living"
  | "room"
  | "kitchen"
  | "bath"
  | "balcony"
  | "unknown";

export type RoomCandidateStatus = "candidate" | "confirmed" | "rejected";

export interface RoomCandidate {
  id: string;
  name: string;
  kind: RoomCandidateKind;
  confidence: number;
  status: RoomCandidateStatus;
  shape: "rect" | "polygon";
  rect: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
  points?: Array<[number, number]>;
  debug?: {
    rawPoints?: number;
    simplifiedPoints?: number;
    finalPoints?: number;
    closedLoop?: boolean;
    reason?: string;
    externalRatio?: number;
    orthogonalCleanup?: boolean;
    wallTrim?: boolean;
    ocrLabel?: string;
    ocrScore?: number;
  };
}

export interface RoomCandidateImageMeta {
  width: number;
  height: number;
}

export interface FloorplanWallSegment {
  id: string;
  axis: "horizontal" | "vertical";
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  length: number;
}
