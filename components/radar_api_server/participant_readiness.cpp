#include "participant_readiness.h"

#include <cstring>
#include <limits>
#include <new>

#ifdef USE_ESP32
#include "participant_registry_store.h"
#include "radar_storage.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#ifdef USE_PSRAM
#include <esp_heap_caps.h>
#endif
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t READINESS_MAGIC[] = {'N', 'M', 'R', 'D'};
constexpr uint8_t COMMIT_MAGIC[] = {'N', 'R', 'D', 'C'};
constexpr uint16_t STORAGE_VERSION = 3;
constexpr uint16_t TRANSFORM_STORAGE_VERSION = 2;
constexpr uint16_t LEGACY_STORAGE_VERSION = 1;
constexpr uint16_t COMMIT_VERSION = 1;
constexpr std::size_t HEADER_SIZE = 12;
constexpr std::size_t LEGACY_RECORD_HEADER_SIZE = 15;
constexpr std::size_t TRANSFORM_RECORD_HEADER_SIZE = 29;
constexpr std::size_t RECORD_HEADER_SIZE = 37;
constexpr std::size_t CHECKSUM_SIZE = 4;
constexpr uint16_t SUPPORTED_CAPABILITIES =
    PARTICIPANT_CAP_POINT_TRACKS | PARTICIPANT_CAP_VELOCITY |
    PARTICIPANT_CAP_POSITION_UNCERTAINTY | PARTICIPANT_CAP_RANGE |
    PARTICIPANT_CAP_PRESENCE | PARTICIPANT_CAP_MOTION |
    PARTICIPANT_CAP_ZONE_PRESENCE | PARTICIPANT_CAP_BRIDGE;

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

int32_t read_i32(const uint8_t *data) {
  return static_cast<int32_t>(read_u32(data));
}

void write_i32(uint8_t *data, int32_t value) {
  write_u32(data, static_cast<uint32_t>(value));
}

int16_t read_i16(const uint8_t *data) {
  return static_cast<int16_t>(read_u16(data));
}

