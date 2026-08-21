import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import test from "node:test";

import { generateFirmwareKeySet } from "./firmware-keygen.mjs";
import { loadPrivateKey, loadPublicKey, p256KeyId } from "./firmware-signing.mjs";

test("key generation encrypts every private key and never overwrites a key directory", () => {
  const parent = mkdtempSync(resolve(tmpdir(), "namo-keys-"));
  const output = resolve(parent, "keys");
  const passphrases = {
    root: "root-passphrase-0001",
    release: "release-passphrase-0001",
    developer: "developer-passphrase-0001"
  };
  try {
    generateFirmwareKeySet({ outputDirectory: output, passphrases });
    const ids = [];
    for (const role of ["root", "release", "developer"]) {
      const privatePem = readFileSync(resolve(output, `${role}.private.pem`));
      const publicPem = readFileSync(resolve(output, `${role}.public.pem`));
      assert.match(privatePem.toString("ascii"), /BEGIN ENCRYPTED PRIVATE KEY/);
      assert.doesNotMatch(privatePem.toString("ascii"), /BEGIN PRIVATE KEY/);
      const privateKey = loadPrivateKey(privatePem, passphrases[role]);
      assert.equal(privateKey.type, "private");
      ids.push(p256KeyId(loadPublicKey(publicPem)).toString("hex"));
    }
    assert.equal(new Set(ids).size, 3);
    assert.throws(
      () => generateFirmwareKeySet({ outputDirectory: output, passphrases }),
      /already exists/
    );
  } finally {
    rmSync(parent, { recursive: true, force: true });
  }
});

test("short passphrases are rejected before creating output", () => {
  const parent = mkdtempSync(resolve(tmpdir(), "namo-keys-"));
  const output = resolve(parent, "keys");
  try {
    assert.throws(() => generateFirmwareKeySet({
      outputDirectory: output,
      passphrases: { root: "short", release: "long-enough-passphrase", developer: "long-enough-passphrase" }
    }), /at least 16/);
  } finally {
    rmSync(parent, { recursive: true, force: true });
  }
});
