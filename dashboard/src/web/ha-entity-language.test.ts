import { describe, expect, it } from "vitest";
import globalsSource from "../../../packages/namo/globals.yaml?raw";
import directionSource from "../../../packages/namo/lambdas/interval-500ms.yaml?raw";
import primaryEntitiesSource from "../../../packages/namo/primary-entities.yaml?raw";
import selectsSource from "../../../packages/namo/selects.yaml?raw";
import sensorsSource from "../../../packages/namo/sensors.yaml?raw";
import textSensorsSource from "../../../packages/namo/text-sensors.yaml?raw";
import zonesSource from "../../../packages/namo/zones.yaml?raw";

const HANGUL = /[\uac00-\ud7a3]/;

function yamlValues(source: string, keys: string[]): string[] {
  const keyPattern = keys.join("|");
  return [...source.matchAll(new RegExp(`^\\s+(?:${keyPattern}):\\s*["']([^"']*)["']`, "gm"))].map(
    (match) => match[1]
  );
}

describe("Home Assistant entity language", () => {
  it("keeps HA-facing entity names and defaults English-only", () => {
    const entitySources = [primaryEntitiesSource, zonesSource, sensorsSource, textSensorsSource];
    const exposedValues = entitySources.flatMap((source) => yamlValues(source, ["name", "initial_value"]));

    expect(exposedValues.length).toBeGreaterThan(0);
    expect(exposedValues.filter((value) => HANGUL.test(value))).toEqual([]);
  });

  it("keeps the numbered primary entity names stable", () => {
    const expected = [
      "01. Presence",
      "02. Motion",
      "03. Moving Target",
      "04. Stationary Target",
      "05. Target Count",
      "06. Temperature",
      "07. Humidity",
      "08. Illuminance",
      "09. Zone 1 Presence",
      "10. Zone 2 Presence",
      "11. Zone 3 Presence",
      "12. Zone 4 Presence",
      "13. Zone 5 Presence",
      "14. Zone 6 Presence"
    ];
    const actual = yamlValues(primaryEntitiesSource, ["name"]).sort();

    expect(actual).toEqual(expected);
  });

  it("uses English canonical direction states throughout the state machine", () => {
    const directionSources = [globalsSource, selectsSource, directionSource, textSensorsSource];
    const combined = directionSources.join("\n");

    expect(combined).toContain('"Stationary"');
    expect(combined).toContain('"Approaching"');
    expect(combined).toContain('"Moving Away"');
    expect(combined).toContain('"Not Detected"');
    expect(combined).not.toMatch(/["'](?:정지|접근|멀어짐|미감지)["']/);
  });

  it("keeps the zone summary English-only", () => {
    expect(zonesSource).toContain('std::string("Detection ")');
    expect(zonesSource).toContain('" / Filter "');
  });
});
