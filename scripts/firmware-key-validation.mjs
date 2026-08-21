import { createPublicKey, timingSafeEqual } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
  loadPrivateKey,
  loadPublicKey,
  p256KeyId
} from "./firmware-signing.mjs";

const supportedRoles = new Set(["developer", "release"]);

export function validateFirmwareKeyPassphrase({
  role,
  keyDirectory,
  passphrase
}) {
  if (!supportedRoles.has(role)) {
    throw new Error(`unsupported firmware key role: ${role}`);
  }
  if (typeof passphrase !== "string" || passphrase.length < 16) {
    throw new Error("private key passphrase is invalid");
  }

  const directory = resolve(keyDirectory);
  const privatePath = resolve(directory, `${role}.private.pem`);
  const publicPath = resolve(directory, `${role}.public.pem`);
  for (const path of [privatePath, publicPath]) {
    if (!existsSync(path)) {
      throw new Error(`firmware key file not found: ${path}`);
    }
  }

  const privateKey = loadPrivateKey(readFileSync(privatePath), passphrase);
  const expectedPublic = loadPublicKey(readFileSync(publicPath));
  const derivedId = p256KeyId(createPublicKey(privateKey));
  const expectedId = p256KeyId(expectedPublic);
  if (
    derivedId.length !== expectedId.length ||
    !timingSafeEqual(derivedId, expectedId)
  ) {
    throw new Error("private key does not match the role public key");
  }
  return expectedId.toString("hex");
}

function valueAfter(args, option) {
  const index = args.indexOf(option);
  return index >= 0 ? args[index + 1] : undefined;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes("--help") || args.includes("-h")) {
    console.log("Usage: node scripts/firmware-key-validation.mjs --role developer|release --key-directory <path>");
    console.log("Requires NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE.");
    return;
  }
  const role = valueAfter(args, "--role");
  const keyDirectory = valueAfter(args, "--key-directory");
  if (!role) throw new Error("--role is required");
  if (!keyDirectory) throw new Error("--key-directory is required");
  const keyId = validateFirmwareKeyPassphrase({
    role,
    keyDirectory,
    passphrase: process.env.NAMO_FIRMWARE_KEY_VALIDATION_PASSPHRASE
  });
  console.log(`firmware-key-validation: ${role} key verified id=${keyId}`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch {
    console.error("firmware-key-validation: passphrase or key pair is invalid");
    process.exitCode = 1;
  }
}
