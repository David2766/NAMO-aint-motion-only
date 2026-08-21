import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { generateKeyPairSync } from "node:crypto";
import { fileURLToPath } from "node:url";

function fail(message) {
  throw new Error(message);
}

function valueAfter(args, option) {
  const index = args.indexOf(option);
  return index >= 0 ? args[index + 1] : undefined;
}

function writeExclusive(path, data, mode) {
  writeFileSync(path, data, { flag: "wx", mode });
}

export function generateFirmwareKeySet({ outputDirectory, passphrases }) {
  const output = resolve(outputDirectory);
  if (existsSync(output))
    fail(`output directory already exists: ${output}`);
  for (const role of ["root", "release", "developer"]) {
    if (typeof passphrases?.[role] !== "string" || passphrases[role].length < 16)
      fail(`${role} passphrase must contain at least 16 characters`);
  }
  mkdirSync(output, { recursive: false, mode: 0o700 });
  for (const role of ["root", "release", "developer"]) {
    const { privateKey, publicKey } = generateKeyPairSync("ec", {
      namedCurve: "prime256v1",
      privateKeyEncoding: {
        type: "pkcs8",
        format: "pem",
        cipher: "aes-256-cbc",
        passphrase: passphrases[role]
      },
      publicKeyEncoding: { type: "spki", format: "pem" }
    });
    writeExclusive(resolve(output, `${role}.private.pem`), privateKey, 0o600);
    writeExclusive(resolve(output, `${role}.public.pem`), publicKey, 0o644);
  }
  return output;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes("--help") || args.includes("-h")) {
    console.log("Usage: node scripts/firmware-keygen.mjs --out <new-directory>");
    console.log("Requires NAMO_ROOT_KEY_PASSPHRASE, NAMO_RELEASE_KEY_PASSPHRASE, and NAMO_DEVELOPER_KEY_PASSPHRASE.");
    return;
  }
  const outputDirectory = valueAfter(args, "--out");
  if (!outputDirectory) fail("--out is required");
  const output = generateFirmwareKeySet({
    outputDirectory,
    passphrases: {
      root: process.env.NAMO_ROOT_KEY_PASSPHRASE,
      release: process.env.NAMO_RELEASE_KEY_PASSPHRASE,
      developer: process.env.NAMO_DEVELOPER_KEY_PASSPHRASE
    }
  });
  console.log(`firmware-keygen: wrote encrypted private keys and public keys to ${output}`);
  console.log("firmware-keygen: keep root.private.pem offline and never place private keys in the repository");
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    console.error(`firmware-keygen: ${error instanceof Error ? error.message : String(error)}`);
    process.exitCode = 1;
  }
}
