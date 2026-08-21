import {
  LD2450_ZONE_MAX_X_MM,
  LD2450_ZONE_MAX_Y_MM,
  LD2450_ZONE_MIN_X_MM,
  LD2450_ZONE_MIN_Y_MM,
  MAX_CALIBRATION_ZONES,
  MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS,
  MAX_SOFTWARE_ZONES,
  MAX_ZONE_NAME_LENGTH,
  MAX_ZONE_POINTS
} from "./constants";
import { boundsFromPoints, type ZonePoint } from "./geometry";
import type { FloorplanStorageDocument } from "./floorplan/floorplan-storage";
import { sha256Hex } from "./sha256";
import type { WebDeviceConfig, WebDeviceStats, WebZone, WebZoneType } from "./types";

export const BACKUP_APP_ID = "radar-zone-configurator";
export const BACKUP_FORMAT_VERSION = 1;
export const BACKUP_CHECKSUM_PREFIX = "sha256:";

export interface BackupDeviceInfo {
  sourceUrl?: string;
  name?: string;
  id?: string;
}

export interface ConfigBackupFile {
  app: string;
  formatVersion: number;
  createdAt: string;
  device?: BackupDeviceInfo;
  config: WebDeviceConfig;
  floorplan?: BackupFloorplanData;
  stats?: WebDeviceStats;
  checksum: string;
}

export interface BackupFloorplanImage {
  name?: string;
  mime: string;
  bytes: number;
  dataBase64: string;
}

export interface BackupFloorplanData {
  document: FloorplanStorageDocument;
  image?: BackupFloorplanImage;
}

export interface BackupIssue {
  path: string;
  message: string;
  detail?: string;
  line?: number;
  code?: string;
  params?: Record<string, string | number | boolean | undefined>;
}

export interface BackupValidationResult {
  config: WebDeviceConfig | null;
  floorplan: BackupFloorplanData | null;
  stats: WebDeviceStats | null;
  errors: BackupIssue[];
  warnings: BackupIssue[];
  checksumValid: boolean;
  checksumExpected?: string;
  checksumActual?: string;
  appMatches: boolean;
  summary: {
    softwareZones: number;
    calibrationZones: number;
    filterZones: number;
    reducedZones: number;
    disabledZones: number;
    hasFloorplan: boolean;
    floorplanImageBytes: number;
    hasStats: boolean;
    statsDailyDays: number;
    hasHeatmap: boolean;
  };
}

type UnknownRecord = Record<string, unknown>;
type ZoneKind = "zone" | "calibration";

export async function createConfigBackup(
  config: WebDeviceConfig,
  device: BackupDeviceInfo = {},
  floorplan?: BackupFloorplanData,
  stats?: WebDeviceStats
): Promise<ConfigBackupFile> {
  const backupWithoutChecksum = {
    app: BACKUP_APP_ID,
    formatVersion: BACKUP_FORMAT_VERSION,
    createdAt: new Date().toISOString(),
    device,
    config,
    floorplan,
    stats
  };
  return {
    ...backupWithoutChecksum,
    checksum: await checksumForBackupPayload(backupWithoutChecksum)
  };
}

