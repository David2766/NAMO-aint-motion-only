import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

function createStorage(): Storage {
  const values = new Map<string, string>();
  return {
    get length() {
      return values.size;
    },
    clear() {
      values.clear();
    },
    getItem(key) {
      return values.get(key) ?? null;
    },
    key(index) {
      return [...values.keys()][index] ?? null;
    },
    removeItem(key) {
      values.delete(key);
    },
    setItem(key, value) {
      values.set(key, String(value));
    }
  };
}

describe("demo mock API", () => {
  beforeEach(() => {
    vi.resetModules();
    vi.stubGlobal("sessionStorage", createStorage());
    vi.stubGlobal("window", { location: { origin: "http://localhost" } });
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it("exposes the installed assist radar and its interactive tuning contract", async () => {
    const { mockApi } = await import("./mock-api");

    const state = await mockApi.getState();
    expect(state.debug?.staticRadar).toMatchObject({
      available: true,
      presence: true
    });

    await mockApi.setStaticRadarTuningSession?.(true);
    const active = await mockApi.getStaticRadarTuningStatus?.();
    expect(active).toMatchObject({
      ok: true,
      available: true,
      active: true,
      resolutionMm: 750
    });
    expect(active?.gates).toHaveLength(9);
    expect(active?.gates.some((gate) => gate.moveEnergy > 0 || gate.stillEnergy > 0)).toBe(true);

    await mockApi.setStaticRadarGateSensitivity?.(3, 72);
    expect((await mockApi.getStaticRadarTuningStatus?.())?.gates[3]?.sensitivity).toBe(72);

    await mockApi.resetStaticRadarFactoryDefaults?.();
    expect((await mockApi.getStaticRadarTuningStatus?.())?.active).toBe(false);
    await mockApi.setStaticRadarTuningSession?.(true);
    expect((await mockApi.getStaticRadarTuningStatus?.())?.gates.every((gate) => gate.sensitivity === 50)).toBe(true);

    await mockApi.setStaticRadarTuningSession?.(false);
    expect((await mockApi.getStaticRadarTuningStatus?.())?.active).toBe(false);
  });

  it("rejects an invalid assist radar gate", async () => {
    const { mockApi } = await import("./mock-api");
    await expect(mockApi.setStaticRadarGateSensitivity?.(9, 50)).rejects.toThrow("invalid static radar gate");
  });

  it("exposes a standalone platform identity without claiming pairing or fusion", async () => {
    const { mockApi } = await import("./mock-api");
    const status = await mockApi.getSystemStatus?.();

    expect(status?.platform).toEqual({
      schemaVersion: 1,
      identityReady: true,
      siteId: "site-00112233445566778899aabbccddeeff",
      nodeId: "namo-node-246f2801a2b3",
      mode: "standalone",
      maxParticipants: 7,
      registryReady: true,
      remoteParticipantCount: 0,
      connectedRemoteParticipantCount: 0,
      activeRemoteParticipantCount: 0,
      readinessReady: true,
      configuredRemoteParticipantCount: 0,
      fusionEnabled: false,
      pairingSupported: true,
      pairingEnabled: false
    });
    expect(status?.platform).not.toHaveProperty("privateKey");
    expect(status?.platform).not.toHaveProperty("certificate");
  });

  it("removes saved zones when demo storage is reset", async () => {
    const { mockApi, resetMockDeviceStorage } = await import("./mock-api");
    const initial = await mockApi.getConfig();
    await mockApi.saveConfig({
      ...initial,
      zones: [
        ...initial.zones,
        {
          id: "zone_created_after_setup",
          name: "Reset test",
          type: "detection",
          shape: "rect",
          points: [
            [0, 0],
            [100, 0],
            [100, 100],
            [0, 100]
          ]
        }
      ]
    });

    expect(sessionStorage.getItem("presence-sensor-demo-config")).not.toBeNull();
    resetMockDeviceStorage();

    expect(sessionStorage.getItem("presence-sensor-demo-config")).toBeNull();
    expect((await mockApi.getConfig()).zones.some((zone) => zone.id === "zone_created_after_setup")).toBe(false);
  });

  it("keeps a stored floorplan standalone until a mock peer is connected", async () => {
    const { mockApi } = await import("./mock-api");
    const { mockFloorplanStorageFetch } = await import("../floorplan/mock-floorplan-storage");
    const { platformFloorplanTargets } = await import("../platform-radar-display");
    const document = {
      version: 1,
      floorplanId: "mock-floorplan",
      geometryVersion: 1,
      image: {
        path: "/floorplan.webp",
        mime: "image/webp",
        width: 100,
        height: 80
      },
      scale: {
        widthMm: 3000,
        heightMm: 2400,
        outerBoundsPx: [0, 0, 100, 80] as [number, number, number, number],
        mmPerPxX: 30,
        mmPerPxY: 30
      },
      radar: {
        originPx: [50, 70] as [number, number],
        rotationDeg: 0,
        scale: 1
      },
      rooms: [
        {
          id: "mock_room",
          name: "Mock room",
          kind: "unknown" as const,
          pointsPx: [
            [5, 5],
            [95, 5],
            [95, 75],
            [5, 75]
          ] as Array<[number, number]>
        }
      ],
      occlusion: {
        ignoredEdges: []
      }
    };
    sessionStorage.setItem("presence-sensor-demo-floorplan-config", JSON.stringify(document));
    sessionStorage.setItem("presence-sensor-demo-floorplan-image", "data:image/webp;base64,bW9jaw==");
    sessionStorage.setItem("presence-sensor-demo-floorplan-image-mime", "image/webp");

    const participantsResponse = await mockFloorplanStorageFetch("/api/platform/participants");
    const siteResponse = await mockFloorplanStorageFetch("/api/platform/site-configuration");
    const participants = await participantsResponse.json();
    const site = await siteResponse.json();
    const state = await mockApi.getState();
    const status = await mockApi.getSystemStatus?.();
    const targets = platformFloorplanTargets(state, state.targets, site, document, status?.platform?.nodeId);

    expect(participantsResponse.status).toBe(200);
    expect(participants.participants).toHaveLength(0);
    expect(state.fusion?.authoritative).toBe(false);
    expect(state.targets.filter((target) => target.active)).toHaveLength(1);
    expect(targets.some((target) => target.active)).toBe(true);
  });

  it("removes a mock participant directly and bounds the simulated delay", async () => {
    const { mockPlatformRemovalDelayMs, removeMockPlatformParticipant } = await import("./mock-platform");
    const { mockFloorplanStorageFetch } = await import("../floorplan/mock-floorplan-storage");
    const { blankMockPairingSide, mockPlatformState, persistMockPlatformState } = await import("./mock-platform-state");

    mockPlatformState.peers = [
      {
        index: 1,
        membership: "active",
        leaderEligible: true,
        floorplanSynced: true
      },
      {
        index: 2,
        membership: "active",
        leaderEligible: false,
        floorplanSynced: true
      }
    ];
    mockPlatformState.pairing = {
      peerIndex: 1,
      local: blankMockPairingSide("leader"),
      peer: blankMockPairingSide("joiner")
    };
    persistMockPlatformState();

    expect(mockPlatformRemovalDelayMs(() => 0)).toBe(1000);
    expect(mockPlatformRemovalDelayMs(() => 1)).toBe(3000);
    expect(removeMockPlatformParticipant("namo-node-mock-000001")).toBe(true);
    expect(mockPlatformState.peers.map((peer) => peer.index)).toEqual([2]);
    expect(mockPlatformState.pairing).toBeNull();
    expect(JSON.parse(sessionStorage.getItem("presence-sensor-demo-platform") ?? "null").peers).toHaveLength(1);
    expect((await (await mockFloorplanStorageFetch("/api/platform/participants")).json()).participants).toHaveLength(1);
    expect((await (await mockFloorplanStorageFetch("/api/platform/site-configuration")).json()).memberCount).toBe(2);
    expect((await (await mockFloorplanStorageFetch("/api/platform/groups")).json()).site.participantsExpected).toBe(2);
    expect((await (await mockFloorplanStorageFetch("/api/platform/discovery")).json()).candidates[0]?.address).toBe(
      "0.0.0.1"
    );
    expect(removeMockPlatformParticipant("namo-node-mock-000001")).toBe(false);
  });

  it("runs the real pairing client against up to six floorplan-free mock peers", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-08-19T10:00:00Z"));
    const { mockApi } = await import("./mock-api");
    const { mockFloorplanStorageFetch } = await import("../floorplan/mock-floorplan-storage");
    const { createPlatformPairingClient } = await import("./platform-pairing");
    const floorplan = {
      version: 1,
      floorplanId: "mock-floorplan",
      geometryVersion: 1,
      image: {
        path: "/floorplan.webp",
        mime: "image/webp",
        width: 100,
        height: 80
      },
      scale: {
        widthMm: 3000,
        heightMm: 2400,
        outerBoundsPx: [0, 0, 100, 80] as [number, number, number, number],
        mmPerPxX: 30,
        mmPerPxY: 30
      },
      radar: {
        originPx: [50, 70] as [number, number],
        rotationDeg: 0,
        scale: 1,
        poseVersion: 1
      },
      rooms: [
        {
          id: "mock_room",
          name: "Mock room",
          kind: "unknown" as const,
          pointsPx: [
            [5, 5],
            [95, 5],
            [95, 75],
            [5, 75]
          ] as Array<[number, number]>
        }
      ],
      occlusion: { ignoredEdges: [] }
    };
    sessionStorage.setItem("presence-sensor-demo-floorplan-config", JSON.stringify(floorplan));
    sessionStorage.setItem("presence-sensor-demo-floorplan-image", "data:image/webp;base64,bW9jaw==");
    sessionStorage.setItem("presence-sensor-demo-floorplan-image-mime", "image/webp");
    const client = createPlatformPairingClient({
      fetcher: mockFloorplanStorageFetch,
      sleep: async () => undefined,
      pollIntervalMs: 1
    });

    expect((await client.scan("http://localhost")).candidates).toHaveLength(6);
    const peerFloorplanBefore = await mockFloorplanStorageFetch("http://0.0.0.1/api/floorplan/status");
    expect((await peerFloorplanBefore.json()).hasConfig).toBe(false);

    for (let index = 1; index <= 6; index += 1) {
      const address = `http://0.0.0.${index}`;
      const participantId = `namo-node-mock-00000${index}`;
      const assignment = {
        floorplanId: "mock-floorplan",
        spaceId: "mock_room",
        groupId: "mock_room",
        geometryVersion: 1,
        localPoseVersion: 1,
        poseVersion: 1,
        calibrationVersion: 1,
        transform: {
          originXMillimeters: 500 + index * 100,
          originYMillimeters: 700 + index * 100,
          rotationCentiDegrees: 0,
          residualMillimeters: 20,
          calibrationSamples: 2
        }
      };
      const capabilities = {
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
      await client.requestInvitation(address, "http://localhost");
      const connection = await client.connectLeader("http://localhost", address);
      await client.confirmPair("http://localhost", address, connection.confirmationCode);
      await client.syncSiteConfiguration(address, "namo-node-246f2801a2b3");
      await client.configureParticipant("http://localhost", participantId, capabilities, assignment);
      await client.configureParticipant(address, "namo-node-246f2801a2b3", capabilities, assignment);
      await client.activateParticipant("http://localhost", participantId);
      await client.activateParticipant(address, "namo-node-246f2801a2b3");
    }

    const participants = await client.getParticipants("http://localhost");
    const status = await mockApi.getSystemStatus?.();
    const syncedPeerFloorplan = await mockFloorplanStorageFetch("http://0.0.0.1/api/floorplan/status");
    const syncedPeerImage = await mockFloorplanStorageFetch("http://0.0.0.1/api/floorplan/image");
    expect(participants.participants).toHaveLength(6);
    expect(participants.participants.every((participant) => participant.membership === "active")).toBe(true);
    expect((await client.scan("http://localhost")).candidates).toHaveLength(0);
    expect(status?.platform).toMatchObject({
      maxParticipants: 7,
      remoteParticipantCount: 6,
      connectedRemoteParticipantCount: 6,
      fusionEnabled: true
    });
    expect((await syncedPeerFloorplan.json()).hasImage).toBe(true);
    expect(syncedPeerImage.status).toBe(200);
    expect(syncedPeerImage.headers.get("Content-Type")).toBe("image/webp");

    const localFloorplanDocument = await (await mockFloorplanStorageFetch("http://localhost/api/floorplan")).json();
    const peerFloorplanDocument = await (await mockFloorplanStorageFetch("http://0.0.0.1/api/floorplan")).json();
    const { pointInPolygon } = await import("../../core/geometry");
    const { buildPlatformCalibrationDisplayModel } = await import("../platform-calibration");
    const localCalibrationState = await client.getDeviceState("http://localhost");
    const peerCalibrationState = await client.getDeviceState("http://0.0.0.1");
    const calibrationModel = buildPlatformCalibrationDisplayModel(
      localFloorplanDocument,
      peerFloorplanDocument,
      localCalibrationState,
      peerCalibrationState
    );
    expect(peerFloorplanDocument.radar.originPx).not.toEqual(localFloorplanDocument.radar.originPx);
    expect(
      pointInPolygon(
        {
          x: peerFloorplanDocument.radar.originPx[0],
          y: peerFloorplanDocument.radar.originPx[1]
        },
        localFloorplanDocument.rooms[0].pointsPx
      )
    ).toBe(true);
    expect(calibrationModel.targetSeparationMillimeters).toBeGreaterThanOrEqual(100);
    expect(calibrationModel.targetSeparationMillimeters).toBeLessThanOrEqual(300);

    vi.setSystemTime(new Date("2026-08-19T10:00:08Z"));
    const movedCalibrationState = await client.getDeviceState("http://localhost");
    expect(movedCalibrationState.targets[0]).not.toMatchObject({
      x: localCalibrationState.targets[0].x,
      y: localCalibrationState.targets[0].y
    });

    vi.setSystemTime(new Date("2026-08-19T10:00:16Z"));
    const state = await mockApi.getState();
    const activeTargets = state.targets.filter((target) => target.active);
    expect(activeTargets).toHaveLength(3);
    expect(activeTargets.some((target) => target.fused === true)).toBe(true);
    expect(activeTargets.some((target) => target.fused !== true)).toBe(true);

    await client.disconnectPair(
      "http://localhost",
      "http://0.0.0.1",
      "namo-node-246f2801a2b3",
      "namo-node-mock-000001"
    );
    expect((await client.getParticipants("http://localhost")).participants).toHaveLength(5);
    expect((await client.scan("http://localhost")).candidates[0]?.address).toBe("0.0.0.1");
  });
});
