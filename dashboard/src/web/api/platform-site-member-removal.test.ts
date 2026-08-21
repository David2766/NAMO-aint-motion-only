import { describe, expect, it } from "vitest";

import {
  createPlatformPairingClient,
  participantUsesSiteMemberRemoval,
  type PlatformParticipant,
  type PlatformParticipantsStatus,
  type PlatformSiteConfigurationStatus
} from "./platform-pairing";
import type { PairingTraceEmitter } from "../pairing-trace";

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" }
  });
}

class SiteMemberRemovalHarness {
  readonly ownerId = "namo-node-owner";
  readonly peerId = "namo-node-peer";
  readonly witnessId = "namo-node-witness";
  readonly ownerUrl = "http://owner";
  readonly peerUrl = "http://peer";
  readonly witnessUrl = "http://witness";

  membership: "active" | "revoked" | "absent" = "active";
  peerStandalone = false;
  resetResponseDrops = false;
  resetDoesNotApply = false;
  resetHttpError = false;
  ownerChangesAfterReset = false;
  ownerHasDirectPair = true;
  witnessConverges = true;
  wrongPeerIdentity = false;
  operations: string[] = [];

  constructor(readonly withWitness = false) {}

  private participant(
    participantId: string,
    membership: "active" | "revoked",
    address: string,
    directPair = false
  ): PlatformParticipant {
    const canonicalMember = !directPair || membership === "active";
    return {
      participantId,
      participantType: "namo_node",
      trust: "official_verified",
      membership,
      canonicalMembership: canonicalMember ? membership : null,
      directMembership: directPair ? membership : null,
      canonicalMember,
      directPair,
      credential: {
        provisioned: true,
        enabled: membership === "active",
        keyVersion: 1
      },
      endpoint: {
        address,
        port: 80,
        online: true,
        authenticated: true,
        lastSeenAgeMs: 0
      }
    };
  }

  ownerParticipants(): PlatformParticipantsStatus {
    const participants: PlatformParticipant[] = [];
    if (this.membership !== "absent") {
      participants.push(this.participant(
        this.peerId,
        this.membership,
        this.peerUrl,
        this.ownerHasDirectPair
      ));
    }
    if (this.withWitness) {
      participants.push(this.participant(
        this.witnessId,
        "active",
        this.witnessUrl
      ));
    }
    return {
      ok: true,
      managementAllowed: true,
      participants
    };
  }

  private site(
    localNodeId: string,
    localOwner: boolean,
    membership = this.membership
  ): PlatformSiteConfigurationStatus {
    const members: NonNullable<PlatformSiteConfigurationStatus["members"]> = [{
      participantId: this.ownerId,
      participantType: "namo_node",
      trust: "official_verified",
      membership: "active",
      leaderEligible: true
    }];
    if (membership === "active" ||
        (membership === "revoked" && !this.ownerHasDirectPair)) {
      members.push({
        participantId: this.peerId,
        participantType: "namo_node",
        trust: "official_verified",
        membership,
        leaderEligible: true
      });
    }
    if (this.withWitness) {
      members.push({
        participantId: this.witnessId,
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        leaderEligible: true
      });
    }
    const generation = membership === "active" ? 1 : membership === "revoked" ? 2 : 3;
    return {
      ok: true,
      schemaVersion: 3,
      siteId: "site-shared",
      authorityEpoch: 1,
      generation,
      configurationDigest: generation.toString(16).padStart(16, "0"),
      configurationConsistent: true,
      rosterRevision: generation,
      rosterCommitted: true,
      configurationOwnerNodeId: this.ownerChangesAfterReset && this.peerStandalone
        ? this.witnessId
        : this.ownerId,
      localOwner: localOwner && !(this.ownerChangesAfterReset && this.peerStandalone),
      sharedWritesAllowed: true,
      memberCount: members.length,
      activeMemberCount: members.filter((member) => member.membership === "active").length,
      members,
      floorplan: {
        committed: false,
        floorplanId: null,
        geometryVersion: 0,
        configHash: 0,
        imageHash: 0
      },
      replication: null
    };
  }

  private peerSite(): PlatformSiteConfigurationStatus {
    if (!this.peerStandalone) return this.site(this.peerId, false, "active");
    return {
      ok: true,
      schemaVersion: 3,
      siteId: "site-peer-standalone",
      authorityEpoch: 1,
      generation: 1,
      configurationDigest: "00000000000000aa",
      configurationConsistent: true,
      rosterRevision: 1,
      rosterCommitted: false,
      configurationOwnerNodeId: this.peerId,
      localOwner: true,
      sharedWritesAllowed: true,
      memberCount: 1,
      activeMemberCount: 1,
      members: [{
        participantId: this.peerId,
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        leaderEligible: true
      }],
      floorplan: {
        committed: false,
        floorplanId: null,
        geometryVersion: 0,
        configHash: 0,
        imageHash: 0
      },
      replication: null
    };
  }