export async function validateConfigBackupText(rawText: string): Promise<BackupValidationResult> {
  const errors: BackupIssue[] = [];
  const warnings: BackupIssue[] = [];
  let parsed: unknown;

  try {
    parsed = JSON.parse(rawText) as unknown;
  } catch (error) {
    const location = jsonParseErrorLocation(rawText, error);
    return emptyResult({
      errors: [
        {
          path: "$",
          line: location?.line,
          code: "backup_json_parse_failed",
          params: {
            column: location?.column,
            detail: location ? undefined : String(error)
          },
          message: "backup_json_parse_failed",
          detail: location ? undefined : String(error)
        }
      ]
    });
  }

  if (!isRecord(parsed)) {
    return emptyResult({
      errors: [
        {
          path: "$",
          line: 1,
          code: "backup_root_not_object",
          message: "backup_root_not_object"
        }
      ]
    });
  }

  const appMatches = parsed.app === BACKUP_APP_ID;
  if (!appMatches) {
    warnings.push(
      issue(
        rawText,
        "app",
        "backup_app_mismatch",
        undefined,
        "backup_app_mismatch",
        { expected: BACKUP_APP_ID }
      )
    );
  }

  if (parsed.formatVersion !== BACKUP_FORMAT_VERSION) {
    errors.push(
      issue(
        rawText,
        "formatVersion",
        "backup_format_version_unsupported",
        undefined,
        "backup_format_version_unsupported",
        { supported: BACKUP_FORMAT_VERSION }
      )
    );
  }

  if (typeof parsed.createdAt !== "string" || !parsed.createdAt.trim()) {
    errors.push(issue(rawText, "createdAt", "backup_created_at_invalid", undefined, "backup_created_at_invalid"));
  }

  const checksumPayload = {
    app: parsed.app,
    formatVersion: parsed.formatVersion,
    createdAt: parsed.createdAt,
    device: parsed.device,
    config: parsed.config,
    floorplan: parsed.floorplan,
    stats: parsed.stats
  };
  const expectedChecksum = await checksumForBackupPayload(checksumPayload);
  const actualChecksum = typeof parsed.checksum === "string" ? parsed.checksum : "";
  const checksumValid = actualChecksum === expectedChecksum;

  if (!actualChecksum.startsWith(BACKUP_CHECKSUM_PREFIX)) {
    warnings.push(
      issue(
        rawText,
        "checksum",
        "backup_checksum_missing",
        undefined,
        "backup_checksum_missing"
      )
    );
  } else if (!checksumValid) {
    warnings.push(
      issue(
        rawText,
        "checksum",
        "backup_checksum_mismatch",
        undefined,
        "backup_checksum_mismatch"
      )
    );
  }

  const config = validateConfig(parsed.config, rawText, errors, warnings);
  const floorplan = validateFloorplanBackup(parsed.floorplan, rawText, errors, warnings);
  const stats = validateStatsBackup(parsed.stats, rawText, warnings);
  return {
    config,
    floorplan,
    stats,
    errors,
    warnings,
    checksumValid,
    checksumExpected: expectedChecksum,
    checksumActual: actualChecksum || undefined,
    appMatches,
    summary: summarizeConfig(config, floorplan, stats)
  };
}

export function backupValidationMessage(issueItem: BackupIssue): string {
  const line = issueItem.line ? `line ${issueItem.line} · ` : "";
  return `${line}${issueItem.path}: ${issueItem.message}${issueItem.detail ? ` ${issueItem.detail}` : ""}`;
}

