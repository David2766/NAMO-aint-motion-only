#include "site_membership.h"

#include <cstring>

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t MEMBERSHIP_MAGIC[] = {'N', 'A', 'M', 'S'};
constexpr uint8_t COMMIT_MAGIC[] = {'N', 'M', 'S', 'C'};
constexpr std::size_t MEMBERSHIP_HEADER_SIZE = 12;
constexpr std::size_t CHECKSUM_SIZE = 4;
constexpr uint16_t COMMIT_VERSION = 1;

uint16_t read_u16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t read_u32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) |
         (static_cast<uint32_t>(data[3]) << 24U);
}

void write_u16(uint8_t *data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
}

void write_u32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value & 0xFFU);
  data[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  data[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  data[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

uint32_t fnv1a(const uint8_t *data, std::size_t size) {
  uint32_t hash = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 16777619U;
  }
  return hash;
}

bool all_bytes_are(const uint8_t *data, std::size_t size, uint8_t expected) {
  for (std::size_t index = 0; index < size; ++index) {
    if (data[index] != expected)
      return false;
  }
  return true;
}

bool valid_identifier(const char *value, std::size_t *length_out = nullptr) {
  if (value == nullptr || value[0] == '\0')
    return false;
  std::size_t length = 0;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (++length > SiteMembership::MAX_ID_LENGTH)
      return false;
    const char character = *cursor;
    const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
                               (character >= 'A' && character <= 'Z') ||
                               (character >= '0' && character <= '9');
    if (!alpha_numeric &&
        (cursor == value || (character != '.' && character != '_' &&
                             character != ':' && character != '-')))
      return false;
  }
  if (length_out != nullptr)
    *length_out = length;
  return true;
}

bool valid_mode(SiteMembershipMode mode) {
  return mode == SiteMembershipMode::STANDALONE || mode == SiteMembershipMode::ENROLLED;
}

void encode_commit(uint8_t slot, uint32_t generation, SiteMembershipCommitBlob *blob) {
  blob->bytes.fill(0xFF);
  std::memcpy(blob->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(blob->bytes.data() + 4, COMMIT_VERSION);
  blob->bytes[6] = slot;
  blob->bytes[7] = 0;
  write_u32(blob->bytes.data() + 8, generation);
  write_u32(blob->bytes.data() + 12, fnv1a(blob->bytes.data(), 12));
}

SiteMembershipStoreResult decode_commit(const SiteMembershipCommitBlob &blob,
                                        uint8_t *slot, uint32_t *generation) {
  if (all_bytes_are(blob.bytes.data(), blob.bytes.size(), 0xFF))
    return SiteMembershipStoreResult::EMPTY;
  if (std::memcmp(blob.bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0)
    return SiteMembershipStoreResult::CORRUPT;
  if (read_u16(blob.bytes.data() + 4) != COMMIT_VERSION)
    return SiteMembershipStoreResult::UNSUPPORTED_VERSION;
  if (blob.bytes[6] > 1 || blob.bytes[7] != 0 ||
      read_u32(blob.bytes.data() + 8) == 0 ||
      read_u32(blob.bytes.data() + 12) != fnv1a(blob.bytes.data(), 12))
    return SiteMembershipStoreResult::CORRUPT;
  *slot = blob.bytes[6];
  *generation = read_u32(blob.bytes.data() + 8);
  return SiteMembershipStoreResult::LOADED;
}

SiteMembershipStoreResult decode_slot(const SiteMembershipStorageBlob &blob,
                                      SiteMembership *membership) {
  const auto result = SiteMembershipCodec::decode(blob.bytes.data(), blob.bytes.size(), membership);
  if (result == SiteMembershipCodecResult::OK)
    return SiteMembershipStoreResult::LOADED;
  if (result == SiteMembershipCodecResult::EMPTY)
    return SiteMembershipStoreResult::EMPTY;
  if (result == SiteMembershipCodecResult::UNSUPPORTED_VERSION)
    return SiteMembershipStoreResult::UNSUPPORTED_VERSION;
  return SiteMembershipStoreResult::CORRUPT;
}

SiteMembershipStoreResult load_committed(SiteMembershipStorageBackend &backend,
                                         SiteMembership *membership,
                                         uint8_t *active_slot) {
  SiteMembershipCommitBlob commit_blob{};
  const bool commit_present = backend.load_site_membership_commit(&commit_blob);
  if (commit_present) {
    uint8_t slot = 0;
    uint32_t generation = 0;
    const auto commit_result = decode_commit(commit_blob, &slot, &generation);
    if (commit_result != SiteMembershipStoreResult::LOADED)
      return commit_result == SiteMembershipStoreResult::EMPTY
                 ? SiteMembershipStoreResult::CORRUPT
                 : commit_result;

    SiteMembershipStorageBlob slot_blob{};
    if (!backend.load_site_membership_slot(slot, &slot_blob))
      return SiteMembershipStoreResult::CORRUPT;
    SiteMembership decoded{};
    const auto slot_result = decode_slot(slot_blob, &decoded);
    if (slot_result != SiteMembershipStoreResult::LOADED)
      return slot_result == SiteMembershipStoreResult::EMPTY
                 ? SiteMembershipStoreResult::CORRUPT
                 : slot_result;
    if (decoded.generation != generation)
      return SiteMembershipStoreResult::CORRUPT;
    *membership = decoded;
    if (active_slot != nullptr)
      *active_slot = slot;
    return SiteMembershipStoreResult::LOADED;
  }

  SiteMembership decoded[2]{};
  bool valid[2]{false, false};
  bool present[2]{false, false};
  for (uint8_t slot = 0; slot < 2; ++slot) {
    SiteMembershipStorageBlob slot_blob{};
    present[slot] = backend.load_site_membership_slot(slot, &slot_blob);
    if (!present[slot])
      continue;
    const auto slot_result = decode_slot(slot_blob, &decoded[slot]);
    if (slot_result == SiteMembershipStoreResult::UNSUPPORTED_VERSION)
      return slot_result;
    if (slot_result == SiteMembershipStoreResult::LOADED)
      valid[slot] = true;
  }

  if (!present[0] && !present[1])
    return SiteMembershipStoreResult::EMPTY;
  if (valid[0] == valid[1])
    return SiteMembershipStoreResult::CORRUPT;

  const uint8_t repair_slot = valid[0] ? 0 : 1;
  SiteMembershipCommitBlob repaired{};
  encode_commit(repair_slot, decoded[repair_slot].generation, &repaired);
  if (!backend.save_site_membership_commit(repaired) || !backend.sync())
    return SiteMembershipStoreResult::STORAGE_FAILED;
  *membership = decoded[repair_slot];
  if (active_slot != nullptr)
    *active_slot = repair_slot;
  return SiteMembershipStoreResult::COMMIT_REPAIRED;
}

SiteMembershipStoreResult write_slot(SiteMembershipStorageBackend &backend, uint8_t slot,
                                     const SiteMembership &membership) {
  SiteMembershipStorageBlob blob{};
  std::size_t encoded_size = 0;
  const auto encode_result = SiteMembershipCodec::encode(
      membership, blob.bytes.data(), blob.bytes.size(), &encoded_size);
  if (encode_result != SiteMembershipCodecResult::OK || encoded_size == 0)
    return SiteMembershipStoreResult::INVALID_MEMBERSHIP;
  if (!backend.save_site_membership_slot(slot, blob) || !backend.sync())
    return SiteMembershipStoreResult::STORAGE_FAILED;
  return SiteMembershipStoreResult::UPDATED;
}

}  // namespace

bool site_membership_matches_target(const SiteMembership &membership,
                                    const char *site_id,
                                    SiteMembershipMode mode) {
  return site_id != nullptr && site_id[0] != '\0' && valid_mode(mode) &&
         std::strcmp(membership.site_id, site_id) == 0 &&
         membership.mode == mode;
}

SiteMembershipCodecResult SiteMembershipCodec::encode(
    const SiteMembership &membership, uint8_t *output, std::size_t output_capacity,
    std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return SiteMembershipCodecResult::INVALID_ARGUMENT;
  std::size_t site_length = 0;
  if (!valid_identifier(membership.site_id, &site_length) ||
      !valid_mode(membership.mode) || membership.generation == 0)
    return SiteMembershipCodecResult::INVALID_MEMBERSHIP;

  const std::size_t encoded_size = MEMBERSHIP_HEADER_SIZE + site_length + CHECKSUM_SIZE;
  if (output_capacity < encoded_size)
    return SiteMembershipCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, MEMBERSHIP_MAGIC, sizeof(MEMBERSHIP_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(membership.mode);
  output[7] = static_cast<uint8_t>(site_length);
  write_u32(output + 8, membership.generation);
  std::memcpy(output + MEMBERSHIP_HEADER_SIZE, membership.site_id, site_length);
  write_u32(output + encoded_size - CHECKSUM_SIZE,
            fnv1a(output, encoded_size - CHECKSUM_SIZE));
  *output_size = encoded_size;
  return SiteMembershipCodecResult::OK;
}

SiteMembershipCodecResult SiteMembershipCodec::decode(
    const uint8_t *data, std::size_t size, SiteMembership *membership) {
  if (data == nullptr || membership == nullptr)
    return SiteMembershipCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF))
    return SiteMembershipCodecResult::EMPTY;
  if (size < MEMBERSHIP_HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, MEMBERSHIP_MAGIC, sizeof(MEMBERSHIP_MAGIC)) != 0)
    return SiteMembershipCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return SiteMembershipCodecResult::UNSUPPORTED_VERSION;

  const auto mode = static_cast<SiteMembershipMode>(data[6]);
  const std::size_t site_length = data[7];
  const uint32_t generation = read_u32(data + 8);
  const std::size_t encoded_size = MEMBERSHIP_HEADER_SIZE + site_length + CHECKSUM_SIZE;
  if (!valid_mode(mode) || site_length == 0 ||
      site_length > SiteMembership::MAX_ID_LENGTH || generation == 0 || encoded_size > size)
    return SiteMembershipCodecResult::CORRUPT;
  if (read_u32(data + encoded_size - CHECKSUM_SIZE) !=
      fnv1a(data, encoded_size - CHECKSUM_SIZE))
    return SiteMembershipCodecResult::CORRUPT;
  if (size > encoded_size &&
      !all_bytes_are(data + encoded_size, size - encoded_size, 0xFF))
    return SiteMembershipCodecResult::CORRUPT;

  SiteMembership decoded{};
  std::memcpy(decoded.site_id, data + MEMBERSHIP_HEADER_SIZE, site_length);
  decoded.mode = mode;
  decoded.generation = generation;
  if (!valid_identifier(decoded.site_id))
    return SiteMembershipCodecResult::CORRUPT;
  *membership = decoded;
  return SiteMembershipCodecResult::OK;
}

