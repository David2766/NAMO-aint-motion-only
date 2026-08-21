import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import type { WebDeviceConfig } from "../core/types";
import type { PlatformSiteConfigurationStatus } from "./api/platform-pairing";
import {
  floorplanStorageRevision,
  hasCompleteFloorplanStorage,
  loadFloorplanStorageDocument,
  loadFloorplanStorageImage,
  loadFloorplanStorageStatus,
  saveFloorplanStorage,
  type FloorplanStorageClientOptions,
  type FloorplanStorageStatus
} from "./floorplan/floorplan-storage-client";
import { withLocalFloorplanRadarPlacement } from "./floorplan/stored-floorplan-document";

interface PairingFloorplanLoadOptions extends FloorplanStorageClientOptions {
  requireLocalRadarPlacement?: boolean;
  localRadarAttempts?: number;
  localRadarRetryDelayMs?: number;
}

export interface PairingRadarPlacementReadinessOptions extends FloorplanStorageClientOptions {
  attempts?: number;
  retryDelayMs?: number;
}

export type PairingRadarPlacementReadiness =
  | { state: "ready"; poseVersion: number }
  | { state: "missing" }
  | { state: "unavailable" };

export type PairingRadarPlacementDevice = "local" | "peer";

export type PairingCalibrationPlacementGate =
  | { state: "ready" }
  | { state: "missing"; devices: PairingRadarPlacementDevice[] }
  | { state: "unavailable" };

export type PairingFloorplanSource = "local" | "peer";
export type PairingFloorplanDecision = PairingFloorplanSource | "choose" | "none";

export interface PairingFloorplanIdentity {
  floorplanId: string;
  geometryVersion: number;
}

export function pairingFloorplanDecision(
  local: FloorplanStorageStatus,
  peer: FloorplanStorageStatus,
  localIdentity?: PairingFloorplanIdentity | null,
  peerIdentity?: PairingFloorplanIdentity | null
): PairingFloorplanDecision {
  const localReady = hasCompleteFloorplanStorage(local);
  const peerReady = hasCompleteFloorplanStorage(peer);
  if (!localReady && !peerReady) return "none";
  if (localReady && !peerReady) return "local";
  if (!localReady && peerReady) return "peer";
  if (localIdentity && peerIdentity && localIdentity.floorplanId === peerIdentity.floorplanId) {
    if (peerIdentity.geometryVersion > localIdentity.geometryVersion) return "peer";
    return "local";
  }
  return floorplanStorageRevision(local) === floorplanStorageRevision(peer) ? "local" : "choose";
}

export function pairingFloorplansMatch(
  local: FloorplanStorageStatus | null,
  peer: FloorplanStorageStatus | null,
  sharedIdentity: boolean
): boolean {
  if (sharedIdentity) return true;
  return Boolean(
    local &&
    peer &&
    hasCompleteFloorplanStorage(local) &&
    hasCompleteFloorplanStorage(peer) &&
    floorplanStorageRevision(local) === floorplanStorageRevision(peer)
  );
}

export async function copyPairingFloorplanToLocal(
  source: PairingFloorplanSource,
  localOptions: FloorplanStorageClientOptions,
  peerOptions: FloorplanStorageClientOptions
): Promise<FloorplanStorageDocument> {
  if (source === "local") return loadFloorplanStorageDocument(localOptions);

  const [peerDocument, peerImage, localPlacement] = await Promise.all([
    loadFloorplanStorageDocument(peerOptions),
    loadFloorplanStorageImage(peerOptions),
    loadLocalRadarPlacement(localOptions)
  ]);
  const document = withLocalFloorplanRadarPlacement(peerDocument, localPlacement);
  await saveFloorplanStorage({ document, image: peerImage }, localOptions);
  return document;
}

export function sharedFloorplanIsReady(status: PlatformSiteConfigurationStatus): boolean {
  return status.floorplan.committed && status.floorplan.geometryVersion > 0;
}

export async function loadPairingFloorplanDocument(
  options: PairingFloorplanLoadOptions
): Promise<FloorplanStorageDocument> {
  const [document, localPlacement] = await Promise.all([
    loadFloorplanStorageDocument(options),
    options.requireLocalRadarPlacement
      ? loadRequiredDeviceConfigRadarPlacement(options)
      : loadDeviceConfigRadarPlacement(options)
  ]);
  return withLocalFloorplanRadarPlacement(document, localPlacement);
}

