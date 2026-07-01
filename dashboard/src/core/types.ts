export interface RadarTargetConfig {
  name?: string;
  color?: string;
  x: string;
  y: string;
}

export interface ResolvedRadarTarget extends RadarTargetConfig {
  name: string;
  color: string;
}

export interface HeldRadarTarget {
  name: string;
  color: string;
  x: number;
  y: number;
  lastSeen: number;
  active: boolean;
}

export type RadarZoneId = "zone_1" | "zone_2" | "zone_3";

export type RadarZoneAxis = "x1" | "y1" | "x2" | "y2";

export interface RadarZoneEntitySet {
  zoneId: RadarZoneId;
  x1?: string;
  y1?: string;
  x2?: string;
  y2?: string;
  name?: string;
}

export interface RadarZoneRect {
  zoneId: RadarZoneId;
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

export interface RadarZoneDisplay extends RadarZoneRect {
  label: string;
  customName?: string;
  type?: string;
  selected: boolean;
  placeholder?: boolean;
}

export interface RadarCardConfig {
  type?: string;
  title: string;
  range_x: number;
  range_y: number;
  hold_ms: number;
  show_distance: boolean;
  distance_decimals: number;
  targets: RadarTargetConfig[];
  device_id: string;
  configurator_url?: string;
  use_yaml_targets?: boolean;
  zone_names: Partial<Record<RadarZoneId, string>>;
}

export interface RadarScreenPoint {
  x: number;
  y: number;
}

export interface RadarViewport {
  width: number;
  height: number;
  pad: number;
  rangeX: number;
  rangeY: number;
  fovDegrees: number;
}

export type WebZoneType = "detection" | "filter" | "reduced" | "disabled";

export interface WebTarget {
  id: string;
  name: string;
  color: string;
  x: number;
  y: number;
  active: boolean;
  rawX?: number;
  rawY?: number;
  rawActive?: boolean;
  filtered?: boolean;
  reduced?: boolean;
  filterReason?: string;
}

export interface WebZone {
  id: string;
  name: string;
  type: WebZoneType;
  shape: "rect" | "polygon";
  points: Array<[number, number]>;
  placeholder?: boolean;
  minSizeUnlocked?: boolean;
  minPoints?: Array<[number, number]>;
}

export interface WebDeviceState {
  connected: boolean;
  targets: WebTarget[];
  updatedAt: number;
  pirMotion?: boolean;
  pirMotionEffective?: boolean;
  filterBlocked?: boolean;
  presence?: boolean;
  motion?: boolean;
  targetCount?: number;
  movingTargetCount?: number;
  stillTargetCount?: number;
  temperatureC?: number | null;
  humidityPercent?: number | null;
  illuminanceLux?: number | null;
}

export interface WebDeviceConfig {
  version: number;
  zones: WebZone[];
  calibrationZones?: WebZone[];
  floorplan?: {
    enabled: boolean;
    hasImage: boolean;
    scaleMmPerPx?: number;
    radarOcclusionIgnoredEdges?: string[];
    radar?: {
      originX: number;
      originY: number;
      rotation: number;
      scale: number;
    };
  };
}

export interface WebStatsEntry {
  d?: number;
  s?: number;
  e?: number;
  f: number;
  r: number;
  fz: number[];
  rz: number[];
  sz: number[];
}

export interface WebStatsHeatmap {
  version: number;
  cols: number;
  rows: number;
  cellMm: number;
  encoding: "rle";
  today: string;
  daily?: Array<string | { d?: number; data?: string }>;
}

export interface WebStatsSummaryEntry {
  days: number;
  f: number;
  r: number;
  fz: number[];
  rz: number[];
  sz: number[];
}

export interface WebStatsSummary {
  last3Days?: WebStatsSummaryEntry;
  last7Days?: WebStatsSummaryEntry;
  last15Days?: WebStatsSummaryEntry;
  last30Days?: WebStatsSummaryEntry;
}

export interface WebDeviceStats {
  today: WebStatsEntry | null;
  daily: Array<WebStatsEntry | null>;
  summary?: WebStatsSummary;
  heatmap?: WebStatsHeatmap;
}