void write_i16(uint8_t *data, int16_t value) {
  write_u16(data, static_cast<uint16_t>(value));
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

bool equal_text(const char *left, const char *right) {
  return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

void encode_commit(uint8_t slot, uint32_t generation,
                   ParticipantReadinessStorageBackend::CommitBlob *blob) {
  blob->fill(0xFF);
  std::memcpy(blob->data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(blob->data() + 4, COMMIT_VERSION);
  (*blob)[6] = slot;
  (*blob)[7] = 0;
  write_u32(blob->data() + 8, generation);
  write_u32(blob->data() + 12, fnv1a(blob->data(), 12));
}

ParticipantReadinessStoreResult decode_commit(
    const ParticipantReadinessStorageBackend::CommitBlob &blob, uint8_t *slot,
    uint32_t *generation) {
  if (all_bytes_are(blob.data(), blob.size(), 0xFF))
    return ParticipantReadinessStoreResult::EMPTY;
  if (std::memcmp(blob.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0)
    return ParticipantReadinessStoreResult::CORRUPT;
  if (read_u16(blob.data() + 4) != COMMIT_VERSION)
    return ParticipantReadinessStoreResult::UNSUPPORTED_VERSION;
  if (blob[6] > 1 || blob[7] != 0 || read_u32(blob.data() + 8) == 0 ||
      read_u32(blob.data() + 12) != fnv1a(blob.data(), 12))
    return ParticipantReadinessStoreResult::CORRUPT;
  *slot = blob[6];
  *generation = read_u32(blob.data() + 8);
  return ParticipantReadinessStoreResult::LOADED;
}

ParticipantReadinessStoreResult encode_snapshot(
    const ParticipantReadinessSnapshot &snapshot,
    ParticipantReadinessStorageBackend::StorageBlob *blob) {
  if (snapshot.generation == 0 || blob == nullptr)
    return ParticipantReadinessStoreResult::INVALID_DATA;

  std::size_t encoded_size = HEADER_SIZE + CHECKSUM_SIZE;
  for (std::size_t slot = 0; slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS;
       ++slot) {
    const auto *record = snapshot.registry.record_at(slot);
    if (record == nullptr)
      continue;
    if (!ParticipantReadinessRegistry::valid_config(record->config))
      return ParticipantReadinessStoreResult::INVALID_DATA;
    encoded_size += RECORD_HEADER_SIZE + std::strlen(record->participant_id) +
                    std::strlen(record->config.floorplan_id) +
                    std::strlen(record->config.space_id) +
                    std::strlen(record->config.group_id);
  }
  if (encoded_size > blob->size())
    return ParticipantReadinessStoreResult::INVALID_DATA;

  blob->fill(0xFF);
  std::memcpy(blob->data(), READINESS_MAGIC, sizeof(READINESS_MAGIC));
  write_u16(blob->data() + 4, STORAGE_VERSION);
  (*blob)[6] = static_cast<uint8_t>(snapshot.registry.size());
  (*blob)[7] = 0;
  write_u32(blob->data() + 8, snapshot.generation);

  std::size_t offset = HEADER_SIZE;
  for (std::size_t slot = 0; slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS;
       ++slot) {
    const auto *record = snapshot.registry.record_at(slot);
    if (record == nullptr)
      continue;
    const std::size_t participant_length = std::strlen(record->participant_id);
    const std::size_t floorplan_length = std::strlen(record->config.floorplan_id);
    const std::size_t space_length = std::strlen(record->config.space_id);
    const std::size_t group_length = std::strlen(record->config.group_id);
    (*blob)[offset++] = static_cast<uint8_t>(participant_length);
    write_u16(blob->data() + offset, record->config.capability_flags);
    offset += 2;
    (*blob)[offset++] = record->config.max_targets;
    (*blob)[offset++] = static_cast<uint8_t>(floorplan_length);
    (*blob)[offset++] = static_cast<uint8_t>(space_length);
    (*blob)[offset++] = static_cast<uint8_t>(group_length);
    write_u32(blob->data() + offset, record->config.pose_version);
    offset += 4;
    write_u32(blob->data() + offset, record->config.calibration_version);
    offset += 4;
    write_u32(blob->data() + offset, record->config.geometry_version);
    offset += 4;
    write_u32(blob->data() + offset, record->config.local_pose_version);
    offset += 4;
    write_i32(blob->data() + offset, record->config.transform.origin_x_mm);
    offset += 4;
    write_i32(blob->data() + offset, record->config.transform.origin_y_mm);
    offset += 4;
    write_i16(blob->data() + offset, record->config.transform.rotation_cdeg);
    offset += 2;
    write_u16(blob->data() + offset, record->config.transform.residual_mm);
    offset += 2;
    (*blob)[offset++] = record->config.transform.sample_count;
    (*blob)[offset++] = record->config.transform.valid ? 1U : 0U;
    std::memcpy(blob->data() + offset, record->participant_id, participant_length);
    offset += participant_length;
    std::memcpy(blob->data() + offset, record->config.floorplan_id, floorplan_length);
    offset += floorplan_length;
    std::memcpy(blob->data() + offset, record->config.space_id, space_length);
    offset += space_length;
    std::memcpy(blob->data() + offset, record->config.group_id, group_length);
    offset += group_length;
  }
  write_u32(blob->data() + offset, fnv1a(blob->data(), offset));
  return ParticipantReadinessStoreResult::UPDATED;
}

ParticipantReadinessStoreResult decode_snapshot(
    const ParticipantReadinessStorageBackend::StorageBlob &blob,
    ParticipantReadinessSnapshot *snapshot) {
  if (snapshot == nullptr)
    return ParticipantReadinessStoreResult::INVALID_DATA;
  if (all_bytes_are(blob.data(), blob.size(), 0xFF))
    return ParticipantReadinessStoreResult::EMPTY;
  if (std::memcmp(blob.data(), READINESS_MAGIC, sizeof(READINESS_MAGIC)) != 0)
    return ParticipantReadinessStoreResult::CORRUPT;
  const uint16_t storage_version = read_u16(blob.data() + 4);
  if (storage_version != STORAGE_VERSION &&
      storage_version != TRANSFORM_STORAGE_VERSION &&
      storage_version != LEGACY_STORAGE_VERSION)
    return ParticipantReadinessStoreResult::UNSUPPORTED_VERSION;
  const std::size_t record_header_size =
      storage_version == STORAGE_VERSION
          ? RECORD_HEADER_SIZE
          : (storage_version == TRANSFORM_STORAGE_VERSION
                 ? TRANSFORM_RECORD_HEADER_SIZE
                 : LEGACY_RECORD_HEADER_SIZE);

  const std::size_t record_count = blob[6];
  const uint32_t generation = read_u32(blob.data() + 8);
  if (record_count > ParticipantRegistry::MAX_REMOTE_PARTICIPANTS || blob[7] != 0 ||
      generation == 0)
    return ParticipantReadinessStoreResult::CORRUPT;

  snapshot->registry.clear();
  snapshot->generation = 0;
  std::size_t offset = HEADER_SIZE;
  for (std::size_t index = 0; index < record_count; ++index) {
    if (offset + record_header_size + CHECKSUM_SIZE > blob.size())
      return ParticipantReadinessStoreResult::CORRUPT;
    const std::size_t participant_length = blob[offset++];
    ParticipantReadinessConfig config{};
    config.capability_flags = read_u16(blob.data() + offset);
    offset += 2;
    config.max_targets = blob[offset++];
    const std::size_t floorplan_length = blob[offset++];
    const std::size_t space_length = blob[offset++];
    const std::size_t group_length = blob[offset++];
    config.pose_version = read_u32(blob.data() + offset);
    offset += 4;
    config.calibration_version = read_u32(blob.data() + offset);
    offset += 4;
    if (storage_version == STORAGE_VERSION) {
      config.geometry_version = read_u32(blob.data() + offset);
      offset += 4;
      config.local_pose_version = read_u32(blob.data() + offset);
      offset += 4;
    }
    if (storage_version >= TRANSFORM_STORAGE_VERSION) {
      config.transform.origin_x_mm = read_i32(blob.data() + offset);
      offset += 4;
      config.transform.origin_y_mm = read_i32(blob.data() + offset);
      offset += 4;
      config.transform.rotation_cdeg = read_i16(blob.data() + offset);
      offset += 2;
      config.transform.residual_mm = read_u16(blob.data() + offset);
      offset += 2;
      config.transform.sample_count = blob[offset++];
      if (blob[offset] > 1)
        return ParticipantReadinessStoreResult::CORRUPT;
      config.transform.valid = blob[offset++] == 1;
    }
    const std::size_t text_size = participant_length + floorplan_length +
                                  space_length + group_length;
    if (participant_length == 0 || participant_length > ParticipantRecord::MAX_ID_LENGTH ||
        floorplan_length == 0 ||
        floorplan_length > ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
        space_length == 0 ||
        space_length > ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
        group_length == 0 ||
        group_length > ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH ||
        offset + text_size + CHECKSUM_SIZE > blob.size())
      return ParticipantReadinessStoreResult::CORRUPT;

    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    std::memcpy(participant_id, blob.data() + offset, participant_length);
    offset += participant_length;
    std::memcpy(config.floorplan_id, blob.data() + offset, floorplan_length);
    offset += floorplan_length;
    std::memcpy(config.space_id, blob.data() + offset, space_length);
    offset += space_length;
    std::memcpy(config.group_id, blob.data() + offset, group_length);
    offset += group_length;
    if (snapshot->registry.configure(participant_id, config) !=
        ParticipantReadinessResult::OK)
      return ParticipantReadinessStoreResult::CORRUPT;
  }

  if (offset + CHECKSUM_SIZE > blob.size() ||
      read_u32(blob.data() + offset) != fnv1a(blob.data(), offset))
    return ParticipantReadinessStoreResult::CORRUPT;
  offset += CHECKSUM_SIZE;
  if (offset < blob.size() && !all_bytes_are(blob.data() + offset,
                                             blob.size() - offset, 0xFF))
    return ParticipantReadinessStoreResult::CORRUPT;
  if (snapshot->registry.size() != record_count)
    return ParticipantReadinessStoreResult::CORRUPT;
  snapshot->generation = generation;
  return ParticipantReadinessStoreResult::LOADED;
}

}  // namespace

ParticipantReadinessResult ParticipantReadinessRegistry::configure(
    const char *participant_id, const ParticipantReadinessConfig &config) {
  if (!valid_id_(participant_id, ParticipantRecord::MAX_ID_LENGTH))
    return ParticipantReadinessResult::INVALID_ARGUMENT;
  if (!valid_config(config))
    return (config.capability_flags & PARTICIPANT_CAP_PRESENCE) == 0
               ? ParticipantReadinessResult::INVALID_CAPABILITIES
               : ParticipantReadinessResult::INVALID_ASSIGNMENT;

  ParticipantReadinessRecord *record = this->find_mutable_(participant_id);
  if (record == nullptr) {
    for (auto &entry : this->entries_) {
      if (!entry.occupied) {
        record = &entry;
        break;
      }
    }
  }
  if (record == nullptr)
    return ParticipantReadinessResult::FULL;

  *record = ParticipantReadinessRecord{};
  record->occupied = true;
  copy_(record->participant_id, sizeof(record->participant_id), participant_id);
  record->config = config;
  return ParticipantReadinessResult::OK;
}

ParticipantReadinessResult ParticipantReadinessRegistry::forget(
    const char *participant_id) {
  ParticipantReadinessRecord *record = this->find_mutable_(participant_id);
  if (record == nullptr)
    return ParticipantReadinessResult::NOT_FOUND;
  *record = ParticipantReadinessRecord{};
  return ParticipantReadinessResult::OK;
}

void ParticipantReadinessRegistry::clear() {
  for (auto &entry : this->entries_)
    entry = ParticipantReadinessRecord{};
}

const ParticipantReadinessRecord *ParticipantReadinessRegistry::find(
    const char *participant_id) const {
  if (!valid_id_(participant_id, ParticipantRecord::MAX_ID_LENGTH))
    return nullptr;
  for (const auto &entry : this->entries_) {
    if (entry.occupied && std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

const ParticipantReadinessRecord *ParticipantReadinessRegistry::record_at(
    std::size_t slot) const {
  if (slot >= this->entries_.size() || !this->entries_[slot].occupied)
    return nullptr;
  return &this->entries_[slot];
}

std::size_t ParticipantReadinessRegistry::size() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_)
    count += entry.occupied ? 1U : 0U;
  return count;
}

bool ParticipantReadinessRegistry::valid_config(
    const ParticipantReadinessConfig &config) {
  if ((config.capability_flags & ~SUPPORTED_CAPABILITIES) != 0 ||
      (config.capability_flags & PARTICIPANT_CAP_PRESENCE) == 0)
    return false;
  const bool spatial =
      (config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) != 0;
  if ((spatial && (config.max_targets == 0 || config.max_targets > 3)) ||
      (!spatial && config.max_targets != 0))
    return false;
  if (!spatial &&
      (config.capability_flags &
       (PARTICIPANT_CAP_VELOCITY | PARTICIPANT_CAP_POSITION_UNCERTAINTY)) != 0)
    return false;
  if (config.transform.valid &&
      (!spatial || config.calibration_version == 0 ||
       !valid_spatial_transform(config.transform)))
    return false;
  return valid_id_(config.floorplan_id, ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) &&
         valid_id_(config.space_id, ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH) &&
         valid_id_(config.group_id, ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH);
}

uint8_t ParticipantReadinessRegistry::missing_requirements(
    const ParticipantReadinessRecord *record) {
  if (record == nullptr)
    return PARTICIPANT_MISSING_CAPABILITIES | PARTICIPANT_MISSING_ASSIGNMENT |
           PARTICIPANT_MISSING_POSE | PARTICIPANT_MISSING_CALIBRATION;
  uint8_t missing = PARTICIPANT_MISSING_NONE;
  if (!valid_config(record->config))
    missing |= PARTICIPANT_MISSING_CAPABILITIES |
               PARTICIPANT_MISSING_ASSIGNMENT;
  if ((record->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) != 0) {
    if (record->config.geometry_version == 0)
      missing |= PARTICIPANT_MISSING_ASSIGNMENT;
    if (record->config.local_pose_version == 0 ||
        record->config.pose_version == 0)
      missing |= PARTICIPANT_MISSING_POSE;
    if (record->config.calibration_version == 0)
      missing |= PARTICIPANT_MISSING_CALIBRATION;
  }
  return missing;
}

SpatialReadinessMismatch spatial_readiness_mismatch(
    const ParticipantReadinessRecord *record, bool floorplan_committed,
    const char *floorplan_id, uint32_t geometry_version,
    uint32_t local_pose_version, uint32_t remote_pose_version) {
  if (record == nullptr ||
      (record->config.capability_flags & PARTICIPANT_CAP_POINT_TRACKS) == 0)
    return SpatialReadinessMismatch::NONE;
  if (!floorplan_committed || floorplan_id == nullptr ||
      floorplan_id[0] == '\0' ||
      std::strcmp(record->config.floorplan_id, floorplan_id) != 0 ||
      record->config.geometry_version != geometry_version)
    return SpatialReadinessMismatch::FLOORPLAN_GEOMETRY;
  if (local_pose_version == 0 ||
      record->config.local_pose_version != local_pose_version)
    return SpatialReadinessMismatch::LOCAL_POSE;
  if (remote_pose_version != 0 &&
      record->config.pose_version != remote_pose_version)
    return SpatialReadinessMismatch::REMOTE_POSE;
  return SpatialReadinessMismatch::NONE;
}

const char *spatial_readiness_mismatch_to_string(
    SpatialReadinessMismatch mismatch) {
  switch (mismatch) {
    case SpatialReadinessMismatch::NONE: return "none";
    case SpatialReadinessMismatch::FLOORPLAN_GEOMETRY:
      return "floorplan_geometry_changed";
    case SpatialReadinessMismatch::LOCAL_POSE:
      return "local_radar_pose_changed";
    case SpatialReadinessMismatch::REMOTE_POSE:
      return "remote_radar_pose_changed";
  }
  return "unknown";
}

ParticipantReadinessRecord *ParticipantReadinessRegistry::find_mutable_(
    const char *participant_id) {
  if (!valid_id_(participant_id, ParticipantRecord::MAX_ID_LENGTH))
    return nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied && std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

bool ParticipantReadinessRegistry::valid_id_(const char *value,
                                             std::size_t max_length) {
  if (value == nullptr || value[0] == '\0')
    return false;
  std::size_t length = 0;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (++length > max_length)
      return false;
    const char character = *cursor;
    const bool alpha_numeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    if (!alpha_numeric && (cursor == value ||
                           (character != '.' && character != '_' &&
                            character != ':' && character != '-')))
      return false;
  }
  return true;
}

void ParticipantReadinessRegistry::copy_(char *target, std::size_t capacity,
                                         const char *source) {
  std::strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}

ParticipantReadinessStoreResult ParticipantReadinessStore::load(
    ParticipantReadinessStorageBackend &backend,
    ParticipantReadinessSnapshot *snapshot) {
  if (snapshot == nullptr)
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  const auto result = this->load_committed_(backend, nullptr);
  if (result == ParticipantReadinessStoreResult::LOADED ||
      result == ParticipantReadinessStoreResult::COMMIT_REPAIRED)
    *snapshot = this->scratch_;
  return result;
}

ParticipantReadinessStoreResult ParticipantReadinessStore::create(
    ParticipantReadinessStorageBackend &backend,
    const ParticipantReadinessRegistry &registry,
    ParticipantReadinessSnapshot *snapshot) {
  this->scratch_.registry = registry;
  this->scratch_.generation = 1;
  const auto write_result = this->write_slot_(backend, 0);
  if (write_result != ParticipantReadinessStoreResult::UPDATED)
    return write_result;
  ParticipantReadinessStorageBackend::CommitBlob commit{};
  encode_commit(0, 1, &commit);
  if (!backend.save_participant_readiness_commit(commit) || !backend.sync())
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return ParticipantReadinessStoreResult::CREATED;
}

ParticipantReadinessStoreResult ParticipantReadinessStore::update(
    ParticipantReadinessStorageBackend &backend,
    const ParticipantReadinessRegistry &registry,
    ParticipantReadinessSnapshot *snapshot) {
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != ParticipantReadinessStoreResult::LOADED &&
      load_result != ParticipantReadinessStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (this->scratch_.generation == std::numeric_limits<uint32_t>::max())
    return ParticipantReadinessStoreResult::INVALID_DATA;
  this->scratch_.registry = registry;
  ++this->scratch_.generation;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != ParticipantReadinessStoreResult::UPDATED)
    return write_result;
  ParticipantReadinessStorageBackend::CommitBlob commit{};
  encode_commit(next_slot, this->scratch_.generation, &commit);
  if (!backend.save_participant_readiness_commit(commit) || !backend.sync())
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return ParticipantReadinessStoreResult::UPDATED;
}

ParticipantReadinessStoreResult ParticipantReadinessStore::load_or_migrate(
    ParticipantReadinessStorageBackend &backend,
    ParticipantReadinessStorageBackend *legacy_backend,
    ParticipantReadinessSnapshot *snapshot) {
  const auto current_result = this->load(backend, snapshot);
  if (current_result != ParticipantReadinessStoreResult::EMPTY)
    return current_result;

  if (legacy_backend != nullptr) {
    const auto legacy_result = this->load(*legacy_backend, snapshot);
    if (legacy_result == ParticipantReadinessStoreResult::LOADED ||
        legacy_result == ParticipantReadinessStoreResult::COMMIT_REPAIRED) {
      const auto create_result = this->create(backend, snapshot->registry, snapshot);
      return create_result == ParticipantReadinessStoreResult::CREATED
                 ? ParticipantReadinessStoreResult::MIGRATED
                 : create_result;
    }
    if (legacy_result != ParticipantReadinessStoreResult::EMPTY)
      return legacy_result;
  }

  snapshot->registry.clear();
  snapshot->generation = 0;
  return this->create(backend, snapshot->registry, snapshot);
}

ParticipantReadinessStoreResult ParticipantReadinessStore::write_slot_(
    ParticipantReadinessStorageBackend &backend, uint8_t slot) {
  const auto encoded = encode_snapshot(this->scratch_, &this->blob_);
  if (encoded != ParticipantReadinessStoreResult::UPDATED)
    return encoded;
  if (!backend.save_participant_readiness_slot(slot, this->blob_) || !backend.sync())
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  return ParticipantReadinessStoreResult::UPDATED;
}

ParticipantReadinessStoreResult ParticipantReadinessStore::decode_blob_() {
  return decode_snapshot(this->blob_, &this->scratch_);
}

ParticipantReadinessStoreResult ParticipantReadinessStore::load_committed_(
    ParticipantReadinessStorageBackend &backend, uint8_t *active_slot) {
  ParticipantReadinessStorageBackend::CommitBlob commit{};
  if (backend.load_participant_readiness_commit(&commit)) {
    uint8_t slot = 0;
    uint32_t generation = 0;
    const auto commit_result = decode_commit(commit, &slot, &generation);
    if (commit_result != ParticipantReadinessStoreResult::LOADED)
      return commit_result == ParticipantReadinessStoreResult::EMPTY
                 ? ParticipantReadinessStoreResult::CORRUPT
                 : commit_result;
    if (!backend.load_participant_readiness_slot(slot, &this->blob_))
      return ParticipantReadinessStoreResult::CORRUPT;
    const auto slot_result = this->decode_blob_();
    if (slot_result != ParticipantReadinessStoreResult::LOADED ||
        this->scratch_.generation != generation)
      return slot_result == ParticipantReadinessStoreResult::LOADED
                 ? ParticipantReadinessStoreResult::CORRUPT
                 : slot_result;
    if (active_slot != nullptr)
      *active_slot = slot;
    return ParticipantReadinessStoreResult::LOADED;
  }

  int valid_slot = -1;
  uint32_t generation = 0;
  bool any_present = false;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (!backend.load_participant_readiness_slot(slot, &this->blob_))
      continue;
    any_present = true;
    const auto result = this->decode_blob_();
    if (result == ParticipantReadinessStoreResult::UNSUPPORTED_VERSION)
      return result;
    if (result != ParticipantReadinessStoreResult::LOADED)
      continue;
    if (valid_slot >= 0)
      return ParticipantReadinessStoreResult::CORRUPT;
    valid_slot = slot;
    generation = this->scratch_.generation;
  }
  if (!any_present)
    return ParticipantReadinessStoreResult::EMPTY;
  if (valid_slot < 0)
    return ParticipantReadinessStoreResult::CORRUPT;
  if (!backend.load_participant_readiness_slot(static_cast<uint8_t>(valid_slot),
                                                &this->blob_) ||
      this->decode_blob_() != ParticipantReadinessStoreResult::LOADED ||
      this->scratch_.generation != generation)
    return ParticipantReadinessStoreResult::CORRUPT;
  ParticipantReadinessStorageBackend::CommitBlob repaired{};
  encode_commit(static_cast<uint8_t>(valid_slot), generation, &repaired);
  if (!backend.save_participant_readiness_commit(repaired) || !backend.sync())
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  if (active_slot != nullptr)
    *active_slot = static_cast<uint8_t>(valid_slot);
  return ParticipantReadinessStoreResult::COMMIT_REPAIRED;
}

ObservationAdmissionResult ObservationAdmission::admit(
    const ParticipantRegistry &participants,
    const ParticipantReadinessRegistry &readiness, const char *site_id,
    const ParticipantObservationMetadata &observation,
    const char *expected_group_id) {
  if (observation.participant_id == nullptr || observation.site_id == nullptr ||
      site_id == nullptr || observation.group_id == nullptr ||
      observation.boot_id == nullptr ||
      observation.sequence == 0 || observation.ttl_ms == 0 ||
      observation.ttl_ms > 10000 || observation.track_count > 3 ||
      std::strlen(observation.boot_id) == 0 ||
      std::strlen(observation.boot_id) > MAX_BOOT_ID_LENGTH)
    return ObservationAdmissionResult::INVALID_ARGUMENT;
  const auto *participant = participants.find(observation.participant_id);
  const auto *profile = readiness.find(observation.participant_id);
  return this->admit(participant, profile, site_id, observation,
                     expected_group_id);
}

ObservationAdmissionResult ObservationAdmission::admit(
    const ParticipantRecord *participant,
    const ParticipantReadinessRecord *profile, const char *site_id,
    const ParticipantObservationMetadata &observation,
    const char *expected_group_id) {
  if (observation.participant_id == nullptr || observation.site_id == nullptr ||
      site_id == nullptr || observation.group_id == nullptr ||
      observation.boot_id == nullptr ||
      observation.sequence == 0 || observation.ttl_ms == 0 ||
      observation.ttl_ms > 10000 || observation.track_count > 3 ||
      std::strlen(observation.boot_id) == 0 ||
      std::strlen(observation.boot_id) > MAX_BOOT_ID_LENGTH)
    return ObservationAdmissionResult::INVALID_ARGUMENT;
  if (participant == nullptr)
    return ObservationAdmissionResult::UNKNOWN_PARTICIPANT;
  if (participant->membership != ParticipantMembership::ACTIVE)
    return ObservationAdmissionResult::PARTICIPANT_INACTIVE;
  if (participant->type != observation.participant_type)
    return ObservationAdmissionResult::TYPE_MISMATCH;
  if (!ParticipantReadinessRegistry::ready(profile))
    return ObservationAdmissionResult::PARTICIPANT_INACTIVE;
  if (!equal_text(site_id, observation.site_id))
    return ObservationAdmissionResult::SITE_MISMATCH;
  const char *admission_group_id =
      expected_group_id != nullptr && expected_group_id[0] != '\0'
          ? expected_group_id
          : profile->config.group_id;
  if (!equal_text(admission_group_id, observation.group_id))
    return ObservationAdmissionResult::GROUP_MISMATCH;
  if (observation.sample_age_ms >= observation.ttl_ms)
    return ObservationAdmissionResult::EXPIRED;

  const uint16_t capabilities = profile->config.capability_flags;
  if (observation.track_count > profile->config.max_targets ||
      (observation.track_count > 0 &&
       (capabilities & PARTICIPANT_CAP_POINT_TRACKS) == 0) ||
      (observation.track_count > 0 &&
       observation.has_velocity !=
           ((capabilities & PARTICIPANT_CAP_VELOCITY) != 0)) ||
      (observation.track_count > 0 &&
       observation.has_position_uncertainty !=
           ((capabilities & PARTICIPANT_CAP_POSITION_UNCERTAINTY) != 0)) ||
      (observation.presence && (capabilities & PARTICIPANT_CAP_PRESENCE) == 0) ||
      (observation.motion && (capabilities & PARTICIPANT_CAP_MOTION) == 0) ||
      (observation.has_range && (capabilities & PARTICIPANT_CAP_RANGE) == 0) ||
      (observation.has_zone_presence &&
       (capabilities & PARTICIPANT_CAP_ZONE_PRESENCE) == 0))
    return ObservationAdmissionResult::CAPABILITY_VIOLATION;
  if ((capabilities & PARTICIPANT_CAP_POINT_TRACKS) != 0 &&
      (observation.pose_version != profile->config.pose_version ||
       observation.calibration_version != profile->config.calibration_version))
    return ObservationAdmissionResult::CONFIG_VERSION_MISMATCH;

  SourceState *source = this->source_for_(observation.participant_id);
  if (source == nullptr)
    return ObservationAdmissionResult::INVALID_ARGUMENT;
  if (source->occupied && equal_text(source->boot_id, observation.boot_id) &&
      observation.sequence <= source->sequence)
    return ObservationAdmissionResult::DUPLICATE_OR_REORDERED;
  *source = SourceState{};
  source->occupied = true;
  std::strncpy(source->participant_id, observation.participant_id,
               ParticipantRecord::MAX_ID_LENGTH);
  std::strncpy(source->boot_id, observation.boot_id, MAX_BOOT_ID_LENGTH);
  source->sequence = observation.sequence;
  return ObservationAdmissionResult::OK;
}

void ObservationAdmission::clear() {
  for (auto &source : this->sources_)
    source = SourceState{};
}

ObservationAdmission::SourceState *ObservationAdmission::source_for_(
    const char *participant_id) {
  SourceState *empty = nullptr;
  for (auto &source : this->sources_) {
    if (source.occupied && std::strcmp(source.participant_id, participant_id) == 0)
      return &source;
    if (!source.occupied && empty == nullptr)
      empty = &source;
  }
  return empty;
}

const char *participant_readiness_store_result_to_string(
    ParticipantReadinessStoreResult result) {
  switch (result) {
    case ParticipantReadinessStoreResult::EMPTY: return "empty";
    case ParticipantReadinessStoreResult::LOADED: return "loaded";
    case ParticipantReadinessStoreResult::CREATED: return "created";
    case ParticipantReadinessStoreResult::MIGRATED: return "migrated";
    case ParticipantReadinessStoreResult::UPDATED: return "updated";
    case ParticipantReadinessStoreResult::COMMIT_REPAIRED: return "commit_repaired";
    case ParticipantReadinessStoreResult::CORRUPT: return "corrupt";
    case ParticipantReadinessStoreResult::UNSUPPORTED_VERSION: return "unsupported_version";
    case ParticipantReadinessStoreResult::STORAGE_FAILED: return "storage_failed";
    case ParticipantReadinessStoreResult::INVALID_DATA: return "invalid_data";
  }
  return "unknown";
}

const char *observation_admission_result_to_string(
    ObservationAdmissionResult result) {
  switch (result) {
    case ObservationAdmissionResult::OK: return "ok";
    case ObservationAdmissionResult::INVALID_ARGUMENT: return "invalid_argument";
    case ObservationAdmissionResult::UNKNOWN_PARTICIPANT: return "unknown_participant";
    case ObservationAdmissionResult::PARTICIPANT_INACTIVE: return "participant_inactive";
    case ObservationAdmissionResult::TYPE_MISMATCH: return "type_mismatch";
    case ObservationAdmissionResult::SITE_MISMATCH: return "site_mismatch";
    case ObservationAdmissionResult::GROUP_MISMATCH: return "group_mismatch";
    case ObservationAdmissionResult::CAPABILITY_VIOLATION: return "capability_violation";
    case ObservationAdmissionResult::CONFIG_VERSION_MISMATCH:
      return "config_version_mismatch";
    case ObservationAdmissionResult::EXPIRED: return "expired";
    case ObservationAdmissionResult::DUPLICATE_OR_REORDERED:
      return "duplicate_or_reordered";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

constexpr uint32_t READINESS_SLOT_0_KEY = 0x7AD61B3CU;
constexpr uint32_t READINESS_SLOT_1_KEY = 0xC29403A7U;
constexpr uint32_t READINESS_COMMIT_KEY = 0x1EEB7D52U;
static const char *const READINESS_TAG = "participant_readiness";

class LegacyNvsParticipantReadinessBackend
    : public ParticipantReadinessStorageBackend {
 public:
  bool initialize() {
    if (this->initialized_)
      return true;
    if (global_preferences == nullptr)
      return false;
    this->slot_preferences_[0] =
        global_preferences->make_preference<StorageBlob>(READINESS_SLOT_0_KEY, true);
    this->slot_preferences_[1] =
        global_preferences->make_preference<StorageBlob>(READINESS_SLOT_1_KEY, true);
    this->commit_preference_ =
        global_preferences->make_preference<CommitBlob>(READINESS_COMMIT_KEY, true);
    this->initialized_ = true;
    return true;
  }
  bool load_participant_readiness_slot(uint8_t slot, StorageBlob *blob) override {
    return slot < 2 && this->slot_preferences_[slot].load(blob);
  }
  bool save_participant_readiness_slot(uint8_t slot,
                                       const StorageBlob &blob) override {
    return slot < 2 && this->slot_preferences_[slot].save(&blob);
  }
  bool load_participant_readiness_commit(CommitBlob *blob) override {
    return this->commit_preference_.load(blob);
  }
  bool save_participant_readiness_commit(const CommitBlob &blob) override {
    return this->commit_preference_.save(&blob);
  }
  bool sync() override { return global_preferences->sync(); }

 private:
  bool initialized_{false};
  ESPPreferenceObject slot_preferences_[2]{};
  ESPPreferenceObject commit_preference_{};
};

class RadarStorageParticipantReadinessBackend
    : public ParticipantReadinessStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return this->storage_ != nullptr && this->storage_->ensure_partition();
  }
  bool load_participant_readiness_slot(uint8_t slot, StorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_READINESS_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_READINESS_SLOT_1;
    return this->storage_->read_platform_blob(target, blob->data(), blob->size());
  }
  bool save_participant_readiness_slot(uint8_t slot,
                                       const StorageBlob &blob) override {
    if (slot >= 2)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_READINESS_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_READINESS_SLOT_1;
    return this->storage_->write_platform_blob(target, blob.data(), blob.size());
  }
  bool load_participant_readiness_commit(CommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob(
                                  RadarPlatformBlob::PARTICIPANT_READINESS_COMMIT,
                                  blob->data(), blob->size());
  }
  bool save_participant_readiness_commit(const CommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::PARTICIPANT_READINESS_COMMIT, blob.data(), blob.size());
  }
  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32ParticipantReadinessPersistence {
  RadarStorageParticipantReadinessBackend backend{};
  ParticipantReadinessStore *store{nullptr};
  bool store_allocation_attempted{false};

  bool ensure_store() {
    if (this->store != nullptr)
      return true;
    if (this->store_allocation_attempted)
      return false;
    this->store_allocation_attempted = true;

#ifdef USE_PSRAM
    void *memory =
        heap_caps_calloc(1, sizeof(ParticipantReadinessStore),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      ESP_LOGE(READINESS_TAG,
               "PSRAM allocation failed for readiness store workspace "
               "size=%u; remote readiness remains disabled",
               static_cast<unsigned>(sizeof(ParticipantReadinessStore)));
      return false;
    }
    this->store = new (memory) ParticipantReadinessStore();
    ESP_LOGI(READINESS_TAG,
             "Readiness store workspace allocated in PSRAM size=%u",
             static_cast<unsigned>(sizeof(ParticipantReadinessStore)));
#else
    this->store = new (std::nothrow) ParticipantReadinessStore();
    if (this->store == nullptr) {
      ESP_LOGE(READINESS_TAG,
               "Internal allocation failed for readiness store workspace "
               "size=%u; remote readiness remains disabled",
               static_cast<unsigned>(sizeof(ParticipantReadinessStore)));
      return false;
    }
    ESP_LOGW(READINESS_TAG,
             "Readiness store workspace allocated in internal RAM size=%u "
             "because PSRAM is not enabled",
             static_cast<unsigned>(sizeof(ParticipantReadinessStore)));
#endif
    return true;
  }
};

static_assert(sizeof(Esp32ParticipantReadinessPersistence) <= 32,
              "readiness persistence shell must remain small in internal RAM");

Esp32ParticipantReadinessPersistence &participant_readiness_persistence() {
  static Esp32ParticipantReadinessPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_participant_readiness_persistence(
    ParticipantReadinessStorageBackend **backend, ParticipantReadinessStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = participant_readiness_persistence();
  if (!persistence.ensure_store()) {
    *backend = nullptr;
    *store = nullptr;
    return false;
  }
  *backend = &persistence.backend;
  *store = persistence.store;
  return true;
}

ParticipantReadinessStoreResult initialize_esp32_participant_readiness(
    RadarStorage *storage, ParticipantReadinessSnapshot *snapshot) {
  if (storage == nullptr || snapshot == nullptr)
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  auto &persistence = participant_readiness_persistence();
  if (!persistence.backend.initialize(storage))
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  ParticipantReadinessStorageBackend *backend = nullptr;
  ParticipantReadinessStore *store = nullptr;
  if (!get_esp32_participant_readiness_persistence(&backend, &store))
    return ParticipantReadinessStoreResult::STORAGE_FAILED;
  static LegacyNvsParticipantReadinessBackend legacy_backend;
  ParticipantReadinessStorageBackend *legacy = nullptr;
  if (global_preferences != nullptr && legacy_backend.initialize())
    legacy = &legacy_backend;
  const auto result = store->load_or_migrate(*backend, legacy, snapshot);
  if (result == ParticipantReadinessStoreResult::LOADED ||
      result == ParticipantReadinessStoreResult::CREATED ||
      result == ParticipantReadinessStoreResult::MIGRATED ||
      result == ParticipantReadinessStoreResult::COMMIT_REPAIRED) {
    const uint32_t keys[] = {READINESS_SLOT_0_KEY, READINESS_SLOT_1_KEY,
                             READINESS_COMMIT_KEY};
    erase_esp32_legacy_preference_keys(keys, 3);
  }
  return result;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