function validateConfig(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): WebDeviceConfig | null {
  if (!isRecord(value)) {
    errors.push(issue(rawText, "config", "backup_config_not_object", undefined, "backup_config_not_object"));
    return null;
  }

  if (typeof value.version !== "number" || !Number.isFinite(value.version)) {
    errors.push(issue(rawText, "config.version", "backup_config_version_invalid", undefined, "backup_config_version_invalid"));
  }

  if (!Array.isArray(value.zones)) {
    errors.push(issue(rawText, "config.zones", "backup_config_zones_not_array", undefined, "backup_config_zones_not_array"));
    return null;
  }

  if (value.zones.length > MAX_SOFTWARE_ZONES) {
    errors.push(
      issue(
        rawText,
        "config.zones",
        "backup_config_zones_too_many",
        undefined,
        "backup_config_zones_too_many",
        { max: MAX_SOFTWARE_ZONES, actual: value.zones.length }
      )
    );
  }

  const zones = validateZoneArray(value.zones, "config.zones", "zone", MAX_SOFTWARE_ZONES, rawText, errors, warnings);
  const calibrationSource = Array.isArray(value.calibrationZones) ? value.calibrationZones : [];

  if (value.calibrationZones !== undefined && !Array.isArray(value.calibrationZones)) {
    errors.push(
      issue(
        rawText,
        "config.calibrationZones",
        "backup_calibration_zones_not_array",
        undefined,
        "backup_calibration_zones_not_array"
      )
    );
  }

  if (calibrationSource.length > MAX_CALIBRATION_ZONES) {
    errors.push(
      issue(
        rawText,
        "config.calibrationZones",
        "backup_calibration_zones_too_many",
        undefined,
        "backup_calibration_zones_too_many",
        { max: MAX_CALIBRATION_ZONES, actual: calibrationSource.length }
      )
    );
  }

  const calibrationZones = validateZoneArray(
    calibrationSource,
    "config.calibrationZones",
    "calibration",
    MAX_CALIBRATION_ZONES,
    rawText,
    errors,
    warnings
  );

  if (value.floorplan !== undefined && !isRecord(value.floorplan)) {
    warnings.push(
      issue(
        rawText,
        "config.floorplan",
        "backup_config_floorplan_not_object",
        undefined,
        "backup_config_floorplan_not_object"
      )
    );
  }

  if (errors.length > 0) return null;
  return {
    version: Number(value.version),
    zones,
    calibrationZones,
    floorplan: isRecord(value.floorplan)
      ? {
          enabled: value.floorplan.enabled === true,
          hasImage: value.floorplan.hasImage === true,
          scaleMmPerPx:
            typeof value.floorplan.scaleMmPerPx === "number" && Number.isFinite(value.floorplan.scaleMmPerPx)
              ? value.floorplan.scaleMmPerPx
              : undefined,
          scaleMmPerPxX:
            typeof value.floorplan.scaleMmPerPxX === "number" && Number.isFinite(value.floorplan.scaleMmPerPxX)
              ? value.floorplan.scaleMmPerPxX
              : undefined,
          scaleMmPerPxY:
            typeof value.floorplan.scaleMmPerPxY === "number" && Number.isFinite(value.floorplan.scaleMmPerPxY)
              ? value.floorplan.scaleMmPerPxY
              : undefined,
          radarOcclusionIgnoredEdges: parseStringArray(value.floorplan.radarOcclusionIgnoredEdges),
          radar: parseFloorplanRadar(value.floorplan.radar),
          room: parseFloorplanRoom(value.floorplan.room)
        }
      : undefined
  };
}

function parseStringArray(value: unknown): string[] | undefined {
  if (!Array.isArray(value)) return undefined;
  return value.filter((item): item is string => typeof item === "string");
}

function parseFloorplanRadar(value: unknown): NonNullable<WebDeviceConfig["floorplan"]>["radar"] | undefined {
  if (!isRecord(value)) return undefined;
  const { originX, originY, rotation, scale, poseVersion } = value;
  if (
    typeof originX !== "number" ||
    typeof originY !== "number" ||
    typeof rotation !== "number" ||
    typeof scale !== "number" ||
    !Number.isFinite(originX) ||
    !Number.isFinite(originY) ||
    !Number.isFinite(rotation) ||
    !Number.isFinite(scale)
  ) {
    return undefined;
  }
  return {
    originX,
    originY,
    rotation,
    scale,
    poseVersion: typeof poseVersion === "number" && Number.isInteger(poseVersion) && poseVersion > 0
      ? poseVersion
      : undefined
  };
}

function validateFloorplanBackup(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): BackupFloorplanData | null {
  if (value === undefined) return null;
  if (!isRecord(value)) {
    warnings.push(issue(rawText, "floorplan", "backup_floorplan_not_object", undefined, "backup_floorplan_not_object"));
    return null;
  }

  if (!isRecord(value.document)) {
    warnings.push(
      issue(
        rawText,
        "floorplan.document",
        "backup_floorplan_document_invalid",
        undefined,
        "backup_floorplan_document_invalid"
      )
    );
    return null;
  }

  const image = validateFloorplanBackupImage(value.image, rawText, errors, warnings);
  return {
    document: value.document as unknown as FloorplanStorageDocument,
    image: image ?? undefined
  };
}

function parseFloorplanRoom(value: unknown): NonNullable<WebDeviceConfig["floorplan"]>["room"] | undefined {
  if (!isRecord(value)) return undefined;
  if (typeof value.id !== "string" || typeof value.name !== "string") return undefined;
  const room: NonNullable<NonNullable<WebDeviceConfig["floorplan"]>["room"]> = {
    id: value.id,
    name: value.name,
    source: "stored_room"
  };
  const boundary = parseFloorplanRoomBoundary(value.boundary);
  if (boundary.length >= 3) room.boundary = boundary;
  return room;
}

