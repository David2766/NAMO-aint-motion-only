import { describe, expect, it } from "vitest";
import {
  createPlatformPairingClient,
  pairingSetupRelationship,
  type PlatformParticipant,
  type PlatformPairingStatus,
  type PlatformParticipantsStatus
} from "./platform-pairing";

type Side = "owner" | "peer";
type FaultMode = "request_drop" | "response_drop" | "busy" | "invalid_operation_id";

interface InjectedFault {
  key: string;
  mode: FaultMode;
}

interface RuntimeState {
  state: PlatformPairingStatus["state"];
  role: PlatformPairingStatus["role"];
  acceptedOperationId: number;
  completedOperationId: number;
  lastOperation: PlatformPairingStatus["lastOperation"];
  operationResult: PlatformPairingStatus["operationResult"];
  commitResult: PlatformPairingStatus["commitResult"];
  invitation: boolean;
}

function jsonResponse(body: unknown, statusCode = 200): Response {
  return new Response(JSON.stringify(body), {
    status: statusCode,
    headers: { "Content-Type": "application/json" }
  });
}

function participant(
  participantId: string,
  membership: PlatformParticipant["membership"],
  provisioned = true
): PlatformParticipant {
  return {
    participantId,
    participantType: "namo_node",
    trust: "community_unverified",
    membership,
    credential: {
      provisioned,
      enabled: provisioned && membership === "active",
      keyVersion: provisioned ? 1 : 0
    }
  };
}

class PairingHttpHarness {
  readonly ownerUrl = "http://owner";
  readonly peerUrl = "http://peer";
  readonly ownerId = "namo-node-owner";
  readonly peerId: string;
  readonly requests: string[] = [];
  readonly runtimes: Record<Side, RuntimeState> = {
    owner: this.runtime("leader"),
    peer: this.runtime("joiner")
  };
  readonly records: Record<Side, PlatformParticipant[]>;
  private faults: InjectedFault[] = [];

  constructor(peerId = "namo-node-peer", ownerRecords: PlatformParticipant[] = []) {
    this.peerId = peerId;
    this.records = {
      owner: ownerRecords.map((item) => ({
        ...item,
        credential: { ...item.credential }
      })),
      peer: []
    };
  }

  readonly fetcher: typeof fetch = async (input, init) => {
    const url = new URL(String(input));
    const side = url.origin === this.ownerUrl ? "owner" : "peer";
    const method = init?.method ?? "GET";
    const form = new URLSearchParams(String(init?.body ?? ""));
    const action = form.get("action") ?? "";
    const endpoint = url.pathname === "/api/platform/pairing"
      ? "pairing"
      : url.pathname === "/api/platform/participants"
        ? "participants"
        : url.pathname === "/api/platform/descriptor"
          ? "descriptor"
          : "unknown";
    const operation = method === "POST"
      ? action
      : endpoint === "pairing"
        ? `get:${this.runtimes[side].lastOperation}`
        : "get";
    const key = `${side}:${endpoint}:${operation}`;
    this.requests.push(key);

    const fault = this.takeFault(key);
    if (fault?.mode === "request_drop")
      throw new TypeError(`connection dropped before ${key}`);
    if (fault?.mode === "busy")
      return jsonResponse({ errorInfo: { code: "busy" } }, 409);

    let response: Response;
    if (endpoint === "pairing") {
      response = method === "POST"
        ? this.handlePairingPost(side, action, form, fault?.mode)
        : jsonResponse(this.pairingStatus(side));
    } else if (endpoint === "participants") {
      response = method === "POST"
        ? this.handleParticipantPost(side, action, form)
        : jsonResponse(this.participantsStatus(side));
    } else if (endpoint === "descriptor") {
      response = jsonResponse({
        ok: true,
        protocolVersion: 1,
        participantId: side === "owner" ? this.ownerId : this.peerId,
        siteId: side === "owner" ? "site-main" : "site-peer",
        participantType: "namo_node",
        product: "namo-spatial-sensor",
        softwareVersion: "test"
      });
    } else {
      throw new Error(`unexpected request: ${url}`);
    }

    if (fault?.mode === "response_drop")
      throw new TypeError(`connection dropped after ${key}`);
    return response;
  };