  readonly fetcher: typeof fetch = async (input, init) => {
    const url = String(input);
    const side = url.startsWith(this.ownerUrl)
      ? "owner"
      : url.startsWith(this.peerUrl)
        ? "peer"
        : "witness";

    if (url.endsWith("/api/platform/descriptor")) {
      this.operations.push(`${side}:descriptor:get`);
      return jsonResponse({
        participantId: this.wrongPeerIdentity
          ? "namo-node-wrong"
          : side === "peer"
            ? this.peerId
            : side === "owner"
              ? this.ownerId
              : this.witnessId
      });
    }
    if (url.endsWith("/api/platform/site-configuration")) {
      this.operations.push(`${side}:site:get:${this.membership}`);
      if (side === "peer") return jsonResponse(this.peerSite());
      if (side === "witness") {
        return jsonResponse(this.site(
          this.witnessId,
          false,
          this.witnessConverges ? this.membership : "active"
        ));
      }
      return jsonResponse(this.site(this.ownerId, true));
    }
    if (url.endsWith("/api/platform/participants")) {
      if (init?.method !== "POST") {
        this.operations.push(`${side}:participants:get`);
        return jsonResponse(side === "owner"
          ? this.ownerParticipants()
          : { ok: true, managementAllowed: false, participants: [] });
      }
      const form = new URLSearchParams(String(init.body));
      const action = form.get("action") ?? "";
      this.operations.push(`${side}:participants:${action}`);
      if (side !== "owner") {
        return jsonResponse({ errorInfo: { code: "participant_not_found" } }, 404);
      }
      if (action === "revoke") this.membership = "revoked";
      if (action === "forget") this.membership = "absent";
      return jsonResponse(this.ownerParticipants());
    }
    if (url.endsWith("/api/system/reset") && init?.method === "POST") {
      this.operations.push(`${side}:reset`);
      if (this.resetHttpError) {
        return jsonResponse({
          errorInfo: { code: "connections_reset_failed" }
        }, 500);
      }
      if (!this.resetDoesNotApply) this.peerStandalone = true;
      if (this.resetResponseDrops || this.resetDoesNotApply) {
        throw new TypeError("connection closed");
      }
      return jsonResponse({
        ok: true,
        rebootRequired: true,
        rebootInMs: 1000
      });
    }
    throw new Error(`unexpected request: ${url}`);
  };

  client(operationTimeoutMs = 1000, traceEmitter?: PairingTraceEmitter) {
    return createPlatformPairingClient({
      fetcher: this.fetcher,
      sleep: async () => undefined,
      operationTimeoutMs,
      traceEmitter
    });
  }
}