SiteMembershipStoreResult SiteMembershipStore::load(
    SiteMembershipStorageBackend &backend, SiteMembership *membership) {
  if (membership == nullptr)
    return SiteMembershipStoreResult::STORAGE_FAILED;
  return load_committed(backend, membership, nullptr);
}

SiteMembershipStoreResult SiteMembershipStore::create(
    SiteMembershipStorageBackend &backend, const char *site_id, SiteMembershipMode mode) {
  SiteMembership membership{};
  if (site_id != nullptr)
    std::strncpy(membership.site_id, site_id, SiteMembership::MAX_ID_LENGTH);
  membership.site_id[SiteMembership::MAX_ID_LENGTH] = '\0';
  membership.mode = mode;
  membership.generation = 1;

  const auto write_result = write_slot(backend, 0, membership);
  if (write_result != SiteMembershipStoreResult::UPDATED)
    return write_result;
  SiteMembershipCommitBlob commit{};
  encode_commit(0, membership.generation, &commit);
  if (!backend.save_site_membership_commit(commit) || !backend.sync())
    return SiteMembershipStoreResult::STORAGE_FAILED;
  return SiteMembershipStoreResult::CREATED;
}

SiteMembershipStoreResult SiteMembershipStore::update(
    SiteMembershipStorageBackend &backend, const char *site_id, SiteMembershipMode mode) {
  SiteMembership current{};
  uint8_t active_slot = 0;
  const auto load_result = load_committed(backend, &current, &active_slot);
  if (load_result != SiteMembershipStoreResult::LOADED &&
      load_result != SiteMembershipStoreResult::COMMIT_REPAIRED)
    return load_result;

  SiteMembership updated{};
  if (site_id != nullptr)
    std::strncpy(updated.site_id, site_id, SiteMembership::MAX_ID_LENGTH);
  updated.site_id[SiteMembership::MAX_ID_LENGTH] = '\0';
  updated.mode = mode;
  updated.generation = current.generation == UINT32_MAX ? 1 : current.generation + 1;

  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = write_slot(backend, next_slot, updated);
  if (write_result != SiteMembershipStoreResult::UPDATED)
    return write_result;
  SiteMembershipCommitBlob commit{};
  encode_commit(next_slot, updated.generation, &commit);
  if (!backend.save_site_membership_commit(commit) || !backend.sync())
    return SiteMembershipStoreResult::STORAGE_FAILED;
  return SiteMembershipStoreResult::UPDATED;
}

}  // namespace radar_api_server
}  // namespace esphome
