import assert from "node:assert/strict";
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync
} from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

import { generateFirmwareKeySet } from "./firmware-keygen.mjs";

const scripts = import.meta.dirname;
const registerScript = resolve(scripts, "register-firmware-secret.ps1");
const buildScript = resolve(scripts, "build-firmware.ps1");
const passphrases = {
  root: "root-passphrase-secret-store-0001",
  release: "release-passphrase-secret-store-0001",
  developer: "developer-passphrase-secret-store-0001"
};

function powershell(script, args, passphrase) {
  const env = { ...process.env };
  if (passphrase !== undefined) {
    env.NAMO_TEST_FIRMWARE_PASSPHRASE = passphrase;
  }
  return spawnSync(
    "powershell.exe",
    ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, ...args],
    {
      encoding: "utf8",
      env,
      timeout: 10000,
      windowsHide: true
    }
  );
}

test("DPAPI registration rejects wrong secrets without saving or replacing", () => {
  const parent = mkdtempSync(resolve(tmpdir(), "namo-secret-store-"));
  const keys = resolve(parent, "keys");
  const secret = resolve(keys, ".secrets", "developer.dpapi");
  try {
    generateFirmwareKeySet({ outputDirectory: keys, passphrases });

    const rejected = powershell(registerScript, [
      "-Role", "developer",
      "-KeyDirectory", keys,
      "-PassphraseEnvironment", "NAMO_TEST_FIRMWARE_PASSPHRASE"
    ], "incorrect-passphrase-secret-store-0001");
    assert.notEqual(rejected.status, 0);
    assert.equal(existsSync(secret), false);
    assert.match(rejected.stderr, /no secret was saved/);

    const registered = powershell(registerScript, [
      "-Role", "developer",
      "-KeyDirectory", keys,
      "-PassphraseEnvironment", "NAMO_TEST_FIRMWARE_PASSPHRASE"
    ], passphrases.developer);
    assert.equal(registered.status, 0, registered.stderr);
    const original = readFileSync(secret, "utf8");
    assert.match(original, /^NAMO-DPAPI-V1\r?\n/);
    assert.doesNotMatch(original, new RegExp(passphrases.developer));

    const invalidReplacement = powershell(registerScript, [
      "-Role", "developer",
      "-KeyDirectory", keys,
      "-Replace",
      "-PassphraseEnvironment", "NAMO_TEST_FIRMWARE_PASSPHRASE"
    ], "incorrect-replacement-secret-store-0001");
    assert.notEqual(invalidReplacement.status, 0);
    assert.equal(readFileSync(secret, "utf8"), original);

    const verified = powershell(buildScript, [
      "-Signer", "developer",
      "-KeyDirectory", keys,
      "-ValidateOnly"
    ]);
    assert.equal(verified.status, 0, verified.stderr);
    assert.match(verified.stdout, /cached developer passphrase verified/);
  } finally {
    rmSync(parent, { recursive: true, force: true });
  }
});

test("release cache validates once and corrupted cache fails closed", () => {
  const parent = mkdtempSync(resolve(tmpdir(), "namo-release-secret-"));
  const keys = resolve(parent, "keys");
  const secret = resolve(keys, ".secrets", "release.dpapi");
  try {
    generateFirmwareKeySet({ outputDirectory: keys, passphrases });
    const registered = powershell(registerScript, [
      "-Role", "release",
      "-KeyDirectory", keys,
      "-PassphraseEnvironment", "NAMO_TEST_FIRMWARE_PASSPHRASE"
    ], passphrases.release);
    assert.equal(registered.status, 0, registered.stderr);

    const verified = powershell(buildScript, [
      "-Release",
      "-Signer", "release",
      "-KeyDirectory", keys,
      "-ValidateOnly"
    ]);
    assert.equal(verified.status, 0, verified.stderr);
    assert.match(verified.stdout, /cached release passphrase verified/);

    writeFileSync(secret, "NAMO-DPAPI-V1\nnot-base64\n");
    const corrupted = powershell(buildScript, [
      "-Release",
      "-Signer", "release",
      "-KeyDirectory", keys,
      "-ValidateOnly"
    ]);
    assert.notEqual(corrupted.status, 0);
    assert.match(corrupted.stderr, /cannot be opened/);
  } finally {
    rmSync(parent, { recursive: true, force: true });
  }
});
