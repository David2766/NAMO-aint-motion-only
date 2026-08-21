import {
  createHash,
  createPrivateKey,
  createPublicKey,
  sign as cryptoSign,
  timingSafeEqual,
  verify as cryptoVerify
} from "node:crypto";

export const FIRMWARE_PACKAGE = Object.freeze({
  magic: Buffer.from("NAMOOTA1", "ascii"),
  formatVersion: 1,
  signedSize: 256,
  signatureSize: 64,
  headerSize: 320,
  productId: 0x3153504e,
  hardwareId: 0x31335358,
  maxImageSize: 0x300000,
  roles: Object.freeze({ release: 1, developer: 2, rootRecovery: 3 })
});

export const SIGNED_OTA_IMAGE_MARKER = Buffer.from("NAMO_SIGNED_OTA_V1", "ascii");

const OFFSETS = Object.freeze({
  role: 12,
  algorithm: 13,
  flags: 14,
  product: 16,
  hardware: 20,
  imageSize: 24,
  trustRevision: 28,
  securityEpoch: 32,
  signerKeyId: 36,
  imageDigest: 52,
  firmwareVersion: 84,
  buildId: 108,
  revokedKeyId: 132,
  nextReleaseKeyId: 148,
  nextReleasePublicKey: 164,
  replacementKeyRole: 229,
  reserved: 230
});

const P256_SPKI_PREFIX = Buffer.from([
  0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
  0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
  0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
]);

function fail(code, message = code) {
  const error = new Error(message);
  error.code = code;
  throw error;
}

function requireUint32(value, name, { nonzero = true } = {}) {
  if (!Number.isSafeInteger(value) || value < (nonzero ? 1 : 0) || value > 0xffffffff)
    fail(`invalid_${name}`, `${name} must be a bounded uint32`);
  return value;
}

function fixedAscii(value, size, name) {
  if (typeof value !== "string" || value.length === 0)
    fail(`invalid_${name}`, `${name} is required`);
  const encoded = Buffer.from(value, "ascii");
  if (encoded.length >= size || encoded.toString("ascii") !== value ||
      [...encoded].some((byte) => byte < 0x20 || byte > 0x7e))
    fail(`invalid_${name}`, `${name} must be printable ASCII shorter than ${size} bytes`);
  const output = Buffer.alloc(size);
  encoded.copy(output);
  return output;
}

function readFixedAscii(buffer, offset, size, name) {
  const field = buffer.subarray(offset, offset + size);
  const terminator = field.indexOf(0);
  if (terminator <= 0 || field.subarray(terminator).some((byte) => byte !== 0))
    fail(`invalid_${name}`);
  const value = field.subarray(0, terminator);
  if (value.some((byte) => byte < 0x20 || byte > 0x7e))
    fail(`invalid_${name}`);
  return value.toString("ascii");
}

function exactBuffer(value, size, name) {
  const buffer = Buffer.from(value ?? []);
  if (buffer.length !== size)
    fail(`invalid_${name}`, `${name} must be exactly ${size} bytes`);
  return buffer;
}

function publicKeyObject(key) {
  const object = key?.type === "public" ? key : createPublicKey(key);
  const details = object.asymmetricKeyDetails ?? {};
  if (object.asymmetricKeyType !== "ec" || details.namedCurve !== "prime256v1")
    fail("unsupported_key", "only P-256 public keys are supported");
  return object;
}

function privateKeyObject(key, passphrase) {
  const object = key?.type === "private"
    ? key
    : createPrivateKey({ key, format: "pem", passphrase });
  const details = object.asymmetricKeyDetails ?? {};
  if (object.asymmetricKeyType !== "ec" || details.namedCurve !== "prime256v1")
    fail("unsupported_key", "only P-256 private keys are supported");
  return object;
}

export function rawP256PublicKey(key) {
  const jwk = publicKeyObject(key).export({ format: "jwk" });
  const x = Buffer.from(jwk.x, "base64url");
  const y = Buffer.from(jwk.y, "base64url");
  if (x.length !== 32 || y.length !== 32)
    fail("invalid_public_key");
  return Buffer.concat([Buffer.from([0x04]), x, y]);
}

export function p256KeyId(key) {
  const spki = publicKeyObject(key).export({ type: "spki", format: "der" });
  return createHash("sha256").update(spki).digest().subarray(0, 16);
}

