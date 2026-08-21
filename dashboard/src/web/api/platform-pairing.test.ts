import { describe, expect, it } from "vitest";
import {
  availablePairingCandidates,
  communityFallbackControlAvailable,
  createPlatformPairingClient,
  onlineLeaderEligibleCount,
  shouldOfferCommunityFallbackPreparation,
  pairingCalibrationAvailability,
  participantIsOnline,
  participantIsRegistered,
  participantOnlineAddress,
  pairingFloorplanUsage,
  pairingConnectionGuard,
  pairingDisplay,
  pairingKeepsCanonicalFloorplan,
  pairingPeerRelationship,
  pairingSetupRelationship,
  pairingSetupResumeAvailability,
  pairingReturnFlowActive,
  rememberDiscoveryCandidates,
  type PlatformPairingStatus,
  type PlatformParticipantsStatus,
  type PlatformSiteConfigurationStatus
} from "./platform-pairing";

function status(overrides: Partial<PlatformPairingStatus> = {}): PlatformPairingStatus {
  return {
    ok: true,
    supported: true,
    busy: false,
    acceptedOperationId: 1,
    completedOperationId: 1,
    lastOperation: "prepare",
    operationResult: "ok",
    commitResult: "ok",
    state: "prepared",
    role: "joiner",
    remainingMs: 180000,
    peerVerified: false,
    peerParticipantId: "",
    leaderSiteId: "",
    offer: {
      role: "joiner",
      participantId: "namo-node-peer",
      siteId: "site-peer",
      sessionId: "11".repeat(16),
      nonce: "22".repeat(32),
      publicKey: `04${"33".repeat(64)}`
    },
    localProof: null,
    confirmationCode: null,
    ...overrides
  };
}

function jsonResponse(body: unknown, statusCode = 200): Response {
  return new Response(JSON.stringify(body), {
    status: statusCode,
    headers: { "Content-Type": "application/json" }
  });
}

function calibrationSite(): PlatformSiteConfigurationStatus {
  return {
    ok: true,
    schemaVersion: 2,
    siteId: "site-main",
    authorityEpoch: 2,
    generation: 7,
    configurationDigest: "0123456789abcdef",
    configurationConsistent: true,
    rosterRevision: 4,
    rosterCommitted: true,
    configurationOwnerNodeId: "namo-node-owner",
    localOwner: true,
    sharedWritesAllowed: true,
    memberCount: 2,
    activeMemberCount: 1,
    floorplan: {
      committed: true,
      floorplanId: "fp-main",
      geometryVersion: 3,
      configHash: 101,
      imageHash: 202
    },
    replication: {
      supported: true,
      socketReady: true,
      state: "complete",
      result: "ok",
      ownerNodeId: "namo-node-owner",
      remoteGeneration: 7,
      transferredBytes: 0,
      totalBytes: 0,
      requests: 0,
      responses: 0,
      retries: 0,
      rejected: 0
    }
  };
}

