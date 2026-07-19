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

export type WebZoneType = "detection" | "filter" | "reduced" | "disabled" | "exit";

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
  displayMode?: "static-matched" | "static-uncertain";
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
  debug?: WebDeviceDebug;
}

export interface WebDeviceDebug {
  presenceReason?: string;
  presenceOffReason?: string;
  motionReason?: string;
  lastPresenceDropMs?: number | null;
  lastValidTargetAgeMs?: number | null;
  emptySamplesConsecutive?: number;
  shortPresenceDropCount?: number;
  longPresenceDropCount?: number;
  presenceEvidence?: {
    pir?: boolean;
    tracker?: boolean;
    staticAssist?: boolean;
  };
  still?: {
    state?: string;
    reason?: string;
    confidence?: number;
    holdActive?: boolean;
    lastSeenAgeMs?: number | null;
    anchor?: {
      x?: number;
      y?: number;
    } | null;
  };
  range?: {
    reason?: string;
    suspectTargetCount?: number;
    outOfRangeTargetCount?: number;
    remoteCandidateCount?: number;
  };
  staticRadar?: {
    available?: boolean;
    presence?: boolean;
    moving?: boolean;
    still?: boolean;
    detectionDistanceMm?: number;
    movingDistanceMm?: number;
    stillDistanceMm?: number;
    movingEnergy?: number;
    stillEnergy?: number;
    reason?: "unavailable" | "moving" | "still" | "target" | "clear" | string;
    assist?: {
      armed?: boolean;
      active?: boolean;
      armPending?: boolean;
      armElapsedMs?: number;
      exitVeto?: boolean;
      heldTarget?: {
        id?: string;
        x?: number;
        y?: number;
        lastDistanceMm?: number;
        staticDistanceMm?: number | null;
        distanceDeltaMm?: number | null;
        distanceMatched?: boolean;
      } | null;
    };
  };
  room?: {
    configured?: boolean;
    boundaryPointCount?: number;
    insideTargetCount?: number;
    outsideTargetCount?: number;
  };
  exit?: {
    active?: boolean;
    zoneCount?: number;
    zoneMask?: number;
    targetCount?: number;
    lastSeenAgeMs?: number | null;
  };
  tracker?: {
    presence?: boolean;
    motion?: boolean;
    state?: string;
    reason?: string;
    dropReason?: string;
    dropMs?: number;
    exitTrackCount?: number;
    trackScore?: number;
    inputDetectionCount?: number;
    activeTrackCount?: number;
    tentativeTrackCount?: number;
    confirmedTrackCount?: number;
    coastingTrackCount?: number;
    movingTrackCount?: number;
    stillTrackCount?: number;
    tracks?: Array<{
      state?: string;
      trackScore?: number;
      observedFrames?: number;
      missedFrames?: number;
      recentHits?: number;
      reason?: string;
      stateChangedMs?: number;
      moving?: boolean;
      x?: number;
      y?: number;
      vx?: number;
      vy?: number;
      speedCmS?: number;
      distance?: number;
      radialVelocityCmS?: number;
      direction?: string;
      exitMask?: number;
      exitAgeMs?: number | null;
      exitRecent?: boolean;
      roomState?: string;
    }>;
  };
}

export interface WebDeviceConfig {
  version: number;
  integrationMode?: "unknown" | "edge" | "ha";
  trackerAssistPresence?: boolean;
  legacyPresenceFallback?: boolean;
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
    room?: {
      id: string;
      name: string;
      source: "stored_room";
      boundary?: Array<[number, number]>;
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