  client() {
    return createPlatformPairingClient({
      fetcher: this.fetcher,
      sleep: async () => undefined,
      pollIntervalMs: 0,
      operationTimeoutMs: 1000
    });
  }

  inject(key: string, mode: FaultMode): void {
    this.faults.push({ key, mode });
  }

  clearFaults(): void {
    this.faults = [];
  }

  participantStatus(side: Side): PlatformParticipantsStatus {
    return this.participantsStatus(side);
  }

  markOwnerRecordActive(participantId: string): void {
    this.records.owner = this.records.owner.map((item) => item.participantId === participantId
      ? participant(participantId, "active")
      : item);
  }

  private runtime(role: RuntimeState["role"]): RuntimeState {
    return {
      state: "idle",
      role,
      acceptedOperationId: 0,
      completedOperationId: 0,
      lastOperation: "none",
      operationResult: "ok",
      commitResult: "ok",
      invitation: false
    };
  }

  private takeFault(key: string): InjectedFault | undefined {
    const index = this.faults.findIndex((fault) => fault.key === key);
    if (index < 0) return undefined;
    return this.faults.splice(index, 1)[0];
  }

  private handlePairingPost(
    side: Side,
    action: string,
    form: URLSearchParams,
    faultMode?: FaultMode
  ): Response {
    const runtime = this.runtimes[side];
    if (action === "invite") {
      runtime.invitation = true;
      return jsonResponse({ ok: true, invitationAccepted: true });
    }
    if (action === "rejectInvite") {
      runtime.invitation = false;
      return jsonResponse({ ok: true });
    }
    if (action === "dismissInvite") {
      runtime.invitation = false;
      return jsonResponse({ ok: true });
    }

    runtime.acceptedOperationId += 1;
    runtime.completedOperationId = runtime.acceptedOperationId;
    runtime.lastOperation = action as RuntimeState["lastOperation"];
    runtime.operationResult = "ok";
    runtime.commitResult = "ok";
    if (action === "prepare") {
      runtime.role = form.get("role") as RuntimeState["role"];
      runtime.state = "prepared";
      runtime.invitation = false;
    } else if (action === "exchange") {
      runtime.state = "exchanged";
    } else if (action === "verify") {
      runtime.state = "verified";
    } else if (action === "confirm") {
      if (form.get("confirmationCode") !== "123456") {
        runtime.operationResult = "confirmation_mismatch";
      } else {
        runtime.state = "committed";
        const remoteId = side === "owner" ? this.peerId : this.ownerId;
        if (!this.records[side].some((item) => item.participantId === remoteId))
          this.records[side].push(participant(remoteId, "quarantined"));
      }
    } else if (action === "cancel") {
      const role = runtime.role;
      Object.assign(runtime, this.runtime(role));
      runtime.acceptedOperationId = 1;
      runtime.completedOperationId = 1;
      runtime.lastOperation = "cancel";
    }
    if (faultMode === "invalid_operation_id")
      return jsonResponse({ operationId: 0 });
    return jsonResponse({ operationId: runtime.acceptedOperationId });
  }

  private handleParticipantPost(
    side: Side,
    action: string,
    form: URLSearchParams
  ): Response {
    if (action === "open")
      return jsonResponse(this.participantsStatus(side));
    const participantId = form.get("participantId") ?? "";
    if (action === "revoke") {
      this.records[side] = this.records[side].map((item) => item.participantId === participantId
        ? participant(participantId, "revoked")
        : item);
    } else if (action === "forget") {
      this.records[side] = this.records[side].filter((item) => item.participantId !== participantId);
    }
    return jsonResponse(this.participantsStatus(side));
  }