describe("platform pairing client", () => {
  it("describes floorplan sharing from real storage and connected-device state", () => {
    expect(pairingFloorplanUsage(false, 0, true, "namo-node-local")).toBe("none");
    expect(pairingFloorplanUsage(true, 0, true, "namo-node-local")).toBe("local");
    expect(pairingFloorplanUsage(true, 1, true, "namo-node-local")).toBe("shared");
    expect(pairingFloorplanUsage(true, 1, false, "namo-node-peer")).toBe("received");
  });

  it("sends only a bounded prompt when requesting an incoming connection", async () => {
    const requests: Array<{ url: string; body: string }> = [];
    const fetcher: typeof fetch = async (input, init) => {
      requests.push({ url: String(input), body: String(init?.body ?? "") });
      return jsonResponse({ ok: true, invitationAccepted: true });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.requestInvitation("http://peer", "192.168.1.40");

    expect(requests).toEqual([{
      url: "http://peer/api/platform/pairing",
      body: "action=invite&requesterAddress=192.168.1.40&durationSeconds=180"
    }]);
  });

  it("keeps the last discovered address when a later snapshot is temporarily empty", () => {
    const remembered = [{
      participantId: "namo-node-peer",
      participantType: "namo_node" as const,
      product: "namo-spatial-sensor" as const,
      address: "192.168.1.50",
      hostname: "namo-peer",
      port: 80,
      protocolMinimum: 1,
      protocolMaximum: 1
    }];

    expect(rememberDiscoveryCandidates(remembered, [])).toBe(remembered);
  });

  it("updates remembered discovery metadata by stable participant id", () => {
    const remembered = [{
      participantId: "namo-node-peer",
      participantType: "namo_node" as const,
      product: "namo-spatial-sensor" as const,
      address: "192.168.1.50",
      hostname: "old-hostname",
      port: 80,
      protocolMinimum: 1,
      protocolMaximum: 1
    }];
    const discovered = [{
      ...remembered[0],
      address: "192.168.1.73",
      hostname: "presence-sensor-peer"
    }];

    expect(rememberDiscoveryCandidates(remembered, discovered)).toEqual(discovered);
  });

  it("does not offer an already registered node as a new pairing candidate", () => {
    const candidate = {
      participantId: "namo-node-peer",
      participantType: "namo_node" as const,
      product: "namo-spatial-sensor" as const,
      address: "192.168.1.50",
      hostname: "namo-peer",
      port: 80,
      protocolMinimum: 1,
      protocolMaximum: 1
    };
    const participants: PlatformParticipantsStatus["participants"] = [{
      participantId: candidate.participantId,
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      credential: { provisioned: true, enabled: false, keyVersion: 1 }
    }];

    expect(availablePairingCandidates([candidate], participants)).toEqual([]);
    expect(participantIsRegistered(participants, candidate.participantId)).toBe(true);
  });

  it("keeps a canonical Site member registered after Owner handover without a direct pair", () => {
    const candidate = {
      participantId: "namo-node-canonical",
      participantType: "namo_node" as const,
      product: "namo-spatial-sensor" as const,
      address: "192.168.1.51",
      hostname: "namo-canonical",
      port: 80,
      protocolMinimum: 1,
      protocolMaximum: 1
    };
    const participants: PlatformParticipantsStatus["participants"] = [{
      participantId: candidate.participantId,
      participantType: "namo_node",
      trust: "official_verified",
      membership: "active",
      canonicalMember: true,
      directPair: false,
      credential: { provisioned: false, enabled: false, keyVersion: 0 }
    }];

    expect(participantIsRegistered(participants, candidate.participantId)).toBe(true);
    expect(availablePairingCandidates([candidate], participants)).toEqual([]);
    expect(pairingPeerRelationship({ ok: true, participants }, candidate.participantId))
      .toBe("managed");
  });

  it("distinguishes a resumable reciprocal quarantine from another Site", () => {
    const resumable: PlatformParticipantsStatus = {
      ok: true,
      participants: [{
        participantId: "namo-node-local",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }]
    };
    expect(pairingPeerRelationship(resumable, "namo-node-local")).toBe("resumable");
    resumable.participants[0].credential.enabled = true;
    expect(pairingPeerRelationship(resumable, "namo-node-local")).toBe("resumable");
    resumable.participants.push({
      participantId: "namo-node-revoked",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "revoked",
      credential: { provisioned: true, enabled: false, keyVersion: 1 }
    });
    expect(pairingPeerRelationship(resumable, "namo-node-local")).toBe("resumable");
    resumable.participants[0].membership = "active";
    expect(pairingPeerRelationship(resumable, "namo-node-local")).toBe("managed");
    resumable.participants = [];
    expect(pairingPeerRelationship(resumable, "namo-node-local")).toBe("available");
  });

  it("routes only a reciprocal quarantined relationship through setup resume", () => {
    const participant = (
      participantId: string,
      membership: "active" | "quarantined" | "revoked"
    ) => ({
      participantId,
      participantType: "namo_node" as const,
      trust: "community_unverified" as const,
      membership,
      credential: {
        provisioned: true,
        enabled: membership !== "revoked",
        keyVersion: 1
      }
    });
    const owner = {
      ok: true,
      participants: [
        participant("namo-node-peer", "quarantined"),
        participant("namo-node-other", "active")
      ]
    };
    const peer = {
      ok: true,
      participants: [participant("namo-node-owner", "quarantined")]
    };

    expect(pairingSetupRelationship(
      owner,
      peer,
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("resume");
    expect(pairingSetupRelationship(
      { ok: true, participants: [] },
      peer,
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("blocked");
    expect(pairingSetupRelationship(
      owner,
      { ok: true, participants: [] },
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("blocked");
    expect(pairingSetupRelationship(
      { ok: true, participants: [] },
      { ok: true, participants: [] },
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("new");
  });

  it("resumes a direct quarantine when the joining peer also projects the active Site", () => {
    const projected = (
      participantId: string,
      canonicalMembership: "active" | "quarantined",
      directPair: boolean,
      directMembership: "active" | "quarantined" | null
    ) => ({
      participantId,
      participantType: "namo_node",
      trust: "official_verified",
      membership: canonicalMembership,
      canonicalMembership,
      directMembership,
      canonicalMember: true,
      directPair,
      credential: {
        provisioned: directPair,
        enabled: directMembership === "active",
        keyVersion: directPair ? 1 : 0
      }
    });
    const owner: PlatformParticipantsStatus = {
      ok: true,
      participants: [
        projected("namo-node-peer", "quarantined", true, "quarantined"),
        projected("namo-node-existing", "active", false, null)
      ]
    };
    const joiningPeer: PlatformParticipantsStatus = {
      ok: true,
      participants: [
        projected("namo-node-owner", "active", true, "quarantined"),
        projected("namo-node-site-leader", "active", false, null),
        projected("namo-node-existing", "active", false, null)
      ]
    };

    expect(pairingPeerRelationship(joiningPeer, "namo-node-owner"))
      .toBe("resumable");
    expect(pairingSetupRelationship(
      owner,
      joiningPeer,
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("resume");

    joiningPeer.participants.push({
      ...projected("namo-node-other-direct", "active", true, "active")
    });
    expect(pairingSetupRelationship(
      owner,
      joiningPeer,
      "namo-node-owner",
      "namo-node-peer"
    )).toBe("blocked");
  });

  it("keeps an established Site owner floorplan canonical", () => {
    const site = {
      localOwner: true,
      rosterCommitted: true,
      memberCount: 2,
      floorplan: { committed: true, floorplanId: "fp-main" }
    } as PlatformSiteConfigurationStatus;
    expect(pairingKeepsCanonicalFloorplan(site)).toBe(true);
    expect(pairingKeepsCanonicalFloorplan({ ...site, memberCount: 1 })).toBe(false);
    expect(pairingKeepsCanonicalFloorplan({ ...site, localOwner: false })).toBe(false);
  });

  it("redirects connection setup to the online configuration owner", () => {
    const site = {
      localOwner: false,
      configurationOwnerNodeId: "namo-node-owner"
    } as PlatformSiteConfigurationStatus;
    const participants: PlatformParticipantsStatus = {
      ok: true,
      participants: [{
        participantId: "namo-node-owner",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        credential: { provisioned: true, enabled: true, keyVersion: 1 },
        endpoint: {
          address: "192.168.1.104",
          port: 49191,
          authenticated: true,
          online: true,
          lastSeenAgeMs: 100
        }
      }]
    };
    expect(pairingConnectionGuard(site, participants)).toEqual({
      allowed: false,
      reason: "owner",
      ownerAddress: "192.168.1.104"
    });
    participants.participants[0].endpoint!.online = false;
    expect(pairingConnectionGuard(site, participants).ownerAddress).toBe("");
  });

  it("blocks setup while canonical Site writes are converging", () => {
    const site = {
      localOwner: true,
      configurationConsistent: false,
      sharedWritesAllowed: false
    } as PlatformSiteConfigurationStatus;
    expect(pairingConnectionGuard(site, { ok: true, participants: [] })).toEqual({
      allowed: false,
      reason: "continuity",
      ownerAddress: ""
    });
    expect(pairingConnectionGuard({
      ...site,
      configurationConsistent: true,
      sharedWritesAllowed: true
    }, { ok: true, participants: [] }).allowed).toBe(true);
  });

  it("only exposes a heartbeat address while the participant is online", () => {
    const participant: PlatformParticipantsStatus["participants"][number] = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      credential: { provisioned: true, enabled: false, keyVersion: 1 },
      endpoint: {
        address: "192.168.1.73",
        port: 49191,
        authenticated: true,
        online: true,
        lastSeenAgeMs: 250
      }
    };

    expect(participantIsOnline(participant)).toBe(true);
    expect(participantOnlineAddress(participant)).toBe("192.168.1.73");

    participant.endpoint = { ...participant.endpoint!, online: false, lastSeenAgeMs: 30000 };
    expect(participantIsOnline(participant)).toBe(false);
    expect(participantOnlineAddress(participant)).toBe("");
  });

  it("treats a participant without heartbeat endpoint data as offline", () => {
    const participant: PlatformParticipantsStatus["participants"][number] = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    };

    expect(participantIsOnline(participant)).toBe(false);
    expect(participantOnlineAddress(participant)).toBe("");
  });

  it("keeps deferred calibration visible while bootstrap heartbeat converges", () => {
    const site = calibrationSite();
    const participant: PlatformParticipantsStatus["participants"][number] = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      credential: { provisioned: true, enabled: true, keyVersion: 1 },
      endpoint: {
        address: "192.168.1.73",
        port: 49191,
        authenticated: false,
        online: false,
        lastSeenAgeMs: 0,
        floorplanReplicaReady: false
      }
    };

    expect(pairingCalibrationAvailability(site, participant)).toBe("waiting");
    participant.endpoint = {
      address: "192.168.1.73",
      port: 49191,
      authenticated: true,
      online: true,
      lastSeenAgeMs: 0,
      siteConfigurationAuthorityEpoch: 2,
      siteConfigurationGeneration: 7,
      siteConfigurationDigest: "0123456789abcdef",
      siteDirectoryRevision: 4,
      siteDirectoryDigest: 305419896,
      floorplanGeometryVersion: 3,
      floorplanConfigHash: 101,
      floorplanImageHash: 202,
      floorplanReplicaReady: true
    };
    expect(pairingCalibrationAvailability(site, participant)).toBe("ready");
    participant.endpoint.siteDirectoryRevision = 0;
    expect(pairingCalibrationAvailability(site, participant)).toBe("waiting");
    participant.endpoint.siteDirectoryRevision = 4;
    participant.endpoint.siteConfigurationDigest = "fedcba9876543210";
    expect(pairingCalibrationAvailability(site, participant)).toBe("waiting");
    participant.endpoint.siteConfigurationDigest = "0123456789abcdef";
    participant.membership = "revoked";
    expect(pairingCalibrationAvailability(site, participant)).toBe("hidden");
  });

  it("waits for authenticated canonical convergence before resuming setup", () => {
    const site = calibrationSite();
    const participant: PlatformParticipantsStatus["participants"][number] = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      canonicalMembership: "quarantined",
      directMembership: "quarantined",
      canonicalMember: true,
      directPair: true,
      credential: { provisioned: true, enabled: true, keyVersion: 1 },
      endpoint: {
        address: "192.168.1.73",
        port: 49191,
        authenticated: false,
        online: false,
        lastSeenAgeMs: 0,
        floorplanReplicaReady: false
      }
    };

    expect(pairingSetupResumeAvailability(site, participant, "192.168.1.73")).toBe("ready");
    participant.endpoint = {
      ...participant.endpoint!,
      authenticated: true,
      online: true,
      siteCredentialsComplete: true,
      siteConfigurationAuthorityEpoch: 2,
      siteConfigurationGeneration: 7,
      siteConfigurationDigest: "0123456789abcdef",
      siteDirectoryRevision: 4,
      siteDirectoryDigest: 303,
      floorplanReplicaReady: false,
      floorplanConfigHash: 999,
      floorplanImageHash: 888
    };
    expect(pairingSetupResumeAvailability(site, participant, "192.168.1.73")).toBe("ready");
    expect(pairingCalibrationAvailability(site, participant)).toBe("waiting");
    site.configurationConsistent = false;
    expect(pairingSetupResumeAvailability(site, participant, "192.168.1.73")).toBe("ready");
    site.configurationConsistent = true;
    participant.directMembership = "active";
    expect(pairingSetupResumeAvailability(site, participant, "192.168.1.73")).toBe("hidden");
  });

  it("offers manual fallback only for active community NAMO devices on the configuration device", () => {
    const participant: PlatformParticipantsStatus["participants"][number] = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    };

    expect(communityFallbackControlAvailable(participant, true)).toBe(true);
    expect(communityFallbackControlAvailable(participant, false)).toBe(false);
    expect(communityFallbackControlAvailable({
      ...participant,
      membership: "quarantined"
    }, true)).toBe(false);
    expect(communityFallbackControlAvailable({
      ...participant,
      trust: "official_verified"
    }, true)).toBe(false);
  });

  it("counts only online active devices allowed to take over", () => {
    const configuration = {
      members: [
        {
          participantId: "namo-node-local",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        },
        {
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "active",
          leaderEligible: true
        }
      ]
    } as PlatformSiteConfigurationStatus;
    const participants: PlatformParticipantsStatus["participants"] = [{
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      leaderEligible: true,
      credential: { provisioned: true, enabled: true, keyVersion: 1 },
      endpoint: {
        address: "192.168.1.73",
        port: 49191,
        authenticated: true,
        online: true,
        lastSeenAgeMs: 250
      }
    }];

    expect(onlineLeaderEligibleCount(
      configuration,
      participants,
      "namo-node-local"
    )).toBe(2);
    participants[0].endpoint = { ...participants[0].endpoint!, online: false };
    expect(onlineLeaderEligibleCount(
      configuration,
      participants,
      "namo-node-local"
    )).toBe(1);
  });

  it("offers fallback preparation only when the local owner is the sole online eligible device", () => {
    const configuration = {
      localOwner: true,
      members: [
        {
          participantId: "namo-node-local",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        },
        {
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          leaderEligible: false
        }
      ]
    } as PlatformSiteConfigurationStatus;
    const participants: PlatformParticipantsStatus["participants"] = [{
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      canonicalMembership: "quarantined",
      leaderEligible: false,
      credential: { provisioned: true, enabled: false, keyVersion: 1 },
      endpoint: {
        address: "192.168.1.73",
        port: 49191,
        authenticated: true,
        online: true,
        lastSeenAgeMs: 250
      }
    }];

    expect(shouldOfferCommunityFallbackPreparation(
      configuration,
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(true);

    expect(shouldOfferCommunityFallbackPreparation(
      { ...configuration, localOwner: false },
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(false);

    participants[0] = { ...participants[0], trust: "official_verified" };
    expect(shouldOfferCommunityFallbackPreparation(
      configuration,
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(false);
  });

  it("does not offer fallback preparation when another online eligible device exists", () => {
    const configuration = {
      localOwner: true,
      members: [
        {
          participantId: "namo-node-local",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        },
        {
          participantId: "namo-node-existing",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "active",
          leaderEligible: true
        },
        {
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          leaderEligible: false
        }
      ]
    } as PlatformSiteConfigurationStatus;
    const participants: PlatformParticipantsStatus["participants"] = [
      {
        participantId: "namo-node-existing",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "active",
        leaderEligible: true,
        credential: { provisioned: true, enabled: true, keyVersion: 1 },
        endpoint: {
          address: "192.168.1.72",
          port: 49191,
          authenticated: true,
          online: true,
          lastSeenAgeMs: 250,
          siteCredentialsComplete: true
        },
        readiness: { ready: true, missing: [] }
      },
      {
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        leaderEligible: false,
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }
    ];

    expect(shouldOfferCommunityFallbackPreparation(
      configuration,
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(false);

    participants[0].endpoint = { ...participants[0].endpoint!, online: false };
    expect(shouldOfferCommunityFallbackPreparation(
      configuration,
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(true);
  });

  it("offers fallback preparation at seven-member capacity when only the local device is ready", () => {
    const offlineMembers = Array.from({ length: 5 }, (_, index) => ({
      participantId: `namo-node-offline-${index}`,
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      leaderEligible: true
    }));
    const configuration = {
      localOwner: true,
      members: [
        {
          participantId: "namo-node-local",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        },
        ...offlineMembers,
        {
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          leaderEligible: false
        }
      ]
    } as PlatformSiteConfigurationStatus;
    const participants: PlatformParticipantsStatus["participants"] = [
      ...offlineMembers.map((member) => ({
        ...member,
        credential: { provisioned: true, enabled: true, keyVersion: 1 },
        endpoint: {
          address: "192.168.1.90",
          port: 49191,
          authenticated: true,
          online: false,
          lastSeenAgeMs: 10000,
          siteCredentialsComplete: true
        },
        readiness: { ready: true, missing: [] }
      })),
      {
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        leaderEligible: false,
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }
    ];

    expect(shouldOfferCommunityFallbackPreparation(
      configuration,
      participants,
      "namo-node-local",
      "namo-node-peer"
    )).toBe(true);
  });

  it("restores paired peer display from the persisted registry after pairing runtime resets", async () => {
    const persisted: PlatformParticipantsStatus = {
      ok: true,
      participants: [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }]
    };

    expect(pairingDisplay(status({
      state: "idle",
      peerParticipantId: "",
      offer: null,
      confirmationCode: null
    }), persisted)).toEqual({
      state: "committed",
      peerParticipantIds: ["namo-node-peer"]
    });
  });

  it("restores paired peer display from canonical membership after Owner handover", () => {
    const canonical: PlatformParticipantsStatus = {
      ok: true,
      managementAllowed: true,
      participants: [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        canonicalMember: true,
        directPair: false,
        credential: { provisioned: false, enabled: false, keyVersion: 0 }
      }]
    };

    expect(pairingDisplay(null, canonical)).toEqual({
      state: "committed",
      peerParticipantIds: ["namo-node-peer"]
    });
  });

  it("does not present an uncredentialed or revoked registry record as a paired device", () => {
    const persisted: PlatformParticipantsStatus = {
      ok: true,
      participants: [
        {
          participantId: "approved-only",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: { provisioned: false, enabled: false, keyVersion: 0 }
        },
        {
          participantId: "revoked-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "revoked",
          credential: { provisioned: true, enabled: false, keyVersion: 1 }
        }
      ]
    };

    expect(pairingDisplay(null, persisted)).toEqual({ state: "idle", peerParticipantIds: [] });
  });

  it("returns to idle after the final persisted pairing record is removed", () => {
    const empty: PlatformParticipantsStatus = {
      ok: true,
      participants: []
    };

    expect(pairingDisplay(status({
      state: "committed",
      peerParticipantId: "namo-node-removed",
      offer: null,
      confirmationCode: null
    }), empty)).toEqual({
      state: "idle",
      peerParticipantIds: []
    });
  });

  it("loads the persisted participant registry without mutating it", async () => {
    const fetcher: typeof fetch = async (input) => {
      expect(String(input)).toBe("http://local/api/platform/participants");
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({ fetcher });
    await expect(client.getParticipants("http://local")).resolves.toEqual({ ok: true, participants: [] });
  });

  it("uses the existing participant lifecycle API for pending and local removal", async () => {
    const bodies: string[] = [];
    const fetcher: typeof fetch = async (_input, init) => {
      bodies.push(String(init?.body ?? ""));
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.revokeParticipant("http://local", "namo-node-peer");
    await client.forgetParticipant("http://local", "namo-node-peer");

    expect(bodies).toEqual([
      "action=revoke&participantId=namo-node-peer",
      "action=forget&participantId=namo-node-peer"
    ]);
  });

  it("posts the explicit community fallback permission through the participant API", async () => {
    const bodies: string[] = [];
    let eligible = false;
    const status = () => ({
      ok: true,
      participants: [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "active",
        canonicalMembership: "active",
        directMembership: "active",
        leaderEligible: eligible,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]
    });
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        const body = String(init.body ?? "");
        bodies.push(body);
        eligible = body.includes("eligible=true");
      }
      return jsonResponse(status());
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.setCommunityFallback("http://local", "namo-node-peer", true);
    await client.setCommunityFallback("http://local", "namo-node-peer", false);

    expect(bodies).toEqual([
      "action=set_community_leader&participantId=namo-node-peer&eligible=true",
      "action=set_community_leader&participantId=namo-node-peer&eligible=false"
    ]);
  });

  it("confirms both devices from the device that started setup", async () => {
    const mutations: string[] = [];
    let operationId = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const side = url.startsWith("http://local") ? "local" : "peer";
      if (init?.method === "POST") {
        mutations.push(`${side}:${String(init.body)}`);
        operationId += 1;
        return jsonResponse({ operationId });
      }
      return jsonResponse(status({
        state: "committed",
        role: side === "local" ? "leader" : "joiner",
        acceptedOperationId: operationId,
        completedOperationId: operationId,
        lastOperation: "confirm",
        peerParticipantId: side === "local" ? "namo-node-peer" : "namo-node-local",
        offer: null,
        confirmationCode: "123456"
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    const result = await client.confirmPair("http://local", "http://peer", "123456");

    expect(result.local.state).toBe("committed");
    expect(result.peer.state).toBe("committed");
    expect(mutations).toEqual([
      "local:action=confirm&confirmationCode=123456",
      "peer:action=confirm&confirmationCode=123456"
    ]);
  });

  it("accepts a committed peer after the confirm response is lost", async () => {
    let localOperation = 0;
    let peerPosted = false;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const local = url.startsWith("http://local");
      if (init?.method === "POST") {
        if (!local) {
          peerPosted = true;
          throw new TypeError("connection closed after commit");
        }
        localOperation += 1;
        return jsonResponse({ operationId: localOperation });
      }
      return jsonResponse(status({
        state: "committed",
        role: local ? "leader" : "joiner",
        acceptedOperationId: localOperation,
        completedOperationId: localOperation,
        lastOperation: "confirm",
        peerVerified: true,
        peerParticipantId: local ? "namo-node-peer" : "namo-node-local",
        leaderSiteId: "site-main",
        offer: null
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await expect(client.confirmPair(
      "http://local", "http://peer", "123456"
    )).resolves.toMatchObject({ peer: { state: "committed" } });
    expect(peerPosted).toBe(true);
  });

  it("does not erase a local pairing anchor when peer commit is uncertain", async () => {
    const mutations: string[] = [];
    let localOperation = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const local = url.startsWith("http://local");
      if (init?.method === "POST") {
        mutations.push(`${local ? "local" : "peer"}:${String(init.body)}`);
        if (!local) throw new TypeError("peer unavailable");
        localOperation += 1;
        return jsonResponse({ operationId: localOperation });
      }
      if (!local) throw new TypeError("peer unavailable");
      return jsonResponse(status({
        state: "committed",
        role: "leader",
        acceptedOperationId: localOperation,
        completedOperationId: localOperation,
        lastOperation: "confirm",
        peerParticipantId: "namo-node-peer",
        leaderSiteId: "site-main",
        offer: null
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await expect(client.confirmPair(
      "http://local", "http://peer", "123456"
    )).rejects.toMatchObject({ code: "pairing_confirmation_uncertain" });
    expect(mutations.some((entry) => entry.includes("action=revoke"))).toBe(false);
    expect(mutations.some((entry) => entry.includes("action=forget"))).toBe(false);
  });

  it("removes only the local quarantine when a reachable peer rejects commit", async () => {
    const mutations: string[] = [];
    let localOperation = 0;
    let localMembership: "quarantined" | "revoked" | "missing" = "quarantined";
    const participants = () => ({
      ok: true,
      participants: localMembership === "missing" ? [] : [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: localMembership,
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }]
    });
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const local = url.startsWith("http://local");
      const pairing = url.endsWith("/api/platform/pairing");
      if (init?.method === "POST") {
        const body = String(init.body ?? "");
        mutations.push(`${local ? "local" : "peer"}:${body}`);
        if (pairing && !local) {
          return jsonResponse({
            errorInfo: { code: "site_configuration_owner_required" }
          }, 409);
        }
        if (pairing) {
          localOperation += 1;
          return jsonResponse({ operationId: localOperation });
        }
        if (body.startsWith("action=revoke")) localMembership = "revoked";
        if (body.startsWith("action=forget")) localMembership = "missing";
        return jsonResponse(participants());
      }
      if (!pairing) return jsonResponse(participants());
      return jsonResponse(status(local ? {
        state: "committed",
        role: "leader",
        acceptedOperationId: localOperation,
        completedOperationId: localOperation,
        lastOperation: "confirm",
        peerParticipantId: "namo-node-peer",
        leaderSiteId: "site-main",
        offer: null
      } : {
        state: "verified",
        role: "joiner",
        commitResult: "site_configuration_owner_required",
        peerParticipantId: "namo-node-local",
        leaderSiteId: "site-main",
        offer: null
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await expect(client.confirmPair(
      "http://local", "http://peer", "123456"
    )).rejects.toMatchObject({ code: "site_configuration_owner_required" });
    expect(mutations.some((entry) => entry.includes("action=revoke"))).toBe(true);
    expect(mutations.some((entry) => entry.includes("action=forget"))).toBe(true);
    expect(localMembership).toBe("missing");
  });

  it("posts the approved spatial transform through the existing configure action", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({ fetcher });
    await client.configureParticipant(
      "http://local",
      "namo-node-peer",
      {
        pointTracks: true,
        velocity: true,
        positionUncertainty: false,
        range: true,
        presence: true,
        motion: true,
        zonePresence: false,
        bridge: false,
        maxTargets: 3
      },
      {
        floorplanId: "fp-main",
        spaceId: "room-main",
        groupId: "room-main",
        geometryVersion: 7,
        localPoseVersion: 3,
        poseVersion: 4,
        calibrationVersion: 2,
        transform: {
          originXMillimeters: 1200,
          originYMillimeters: 800,
          rotationCentiDegrees: -350,
          residualMillimeters: 42,
          calibrationSamples: 2
        }
      }
    );

    const fields = new URLSearchParams(body);
    expect(Object.fromEntries(fields)).toMatchObject({
      action: "configure",
      participantId: "namo-node-peer",
      automaticGroup: "true",
      floorplanId: "fp-main",
      geometryVersion: "7",
      localPoseVersion: "3",
      poseVersion: "4",
      calibrationVersion: "2",
      originXMillimeters: "1200",
      originYMillimeters: "800",
      rotationCentiDegrees: "-350",
      residualMillimeters: "42",
      calibrationSamples: "2"
    });
  });

  it("keeps the legacy configure request valid when no transform is supplied", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.configureParticipant(
      "http://local",
      "namo-node-peer",
      {
        pointTracks: true,
        velocity: true,
        positionUncertainty: false,
        range: true,
        presence: true,
        motion: true,
        zonePresence: false,
        bridge: false,
        maxTargets: 3
      },
      {
        floorplanId: "fp-main",
        spaceId: "room-main",
        groupId: "room-main",
        geometryVersion: 7,
        localPoseVersion: 3,
        poseVersion: 4,
        calibrationVersion: 2
      }
    );

    const fields = new URLSearchParams(body);
    expect(fields.get("calibrationVersion")).toBe("2");
    expect(fields.has("originXMillimeters")).toBe(false);
    expect(fields.has("originYMillimeters")).toBe(false);
    expect(fields.has("rotationCentiDegrees")).toBe(false);
    expect(fields.has("residualMillimeters")).toBe(false);
    expect(fields.has("calibrationSamples")).toBe(false);
  });

  it("uses the active-preserving reconfigure action for recalibration", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.reconfigureParticipant(
      "http://local",
      "namo-node-peer",
      {
        pointTracks: true, velocity: true, positionUncertainty: false,
        range: true, presence: true, motion: true, zonePresence: false,
        bridge: false, maxTargets: 3
      },
      {
        floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
        geometryVersion: 7, localPoseVersion: 3, poseVersion: 4,
        calibrationVersion: 3
      }
    );

    expect(new URLSearchParams(body).get("action")).toBe("reconfigure");
  });

  it("retries a participant mutation only after a bounded busy response", async () => {
    let posts = 0;
    let reads = 0;
    const bodies: string[] = [];
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        bodies.push(String(init.body));
        if (posts === 1) {
          return jsonResponse({
            error: "busy",
            errorInfo: { code: "busy" }
          }, 409);
        }
        return jsonResponse({ ok: true, participants: [] });
      }
      reads += 1;
      return jsonResponse({ ok: true, participants: [] });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await client.activateParticipant("http://local", "namo-node-peer");

    expect(posts).toBe(2);
    expect(reads).toBe(1);
    expect(bodies.every(
      (body) => new URLSearchParams(body).get("automaticGroup") === "true"
    )).toBe(true);
  });

  it("accepts a lost configure response when the persisted state matches", async () => {
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const assignment = {
      floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
      geometryVersion: 7, localPoseVersion: 3, poseVersion: 4,
      calibrationVersion: 3,
      transform: {
        originXMillimeters: 1200,
        originYMillimeters: 800,
        rotationCentiDegrees: -350,
        residualMillimeters: 42,
        calibrationSamples: 2
      }
    };
    let posts = 0;
    let reads = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        throw new TypeError("connection closed after commit");
      }
      reads += 1;
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "active",
          leaderEligible: true,
          capabilities,
          assignment,
          credential: { provisioned: true, enabled: true, keyVersion: 1 }
        }]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await client.reconfigureParticipant(
      "http://local", "namo-node-peer", capabilities, assignment
    );

    expect(posts).toBe(1);
    expect(reads).toBe(1);
  });

  it("verifies a pair-local configure against directAssignment, not the canonical projection", async () => {
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: true,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const canonicalAssignment = {
      floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
      geometryVersion: 2, localPoseVersion: 3, poseVersion: 3,
      calibrationVersion: 13,
      transform: {
        originXMillimeters: 2112,
        originYMillimeters: 3841,
        rotationCentiDegrees: -20,
        residualMillimeters: 0,
        calibrationSamples: 1
      }
    };
    const directAssignment = {
      ...canonicalAssignment,
      transform: {
        originXMillimeters: 1253,
        originYMillimeters: 450,
        rotationCentiDegrees: 17596,
        residualMillimeters: 0,
        calibrationSamples: 1
      }
    };
    let posts = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") posts += 1;
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-owner",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          canonicalMembership: "active",
          directMembership: "quarantined",
          canonicalMember: true,
          directPair: true,
          capabilities,
          assignment: canonicalAssignment,
          directAssignment,
          credential: { provisioned: true, enabled: true, keyVersion: 1 }
        }]
      });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.configureParticipant(
      "http://joining",
      "namo-node-owner",
      capabilities,
      directAssignment,
      "direct"
    );

    expect(posts).toBe(0);
  });

  it("accepts one successful direct configure when canonical assignment differs", async () => {
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: true,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const expected = {
      floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
      geometryVersion: 2, localPoseVersion: 3, poseVersion: 3,
      calibrationVersion: 13,
      transform: {
        originXMillimeters: 1253,
        originYMillimeters: 450,
        rotationCentiDegrees: 17596,
        residualMillimeters: 0,
        calibrationSamples: 1
      }
    };
    const participant = (directAssignment: typeof expected | null) => ({
      participantId: "namo-node-owner",
      participantType: "namo_node",
      trust: "official_verified",
      membership: "active",
      canonicalMembership: "active",
      directMembership: "quarantined",
      canonicalMember: true,
      directPair: true,
      capabilities,
      assignment: {
        ...expected,
        transform: { ...expected.transform, originXMillimeters: 2112 }
      },
      directAssignment,
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    });
    let posts = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        return jsonResponse({ ok: true, participants: [participant(expected)] });
      }
      return jsonResponse({ ok: true, participants: [participant(null)] });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await client.configureParticipant(
      "http://joining",
      "namo-node-owner",
      capabilities,
      expected,
      "direct"
    );

    expect(posts).toBe(1);
  });

  it("accepts a canonical configure after Site transform diagnostics are normalized", async () => {
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: true,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const expected = {
      floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
      geometryVersion: 2, localPoseVersion: 3, poseVersion: 3,
      calibrationVersion: 15,
      transform: {
        originXMillimeters: 2080,
        originYMillimeters: 299,
        rotationCentiDegrees: -16951,
        residualMillimeters: 328,
        calibrationSamples: 2
      }
    };
    const canonicalAssignment = {
      ...expected,
      transform: {
        ...expected.transform,
        residualMillimeters: 0,
        calibrationSamples: 1
      }
    };
    const participant = (assignment: typeof canonicalAssignment | null) => ({
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: assignment ? "quarantined" : "active",
      canonicalMembership: assignment ? "quarantined" : "active",
      directMembership: "quarantined",
      canonicalMember: true,
      directPair: true,
      capabilities,
      assignment,
      directAssignment: expected,
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    });
    let posts = 0;
    let reads = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        return jsonResponse({ ok: true, participants: [participant(null)] });
      }
      reads += 1;
      return jsonResponse({
        ok: true,
        participants: [participant(reads >= 3 ? canonicalAssignment : null)]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await client.configureParticipant(
      "http://owner",
      "namo-node-peer",
      capabilities,
      expected,
      "canonical"
    );

    expect(posts).toBe(1);
    expect(reads).toBe(3);
  });

  it("accepts a successful direct configure response from a legacy peer", async () => {
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const expected = {
      floorplanId: "fp-main", spaceId: "room-main", groupId: "room-main",
      geometryVersion: 2, localPoseVersion: 3, poseVersion: 3,
      calibrationVersion: 13
    };
    const legacyParticipant = {
      participantId: "namo-node-owner",
      participantType: "namo_node",
      trust: "official_verified",
      membership: "active",
      canonicalMembership: "active",
      directMembership: "quarantined",
      canonicalMember: true,
      directPair: true,
      capabilities,
      assignment: { ...expected, poseVersion: 2 },
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    };
    let posts = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        return jsonResponse({ ok: true, participants: [legacyParticipant] });
      }
      return jsonResponse({
        ok: true,
        participants: [{ ...legacyParticipant, directMembership: "active" }]
      });
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await client.configureParticipant(
      "http://joining", "namo-node-owner", capabilities, expected, "direct"
    );

    expect(posts).toBe(1);
  });

  it("waits for direct membership readback without repeating a successful mutation", async () => {
    let posts = 0;
    let reads = 0;
    const participant = (directMembership: string) => ({
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      canonicalMembership: "active",
      directMembership,
      canonicalMember: true,
      directPair: true,
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    });
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        return jsonResponse({
          ok: true,
          participants: [participant("quarantined")]
        });
      }
      reads += 1;
      return jsonResponse({
        ok: true,
        participants: [participant(posts > 0 ? "active" : "quarantined")]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await client.activateParticipant("http://local", "namo-node-peer", "direct");

    expect(posts).toBe(1);
    expect(reads).toBe(2);
  });

  it("treats an already active direct pair as a completed retry", async () => {
    let posts = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") {
        posts += 1;
        return jsonResponse({ ok: true, participants: [] });
      }
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-owner",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          canonicalMembership: "active",
          directMembership: "active",
          canonicalMember: true,
          directPair: true,
          credential: { provisioned: true, enabled: true, keyVersion: 1 }
        }]
      });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.activateParticipant("http://joining", "namo-node-owner", "direct");

    expect(posts).toBe(0);
  });

  it("waits until the owner observes the peer's replicated floorplan heartbeat", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: { provisioned: true, enabled: false, keyVersion: 1 },
          endpoint: {
            authenticated: true,
            online: true,
            floorplanReplicaReady: true,
            siteConfigurationAuthorityEpoch: 2,
            siteConfigurationGeneration: 4,
            siteConfigurationDigest: reads >= 2
              ? "0123456789abcdef"
              : "fedcba9876543210",
            siteDirectoryRevision: 4,
            siteDirectoryDigest: 305419896,
            floorplanGeometryVersion: 7,
            floorplanConfigHash: 101,
            floorplanImageHash: 202,
            radarPoseVersion: 5
          }
        }]
      });
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await expect(client.waitForParticipantReplica("http://local", "namo-node-peer", {
      siteConfigurationAuthorityEpoch: 2,
      siteConfigurationGeneration: 4,
      siteConfigurationDigest: "0123456789abcdef",
      floorplanGeometryVersion: 7,
      floorplanConfigHash: 101,
      floorplanImageHash: 202,
      radarPoseVersion: 5
    }, 1000))
      .resolves.toMatchObject({ participantId: "namo-node-peer" });
    expect(reads).toBe(2);
  });

  it("keeps waiting for a replica after a transient participants read failure", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      if (reads === 1) throw new TypeError("connection reset");
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "quarantined",
          credential: { provisioned: true, enabled: false, keyVersion: 1 },
          endpoint: {
            authenticated: true,
            online: true,
            address: "192.168.1.51",
            floorplanReplicaReady: true,
            siteConfigurationAuthorityEpoch: 2,
            siteConfigurationGeneration: 4,
            siteConfigurationDigest: "0123456789abcdef",
            siteDirectoryRevision: 4,
            siteDirectoryDigest: 305419896,
            floorplanGeometryVersion: 3,
            floorplanConfigHash: 101,
            floorplanImageHash: 202
          }
        }]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForParticipantReplica(
      "http://local",
      "namo-node-peer",
      {
        siteConfigurationAuthorityEpoch: 2,
        siteConfigurationGeneration: 4,
        siteConfigurationDigest: "0123456789abcdef",
        floorplanGeometryVersion: 3,
        floorplanConfigHash: 101,
        floorplanImageHash: 202
      },
      1000
    )).resolves.toMatchObject({ participantId: "namo-node-peer" });
    expect(reads).toBe(2);
  });

  it("waits for the first authenticated pairing heartbeat before Site sync", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: { provisioned: true, enabled: true, keyVersion: 1 },
          endpoint: reads >= 3 ? {
            address: "192.168.1.73",
            port: 49191,
            authenticated: true,
            online: true,
            lastSeenAgeMs: 0
          } : null
        }]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await expect(client.waitForParticipantEndpoint(
      "http://local",
      "namo-node-peer",
      1000
    )).resolves.toMatchObject({ participantId: "namo-node-peer" });
    expect(reads).toBe(3);
  });

  it("keeps waiting for an endpoint after a transient participants 503", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      if (reads === 1) {
        return jsonResponse({ error: "temporarily_unavailable" }, 503);
      }
      return jsonResponse({
        ok: true,
        participants: [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "quarantined",
          credential: { provisioned: true, enabled: false, keyVersion: 1 },
          endpoint: {
            address: "192.168.1.73",
            port: 49191,
            authenticated: true,
            online: true,
            lastSeenAgeMs: 0
          }
        }]
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForParticipantEndpoint(
      "http://local",
      "namo-node-peer",
      1000
    )).resolves.toMatchObject({ participantId: "namo-node-peer" });
    expect(reads).toBe(2);
  });

  it("reports a radar pose mismatch instead of a generic replica timeout", async () => {
    const fetcher: typeof fetch = async () => jsonResponse({
      ok: true,
      participants: [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        endpoint: {
          authenticated: true,
          online: true,
          floorplanReplicaReady: true,
          siteConfigurationAuthorityEpoch: 2,
          siteConfigurationGeneration: 4,
          siteConfigurationDigest: "0123456789abcdef",
          siteDirectoryRevision: 4,
          siteDirectoryDigest: 305419896,
          floorplanGeometryVersion: 7,
          floorplanConfigHash: 101,
          floorplanImageHash: 202,
          radarPoseVersion: 0
        }
      }]
    });
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForParticipantReplica("http://local", "namo-node-peer", {
      siteConfigurationAuthorityEpoch: 2,
      siteConfigurationGeneration: 4,
      siteConfigurationDigest: "0123456789abcdef",
      floorplanGeometryVersion: 7,
      floorplanConfigHash: 101,
      floorplanImageHash: 202,
      radarPoseVersion: 1
    }, 0)).rejects.toMatchObject({ code: "radar_pose_version_mismatch" });
  });

  it("reports a site revision change instead of a generic replica timeout", async () => {
    const fetcher: typeof fetch = async () => jsonResponse({
      ok: true,
      participants: [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        endpoint: {
          authenticated: true,
          online: true,
          floorplanReplicaReady: true,
          siteConfigurationAuthorityEpoch: 2,
          siteConfigurationGeneration: 5,
          siteConfigurationDigest: "0123456789abcdef",
          siteDirectoryRevision: 4,
          siteDirectoryDigest: 305419896,
          floorplanGeometryVersion: 7,
          floorplanConfigHash: 101,
          floorplanImageHash: 202,
          radarPoseVersion: 1
        }
      }]
    });
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForParticipantReplica("http://local", "namo-node-peer", {
      siteConfigurationAuthorityEpoch: 2,
      siteConfigurationGeneration: 4,
      siteConfigurationDigest: "0123456789abcdef",
      floorplanGeometryVersion: 7,
      floorplanConfigHash: 101,
      floorplanImageHash: 202,
      radarPoseVersion: 1
    }, 0)).rejects.toMatchObject({ code: "site_revision_changed" });
  });

  it("runs one bounded discovery scan before returning compatible candidates", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      if (init?.method === "POST") {
        expect(String(init.body)).toBe("action=scan");
        return jsonResponse({ ok: true, accepted: true });
      }
      expect(url).toBe("http://local/api/platform/discovery");
      reads += 1;
      return jsonResponse({
        ok: true,
        supported: true,
        scanning: reads === 1,
        generation: reads === 1 ? 0 : 1,
        result: reads === 1 ? "idle" : "ok",
        candidates: reads === 1 ? [] : [{
          participantId: "namo-node-peer",
          participantType: "namo_node",
          product: "namo-spatial-sensor",
          address: "192.168.1.50",
          hostname: "namo-peer",
          port: 80,
          protocolMinimum: 1,
          protocolMaximum: 1
        }]
      });
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });
    const discovery = await client.scan("http://local");
    expect(discovery.candidates).toHaveLength(1);
    expect(discovery.candidates[0].address).toBe("192.168.1.50");
    expect(reads).toBe(2);
  });

  it("waits through a transient idle discovery snapshot", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") return jsonResponse({ ok: true, accepted: true });
      reads += 1;
      return jsonResponse({
        ok: true,
        supported: true,
        scanning: reads === 2,
        generation: reads < 3 ? 0 : 1,
        result: reads < 3 ? "idle" : "ok",
        candidates: []
      });
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });

    await expect(client.scan("http://local")).resolves.toMatchObject({ result: "ok" });
    expect(reads).toBe(3);
  });

  it("uses form posts and waits for deferred main-loop completion", async () => {
    const requests: Array<{ url: string; body: string }> = [];
    let reads = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      requests.push({ url, body: String(init?.body ?? "") });
      if (url.endsWith("/participants")) return jsonResponse({ ok: true });
      if (init?.method === "POST") return jsonResponse({ ok: true, operationId: 7 });
      reads += 1;
      return jsonResponse(status({
        busy: reads === 1,
        acceptedOperationId: 7,
        completedOperationId: reads === 1 ? 0 : 7
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });
    await client.prepareJoiner("192.168.1.20");
    expect(requests[0].url).toBe("http://192.168.1.20/api/platform/participants");
    expect(requests[0].body).toContain("action=open");
    expect(requests[1].body).toContain("action=prepare");
    expect(requests[1].body).toContain("role=joiner");
    expect(reads).toBe(2);
  });

  it("rejects a failed device operation instead of continuing enrollment", async () => {
    const fetcher: typeof fetch = async (_input, init) => {
      if (init?.method === "POST") return jsonResponse({ operationId: 3 });
      return jsonResponse(status({
        acceptedOperationId: 3,
        completedOperationId: 3,
        operationResult: "proof_mismatch"
      }));
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });
    await expect(client.cancel("http://device.local")).rejects.toMatchObject({ code: "proof_mismatch" });
  });

  it("exchanges both offers and proofs before showing a shared code", async () => {
    const leaderOffer = {
      role: "leader" as const,
      participantId: "namo-node-leader",
      siteId: "site-main",
      sessionId: "10".repeat(16),
      nonce: "20".repeat(32),
      publicKey: `04${"30".repeat(64)}`
    };
    const joinerOffer = {
      role: "joiner" as const,
      participantId: "namo-node-joiner",
      siteId: "site-other",
      sessionId: "40".repeat(16),
      nonce: "50".repeat(32),
      publicKey: `04${"60".repeat(64)}`
    };
    const queues = new Map<string, Array<PlatformPairingStatus>>([
      ["local", [
        status({ role: "leader", offer: leaderOffer }),
        status({ state: "exchanged", role: "leader", offer: leaderOffer, localProof: "aa".repeat(32), confirmationCode: "123456" }),
        status({ state: "verified", role: "leader", offer: leaderOffer, localProof: "aa".repeat(32), confirmationCode: "123456", peerVerified: true }),
        status({ state: "verified", role: "leader", offer: leaderOffer, localProof: "aa".repeat(32), confirmationCode: "123456", peerVerified: true })
      ]],
      ["peer", [
        status({ role: "joiner", offer: joinerOffer }),
        status({ state: "exchanged", role: "joiner", offer: joinerOffer, localProof: "bb".repeat(32), confirmationCode: "123456" }),
        status({ state: "verified", role: "joiner", offer: joinerOffer, localProof: "bb".repeat(32), confirmationCode: "123456", peerVerified: true }),
        status({ state: "verified", role: "joiner", offer: joinerOffer, localProof: "bb".repeat(32), confirmationCode: "123456", peerVerified: true })
      ]]
    ]);
    const posts: string[] = [];
    let operationId = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const side = url.startsWith("http://local") ? "local" : "peer";
      if (url.endsWith("/participants")) return jsonResponse({ ok: true });
      if (init?.method === "POST") {
        posts.push(`${side}:${String(init.body)}`);
        operationId += 1;
        return jsonResponse({ operationId });
      }
      const next = queues.get(side)?.shift();
      if (!next) throw new Error(`unexpected ${side} status request`);
      return jsonResponse({ ...next, acceptedOperationId: operationId, completedOperationId: operationId });
    };
    const client = createPlatformPairingClient({ fetcher, sleep: async () => undefined });
    const connected = await client.connectLeader("http://local", "http://peer");
    expect(connected.confirmationCode).toBe("123456");
    expect(posts.some((body) => body.includes("local:action=exchange") && body.includes("participantId=namo-node-joiner"))).toBe(true);
    expect(posts.some((body) => body.includes("peer:action=exchange") && body.includes("participantId=namo-node-leader"))).toBe(true);
    expect(posts.some((body) => body.includes("local:action=verify") && body.includes(`proof=${"bb".repeat(32)}`))).toBe(true);
    expect(posts.some((body) => body.includes("peer:action=verify") && body.includes(`proof=${"aa".repeat(32)}`))).toBe(true);
  });

  it("removes the relationship from the peer before deleting the local retry anchor", async () => {
    const records = new Map<string, Array<PlatformParticipantsStatus["participants"][number]>>([
      ["local", [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        credential: { provisioned: true, enabled: false, keyVersion: 1 }
      }]],
      ["peer", [{
        participantId: "namo-node-local",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "active",
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]]
    ]);
    const mutations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const side = url.startsWith("http://local") ? "local" : "peer";
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-peer" });
      }
      if (init?.method === "POST") {
        const form = new URLSearchParams(String(init.body));
        const action = form.get("action") ?? "";
        const participantId = form.get("participantId") ?? "";
        mutations.push(`${side}:${action}`);
        const current = records.get(side) ?? [];
        if (action === "revoke") {
          records.set(side, current.map((item) => item.participantId === participantId
            ? { ...item, membership: "revoked", credential: { ...item.credential, enabled: false } }
            : item));
        } else if (action === "forget") {
          records.set(side, current.filter((item) => item.participantId !== participantId));
        }
        return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
      }
      return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
    };

    const client = createPlatformPairingClient({ fetcher });
    const result = await client.disconnectPair(
      "http://local",
      "http://peer",
      "namo-node-local",
      "namo-node-peer"
    );

    expect(result.peer.participants).toEqual([]);
    expect(result.local.participants).toEqual([]);
    expect(mutations).toEqual([
      "peer:revoke",
      "peer:forget",
      "local:revoke",
      "local:forget"
    ]);
  });

  it("accepts the canonical projection remaining after the peer direct record is removed", async () => {
    const records = new Map<string, Array<PlatformParticipantsStatus["participants"][number]>>([
      ["local", [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        canonicalMember: true,
        directPair: true,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]],
      ["peer", [{
        participantId: "namo-node-local",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        canonicalMember: true,
        directPair: true,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]]
    ]);
    const mutations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const side = url.startsWith("http://local") ? "local" : "peer";
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-peer" });
      }
      if (init?.method === "POST") {
        const form = new URLSearchParams(String(init.body));
        const action = form.get("action") ?? "";
        const participantId = form.get("participantId") ?? "";
        mutations.push(`${side}:${action}`);
        const current = records.get(side) ?? [];
        if (action === "revoke" && side === "peer") {
          records.set(side, current.map((item) => item.participantId === participantId
            ? {
                ...item,
                directMembership: "revoked",
                credential: { ...item.credential, enabled: false }
              }
            : item));
        } else if (action === "forget" && side === "peer") {
          records.set(side, current.map((item) => item.participantId === participantId
            ? {
                ...item,
                directMembership: null,
                directPair: false,
                credential: { ...item.credential, provisioned: false, enabled: false }
              }
            : item));
        } else if (action === "revoke" && side === "local") {
          records.set(side, current.map((item) => item.participantId === participantId
            ? { ...item, membership: "revoked", credential: { ...item.credential, enabled: false } }
            : item));
        } else if (action === "forget" && side === "local") {
          records.set(side, current.filter((item) => item.participantId !== participantId));
        }
        return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
      }
      return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
    };

    const client = createPlatformPairingClient({ fetcher });
    const result = await client.disconnectPair(
      "http://local",
      "http://peer",
      "namo-node-local",
      "namo-node-peer"
    );

    expect(result.peer.participants[0]).toMatchObject({
      canonicalMember: true,
      directPair: false
    });
    expect(result.local.participants).toEqual([]);
    expect(mutations).toEqual([
      "peer:revoke",
      "peer:forget",
      "local:revoke",
      "local:forget"
    ]);
  });

  it("rejects canonical-only records from the direct-pair disconnect path", async () => {
    const records = new Map<string, Array<PlatformParticipantsStatus["participants"][number]>>([
      ["local", [{
        participantId: "namo-node-peer",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        canonicalMembership: "active",
        directMembership: null,
        canonicalMember: true,
        directPair: false,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]],
      ["peer", [{
        participantId: "namo-node-local",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        canonicalMembership: "active",
        directMembership: null,
        canonicalMember: true,
        directPair: false,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]]
    ]);
    const mutations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const side = url.startsWith("http://local") ? "local" : "peer";
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-peer" });
      }
      if (init?.method === "POST") {
        const form = new URLSearchParams(String(init.body));
        const action = form.get("action") ?? "";
        const participantId = form.get("participantId") ?? "";
        mutations.push(`${side}:${action}`);
        if (side === "peer") {
          return jsonResponse({ errorInfo: { code: "participant_not_found" } }, 404);
        }
        const current = records.get(side) ?? [];
        if (action === "revoke") {
          records.set(side, current.map((item) => item.participantId === participantId
            ? {
                ...item,
                membership: "revoked",
                canonicalMembership: "revoked",
                credential: { ...item.credential, enabled: false }
              }
            : item));
        } else if (action === "forget") {
          records.set(side, current.filter((item) => item.participantId !== participantId));
        }
        return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
      }
      return jsonResponse({ ok: true, participants: records.get(side) ?? [] });
    };

    const client = createPlatformPairingClient({ fetcher });
    await expect(client.disconnectPair(
      "http://local",
      "http://peer",
      "namo-node-local",
      "namo-node-peer"
    )).rejects.toMatchObject({ code: "direct_pair_not_found" });
    expect(records.get("local")?.[0]).toMatchObject({
      canonicalMember: true,
      directPair: false,
      membership: "active"
    });
    expect(records.get("peer")?.[0]).toMatchObject({
      canonicalMember: true,
      directPair: false,
      membership: "active"
    });
    expect(mutations).toEqual([]);
  });

  it("leaves the local relationship untouched when peer removal cannot complete", async () => {
    const localParticipant = {
      participantId: "namo-node-peer",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "quarantined",
      credential: { provisioned: true, enabled: false, keyVersion: 1 }
    };
    const localMutations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      if (url.endsWith("/descriptor")) return jsonResponse({ participantId: "namo-node-peer" });
      if (url.startsWith("http://peer") && init?.method === "POST") {
        return jsonResponse({ errorInfo: { code: "participant_storage_failed" } }, 503);
      }
      if (url.startsWith("http://peer")) {
        return jsonResponse({
          ok: true,
          participants: [{ ...localParticipant, participantId: "namo-node-local" }]
        });
      }
      if (init?.method === "POST") localMutations.push(String(init.body));
      return jsonResponse({ ok: true, participants: [localParticipant] });
    };

    const client = createPlatformPairingClient({ fetcher });
    await expect(client.disconnectPair(
      "http://local",
      "http://peer",
      "namo-node-local",
      "namo-node-peer"
    )).rejects.toMatchObject({ code: "participant_storage_failed" });
    expect(localMutations).toEqual([]);
  });

  it("rejects a manual address that belongs to a different node before changing either registry", async () => {
    let mutationCount = 0;
    const fetcher: typeof fetch = async (input, init) => {
      if (init?.method === "POST") mutationCount += 1;
      if (String(input).endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-someone-else" });
      }
      return jsonResponse({ ok: true, participants: [] });
    };

    const client = createPlatformPairingClient({ fetcher });
    await expect(client.disconnectPair(
      "http://local",
      "http://peer",
      "namo-node-local",
      "namo-node-peer"
    )).rejects.toMatchObject({ code: "peer_identity_mismatch" });
    expect(mutationCount).toBe(0);
  });

  it("removes a non-owner Node from the canonical Owner before resetting it to standalone", async () => {
    let ownerParticipants: PlatformParticipantsStatus["participants"] = [{
      participantId: "namo-node-local",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      canonicalMember: true,
      directPair: true,
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    }];
    const operations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const isOwner = url.startsWith("http://owner");
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-owner" });
      }
      if (url.endsWith("/site-configuration")) {
        return jsonResponse({
          ok: true,
          siteId: "site-main",
          localOwner: isOwner,
          configurationOwnerNodeId: "namo-node-owner"
        });
      }
      if (url.endsWith("/participants") && init?.method !== "POST") {
        return jsonResponse({ ok: true, participants: isOwner ? ownerParticipants : [] });
      }
      if (url.endsWith("/participants") && init?.method === "POST") {
        const form = new URLSearchParams(String(init.body));
        const action = form.get("action") ?? "";
        operations.push(`owner:${action}`);
        if (action === "revoke") {
          ownerParticipants = ownerParticipants.map((participant) => ({
            ...participant,
            membership: "revoked",
            credential: { ...participant.credential, enabled: false }
          }));
        } else if (action === "forget") {
          ownerParticipants = [];
        }
        return jsonResponse({ ok: true, participants: ownerParticipants });
      }
      if (url.endsWith("/api/system/reset") && init?.method === "POST") {
        operations.push(`local:${String(init.body)}`);
        return jsonResponse({ ok: true, rebootRequired: true, rebootInMs: 1000 });
      }
      throw new Error(`unexpected request ${url}`);
    };

    const client = createPlatformPairingClient({ fetcher });
    const result = await client.leaveSite(
      "http://local", "http://owner", "namo-node-local", "namo-node-owner"
    );

    expect(result.owner.participants).toEqual([]);
    expect(result.reset.rebootRequired).toBe(true);
    expect(operations).toEqual([
      "owner:revoke",
      "owner:forget",
      "local:connections=true"
    ]);
  });

  it("recovers a stale non-owner Node when the verified Owner already removed it", async () => {
    const operations: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const isOwner = url.startsWith("http://owner");
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-owner" });
      }
      if (url.endsWith("/site-configuration")) {
        return jsonResponse({
          ok: true,
          siteId: isOwner ? "site-new" : "site-old",
          localOwner: isOwner,
          configurationOwnerNodeId: "namo-node-owner"
        });
      }
      if (url.endsWith("/participants")) {
        if (init?.method === "POST") operations.push("unexpected-owner-mutation");
        return jsonResponse({ ok: true, participants: [] });
      }
      if (url.endsWith("/api/system/reset")) {
        operations.push(`local:${String(init?.body)}`);
        return jsonResponse({ ok: true, rebootRequired: true, rebootInMs: 1000 });
      }
      throw new Error(`unexpected request ${url}`);
    };

    const client = createPlatformPairingClient({ fetcher });
    await client.leaveSite(
      "http://local", "http://owner", "namo-node-local", "namo-node-owner"
    );
    expect(operations).toEqual(["local:connections=true"]);
  });

  it("does not reset a Node while the selected Owner still lists it under another Site", async () => {
    let resetCount = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const isOwner = url.startsWith("http://owner");
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-owner" });
      }
      if (url.endsWith("/site-configuration")) {
        return jsonResponse({
          ok: true,
          siteId: isOwner ? "site-other" : "site-main",
          localOwner: isOwner,
          configurationOwnerNodeId: "namo-node-owner"
        });
      }
      if (url.endsWith("/participants")) {
        return jsonResponse({
          ok: true,
          participants: isOwner ? [{ participantId: "namo-node-local" }] : []
        });
      }
      if (url.endsWith("/api/system/reset") && init?.method === "POST") resetCount += 1;
      return jsonResponse({ ok: true });
    };

    const client = createPlatformPairingClient({ fetcher });
    await expect(client.leaveSite(
      "http://local", "http://owner", "namo-node-local", "namo-node-owner"
    )).rejects.toMatchObject({ code: "site_leave_owner_mismatch" });
    expect(resetCount).toBe(0);
  });

  it("does not reset a Node when the Owner cannot remove its canonical record", async () => {
    let resetCount = 0;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      const isOwner = url.startsWith("http://owner");
      if (url.endsWith("/descriptor")) {
        return jsonResponse({ participantId: "namo-node-owner" });
      }
      if (url.endsWith("/site-configuration")) {
        return jsonResponse({
          ok: true,
          siteId: "site-main",
          localOwner: isOwner,
          configurationOwnerNodeId: "namo-node-owner"
        });
      }
      if (url.endsWith("/participants") && init?.method !== "POST") {
        return jsonResponse({
          ok: true,
          participants: isOwner ? [{ participantId: "namo-node-local" }] : []
        });
      }
      if (url.endsWith("/participants") && init?.method === "POST") {
        return jsonResponse(
          {
            ok: false,
            errorInfo: { code: "site_configuration_continuity_unavailable" }
          },
          409
        );
      }
      if (url.endsWith("/api/system/reset") && init?.method === "POST") {
        resetCount += 1;
      }
      return jsonResponse({ ok: true });
    };

    const client = createPlatformPairingClient({ fetcher });
    await expect(client.leaveSite(
      "http://local", "http://owner", "namo-node-local", "namo-node-owner"
    )).rejects.toMatchObject({ code: "site_configuration_continuity_unavailable" });
    expect(resetCount).toBe(0);
  });

  it("commits the current floorplan manifest without uploading the image again", async () => {
    const requests: Array<{ url: string; body: string }> = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      requests.push({ url, body: String(init?.body ?? "") });
      if (url.endsWith("/api/floorplan")) {
        return jsonResponse({
          floorplanId: "fp-main",
          geometryVersion: 7,
          image: { width: 1000, height: 600 },
          scale: { mmPerPxX: 10, mmPerPxY: 10 }
        });
      }
      return jsonResponse({ ok: true, floorplan: { committed: true } });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.commitLocalFloorplan("http://local");

    expect(requests).toEqual([
      { url: "http://local/api/floorplan", body: "" },
      {
        url: "http://local/api/platform/site-configuration",
        body: "action=commitFloorplan&floorplanId=fp-main&geometryVersion=7&coordinateWidthMm=10000&coordinateHeightMm=6000"
      }
    ]);
  });

  it("commits a saved floorplan manifest without reading the payload back", async () => {
    const requests: Array<{ url: string; body: string }> = [];
    const fetcher: typeof fetch = async (input, init) => {
      requests.push({ url: String(input), body: String(init?.body ?? "") });
      return jsonResponse({ ok: true, floorplan: { committed: true } });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.commitFloorplanManifest("http://local", "fp-main", 8, 12000, 8000);

    expect(requests).toEqual([{
      url: "http://local/api/platform/site-configuration",
      body: "action=commitFloorplan&floorplanId=fp-main&geometryVersion=8&coordinateWidthMm=12000&coordinateHeightMm=8000"
    }]);
  });

  it("claims an unresolved local Site owner with one bounded request", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({
        ok: true,
        configurationOwnerNodeId: "namo-node-local",
        localOwner: true,
        rosterCommitted: true
      });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.claimSiteOwnership("http://local");

    expect(body).toBe("action=claimOwnership");
  });

  it("starts a site sync with one small form request", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({ ok: true, replication: { state: "waiting_for_owner" } });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.syncSiteConfiguration("http://local");

    expect(body).toBe("action=sync");
  });

  it("names the confirmed owner when a new Joiner has no inferred owner", async () => {
    let body = "";
    const fetcher: typeof fetch = async (_input, init) => {
      body = String(init?.body ?? "");
      return jsonResponse({
        ok: true,
        replication: { state: "waiting_for_owner" }
      });
    };
    const client = createPlatformPairingClient({ fetcher });

    await client.syncSiteConfiguration(
      "http://local",
      "namo-node-owner"
    );

    expect(body).toBe("action=sync&ownerNodeId=namo-node-owner");
  });

  it("waits for the peer to adopt the exact authoritative site manifest", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      return jsonResponse({
        ok: true,
        schemaVersion: 1,
        siteId: "site-main",
        authorityEpoch: 2,
        generation: reads >= 2 ? 2 : 1,
        configurationDigest: reads >= 3 ? "0123456789abcdef" : "fedcba9876543210",
        configurationConsistent: reads >= 3,
        rosterRevision: 2,
        rosterCommitted: true,
        configurationOwnerNodeId: "namo-node-owner",
        localOwner: false,
        memberCount: 2,
        activeMemberCount: 1,
        floorplan: {
          committed: reads >= 2,
          floorplanId: reads >= 2 ? "fp-main" : null,
          geometryVersion: reads >= 2 ? 7 : 0,
          configHash: reads >= 2 ? 101 : 0,
          imageHash: reads >= 2 ? 202 : 0
        },
        replication: {
          supported: true,
          socketReady: true,
          state: reads >= 2 ? "complete" : "config_payload",
          result: reads >= 2 ? "ok" : "idle",
          ownerNodeId: "namo-node-owner",
          remoteGeneration: reads >= 2 ? 2 : 0,
          transferredBytes: 0,
          totalBytes: 0,
          requests: 1,
          responses: 1,
          retries: 0,
          rejected: 0
        }
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 1000)).resolves.toMatchObject({
      localOwner: false,
      floorplan: { committed: true, floorplanId: "fp-main" }
    });
    expect(reads).toBe(3);
  });

  it("keeps waiting for Site convergence after a transient request failure", async () => {
    let reads = 0;
    const converged: PlatformSiteConfigurationStatus = {
      ...calibrationSite(),
      localOwner: false
    };
    const fetcher: typeof fetch = async () => {
      reads += 1;
      if (reads === 1) throw new TypeError("connection reset");
      return jsonResponse(converged);
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: converged.siteId,
      authorityEpoch: converged.authorityEpoch as number,
      generation: converged.generation,
      configurationDigest: converged.configurationDigest as string,
      configurationOwnerNodeId: converged.configurationOwnerNodeId as string,
      floorplanId: converged.floorplan.floorplanId as string,
      geometryVersion: converged.floorplan.geometryVersion,
      configHash: converged.floorplan.configHash,
      imageHash: converged.floorplan.imageHash
    }, 1000)).resolves.toMatchObject({ generation: 7 });
    expect(reads).toBe(2);
  });

  it("reports the final replication failure instead of a generic sync timeout", async () => {
    const fetcher: typeof fetch = async () => jsonResponse({
      ok: true,
      schemaVersion: 2,
      siteId: "site-main",
      generation: 1,
      rosterRevision: 1,
      rosterCommitted: false,
      configurationOwnerNodeId: null,
      localOwner: false,
      memberCount: 1,
      activeMemberCount: 1,
      floorplan: {
        committed: false,
        floorplanId: null,
        geometryVersion: 0,
        configHash: 0,
        imageHash: 0
      },
      replication: {
        supported: true,
        socketReady: true,
        state: "failed",
        result: "owner_offline",
        ownerNodeId: "namo-node-owner",
        remoteGeneration: 0,
        transferredBytes: 0,
        totalBytes: 0,
        requests: 0,
        responses: 0,
        retries: 0,
        rejected: 0
      }
    });
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 0)).rejects.toMatchObject({ code: "owner_offline" });
  });

  it("waits through a busy replication result until the existing transfer converges", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      const converged = reads >= 2;
      return jsonResponse({
        ok: true,
        schemaVersion: 2,
        siteId: "site-main",
        authorityEpoch: 2,
        generation: converged ? 2 : 1,
        configurationDigest: converged ? "0123456789abcdef" : "fedcba9876543210",
        configurationConsistent: converged,
        rosterRevision: 2,
        rosterCommitted: true,
        configurationOwnerNodeId: "namo-node-owner",
        localOwner: false,
        memberCount: 2,
        activeMemberCount: 2,
        floorplan: {
          committed: converged,
          floorplanId: converged ? "fp-main" : null,
          geometryVersion: converged ? 7 : 0,
          configHash: converged ? 101 : 0,
          imageHash: converged ? 202 : 0
        },
        replication: {
          supported: true,
          socketReady: true,
          state: converged ? "complete" : "failed",
          result: converged ? "ok" : "busy",
          ownerNodeId: "namo-node-owner",
          remoteGeneration: converged ? 2 : 0,
          transferredBytes: 0,
          totalBytes: 0,
          requests: 1,
          responses: converged ? 1 : 0,
          retries: 0,
          rejected: 0
        }
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 1000)).resolves.toMatchObject({ generation: 2 });
    expect(reads).toBe(2);
  });

  it("ignores an unrelated replication conflict while the requested site converges", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      const converged = reads >= 2;
      return jsonResponse({
        ok: true,
        schemaVersion: 2,
        siteId: "site-main",
        authorityEpoch: 2,
        generation: converged ? 2 : 1,
        configurationDigest: converged ? "0123456789abcdef" : "fedcba9876543210",
        configurationConsistent: converged,
        rosterRevision: 2,
        rosterCommitted: true,
        configurationOwnerNodeId: "namo-node-owner",
        localOwner: false,
        memberCount: 2,
        activeMemberCount: 2,
        floorplan: {
          committed: converged,
          floorplanId: converged ? "fp-main" : null,
          geometryVersion: converged ? 7 : 0,
          configHash: converged ? 101 : 0,
          imageHash: converged ? 202 : 0
        },
        replication: {
          supported: true,
          socketReady: true,
          state: converged ? "complete" : "conflict",
          result: converged ? "ok" : "invalid_configuration",
          validationResult: converged ? "ok" : "owner_mismatch",
          operation: converged ? "floorplan" : "credential_bundle",
          ownerNodeId: converged ? "namo-node-owner" : "namo-node-stale",
          remoteGeneration: converged ? 2 : 1,
          transferredBytes: 0,
          totalBytes: 0,
          requests: 1,
          responses: converged ? 1 : 0,
          retries: 0,
          rejected: 0
        }
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 1000)).resolves.toMatchObject({ generation: 2 });
    expect(reads).toBe(2);
  });

  it("waits through a transient owner mismatch from the requested floorplan sync", async () => {
    let reads = 0;
    const fetcher: typeof fetch = async () => {
      reads += 1;
      const converged = reads >= 2;
      return jsonResponse({
        ok: true,
        schemaVersion: 2,
        siteId: "site-main",
        authorityEpoch: 2,
        generation: converged ? 2 : 1,
        configurationDigest: converged ? "0123456789abcdef" : "fedcba9876543210",
        configurationConsistent: converged,
        rosterRevision: 2,
        rosterCommitted: true,
        configurationOwnerNodeId: "namo-node-owner",
        localOwner: false,
        memberCount: 2,
        activeMemberCount: 2,
        floorplan: {
          committed: converged,
          floorplanId: converged ? "fp-main" : null,
          geometryVersion: converged ? 7 : 0,
          configHash: converged ? 101 : 0,
          imageHash: converged ? 202 : 0
        },
        replication: {
          supported: true,
          socketReady: true,
          state: converged ? "complete" : "conflict",
          result: converged ? "ok" : "invalid_configuration",
          validationResult: converged ? "ok" : "owner_mismatch",
          operation: "floorplan",
          ownerNodeId: "namo-node-owner",
          remoteGeneration: 2,
          transferredBytes: 0,
          totalBytes: 0,
          requests: 1,
          responses: converged ? 1 : 0,
          retries: 0,
          rejected: 0
        }
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0
    });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 1000)).resolves.toMatchObject({ generation: 2 });
    expect(reads).toBe(2);
  });

  it("still rejects a definitive conflict for the requested owner and generation", async () => {
    const fetcher: typeof fetch = async () => jsonResponse({
      ok: true,
      schemaVersion: 2,
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 1,
      configurationDigest: "fedcba9876543210",
      configurationConsistent: false,
      rosterRevision: 2,
      rosterCommitted: true,
      configurationOwnerNodeId: "namo-node-owner",
      localOwner: false,
      memberCount: 2,
      activeMemberCount: 2,
      floorplan: {
        committed: true,
        floorplanId: "fp-main",
        geometryVersion: 7,
        configHash: 101,
        imageHash: 202
      },
      replication: {
        supported: true,
        socketReady: true,
        state: "conflict",
        result: "revision_conflict",
        validationResult: "revision_conflict",
        operation: "floorplan",
        ownerNodeId: "namo-node-owner",
        remoteGeneration: 2,
        transferredBytes: 0,
        totalBytes: 0,
        requests: 1,
        responses: 1,
        retries: 0,
        rejected: 0
      }
    });
    const client = createPlatformPairingClient({ fetcher });

    await expect(client.waitForSiteConvergence("http://peer", {
      siteId: "site-main",
      authorityEpoch: 2,
      generation: 2,
      configurationDigest: "0123456789abcdef",
      configurationOwnerNodeId: "namo-node-owner",
      floorplanId: "fp-main",
      geometryVersion: 7,
      configHash: 101,
      imageHash: 202
    }, 1000)).rejects.toMatchObject({ code: "revision_conflict" });
  });

  it("does not keep a stale return link after every relationship is gone", () => {
    const none: PlatformParticipantsStatus = {
      ok: true,
      participants: []
    };
    expect(pairingReturnFlowActive(
      "http://192.168.1.10/dashboard?tab=pairing",
      status({ state: "committed", role: "joiner", invitation: null }),
      none
    )).toBe(false);
    expect(pairingReturnFlowActive(
      "http://192.168.1.10/dashboard?tab=pairing",
      status({
        state: "prepared",
        role: "joiner",
        invitation: {
          state: "pending",
          requesterAddress: "192.168.1.10",
          remainingMs: 60000
        }
      }),
      none
    )).toBe(true);
    expect(pairingReturnFlowActive(
      "http://192.168.1.10/dashboard?tab=pairing",
      status({
        state: "committed",
        role: "joiner",
        peerParticipantId: "namo-node-owner",
        invitation: null
      }),
      {
        ok: true,
        participants: [{
          participantId: "namo-node-owner",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: {
            provisioned: true,
            enabled: false,
            keyVersion: 1
          },
          endpoint: {
            address: "192.168.1.10",
            port: 80,
            authenticated: true,
            online: true,
            lastSeenAgeMs: 100
          }
        }]
      }
    )).toBe(true);
    expect(pairingReturnFlowActive(
      "http://192.168.1.20/dashboard?tab=pairing",
      status({
        state: "committed",
        role: "joiner",
        peerParticipantId: "namo-node-owner",
        invitation: null
      }),
      {
        ok: true,
        participants: [{
          participantId: "namo-node-owner",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: {
            provisioned: true,
            enabled: false,
            keyVersion: 1
          },
          endpoint: {
            address: "192.168.1.10",
            port: 80,
            authenticated: true,
            online: true,
            lastSeenAgeMs: 100
          }
        }]
      }
    )).toBe(false);
  });
});
