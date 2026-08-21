import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { generateKeyPairSync } from "node:crypto";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

import {
  FIRMWARE_PACKAGE,
  assertFirmwareEmbedsPublicKeys,
  assertFirmwareSigningIdentity,
  assertSignedOtaFirmwareImage,
  createFirmwarePackage,
  p256KeyId,
  readEspFirmwareVersion,
  rawP256PublicKey,
  verifyFirmwarePackage
} from "./firmware-signing.mjs";

function keys() {
  return generateKeyPairSync("ec", { namedCurve: "prime256v1" });
}

function releaseFixture() {
  const signer = keys();
  const firmware = Buffer.from(Array.from({ length: 257 }, (_, index) => index & 0xff));
  const packageBytes = createFirmwarePackage({
    firmware,
    privateKey: signer.privateKey,
    role: "release",
    firmwareVersion: "0.6.0",
    buildId: "20260806T120000Z",
    trustRevision: 4,
    securityEpoch: 9
  });
  return { signer, firmware, packageBytes };
}

function espFirmware(version = "0.6.0") {
  const image = Buffer.alloc(320);
  image[0] = 0xe9;
  image.writeUInt32LE(0xabcd5432, 32);
  image.write("presence-sensor", 80, "ascii");
  image.write(`Project truststorm.presence-sensor version ${version}`, 176, "ascii");
  image.write("NAMO_SIGNED_OTA_V1", 240, "ascii");
  return image;
}

test("release tooling reads the exact NAMO version embedded in the ESP image", () => {
  assert.equal(readEspFirmwareVersion(espFirmware("0.6.0-test")), "0.6.0-test");
  const wrongProduct = espFirmware();
  wrongProduct.write("other-product", 80, "ascii");
  assert.throws(() => readEspFirmwareVersion(wrongProduct), /firmware_image_invalid/);
  assert.throws(() => readEspFirmwareVersion(Buffer.alloc(320)), /firmware_image_invalid/);
  const unsignedOtaImage = espFirmware();
  unsignedOtaImage.fill(0, 240, 258);
  assert.throws(() => assertSignedOtaFirmwareImage(unsignedOtaImage), /signed OTA implementation/);
  assert.equal(assertSignedOtaFirmwareImage(espFirmware()).length, 320);

  const embedded = keys();
  const imageWithKey = Buffer.concat([espFirmware(), rawP256PublicKey(embedded.publicKey)]);
  assert.equal(assertFirmwareEmbedsPublicKeys(imageWithKey, [embedded.publicKey]).length, imageWithKey.length);
  assert.throws(() => assertFirmwareEmbedsPublicKeys(espFirmware(), [embedded.publicKey]), /required public key/);
});

test("release package is deterministic in shape and verifies exact metadata", () => {
  const { signer, firmware, packageBytes } = releaseFixture();
  const parsed = verifyFirmwarePackage(packageBytes, signer.publicKey);
  assert.equal(packageBytes.length, FIRMWARE_PACKAGE.headerSize + firmware.length);
  assert.equal(parsed.roleName, "release");
  assert.equal(parsed.firmwareVersion, "0.6.0");
  assert.equal(parsed.buildId, "20260806T120000Z");
  assert.equal(parsed.trustRevision, 4);
  assert.equal(parsed.securityEpoch, 9);
  assert.deepEqual(parsed.signerKeyId, p256KeyId(signer.publicKey));
  assert.deepEqual(parsed.image, firmware);
  assert.equal(packageBytes.includes(Buffer.from("PRIVATE KEY")), false);
});

test("every signed header and signature byte is protected", () => {
  const { signer, packageBytes } = releaseFixture();
  for (let index = 0; index < FIRMWARE_PACKAGE.headerSize; index += 1) {
    const changed = Buffer.from(packageBytes);
    changed[index] ^= 1;
    assert.throws(
      () => verifyFirmwarePackage(changed, signer.publicKey),
      undefined,
      `mutation at package byte ${index} must fail`
    );
  }
});

