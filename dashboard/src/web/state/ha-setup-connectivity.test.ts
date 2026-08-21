import { describe, expect, it } from "vitest";

import { esphomeWifiReady, haRuntimeReady, haSetupConnectivity } from "./ha-setup-connectivity";

describe("Home Assistant setup connectivity", () => {
  it("uses ESPHome-owned Wi-Fi as the setup handoff boundary", () => {
    expect(esphomeWifiReady(undefined)).toBe(false);
    expect(esphomeWifiReady({ ok: true, wifi: { connected: false } })).toBe(false);
    expect(esphomeWifiReady({ ok: true, wifi: { connected: true } })).toBe(true);
  });

  it("waits for ESPHome Wi-Fi ownership before checking the API", () => {
    expect(
      haSetupConnectivity({
        ok: true,
        wifi: { connected: false },
        api: { serviceReady: true, clientConnected: true }
      })
    ).toBe("wifi-not-ready");
  });

  it("distinguishes an unready API service from Wi-Fi failure", () => {
    expect(
      haSetupConnectivity({
        ok: true,
        wifi: { connected: true },
        api: { serviceReady: false, clientConnected: false }
      })
    ).toBe("api-not-ready");
  });

  it("treats an API service waiting for Home Assistant as ready", () => {
    const status = {
      ok: true,
      wifi: { connected: true },
      api: { serviceReady: true, clientConnected: false, warning: true }
    };

    expect(haSetupConnectivity(status)).toBe("client-waiting");
    expect(haRuntimeReady(status)).toBe(true);
  });

  it("accepts both the explicit and compatibility client fields", () => {
    expect(
      haSetupConnectivity({
        ok: true,
        wifi: { connected: true },
        api: { serviceReady: true, clientConnected: true }
      })
    ).toBe("client-connected");
    expect(
      haSetupConnectivity({
        ok: true,
        wifi: { connected: true },
        api: { serviceReady: true, connected: true }
      })
    ).toBe("client-connected");
  });
});
