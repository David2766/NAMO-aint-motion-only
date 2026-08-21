import { describe, expect, it } from "vitest";
import { pairingDeviceAddressLabel, pairingDeviceShortName } from "./pairing-device-label";

describe("pairing device labels", () => {
  it("shows only the unique suffix of the standard hostname", () => {
    expect(pairingDeviceShortName("presence-sensor-50d884", "Nearby device")).toBe("50d884");
  });

  it("keeps a custom hostname and falls back when no name is available", () => {
    expect(pairingDeviceShortName("office-radar", "Nearby device")).toBe("office-radar");
    expect(pairingDeviceShortName("", "Nearby device")).toBe("Nearby device");
    expect(pairingDeviceShortName("presence-sensor-", "Nearby device")).toBe("Nearby device");
  });

  it("normalizes a device origin to its IP address", () => {
    expect(pairingDeviceAddressLabel("http://192.168.29.78:80")).toBe("192.168.29.78");
    expect(pairingDeviceAddressLabel("192.168.29.178")).toBe("192.168.29.178");
  });
});
