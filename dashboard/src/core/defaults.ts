import type { RadarCardConfig, RadarZoneId } from "./types";

export const DEFAULT_CARD_CONFIG: RadarCardConfig = {
  title: "Radar Map",
  range_x: 3000,
  range_y: 6000,
  hold_ms: 1500,
  show_distance: true,
  distance_decimals: 2,
  targets: [],
  device_id: "",
  configurator_url: "",
  use_yaml_targets: undefined,
  zone_names: {}
};

export function targetColor(index: number): string {
  return ["#ff6b7a", "#ffd166", "#06d6a0"][index] || "#d7eefc";
}

export function zoneLabel(zoneId: RadarZoneId): string {
  return {
    zone_1: "Zone 1",
    zone_2: "Zone 2",
    zone_3: "Zone 3"
  }[zoneId];
}
