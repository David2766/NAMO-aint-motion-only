import { describe, expect, it, vi } from "vitest";
import {
  PlatformPairingError,
  type PlatformSiteConfigurationStatus
} from "../api/platform-pairing";
import {
  runSharedFloorplanMutationAfterContinuity,
  waitForSharedFloorplanWritable
} from "./shared-floorplan-write";

function site(overrides: Partial<PlatformSiteConfigurationStatus> = {}): PlatformSiteConfigurationStatus {
  return {
    configurationConsistent: true,
    configurationOwnerNodeId: "owner",
    localOwner: true,
    sharedWritesAllowed: true,
    ...overrides
  } as PlatformSiteConfigurationStatus;
}

describe("shared Floorplan write wait", () => {
  it("waits through replica convergence and returns the writable revision", async () => {
    const states = [
      site({ sharedWritesAllowed: false, generation: 4 }),
      site({ sharedWritesAllowed: false, generation: 4 }),
      site({ sharedWritesAllowed: true, generation: 4 })
    ];
    const waiting: number[] = [];

    const result = await waitForSharedFloorplanWritable({
      readSite: vi.fn(async () => states.shift() ?? site()),
      sleep: vi.fn(async () => undefined),
      timeoutMs: 1000,
      pollIntervalMs: 1,
      onWaiting: (current) => waiting.push(current.generation)
    });

    expect(result.sharedWritesAllowed).toBe(true);
    expect(waiting).toEqual([4, 4]);
  });

  it("waits through a transient configuration mismatch after pairing", async () => {
    const states = [
      site({ configurationConsistent: false, sharedWritesAllowed: false }),
      site({ configurationConsistent: false, sharedWritesAllowed: false }),
      site({ configurationConsistent: true, sharedWritesAllowed: true })
    ];
    const mutation = vi.fn(async () => "committed");

    await expect(runSharedFloorplanMutationAfterContinuity({
      readSite: vi.fn(async () => states.shift() ?? site()),
      mutation,
      sleep: vi.fn(async () => undefined),
      timeoutMs: 1000,
      pollIntervalMs: 1
    })).resolves.toBe("committed");
    expect(mutation).toHaveBeenCalledTimes(1);
  });

  it("keeps waiting after a transient Site status request failure", async () => {
    const readSite = vi.fn()
      .mockRejectedValueOnce(new TypeError("connection reset"))
      .mockResolvedValueOnce(site());

    await expect(waitForSharedFloorplanWritable({
      readSite,
      sleep: vi.fn(async () => undefined),
      timeoutMs: 1000,
      pollIntervalMs: 1
    })).resolves.toMatchObject({ sharedWritesAllowed: true });
    expect(readSite).toHaveBeenCalledTimes(2);
  });

  it("reports a configuration conflict when convergence stays mismatched", async () => {
    await expect(waitForSharedFloorplanWritable({
      readSite: async () => site({
        configurationConsistent: false,
        sharedWritesAllowed: false
      }),
      sleep: async () => undefined,
      timeoutMs: 0
    })).rejects.toMatchObject({ code: "site_configuration_conflict" });
  });

  it("fails immediately when called on a non-owner", async () => {
    await expect(waitForSharedFloorplanWritable({
      readSite: async () => site({ localOwner: false }),
      sleep: async () => undefined
    })).rejects.toMatchObject({ code: "site_configuration_owner_required" });
  });

  it("keeps the continuity error for an offline or stalled successor", async () => {
    await expect(waitForSharedFloorplanWritable({
      readSite: async () => site({ sharedWritesAllowed: false }),
      sleep: async () => undefined,
      timeoutMs: 0
    })).rejects.toMatchObject({ code: "site_configuration_continuity_unavailable" });
  });

  it("runs a shared mutation only after continuity converges", async () => {
    const states = [
      site({ sharedWritesAllowed: false }),
      site({ sharedWritesAllowed: true })
    ];
    const mutation = vi.fn(async () => "committed");

    await expect(runSharedFloorplanMutationAfterContinuity({
      readSite: vi.fn(async () => states.shift() ?? site()),
      mutation,
      sleep: vi.fn(async () => undefined),
      pollIntervalMs: 1
    })).resolves.toBe("committed");
    expect(mutation).toHaveBeenCalledTimes(1);
  });

  it("rechecks continuity once when the commit races a new revision", async () => {
    const mutation = vi.fn()
      .mockRejectedValueOnce(new PlatformPairingError(
        "site_configuration_continuity_unavailable",
        409,
        "site_configuration_continuity_unavailable"
      ))
      .mockResolvedValueOnce("committed");

    await expect(runSharedFloorplanMutationAfterContinuity({
      readSite: vi.fn(async () => site()),
      mutation,
      sleep: vi.fn(async () => undefined)
    })).resolves.toBe("committed");
    expect(mutation).toHaveBeenCalledTimes(2);
  });

  it("rechecks a transient configuration conflict returned by the commit", async () => {
    const mutation = vi.fn()
      .mockRejectedValueOnce(new PlatformPairingError(
        "site_configuration_conflict",
        409,
        "site_configuration_conflict"
      ))
      .mockResolvedValueOnce("committed");
    const states = [
      site(),
      site({ configurationConsistent: false, sharedWritesAllowed: false }),
      site()
    ];

    await expect(runSharedFloorplanMutationAfterContinuity({
      readSite: vi.fn(async () => states.shift() ?? site()),
      mutation,
      sleep: vi.fn(async () => undefined),
      timeoutMs: 1000,
      pollIntervalMs: 1
    })).resolves.toBe("committed");
    expect(mutation).toHaveBeenCalledTimes(2);
  });

  it("does not retry unrelated shared mutation failures", async () => {
    const mutation = vi.fn(async () => {
      throw new PlatformPairingError("floorplan_manifest_invalid", 409, "floorplan_manifest_invalid");
    });

    await expect(runSharedFloorplanMutationAfterContinuity({
      readSite: vi.fn(async () => site()),
      mutation,
      sleep: vi.fn(async () => undefined)
    })).rejects.toMatchObject({ code: "floorplan_manifest_invalid" });
    expect(mutation).toHaveBeenCalledTimes(1);
  });
});
