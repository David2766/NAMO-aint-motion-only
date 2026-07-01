import {
  LD2450_ZONE_MAX_X_MM,
  LD2450_ZONE_MAX_Y_MM,
  LD2450_ZONE_MIN_X_MM,
  LD2450_ZONE_MIN_Y_MM,
  MAX_CALIBRATION_ZONES,
  MAX_SOFTWARE_ZONES,
  MAX_ZONE_NAME_LENGTH,
  MAX_ZONE_POINTS
} from "./constants";
import { boundsFromPoints, type ZonePoint } from "./geometry";
import type { FloorplanStorageDocument } from "./floorplan/floorplan-storage";
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
          message: "JSON 파일을 읽을 수 없습니다.",
          detail: location
            ? `${location.line}번째 줄 ${location.column}번째 글자 근처에서 JSON 문법 오류가 발생했습니다.`
            : String(error)
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
          message: "백업 파일의 최상위 값은 객체여야 합니다."
        }
      ]
    });
  }

  const appMatches = parsed.app === BACKUP_APP_ID;
  if (!appMatches) {
    warnings.push(issue(rawText, "app", "이 앱에서 만든 백업 파일인지 확인할 수 없습니다.", `예상 값은 ${BACKUP_APP_ID}입니다.`));
  }

  if (parsed.formatVersion !== BACKUP_FORMAT_VERSION) {
    errors.push(
      issue(
        rawText,
        "formatVersion",
        "지원하지 않는 백업 형식 버전입니다.",
        `현재 지원 버전은 ${BACKUP_FORMAT_VERSION}입니다.`
      )
    );
  }

  if (typeof parsed.createdAt !== "string" || !parsed.createdAt.trim()) {
    errors.push(issue(rawText, "createdAt", "백업 생성 시간이 없거나 올바르지 않습니다."));
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
        "체크섬이 없거나 형식이 올바르지 않습니다.",
        "파일이 수동으로 수정되었거나 이 앱에서 만든 백업이 아닐 수 있습니다."
      )
    );
  } else if (!checksumValid) {
    warnings.push(
      issue(
        rawText,
        "checksum",
        "체크섬이 일치하지 않습니다.",
        "백업 파일이 수동으로 수정되었거나 일부 내용이 손상되었을 수 있습니다."
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
  const line = issueItem.line ? `${issueItem.line}번째 줄 · ` : "";
  return `${line}${issueItem.path}: ${issueItem.message}${issueItem.detail ? ` ${issueItem.detail}` : ""}`;
}

function validateConfig(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): WebDeviceConfig | null {
  if (!isRecord(value)) {
    errors.push(issue(rawText, "config", "config 값은 객체여야 합니다."));
    return null;
  }

  if (typeof value.version !== "number" || !Number.isFinite(value.version)) {
    errors.push(issue(rawText, "config.version", "config.version은 숫자여야 합니다."));
  }

  if (!Array.isArray(value.zones)) {
    errors.push(issue(rawText, "config.zones", "software zone 목록은 배열이어야 합니다."));
    return null;
  }

  if (value.zones.length > MAX_SOFTWARE_ZONES) {
    errors.push(
      issue(
        rawText,
        "config.zones",
        `software zone은 최대 ${MAX_SOFTWARE_ZONES}개까지만 가져올 수 있습니다.`,
        `현재 파일에는 ${value.zones.length}개가 들어 있습니다.`
      )
    );
  }

  const zones = validateZoneArray(value.zones, "config.zones", "zone", MAX_SOFTWARE_ZONES, rawText, errors, warnings);
  const calibrationSource = Array.isArray(value.calibrationZones) ? value.calibrationZones : [];

  if (value.calibrationZones !== undefined && !Array.isArray(value.calibrationZones)) {
    errors.push(issue(rawText, "config.calibrationZones", "오탐 보정 구역 목록은 배열이어야 합니다."));
  }

  if (calibrationSource.length > MAX_CALIBRATION_ZONES) {
    errors.push(
      issue(
        rawText,
        "config.calibrationZones",
        `오탐 보정 구역은 최대 ${MAX_CALIBRATION_ZONES}개까지만 가져올 수 있습니다.`,
        `현재 파일에는 ${calibrationSource.length}개가 들어 있습니다.`
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
    warnings.push(issue(rawText, "config.floorplan", "floorplan 값이 객체가 아니어서 가져오지 않습니다."));
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
          radarOcclusionIgnoredEdges: parseStringArray(value.floorplan.radarOcclusionIgnoredEdges),
          radar: parseFloorplanRadar(value.floorplan.radar)
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
  const { originX, originY, rotation, scale } = value;
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
  return { originX, originY, rotation, scale };
}

function validateFloorplanBackup(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): BackupFloorplanData | null {
  if (value === undefined) return null;
  if (!isRecord(value)) {
    warnings.push(issue(rawText, "floorplan", "평면도 백업 값이 객체가 아니어서 가져오지 않습니다."));
    return null;
  }

  if (!isRecord(value.document)) {
    warnings.push(issue(rawText, "floorplan.document", "평면도 설정 문서가 없거나 올바르지 않아 가져오지 않습니다."));
    return null;
  }

  const image = validateFloorplanBackupImage(value.image, rawText, errors, warnings);
  return {
    document: value.document as unknown as FloorplanStorageDocument,
    image: image ?? undefined
  };
}

function validateFloorplanBackupImage(
  value: unknown,
  rawText: string,
  errors: BackupIssue[],
  warnings: BackupIssue[]
): BackupFloorplanImage | null {
  if (value === undefined) return null;
  if (!isRecord(value)) {
    warnings.push(issue(rawText, "floorplan.image", "평면도 이미지 백업 값이 객체가 아니어서 가져오지 않습니다."));
    return null;
  }

  const mime = typeof value.mime === "string" && value.mime.trim() ? value.mime : "";
  const dataBase64 = typeof value.dataBase64 === "string" && value.dataBase64.trim() ? value.dataBase64 : "";
  const bytes = typeof value.bytes === "number" && Number.isFinite(value.bytes) && value.bytes >= 0 ? Math.round(value.bytes) : 0;

  if (!mime || !dataBase64 || bytes <= 0) {
    errors.push(issue(rawText, "floorplan.image", "평면도 이미지 백업 정보가 부족합니다.", "mime, bytes, dataBase64가 필요합니다."));
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
    warnings.push(issue(rawText, "stats", "통계 백업 값이 객체가 아니어서 가져오지 않습니다."));
    return null;
  }

  const today = isRecord(value.today) || value.today === null ? (value.today as WebDeviceStats["today"]) : null;
  if (value.today !== undefined && !isRecord(value.today) && value.today !== null) {
    warnings.push(issue(rawText, "stats.today", "오늘 통계 값이 객체가 아니어서 비워둡니다."));
  }

  const daily = Array.isArray(value.daily) ? (value.daily as WebDeviceStats["daily"]) : [];
  if (value.daily !== undefined && !Array.isArray(value.daily)) {
    warnings.push(issue(rawText, "stats.daily", "일별 통계 값이 배열이 아니어서 비워둡니다."));
  }

  const summary = isRecord(value.summary) ? (value.summary as WebDeviceStats["summary"]) : undefined;
  const heatmap = isRecord(value.heatmap) ? (value.heatmap as unknown as WebDeviceStats["heatmap"]) : undefined;
  if (value.heatmap !== undefined && !isRecord(value.heatmap)) {
    warnings.push(issue(rawText, "stats.heatmap", "히트맵 값이 객체가 아니어서 가져오지 않습니다."));
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
      errors.push(issue(rawText, path, "구역 정보는 객체여야 합니다."));
      return;
    }

    const id = typeof value.id === "string" ? value.id : "";
    const idMatch = kind === "zone" ? /^zone_(\d+)$/.exec(id) : /^calibration_(\d+)$/.exec(id);
    const line = id ? findLineForStringValue(rawText, id) : findLineForPath(rawText, path);

    if (!idMatch) {
      errors.push({
        path: `${path}.id`,
        line,
        message: kind === "zone" ? "software zone id가 올바르지 않습니다." : "오탐 보정 구역 id가 올바르지 않습니다.",
        detail:
          kind === "zone"
            ? "zone_1부터 zone_6까지만 사용할 수 있습니다."
            : "calibration_1부터 calibration_4까지만 사용할 수 있습니다."
      });
      return;
    }

    const slot = Number(idMatch[1]);
    if (!Number.isInteger(slot) || slot < 1 || slot > maxIndex) {
      errors.push({
        path: `${path}.id`,
        line,
        message: `${id}는 사용할 수 없습니다.`,
        detail:
          kind === "zone"
            ? "software zone은 zone_1부터 zone_6까지만 지원합니다."
            : "오탐 보정 구역은 calibration_1부터 calibration_4까지만 지원합니다."
      });
      return;
    }

    if (seen.has(id)) {
      errors.push({
        path: `${path}.id`,
        line,
        message: `${id}가 중복되었습니다.`
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
        message: "오탐 보정 구역은 현재 사각형만 가져올 수 있습니다.",
        detail: "자동 보정 구역의 최소 크기 보호 로직이 사각형 기준으로 동작합니다."
      });
    }

    if (value.minSizeUnlocked !== undefined && typeof value.minSizeUnlocked !== "boolean") {
      warnings.push({
        path: `${path}.minSizeUnlocked`,
        line,
        message: "minSizeUnlocked 값이 boolean이 아니어서 false로 처리합니다."
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
    errors.push({ path, line, message: "구역 이름은 문자열이어야 합니다." });
    return null;
  }

  const trimmed = value.trim();
  const length = Array.from(trimmed).length;
  if (length > MAX_ZONE_NAME_LENGTH) {
    errors.push({
      path,
      line,
      message: `구역 이름은 공백 포함 최대 ${MAX_ZONE_NAME_LENGTH}글자까지만 사용할 수 있습니다.`,
      detail: `현재 이름은 ${length}글자입니다.`
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
  const allowed: WebZoneType[] = kind === "zone" ? ["detection", "filter", "disabled"] : ["filter", "reduced", "disabled"];
  if (typeof value !== "string" || !allowed.includes(value as WebZoneType)) {
    errors.push({
      path,
      line,
      message: "구역 타입이 올바르지 않습니다.",
      detail: `허용 값은 ${allowed.join(", ")}입니다.`
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
      message: "구역 shape이 올바르지 않습니다.",
      detail: "rect 또는 polygon만 사용할 수 있습니다."
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
    errors.push({ path, line: fallbackLine, message: "points 값은 배열이어야 합니다." });
    return null;
  }

  if (shape === "rect" && value.length !== 4) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: "사각형 구역은 꼭짓점이 정확히 4개여야 합니다.",
      detail: `현재 꼭짓점은 ${value.length}개입니다.`
    });
  }

  if (shape === "polygon" && (value.length < 3 || value.length > MAX_ZONE_POINTS)) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: `다각형 구역은 꼭짓점이 최소 3개, 최대 ${MAX_ZONE_POINTS}개여야 합니다.`,
      detail: `현재 꼭짓점은 ${value.length}개입니다.`
    });
  }

  const points: ZonePoint[] = [];
  value.forEach((point, index) => {
    const pointPath = `${path}[${index}]`;
    const line = findLineForPoint(rawText, point) || fallbackLine;
    if (!Array.isArray(point) || point.length !== 2) {
      errors.push({ path: pointPath, line, message: "각 꼭짓점은 [x, y] 형태여야 합니다." });
      return;
    }

    const [x, y] = point;
    if (typeof x !== "number" || !Number.isFinite(x) || typeof y !== "number" || !Number.isFinite(y)) {
      errors.push({ path: pointPath, line, message: "x, y 좌표는 숫자여야 합니다." });
      return;
    }
    if (!Number.isInteger(x) || !Number.isInteger(y)) {
      errors.push({ path: pointPath, line, message: "x, y 좌표는 정수 mm 값이어야 합니다." });
      return;
    }
    if (x < LD2450_ZONE_MIN_X_MM || x > LD2450_ZONE_MAX_X_MM) {
      errors.push({
        path: `${pointPath}.x`,
        line,
        message: `x 좌표가 허용 범위를 벗어났습니다: ${x}`,
        detail: `허용 범위는 ${LD2450_ZONE_MIN_X_MM} ~ ${LD2450_ZONE_MAX_X_MM}mm입니다.`
      });
    }
    if (y < LD2450_ZONE_MIN_Y_MM || y > LD2450_ZONE_MAX_Y_MM) {
      errors.push({
        path: `${pointPath}.y`,
        line,
        message: `y 좌표가 허용 범위를 벗어났습니다: ${y}`,
        detail: `허용 범위는 ${LD2450_ZONE_MIN_Y_MM} ~ ${LD2450_ZONE_MAX_Y_MM}mm입니다.`
      });
    }
    points.push([x, y]);
  });

  if (points.length > 0 && points.every(([x, y]) => x === 0 && y === 0)) {
    errors.push({
      path,
      line: findLineForPath(rawText, path) || fallbackLine,
      message: "모든 꼭짓점이 (0, 0)입니다.",
      detail: "이 구역은 빈 구역으로 간주되어 가져올 수 없습니다."
    });
  }

  if (points.length >= 2) {
    const bounds = boundsFromPoints(points);
    if (bounds.width <= 0 || bounds.height <= 0) {
      errors.push({
        path,
        line: findLineForPath(rawText, path) || fallbackLine,
        message: "구역의 너비 또는 높이가 0입니다.",
        detail: "실제로 그릴 수 있는 면적이 있어야 합니다."
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

const SHA256_INITIAL_HASH = [
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
] as const;

const SHA256_K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
] as const;

function sha256Hex(bytes: Uint8Array): string {
  const paddedLength = Math.ceil((bytes.length + 9) / 64) * 64;
  const padded = new Uint8Array(paddedLength);
  padded.set(bytes);
  padded[bytes.length] = 0x80;

  const bitLength = bytes.length * 8;
  const bitLengthHigh = Math.floor(bitLength / 0x100000000);
  const bitLengthLow = bitLength >>> 0;
  padded[paddedLength - 8] = (bitLengthHigh >>> 24) & 0xff;
  padded[paddedLength - 7] = (bitLengthHigh >>> 16) & 0xff;
  padded[paddedLength - 6] = (bitLengthHigh >>> 8) & 0xff;
  padded[paddedLength - 5] = bitLengthHigh & 0xff;
  padded[paddedLength - 4] = (bitLengthLow >>> 24) & 0xff;
  padded[paddedLength - 3] = (bitLengthLow >>> 16) & 0xff;
  padded[paddedLength - 2] = (bitLengthLow >>> 8) & 0xff;
  padded[paddedLength - 1] = bitLengthLow & 0xff;

  const hash: number[] = [...SHA256_INITIAL_HASH];
  const words = new Uint32Array(64);

  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let index = 0; index < 16; index += 1) {
      const cursor = offset + index * 4;
      words[index] =
        ((padded[cursor] << 24) | (padded[cursor + 1] << 16) | (padded[cursor + 2] << 8) | padded[cursor + 3]) >>> 0;
    }

    for (let index = 16; index < 64; index += 1) {
      const s0 = rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^ (words[index - 15] >>> 3);
      const s1 = rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^ (words[index - 2] >>> 10);
      words[index] = (words[index - 16] + s0 + words[index - 7] + s1) >>> 0;
    }

    let [a, b, c, d, e, f, g, h] = hash;
    for (let index = 0; index < 64; index += 1) {
      const sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const choice = (e & f) ^ (~e & g);
      const temp1 = (h + sum1 + choice + SHA256_K[index] + words[index]) >>> 0;
      const sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const temp2 = (sum0 + majority) >>> 0;

      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }

    hash[0] = (hash[0] + a) >>> 0;
    hash[1] = (hash[1] + b) >>> 0;
    hash[2] = (hash[2] + c) >>> 0;
    hash[3] = (hash[3] + d) >>> 0;
    hash[4] = (hash[4] + e) >>> 0;
    hash[5] = (hash[5] + f) >>> 0;
    hash[6] = (hash[6] + g) >>> 0;
    hash[7] = (hash[7] + h) >>> 0;
  }

  return hash.map((value) => value.toString(16).padStart(8, "0")).join("");
}

function rotateRight(value: number, bits: number): number {
  return (value >>> bits) | (value << (32 - bits));
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

function issue(rawText: string, path: string, message: string, detail?: string): BackupIssue {
  return {
    path,
    line: findLineForPath(rawText, path),
    message,
    detail
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