test("every firmware byte is protected by the signed digest", () => {
  const { signer, packageBytes } = releaseFixture();
  for (let index = FIRMWARE_PACKAGE.headerSize; index < packageBytes.length; index += 1) {
    const changed = Buffer.from(packageBytes);
    changed[index] ^= 1;
    assert.throws(
      () => verifyFirmwarePackage(changed, signer.publicKey),
      /image_digest_mismatch/,
      `mutation at image byte ${index - FIRMWARE_PACKAGE.headerSize} must fail`
    );
  }
});

test("wrong signer and malformed or truncated package fail closed", () => {
  const { packageBytes } = releaseFixture();
  const wrong = keys();
  assert.throws(() => verifyFirmwarePackage(packageBytes, wrong.publicKey), /signer_key_mismatch/);
  assert.throws(() => verifyFirmwarePackage(packageBytes.subarray(0, 319), wrong.publicKey), /truncated_header/);
  assert.throws(() => verifyFirmwarePackage(packageBytes.subarray(0, -1), wrong.publicKey), /invalid_image_size/);
  assert.throws(() => verifyFirmwarePackage(Buffer.concat([packageBytes, Buffer.from([0])]), wrong.publicKey), /invalid_image_size/);
  assert.throws(() => createFirmwarePackage({ firmware: Buffer.alloc(0), privateKey: wrong.privateKey }), /invalid_image_size/);
});

test("package creation enforces every bounded release field", () => {
  const signer = keys();
  const base = {
    firmware: Buffer.from([1]),
    privateKey: signer.privateKey,
    role: "release",
    firmwareVersion: "0.6.0",
    buildId: "bounded-build",
    trustRevision: 1,
    securityEpoch: 1
  };
  assert.throws(
    () => createFirmwarePackage({ ...base, firmware: Buffer.alloc(FIRMWARE_PACKAGE.maxImageSize + 1) }),
    /invalid_image_size/
  );
  assert.throws(() => createFirmwarePackage({ ...base, trustRevision: 0 }), /trust_revision/);
  assert.throws(() => createFirmwarePackage({ ...base, securityEpoch: 0 }), /security_epoch/);
  assert.throws(() => createFirmwarePackage({ ...base, trustRevision: 0x100000000 }), /trust_revision/);
  assert.throws(() => createFirmwarePackage({ ...base, firmwareVersion: "x".repeat(24) }), /firmware_version/);
  assert.throws(() => createFirmwarePackage({ ...base, buildId: "bad\nbuild" }), /build_id/);
  assert.throws(() => createFirmwarePackage({ ...base, role: "operator" }), /invalid_role/);

  const maximum = createFirmwarePackage({
    ...base,
    firmware: Buffer.alloc(FIRMWARE_PACKAGE.maxImageSize, 0xa5),
    trustRevision: 0xffffffff,
    securityEpoch: 0xffffffff
  });
  const parsed = verifyFirmwarePackage(maximum, signer.publicKey);
  assert.equal(parsed.image.length, FIRMWARE_PACKAGE.maxImageSize);
  assert.equal(parsed.trustRevision, 0xffffffff);
  assert.equal(parsed.securityEpoch, 0xffffffff);
});

