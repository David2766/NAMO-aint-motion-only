import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type {
  PlatformParticipantAssignment,
  PlatformParticipantCapabilities,
  PlatformPairingRole,
  PlatformPairingState
} from "./platform-pairing";

const STORAGE_KEY = "presence-sensor-demo-platform";

export const MOCK_LOCAL_NODE_ID = "namo-node-246f2801a2b3";
export const MOCK_SITE_ID = "site-00112233445566778899aabbccddeeff";
export const MOCK_MAX_PEERS = 6;

export type MockFloorplanSnapshot = {
  configured: boolean;
  document: FloorplanStorageDocument | null;
  configHash: number;
  imageHash: number;
};

export type MockPeer = {
  index: number;
  membership: "quarantined" | "active" | "revoked";
  leaderEligible: boolean;
  floorplanSynced: boolean;
  radarPlacement?: {
    originPx: [number, number];
    rotationDeg: number;
  };
  capabilities?: PlatformParticipantCapabilities;
  assignment?: PlatformParticipantAssignment;
};

export type MockPairingSide = {
  state: PlatformPairingState;
  role: PlatformPairingRole;
  acceptedOperationId: number;
  completedOperationId: number;
  lastOperation: string;
  operationResult: string;
  commitResult: string;
  localProof: string | null;
  confirmationCode: string | null;
};

export type MockPlatformState = {
  generation: number;
  nextOperationId: number;
  discoveryGeneration: number;
  peers: MockPeer[];
  pairing: {
    peerIndex: number;
    local: MockPairingSide;
    peer: MockPairingSide;
  } | null;
  localCapabilities?: PlatformParticipantCapabilities;
  localAssignment?: PlatformParticipantAssignment;
};

export type MockPlatformOptions = {
  floorplan: (device: string) => MockFloorplanSnapshot;
};

export const mockDefaultCapabilities: PlatformParticipantCapabilities = {
  pointTracks: true,
  velocity: false,
  positionUncertainty: false,
  range: true,
  presence: true,
  motion: true,
  zonePresence: true,
  bridge: false,
  maxTargets: 3
};

export const mockPlatformState = loadState();

export function resetMockPlatformState(): void {
  sessionStorage.removeItem(STORAGE_KEY);
  Object.assign(mockPlatformState, createState());
}

export function persistMockPlatformState(): void {
  sessionStorage.setItem(STORAGE_KEY, JSON.stringify(mockPlatformState));
}

export function mockPeerId(index: number): string {
  return `namo-node-mock-00000${index}`;
}

export function mockPeerAddress(index: number): string {
  return `0.0.0.${index}`;
}

export function mockParticipantId(index: number): string {
  return index === 0 ? MOCK_LOCAL_NODE_ID : mockPeerId(index);
}

export function removeMockPlatformParticipant(participantId: string): boolean {
  const peer = mockPlatformState.peers.find((candidate) => mockPeerId(candidate.index) === participantId);
  if (!peer) return false;

  mockPlatformState.peers = mockPlatformState.peers.filter((candidate) => candidate.index !== peer.index);
  if (mockPlatformState.pairing?.peerIndex === peer.index) {
    mockPlatformState.pairing = null;
  }
  mockPlatformState.generation += 1;
  persistMockPlatformState();
  return true;
}

export function mockRequestUrl(input: RequestInfo | URL): URL {
  const raw = input instanceof Request ? input.url : String(input);
  return new URL(raw, window.location.origin);
}

export function mockDeviceIndex(input: RequestInfo | URL): number {
  const hostname = mockRequestUrl(input).hostname;
  const match = hostname.match(/^0\.0\.0\.([1-6])$/u);
  if (match) return Number(match[1]);
  return hostname.startsWith("0.0.0.") ? -1 : 0;
}

export function blankMockPairingSide(role: PlatformPairingRole): MockPairingSide {
  return {
    state: "idle",
    role,
    acceptedOperationId: 0,
    completedOperationId: 0,
    lastOperation: "none",
    operationResult: "ok",
    commitResult: "idle",
    localProof: null,
    confirmationCode: null
  };
}

export function ensureMockPairing(peerIndex: number) {
  if (!mockPlatformState.pairing || mockPlatformState.pairing.peerIndex !== peerIndex) {
    mockPlatformState.pairing = {
      peerIndex,
      local: blankMockPairingSide("leader"),
      peer: blankMockPairingSide("joiner")
    };
  }
  return mockPlatformState.pairing;
}

function createState(): MockPlatformState {
  return {
    generation: 1,
    nextOperationId: 1,
    discoveryGeneration: 1,
    peers: [],
    pairing: null
  };
}

function loadState(): MockPlatformState {
  try {
    const parsed = JSON.parse(sessionStorage.getItem(STORAGE_KEY) ?? "null") as MockPlatformState | null;
    if (parsed && Array.isArray(parsed.peers)) return parsed;
  } catch {
    // A malformed demo snapshot is equivalent to a fresh mock device.
  }
  return createState();
}