export async function pairingRadarPlacementReadiness(
  options: PairingRadarPlacementReadinessOptions
): Promise<PairingRadarPlacementReadiness> {
  const attempts = Math.max(1, Math.min(5, options.attempts ?? 3));
  const retryDelayMs = Math.max(0, options.retryDelayMs ?? 300);
  let missingObserved = false;
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      const placement = await requestDeviceConfigRadarPlacement(options);
      if (placement) return { state: "ready", poseVersion: placement.poseVersion };
      missingObserved = true;
    } catch {
      // Keep request failures separate from a confirmed missing local sidecar.
    }
    if (attempt < attempts && retryDelayMs > 0) {
      await new Promise((resolve) => globalThis.setTimeout(resolve, retryDelayMs));
    }
  }
  return { state: missingObserved ? "missing" : "unavailable" };
}

export async function pairingCalibrationPlacementGate(
  localOptions: PairingRadarPlacementReadinessOptions,
  peerOptions: PairingRadarPlacementReadinessOptions
): Promise<PairingCalibrationPlacementGate> {
  const [local, peer] = await Promise.all([
    pairingRadarPlacementReadiness(localOptions),
    pairingRadarPlacementReadiness(peerOptions)
  ]);
  if (local.state === "unavailable" || peer.state === "unavailable") {
    return { state: "unavailable" };
  }
  const devices: PairingRadarPlacementDevice[] = [];
  if (local.state === "missing") devices.push("local");
  if (peer.state === "missing") devices.push("peer");
  return devices.length > 0 ? { state: "missing", devices } : { state: "ready" };
}

async function loadLocalRadarPlacement(options: FloorplanStorageClientOptions) {
  const configuredPlacement = await loadDeviceConfigRadarPlacement(options);
  if (configuredPlacement) return configuredPlacement;

  try {
    const status = await loadFloorplanStorageStatus(options);
    if (hasCompleteFloorplanStorage(status)) {
      const document = await loadFloorplanStorageDocument(options);
      return {
        originX: document.radar.originPx[0],
        originY: document.radar.originPx[1],
        rotation: document.radar.rotationDeg,
        scale: document.radar.scale,
        poseVersion: document.radar.poseVersion
      };
    }
  } catch {
    // A device without a stored floorplan has no legacy placement fallback.
  }

  return null;
}

async function loadDeviceConfigRadarPlacement(options: FloorplanStorageClientOptions) {
  try {
    return await requestDeviceConfigRadarPlacement(options);
  } catch {
    return null;
  }
}

async function loadRequiredDeviceConfigRadarPlacement(options: PairingFloorplanLoadOptions) {
  const attempts = Math.max(1, Math.min(5, options.localRadarAttempts ?? 3));
  const retryDelayMs = Math.max(0, options.localRadarRetryDelayMs ?? 300);
  for (let attempt = 1; attempt <= attempts; attempt += 1) {
    try {
      const placement = await requestDeviceConfigRadarPlacement(options);
      if (placement) return placement;
    } catch {
      // A paired Node must never borrow the shared document's owner pose.
    }
    if (attempt < attempts && retryDelayMs > 0) {
      await new Promise((resolve) => globalThis.setTimeout(resolve, retryDelayMs));
    }
  }
  throw new Error("calibration_pose_missing");
}

async function requestDeviceConfigRadarPlacement(options: FloorplanStorageClientOptions) {
  const fetcher = options.fetcher ?? fetch;
  const response = await fetcher(resolvePairingApiUrl(options.baseUrl ?? "", "/api/config"), {
    cache: "no-store"
  });
  if (!response.ok) throw new Error(`config_request_failed:${response.status}`);
  const config = (await response.json()) as WebDeviceConfig;
  const radar = config.floorplan?.radar;
  if (!radar) return null;
  const poseVersion = radar.poseVersion ?? 1;
  if (
    !Number.isFinite(radar.originX) ||
    !Number.isFinite(radar.originY) ||
    !Number.isFinite(radar.rotation) ||
    !Number.isFinite(radar.scale) ||
    radar.scale <= 0 ||
    !Number.isSafeInteger(poseVersion) ||
    poseVersion <= 0
  ) {
    return null;
  }
  return {
    ...radar,
    poseVersion
  };
}

function resolvePairingApiUrl(baseUrl: string, path: string): string {
  const normalized = baseUrl.trim().replace(/\/+$/, "");
  if (!normalized) return path;
  const absolute = /^https?:\/\//i.test(normalized) ? normalized : `http://${normalized}`;
  return `${absolute}${path}`;
}
