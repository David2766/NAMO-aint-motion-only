import { describe, expect, it, vi } from "vitest";
import { sha256 } from "../../core/sha256";
import { parseNamoFirmwarePackage } from "./firmware-package";

function writeAscii(bytes: Uint8Array, offset: number, value: string): void {
  for (let index = 0; index < value.length; index += 1) bytes[offset + index] = value.charCodeAt(index);
}

function firmwareImage(): Uint8Array {
  const bytes = new Uint8Array(320);
  const view = new DataView(bytes.buffer);
  bytes[0] = 0xe9;
  view.setUint32(32, 0xabcd5432, true);
  writeAscii(bytes, 48, "2026.5.1");
  writeAscii(bytes, 80, "presence-sensor");
  writeAscii(bytes, 112, "21:14:05");
  writeAscii(bytes, 128, "Aug  6 2026");
  writeAscii(bytes, 176, "Project truststorm.presence-sensor version 0.6.0");
  return bytes;
}

async function signedPackage(): Promise<Uint8Array> {
  const image = firmwareImage();
  const bytes = new Uint8Array(320 + image.length);
  const view = new DataView(bytes.buffer);
  writeAscii(bytes, 0, "NAMOOTA1");
  view.setUint16(8, 1, true);
  view.setUint16(10, 320, true);
  bytes[12] = 1;
  bytes[13] = 1;
  view.setUint32(16, 0x3153504e, true);
  view.setUint32(20, 0x31335358, true);
  view.setUint32(24, image.length, true);
  view.setUint32(28, 4, true);
  view.setUint32(32, 9, true);
  bytes.fill(0x11, 36, 52);
  bytes.set(sha256(image), 52);
  writeAscii(bytes, 84, "0.6.0");
  writeAscii(bytes, 108, "20260806T120000Z");
  bytes.fill(0x80, 256, 320);
  bytes.set(image, 320);
  return bytes;
}

describe("parseNamoFirmwarePackage", () => {
  it("validates the signed envelope digest and reads the embedded ESP image", async () => {
    const metadata = await parseNamoFirmwarePackage(await signedPackage());
    expect(metadata).toMatchObject({
      packageFormat: "namo-signed",
      signerRole: "release",
      trustRevision: 4,
      securityEpoch: 9,
      imageSize: 320,
      signedVersion: "0.6.0",
      signedBuildId: "20260806T120000Z",
      version: "0.6.0"
    });
  });

  it("works on a plain HTTP dashboard without Web Crypto", async () => {
    const packageBytes = await signedPackage();
    vi.stubGlobal("crypto", undefined);
    try {
      await expect(parseNamoFirmwarePackage(packageBytes)).resolves.toMatchObject({
        packageFormat: "namo-signed",
        version: "0.6.0"
      });
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it("rejects image mutation, truncation, reserved fields, and metadata mismatch", async () => {
    const original = await signedPackage();
    const changed = original.slice();
    changed[400] ^= 1;
    await expect(parseNamoFirmwarePackage(changed)).rejects.toThrow("firmware_package_digest_invalid");
    await expect(parseNamoFirmwarePackage(original.subarray(0, -1))).rejects.toThrow("firmware_package_invalid");
    const reserved = original.slice();
    reserved[230] = 1;
    await expect(parseNamoFirmwarePackage(reserved)).rejects.toThrow("firmware_package_invalid");
    const mismatch = original.slice();
    writeAscii(mismatch, 84, "0.7.0");
    mismatch[89] = 0;
    await expect(parseNamoFirmwarePackage(mismatch)).rejects.toThrow("firmware_package_version_mismatch");
  });

  it("rejects incomplete recovery fields and empty signer or signature material", async () => {
    const original = await signedPackage();
    const emptySigner = original.slice();
    emptySigner.fill(0, 36, 52);
    await expect(parseNamoFirmwarePackage(emptySigner)).rejects.toThrow("firmware_package_invalid");
    const emptySignature = original.slice();
    emptySignature.fill(0, 256, 320);
    await expect(parseNamoFirmwarePackage(emptySignature)).rejects.toThrow("firmware_package_invalid");
    const illegalRecovery = original.slice();
    illegalRecovery[132] = 1;
    await expect(parseNamoFirmwarePackage(illegalRecovery)).rejects.toThrow("firmware_package_invalid");
    const incompleteRecovery = original.slice();
    incompleteRecovery[12] = 3;
    incompleteRecovery.fill(0x21, 132, 148);
    await expect(parseNamoFirmwarePackage(incompleteRecovery)).rejects.toThrow("firmware_package_invalid");
  });

  it("requires a recovery replacement key id derived from the exact P-256 key", async () => {
    const recovery = await signedPackage();
    recovery[12] = 3;
    recovery.fill(0x31, 132, 148);
    const rawPublicKey = new Uint8Array(65);
    rawPublicKey[0] = 0x04;
    rawPublicKey.fill(0x71, 1);
    recovery.set(rawPublicKey, 164);
    recovery[229] = 1;
    const spkiPrefix = new Uint8Array([
      0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
      0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
      0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
    ]);
    const spki = new Uint8Array(spkiPrefix.length + rawPublicKey.length);
    spki.set(spkiPrefix);
    spki.set(rawPublicKey, spkiPrefix.length);
    recovery.set(sha256(spki).subarray(0, 16), 148);
    await expect(parseNamoFirmwarePackage(recovery)).resolves.toMatchObject({
      signerRole: "root_recovery"
    });

    const mismatched = recovery.slice();
    mismatched[148] ^= 1;
    await expect(parseNamoFirmwarePackage(mismatched)).rejects.toThrow("firmware_package_invalid");
  });
});
