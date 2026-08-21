import { render } from "svelte/server";
import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import PairingCalibrationScene from "./PairingCalibrationScene.svelte";

const floorplan: FloorplanStorageDocument = {
  version: 2,
  floorplanId: "fp-12345678901234567890123456789012",
  geometryVersion: 4,
  image: { path: "/floorplan.webp", mime: "image/webp", width: 500, height: 400 },
  scale: {
    widthMm: 5000,
    heightMm: 4000,
    outerBoundsPx: [0, 0, 500, 400],
    mmPerPxX: 10,
    mmPerPxY: 10
  },
  radar: { originPx: [100, 100], rotationDeg: 0, scale: 1, poseVersion: 3 },
  rooms: [],
  occlusion: { ignoredEdges: [] }
};

const model = {
  referenceRadar: { xPx: 100, yPx: 100, rotationDegrees: 0 },
  peerRadar: { xPx: 105, yPx: 100, rotationDegrees: 5 },
  referenceTarget: { xPx: 100, yPx: 30 },
  peerTarget: { xPx: 106, yPx: 32 },
  referenceTargetCount: 1,
  peerTargetCount: 1,
  targetSeparationMillimeters: 63,
  captureReady: true
};

describe("PairingCalibrationScene", () => {
  it("renders both devices and the second-position baseline on the shared floorplan", () => {
    const { body } = render(PairingCalibrationScene, {
      props: {
        floorplan,
        imageUrl: "http://device/api/floorplan/image",
        model,
        samples: [{ referenceLocalMm: [0, 700], peerLocalMm: [0, 700] }],
        view: "together",
        referenceName: "This device",
        peerName: "Bedroom sensor",
        ariaLabel: "Calibration map",
        minimumBaselineLabel: "Move outside this circle"
      }
    });

    expect(body).toContain("http://device/api/floorplan/image");
    expect(body).toContain("This device");
    expect(body).toContain("Bedroom sensor");
    expect(body).toContain("Move outside this circle");
    expect(body).toContain("calibration-gap");
    expect(body).toContain("calibration-baseline");
  });

  it("shows only the selected device when the mobile view is filtered", () => {
    const { body } = render(PairingCalibrationScene, {
      props: {
        floorplan,
        imageUrl: "http://device/api/floorplan/image",
        model,
        samples: [],
        view: "reference",
        referenceName: "This device",
        peerName: "Bedroom sensor",
        ariaLabel: "Calibration map",
        minimumBaselineLabel: "Move outside this circle"
      }
    });

    expect(body).toContain("This device");
    expect(body).not.toContain("Bedroom sensor");
    expect(body).not.toContain("calibration-gap");
  });
});
