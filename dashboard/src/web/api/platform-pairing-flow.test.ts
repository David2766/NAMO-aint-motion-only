import { describe, expect, it } from "vitest";
import {
  createPlatformPairingClient,
  type PlatformPairingStatus,
  type PlatformSiteConfigurationStatus
} from "./platform-pairing";
import {
  approvePlatformCalibration,
  type PlatformCalibrationProposal
} from "../platform-calibration";
import { runSharedFloorplanMutationAfterContinuity } from "../state/shared-floorplan-write";

function jsonResponse(body: unknown, statusCode = 200): Response {
  return new Response(JSON.stringify(body), {
    status: statusCode,
    headers: { "Content-Type": "application/json" }
  });
}

describe("platform pairing user flow", () => {
  it("runs pairing through delayed heartbeat convergence and bilateral calibration activation", async () => {
    type Device = "local" | "peer";
    const localUrl = "http://local";
    const peerUrl = "http://peer";
    const localNodeId = "namo-node-owner";
    const peerNodeId = "namo-node-peer";
    const existingNodeIds = ["namo-node-existing-a", "namo-node-existing-b"];
    const actions: string[] = [];
    const operationIds: Record<Device, number> = { local: 0, peer: 0 };
    const roles: Record<Device, "leader" | "joiner"> = {
      local: "leader",
      peer: "joiner"
    };
    const pairingStates: Record<Device, string> = {
      local: "idle",
      peer: "idle"
    };
    const lastOperations: Record<Device, string> = {
      local: "none",
      peer: "none"
    };
    const proofs: Record<Device, string | null> = { local: null, peer: null };
    let ownerGeneration = 10;
    let peerGeneration = 0;
    let configHash = 101;
    let imageHash = 202;
    let explicitSyncs = 0;
    let replicationTransfers = 0;
    let ownerConvergenceReadsRemaining = 0;
    let ownerConvergenceReads = 0;
    let calibrationStarted = false;
    let peerOnline = true;
    const heartbeatReads: Record<Device, number> = { local: 0, peer: 0 };
    const memberships: Record<Device, "quarantined" | "active"> = {
      local: "quarantined",
      peer: "active"
    };
    const directMemberships: Record<Device, "quarantined" | "active"> = {
      local: "quarantined",
      peer: "quarantined"
    };
    const participantConfigurations: Record<Device, {
      capabilities: Record<string, boolean | number>;
      assignment: Record<string, unknown>;
    } | null> = { local: null, peer: null };
    const canonicalAssignment = (side: Device): Record<string, unknown> | null => {
      const assignment = participantConfigurations[side]?.assignment;
      if (!assignment) return null;
      const transform = assignment.transform as Record<string, unknown> | undefined;
      return {
        ...assignment,
        transform: transform ? {
          ...transform,
          residualMillimeters: 0,
          calibrationSamples: 1
        } : transform
      };
    };

    const digestFor = (generation: number): string =>
      generation.toString(16).padStart(16, "0");

    const sideOf = (url: string): Device =>
      url.startsWith(localUrl) ? "local" : "peer";
    const nodeOf = (side: Device): string =>
      side === "local" ? localNodeId : peerNodeId;
    const peerOf = (side: Device): string =>
      side === "local" ? peerNodeId : localNodeId;
    const offerFor = (side: Device) => ({
      role: roles[side],
      participantId: nodeOf(side),
      siteId: side === "local" ? "site-main" : "site-peer",
      sessionId: side === "local" ? "11".repeat(16) : "22".repeat(16),
      nonce: side === "local" ? "33".repeat(32) : "44".repeat(32),
      publicKey: `04${side === "local" ? "55".repeat(64) : "66".repeat(64)}`
    });
    const pairingStatus = (side: Device): PlatformPairingStatus => ({
      ok: true,
      supported: true,
      busy: false,
      acceptedOperationId: operationIds[side],
      completedOperationId: operationIds[side],
      lastOperation: lastOperations[side],
      operationResult: "ok",
      commitResult: "ok",
      state: pairingStates[side],
      role: roles[side],
      remainingMs: pairingStates[side] === "committed" ? 0 : 180000,
      peerVerified:
        pairingStates[side] === "verified" ||
        pairingStates[side] === "committed",
      peerParticipantId:
        pairingStates[side] === "idle" ? "" : peerOf(side),
      leaderSiteId:
        pairingStates[side] === "committed" ? "site-main" : "",
      offer:
        pairingStates[side] === "idle" || pairingStates[side] === "committed"
          ? null
          : offerFor(side),
      localProof: proofs[side],
      confirmationCode: pairingStates[side] === "verified" ? "123456" : null
    } as PlatformPairingStatus);
    const siteStatus = (side: Device): PlatformSiteConfigurationStatus => {
      const generation = side === "local" ? ownerGeneration : peerGeneration;
      return {
        ok: true,
        schemaVersion: 2,
        siteId: generation > 0 ? "site-main" : "site-peer",
        authorityEpoch: 1,
        generation,
        configurationDigest: generation > 0
          ? digestFor(generation)
          : "0000000000000001",
        configurationConsistent: true,
        rosterRevision: generation > 0 ? 2 : 1,
        rosterCommitted: generation > 0,
        configurationOwnerNodeId: generation > 0 ? localNodeId : peerNodeId,
        localOwner: side === "local",
        sharedWritesAllowed: side === "local",
        memberCount: generation > 0 ? 4 : 1,
        activeMemberCount: memberships.local === "active" ? 4 : 3,
        floorplan: {
          committed: generation > 0,
          floorplanId: generation > 0 ? "fp-main" : null,
          geometryVersion: generation > 0 ? 7 : 0,
          configHash: generation > 0 ? configHash : 0,
          imageHash: generation > 0 ? imageHash : 0
        },
        members: generation > 0 ? [
          {
            participantId: localNodeId,
            participantType: "namo_node",
            trust: "official_verified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: null
          },
          ...existingNodeIds.map((participantId, index) => ({
            participantId,
            participantType: "namo_node",
            trust: "community_unverified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: {
              spaceId: "room-main",
              groupId: "room-main",
              poseVersion: index + 5,
              calibrationVersion: index + 10,
              transform: {
                originXMillimeters: 500 + index * 500,
                originYMillimeters: 500,
                rotationCentiDegrees: 0
              }
            }
          })),
          {
            participantId: peerNodeId,
            participantType: "namo_node",
            trust: "community_unverified",
            membership: memberships.local,
            leaderEligible: memberships.local === "active",
            spatialProfile: null
          }
        ] : [],
        replication: {
          supported: true,
          socketReady: true,
          state: generation === ownerGeneration ? "complete" : "idle",
          result: generation === ownerGeneration ? "ok" : "idle",
          ownerNodeId: localNodeId,
          remoteGeneration: generation,
          transferredBytes: 0,
          totalBytes: 0,
          requests: explicitSyncs,
          responses: explicitSyncs,
          retries: 0,
          rejected: 0
        }
      } as PlatformSiteConfigurationStatus;
    };

    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const parsed = new URL(url);
      const side = sideOf(url);
      const body = new URLSearchParams(String(init?.body ?? ""));
      const action = body.get("action") ?? "";
      if (init?.method === "POST")
        actions.push(`${side}:${parsed.pathname}:${action}`);

      if (parsed.pathname === "/api/platform/descriptor") {
        return jsonResponse({
          ok: true,
          protocolVersion: 1,
          participantId: nodeOf(side),
          siteId: side === "local" ? "site-main" : "site-peer",
          participantType: "namo_node",
          product: "namo-spatial-sensor",
          softwareVersion: "test",
          capabilities: {
            pointTracks: true,
            velocity: true,
            positionUncertainty: false,
            range: true,
            presence: true,
            motion: true,
            zonePresence: false,
            bridge: false,
            maxTargets: 3
          }
        });
      }
      if (parsed.pathname === "/api/platform/participants") {
        if (!calibrationStarted) return jsonResponse({ ok: true, participants: [] });
        if (init?.method === "POST") {
          if (action === "configure" || action === "reconfigure") {
            participantConfigurations[side] = {
              capabilities: {
                pointTracks: body.get("pointTracks") === "true",
                velocity: body.get("velocity") === "true",
                positionUncertainty: body.get("positionUncertainty") === "true",
                range: body.get("range") === "true",
                presence: body.get("presence") === "true",
                motion: body.get("motion") === "true",
                zonePresence: body.get("zonePresence") === "true",
                bridge: body.get("bridge") === "true",
                maxTargets: Number(body.get("maxTargets"))
              },
              assignment: {
                floorplanId: body.get("floorplanId"),
                spaceId: body.get("spaceId"),
                groupId: body.get("groupId"),
                geometryVersion: Number(body.get("geometryVersion")),
                localPoseVersion: Number(body.get("localPoseVersion")),
                poseVersion: Number(body.get("poseVersion")),
                calibrationVersion: Number(body.get("calibrationVersion")),
                transform: {
                  originXMillimeters: Number(body.get("originXMillimeters")),
                  originYMillimeters: Number(body.get("originYMillimeters")),
                  rotationCentiDegrees: Number(body.get("rotationCentiDegrees")),
                  residualMillimeters: Number(body.get("residualMillimeters")),
                  calibrationSamples: Number(body.get("calibrationSamples"))
                }
              }
            };
          }
          if (action === "configure") {
            directMemberships[side] = "quarantined";
            if (side === "local") {
              memberships.local = "quarantined";
              ownerGeneration += 1;
              ownerConvergenceReadsRemaining = 2;
            }
          } else if (action === "activate") {
            directMemberships[side] = "active";
            if (side === "local") {
              memberships.local = "active";
              ownerGeneration += 1;
              ownerConvergenceReadsRemaining = 2;
            }
          } else if (action === "quarantine") {
            directMemberships[side] = "quarantined";
            if (side === "local") {
              memberships.local = "quarantined";
              ownerGeneration += 1;
            }
          }
        }
        heartbeatReads[side] += 1;
        const senderSide: Device = side === "local" ? "peer" : "local";
        const senderGeneration = senderSide === "local" ? ownerGeneration : peerGeneration;
        const senderOnline = senderSide === "local" || peerOnline;
        const endpoint = heartbeatReads[side] >= 2 ? {
          address: senderSide === "local" ? "192.168.1.10" : "192.168.1.11",
          port: 49191,
          authenticated: true,
          online: senderOnline,
          lastSeenAgeMs: 0,
          siteConfigurationAuthorityEpoch: 1,
          siteConfigurationGeneration: senderGeneration,
          siteConfigurationDigest: digestFor(senderGeneration),
          siteDirectoryRevision: 4,
          siteDirectoryDigest: 305419896,
          floorplanGeometryVersion: 7,
          floorplanConfigHash: configHash,
          floorplanImageHash: imageHash,
          radarPoseVersion: senderSide === "local" ? 4 : 3,
          floorplanReplicaReady: true
        } : null;
        return jsonResponse({
          ok: true,
          participants: [{
            participantId: peerOf(side),
            participantType: "namo_node",
            trust: "community_unverified",
            membership: memberships[side],
            canonicalMembership: memberships[side],
            directMembership: directMemberships[side],
            canonicalMember: true,
            directPair: true,
            capabilities: participantConfigurations[side]?.capabilities ?? null,
            assignment: canonicalAssignment(side),
            directAssignment: participantConfigurations[side]?.assignment ?? null,
            credential: { provisioned: true, enabled: true, keyVersion: 1 },
            endpoint
          }]
        });
      }
      if (parsed.pathname === "/api/floorplan")
        return jsonResponse({
          floorplanId: "fp-main",
          geometryVersion: 7,
          image: { width: 1000, height: 600 },
          scale: { mmPerPxX: 10, mmPerPxY: 10 }
        });
      if (parsed.pathname === "/api/platform/site-configuration") {
        if (init?.method === "POST" && action === "sync") {
          explicitSyncs += 1;
          if (peerGeneration !== ownerGeneration) replicationTransfers += 1;
          peerGeneration = ownerGeneration;
          return jsonResponse(siteStatus("peer"));
        }
        if (init?.method === "POST" && action === "commitFloorplan") {
          ownerGeneration += 1;
          configHash += 1;
          imageHash += 1;
          return jsonResponse(siteStatus("local"));
        }
        const status = siteStatus(side);
        if (side === "local" && ownerConvergenceReadsRemaining > 0) {
          ownerConvergenceReadsRemaining -= 1;
          ownerConvergenceReads += 1;
          status.configurationConsistent = false;
          status.sharedWritesAllowed = false;
        }
        return jsonResponse(status);
      }
      if (parsed.pathname === "/api/platform/pairing") {
        if (init?.method !== "POST") return jsonResponse(pairingStatus(side));
        if (action === "invite")
          return jsonResponse({ ok: true, invitationAccepted: true });
        operationIds[side] += 1;
        lastOperations[side] = action;
        if (action === "prepare") {
          roles[side] = body.get("role") as "leader" | "joiner";
          pairingStates[side] = "prepared";
        } else if (action === "exchange") {
          pairingStates[side] = "exchanged";
          proofs[side] = side === "local" ? "local-proof" : "peer-proof";
        } else if (action === "verify") {
          pairingStates[side] = "verified";
        } else if (action === "confirm") {
          pairingStates[side] = "committed";
          if (side === "peer") {
            // Automatic replication completes before the wizard's first
            // explicit sync request reaches the joiner.
            peerGeneration = ownerGeneration;
          }
        }
        return jsonResponse({ operationId: operationIds[side] });
      }
      throw new Error(`unexpected request: ${url}`);
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await Promise.all([
      client.getSiteConfiguration(localUrl),
      client.getSiteConfiguration(peerUrl),
      client.getParticipants(peerUrl),
      client.getDescriptor(localUrl),
      client.getFloorplan(localUrl),
      client.getFloorplan(peerUrl)
    ]);
    await client.requestInvitation(peerUrl, "192.168.1.10");
    await client.prepareJoiner(peerUrl);
    const connection = await client.connectLeader(localUrl, peerUrl);
    expect(connection.confirmationCode).toBe("123456");
    await client.confirmPair(localUrl, peerUrl, connection.confirmationCode);

    await expect(client.syncSiteConfiguration(peerUrl, localNodeId))
      .resolves.toMatchObject({ generation: 10 });
    ownerConvergenceReadsRemaining = 2;
    const committed = await runSharedFloorplanMutationAfterContinuity({
      readSite: () => client.getSiteConfiguration(localUrl),
      mutation: () => client.commitFloorplanManifest(localUrl, "fp-main", 7, 10000, 6000),
      sleep: async () => undefined,
      timeoutMs: 1000,
      pollIntervalMs: 1
    });
    expect(ownerConvergenceReads).toBe(2);
    await client.syncSiteConfiguration(peerUrl, localNodeId);
    await expect(client.waitForSiteConvergence(peerUrl, {
      siteId: committed.siteId,
      authorityEpoch: committed.authorityEpoch ?? 0,
      generation: committed.generation,
      configurationDigest: committed.configurationDigest ?? "",
      configurationOwnerNodeId: localNodeId,
      floorplanId: committed.floorplan.floorplanId ?? "",
      geometryVersion: committed.floorplan.geometryVersion,
      configHash: committed.floorplan.configHash,
      imageHash: committed.floorplan.imageHash
    }, 1000)).resolves.toMatchObject({ generation: 11, localOwner: false });
    await expect(client.syncSiteConfiguration(peerUrl, localNodeId))
      .resolves.toMatchObject({ generation: 11 });

    expect(explicitSyncs).toBe(3);
    expect(replicationTransfers).toBe(1);
    expect(actions).toEqual([
      "peer:/api/platform/pairing:invite",
      "peer:/api/platform/participants:open",
      "peer:/api/platform/pairing:prepare",
      "local:/api/platform/participants:open",
      "local:/api/platform/pairing:prepare",
      "local:/api/platform/pairing:exchange",
      "peer:/api/platform/pairing:exchange",
      "local:/api/platform/pairing:verify",
      "peer:/api/platform/pairing:verify",
      "local:/api/platform/pairing:confirm",
      "peer:/api/platform/pairing:confirm",
      "peer:/api/platform/site-configuration:sync",
      "local:/api/platform/site-configuration:commitFloorplan",
      "peer:/api/platform/site-configuration:sync",
      "peer:/api/platform/site-configuration:sync"
    ]);

    calibrationStarted = true;
    const [referenceDescriptor, peerDescriptor] = await Promise.all([
      client.getDescriptor(localUrl),
      client.getDescriptor(peerUrl)
    ]);
    const referenceAssignment = {
      floorplanId: "fp-main",
      spaceId: "room-main",
      groupId: "room-main",
      geometryVersion: 7,
      localPoseVersion: 3,
      poseVersion: 4,
      calibrationVersion: 1,
      transform: {
        originXMillimeters: 1000,
        originYMillimeters: 800,
        rotationCentiDegrees: 0,
        residualMillimeters: 20,
        calibrationSamples: 2
      }
    };
    const peerAssignment = {
      ...referenceAssignment,
      localPoseVersion: 4,
      poseVersion: 3
    };
    const proposal: PlatformCalibrationProposal = {
      samples: 2,
      floorplanId: "fp-main",
      geometryVersion: 7,
      spaceId: "room-main",
      groupId: "room-main",
      translationMillimeters: 20,
      rotationCorrectionDegrees: 0,
      residualMillimeters: 20,
      referenceAssignment,
      peerAssignment
    };

    await expect(approvePlatformCalibration(
      client,
      localUrl,
      peerUrl,
      referenceDescriptor,
      peerDescriptor,
      proposal
    )).resolves.toBeUndefined();

    expect(heartbeatReads.local).toBeGreaterThanOrEqual(5);
    expect(heartbeatReads.peer).toBeGreaterThanOrEqual(5);
    expect(ownerConvergenceReads).toBe(6);
    expect(memberships).toEqual({ local: "active", peer: "active" });
    expect(directMemberships).toEqual({ local: "active", peer: "active" });
    expect(ownerGeneration).toBe(13);
    expect(peerGeneration).toBe(13);
    expect(actions.slice(-7)).toEqual([
      "peer:/api/platform/site-configuration:sync",
      "peer:/api/platform/participants:configure",
      "local:/api/platform/participants:configure",
      "peer:/api/platform/site-configuration:sync",
      "peer:/api/platform/participants:activate",
      "local:/api/platform/participants:activate",
      "peer:/api/platform/site-configuration:sync"
    ]);

    peerOnline = false;
    const offlineView = await client.getParticipants(localUrl);
    expect(offlineView.participants[0]).toMatchObject({
      membership: "active",
      endpoint: { online: false }
    });
    await expect(client.waitForParticipantEndpoint(localUrl, peerNodeId, 0))
      .rejects.toBeDefined();

    peerOnline = true;
    const rejoined = await client.waitForParticipantEndpoint(
      localUrl,
      peerNodeId,
      1000
    );
    expect(rejoined).toMatchObject({
      membership: "active",
      assignment: {
        floorplanId: "fp-main",
        groupId: "room-main"
      },
      endpoint: { online: true, authenticated: true }
    });
    expect(ownerGeneration).toBe(13);
    expect(peerGeneration).toBe(13);
  });
});