  private pairingStatus(side: Side): PlatformPairingStatus {
    const runtime = this.runtimes[side];
    const remoteId = side === "owner" ? this.peerId : this.ownerId;
    const offerRole = runtime.role;
    const hasOffer = runtime.state === "prepared" || runtime.state === "exchanged" || runtime.state === "verified";
    const hasProof = runtime.state === "exchanged" || runtime.state === "verified";
    return {
      ok: true,
      supported: true,
      busy: false,
      acceptedOperationId: runtime.acceptedOperationId,
      completedOperationId: runtime.completedOperationId,
      lastOperation: runtime.lastOperation,
      operationResult: runtime.operationResult,
      commitResult: runtime.commitResult,
      state: runtime.state,
      role: runtime.role,
      remainingMs: runtime.state === "idle" || runtime.state === "committed" ? 0 : 180000,
      peerVerified: runtime.state === "verified" || runtime.state === "committed",
      peerParticipantId: runtime.state === "idle" ? "" : remoteId,
      leaderSiteId: runtime.state === "committed" ? "site-main" : "",
      offer: hasOffer ? {
        role: offerRole,
        participantId: side === "owner" ? this.ownerId : this.peerId,
        siteId: side === "owner" ? "site-main" : "site-peer",
        sessionId: side === "owner" ? "11".repeat(16) : "22".repeat(16),
        nonce: side === "owner" ? "33".repeat(32) : "44".repeat(32),
        publicKey: `04${side === "owner" ? "55".repeat(64) : "66".repeat(64)}`
      } : null,
      localProof: hasProof ? (side === "owner" ? "aa".repeat(32) : "bb".repeat(32)) : null,
      confirmationCode: runtime.state === "verified" ? "123456" : null,
      invitation: runtime.invitation ? {
        state: "pending",
        requesterAddress: "192.168.1.10",
        remainingMs: 180000
      } : null
    } as PlatformPairingStatus;
  }

  private participantsStatus(side: Side): PlatformParticipantsStatus {
    return {
      ok: true,
      participants: this.records[side].map((item) => ({
        ...item,
        credential: { ...item.credential }
      }))
    };
  }
}

async function runPairingProtocol(harness: PairingHttpHarness): Promise<void> {
  const client = harness.client();
  await client.requestInvitation(harness.peerUrl, "192.168.1.10");
  await client.prepareJoiner(harness.peerUrl);
  const connection = await client.connectLeader(harness.ownerUrl, harness.peerUrl);
  await client.confirmPair(
    harness.ownerUrl,
    harness.peerUrl,
    connection.confirmationCode
  );
}

async function cancelBoth(harness: PairingHttpHarness): Promise<void> {
  harness.clearFaults();
  const client = harness.client();
  await Promise.allSettled([
    client.cancel(harness.ownerUrl),
    client.cancel(harness.peerUrl)
  ]);
}

describe("platform pairing relationship state matrix", () => {
  const states = ["missing", "quarantined", "active", "revoked", "unprovisioned"] as const;

  for (const localState of states) {
    for (const remoteState of states) {
      it(`classifies local=${localState}, remote=${remoteState} deterministically`, () => {
        const localParticipants = localState === "missing"
          ? []
          : [participant(
              "namo-node-peer",
              localState === "unprovisioned" ? "quarantined" : localState,
              localState !== "unprovisioned"
            )];
        const remoteParticipants = remoteState === "missing"
          ? []
          : [participant(
              "namo-node-owner",
              remoteState === "unprovisioned" ? "quarantined" : remoteState,
              remoteState !== "unprovisioned"
            )];
        const localVisible = localState === "quarantined" || localState === "active";
        const remoteVisible = remoteState === "quarantined" || remoteState === "active";
        const expected = localState === "quarantined" && remoteState === "quarantined"
          ? "resume"
          : !localVisible && !remoteVisible
            ? "new"
            : "blocked";

        expect(pairingSetupRelationship(
          { ok: true, participants: localParticipants },
          { ok: true, participants: remoteParticipants },
          "namo-node-owner",
          "namo-node-peer"
        )).toBe(expected);
      });
    }
  }

  for (let existing = 0; existing <= 5; existing += 1) {
    it(`keeps a reciprocal quarantine resumable with ${existing} existing active peers`, () => {
      const active = Array.from({ length: existing }, (_, index) =>
        participant(`namo-node-existing-${index}`, "active"));
      expect(pairingSetupRelationship(
        {
          ok: true,
          participants: [...active, participant("namo-node-peer", "quarantined")]
        },
        {
          ok: true,
          participants: [participant("namo-node-owner", "quarantined")]
        },
        "namo-node-owner",
        "namo-node-peer"
      )).toBe("resume");
    });
  }
});

