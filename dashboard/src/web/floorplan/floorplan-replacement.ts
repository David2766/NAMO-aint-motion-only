import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
import type { LocalFloorplanRadarPlacement } from "./stored-floorplan-document";

const MAX_COORDINATE_SIZE_MM = 0x7fffffff;

export type FloorplanMutationDecision =
  | "allowed"
  | "owner_required"
  | "connected_delete_blocked";

export function floorplanMutationDecision(
  site: PlatformSiteConfigurationStatus | null | undefined,
  localNodeId: string | null | undefined,
  action: "replace" | "delete"
): FloorplanMutationDecision {
  if (site?.localOwner === false) return "owner_required";
  if (
    action === "delete" &&
    (site?.members ?? []).some(
      (member) =>
        member.participantId !== localNodeId && member.membership !== "revoked"
    )
  ) {
    return "connected_delete_blocked";
  }
  return "allowed";
}

export interface FloorplanCoordinateSize {
  widthMm: number;
  heightMm: number;
}

export interface RelocatedRadarPlacement {
  participantId: string;
  originXMillimeters: number;
  originYMillimeters: number;
}

export interface FloorplanReplacementActionOptions {
  site: PlatformSiteConfigurationStatus | null;
  document: FloorplanStorageDocument;
  prepare: () => Promise<PlatformSiteConfigurationStatus | null | void>;
  savePayloads: () => Promise<void>;
  commitManifest: (coordinateSize: FloorplanCoordinateSize) => Promise<PlatformSiteConfigurationStatus | null>;
  finalizeLocalState: () => Promise<void>;
}

export interface FloorplanReplacementActionResult {
  site: PlatformSiteConfigurationStatus | null;
  relocatedParticipantIds: string[];
  localFinalizationError?: unknown;
}

export function floorplanCoordinateSize(
  document: FloorplanStorageDocument
): FloorplanCoordinateSize {
  const widthMm = Math.round(document.image.width * document.scale.mmPerPxX);
  const heightMm = Math.round(document.image.height * document.scale.mmPerPxY);
  if (!Number.isSafeInteger(widthMm) || widthMm <= 0 || widthMm > MAX_COORDINATE_SIZE_MM ||
      !Number.isSafeInteger(heightMm) || heightMm <= 0 || heightMm > MAX_COORDINATE_SIZE_MM) {
    throw new Error("floorplan_coordinate_bounds_invalid");
  }
  return { widthMm, heightMm };
}

export function floorplanReplacementRelocations(
  site: PlatformSiteConfigurationStatus | null,
  document: FloorplanStorageDocument
): RelocatedRadarPlacement[] {
  if (!site?.floorplan.committed ||
      (site.floorplan.floorplanId === document.floorplanId &&
       site.floorplan.geometryVersion === document.geometryVersion)) {
    return [];
  }
  const { widthMm, heightMm } = floorplanCoordinateSize(document);
  const centerX = Math.floor(widthMm / 2);
  const centerY = Math.floor(heightMm / 2);
  return (site.members ?? []).flatMap((member) => {
    const transform = member.spatialProfile?.transform;
    if (!transform) return [];
    const outside = !Number.isFinite(transform.originXMillimeters) ||
      !Number.isFinite(transform.originYMillimeters) ||
      transform.originXMillimeters < 0 ||
      transform.originYMillimeters < 0 ||
      transform.originXMillimeters > widthMm ||
      transform.originYMillimeters > heightMm;
    return outside
      ? [{
          participantId: member.participantId,
          originXMillimeters: centerX,
          originYMillimeters: centerY
        }]
      : [];
  });
}

export function canonicalLocalRadarPlacement(
  site: PlatformSiteConfigurationStatus | null,
  document: FloorplanStorageDocument,
  localNodeId: string | undefined
): LocalFloorplanRadarPlacement | null {
  if (!localNodeId || !site?.floorplan.committed ||
      site.floorplan.floorplanId !== document.floorplanId ||
      site.floorplan.geometryVersion !== document.geometryVersion) {
    return null;
  }
  const profile = site.members?.find((member) =>
    member.participantId === localNodeId && member.membership === "active"
  )?.spatialProfile;
  const transform = profile?.transform;
  if (!profile || !transform ||
      !Number.isFinite(transform.originXMillimeters) ||
      !Number.isFinite(transform.originYMillimeters) ||
      !Number.isFinite(transform.rotationCentiDegrees) ||
      !Number.isFinite(document.scale.mmPerPxX) || document.scale.mmPerPxX <= 0 ||
      !Number.isFinite(document.scale.mmPerPxY) || document.scale.mmPerPxY <= 0) {
    return null;
  }
  return {
    originX: transform.originXMillimeters / document.scale.mmPerPxX,
    originY: transform.originYMillimeters / document.scale.mmPerPxY,
    rotation: transform.rotationCentiDegrees / 100,
    scale: document.radar.scale,
    poseVersion: profile.poseVersion
  };
}

export async function runFloorplanReplacementAction({
  site,
  document,
  prepare,
  savePayloads,
  commitManifest,
  finalizeLocalState
}: FloorplanReplacementActionOptions): Promise<FloorplanReplacementActionResult> {
  const coordinateSize = floorplanCoordinateSize(document);
  const preparedSite = (await prepare()) ?? site;
  const relocatedParticipantIds = floorplanReplacementRelocations(preparedSite, document)
    .map((relocation) => relocation.participantId);
  await savePayloads();
  const committedSite = await commitManifest(coordinateSize);
  try {
    await finalizeLocalState();
    return { site: committedSite, relocatedParticipantIds };
  } catch (localFinalizationError) {
    return { site: committedSite, relocatedParticipantIds, localFinalizationError };
  }
}