describe("canonical Site member removal", () => {
  it("keeps a revoked direct retry anchor on the Site-member removal path", () => {
    const harness = new SiteMemberRemovalHarness();
    harness.membership = "revoked";
    const participant = harness.ownerParticipants().participants[0];

    expect(participantUsesSiteMemberRemoval(participant)).toBe(true);
  });

  it("resets the target, removes the Owner roster entry, then forgets the direct record", async () => {
    const harness = new SiteMemberRemovalHarness();
    const result = await harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    expect(result.owner.participants).toEqual([]);
    expect(harness.peerStandalone).toBe(true);
    expect(harness.operations.filter((operation) =>
      /:participants:(?:revoke|forget)$/.test(operation) || operation.endsWith(":reset")
    )).toEqual([
      "peer:reset",
      "owner:participants:revoke",
      "owner:participants:forget"
    ]);
  });

  it("records every removal boundary without changing the removal sequence", async () => {
    const harness = new SiteMemberRemovalHarness(true);
    const events: Array<{
      phase: string;
      state: string;
      detail?: Record<string, unknown>;
    }> = [];
    const traceEmitter: PairingTraceEmitter = (phase, state, detail) => {
      events.push({ phase, state, detail });
    };

    await harness.client(1000, traceEmitter).removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    const completedPhases = events
      .filter((event) => event.state === "end" || event.state === "event")
      .map((event) => event.phase);
    expect(completedPhases).toEqual(expect.arrayContaining([
      "removal.preflight.read",
      "removal.preflight.state",
      "removal.peer.reset",
      "removal.peer.standalone_wait",
      "removal.owner.post_reset_readback",
      "removal.owner.revoke",
      "removal.owner.revoke_readback",
      "removal.owner.replication_wait",
      "removal.owner.pre_forget_readback",
      "removal.owner.forget",
      "removal.complete"
    ]));
    const replication = events.find(
      (event) => event.phase === "removal.replication.sample"
    );
    expect(replication?.detail).toMatchObject({
      requiredCopies: 2,
      matchingCopies: 2,
      failedReads: 0,
      revisionMismatches: 0,
      membershipMismatches: 0
    });
  });

  it("removes a canonical-only member after Configuration Owner handover", async () => {
    const harness = new SiteMemberRemovalHarness(true);
    harness.ownerHasDirectPair = false;

    await harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    expect(harness.peerStandalone).toBe(true);
    expect(harness.membership).toBe("absent");
    expect(harness.operations.filter((operation) =>
      /:participants:(?:revoke|forget)$/.test(operation) || operation.endsWith(":reset")
    )).toEqual([
      "peer:reset",
      "owner:participants:revoke",
      "owner:participants:forget"
    ]);
  });

  it("confirms standalone state instead of replaying a reset whose response was lost", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.resetResponseDrops = true;

    await expect(harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).resolves.toBeDefined();

    expect(harness.operations.filter((operation) => operation === "peer:reset"))
      .toHaveLength(1);
    expect(harness.membership).toBe("absent");
  });

  it("keeps the Owner roster unchanged when target reset cannot be confirmed", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.resetDoesNotApply = true;

    await expect(harness.client(-1).removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "site_member_reset_unconfirmed" });

    expect(harness.membership).toBe("active");
    expect(harness.operations).not.toContain("owner:participants:revoke");
    expect(harness.operations).not.toContain("owner:participants:forget");
  });

  it("preserves a definite target reset error instead of replacing it with a timeout", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.resetHttpError = true;

    await expect(harness.client(-1).removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "connections_reset_failed" });

    expect(harness.membership).toBe("active");
    expect(harness.operations).not.toContain("owner:participants:revoke");
  });

  it("allows an explicit force removal from the persisted revoked state", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.membership = "revoked";

    await harness.client().forgetParticipant(harness.ownerUrl, harness.peerId);

    expect(harness.membership).toBe("absent");
    expect(harness.peerStandalone).toBe(false);
    expect(harness.operations).toContain("owner:participants:forget");
  });

  it("resumes an older half-removed state whose target still uses the shared Site", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.membership = "revoked";

    await harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    expect(harness.membership).toBe("absent");
    expect(harness.operations).toContain("peer:reset");
    expect(harness.operations).not.toContain("owner:participants:revoke");
    expect(harness.operations).toContain("owner:participants:forget");
  });

  it("resumes after reload when the target reset completed before Owner removal", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.peerStandalone = true;

    await harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    expect(harness.membership).toBe("absent");
    expect(harness.operations).not.toContain("peer:reset");
    expect(harness.operations).toContain("owner:participants:revoke");
    expect(harness.operations).toContain("owner:participants:forget");
  });

  it("waits for a majority removal copy before forgetting a target in a three-node Site", async () => {
    const harness = new SiteMemberRemovalHarness(true);

    await harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );

    const witnessRead = harness.operations.findIndex(
      (operation) => operation === "witness:site:get:revoked"
    );
    const reset = harness.operations.indexOf("peer:reset");
    expect(witnessRead).toBeGreaterThan(-1);
    expect(reset).toBeGreaterThan(-1);
    expect(witnessRead).toBeGreaterThan(reset);
  });

  it("keeps the retry anchor when the removed roster lacks a majority copy", async () => {
    const harness = new SiteMemberRemovalHarness(true);
    harness.witnessConverges = false;

    await expect(harness.client(-1).removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "site_member_removal_replication_pending" });

    expect(harness.peerStandalone).toBe(true);
    expect(harness.membership).toBe("revoked");
    expect(harness.operations).not.toContain("owner:participants:forget");
  });

  it("stops after target reset if Configuration Owner changes mid-operation", async () => {
    const harness = new SiteMemberRemovalHarness(true);
    harness.ownerChangesAfterReset = true;

    await expect(harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "site_member_removal_owner_changed" });

    expect(harness.peerStandalone).toBe(true);
    expect(harness.membership).toBe("active");
    expect(harness.operations).not.toContain("owner:participants:forget");
  });

  it("rejects a wrong target identity before changing either device", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.wrongPeerIdentity = true;

    await expect(harness.client().removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "peer_identity_mismatch" });

    expect(harness.membership).toBe("active");
    expect(harness.operations.some((operation) =>
      /:participants:(?:revoke|forget)$/.test(operation)))
      .toBe(false);
    expect(harness.operations).not.toContain("peer:reset");
  });

  it("rejects a target that has already joined an unrelated Site", async () => {
    const harness = new SiteMemberRemovalHarness();
    harness.peerStandalone = true;
    const originalFetcher = harness.fetcher;
    const fetcher: typeof fetch = async (input, init) => {
      const response = await originalFetcher(input, init);
      const url = String(input);
      if (!url.startsWith(harness.peerUrl) ||
          !url.endsWith("/api/platform/site-configuration")) return response;
      const site = await response.json() as PlatformSiteConfigurationStatus;
      return jsonResponse({
        ...site,
        siteId: "site-unrelated",
        configurationOwnerNodeId: "namo-node-other",
        localOwner: false,
        memberCount: 2,
        activeMemberCount: 2,
        members: [{
          participantId: "namo-node-other",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        }, {
          participantId: harness.peerId,
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true
        }]
      });
    };
    const client = createPlatformPairingClient({ fetcher });

    await expect(client.removeSiteMember(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toMatchObject({ code: "site_member_removal_site_mismatch" });

    expect(harness.membership).toBe("active");
    expect(harness.operations).not.toContain("peer:reset");
  });
});