function parseFloorplanRoomBoundary(value: unknown): Array<[number, number]> {
  if (!Array.isArray(value) || value.length > MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS) return [];
  return value
    .map((point): [number, number] | null => {
      if (!Array.isArray(point) || point.length < 2) return null;
      const x = Number(point[0]);
      const y = Number(point[1]);
      if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
      return [Math.round(x), Math.round(y)];
    })
    .filter((point): point is [number, number] => point !== null);
}

function validateFloorplanBackupImage(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): BackupFloorplanImage | null {
  if (value === undefined) return null;
  if (!isRecord(value)) {
    warnings.push(
      issue(
        rawText,
        "floorplan.image",
        "backup_floorplan_image_not_object",
        undefined,
        "backup_floorplan_image_not_object"
      )
    );
    return null;
  }

  const mime = typeof value.mime === "string" && value.mime.trim() ? value.mime : "";
  const dataBase64 = typeof value.dataBase64 === "string" && value.dataBase64.trim() ? value.dataBase64 : "";
  const bytes = typeof value.bytes === "number" && Number.isFinite(value.bytes) && value.bytes >= 0 ? Math.round(value.bytes) : 0;

  if (!mime || !dataBase64 || bytes <= 0) {
    errors.push(
      issue(
        rawText,
        "floorplan.image",
        "backup_floorplan_image_incomplete",
        undefined,
        "backup_floorplan_image_incomplete"
      )
    );
    return null;
  }

  return {
    name: typeof value.name === "string" ? value.name : undefined,
    mime,
    bytes,
    dataBase64
  };
}

function validateStatsBackup(value: unknown, rawText: string, warnings: BackupIssue[]): WebDeviceStats | null {
  if (value === undefined) return null;
  if (!isRecord(value)) {
    warnings.push(issue(rawText, "stats", "backup_stats_not_object", undefined, "backup_stats_not_object"));
    return null;
  }

  const today = isRecord(value.today) || value.today === null ? (value.today as WebDeviceStats["today"]) : null;
  if (value.today !== undefined && !isRecord(value.today) && value.today !== null) {
    warnings.push(issue(rawText, "stats.today", "backup_stats_today_invalid", undefined, "backup_stats_today_invalid"));
  }

  const daily = Array.isArray(value.daily) ? (value.daily as WebDeviceStats["daily"]) : [];
  if (value.daily !== undefined && !Array.isArray(value.daily)) {
    warnings.push(issue(rawText, "stats.daily", "backup_stats_daily_invalid", undefined, "backup_stats_daily_invalid"));
  }

  const summary = isRecord(value.summary) ? (value.summary as WebDeviceStats["summary"]) : undefined;
  const heatmap = isRecord(value.heatmap) ? (value.heatmap as unknown as WebDeviceStats["heatmap"]) : undefined;
  if (value.heatmap !== undefined && !isRecord(value.heatmap)) {
    warnings.push(issue(rawText, "stats.heatmap", "backup_stats_heatmap_invalid", undefined, "backup_stats_heatmap_invalid"));
  }

  return {
    today,
    daily,
    summary,
    heatmap
  };
}

