import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import { floorplanPxToRadarPoint } from "../core/floorplan/radar-floorplan-transform";
import type { WebDeviceState, WebTarget } from "../core/types";
import type {
  PlatformGroupsStatus,
  PlatformSiteConfigurationStatus
} from "./api/platform-pairing";

export interface PlatformRadarMarker {
  participantId: string;
  xPx: number;
  yPx: number;
  rotationDegrees: number;
}

export interface PlatformFusionSummary {
  visible: boolean;
  sensorCount: number;
  state: "active" | "sending" | "fallback";
}

function authoritativeFusionAvailable(state: WebDeviceState | null): boolean {
  return state?.automation !== undefined
    ? state.automation.available === true
    : state?.fusion?.authoritative === true;
}

export function platformFloorplanTargets(
  state: WebDeviceState | null,
  targets: WebTarget[],
  site: PlatformSiteConfigurationStatus | null,
  floorplan: FloorplanStorageDocument | null,
  localNodeId: string | undefined
): WebTarget[] {
  if (!authoritativeFusionAvailable(state)) return targets;
  if (!site?.floorplan.committed || !floorplan || !localNodeId ||
      site.floorplan.floorplanId !== floorplan.floorplanId ||
      site.floorplan.geometryVersion !== floorplan.geometryVersion) return [];

  const transform = site.members?.find((member) =>
    member.participantId === localNodeId &&
    member.membership === "active"
  )?.spatialProfile?.transform;
  const scaleX = floorplan.scale.mmPerPxX;
  const scaleY = floorplan.scale.mmPerPxY;
  if (!transform ||
      !Number.isFinite(transform.originXMillimeters) ||
      !Number.isFinite(transform.originYMillimeters) ||
      !Number.isFinite(transform.rotationCentiDegrees) ||
      !Number.isFinite(scaleX) || scaleX <= 0 ||
      !Number.isFinite(scaleY) || scaleY <= 0) return [];

  const angle = transform.rotationCentiDegrees * 0.01 * Math.PI / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const options = {
    placement: {
      originX: floorplan.radar.originPx[0],
      originY: floorplan.radar.originPx[1],
      rotation: floorplan.radar.rotationDeg
    },
    scale: floorplan.scale,
    scaleFactor: floorplan.radar.scale ?? 1
  };

  return targets.map((target) => {
    if (!target.active || !Number.isFinite(target.x) || !Number.isFinite(target.y)) {
      return target;
    }
    const worldX = transform.originXMillimeters + target.x * cos + target.y * sin;
    const worldY = transform.originYMillimeters + target.x * sin - target.y * cos;
    const [x, y] = floorplanPxToRadarPoint(
      { x: worldX / scaleX, y: worldY / scaleY },
      options
    );
    return { ...target, x, y };
  });
}

export function platformFusionSummary(
  state: WebDeviceState | null,
  groups: PlatformGroupsStatus | null,
  site: PlatformSiteConfigurationStatus | null
): PlatformFusionSummary {
  const configuredSensorCount = (site?.members ?? [])
    .filter((member) => member.membership === "active" && member.spatialProfile)
    .length;
  const localGroup = groups?.groups?.find((group) =>
    group.configured && Boolean(group.groupId)
  );
  const sensorCount = configuredSensorCount > 0 &&
      localGroup && localGroup.participantsOnline > 0
    ? localGroup.participantsOnline
    : configuredSensorCount;
  const buildEnabled =
    state?.fusion?.buildEnabled === true ||
    groups?.productionFusion?.buildEnabled === true;
  const authoritative = state?.automation !== undefined
    ? state.automation.available === true
    : state?.fusion?.authoritative === true ||
      groups?.productionFusion?.authoritative === true;
  const sending =
    localGroup?.localLeader === false &&
    localGroup.leaderNodeId.length > 0 &&
    localGroup.health !== "configuration_conflict";

  return {
    visible: buildEnabled && configuredSensorCount > 1,
    sensorCount,
    state: authoritative ? "active" : sending ? "sending" : "fallback"
  };
}

export function platformPeerRadarMarkers(
  site: PlatformSiteConfigurationStatus | null,
  floorplan: FloorplanStorageDocument | null,
  localNodeId: string | undefined
): PlatformRadarMarker[] {
  if (!site?.floorplan.committed || !floorplan || !localNodeId) return [];
  if (site.floorplan.floorplanId !== floorplan.floorplanId ||
      site.floorplan.geometryVersion !== floorplan.geometryVersion) return [];

  const scaleX = floorplan.scale.mmPerPxX;
  const scaleY = floorplan.scale.mmPerPxY;
  if (!Number.isFinite(scaleX) || scaleX <= 0 || !Number.isFinite(scaleY) || scaleY <= 0) return [];

  return (site.members ?? [])
    .filter((member) =>
      member.participantId !== localNodeId &&
      member.membership === "active" &&
      member.spatialProfile?.transform
    )
    .map((member) => {
      const transform = member.spatialProfile!.transform;
      return {
        participantId: member.participantId,
        xPx: transform.originXMillimeters / scaleX,
        yPx: transform.originYMillimeters / scaleY,
        rotationDegrees: transform.rotationCentiDegrees / 100
      };
    })
    .filter((marker) =>
      Number.isFinite(marker.xPx) &&
      Number.isFinite(marker.yPx) &&
      Number.isFinite(marker.rotationDegrees)
    );
}
