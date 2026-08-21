import { describe, expect, it } from "vitest";
import { parseEspFirmwareMetadata } from "./firmware-image";

function writeCString(bytes: Uint8Array, offset: number, value: string): void {
  for (let index = 0; index < value.length; index += 1) bytes[offset + index] = value.charCodeAt(index);
}

function firmwareImage(): Uint8Array {
  const bytes = new Uint8Array(320);
  const view = new DataView(bytes.buffer);
  bytes[0] = 0xe9;
  view.setUint32(32, 0xabcd5432, true);
  writeCString(bytes, 48, "2026.5.1");
  writeCString(bytes, 80, "presence-sensor");
  writeCString(bytes, 112, "21:14:05");
  writeCString(bytes, 128, "Aug  4 2026");
  writeCString(bytes, 176, "Project truststorm.presence-sensor version 0.5.5-z");
  return bytes;
}

describe("parseEspFirmwareMetadata", () => {
  it("reads the fixed ESP-IDF application descriptor", () => {
    expect(parseEspFirmwareMetadata(firmwareImage())).toEqual({
      version: "0.5.5-z",
      projectName: "presence-sensor",
      buildTime: "Aug  4 2026 21:14:05",
      frameworkVersion: "2026.5.1"
    });
  });

  it("rejects a bin file without an ESP application descriptor", () => {
    expect(() => parseEspFirmwareMetadata(new Uint8Array(224))).toThrow("firmware_image_invalid");
  });
});