function validateZoneArray(
  values: unknown[],
  basePath: string,
  kind: ZoneKind,
  maxIndex: number,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): WebZone[] {
  const zones: WebZone[] = [];
  const seen = new Set<string>();

  values.forEach((value, index) => {
    const path = `${basePath}[${index}]`;
    if (!isRecord(value)) {
      errors.push(issue(rawText, path, "backup_zone_not_object", undefined, "backup_zone_not_object"));
      return;
    }

    const id = typeof value.id === "string" ? value.id : "";
    const idMatch = kind === "zone" ? /^zone_(\d+)$/.exec(id) : /^calibration_(\d+)$/.exec(id);
    const line = id ? findLineForStringValue(rawText, id) : findLineForPath(rawText, path);

    if (!idMatch) {
      errors.push({
        path: `${path}.id`,
        line,
        message: "backup_zone_id_invalid",
        code: "backup_zone_id_invalid",
        params: { kind, max: maxIndex }
      });
      return;
    }

    const slot = Number(idMatch[1]);
    if (!Number.isInteger(slot) || slot < 1 || slot > maxIndex) {
      errors.push({
        path: `${path}.id`,
        line,
        message: "backup_zone_id_out_of_range",
        code: "backup_zone_id_out_of_range",
        params: { id, kind, max: maxIndex }
      });
      return;
    }

    if (seen.has(id)) {
      errors.push({
        path: `${path}.id`,
        line,
        message: "backup_zone_id_duplicate",
        code: "backup_zone_id_duplicate",
        params: { id }
      });
      return;
    }
    seen.add(id);

    const name = validateZoneName(value.name, `${path}.name`, line, errors);
    const type = validateZoneType(value.type, kind, `${path}.type`, line, errors);
    const shape = validateShape(value.shape, `${path}.shape`, line, errors);
    const points = validatePoints(value.points, shape, `${path}.points`, rawText, line, errors);
    const minPoints =
      value.minPoints === undefined ? undefined : validatePoints(value.minPoints, "rect", `${path}.minPoints`, rawText, line, errors);
    const minSizeUnlocked = value.minSizeUnlocked === undefined ? undefined : value.minSizeUnlocked === true;

    if (kind === "calibration" && shape === "polygon") {
      errors.push({
        path: `${path}.shape`,
        line,
        message: "backup_calibration_polygon_unsupported",
        code: "backup_calibration_polygon_unsupported"
      });
    }

    if (value.minSizeUnlocked !== undefined && typeof value.minSizeUnlocked !== "boolean") {
      warnings.push({
        path: `${path}.minSizeUnlocked`,
        line,
        message: "backup_min_size_unlocked_invalid",
        code: "backup_min_size_unlocked_invalid"
      });
    }

    if (name === null || type === null || shape === null || points === null) return;
    zones.push({
      id,
      name,
      type,
      shape,
      points,
      minPoints: minPoints || undefined,
      minSizeUnlocked
    });
  });

  return zones.sort((a, b) => zoneSlot(a.id) - zoneSlot(b.id));
}

function validateZoneName(value: unknown, path: string, line: number | undefined, errors: BackupIssue[]): string | null {
  if (value === undefined) return "";
  if (typeof value !== "string") {
    errors.push({ path, line, message: "backup_zone_name_not_string", code: "backup_zone_name_not_string" });
    return null;
  }

  const trimmed = value.trim();
  const length = Array.from(trimmed).length;
  if (length > MAX_ZONE_NAME_LENGTH) {
    errors.push({
      path,
      line,
      message: "backup_zone_name_too_long",
      code: "backup_zone_name_too_long",
      params: { max: MAX_ZONE_NAME_LENGTH, actual: length }
    });
    return null;
  }
  return trimmed;
}

function validateZoneType(
  value: unknown,
  kind: ZoneKind,
  path: string,
  line: number | undefined,
  errors: BackupIssue[]
): WebZoneType | null {
  const allowed: WebZoneType[] = kind === "zone"
    ? ["detection", "filter", "reduced", "disabled", "exit"]
    : ["filter", "reduced", "disabled"];
  if (typeof value !== "string" || !allowed.includes(value as WebZoneType)) {
    errors.push({
      path,
      line,
      message: "backup_zone_type_invalid",
      code: "backup_zone_type_invalid",
      params: { allowed: allowed.join(", ") }
    });
    return null;
  }
  return value as WebZoneType;
}

function validateShape(value: unknown, path: string, line: number | undefined, errors: BackupIssue[]): WebZone["shape"] | null {
  if (value !== "rect" && value !== "polygon") {
    errors.push({
      path,
      line,
      message: "backup_zone_shape_invalid",
      code: "backup_zone_shape_invalid"
    });
    return null;
  }
  return value;
}

