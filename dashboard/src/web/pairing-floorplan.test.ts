import { describe, expect, it } from "vitest";
import { buildFloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import type { FloorplanStorageStatus } from "./floorplan/floorplan-storage-client";
import {
  copyPairingFloorplanToLocal,
  loadPairingFloorplanDocument,
  pairingCalibrationPlacementGate,
  pairingRadarPlacementReadiness,
  pairingFloorplanDecision,
  pairingFloorplansMatch
} from "./pairing-floorplan";

function status(overrides: Partial<FloorplanStorageStatus> = {}): FloorplanStorageStatus {
  return {
    ok: true,
    storage: "spiffs",
    partition: "spiffs",
    hasConfig: true,
    hasImage: true,
    totalBytes: 1024,
    usedBytes: 100,
    configBytes: 60,
    imageBytes: 40,
    configHash: 11,
    imageHash: 22,
    uploadTarget: 0,
    uploadSize: 0,
    uploadWritten: 0,
    ...overrides
  };
}

describe("pairing floorplan selection", () => {
  it("requires at least one complete floorplan", () => {
    const empty = status({ hasConfig: false, hasImage: false });
    expect(pairingFloorplanDecision(empty, empty)).toBe("none");
  });

  it("automatically chooses the only complete floorplan", () => {
    const empty = status({ hasConfig: false, hasImage: false });
    expect(pairingFloorplanDecision(status(), empty)).toBe("local");
    expect(pairingFloorplanDecision(empty, status())).toBe("peer");
  });

  it("does not ask the user to choose when both stored revisions match", () => {
    expect(pairingFloorplanDecision(status(), status())).toBe("local");
  });

  it("requires an explicit choice when both devices contain different floorplans", () => {
    expect(pairingFloorplanDecision(status(), status({ imageHash: 23 }))).toBe("choose");
  });

  it("uses floorplan identity instead of mistaking local radar differences for different room geometry", () => {
    const localIdentity = { floorplanId: "fp-shared", geometryVersion: 4 };
    const peerIdentity = { floorplanId: "fp-shared", geometryVersion: 4 };
    expect(pairingFloorplanDecision(
      status({ configHash: 101 }),
      status({ configHash: 202 }),
      localIdentity,
      peerIdentity
    )).toBe("local");
  });

  it("chooses the newer geometry revision when both copies share an identity", () => {
    expect(pairingFloorplanDecision(
      status(),
      status(),
      { floorplanId: "fp-shared", geometryVersion: 4 },
      { floorplanId: "fp-shared", geometryVersion: 5 }
    )).toBe("peer");
  });

  it("keeps a resumable pairing at Floorplan selection when the peer has no Floorplan", () => {
    const empty = status({ hasConfig: false, hasImage: false });
    expect(pairingFloorplansMatch(status(), empty, false)).toBe(false);
  });

  it("keeps a resumable pairing at Floorplan selection when payloads differ", () => {
    expect(pairingFloorplansMatch(status(), status({ imageHash: 23 }), false)).toBe(false);
  });

  it("allows a resumable pairing to continue when the existing Floorplans match", () => {
    expect(pairingFloorplansMatch(status(), status(), false)).toBe(true);
    expect(pairingFloorplansMatch(
      status({ configHash: 101 }),
      status({ configHash: 202 }),
      true
    )).toBe(true);
  });

  it("keeps device-config radar placement when importing the first floorplan", async () => {
    const remoteDocument = buildFloorplanStorageDocument({
      image: { width: 200, height: 100 },
      scale: {
        widthMm: 4000,
        heightMm: 2000,
        outerBounds: { x: 0, y: 0, width: 200, height: 100 },
        mmPerPxX: 20,
        mmPerPxY: 20
      },
      radar: { originX: 50, originY: 60, rotation: 10, scale: 1 },
      rooms: [],
      wallSegments: []
    });
    const uploads: string[] = [];
    const fetcher: typeof fetch = async (input, init) => {
      const url = String(input);
      if (url === "http://peer/api/floorplan") {
        return new Response(JSON.stringify(remoteDocument), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://peer/api/floorplan/image") {
        return new Response(new Blob(["image"], { type: "image/webp" }));
      }
      if (url === "http://local/api/floorplan/status") {
        return new Response(JSON.stringify(status({ hasConfig: false, hasImage: false })), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://local/api/config") {
        return new Response(JSON.stringify({
          version: 1,
          zones: [],
          floorplan: {
            enabled: false,
            hasImage: false,
            radar: {
              originX: 120,
              originY: 140,
              rotation: 35,
              scale: 0.9,
              poseVersion: 7
            }
          }
        }), { headers: { "Content-Type": "application/json" } });
      }
      if (url.startsWith("http://local/api/floorplan/upload/") && init?.method === "POST") {
        uploads.push(new URLSearchParams(String(init.body)).get("target") ?? "");
        return new Response("ok");
      }
      throw new Error(`unexpected request: ${url}`);
    };

    const imported = await copyPairingFloorplanToLocal(
      "peer",
      { baseUrl: "http://local", fetcher },
      { baseUrl: "http://peer", fetcher }
    );

    expect(imported.radar).toMatchObject({
      originPx: [120, 140],
      rotationDeg: 35,
      scale: 0.9,
      poseVersion: 7
    });
    expect(uploads).toContain("image");
    expect(uploads).toContain("config");
  });

  it("overlays a replicated floorplan with each device's local radar sidecar", async () => {
    const sharedDocument = buildFloorplanStorageDocument({
      image: { width: 200, height: 100 },
      scale: {
        widthMm: 4000,
        heightMm: 2000,
        outerBounds: { x: 0, y: 0, width: 200, height: 100 },
        mmPerPxX: 20,
        mmPerPxY: 20
      },
      radar: { originX: 50, originY: 60, rotation: 10, scale: 1 },
      rooms: [],
      wallSegments: []
    });
    const fetcher: typeof fetch = async (input) => {
      const url = String(input);
      if (url === "http://peer/api/floorplan") {
        return new Response(JSON.stringify(sharedDocument), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://peer/api/config") {
        return new Response(JSON.stringify({
          version: 1,
          zones: [],
          floorplan: {
            enabled: true,
            hasImage: true,
            radar: {
              originX: 145,
              originY: 35,
              rotation: 92,
              scale: 0.85,
              poseVersion: 11
            }
          }
        }), { headers: { "Content-Type": "application/json" } });
      }
      throw new Error(`unexpected request: ${url}`);
    };

    const localized = await loadPairingFloorplanDocument({
      baseUrl: "http://peer",
      fetcher
    });

    expect(localized.radar).toMatchObject({
      originPx: [145, 35],
      rotationDeg: 92,
      scale: 0.85,
      poseVersion: 11
    });
    expect(sharedDocument.radar).toMatchObject({
      originPx: [50, 60],
      rotationDeg: 10
    });
  });

  it("uses pose version one for a legacy local radar sidecar", async () => {
    const sharedDocument = buildFloorplanStorageDocument({
      image: { width: 200, height: 100 },
      scale: {
        widthMm: 4000,
        heightMm: 2000,
        outerBounds: { x: 0, y: 0, width: 200, height: 100 },
        mmPerPxX: 20,
        mmPerPxY: 20
      },
      radar: { originX: 50, originY: 60, rotation: 10, scale: 1 },
      rooms: [],
      wallSegments: []
    });
    sharedDocument.radar.poseVersion = 9;
    const fetcher: typeof fetch = async (input) => {
      const url = String(input);
      if (url === "http://peer/api/floorplan") {
        return new Response(JSON.stringify(sharedDocument), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://peer/api/config") {
        return new Response(JSON.stringify({
          version: 1,
          zones: [],
          floorplan: {
            enabled: true,
            hasImage: true,
            radar: {
              originX: 145,
              originY: 35,
              rotation: 92,
              scale: 0.85
            }
          }
        }), { headers: { "Content-Type": "application/json" } });
      }
      throw new Error(`unexpected request: ${url}`);
    };

    const localized = await loadPairingFloorplanDocument({
      baseUrl: "http://peer",
      fetcher
    });

    expect(localized.radar.poseVersion).toBe(1);
  });

  it("retries the required local sidecar without borrowing the shared owner pose", async () => {
    const sharedDocument = buildFloorplanStorageDocument({
      image: { width: 200, height: 100 },
      scale: {
        widthMm: 4000,
        heightMm: 2000,
        outerBounds: { x: 0, y: 0, width: 200, height: 100 },
        mmPerPxX: 20,
        mmPerPxY: 20
      },
      radar: { originX: 50, originY: 60, rotation: 10, scale: 1 },
      rooms: [],
      wallSegments: []
    });
    sharedDocument.radar.poseVersion = 4;
    let configRequests = 0;
    const fetcher: typeof fetch = async (input) => {
      const url = String(input);
      if (url === "http://peer/api/floorplan") {
        return new Response(JSON.stringify(sharedDocument), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://peer/api/config") {
        configRequests += 1;
        if (configRequests === 1) return new Response("busy", { status: 503 });
        return new Response(
          JSON.stringify({
            version: 1,
            zones: [],
            floorplan: {
              radar: {
                originX: 145,
                originY: 35,
                rotation: 92,
                scale: 0.85,
                poseVersion: 3
              }
            }
          }),
          { headers: { "Content-Type": "application/json" } }
        );
      }
      throw new Error(`unexpected request: ${url}`);
    };

    const localized = await loadPairingFloorplanDocument({
      baseUrl: "http://peer",
      fetcher,
      requireLocalRadarPlacement: true,
      localRadarAttempts: 2,
      localRadarRetryDelayMs: 0
    });

    expect(configRequests).toBe(2);
    expect(localized.radar.poseVersion).toBe(3);
    expect(localized.radar.originPx).toEqual([145, 35]);
  });

  it("fails closed when a paired Node local sidecar cannot be read", async () => {
    const sharedDocument = buildFloorplanStorageDocument({
      image: { width: 200, height: 100 },
      scale: {
        widthMm: 4000,
        heightMm: 2000,
        outerBounds: { x: 0, y: 0, width: 200, height: 100 },
        mmPerPxX: 20,
        mmPerPxY: 20
      },
      radar: { originX: 50, originY: 60, rotation: 10, scale: 1 },
      rooms: [],
      wallSegments: []
    });
    sharedDocument.radar.poseVersion = 4;
    const fetcher: typeof fetch = async (input) => {
      const url = String(input);
      if (url === "http://peer/api/floorplan") {
        return new Response(JSON.stringify(sharedDocument), {
          headers: { "Content-Type": "application/json" }
        });
      }
      if (url === "http://peer/api/config") {
        return new Response("unavailable", { status: 503 });
      }
      throw new Error(`unexpected request: ${url}`);
    };

    await expect(
      loadPairingFloorplanDocument({
        baseUrl: "http://peer",
        fetcher,
        requireLocalRadarPlacement: true,
        localRadarAttempts: 2,
        localRadarRetryDelayMs: 0
      })
    ).rejects.toThrow("calibration_pose_missing");
  });

  it("reports a saved local radar placement as ready", async () => {
    const fetcher: typeof fetch = async () =>
      new Response(
        JSON.stringify({
          version: 1,
          zones: [],
          floorplan: {
            radar: {
              originX: 120,
              originY: 140,
              rotation: 35,
              scale: 0.9,
              poseVersion: 7
            }
          }
        }),
        { headers: { "Content-Type": "application/json" } }
      );

    await expect(
      pairingRadarPlacementReadiness({
        baseUrl: "http://device",
        fetcher,
        retryDelayMs: 0
      })
    ).resolves.toEqual({ state: "ready", poseVersion: 7 });
  });

  it("reports a visible default without a local radar sidecar as missing", async () => {
    const fetcher: typeof fetch = async () =>
      new Response(
        JSON.stringify({
          version: 1,
          zones: [],
          floorplan: { enabled: true, hasImage: true }
        }),
        { headers: { "Content-Type": "application/json" } }
      );

    await expect(
      pairingRadarPlacementReadiness({
        baseUrl: "http://device",
        fetcher,
        retryDelayMs: 0
      })
    ).resolves.toEqual({ state: "missing" });
  });

  it("accepts a radar placement that finishes saving during the bounded recheck", async () => {
    let requests = 0;
    const fetcher: typeof fetch = async () => {
      requests += 1;
      return new Response(JSON.stringify({
        version: 1,
        zones: [],
        floorplan: requests === 1 ? {} : {
          radar: {
            originX: 120,
            originY: 140,
            rotation: 35,
            scale: 0.9,
            poseVersion: 8
          }
        }
      }), { headers: { "Content-Type": "application/json" } });
    };

    await expect(pairingRadarPlacementReadiness({
      baseUrl: "http://device",
      fetcher,
      attempts: 2,
      retryDelayMs: 0
    })).resolves.toEqual({ state: "ready", poseVersion: 8 });
    expect(requests).toBe(2);
  });

  it("treats corrupt or uncommitted radar coordinates as missing", async () => {
    const fetcher: typeof fetch = async () =>
      new Response(
        JSON.stringify({
          version: 1,
          zones: [],
          floorplan: {
            radar: {
              originX: null,
              originY: 140,
              rotation: 35,
              scale: 0.9,
              poseVersion: 0
            }
          }
        }),
        { headers: { "Content-Type": "application/json" } }
      );

    await expect(
      pairingRadarPlacementReadiness({
        baseUrl: "http://device",
        fetcher,
        retryDelayMs: 0
      })
    ).resolves.toEqual({ state: "missing" });
  });

  it("keeps an unreachable device distinct from a missing radar placement", async () => {
    let requests = 0;
    const fetcher: typeof fetch = async () => {
      requests += 1;
      return new Response("busy", { status: 503 });
    };

    await expect(
      pairingRadarPlacementReadiness({
        baseUrl: "http://device",
        fetcher,
        attempts: 2,
        retryDelayMs: 0
      })
    ).resolves.toEqual({ state: "unavailable" });
    expect(requests).toBe(2);
  });

  it("identifies every device whose saved radar placement is missing", async () => {
    const readyFetcher: typeof fetch = async () => new Response(JSON.stringify({
      floorplan: {
        radar: { originX: 100, originY: 120, rotation: 0, scale: 1, poseVersion: 4 }
      }
    }), { headers: { "Content-Type": "application/json" } });
    const missingFetcher: typeof fetch = async () => new Response(JSON.stringify({
      floorplan: { enabled: true }
    }), { headers: { "Content-Type": "application/json" } });

    await expect(pairingCalibrationPlacementGate(
      { baseUrl: "http://local", fetcher: missingFetcher, attempts: 1 },
      { baseUrl: "http://peer", fetcher: readyFetcher, attempts: 1 }
    )).resolves.toEqual({ state: "missing", devices: ["local"] });
    await expect(pairingCalibrationPlacementGate(
      { baseUrl: "http://local", fetcher: missingFetcher, attempts: 1 },
      { baseUrl: "http://peer", fetcher: missingFetcher, attempts: 1 }
    )).resolves.toEqual({ state: "missing", devices: ["local", "peer"] });
  });

  it("does not misreport an unreachable device as a missing placement", async () => {
    const unavailableFetcher: typeof fetch = async () => new Response("busy", { status: 503 });
    const missingFetcher: typeof fetch = async () => new Response(JSON.stringify({
      floorplan: { enabled: true }
    }), { headers: { "Content-Type": "application/json" } });

    await expect(pairingCalibrationPlacementGate(
      { baseUrl: "http://local", fetcher: missingFetcher, attempts: 1 },
      { baseUrl: "http://peer", fetcher: unavailableFetcher, attempts: 1 }
    )).resolves.toEqual({ state: "unavailable" });
  });
});