function rawP256KeyId(rawKey) {
  const raw = exactBuffer(rawKey, 65, "public_key");
  return createHash("sha256").update(P256_SPKI_PREFIX).update(raw).digest().subarray(0, 16);
}

export function roleName(role) {
  if (role === FIRMWARE_PACKAGE.roles.release) return "release";
  if (role === FIRMWARE_PACKAGE.roles.developer) return "developer";
  if (role === FIRMWARE_PACKAGE.roles.rootRecovery) return "root_recovery";
  return "unknown";
}

function roleValue(role) {
  if (typeof role === "number") return role;
  if (role === "release") return FIRMWARE_PACKAGE.roles.release;
  if (role === "developer") return FIRMWARE_PACKAGE.roles.developer;
  if (role === "root_recovery" || role === "rootRecovery")
    return FIRMWARE_PACKAGE.roles.rootRecovery;
  fail("invalid_role");
}

function readCString(buffer, offset, size) {
  const end = Math.min(buffer.length, offset + size);
  let value = "";
  for (let index = offset; index < end; index += 1) {
    const byte = buffer[index];
    if (byte === 0) break;
    if (byte < 0x20 || byte > 0x7e) fail("firmware_metadata_invalid");
    value += String.fromCharCode(byte);
  }
  return value.trim();
}

export function readEspFirmwareVersion(firmware) {
  const image = Buffer.from(firmware ?? []);
  const appDescriptionOffset = 32;
  if (image.length < 192 || image[0] !== 0xe9 ||
      image.readUInt32LE(appDescriptionOffset) !== 0xabcd5432 ||
      readCString(image, appDescriptionOffset + 48, 32) !== "presence-sensor")
    fail("firmware_image_invalid");
  const prefix = Buffer.from("Project truststorm.presence-sensor version ", "ascii");
  const prefixOffset = image.indexOf(prefix);
  if (prefixOffset < 0) fail("firmware_metadata_invalid");
  const versionText = readCString(image, prefixOffset + prefix.length, 32);
  const match = /^\d+\.\d+\.\d+(?:-[A-Za-z0-9_-]+)?/.exec(versionText);
  if (!match) fail("firmware_metadata_invalid");
  return match[0];
}

export function assertSignedOtaFirmwareImage(firmware) {
  const image = Buffer.from(firmware ?? []);
  readEspFirmwareVersion(image);
  if (image.indexOf(SIGNED_OTA_IMAGE_MARKER) < 0)
    fail("signed_ota_marker_missing", "firmware image does not contain the signed OTA implementation");
  return image;
}

export function assertFirmwareEmbedsPublicKeys(firmware, publicKeys) {
  const image = Buffer.from(firmware ?? []);
  if (!Array.isArray(publicKeys) || publicKeys.length === 0)
    fail("embedded_public_keys_required");
  for (const key of publicKeys) {
    if (image.indexOf(rawP256PublicKey(key)) < 0)
      fail("embedded_public_key_missing", "firmware image does not contain a required public key");
  }
  return image;
}

export function assertFirmwareSigningIdentity({
  role,
  privateKey,
  rootPublicKey,
  releasePublicKey,
  developerPublicKey
}) {
  const signerRole = roleValue(role);
  const signerPublic = createPublicKey(privateKeyObject(privateKey));
  const root = publicKeyObject(rootPublicKey);
  const release = publicKeyObject(releasePublicKey);
  const developer = publicKeyObject(developerPublicKey);
  const publicKeys = [root, release, developer];
  const keyIds = publicKeys.map(p256KeyId);
  if (keyIds.some((left, index) => keyIds.some((right, other) =>
    index !== other && timingSafeEqual(left, right)))) {
    fail("duplicate_trust_key", "root, release, and developer keys must be distinct");
  }
  const expectedSigner = signerRole === FIRMWARE_PACKAGE.roles.rootRecovery
    ? root
    : signerRole === FIRMWARE_PACKAGE.roles.release
      ? release
      : developer;
  if (!timingSafeEqual(rawP256PublicKey(signerPublic), rawP256PublicKey(expectedSigner))) {
    fail("signer_role_mismatch", "private signing key does not match its declared trust role");
  }
  return publicKeys;
}