function validatePoints(
  value: unknown,
  shape: WebZone["shape"] | null,
  path: string,
  rawText: string,
  fallbackLine: number | undefined,
  errors: BackupIssue[]
): ZonePoint[] | null {
  if (!shape) return null;
  if (!Array.isArray(value)) {
    errors.push({ path, line: fallbackLine, message: "backup_zone_points_not_array", code: "backup_zone_points_not_array" });
    return null;
  }

  if (shape === "rect" && value.length !== 4) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: "backup_zone_rect_points_invalid",
      code: "backup_zone_rect_points_invalid",
      params: { actual: value.length }
    });
  }

  if (shape === "polygon" && (value.length < 3 || value.length > MAX_ZONE_POINTS)) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: "backup_zone_polygon_points_invalid",
      code: "backup_zone_polygon_points_invalid",
      params: { min: 3, max: MAX_ZONE_POINTS, actual: value.length }
    });
  }

  const points: ZonePoint[] = [];
  value.forEach((point, index) => {
    const pointPath = `${path}[${index}]`;
    const line = findLineForPoint(rawText, point) || fallbackLine;
    if (!Array.isArray(point) || point.length !== 2) {
      errors.push({ path: pointPath, line, message: "backup_zone_point_invalid", code: "backup_zone_point_invalid" });
      return;
    }

    const [x, y] = point;
    if (typeof x !== "number" || !Number.isFinite(x) || typeof y !== "number" || !Number.isFinite(y)) {
      errors.push({ path: pointPath, line, message: "backup_zone_point_number_invalid", code: "backup_zone_point_number_invalid" });
      return;
    }
    if (!Number.isInteger(x) || !Number.isInteger(y)) {
      errors.push({ path: pointPath, line, message: "backup_zone_point_integer_invalid", code: "backup_zone_point_integer_invalid" });
      return;
    }
    if (x < LD2450_ZONE_MIN_X_MM || x > LD2450_ZONE_MAX_X_MM) {
      errors.push({
        path: `${pointPath}.x`,
        line,
        message: "backup_zone_point_x_out_of_range",
        code: "backup_zone_point_x_out_of_range",
        params: { value: x, min: LD2450_ZONE_MIN_X_MM, max: LD2450_ZONE_MAX_X_MM }
      });
    }
    if (y < LD2450_ZONE_MIN_Y_MM || y > LD2450_ZONE_MAX_Y_MM) {
      errors.push({
        path: `${pointPath}.y`,
        line,
        message: "backup_zone_point_y_out_of_range",
        code: "backup_zone_point_y_out_of_range",
        params: { value: y, min: LD2450_ZONE_MIN_Y_MM, max: LD2450_ZONE_MAX_Y_MM }
      });
    }
    points.push([x, y]);
  });

  if (points.length > 0 && points.every(([x, y]) => x === 0 && y === 0)) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: "backup_zone_points_all_zero",
      code: "backup_zone_points_all_zero"
    });
  }

  if (points.length >= 2) {
    const bounds = boundsFromPoints(points);
    if (bounds.width <= 0 || bounds.height <= 0) {
      errors.push({
        path,
        line: findLineForPath(rawText, path) || fallbackLine,
        message: "backup_zone_area_empty",
        code: "backup_zone_area_empty"
      });
    }
  }

  return points;
}

function summarizeConfig(
  config: WebDeviceConfig | null,
  floorplan: BackupFloorplanData | null,
  stats: WebDeviceStats | null
): BackupValidationResult["summary"] {
  const allZones = [...(config?.zones || []), ...(config?.calibrationZones || [])];
  return {
    softwareZones: config?.zones.length || 0,
    calibrationZones: config?.calibrationZones?.length || 0,
    filterZones: allZones.filter((zone) => zone.type === "filter").length,
    reducedZones: allZones.filter((zone) => zone.type === "reduced").length,
    disabledZones: allZones.filter((zone) => zone.type === "disabled").length,
    hasFloorplan: Boolean(floorplan?.document),
    floorplanImageBytes: floorplan?.image?.bytes ?? 0,
    hasStats: Boolean(stats),
    statsDailyDays: stats?.daily?.filter(Boolean).length ?? 0,
    hasHeatmap: Boolean(stats?.heatmap)
  };
}

