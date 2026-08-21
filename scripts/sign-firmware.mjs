import { createPublicKey } from "node:crypto";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

import {
  createFirmwarePackage,
  assertFirmwareEmbedsPublicKeys,
  assertFirmwareSigningIdentity,
  assertSignedOtaFirmwareImage,
  loadPrivateKey,
  loadPublicKey,
  p256KeyId,
  readEspFirmwareVersion,
  verifyFirmwarePackage
} from "./firmware-signing.mjs";

function fail(message) {
  throw new Error(message);
}

function option(args, name, required = true) {
  const index = args.indexOf(name);
  const value = index >= 0 ? args[index + 1] : undefined;
  if (required && !value) fail(`${name} is required`);
  return value;
}

function uint32(value, name) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < 1 || parsed > 0xffffffff)
    fail(`${name} must be a positive uint32`);
  return parsed;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes("--help") || args.includes("-h")) {
    console.log("Usage: node scripts/sign-firmware.mjs --role release|developer|root_recovery --firmware firmware.ota.bin --private-key key.pem --root-public root.pem --release-public release.pem --developer-public developer.pem --version 0.6.0 --build build-id --trust-revision 1 --security-epoch 1 --out firmware.namo-ota");
    console.log("Recovery additionally requires --revoked-public old.pem --replacement-public new.pem --replacement-role release|developer.");
    console.log("The private key passphrase is read from NAMO_SIGNING_KEY_PASSPHRASE.");
    return;
  }
  const role = option(args, "--role");
  if (!["release", "developer", "root_recovery"].includes(role))
    fail("--role must be release, developer, or root_recovery");
  const firmwarePath = resolve(option(args, "--firmware"));
  const privatePath = resolve(option(args, "--private-key"));
  const rootPublicPath = resolve(option(args, "--root-public"));
  const releasePublicPath = resolve(option(args, "--release-public"));
  const developerPublicPath = resolve(option(args, "--developer-public"));
  const outputPath = resolve(option(args, "--out"));
  for (const path of [firmwarePath, privatePath, rootPublicPath,
    releasePublicPath, developerPublicPath]) {
    if (!existsSync(path)) fail(`file not found: ${path}`);
  }
  const passphrase = String(process.env.NAMO_SIGNING_KEY_PASSPHRASE ?? "");
  if (!passphrase) fail("NAMO_SIGNING_KEY_PASSPHRASE is required");
  const privateKey = loadPrivateKey(readFileSync(privatePath), passphrase);
  const rootPublicKey = loadPublicKey(readFileSync(rootPublicPath));
  const releasePublicKey = loadPublicKey(readFileSync(releasePublicPath));
  const developerPublicKey = loadPublicKey(readFileSync(developerPublicPath));
  const firmware = readFileSync(firmwarePath);
  assertSignedOtaFirmwareImage(firmware);
  const embeddedTrustKeys = assertFirmwareSigningIdentity({
    role,
    privateKey,
    rootPublicKey,
    releasePublicKey,
    developerPublicKey
  });
  assertFirmwareEmbedsPublicKeys(firmware, embeddedTrustKeys);
  const requestedVersion = option(args, "--version");
  const embeddedVersion = readEspFirmwareVersion(firmware);
  if (embeddedVersion !== requestedVersion)
    fail(`firmware version mismatch: expected ${requestedVersion}, got ${embeddedVersion}`);
  const parameters = {
    firmware,
    privateKey,
    role,
    firmwareVersion: requestedVersion,
    buildId: option(args, "--build"),
    trustRevision: uint32(option(args, "--trust-revision"), "trust revision"),
    securityEpoch: uint32(option(args, "--security-epoch"), "security epoch")
  };
  if (role === "root_recovery") {
    const revokedPublicPath = resolve(option(args, "--revoked-public"));
    const replacementPublicPath = resolve(option(args, "--replacement-public"));
    const replacementRole = option(args, "--replacement-role");
    if (!["release", "developer"].includes(replacementRole))
      fail("--replacement-role must be release or developer");
    parameters.revokedKeyId = p256KeyId(loadPublicKey(readFileSync(revokedPublicPath)));
    parameters.replacementPublicKey = loadPublicKey(readFileSync(replacementPublicPath));
    assertFirmwareEmbedsPublicKeys(firmware, [parameters.replacementPublicKey]);
    parameters.replacementRole = replacementRole;
  }
  const packageBytes = createFirmwarePackage(parameters);
  verifyFirmwarePackage(packageBytes, createPublicKey(privateKey));
  writeFileSync(outputPath, packageBytes, { flag: "wx", mode: 0o644 });
  console.log(`sign-firmware: verified and wrote ${outputPath}`);
}

try {
  main();
} catch (error) {
  console.error(`sign-firmware: ${error instanceof Error ? error.message : String(error)}`);
  process.exitCode = 1;
}