export function createFirmwarePackage({
  firmware,
  privateKey,
  passphrase,
  role = "release",
  firmwareVersion,
  buildId,
  trustRevision,
  securityEpoch,
  revokedKeyId,
  replacementPublicKey,
  replacementRole
}) {
  const image = Buffer.from(firmware ?? []);
  if (image.length === 0 || image.length > FIRMWARE_PACKAGE.maxImageSize)
    fail("invalid_image_size");
  const signer = privateKeyObject(privateKey, passphrase);
  const signerPublic = createPublicKey(signer);
  const signerRole = roleValue(role);
  const header = Buffer.alloc(FIRMWARE_PACKAGE.headerSize);
  FIRMWARE_PACKAGE.magic.copy(header, 0);
  header.writeUInt16LE(FIRMWARE_PACKAGE.formatVersion, 8);
  header.writeUInt16LE(FIRMWARE_PACKAGE.headerSize, 10);
  header[OFFSETS.role] = signerRole;
  header[OFFSETS.algorithm] = 1;
  header.writeUInt32LE(FIRMWARE_PACKAGE.productId, OFFSETS.product);
  header.writeUInt32LE(FIRMWARE_PACKAGE.hardwareId, OFFSETS.hardware);
  header.writeUInt32LE(image.length, OFFSETS.imageSize);
  header.writeUInt32LE(requireUint32(trustRevision, "trust_revision"), OFFSETS.trustRevision);
  header.writeUInt32LE(requireUint32(securityEpoch, "security_epoch"), OFFSETS.securityEpoch);
  p256KeyId(signerPublic).copy(header, OFFSETS.signerKeyId);
  createHash("sha256").update(image).digest().copy(header, OFFSETS.imageDigest);
  fixedAscii(firmwareVersion, 24, "firmware_version").copy(header, OFFSETS.firmwareVersion);
  fixedAscii(buildId, 24, "build_id").copy(header, OFFSETS.buildId);

  if (signerRole === FIRMWARE_PACKAGE.roles.rootRecovery) {
    exactBuffer(revokedKeyId, 16, "revoked_key_id").copy(header, OFFSETS.revokedKeyId);
    const nextPublic = publicKeyObject(replacementPublicKey);
    p256KeyId(nextPublic).copy(header, OFFSETS.nextReleaseKeyId);
    rawP256PublicKey(nextPublic).copy(header, OFFSETS.nextReleasePublicKey);
    const nextRole = roleValue(replacementRole);
    if (![FIRMWARE_PACKAGE.roles.release, FIRMWARE_PACKAGE.roles.developer].includes(nextRole))
      fail("invalid_recovery", "replacement role must be release or developer");
    header[OFFSETS.replacementKeyRole] = nextRole;
    if (timingSafeEqual(
      header.subarray(OFFSETS.revokedKeyId, OFFSETS.revokedKeyId + 16),
      header.subarray(OFFSETS.nextReleaseKeyId, OFFSETS.nextReleaseKeyId + 16)
    )) fail("invalid_recovery", "replacement key cannot have the revoked key id");
  } else if (revokedKeyId != null || replacementPublicKey != null || replacementRole != null) {
    fail("invalid_recovery", "recovery fields require a root recovery package");
  }

  const signed = header.subarray(0, FIRMWARE_PACKAGE.signedSize);
  const signature = cryptoSign("sha256", signed, {
    key: signer,
    dsaEncoding: "ieee-p1363"
  });
  if (signature.length !== FIRMWARE_PACKAGE.signatureSize)
    fail("invalid_signature_size");
  signature.copy(header, FIRMWARE_PACKAGE.signedSize);
  return Buffer.concat([header, image]);
}

