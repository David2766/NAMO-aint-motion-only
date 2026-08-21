import { render } from "svelte/server";
import { describe, expect, it } from "vitest";
import { messages } from "../i18n";
import StaticRadarTuningPanel from "./StaticRadarTuningPanel.svelte";

describe("StaticRadarTuningPanel", () => {
  it.each(["en", "ko"] as const)("renders the localized factory reset action in %s", (language) => {
    const catalog = messages[language];
    const { body } = render(StaticRadarTuningPanel, {
      props: {
        messages: catalog,
        status: {
          ok: true,
          available: true,
          active: true,
          resolutionMm: 750,
          gates: [{
            gate: 0,
            startMm: 0,
            endMm: 750,
            sensitivity: 50,
            moveEnergy: 10,
            stillEnergy: 20
          }]
        },
        detectedGate: 0,
        selectedGate: 0,
        sensitivity: 50,
        sensitivityDirty: false,
        busy: false,
        error: "",
        onSelectGate: () => {},
        onSetSensitivity: () => {},
        onApply: () => {},
        onFactoryReset: () => {}
      }
    });

    expect(body).toContain(catalog.zones.staticRadarFactoryReset);
    expect(body).toContain(catalog.zones.staticRadarSensitivity);
    expect(body).not.toContain(catalog.zones.staticRadarFactoryResetDescription);
  });

  it("does not render the loading state together with an error", () => {
    const catalog = messages.ko;
    const { body } = render(StaticRadarTuningPanel, {
      props: {
        messages: catalog,
        status: null,
        detectedGate: null,
        selectedGate: 0,
        sensitivity: 50,
        sensitivityDirty: false,
        busy: false,
        error: catalog.zones.staticRadarUnavailable,
        onSelectGate: () => {},
        onSetSensitivity: () => {},
        onApply: () => {},
        onFactoryReset: () => {}
      }
    });

    expect(body).toContain(catalog.zones.staticRadarUnavailable);
    expect(body).not.toContain(catalog.zones.staticRadarTuningStarting);
  });

  it("renders a failed device phase as an error instead of loading forever", () => {
    const catalog = messages.ko;
    const { body } = render(StaticRadarTuningPanel, {
      props: {
        messages: catalog,
        status: {
          ok: true,
          available: true,
          active: false,
          phase: "failed",
          result: "ack_timeout",
          resolutionMm: 750,
          gates: []
        },
        detectedGate: null,
        selectedGate: 0,
        sensitivity: 50,
        sensitivityDirty: false,
        busy: false,
        error: "",
        onSelectGate: () => {},
        onSetSensitivity: () => {},
        onApply: () => {},
        onFactoryReset: () => {}
      }
    });

    expect(body).toContain(catalog.zones.staticRadarLoadFailed);
    expect(body).not.toContain(catalog.zones.staticRadarTuningStarting);
  });
});
