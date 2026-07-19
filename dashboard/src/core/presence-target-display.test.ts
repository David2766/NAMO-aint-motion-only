import { describe, expect, it } from "vitest";
import { presenceDisplayTargets } from "./presence-target-display";
import type { WebDeviceState } from "./types";

function state(overrides: Partial<WebDeviceState> = {}): WebDeviceState {
  return {
    connected: true,
    updatedAt: 1,
    presence: true,
    targets: [
      { id: "target_1", name: "T1", color: "#ff6b7a", x: 0, y: 0, active: false }
    ],
    ...overrides
  };
}

function heldState(distanceMatched: boolean): WebDeviceState {
  return state({
    debug: {
      staticRadar: {
        available: true,
        presence: true,
        assist: {
          active: true,
          heldTarget: {
            id: "target_1",
            x: 600,
            y: 1800,
            lastDistanceMm: 1897,
            staticDistanceMm: distanceMatched ? 1950 : 3200,
            distanceDeltaMm: distanceMatched ? 53 : 1303,
            distanceMatched
          }
        }
      }
    }
  });
}

describe("presenceDisplayTargets", () => {
  it("preserves live targets without adding a held marker", () => {
    const live = state({
      targets: [{ id: "target_1", name: "T1", color: "#ff6b7a", x: 300, y: 1200, active: true }],
      debug: heldState(true).debug
    });

    expect(presenceDisplayTargets(live)).toEqual(live.targets);
  });

  it("shows a distance-matched held target at the last spatial coordinate", () => {
    expect(presenceDisplayTargets(heldState(true))).toContainEqual(
      expect.objectContaining({ x: 600, y: 1800, active: true, displayMode: "static-matched" })
    );
  });

  it("marks a distance-mismatched held target as spatially uncertain", () => {
    expect(presenceDisplayTargets(heldState(false))).toContainEqual(
      expect.objectContaining({ x: 600, y: 1800, active: true, displayMode: "static-uncertain" })
    );
  });

  it("does not show held coordinates after final presence turns off", () => {
    const inactive = heldState(true);
    inactive.presence = false;

    expect(presenceDisplayTargets(inactive)).toEqual(inactive.targets);
  });

  it("does not synthesize a marker without active assist or finite coordinates", () => {
    const inactiveAssist = heldState(true);
    if (inactiveAssist.debug?.staticRadar?.assist) inactiveAssist.debug.staticRadar.assist.active = false;
    expect(presenceDisplayTargets(inactiveAssist)).toEqual(inactiveAssist.targets);

    const invalidCoordinate = heldState(true);
    const held = invalidCoordinate.debug?.staticRadar?.assist?.heldTarget;
    if (held) held.x = Number.NaN;
    expect(presenceDisplayTargets(invalidCoordinate)).toEqual(invalidCoordinate.targets);
  });
});