test("root recovery revokes one release key and installs a distinct replacement", () => {
  const root = keys();
  const oldRelease = keys();
  const nextRelease = keys();
  const packageBytes = createFirmwarePackage({
    firmware: Buffer.from([1, 2, 3, 4]),
    privateKey: root.privateKey,
    role: "root_recovery",
    firmwareVersion: "0.6.1",
    buildId: "recovery-20260806",
    trustRevision: 5,
    securityEpoch: 10,
    revokedKeyId: p256KeyId(oldRelease.publicKey),
    replacementPublicKey: nextRelease.publicKey,
    replacementRole: "release"
  });
  const parsed = verifyFirmwarePackage(packageBytes, root.publicKey);
  assert.equal(parsed.roleName, "root_recovery");
  assert.deepEqual(parsed.revokedKeyId, p256KeyId(oldRelease.publicKey));
  assert.deepEqual(parsed.nextReleaseKeyId, p256KeyId(nextRelease.publicKey));
  assert.deepEqual(parsed.nextReleasePublicKey, rawP256PublicKey(nextRelease.publicKey));
  assert.throws(() => createFirmwarePackage({
    firmware: Buffer.from([1]),
    privateKey: root.privateKey,
    role: "root_recovery",
    firmwareVersion: "0.6.1",
    buildId: "recovery",
    trustRevision: 5,
    securityEpoch: 10,
    revokedKeyId: p256KeyId(nextRelease.publicKey),
    replacementPublicKey: nextRelease.publicKey,
    replacementRole: "release"
  }), /replacement key cannot have the revoked key id/);
});

test("root recovery can independently rotate a leaked developer key", () => {
  const root = keys();
  const oldDeveloper = keys();
  const nextDeveloper = keys();
  const packageBytes = createFirmwarePackage({
    firmware: Buffer.from([5, 6, 7]),
    privateKey: root.privateKey,
    role: "root_recovery",
    firmwareVersion: "0.6.2",
    buildId: "developer-recovery",
    trustRevision: 6,
    securityEpoch: 11,
    revokedKeyId: p256KeyId(oldDeveloper.publicKey),
    replacementPublicKey: nextDeveloper.publicKey,
    replacementRole: "developer"
  });
  const parsed = verifyFirmwarePackage(packageBytes, root.publicKey);
  assert.equal(parsed.replacementKeyRole, FIRMWARE_PACKAGE.roles.developer);
  assert.deepEqual(parsed.nextReleaseKeyId, p256KeyId(nextDeveloper.publicKey));
});

test("developer package is cryptographically distinct from release package", () => {
  const developer = keys();
  const packageBytes = createFirmwarePackage({
    firmware: Buffer.from([9, 8, 7]),
    privateKey: developer.privateKey,
    role: "developer",
    firmwareVersion: "0.6.0-dev",
    buildId: "local-build",
    trustRevision: 4,
    securityEpoch: 9
  });
  assert.equal(verifyFirmwarePackage(packageBytes, developer.publicKey).roleName, "developer");
});

test("manual signing requires distinct embedded trust keys and the declared signer", () => {
  const root = keys();
  const release = keys();
  const developer = keys();
  const trustKeys = assertFirmwareSigningIdentity({
    role: "developer",
    privateKey: developer.privateKey,
    rootPublicKey: root.publicKey,
    releasePublicKey: release.publicKey,
    developerPublicKey: developer.publicKey
  });
  assert.deepEqual(trustKeys.map((key) => rawP256PublicKey(key)), [
    rawP256PublicKey(root.publicKey),
    rawP256PublicKey(release.publicKey),
    rawP256PublicKey(developer.publicKey)
  ]);
  assert.throws(() => assertFirmwareSigningIdentity({
    role: "release",
    privateKey: developer.privateKey,
    rootPublicKey: root.publicKey,
    releasePublicKey: release.publicKey,
    developerPublicKey: developer.publicKey
  }), /declared trust role/);
  assert.throws(() => assertFirmwareSigningIdentity({
    role: "developer",
    privateKey: developer.privateKey,
    rootPublicKey: root.publicKey,
    releasePublicKey: root.publicKey,
    developerPublicKey: developer.publicKey
  }), /must be distinct/);
});