describe("platform pairing HTTP fault matrix", () => {
  const precommitBoundaries = [
    "peer:pairing:invite",
    "peer:participants:open",
    "peer:pairing:prepare",
    "owner:participants:open",
    "owner:pairing:prepare",
    "owner:pairing:exchange",
    "peer:pairing:exchange",
    "owner:pairing:verify",
    "peer:pairing:verify"
  ];
  const faultModes: FaultMode[] = [
    "request_drop",
    "response_drop",
    "busy",
    "invalid_operation_id"
  ];

  for (const boundary of precommitBoundaries) {
    for (const mode of faultModes) {
      if (mode === "invalid_operation_id" &&
          (boundary.includes("participants") || boundary.endsWith(":invite")))
        continue;
      it(`fails closed and permits a clean retry after ${mode} at ${boundary}`, async () => {
        const harness = new PairingHttpHarness();
        harness.inject(boundary, mode);

        await expect(runPairingProtocol(harness)).rejects.toBeDefined();
        expect(harness.records.owner).toHaveLength(0);
        expect(harness.records.peer).toHaveLength(0);

        await cancelBoth(harness);
        await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
        expect(harness.records.owner.map((item) => item.participantId)).toEqual([harness.peerId]);
        expect(harness.records.peer.map((item) => item.participantId)).toEqual([harness.ownerId]);
      });
    }
  }

  for (const boundary of [
    "peer:pairing:get:prepare",
    "owner:pairing:get:prepare",
    "owner:pairing:get:exchange",
    "peer:pairing:get:exchange",
    "owner:pairing:get:verify",
    "peer:pairing:get:verify"
  ]) {
    it(`reconciles a completed operation after one status-read disconnect at ${boundary}`, async () => {
      const harness = new PairingHttpHarness();
      harness.inject(boundary, "request_drop");
      await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
      expect(harness.records.owner).toHaveLength(1);
      expect(harness.records.peer).toHaveLength(1);
    });
  }

  it("bounds consecutive status-read retries without replaying the mutation", async () => {
    const harness = new PairingHttpHarness();
    for (let attempt = 0; attempt < 5; attempt += 1)
      harness.inject("peer:pairing:get:prepare", "request_drop");

    await expect(runPairingProtocol(harness)).rejects.toBeDefined();
    expect(harness.requests.filter((key) => key === "peer:pairing:prepare"))
      .toHaveLength(1);
  });

  it("accepts a peer confirmation whose response disappears after persistence", async () => {
    const harness = new PairingHttpHarness();
    harness.inject("peer:pairing:confirm", "response_drop");
    await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
    expect(harness.records.owner).toHaveLength(1);
    expect(harness.records.peer).toHaveLength(1);
  });

  it("keeps a one-sided local confirmation fail-closed and removable", async () => {
    const harness = new PairingHttpHarness();
    harness.inject("owner:pairing:confirm", "response_drop");
    await expect(runPairingProtocol(harness)).rejects.toBeDefined();
    expect(harness.records.owner).toHaveLength(1);
    expect(harness.records.peer).toHaveLength(0);
    expect(pairingSetupRelationship(
      harness.participantStatus("owner"),
      harness.participantStatus("peer"),
      harness.ownerId,
      harness.peerId
    )).toBe("blocked");

    harness.clearFaults();
    await expect(harness.client().disconnectPair(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).resolves.toBeDefined();
    expect(harness.records.owner).toHaveLength(0);
    expect(harness.records.peer).toHaveLength(0);
  });
});

describe("platform pairing cancellation and capacity matrix", () => {
  const phases = ["invited", "joiner_prepared", "both_verified"] as const;
  for (const phase of phases) {
    it(`cancels ${phase} without leaving a registry record`, async () => {
      const harness = new PairingHttpHarness();
      const client = harness.client();
      await client.requestInvitation(harness.peerUrl, "192.168.1.10");
      if (phase !== "invited")
        await client.prepareJoiner(harness.peerUrl);
      if (phase === "both_verified")
        await client.connectLeader(harness.ownerUrl, harness.peerUrl);

      await cancelBoth(harness);
      expect((await client.getStatus(harness.ownerUrl)).state).toBe("idle");
      expect((await client.getStatus(harness.peerUrl)).state).toBe("idle");
      expect(harness.records.owner).toHaveLength(0);
      expect(harness.records.peer).toHaveLength(0);
      await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
    });
  }

  for (const side of ["owner", "peer"] as const) {
    for (const mode of ["request_drop", "response_drop", "busy"] as const) {
      it(`converges after ${mode} interrupts ${side} cancellation`, async () => {
        const harness = new PairingHttpHarness();
        const client = harness.client();
        await client.requestInvitation(harness.peerUrl, "192.168.1.10");
        await client.prepareJoiner(harness.peerUrl);
        await client.connectLeader(harness.ownerUrl, harness.peerUrl);
        harness.inject(`${side}:pairing:cancel`, mode);
        await Promise.allSettled([
          client.cancel(harness.ownerUrl),
          client.cancel(harness.peerUrl)
        ]);

        await cancelBoth(harness);
        expect((await client.getStatus(harness.ownerUrl)).state).toBe("idle");
        expect((await client.getStatus(harness.peerUrl)).state).toBe("idle");
        expect(harness.records.owner).toHaveLength(0);
        expect(harness.records.peer).toHaveLength(0);
      });
    }
  }

  it("keeps invitation rejection and dismissal free of persistent state", async () => {
    const harness = new PairingHttpHarness();
    const client = harness.client();
    await client.requestInvitation(harness.peerUrl, "192.168.1.10");
    await client.rejectInvitation(harness.peerUrl);
    expect(harness.records.peer).toHaveLength(0);
    expect((await client.getStatus(harness.peerUrl)).state).toBe("idle");

    await client.requestInvitation(harness.peerUrl, "192.168.1.10");
    await client.dismissInvitation(harness.peerUrl);
    expect(harness.records.peer).toHaveLength(0);
    expect((await client.getStatus(harness.peerUrl)).state).toBe("idle");
  });

  it("rejects a wrong confirmation code without persisting either side", async () => {
    const harness = new PairingHttpHarness();
    const client = harness.client();
    await client.requestInvitation(harness.peerUrl, "192.168.1.10");
    await client.prepareJoiner(harness.peerUrl);
    await client.connectLeader(harness.ownerUrl, harness.peerUrl);
    await expect(client.confirmPair(
      harness.ownerUrl,
      harness.peerUrl,
      "654321"
    )).rejects.toMatchObject({ code: "confirmation_mismatch" });
    expect(harness.records.owner).toHaveLength(0);
    expect(harness.records.peer).toHaveLength(0);
    await cancelBoth(harness);
    await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
  });

  it("reports a pairing operation that remains busy past its bounded deadline", async () => {
    let operationAccepted = false;
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      if (url.endsWith("/api/platform/participants"))
        return jsonResponse({ ok: true, participants: [] });
      if (init?.method === "POST") {
        operationAccepted = true;
        return jsonResponse({ operationId: 1 });
      }
      return jsonResponse({
        ok: true,
        supported: true,
        busy: true,
        acceptedOperationId: 1,
        completedOperationId: 0,
        lastOperation: "prepare",
        operationResult: "ok",
        commitResult: "ok",
        state: "idle",
        role: "joiner",
        remainingMs: 0,
        peerVerified: false,
        peerParticipantId: "",
        leaderSiteId: "",
        offer: null,
        localProof: null,
        confirmationCode: null
      });
    };
    const client = createPlatformPairingClient({
      fetcher,
      sleep: async () => undefined,
      operationTimeoutMs: -1
    });
    await expect(client.prepareJoiner("http://peer"))
      .rejects.toMatchObject({ code: "pairing_operation_timeout" });
    expect(operationAccepted).toBe(true);
  });

  it("adds peers sequentially up to the seven-device Group contract", async () => {
    let ownerRecords: PlatformParticipant[] = [];
    for (let peerIndex = 1; peerIndex <= 6; peerIndex += 1) {
      const peerId = `namo-node-peer-${peerIndex}`;
      const harness = new PairingHttpHarness(peerId, ownerRecords);
      expect(pairingSetupRelationship(
        harness.participantStatus("owner"),
        harness.participantStatus("peer"),
        harness.ownerId,
        peerId
      )).toBe("new");
      await runPairingProtocol(harness);
      harness.markOwnerRecordActive(peerId);
      ownerRecords = harness.records.owner;
      expect(ownerRecords).toHaveLength(peerIndex);
      expect(new Set(ownerRecords.map((item) => item.participantId)).size).toBe(peerIndex);
    }
    expect(ownerRecords).toHaveLength(6);
  });

  it("blocks a second setup attempt while the bilateral relationship is active", async () => {
    const harness = new PairingHttpHarness();
    await runPairingProtocol(harness);
    harness.markOwnerRecordActive(harness.peerId);
    harness.records.peer = [participant(harness.ownerId, "active")];
    expect(pairingSetupRelationship(
      harness.participantStatus("owner"),
      harness.participantStatus("peer"),
      harness.ownerId,
      harness.peerId
    )).toBe("blocked");
  });

  it("allows a clean rejoin only after both persisted records are removed", async () => {
    const harness = new PairingHttpHarness();
    await runPairingProtocol(harness);
    const client = harness.client();
    await client.disconnectPair(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    );
    expect(pairingSetupRelationship(
      harness.participantStatus("owner"),
      harness.participantStatus("peer"),
      harness.ownerId,
      harness.peerId
    )).toBe("new");
    await cancelBoth(harness);
    await expect(runPairingProtocol(harness)).resolves.toBeUndefined();
    expect(harness.records.owner).toHaveLength(1);
    expect(harness.records.peer).toHaveLength(1);
  });
});