async function checksumForBackupPayload(payload: unknown): Promise<string> {
  const canonical = canonicalJson(payload);
  const bytes = new TextEncoder().encode(canonical);
  return `${BACKUP_CHECKSUM_PREFIX}${sha256Hex(bytes)}`;
}

function canonicalJson(value: unknown): string {
  if (value === null || typeof value !== "object") return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map((item) => canonicalJson(item)).join(",")}]`;
  const record = value as UnknownRecord;
  return `{${Object.keys(record)
    .sort()
    .filter((key) => record[key] !== undefined)
    .map((key) => `${JSON.stringify(key)}:${canonicalJson(record[key])}`)
    .join(",")}}`;
}

function emptyResult(partial: Partial<BackupValidationResult>): BackupValidationResult {
  return {
    config: null,
    floorplan: null,
    stats: null,
    errors: partial.errors || [],
    warnings: partial.warnings || [],
    checksumValid: false,
    appMatches: false,
    summary: {
      softwareZones: 0,
      calibrationZones: 0,
      filterZones: 0,
      reducedZones: 0,
      disabledZones: 0,
      hasFloorplan: false,
      floorplanImageBytes: 0,
      hasStats: false,
      statsDailyDays: 0,
      hasHeatmap: false
    }
  };
}

function issue(
  rawText: string,
  path: string,
  message: string,
  detail?: string,
  code?: string,
  params?: BackupIssue["params"]
): BackupIssue {
  return {
    path,
    line: findLineForPath(rawText, path),
    message,
    detail,
    code,
    params
  };
}

function zoneSlot(zoneId: string): number {
  return Number(/_(\d+)$/.exec(zoneId)?.[1] || 0);
}

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function jsonParseErrorLocation(rawText: string, error: unknown): { line: number; column: number } | null {
  const message = error instanceof Error ? error.message : String(error);
  const match = /position\s+(\d+)/i.exec(message);
  if (!match) return null;
  return lineColumnFromOffset(rawText, Number(match[1]));
}

function lineColumnFromOffset(text: string, offset: number): { line: number; column: number } {
  const before = text.slice(0, Math.max(0, offset));
  const lines = before.split(/\r\n|\r|\n/);
  return {
    line: lines.length,
    column: lines[lines.length - 1].length + 1
  };
}

function lineFromIndex(text: string, index: number): number | undefined {
  if (index < 0) return undefined;
  return text.slice(0, index).split(/\r\n|\r|\n/).length;
}

function findLineForPath(text: string, path: string): number | undefined {
  const lastSegment = path.split(".").pop() || path;
  const key = lastSegment.replace(/\[\d+\]/g, "");
  if (!key || key === "$") return 1;
  const keyLine = findLineForKey(text, key);
  if (keyLine) return keyLine;
  const itemMatch = /\[(\d+)\]/.exec(path);
  if (itemMatch) {
    const parent = path.slice(0, path.lastIndexOf("["));
    return findLineForArrayItem(text, parent, Number(itemMatch[1]));
  }
  return undefined;
}

function findLineForKey(text: string, key: string): number | undefined {
  return lineFromIndex(text, text.search(new RegExp(`"${escapeRegExp(key)}"\\s*:`)));
}

function findLineForArrayItem(text: string, basePath: string, index: number): number | undefined {
  const key = basePath.split(".").pop();
  if (!key) return undefined;
  const start = text.search(new RegExp(`"${escapeRegExp(key)}"\\s*:`));
  if (start < 0) return undefined;
  let cursor = start;
  for (let count = 0; count <= index; count += 1) {
    cursor = text.indexOf("{", cursor + 1);
    if (cursor < 0) return undefined;
  }
  return lineFromIndex(text, cursor);
}

function findLineForStringValue(text: string, value: string): number | undefined {
  return lineFromIndex(text, text.indexOf(JSON.stringify(value)));
}

function findLineForPoint(text: string, point: unknown): number | undefined {
  if (!Array.isArray(point) || point.length < 2) return undefined;
  const compact = `[${point[0]},${point[1]}]`;
  const spaced = `[${point[0]}, ${point[1]}]`;
  let index = text.indexOf(compact);
  if (index < 0) index = text.indexOf(spaced);
  return lineFromIndex(text, index);
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
