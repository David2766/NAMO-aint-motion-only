import type {
  PlatformPairingOffer,
  PlatformPairingRole,
} from "./platform-pairing";
import {
  MOCK_MAX_PEERS,
  MOCK_SITE_ID,
  blankMockPairingSide,
  ensureMockPairing,
  mockDefaultCapabilities,
  mockParticipantId,
  mockPeerAddress,
  mockPeerId,
  mockPlatformState,
  persistMockPlatformState,
} from "./mock-platform-state";

const CONFIRMATION_CODE = "482731";

export function mockDescriptor(index: number) {
  return {
    protocolVersion: 1,
    participantId: mockParticipantId(index),
    participantType: "namo_node",
    product: "namo-spatial-sensor",
    softwareVersion: "mock",
    capabilities: mockDefaultCapabilities,
  };
}

export function mockDiscoveryStatus() {
  const occupied = new Set(mockPlatformState.peers.map((peer) => peer.index));
  return {
    ok: true,
    supported: true,
    scanning: false,
    generation: mockPlatformState.discoveryGeneration,
    result: "ok",
    candidates: Array.from(
      { length: MOCK_MAX_PEERS },
      (_, offset) => offset + 1,
    )
      .filter((index) => !occupied.has(index))
      .map((index) => ({
        participantId: mockPeerId(index),
        participantType: "namo_node",
        product: "namo-spatial-sensor",
        address: mockPeerAddress(index),
        hostname: `presence-sensor-mock${index}`,
        port: 80,
        protocolMinimum: 1,
        protocolMaximum: 1,
      })),
  };
}

export function startMockDiscovery(): void {
  mockPlatformState.discoveryGeneration += 1;
  persistMockPlatformState();
}

export function mockPairingStatus(index: number) {
  const side =
    pairingSide(index) ??
    blankMockPairingSide(index === 0 ? "leader" : "joiner");
  const session = mockPlatformState.pairing;
  const peer = session ? (index === 0 ? session.peerIndex : 0) : 0;
  return {
    ok: true,
    supported: true,
    busy: false,
    acceptedOperationId: side.acceptedOperationId,
    completedOperationId: side.completedOperationId,
    lastOperation: side.lastOperation,
    operationResult: side.operationResult,
    commitResult: side.commitResult,
    state: side.state,
    role: side.role,
    remainingMs:
      side.state === "committed" || side.state === "idle" ? 0 : 180000,
    peerVerified: side.state === "verified" || side.state === "committed",
    peerParticipantId: peer ? mockParticipantId(peer) : "",
    leaderSiteId: side.state === "committed" ? MOCK_SITE_ID : "",
    offer: side.state === "prepared" ? pairingOffer(index, side.role) : null,
    localProof: side.localProof,
    confirmationCode: side.confirmationCode,
    invitation: null,
  };
}

export function mutateMockPairing(
  index: number,
  form: URLSearchParams,
): Response {
  const action = String(form.get("action") ?? "");
  if (action === "invite") {
    if (index < 1 || index > MOCK_MAX_PEERS)
      return mockPairingError(404, "participant_not_found");
    const session = ensureMockPairing(index);
    session.peer.state = "prepared";
    session.peer.role = "joiner";
    persistMockPlatformState();
    return mockPairingJson({ ok: true });
  }
  if (action === "rejectInvite" || action === "dismissInvite") {
    mockPlatformState.pairing = null;
    persistMockPlatformState();
    return mockPairingJson({ ok: true });
  }

  const peerIndex =
    index === 0 ? (mockPlatformState.pairing?.peerIndex ?? 1) : index;
  const session = ensureMockPairing(peerIndex);
  const side = index === 0 ? session.local : session.peer;
  const operationId = mockPlatformState.nextOperationId++;
  side.acceptedOperationId = operationId;
  side.completedOperationId = operationId;
  side.lastOperation = action;
  side.operationResult = "ok";

  if (action === "prepare") {
    side.role = form.get("role") === "joiner" ? "joiner" : "leader";
    side.state = "prepared";
  } else if (action === "exchange") {
    side.state = "exchanged";
    side.localProof = `mock-proof-${index}`;
  } else if (action === "verify") {
    side.state = "verified";
    side.confirmationCode = CONFIRMATION_CODE;
  } else if (action === "confirm") {
    side.state = "committed";
    side.commitResult = "ok";
    let peer = mockPlatformState.peers.find(
      (candidate) => candidate.index === peerIndex,
    );
    if (!peer) {
      peer = {
        index: peerIndex,
        membership: "quarantined",
        leaderEligible: false,
        floorplanSynced: false,
      };
      mockPlatformState.peers.push(peer);
      mockPlatformState.generation += 1;
    } else if (peer.membership === "revoked") {
      peer.membership = "quarantined";
    }
  } else if (action === "cancel") {
    mockPlatformState.pairing = null;
  }
  persistMockPlatformState();
  return mockPairingJson({ operationId });
}

function pairingSide(index: number) {
  if (!mockPlatformState.pairing) return null;
  return index === 0
    ? mockPlatformState.pairing.local
    : index === mockPlatformState.pairing.peerIndex
      ? mockPlatformState.pairing.peer
      : null;
}

function pairingOffer(
  index: number,
  role: PlatformPairingRole,
): PlatformPairingOffer {
  return {
    role,
    participantId: mockParticipantId(index),
    siteId: index === 0 ? MOCK_SITE_ID : `site-mock-${index}`,
    sessionId: `mock-session-${mockPlatformState.pairing?.peerIndex ?? index}`,
    nonce: `mock-nonce-${index}`,
    publicKey: "11".repeat(32),
  };
}

export function mockPairingJson(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
}

export function mockPairingError(status: number, code: string): Response {
  return new Response(
    JSON.stringify({
      ok: false,
      error: code,
      errorInfo: {
        code,
        severity: status >= 500 ? "error" : "warning",
        detail: {},
      },
    }),
    {
      status,
      headers: { "Content-Type": "application/json" },
    },
  );
}
