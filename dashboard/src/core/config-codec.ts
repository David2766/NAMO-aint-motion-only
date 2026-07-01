import { normalizeAdvancedZone, type AdvancedZoneDisplayModel } from "./zones";

export function isUsableConfigText(value: string): boolean {
  return Boolean(value && value !== "unknown" && value !== "unavailable" && value !== "__EMPTY__" && value !== "{}");
}

export function advancedZoneFromConfig(rawJson: string): AdvancedZoneDisplayModel | null {
  if (!isUsableConfigText(rawJson)) return null;
  try {
    const zone = JSON.parse(rawJson) as {
      id?: unknown;
      name?: unknown;
      type?: unknown;
      points?: unknown;
    };
    return normalizeAdvancedZone(zone);
  } catch {
    return null;
  }
}

export function advancedZonesFromFullJson(rawJson: string): AdvancedZoneDisplayModel[] {
  if (!isUsableConfigText(rawJson)) return [];

  try {
    const parsed = JSON.parse(rawJson) as {
      advanced?: boolean;
      zones?: Array<{
        id?: unknown;
        name?: unknown;
        type?: unknown;
        points?: unknown;
      }>;
      calibrationZones?: Array<{
        id?: unknown;
        name?: unknown;
        type?: unknown;
        points?: unknown;
      }>;
    };
    if (!parsed.advanced) return [];
    return [...(Array.isArray(parsed.zones) ? parsed.zones : []), ...(Array.isArray(parsed.calibrationZones) ? parsed.calibrationZones : [])]
      .map((zone) => normalizeAdvancedZone(zone))
      .filter((zone): zone is AdvancedZoneDisplayModel => zone !== null);
  } catch {
    return [];
  }
}

export function advancedZonesFromDeviceConfigJson(rawJson: string): AdvancedZoneDisplayModel[] {
  if (!isUsableConfigText(rawJson)) return [];

  try {
    const parsed = JSON.parse(rawJson) as {
      zones?: Array<{
        id?: unknown;
        name?: unknown;
        type?: unknown;
        points?: unknown;
      }>;
      calibrationZones?: Array<{
        id?: unknown;
        name?: unknown;
        type?: unknown;
        points?: unknown;
      }>;
    };
    return [...(Array.isArray(parsed.zones) ? parsed.zones : []), ...(Array.isArray(parsed.calibrationZones) ? parsed.calibrationZones : [])]
      .map((zone) => normalizeAdvancedZone(zone))
      .filter((zone): zone is AdvancedZoneDisplayModel => zone !== null);
  } catch {
    return [];
  }
}

export function advancedZonesFromConfigTexts(rawConfigValues: string[], fullConfigJson = ""): AdvancedZoneDisplayModel[] {
  const zones = rawConfigValues
    .map((value) => advancedZoneFromConfig(value))
    .filter((zone): zone is AdvancedZoneDisplayModel => zone !== null);

  return zones.length > 0 ? zones : advancedZonesFromFullJson(fullConfigJson);
}
