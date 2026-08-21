import assert from "node:assert/strict";
import test from "node:test";

import {
  assertFirmwareTrustEpochMatches,
  firmwareBuildRequiresClean,
  firmwareSubstitutions,
  nextFirmwareSecurityEpoch,
  parseFirmwareBuildArgs,
  shouldRestoreFirmwareTrustHeaderAfterSuccess
} from "./firmware-build-options.mjs";

test("standard developer builds retain the legacy OTA policy", () => {
  assert.deepEqual(firmwareSubstitutions(), [
    "-s", "production_fusion", "true",
    "-s", "signed_ota", "false",
    "-s", "ota_policy", "legacy"
  ]);
});

test("transport smoke disables production fusion", () => {
  assert.deepEqual(
    firmwareSubstitutions({ transportSmoke: true, productionFusion: false }),
    [
      "-s", "shadow_transport_smoke", "true",
      "-s", "production_fusion", "false",
      "-s", "signed_ota", "false",
      "-s", "ota_policy", "legacy"
    ]
  );
});

test("signed release builds close the legacy web OTA path", () => {
  assert.deepEqual(firmwareSubstitutions({ signedOta: true }), [
    "-s", "production_fusion", "true",
    "-s", "signed_ota", "true",
    "-s", "ota_policy", "signed"
  ]);
});

test("transport smoke cannot carry production authority", () => {
  assert.throws(
    () => firmwareSubstitutions({ transportSmoke: true, productionFusion: true }),
    /mutually exclusive/
  );
});

test("every official release advances the anti-rollback epoch exactly once", () => {
  assert.equal(nextFirmwareSecurityEpoch(1, false), 1);
  assert.equal(nextFirmwareSecurityEpoch(1, true), 2);
  assert.equal(nextFirmwareSecurityEpoch("41", true), 42);
  assert.throws(() => nextFirmwareSecurityEpoch(0, true), /positive uint32/);
  assert.throws(() => nextFirmwareSecurityEpoch(0x100000000, true), /positive uint32/);
  assert.throws(() => nextFirmwareSecurityEpoch(0xffffffff, true), /exhausted/);
});

test("developer builds may skip clean while release builds always clean", () => {
  assert.equal(firmwareBuildRequiresClean({ signer: "developer" }), false);
  assert.equal(
    firmwareBuildRequiresClean({ signer: "developer", clean: true }),
    true
  );
  assert.equal(
    firmwareBuildRequiresClean({ signer: "release", release: true }),
    true
  );
  assert.equal(
    firmwareBuildRequiresClean({ signer: "release", release: true, clean: false }),
    true
  );
});

test("only a successful release retains the newly provisioned trust header", () => {
  assert.equal(shouldRestoreFirmwareTrustHeaderAfterSuccess(), true);
  assert.equal(
    shouldRestoreFirmwareTrustHeaderAfterSuccess({ signer: "developer" }),
    true
  );
  assert.equal(
    shouldRestoreFirmwareTrustHeaderAfterSuccess({ release: true, signer: "release" }),
    false
  );
});

test("exported release metadata requires one security epoch", () => {
  const version = { firmwareSecurityEpoch: 2 };
  assert.equal(
    assertFirmwareTrustEpochMatches(version, "SECURITY_EPOCH = 2U;"),
    2
  );
  assert.throws(
    () => assertFirmwareTrustEpochMatches(version, "SECURITY_EPOCH = 1U;"),
    /trust epoch mismatch/
  );
  assert.throws(
    () => assertFirmwareTrustEpochMatches(version, "TRUST_REVISION = 1U;"),
    /SECURITY_EPOCH is missing/
  );
});

test("build arguments keep release versioning separate from signing identity", () => {
  assert.deepEqual(
    parseFirmwareBuildArgs(["--release", "--signer", "release"]),
    {
      beta: false,
      clean: false,
      config: false,
      dashboard: false,
      help: false,
      release: true,
      signer: "release",
      transportSmoke: false,
      productionFusion: true,
      releaseType: "patch",
      firmwareEntry: "namo.yaml"
    }
  );
  assert.equal(
    parseFirmwareBuildArgs([
      "--release", "--release-type", "minor", "--signer", "release"
    ]).releaseType,
    "minor"
  );
  assert.equal(
    parseFirmwareBuildArgs([
      "--release-type", "Major", "--release", "--signer", "release"
    ]).releaseType,
    "major"
  );
  assert.throws(
    () => parseFirmwareBuildArgs(["--release"]),
    /requires the separate --signer release/
  );
  assert.throws(
    () => parseFirmwareBuildArgs(["--signer", "release"]),
    /requires --release/
  );
  assert.throws(
    () => parseFirmwareBuildArgs(["--release-type", "minor"]),
    /requires --release/
  );
  assert.throws(
    () => parseFirmwareBuildArgs([
      "--release", "--release-type", "feature", "--signer", "release"
    ]),
    /unsupported release type/
  );
});

test("developer and unsigned signing modes never advance release versioning", () => {
  const developer = parseFirmwareBuildArgs([
    "--signer", "developer", "--dashboard", "custom.yaml"
  ]);
  assert.equal(developer.release, false);
  assert.equal(developer.signer, "developer");
  assert.equal(developer.dashboard, true);
  assert.equal(developer.firmwareEntry, "custom.yaml");

  const unsigned = parseFirmwareBuildArgs(["--signer", "none"]);
  assert.equal(unsigned.release, false);
  assert.equal(unsigned.signer, "none");
});

test("legacy signed developer alias remains isolated and bounded", () => {
  assert.equal(
    parseFirmwareBuildArgs(["--signed-developer"]).signer,
    "developer"
  );
  assert.throws(
    () => parseFirmwareBuildArgs([
      "--signed-developer", "--signer", "developer"
    ]),
    /cannot be combined/
  );
  assert.throws(
    () => parseFirmwareBuildArgs(["--signer", "developer", "--beta"]),
    /cannot be combined with a signed build/
  );
  assert.throws(
    () => parseFirmwareBuildArgs(["--signer", "unknown"]),
    /unsupported signer/
  );
});
