#include "firmware_update_security.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t PACKAGE_MAGIC[8] = {'N', 'A', 'M', 'O', 'O', 'T', 'A', '1'};
constexpr uint8_t STORE_MAGIC[8] = {'N', 'A', 'M', 'O', 'T', 'R', 'S', '1'};
constexpr uint8_t COMMIT_MAGIC[8] = {'N', 'A', 'M', 'O', 'T', 'R', 'C', '1'};
constexpr uint16_t STORE_VERSION = 1;

uint16_t read_u16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(data[1]) << 8U;
}

uint32_t read_u32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         static_cast<uint32_t>(data[1]) << 8U |
         static_cast<uint32_t>(data[2]) << 16U |
         static_cast<uint32_t>(data[3]) << 24U;
}

void write_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t checksum(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

bool all_zero(const uint8_t *data, std::size_t size) {
  uint8_t value = 0;
  for (std::size_t index = 0; index < size; ++index)
    value |= data[index];
  return value == 0;
}

bool valid_text(const char *data, std::size_t size) {
  bool terminated = false;
  bool nonempty = false;
  for (std::size_t index = 0; index < size; ++index) {
    const uint8_t value = static_cast<uint8_t>(data[index]);
    if (value == 0) {
      terminated = true;
      continue;
    }
    if (terminated || value < 0x20 || value > 0x7e)
      return false;
    nonempty = true;
  }
  return terminated && nonempty;
}

bool valid_public_key(
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE> &key) {
  return key[0] == 0x04 && !all_zero(key.data() + 1, key.size() - 1);
}

bool key_id_equal(
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE> &left,
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE> &right) {
  uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

bool revoked(const FirmwareTrustState &state,
             const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
                 &key_id) {
  for (const auto &candidate : state.revoked_key_ids) {
    if (!all_zero(candidate.data(), candidate.size()) &&
        key_id_equal(candidate, key_id))
      return true;
  }
  return false;
}

const FirmwareTrustKey *find_key(const FirmwareTrustState &state,
                                 FirmwareSignerRole role,
                                 const std::array<uint8_t, 16> &key_id) {
  const auto *keys = role == FirmwareSignerRole::RELEASE
                         ? state.release_keys.data()
                         : state.developer_keys.data();
  const std::size_t count = role == FirmwareSignerRole::RELEASE
                                ? state.release_keys.size()
                                : state.developer_keys.size();
  for (std::size_t index = 0; index < count; ++index) {
    if (keys[index].occupied && keys[index].role == role &&
        key_id_equal(keys[index].key_id, key_id))
      return &keys[index];
  }
  return nullptr;
}

bool append_revoked(FirmwareTrustState *state,
                    const std::array<uint8_t, 16> &key_id) {
  if (all_zero(key_id.data(), key_id.size()))
    return true;
  for (auto &candidate : state->revoked_key_ids) {
    if (key_id_equal(candidate, key_id))
      return true;
    if (all_zero(candidate.data(), candidate.size())) {
      candidate = key_id;
      return true;
    }
  }
  return false;
}

void remove_key(FirmwareTrustState *state,
                const std::array<uint8_t, 16> &key_id) {
  for (auto &key : state->release_keys) {
    if (key.occupied && key_id_equal(key.key_id, key_id))
      key = {};
  }
  for (auto &key : state->developer_keys) {
    if (key.occupied && key_id_equal(key.key_id, key_id))
      key = {};
  }
}

bool install_key(
    FirmwareTrustState *state,
    FirmwareSignerRole role,
    const std::array<uint8_t, 16> &key_id,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &public_key) {
  if (all_zero(key_id.data(), key_id.size()) ||
      !valid_public_key(public_key) || revoked(*state, key_id))
    return false;
  FirmwareTrustKey *keys = role == FirmwareSignerRole::RELEASE
                               ? state->release_keys.data()
                               : state->developer_keys.data();
  const std::size_t key_count = role == FirmwareSignerRole::RELEASE
                                    ? state->release_keys.size()
                                    : state->developer_keys.size();
  for (std::size_t index = 0; index < key_count; ++index) {
    auto &key = keys[index];
    if (key.occupied && key_id_equal(key.key_id, key_id)) {
      key.public_key = public_key;
      return true;
    }
  }
  for (std::size_t index = 0; index < key_count; ++index) {
    auto &key = keys[index];
    if (!key.occupied) {
      key.occupied = true;
      key.role = role;
      key.key_id = key_id;
      key.public_key = public_key;
      return true;
    }
  }
  return false;
}

bool valid_state(const FirmwareTrustState &state) {
  if (state.trust_revision == 0 || state.minimum_security_epoch == 0)
    return false;
  std::size_t release_count = 0;
  std::array<std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>,
             FirmwareTrustState::MAX_RELEASE_KEYS +
                 FirmwareTrustState::MAX_DEVELOPER_KEYS>
      occupied_ids{};
  std::size_t occupied_count = 0;
  const auto remember_unique = [&](const FirmwareTrustKey &key) {
    for (std::size_t index = 0; index < occupied_count; ++index) {
      if (key_id_equal(occupied_ids[index], key.key_id))
        return false;
    }
    occupied_ids[occupied_count++] = key.key_id;
    return true;
  };
  for (const auto &key : state.release_keys) {
    if (!key.occupied)
      continue;
    if (key.role != FirmwareSignerRole::RELEASE ||
        all_zero(key.key_id.data(), key.key_id.size()) ||
        !valid_public_key(key.public_key) || revoked(state, key.key_id) ||
        !remember_unique(key))
      return false;
    ++release_count;
  }
  for (const auto &key : state.developer_keys) {
    if (!key.occupied)
      continue;
    if (key.role != FirmwareSignerRole::DEVELOPER ||
        all_zero(key.key_id.data(), key.key_id.size()) ||
        !valid_public_key(key.public_key) || revoked(state, key.key_id) ||
        !remember_unique(key))
      return false;
  }
  for (std::size_t left = 0; left < state.revoked_key_ids.size(); ++left) {
    if (all_zero(state.revoked_key_ids[left].data(),
                 state.revoked_key_ids[left].size()))
      continue;
    for (std::size_t right = left + 1;
         right < state.revoked_key_ids.size(); ++right) {
      if (key_id_equal(state.revoked_key_ids[left],
                       state.revoked_key_ids[right]))
        return false;
    }
  }
  return release_count > 0;
}

constexpr std::size_t STATE_ENCODED_SIZE =
    FirmwareTrustStorageBlob::ENCODED_SIZE;

bool encode_state(const FirmwareTrustState &state, uint32_t generation,
                  FirmwareTrustStorageBlob *blob) {
  if (blob == nullptr || generation == 0 || !valid_state(state) ||
      STATE_ENCODED_SIZE > blob->bytes.size())
    return false;
  blob->bytes.fill(0);
  auto *data = blob->bytes.data();
  std::memcpy(data, STORE_MAGIC, sizeof(STORE_MAGIC));
  write_u16(data + 8, STORE_VERSION);
  write_u16(data + 10, static_cast<uint16_t>(STATE_ENCODED_SIZE));
  write_u32(data + 12, generation);
  write_u32(data + 16, state.trust_revision);
  write_u32(data + 20, state.minimum_security_epoch);
  std::size_t offset = 24;
  auto encode_key = [&](const FirmwareTrustKey &key) {
    data[offset++] = key.occupied ? 1 : 0;
    data[offset++] = static_cast<uint8_t>(key.role);
    offset += 2;
    std::memcpy(data + offset, key.key_id.data(), key.key_id.size());
    offset += key.key_id.size();
    std::memcpy(data + offset, key.public_key.data(), key.public_key.size());
    offset += key.public_key.size();
  };
  for (const auto &key : state.release_keys)
    encode_key(key);
  for (const auto &key : state.developer_keys)
    encode_key(key);
  for (const auto &key_id : state.revoked_key_ids) {
    std::memcpy(data + offset, key_id.data(), key_id.size());
    offset += key_id.size();
  }
  write_u32(data + offset, checksum(data, offset));
  return offset + 4 == STATE_ENCODED_SIZE;
}

FirmwareTrustStoreResult decode_state(const FirmwareTrustStorageBlob &blob,
                                      FirmwareTrustSnapshot *snapshot) {
  if (snapshot == nullptr)
    return FirmwareTrustStoreResult::INVALID_STATE;
  const auto *data = blob.bytes.data();
  if (std::memcmp(data, STORE_MAGIC, sizeof(STORE_MAGIC)) != 0)
    return all_zero(data, blob.bytes.size()) ? FirmwareTrustStoreResult::EMPTY
                                             : FirmwareTrustStoreResult::CORRUPT;
  if (read_u16(data + 8) != STORE_VERSION)
    return FirmwareTrustStoreResult::UNSUPPORTED_VERSION;
  const std::size_t encoded_size = read_u16(data + 10);
  if (encoded_size != STATE_ENCODED_SIZE || encoded_size > blob.bytes.size() ||
      checksum(data, encoded_size - 4) != read_u32(data + encoded_size - 4))
    return FirmwareTrustStoreResult::CORRUPT;
  FirmwareTrustSnapshot decoded{};
  decoded.generation = read_u32(data + 12);
  decoded.state.trust_revision = read_u32(data + 16);
  decoded.state.minimum_security_epoch = read_u32(data + 20);
  std::size_t offset = 24;
  auto decode_key = [&](FirmwareTrustKey *key) {
    key->occupied = data[offset++] == 1;
    key->role = static_cast<FirmwareSignerRole>(data[offset++]);
    offset += 2;
    std::memcpy(key->key_id.data(), data + offset, key->key_id.size());
    offset += key->key_id.size();
    std::memcpy(key->public_key.data(), data + offset,
                key->public_key.size());
    offset += key->public_key.size();
  };
  for (auto &key : decoded.state.release_keys)
    decode_key(&key);
  for (auto &key : decoded.state.developer_keys)
    decode_key(&key);
  for (auto &key_id : decoded.state.revoked_key_ids) {
    std::memcpy(key_id.data(), data + offset, key_id.size());
    offset += key_id.size();
  }
  if (decoded.generation == 0 || !valid_state(decoded.state))
    return FirmwareTrustStoreResult::CORRUPT;
  *snapshot = decoded;
  return FirmwareTrustStoreResult::LOADED;
}

void encode_commit(uint8_t slot, uint32_t generation, uint32_t trust_revision,
                   FirmwareTrustCommitBlob *blob) {
  blob->bytes.fill(0);
  std::memcpy(blob->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  blob->bytes[8] = slot;
  write_u32(blob->bytes.data() + 12, generation);
  write_u32(blob->bytes.data() + 16, trust_revision);
  write_u32(blob->bytes.data() + 20,
            checksum(blob->bytes.data(), blob->bytes.size() - 4));
}

bool decode_commit(const FirmwareTrustCommitBlob &blob, uint8_t *slot,
                   uint32_t *generation, uint32_t *trust_revision) {
  if (slot == nullptr || generation == nullptr || trust_revision == nullptr ||
      std::memcmp(blob.bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0 ||
      blob.bytes[8] > 1 ||
      checksum(blob.bytes.data(), blob.bytes.size() - 4) !=
          read_u32(blob.bytes.data() + blob.bytes.size() - 4))
    return false;
  *slot = blob.bytes[8];
  *generation = read_u32(blob.bytes.data() + 12);
  *trust_revision = read_u32(blob.bytes.data() + 16);
  return *generation != 0 && *trust_revision != 0;
}

}  // namespace

FirmwarePackageResult parse_firmware_package_header(
    const uint8_t *data, std::size_t size, FirmwarePackageHeader *output) {
  if (data == nullptr || output == nullptr ||
      size != FirmwarePackageHeader::HEADER_SIZE)
    return FirmwarePackageResult::INVALID_ARGUMENT;
  if (std::memcmp(data, PACKAGE_MAGIC, sizeof(PACKAGE_MAGIC)) != 0)
    return FirmwarePackageResult::INVALID_MAGIC;
  if (read_u16(data + 8) != FirmwarePackageHeader::FORMAT_VERSION ||
      read_u16(data + 10) != FirmwarePackageHeader::HEADER_SIZE)
    return FirmwarePackageResult::UNSUPPORTED_FORMAT;

  FirmwarePackageHeader parsed{};
  parsed.manifest.signer_role = static_cast<FirmwareSignerRole>(data[12]);
  parsed.manifest.signature_algorithm =
      static_cast<FirmwareSignatureAlgorithm>(data[13]);
  parsed.manifest.flags = read_u16(data + 14);
  parsed.manifest.product_id = read_u32(data + 16);
  parsed.manifest.hardware_id = read_u32(data + 20);
  parsed.manifest.image_size = read_u32(data + 24);
  parsed.manifest.trust_revision = read_u32(data + 28);
  parsed.manifest.security_epoch = read_u32(data + 32);
  std::memcpy(parsed.manifest.signer_key_id.data(), data + 36, 16);
  std::memcpy(parsed.manifest.image_sha256.data(), data + 52, 32);
  std::memcpy(parsed.manifest.firmware_version.data(), data + 84, 24);
  std::memcpy(parsed.manifest.build_id.data(), data + 108, 24);
  std::memcpy(parsed.manifest.revoked_key_id.data(), data + 132, 16);
  std::memcpy(parsed.manifest.replacement_key_id.data(), data + 148, 16);
  std::memcpy(parsed.manifest.replacement_public_key.data(), data + 164, 65);
  parsed.manifest.replacement_key_role =
      static_cast<FirmwareSignerRole>(data[229]);
  std::memcpy(parsed.signature.data(), data + 256, parsed.signature.size());
  std::memcpy(parsed.signed_bytes.data(), data, parsed.signed_bytes.size());

  if (parsed.manifest.signature_algorithm !=
      FirmwareSignatureAlgorithm::ES256_P1363)
    return FirmwarePackageResult::UNSUPPORTED_ALGORITHM;
  if (parsed.manifest.signer_role != FirmwareSignerRole::RELEASE &&
      parsed.manifest.signer_role != FirmwareSignerRole::DEVELOPER &&
      parsed.manifest.signer_role != FirmwareSignerRole::ROOT_RECOVERY)
    return FirmwarePackageResult::INVALID_ROLE;
  if (parsed.manifest.flags != 0 ||
      !all_zero(data + 230, FirmwarePackageHeader::SIGNED_SIZE - 230))
    return FirmwarePackageResult::INVALID_RESERVED;
  if (parsed.manifest.product_id != FirmwarePackageHeader::PRODUCT_ID)
    return FirmwarePackageResult::INVALID_PRODUCT;
  if (parsed.manifest.hardware_id != FirmwarePackageHeader::HARDWARE_ID)
    return FirmwarePackageResult::INVALID_HARDWARE;
  if (parsed.manifest.image_size == 0 ||
      parsed.manifest.image_size > FirmwarePackageHeader::MAX_IMAGE_SIZE)
    return FirmwarePackageResult::INVALID_IMAGE_SIZE;
  if (parsed.manifest.trust_revision == 0 ||
      parsed.manifest.security_epoch == 0 ||
      all_zero(parsed.manifest.signer_key_id.data(), 16) ||
      all_zero(parsed.manifest.image_sha256.data(), 32) ||
      !valid_text(parsed.manifest.firmware_version.data(), 24) ||
      !valid_text(parsed.manifest.build_id.data(), 24))
    return FirmwarePackageResult::INVALID_TEXT;

  const bool recovery =
      parsed.manifest.signer_role == FirmwareSignerRole::ROOT_RECOVERY;
  const bool any_recovery =
      !all_zero(parsed.manifest.revoked_key_id.data(), 16) ||
      !all_zero(parsed.manifest.replacement_key_id.data(), 16) ||
      !all_zero(parsed.manifest.replacement_public_key.data(), 65) ||
      data[229] != 0;
  const bool complete_recovery =
      !all_zero(parsed.manifest.revoked_key_id.data(), 16) &&
      !all_zero(parsed.manifest.replacement_key_id.data(), 16) &&
      valid_public_key(parsed.manifest.replacement_public_key) &&
      (parsed.manifest.replacement_key_role == FirmwareSignerRole::RELEASE ||
       parsed.manifest.replacement_key_role == FirmwareSignerRole::DEVELOPER);
  if ((recovery && !complete_recovery) || (!recovery && any_recovery) ||
      (recovery &&
       key_id_equal(parsed.manifest.revoked_key_id,
                    parsed.manifest.replacement_key_id)))
    return FirmwarePackageResult::INVALID_RECOVERY;
  *output = parsed;
  return FirmwarePackageResult::OK;
}

const char *firmware_package_result_to_string(FirmwarePackageResult result) {
  switch (result) {
    case FirmwarePackageResult::OK: return "ok";
    case FirmwarePackageResult::INVALID_ARGUMENT: return "invalid_argument";
    case FirmwarePackageResult::INVALID_MAGIC: return "invalid_magic";
    case FirmwarePackageResult::UNSUPPORTED_FORMAT: return "unsupported_format";
    case FirmwarePackageResult::UNSUPPORTED_ALGORITHM: return "unsupported_algorithm";
    case FirmwarePackageResult::INVALID_ROLE: return "invalid_role";
    case FirmwarePackageResult::INVALID_PRODUCT: return "invalid_product";
    case FirmwarePackageResult::INVALID_HARDWARE: return "invalid_hardware";
    case FirmwarePackageResult::INVALID_IMAGE_SIZE: return "invalid_image_size";
    case FirmwarePackageResult::INVALID_TEXT: return "invalid_text";
    case FirmwarePackageResult::INVALID_RECOVERY: return "invalid_recovery";
    case FirmwarePackageResult::INVALID_RESERVED: return "invalid_reserved";
  }
  return "unknown";
}

FirmwareTrustDecision evaluate_firmware_package_trust(
    const FirmwarePackageHeader &package, const FirmwareTrustState &current,
    const std::array<uint8_t, FirmwarePackageManifest::KEY_ID_SIZE>
        &root_key_id,
    const std::array<uint8_t, FirmwarePackageManifest::PUBLIC_KEY_SIZE>
        &root_public_key,
    FirmwareSignatureVerifier &verifier) {
  FirmwareTrustDecision decision{};
  if (!valid_state(current) || !valid_public_key(root_public_key)) {
    decision.result = FirmwareTrustResult::TRUST_UNAVAILABLE;
    return decision;
  }
  if (revoked(current, package.manifest.signer_key_id)) {
    decision.result = FirmwareTrustResult::REVOKED_KEY;
    return decision;
  }

  if (package.manifest.signer_role == FirmwareSignerRole::ROOT_RECOVERY) {
    if (all_zero(root_key_id.data(), root_key_id.size()) ||
        !key_id_equal(package.manifest.signer_key_id, root_key_id)) {
      decision.result = FirmwareTrustResult::UNKNOWN_KEY;
      return decision;
    }
    if (package.manifest.trust_revision <= current.trust_revision) {
      decision.result = FirmwareTrustResult::TRUST_ROLLBACK;
      return decision;
    }
    if (!verifier.verify_es256_p1363(root_public_key,
                                     package.signed_bytes.data(),
                                     package.signed_bytes.size(),
                                     package.signature)) {
      decision.result = FirmwareTrustResult::INVALID_SIGNATURE;
      return decision;
    }
    if (!verifier.validate_es256_public_key(
            package.manifest.replacement_public_key,
            package.manifest.replacement_key_id)) {
      decision.result = FirmwareTrustResult::INVALID_RECOVERY;
      return decision;
    }
    decision.next_trust = current;
    remove_key(&decision.next_trust, package.manifest.revoked_key_id);
    if (!append_revoked(&decision.next_trust,
                        package.manifest.revoked_key_id) ||
        !install_key(
            &decision.next_trust, package.manifest.replacement_key_role,
            package.manifest.replacement_key_id,
            package.manifest.replacement_public_key)) {
      decision.result = FirmwareTrustResult::KEY_CAPACITY_EXCEEDED;
      return decision;
    }
    decision.next_trust.trust_revision = package.manifest.trust_revision;
    // A higher Root-authorized trust revision may rebase an epoch pinned by a
    // compromised Release key. The old key is removed and revoked atomically.
    decision.next_trust.minimum_security_epoch =
        package.manifest.security_epoch;
    if (!valid_state(decision.next_trust)) {
      decision.result = FirmwareTrustResult::INVALID_RECOVERY;
      return decision;
    }
    decision.result = FirmwareTrustResult::OK;
    decision.trust_update_required = true;
    return decision;
  }

  if (package.manifest.security_epoch < current.minimum_security_epoch) {
    decision.result = FirmwareTrustResult::SECURITY_ROLLBACK;
    return decision;
  }

  if (package.manifest.trust_revision != current.trust_revision) {
    decision.result = FirmwareTrustResult::TRUST_REVISION_MISMATCH;
    return decision;
  }
  const auto *key = find_key(current, package.manifest.signer_role,
                             package.manifest.signer_key_id);
  if (key == nullptr) {
    const auto *other =
        find_key(current,
                 package.manifest.signer_role == FirmwareSignerRole::RELEASE
                     ? FirmwareSignerRole::DEVELOPER
                     : FirmwareSignerRole::RELEASE,
                 package.manifest.signer_key_id);
    decision.result = other == nullptr ? FirmwareTrustResult::UNKNOWN_KEY
                                       : FirmwareTrustResult::ROLE_MISMATCH;
    return decision;
  }
  if (!verifier.verify_es256_p1363(
          key->public_key, package.signed_bytes.data(),
          package.signed_bytes.size(), package.signature)) {
    decision.result = FirmwareTrustResult::INVALID_SIGNATURE;
    return decision;
  }
  if (package.manifest.signer_role == FirmwareSignerRole::DEVELOPER &&
      package.manifest.security_epoch != current.minimum_security_epoch) {
    decision.result = FirmwareTrustResult::SECURITY_EPOCH_MISMATCH;
    return decision;
  }
  decision.result = FirmwareTrustResult::OK;
  decision.next_trust = current;
  if (package.manifest.signer_role == FirmwareSignerRole::RELEASE &&
      package.manifest.security_epoch > current.minimum_security_epoch) {
    decision.next_trust.minimum_security_epoch =
        package.manifest.security_epoch;
    decision.trust_update_required = true;
  }
  return decision;
}

const char *firmware_trust_result_to_string(FirmwareTrustResult result) {
  switch (result) {
    case FirmwareTrustResult::OK: return "ok";
    case FirmwareTrustResult::INVALID_ARGUMENT: return "invalid_argument";
    case FirmwareTrustResult::TRUST_UNAVAILABLE: return "trust_unavailable";
    case FirmwareTrustResult::UNKNOWN_KEY: return "unknown_key";
    case FirmwareTrustResult::REVOKED_KEY: return "revoked_key";
    case FirmwareTrustResult::ROLE_MISMATCH: return "role_mismatch";
    case FirmwareTrustResult::TRUST_REVISION_MISMATCH: return "trust_revision_mismatch";
    case FirmwareTrustResult::TRUST_ROLLBACK: return "trust_rollback";
    case FirmwareTrustResult::SECURITY_ROLLBACK: return "security_rollback";
    case FirmwareTrustResult::SECURITY_EPOCH_MISMATCH: return "security_epoch_mismatch";
    case FirmwareTrustResult::INVALID_SIGNATURE: return "invalid_signature";
    case FirmwareTrustResult::INVALID_RECOVERY: return "invalid_recovery";
    case FirmwareTrustResult::KEY_CAPACITY_EXCEEDED: return "key_capacity_exceeded";
  }
  return "unknown";
}

FirmwareTrustStoreResult FirmwareTrustStore::load_slot_(
    FirmwareTrustStorageBackend &backend, uint8_t slot,
    FirmwareTrustSnapshot *snapshot) {
  if (slot > 1 || snapshot == nullptr ||
      !backend.load_firmware_trust_slot(slot, &this->blob_))
    return FirmwareTrustStoreResult::EMPTY;
  return decode_state(this->blob_, snapshot);
}

FirmwareTrustStoreResult FirmwareTrustStore::write_(
    FirmwareTrustStorageBackend &backend, uint8_t slot,
    const FirmwareTrustState &state, uint32_t generation) {
  const auto slot_result =
      this->write_slot_(backend, slot, state, generation);
  if (slot_result != FirmwareTrustStoreResult::STAGED)
    return slot_result;
  FirmwareTrustCommitBlob commit{};
  encode_commit(slot, generation, state.trust_revision, &commit);
  if (!backend.save_firmware_trust_commit(commit) || !backend.sync())
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  return FirmwareTrustStoreResult::UPDATED;
}

FirmwareTrustStoreResult FirmwareTrustStore::write_slot_(
    FirmwareTrustStorageBackend &backend, uint8_t slot,
    const FirmwareTrustState &state, uint32_t generation) {
  if (!encode_state(state, generation, &this->blob_))
    return FirmwareTrustStoreResult::INVALID_STATE;
  if (!backend.save_firmware_trust_slot(slot, this->blob_) || !backend.sync())
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  return FirmwareTrustStoreResult::STAGED;
}

FirmwareTrustStoreResult FirmwareTrustStore::load(
    FirmwareTrustStorageBackend &backend, FirmwareTrustSnapshot *snapshot) {
  if (snapshot == nullptr)
    return FirmwareTrustStoreResult::INVALID_STATE;
  FirmwareTrustCommitBlob commit{};
  uint8_t committed_slot = 0;
  uint32_t committed_generation = 0;
  uint32_t committed_revision = 0;
  const bool has_commit = backend.load_firmware_trust_commit(&commit);
  if (has_commit) {
    if (!decode_commit(commit, &committed_slot, &committed_generation,
                       &committed_revision)) {
      *snapshot = {};
      return FirmwareTrustStoreResult::CORRUPT;
    }
    const auto result = this->load_slot_(backend, committed_slot, snapshot);
    if (result != FirmwareTrustStoreResult::LOADED ||
        snapshot->generation != committed_generation ||
        snapshot->state.trust_revision != committed_revision) {
      // Never fall back to an older valid keyring after a committed revocation.
      *snapshot = {};
      return FirmwareTrustStoreResult::CORRUPT;
    }
    return result;
  }

  FirmwareTrustSnapshot first{};
  FirmwareTrustSnapshot second{};
  const auto first_result = this->load_slot_(backend, 0, &first);
  const auto second_result = this->load_slot_(backend, 1, &second);
  const bool first_valid = first_result == FirmwareTrustStoreResult::LOADED;
  const bool second_valid = second_result == FirmwareTrustStoreResult::LOADED;
  if (!first_valid && !second_valid)
    return first_result == FirmwareTrustStoreResult::EMPTY &&
                   second_result == FirmwareTrustStoreResult::EMPTY
               ? FirmwareTrustStoreResult::EMPTY
               : FirmwareTrustStoreResult::CORRUPT;
  const uint8_t repaired_slot =
      second_valid && (!first_valid || second.generation > first.generation)
          ? 1
          : 0;
  *snapshot = repaired_slot == 0 ? first : second;
  encode_commit(repaired_slot, snapshot->generation,
                snapshot->state.trust_revision, &commit);
  if (!backend.save_firmware_trust_commit(commit) || !backend.sync())
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  return FirmwareTrustStoreResult::COMMIT_REPAIRED;
}

FirmwareTrustStoreResult FirmwareTrustStore::create(
    FirmwareTrustStorageBackend &backend, const FirmwareTrustState &initial,
    FirmwareTrustSnapshot *snapshot) {
  if (snapshot == nullptr || !valid_state(initial))
    return FirmwareTrustStoreResult::INVALID_STATE;
  const auto result = this->write_(backend, 0, initial, 1);
  if (result != FirmwareTrustStoreResult::UPDATED)
    return result;
  snapshot->generation = 1;
  snapshot->state = initial;
  return FirmwareTrustStoreResult::CREATED;
}

FirmwareTrustStoreResult FirmwareTrustStore::update(
    FirmwareTrustStorageBackend &backend, const FirmwareTrustState &next,
    FirmwareTrustSnapshot *snapshot) {
  if (snapshot == nullptr)
    return FirmwareTrustStoreResult::INVALID_STATE;
  FirmwareTrustStagedUpdate staged{};
  const auto stage_result =
      this->stage_update(backend, next, *snapshot, &staged);
  if (stage_result != FirmwareTrustStoreResult::STAGED)
    return stage_result;
  return this->commit_staged(backend, staged, snapshot);
}

FirmwareTrustStoreResult FirmwareTrustStore::stage_update(
    FirmwareTrustStorageBackend &backend, const FirmwareTrustState &next,
    const FirmwareTrustSnapshot &snapshot,
    FirmwareTrustStagedUpdate *staged) {
  if (staged == nullptr)
    return FirmwareTrustStoreResult::INVALID_STATE;
  *staged = {};
  if (snapshot.generation == 0 || !valid_state(next))
    return FirmwareTrustStoreResult::INVALID_STATE;
  if (next.trust_revision < snapshot.state.trust_revision)
    return FirmwareTrustStoreResult::ROLLBACK_REJECTED;
  if (next.trust_revision == snapshot.state.trust_revision &&
      next.minimum_security_epoch <=
          snapshot.state.minimum_security_epoch)
    return FirmwareTrustStoreResult::ROLLBACK_REJECTED;
  FirmwareTrustCommitBlob commit{};
  uint8_t active_slot = 0;
  uint32_t generation = 0;
  uint32_t revision = 0;
  if (!backend.load_firmware_trust_commit(&commit) ||
      !decode_commit(commit, &active_slot, &generation, &revision) ||
      generation != snapshot.generation ||
      revision != snapshot.state.trust_revision)
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  const uint32_t next_generation = generation + 1;
  if (next_generation == 0)
    return FirmwareTrustStoreResult::INVALID_STATE;
  const uint8_t staged_slot = active_slot == 0 ? 1 : 0;
  const auto result =
      this->write_slot_(backend, staged_slot, next, next_generation);
  if (result != FirmwareTrustStoreResult::STAGED)
    return result;
  staged->valid = true;
  staged->slot = staged_slot;
  staged->generation = next_generation;
  staged->trust_revision = next.trust_revision;
  return FirmwareTrustStoreResult::STAGED;
}

FirmwareTrustStoreResult FirmwareTrustStore::commit_staged(
    FirmwareTrustStorageBackend &backend,
    const FirmwareTrustStagedUpdate &staged,
    FirmwareTrustSnapshot *snapshot) {
  if (!staged.valid || staged.slot > 1 || staged.generation == 0 ||
      snapshot == nullptr || snapshot->generation == 0)
    return FirmwareTrustStoreResult::INVALID_STATE;

  FirmwareTrustCommitBlob current_commit{};
  uint8_t active_slot = 0;
  uint32_t active_generation = 0;
  uint32_t active_revision = 0;
  if (!backend.load_firmware_trust_commit(&current_commit) ||
      !decode_commit(current_commit, &active_slot, &active_generation,
                     &active_revision) ||
      active_generation != snapshot->generation ||
      active_revision != snapshot->state.trust_revision ||
      staged.slot == active_slot ||
      staged.generation != active_generation + 1)
    return FirmwareTrustStoreResult::STORAGE_FAILED;

  FirmwareTrustSnapshot candidate{};
  const auto load_result =
      this->load_slot_(backend, staged.slot, &candidate);
  if (load_result != FirmwareTrustStoreResult::LOADED ||
      candidate.generation != staged.generation ||
      candidate.state.trust_revision != staged.trust_revision)
    return FirmwareTrustStoreResult::STORAGE_FAILED;

  FirmwareTrustCommitBlob next_commit{};
  encode_commit(staged.slot, staged.generation, staged.trust_revision,
                &next_commit);
  if (!backend.save_firmware_trust_commit(next_commit) || !backend.sync())
    return FirmwareTrustStoreResult::STORAGE_FAILED;
  *snapshot = candidate;
  return FirmwareTrustStoreResult::UPDATED;
}

const char *firmware_trust_store_result_to_string(
    FirmwareTrustStoreResult result) {
  switch (result) {
    case FirmwareTrustStoreResult::EMPTY: return "empty";
    case FirmwareTrustStoreResult::LOADED: return "loaded";
    case FirmwareTrustStoreResult::CREATED: return "created";
    case FirmwareTrustStoreResult::STAGED: return "staged";
    case FirmwareTrustStoreResult::UPDATED: return "updated";
    case FirmwareTrustStoreResult::COMMIT_REPAIRED: return "commit_repaired";
    case FirmwareTrustStoreResult::CORRUPT: return "corrupt";
    case FirmwareTrustStoreResult::UNSUPPORTED_VERSION: return "unsupported_version";
    case FirmwareTrustStoreResult::STORAGE_FAILED: return "storage_failed";
    case FirmwareTrustStoreResult::INVALID_STATE: return "invalid_state";
    case FirmwareTrustStoreResult::ROLLBACK_REJECTED: return "rollback_rejected";
  }
  return "unknown";
}

}  // namespace radar_api_server
}  // namespace esphome
