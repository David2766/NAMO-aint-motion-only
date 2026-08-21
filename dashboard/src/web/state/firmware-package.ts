import { parseEspFirmwareMetadata, type FirmwareImageMetadata } from "./firmware-image";
import { sha256 } from "../../core/sha256";

export const NAMO_FIRMWARE_HEADER_SIZE = 320;
const SIGNED_SIZE = 256;
const MAX_IMAGE_SIZE = 0x300000;
const MAGIC = new TextEncoder().encode("NAMOOTA1");
const P256_SPKI_PREFIX = new Uint8Array([
  0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
  0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
  0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
]);

export interface NamoFirmwarePackageMetadata extends FirmwareImageMetadata {
  packageFormat: "namo-signed";
  signerRole: "release" | "developer" | "root_recovery";
  trustRevision: number;
  securityEpoch: number;
  imageSize: number;
  signedVersion: string;
  signedBuildId: string;
}

function fixedAscii(bytes: Uint8Array, offset: number, size: number): string {
  const field = bytes.subarray(offset, offset + size);
  const terminator = field.indexOf(0);
  if (terminator <= 0) throw new Error("firmware_package_text_invalid");
  for (let index = 0; index < field.length; index += 1) {
    const value = field[index];
    if (index >= terminator) {
      if (value !== 0) throw new Error("firmware_package_text_invalid");
    } else if (value < 0x20 || value > 0x7e) {
      throw new Error("firmware_package_text_invalid");
    }
  }
  return new TextDecoder().decode(field.subarray(0, terminator));
}

function equal(left: Uint8Array, right: Uint8Array): boolean {
  if (left.length !== right.length) return false;
  let difference = 0;
  for (let index = 0; index < left.length; index += 1) difference |= left[index] ^ right[index];
  return difference === 0;
}

function allZero(value: Uint8Array): boolean {
  let combined = 0;
  for (const byte of value) combined |= byte;
  return combined === 0;
}

export async function parseNamoFirmwarePackage(bytes: Uint8Array): Promise<NamoFirmwarePackageMetadata> {
  if (bytes.length < NAMO_FIRMWARE_HEADER_SIZE || !equal(bytes.subarray(0, 8), MAGIC)) {
    throw new Error("firmware_package_invalid");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint16(8, true) !== 1 || view.getUint16(10, true) !== NAMO_FIRMWARE_HEADER_SIZE ||
      bytes[13] !== 1 || view.getUint16(14, true) !== 0 ||
      view.getUint32(16, true) !== 0x3153504e || view.getUint32(20, true) !== 0x31335358 ||
      bytes.subarray(230, SIGNED_SIZE).some((value) => value !== 0)) {
    throw new Error("firmware_package_invalid");
  }
  const signerRole = bytes[12] === 1 ? "release" : bytes[12] === 2 ? "developer" : bytes[12] === 3 ? "root_recovery" : null;
  if (!signerRole) throw new Error("firmware_package_invalid");
  const imageSize = view.getUint32(24, true);
  const trustRevision = view.getUint32(28, true);
  const securityEpoch = view.getUint32(32, true);
  const signerKeyId = bytes.subarray(36, 52);
  const imageDigest = bytes.subarray(52, 84);
  const revokedKeyId = bytes.subarray(132, 148);
  const replacementKeyId = bytes.subarray(148, 164);
  const replacementPublicKey = bytes.subarray(164, 229);
  const replacementRole = bytes[229];
  const anyRecovery = !allZero(revokedKeyId) || !allZero(replacementKeyId) ||
    !allZero(replacementPublicKey) || replacementRole !== 0;
  const replacementSpki = new Uint8Array(P256_SPKI_PREFIX.length + replacementPublicKey.length);
  replacementSpki.set(P256_SPKI_PREFIX);
  replacementSpki.set(replacementPublicKey, P256_SPKI_PREFIX.length);
  const expectedReplacementKeyId = sha256(replacementSpki).subarray(0, 16);
  const completeRecovery = !allZero(revokedKeyId) && !allZero(replacementKeyId) &&
    replacementPublicKey[0] === 0x04 && !allZero(replacementPublicKey.subarray(1)) &&
    equal(replacementKeyId, expectedReplacementKeyId) &&
    (replacementRole === 1 || replacementRole === 2) && !equal(revokedKeyId, replacementKeyId);
  if (!imageSize || imageSize > MAX_IMAGE_SIZE ||
      bytes.length !== NAMO_FIRMWARE_HEADER_SIZE + imageSize ||
      !trustRevision || !securityEpoch || allZero(signerKeyId) ||
      allZero(imageDigest) || allZero(bytes.subarray(SIGNED_SIZE, NAMO_FIRMWARE_HEADER_SIZE)) ||
      (signerRole === "root_recovery" ? !completeRecovery : anyRecovery)) {
    throw new Error("firmware_package_invalid");
  }
  const image = bytes.subarray(NAMO_FIRMWARE_HEADER_SIZE);
  const actualDigest = sha256(image);
  if (!equal(actualDigest, imageDigest)) throw new Error("firmware_package_digest_invalid");
  const signedVersion = fixedAscii(bytes, 84, 24);
  const signedBuildId = fixedAscii(bytes, 108, 24);
  const metadata = parseEspFirmwareMetadata(image);
  if (metadata.version !== signedVersion) throw new Error("firmware_package_version_mismatch");
  return {
    ...metadata,
    packageFormat: "namo-signed",
    signerRole,
    trustRevision,
    securityEpoch,
    imageSize,
    signedVersion,
    signedBuildId
  };
}

export async function readFirmwareFileMetadata(file: File): Promise<FirmwareImageMetadata> {
  const bytes = new Uint8Array(await file.arrayBuffer());
  return file.name.toLowerCase().endsWith(".namo-ota")
    ? parseNamoFirmwarePackage(bytes)
    : parseEspFirmwareMetadata(bytes);
}

export async function readNamoFirmwareHeaderHex(file: File): Promise<string> {
  if (!file.name.toLowerCase().endsWith(".namo-ota") || file.size < NAMO_FIRMWARE_HEADER_SIZE) {
    throw new Error("signed_firmware_required");
  }
  const header = new Uint8Array(await file.slice(0, NAMO_FIRMWARE_HEADER_SIZE).arrayBuffer());
  return [...header].map((value) => value.toString(16).padStart(2, "0")).join("");
}
