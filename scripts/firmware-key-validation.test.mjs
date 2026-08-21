import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import test from "node:test";

import { generateFirmwareKeySet } from "./firmware-keygen.mjs";
import { validateFirmwareKeyPassphrase } from "./firmware-key-validation.mjs";

const passphrases = {
  root: "root-passphrase-validation-0001",
  release: "release-passphrase-validation-0001",
  developer: "developer-passphrase-validation-0001"
};

test("private key validation requires the correct passphrase and public pair", () => {
  const parent = mkdtempSync(resolve(tmpdir(), "namo-key-validation-"));
  const keys = resolve(parent, "keys");
  try {
    generateFirmwareKeySet({ outputDirectory: keys, passphrases });
    assert.match(
      validateFirmwareKeyPassphrase({
        role: "developer",
        keyDirectory: keys,
        passphrase: passphrases.developer
      }),
      /^[0-9a-f]{32}$/
    );
    assert.throws(
      () => validateFirmwareKeyPassphrase({
        role: "developer",
        keyDirectory: keys,
        passphrase: "incorrect-passphrase-0001"
      })
    );

    writeFileSync(
      resolve(keys, "developer.public.pem"),
      readFileSync(resolve(keys, "release.public.pem"))
    );
    assert.throws(
      () => validateFirmwareKeyPassphrase({
        role: "developer",
        keyDirectory: keys,
        passphrase: passphrases.developer
      }),
      /does not match/
    );
  } finally {
    rmSync(parent, { recursive: true, force: true });
  }
});

test("private key validation rejects unsupported roles and short secrets", () => {
  assert.throws(
    () => validateFirmwareKeyPassphrase({
      role: "root",
      keyDirectory: ".",
      passphrase: passphrases.root
    }),
    /unsupported/
  );
  assert.throws(
    () => validateFirmwareKeyPassphrase({
      role: "release",
      keyDirectory: ".",
      passphrase: "short"
    }),
    /invalid/
  );
});
