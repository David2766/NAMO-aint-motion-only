import {
  mockDescriptor,
  mockDiscoveryStatus,
  mockPairingError,
  mockPairingJson,
  mockPairingStatus,
  mutateMockPairing,
  startMockDiscovery
} from "./mock-platform-pairing";
import {
  type MockPlatformOptions,
  mockDeviceIndex,
  mockPlatformState,
  mockRequestUrl,
  removeMockPlatformParticipant,
  resetMockPlatformState
} from "./mock-platform-state";
import {
  ensureMockRadarPlacement,
  mockDeviceConfig,
  mockDeviceState,
  mockGroupsStatus,
  mockParticipantsStatus,
  mockSiteStatus,
  mutateMockParticipant,
  mutateMockSite
} from "./mock-platform-site";

export function resetMockPlatformStorage(): void {
  resetMockPlatformState();
}

export function mockConnectedPeerCount(): number {
  return mockPlatformState.peers.filter((peer) => peer.membership === "active").length;
}

export function mockRegisteredPeerCount(): number {
  return mockPlatformState.peers.filter((peer) => peer.membership !== "revoked").length;
}

export function mockPlatformRemovalDelayMs(random = Math.random): number {
  return Math.min(3000, 1000 + Math.floor(Math.max(0, random()) * 2001));
}

export { removeMockPlatformParticipant };

export function mockPlatformUsesCanonicalFloorplan(input: RequestInfo | URL): boolean {
  const index = mockDeviceIndex(input);
  return (
    index === 0 ||
    mockPlatformState.peers.some(
      (peer) => peer.index === index && peer.floorplanSynced && peer.membership !== "revoked"
    )
  );
}

export function mockPlatformRadarPlacement(
  input: RequestInfo | URL,
  document: import("../../core/floorplan/floorplan-storage").FloorplanStorageDocument
) {
  return ensureMockRadarPlacement(mockDeviceIndex(input), document);
}

export async function mockPlatformFetch(
  input: RequestInfo | URL,
  init: RequestInit | undefined,
  options: MockPlatformOptions
): Promise<Response | null> {
  const method = String(init?.method ?? "GET").toUpperCase();
  const path = mockRequestUrl(input).pathname;
  const index = mockDeviceIndex(input);
  if (index < 0) return mockPairingError(404, "participant_not_found");

  if (method === "GET" && path === "/api/platform/descriptor") {
    return mockPairingJson(mockDescriptor(index));
  }
  if (path === "/api/platform/discovery") {
    if (method === "POST") startMockDiscovery();
    return mockPairingJson(mockDiscoveryStatus());
  }
  if (path === "/api/platform/pairing") {
    if (method === "POST") return mutateMockPairing(index, await requestForm(init));
    return mockPairingJson(mockPairingStatus(index));
  }
  if (path === "/api/platform/participants") {
    if (method === "POST") return mutateMockParticipant(index, await requestForm(init), options);
    return mockPairingJson(mockParticipantsStatus(index, options));
  }
  if (method === "GET" && path === "/api/platform/groups") {
    return mockPairingJson(mockGroupsStatus());
  }
  if (path === "/api/platform/site-configuration") {
    if (method === "POST") return mutateMockSite(index, await requestForm(init), options);
    return mockPairingJson(mockSiteStatus(index, options));
  }
  if (method === "GET" && path === "/api/config") {
    return mockPairingJson(mockDeviceConfig(index, options));
  }
  if (method === "GET" && path === "/api/state") {
    return mockPairingJson(mockDeviceState(index, options));
  }
  return null;
}

async function requestForm(init: RequestInit | undefined): Promise<URLSearchParams> {
  const body = init?.body;
  if (body instanceof URLSearchParams) return body;
  if (typeof body === "string") return new URLSearchParams(body);
  if (body instanceof FormData) {
    const form = new URLSearchParams();
    body.forEach((value, key) => form.set(key, String(value)));
    return form;
  }
  return new URLSearchParams();
}