export function parseFirmwarePackage(packageBytes) {
  const data = Buffer.from(packageBytes ?? []);
  if (data.length < FIRMWARE_PACKAGE.headerSize)
    fail("truncated_header");
  if (!data.subarray(0, 8).equals(FIRMWARE_PACKAGE.magic))
    fail("invalid_magic");
  if (data.readUInt16LE(8) !== FIRMWARE_PACKAGE.formatVersion ||
      data.readUInt16LE(10) !== FIRMWARE_PACKAGE.headerSize)
    fail("unsupported_format");
  const role = data[OFFSETS.role];
  if (![1, 2, 3].includes(role)) fail("invalid_role");
  if (data[OFFSETS.algorithm] !== 1) fail("unsupported_algorithm");
  if (data.readUInt16LE(OFFSETS.flags) !== 0 ||
      data.subarray(OFFSETS.reserved, FIRMWARE_PACKAGE.signedSize).some((byte) => byte !== 0))
    fail("invalid_reserved");
  if (data.readUInt32LE(OFFSETS.product) !== FIRMWARE_PACKAGE.productId)
    fail("invalid_product");
  if (data.readUInt32LE(OFFSETS.hardware) !== FIRMWARE_PACKAGE.hardwareId)
    fail("invalid_hardware");
  const imageSize = data.readUInt32LE(OFFSETS.imageSize);
  if (imageSize === 0 || imageSize > FIRMWARE_PACKAGE.maxImageSize ||
      data.length !== FIRMWARE_PACKAGE.headerSize + imageSize)
    fail("invalid_image_size");
  const trustRevision = requireUint32(data.readUInt32LE(OFFSETS.trustRevision), "trust_revision");
  const securityEpoch = requireUint32(data.readUInt32LE(OFFSETS.securityEpoch), "security_epoch");
  const signerKeyId = Buffer.from(data.subarray(OFFSETS.signerKeyId, OFFSETS.signerKeyId + 16));
  const imageDigest = Buffer.from(data.subarray(OFFSETS.imageDigest, OFFSETS.imageDigest + 32));
  if (signerKeyId.every((byte) => byte === 0) || imageDigest.every((byte) => byte === 0))
    fail("invalid_identity");
  const firmwareVersion = readFixedAscii(data, OFFSETS.firmwareVersion, 24, "firmware_version");
  const buildId = readFixedAscii(data, OFFSETS.buildId, 24, "build_id");
  const revokedKeyId = Buffer.from(data.subarray(OFFSETS.revokedKeyId, OFFSETS.revokedKeyId + 16));
  const nextReleaseKeyId = Buffer.from(data.subarray(OFFSETS.nextReleaseKeyId, OFFSETS.nextReleaseKeyId + 16));
  const nextReleasePublicKey = Buffer.from(data.subarray(OFFSETS.nextReleasePublicKey, OFFSETS.nextReleasePublicKey + 65));
  const replacementKeyRole = data[OFFSETS.replacementKeyRole];
  const anyRecovery = !revokedKeyId.every((byte) => byte === 0) ||
    !nextReleaseKeyId.every((byte) => byte === 0) ||
    !nextReleasePublicKey.every((byte) => byte === 0) || replacementKeyRole !== 0;
  const completeRecovery = !revokedKeyId.every((byte) => byte === 0) &&
    !nextReleaseKeyId.every((byte) => byte === 0) && nextReleasePublicKey[0] === 0x04 &&
    nextReleasePublicKey.subarray(1).some((byte) => byte !== 0) &&
    timingSafeEqual(nextReleaseKeyId, rawP256KeyId(nextReleasePublicKey)) &&
    [FIRMWARE_PACKAGE.roles.release, FIRMWARE_PACKAGE.roles.developer].includes(replacementKeyRole);
  if ((role === 3 && !completeRecovery) || (role !== 3 && anyRecovery) ||
      (role === 3 && timingSafeEqual(revokedKeyId, nextReleaseKeyId)))
    fail("invalid_recovery");
  return {
    role,
    roleName: roleName(role),
    imageSize,
    trustRevision,
    securityEpoch,
    signerKeyId,
    imageDigest,
    firmwareVersion,
    buildId,
    revokedKeyId,
    nextReleaseKeyId,
    nextReleasePublicKey,
    replacementKeyRole,
    signedBytes: Buffer.from(data.subarray(0, FIRMWARE_PACKAGE.signedSize)),
    signature: Buffer.from(data.subarray(FIRMWARE_PACKAGE.signedSize, FIRMWARE_PACKAGE.headerSize)),
    image: Buffer.from(data.subarray(FIRMWARE_PACKAGE.headerSize))
  };
}

export function verifyFirmwarePackage(packageBytes, publicKey) {
  const parsed = parseFirmwarePackage(packageBytes);
  const key = publicKeyObject(publicKey);
  if (!timingSafeEqual(parsed.signerKeyId, p256KeyId(key)))
    fail("signer_key_mismatch");
  if (!cryptoVerify("sha256", parsed.signedBytes, {
    key,
    dsaEncoding: "ieee-p1363"
  }, parsed.signature)) fail("invalid_signature");
  const actualDigest = createHash("sha256").update(parsed.image).digest();
  if (!timingSafeEqual(actualDigest, parsed.imageDigest))
    fail("image_digest_mismatch");
  return parsed;
}

export function loadPrivateKey(pem, passphrase) {
  return privateKeyObject(pem, passphrase);
}

export function loadPublicKey(pem) {
  return publicKeyObject(pem);
}