describe("platform pairing disconnect fault matrix", () => {
  const boundaries = [
    "peer:participants:revoke",
    "peer:participants:forget",
    "owner:participants:revoke",
    "owner:participants:forget"
  ];
  const modes: FaultMode[] = ["request_drop", "response_drop", "busy"];

  for (const boundary of boundaries) {
    for (const mode of modes) {
      it(`converges removal after ${mode} at ${boundary}`, async () => {
        const harness = new PairingHttpHarness();
        await runPairingProtocol(harness);
        harness.inject(boundary, mode);
        await expect(harness.client().disconnectPair(
          harness.ownerUrl,
          harness.peerUrl,
          harness.ownerId,
          harness.peerId
        )).resolves.toBeDefined();
        expect(harness.records.owner).toHaveLength(0);
        expect(harness.records.peer).toHaveLength(0);
      });
    }
  }

  it("does not touch the local retry anchor while the peer identity is unreachable", async () => {
    const harness = new PairingHttpHarness();
    await runPairingProtocol(harness);
    harness.inject("peer:descriptor:get", "request_drop");
    await expect(harness.client().disconnectPair(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toBeDefined();
    expect(harness.records.owner).toHaveLength(1);
    expect(harness.records.peer).toHaveLength(1);
  });

  it("finishes on retry after the peer was removed but the local removal was interrupted", async () => {
    const harness = new PairingHttpHarness();
    await runPairingProtocol(harness);
    harness.inject("owner:participants:revoke", "request_drop");
    harness.inject("owner:participants:revoke", "request_drop");
    harness.inject("owner:participants:revoke", "request_drop");
    await expect(harness.client().disconnectPair(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).rejects.toBeDefined();
    expect(harness.records.peer).toHaveLength(0);
    expect(harness.records.owner).toHaveLength(1);

    harness.clearFaults();
    await expect(harness.client().disconnectPair(
      harness.ownerUrl,
      harness.peerUrl,
      harness.ownerId,
      harness.peerId
    )).resolves.toBeDefined();
    expect(harness.records.owner).toHaveLength(0);
    expect(harness.records.peer).toHaveLength(0);
  });
});