test("manual signing CLI produces Developer and Root recovery packages fail closed", () => {
  const directory = mkdtempSync(join(tmpdir(), "namo-sign-cli-"));
  const projectRoot = resolve(import.meta.dirname, "..");
  const passphrase = "test-only-signing-passphrase";
  const root = keys();
  const release = keys();
  const developer = keys();
  const replacement = keys();
  const paths = {};
  try {
    for (const [name, pair] of Object.entries({ root, release, developer, replacement })) {
      paths[`${name}Public`] = join(directory, `${name}.public.pem`);
      paths[`${name}Private`] = join(directory, `${name}.private.pem`);
      writeFileSync(paths[`${name}Public`], pair.publicKey.export({ type: "spki", format: "pem" }));
      writeFileSync(paths[`${name}Private`], pair.privateKey.export({
        type: "pkcs8",
        format: "pem",
        cipher: "aes-256-cbc",
        passphrase
      }));
    }
    const developerImage = Buffer.concat([
      espFirmware(),
      rawP256PublicKey(root.publicKey),
      rawP256PublicKey(release.publicKey),
      rawP256PublicKey(developer.publicKey)
    ]);
    const developerImagePath = join(directory, "developer.bin");
    const developerOutputPath = join(directory, "developer.namo-ota");
    writeFileSync(developerImagePath, developerImage);
    const common = [
      "scripts/sign-firmware.mjs",
      "--firmware", developerImagePath,
      "--root-public", paths.rootPublic,
      "--release-public", paths.releasePublic,
      "--developer-public", paths.developerPublic,
      "--version", "0.6.0",
      "--build", "cli-test",
      "--trust-revision", "4",
      "--security-epoch", "9"
    ];
    const developerResult = spawnSync(process.execPath, [
      ...common,
      "--role", "developer",
      "--private-key", paths.developerPrivate,
      "--out", developerOutputPath
    ], {
      cwd: projectRoot,
      encoding: "utf8",
      env: { ...process.env, NAMO_SIGNING_KEY_PASSPHRASE: passphrase }
    });
    assert.equal(developerResult.status, 0, developerResult.stderr);
    assert.equal(
      verifyFirmwarePackage(readFileSync(developerOutputPath), developer.publicKey).roleName,
      "developer"
    );

    const wrongRoleOutput = join(directory, "wrong-role.namo-ota");
    const wrongRoleResult = spawnSync(process.execPath, [
      ...common,
      "--role", "release",
      "--private-key", paths.developerPrivate,
      "--out", wrongRoleOutput
    ], {
      cwd: projectRoot,
      encoding: "utf8",
      env: { ...process.env, NAMO_SIGNING_KEY_PASSPHRASE: passphrase }
    });
    assert.notEqual(wrongRoleResult.status, 0);
    assert.match(wrongRoleResult.stderr, /declared trust role/);

    const recoveryImage = Buffer.concat([
      espFirmware("0.6.1"),
      rawP256PublicKey(root.publicKey),
      rawP256PublicKey(replacement.publicKey),
      rawP256PublicKey(developer.publicKey)
    ]);
    const recoveryImagePath = join(directory, "recovery.bin");
    const recoveryOutputPath = join(directory, "recovery.namo-ota");
    writeFileSync(recoveryImagePath, recoveryImage);
    const recoveryResult = spawnSync(process.execPath, [
      "scripts/sign-firmware.mjs",
      "--role", "root_recovery",
      "--firmware", recoveryImagePath,
      "--private-key", paths.rootPrivate,
      "--root-public", paths.rootPublic,
      "--release-public", paths.replacementPublic,
      "--developer-public", paths.developerPublic,
      "--version", "0.6.1",
      "--build", "recovery-cli-test",
      "--trust-revision", "5",
      "--security-epoch", "1",
      "--revoked-public", paths.releasePublic,
      "--replacement-public", paths.replacementPublic,
      "--replacement-role", "release",
      "--out", recoveryOutputPath
    ], {
      cwd: projectRoot,
      encoding: "utf8",
      env: { ...process.env, NAMO_SIGNING_KEY_PASSPHRASE: passphrase }
    });
    assert.equal(recoveryResult.status, 0, recoveryResult.stderr);
    const recovered = verifyFirmwarePackage(readFileSync(recoveryOutputPath), root.publicKey);
    assert.equal(recovered.roleName, "root_recovery");
    assert.equal(recovered.securityEpoch, 1);
    assert.deepEqual(recovered.nextReleaseKeyId, p256KeyId(replacement.publicKey));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
