#include "participant_registry_store.h"

#include <cstdio>
#include <cstring>
#include <limits>

#ifdef USE_ESP32
#include "radar_storage.h"
#include "esphome/components/esp32/preferences.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include <new>
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint8_t REGISTRY_MAGIC[] = {'N', 'A', 'M', 'R'};
constexpr uint8_t COMMIT_MAGIC[] = {'N', 'M', 'R', 'C'};
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

bool valid_type(ParticipantType type) {
  return type == ParticipantType::NAMO_NODE || type == ParticipantType::SATELLITE;
}

bool valid_trust(ParticipantTrust trust) {
  return trust == ParticipantTrust::UNKNOWN ||
         trust == ParticipantTrust::OFFICIAL_VERIFIED ||
         trust == ParticipantTrust::COMMUNITY_UNVERIFIED ||
         trust == ParticipantTrust::SATELLITE;
}

bool valid_membership(ParticipantMembership membership) {
  return membership == ParticipantMembership::QUARANTINED ||
         membership == ParticipantMembership::ACTIVE ||
         membership == ParticipantMembership::REVOKED;
}

bool valid_record(const ParticipantRecord &record) {
  if (!record.occupied || !valid_type(record.type) || !valid_trust(record.trust) ||
      !valid_membership(record.membership))
    return false;
  if (record.membership != ParticipantMembership::ACTIVE && record.leader_eligible)
    return false;
  if (record.type == ParticipantType::SATELLITE &&
      (record.trust != ParticipantTrust::UNKNOWN &&
       record.trust != ParticipantTrust::SATELLITE))
    return false;
  if (record.type == ParticipantType::NAMO_NODE &&
      record.trust == ParticipantTrust::SATELLITE)
    return false;
  if (record.membership == ParticipantMembership::ACTIVE &&
      record.trust == ParticipantTrust::UNKNOWN)
    return false;
  if (record.leader_eligible &&
      (record.type != ParticipantType::NAMO_NODE ||
       (record.trust != ParticipantTrust::OFFICIAL_VERIFIED &&
        record.trust != ParticipantTrust::COMMUNITY_UNVERIFIED)))
    return false;
  if (record.membership == ParticipantMembership::ACTIVE &&
      record.type == ParticipantType::NAMO_NODE &&
      record.trust == ParticipantTrust::OFFICIAL_VERIFIED &&
      !record.leader_eligible)
    return false;
  return true;
}

ParticipantRegistryCodecResult restore_record(ParticipantRegistry *registry,
                                              const char *participant_id,
                                              ParticipantType type,
                                              ParticipantTrust trust,
                                              ParticipantMembership membership,
                                              bool leader_eligible) {
  if (registry->discover(participant_id, type) != ParticipantRegistryResult::OK)
    return ParticipantRegistryCodecResult::CORRUPT;
  if (trust != ParticipantTrust::UNKNOWN &&
      registry->classify(participant_id, trust) != ParticipantRegistryResult::OK)
    return ParticipantRegistryCodecResult::CORRUPT;

  if (membership == ParticipantMembership::ACTIVE) {
    if (registry->activate(participant_id) != ParticipantRegistryResult::OK)
      return ParticipantRegistryCodecResult::CORRUPT;
    if (type == ParticipantType::NAMO_NODE &&
        trust == ParticipantTrust::COMMUNITY_UNVERIFIED && leader_eligible &&
        registry->set_community_leader_eligible(participant_id, true) !=
            ParticipantRegistryResult::OK)
      return ParticipantRegistryCodecResult::CORRUPT;
  } else if (membership == ParticipantMembership::REVOKED) {
    if (registry->revoke(participant_id) != ParticipantRegistryResult::OK)
      return ParticipantRegistryCodecResult::CORRUPT;
  }

  const ParticipantRecord *restored = registry->find(participant_id);
  if (restored == nullptr || restored->type != type || restored->trust != trust ||
      restored->membership != membership || restored->leader_eligible != leader_eligible)
    return ParticipantRegistryCodecResult::CORRUPT;
  return ParticipantRegistryCodecResult::OK;
}

void encode_commit(uint8_t slot, uint32_t generation,
                   ParticipantRegistryCommitBlob *blob) {
  blob->bytes.fill(0xFF);
  std::memcpy(blob->bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC));
  write_u16(blob->bytes.data() + 4, COMMIT_VERSION);
  blob->bytes[6] = slot;
  blob->bytes[7] = 0;
  write_u32(blob->bytes.data() + 8, generation);
  write_u32(blob->bytes.data() + 12, fnv1a(blob->bytes.data(), 12));
}

ParticipantRegistryStoreResult decode_commit(const ParticipantRegistryCommitBlob &blob,
                                             uint8_t *slot, uint32_t *generation) {
  if (all_bytes_are(blob.bytes.data(), blob.bytes.size(), 0xFF))
    return ParticipantRegistryStoreResult::EMPTY;
  if (std::memcmp(blob.bytes.data(), COMMIT_MAGIC, sizeof(COMMIT_MAGIC)) != 0)
    return ParticipantRegistryStoreResult::CORRUPT;
  if (read_u16(blob.bytes.data() + 4) != COMMIT_VERSION)
    return ParticipantRegistryStoreResult::UNSUPPORTED_VERSION;
  if (blob.bytes[6] > 1 || blob.bytes[7] != 0 ||
      read_u32(blob.bytes.data() + 8) == 0 ||
      read_u32(blob.bytes.data() + 12) != fnv1a(blob.bytes.data(), 12))
    return ParticipantRegistryStoreResult::CORRUPT;
  *slot = blob.bytes[6];
  *generation = read_u32(blob.bytes.data() + 8);
  return ParticipantRegistryStoreResult::LOADED;
}

}  // namespace

ParticipantRegistryCodecResult ParticipantRegistryCodec::encode(
    const ParticipantRegistrySnapshot &snapshot, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr)
    return ParticipantRegistryCodecResult::INVALID_ARGUMENT;
  if (snapshot.generation == 0 ||
      snapshot.registry.size() > ParticipantRegistry::MAX_REMOTE_PARTICIPANTS)
    return ParticipantRegistryCodecResult::INVALID_REGISTRY;

  std::size_t encoded_size = HEADER_SIZE + CHECKSUM_SIZE;
  for (std::size_t slot = 0; slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const ParticipantRecord *record = snapshot.registry.record_at(slot);
    if (record == nullptr)
      continue;
    if (!valid_record(*record))
      return ParticipantRegistryCodecResult::INVALID_REGISTRY;
    const std::size_t id_length = std::strlen(record->participant_id);
    if (id_length == 0 || id_length > ParticipantRecord::MAX_ID_LENGTH)
      return ParticipantRegistryCodecResult::INVALID_REGISTRY;
    encoded_size += RECORD_HEADER_SIZE + id_length;
  }
  if (encoded_size > output_capacity)
    return ParticipantRegistryCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0xFF, output_capacity);
  std::memcpy(output, REGISTRY_MAGIC, sizeof(REGISTRY_MAGIC));
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(snapshot.registry.size());
  output[7] = 0;
  write_u32(output + 8, snapshot.generation);

  std::size_t offset = HEADER_SIZE;
  for (std::size_t slot = 0; slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const ParticipantRecord *record = snapshot.registry.record_at(slot);
    if (record == nullptr)
      continue;
    const std::size_t id_length = std::strlen(record->participant_id);
    output[offset++] = static_cast<uint8_t>(id_length);
    output[offset++] = static_cast<uint8_t>(record->type);
    output[offset++] = static_cast<uint8_t>(record->trust);
    output[offset++] = static_cast<uint8_t>(record->membership);
    output[offset++] = record->leader_eligible ? 1U : 0U;
    std::memcpy(output + offset, record->participant_id, id_length);
    offset += id_length;
  }
  write_u32(output + offset, fnv1a(output, offset));
  offset += CHECKSUM_SIZE;
  *output_size = offset;
  return ParticipantRegistryCodecResult::OK;
}

ParticipantRegistryCodecResult ParticipantRegistryCodec::decode(
    const uint8_t *data, std::size_t size, ParticipantRegistrySnapshot *snapshot) {
  if (data == nullptr || snapshot == nullptr)
    return ParticipantRegistryCodecResult::INVALID_ARGUMENT;
  if (size == 0 || all_bytes_are(data, size, 0xFF))
    return ParticipantRegistryCodecResult::EMPTY;
  if (size < HEADER_SIZE + CHECKSUM_SIZE ||
      std::memcmp(data, REGISTRY_MAGIC, sizeof(REGISTRY_MAGIC)) != 0)
    return ParticipantRegistryCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return ParticipantRegistryCodecResult::UNSUPPORTED_VERSION;

  const std::size_t record_count = data[6];
  const uint32_t generation = read_u32(data + 8);
  if (record_count > ParticipantRegistry::MAX_REMOTE_PARTICIPANTS || data[7] != 0 ||
      generation == 0)
    return ParticipantRegistryCodecResult::CORRUPT;

  snapshot->registry.clear();
  snapshot->generation = 0;
  std::size_t offset = HEADER_SIZE;
  for (std::size_t index = 0; index < record_count; ++index) {
    if (offset + RECORD_HEADER_SIZE + CHECKSUM_SIZE > size)
      return ParticipantRegistryCodecResult::CORRUPT;
    const std::size_t id_length = data[offset++];
    const auto type = static_cast<ParticipantType>(data[offset++]);
    const auto trust = static_cast<ParticipantTrust>(data[offset++]);
    const auto membership = static_cast<ParticipantMembership>(data[offset++]);
    const uint8_t flags = data[offset++];
    if (id_length == 0 || id_length > ParticipantRecord::MAX_ID_LENGTH ||
        offset + id_length + CHECKSUM_SIZE > size || !valid_type(type) ||
        !valid_trust(trust) || !valid_membership(membership) || flags > 1)
      return ParticipantRegistryCodecResult::CORRUPT;

    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    std::memcpy(participant_id, data + offset, id_length);
    offset += id_length;
    if (restore_record(&snapshot->registry, participant_id, type, trust, membership,
                       flags == 1) != ParticipantRegistryCodecResult::OK)
      return ParticipantRegistryCodecResult::CORRUPT;
  }

  if (offset + CHECKSUM_SIZE > size || read_u32(data + offset) != fnv1a(data, offset))
    return ParticipantRegistryCodecResult::CORRUPT;
  offset += CHECKSUM_SIZE;
  if (offset < size && !all_bytes_are(data + offset, size - offset, 0xFF))
    return ParticipantRegistryCodecResult::CORRUPT;
  if (snapshot->registry.size() != record_count)
    return ParticipantRegistryCodecResult::CORRUPT;

  snapshot->generation = generation;
  return ParticipantRegistryCodecResult::OK;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::load(
    ParticipantRegistryStorageBackend &backend, ParticipantRegistrySnapshot *snapshot) {
  if (snapshot == nullptr)
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  const auto result = this->load_committed_(backend, nullptr);
  if (result == ParticipantRegistryStoreResult::LOADED ||
      result == ParticipantRegistryStoreResult::COMMIT_REPAIRED)
    *snapshot = this->scratch_;
  return result;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::create(
    ParticipantRegistryStorageBackend &backend, const ParticipantRegistry &registry,
    ParticipantRegistrySnapshot *snapshot) {
  this->scratch_.registry = registry;
  this->scratch_.generation = 1;
  const auto write_result = this->write_slot_(backend, 0);
  if (write_result != ParticipantRegistryStoreResult::UPDATED)
    return write_result;
  ParticipantRegistryCommitBlob commit{};
  encode_commit(0, this->scratch_.generation, &commit);
  if (!backend.save_participant_registry_commit(commit) || !backend.sync())
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return ParticipantRegistryStoreResult::CREATED;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::update(
    ParticipantRegistryStorageBackend &backend, const ParticipantRegistry &registry,
    ParticipantRegistrySnapshot *snapshot) {
  uint8_t active_slot = 0;
  const auto load_result = this->load_committed_(backend, &active_slot);
  if (load_result != ParticipantRegistryStoreResult::LOADED &&
      load_result != ParticipantRegistryStoreResult::COMMIT_REPAIRED)
    return load_result;
  if (this->scratch_.generation == std::numeric_limits<uint32_t>::max())
    return ParticipantRegistryStoreResult::INVALID_REGISTRY;

  this->scratch_.registry = registry;
  ++this->scratch_.generation;
  const uint8_t next_slot = active_slot == 0 ? 1 : 0;
  const auto write_result = this->write_slot_(backend, next_slot);
  if (write_result != ParticipantRegistryStoreResult::UPDATED)
    return write_result;
  ParticipantRegistryCommitBlob commit{};
  encode_commit(next_slot, this->scratch_.generation, &commit);
  if (!backend.save_participant_registry_commit(commit) || !backend.sync())
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  if (snapshot != nullptr)
    *snapshot = this->scratch_;
  return ParticipantRegistryStoreResult::UPDATED;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::load_or_migrate(
    ParticipantRegistryStorageBackend &backend,
    ParticipantRegistryStorageBackend *legacy_backend,
    ParticipantRegistrySnapshot *snapshot) {
  const auto current_result = this->load(backend, snapshot);
  if (current_result != ParticipantRegistryStoreResult::EMPTY)
    return current_result;

  if (legacy_backend != nullptr) {
    const auto legacy_result = this->load(*legacy_backend, snapshot);
    if (legacy_result == ParticipantRegistryStoreResult::LOADED ||
        legacy_result == ParticipantRegistryStoreResult::COMMIT_REPAIRED) {
      const auto create_result = this->create(backend, snapshot->registry, snapshot);
      return create_result == ParticipantRegistryStoreResult::CREATED
                 ? ParticipantRegistryStoreResult::MIGRATED
                 : create_result;
    }
    if (legacy_result != ParticipantRegistryStoreResult::EMPTY)
      return legacy_result;
  }

  snapshot->registry.clear();
  snapshot->generation = 0;
  return this->create(backend, snapshot->registry, snapshot);
}

ParticipantRegistryStoreResult ParticipantRegistryStore::write_slot_(
    ParticipantRegistryStorageBackend &backend, uint8_t slot) {
  std::size_t encoded_size = 0;
  const auto encode_result = ParticipantRegistryCodec::encode(
      this->scratch_, this->blob_.bytes.data(), this->blob_.bytes.size(), &encoded_size);
  if (encode_result != ParticipantRegistryCodecResult::OK || encoded_size == 0)
    return ParticipantRegistryStoreResult::INVALID_REGISTRY;
  if (!backend.save_participant_registry_slot(slot, this->blob_) || !backend.sync())
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  return ParticipantRegistryStoreResult::UPDATED;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::decode_slot_() {
  const auto result = ParticipantRegistryCodec::decode(
      this->blob_.bytes.data(), this->blob_.bytes.size(), &this->scratch_);
  if (result == ParticipantRegistryCodecResult::OK)
    return ParticipantRegistryStoreResult::LOADED;
  if (result == ParticipantRegistryCodecResult::EMPTY)
    return ParticipantRegistryStoreResult::EMPTY;
  if (result == ParticipantRegistryCodecResult::UNSUPPORTED_VERSION)
    return ParticipantRegistryStoreResult::UNSUPPORTED_VERSION;
  if (result == ParticipantRegistryCodecResult::INVALID_REGISTRY)
    return ParticipantRegistryStoreResult::INVALID_REGISTRY;
  return ParticipantRegistryStoreResult::CORRUPT;
}

ParticipantRegistryStoreResult ParticipantRegistryStore::load_committed_(
    ParticipantRegistryStorageBackend &backend, uint8_t *active_slot) {
  ParticipantRegistryCommitBlob commit{};
  if (backend.load_participant_registry_commit(&commit)) {
    uint8_t slot = 0;
    uint32_t generation = 0;
    const auto commit_result = decode_commit(commit, &slot, &generation);
    if (commit_result != ParticipantRegistryStoreResult::LOADED)
      return commit_result == ParticipantRegistryStoreResult::EMPTY
                 ? ParticipantRegistryStoreResult::CORRUPT
                 : commit_result;
    if (!backend.load_participant_registry_slot(slot, &this->blob_))
      return ParticipantRegistryStoreResult::CORRUPT;
    const auto slot_result = this->decode_slot_();
    if (slot_result != ParticipantRegistryStoreResult::LOADED)
      return slot_result == ParticipantRegistryStoreResult::EMPTY
                 ? ParticipantRegistryStoreResult::CORRUPT
                 : slot_result;
    if (this->scratch_.generation != generation)
      return ParticipantRegistryStoreResult::CORRUPT;
    if (active_slot != nullptr)
      *active_slot = slot;
    return ParticipantRegistryStoreResult::LOADED;
  }

  bool any_present = false;
  int valid_slot = -1;
  uint32_t valid_generation = 0;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (!backend.load_participant_registry_slot(slot, &this->blob_))
      continue;
    any_present = true;
    const auto slot_result = this->decode_slot_();
    if (slot_result == ParticipantRegistryStoreResult::UNSUPPORTED_VERSION)
      return slot_result;
    if (slot_result != ParticipantRegistryStoreResult::LOADED)
      continue;
    if (valid_slot >= 0)
      return ParticipantRegistryStoreResult::CORRUPT;
    valid_slot = slot;
    valid_generation = this->scratch_.generation;
  }
  if (!any_present)
    return ParticipantRegistryStoreResult::EMPTY;
  if (valid_slot < 0)
    return ParticipantRegistryStoreResult::CORRUPT;

  // The scan reuses one bounded workspace. Reload the sole valid slot before
  // publishing it instead of keeping two registry snapshots on loopTask's stack.
  if (!backend.load_participant_registry_slot(static_cast<uint8_t>(valid_slot),
                                                &this->blob_) ||
      this->decode_slot_() != ParticipantRegistryStoreResult::LOADED ||
      this->scratch_.generation != valid_generation)
    return ParticipantRegistryStoreResult::CORRUPT;

  ParticipantRegistryCommitBlob repaired{};
  encode_commit(static_cast<uint8_t>(valid_slot), valid_generation, &repaired);
  if (!backend.save_participant_registry_commit(repaired) || !backend.sync())
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  if (active_slot != nullptr)
    *active_slot = static_cast<uint8_t>(valid_slot);
  return ParticipantRegistryStoreResult::COMMIT_REPAIRED;
}

const char *participant_registry_store_result_to_string(
    ParticipantRegistryStoreResult result) {
  switch (result) {
    case ParticipantRegistryStoreResult::EMPTY:
      return "empty";
    case ParticipantRegistryStoreResult::LOADED:
      return "loaded";
    case ParticipantRegistryStoreResult::CREATED:
      return "created";
    case ParticipantRegistryStoreResult::MIGRATED:
      return "migrated";
    case ParticipantRegistryStoreResult::UPDATED:
      return "updated";
    case ParticipantRegistryStoreResult::COMMIT_REPAIRED:
      return "commit_repaired";
    case ParticipantRegistryStoreResult::CORRUPT:
      return "corrupt";
    case ParticipantRegistryStoreResult::UNSUPPORTED_VERSION:
      return "unsupported_version";
    case ParticipantRegistryStoreResult::STORAGE_FAILED:
      return "storage_failed";
    case ParticipantRegistryStoreResult::INVALID_REGISTRY:
      return "invalid_registry";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

constexpr uint32_t REGISTRY_SLOT_0_KEY = 0x0D82410CU;
constexpr uint32_t REGISTRY_SLOT_1_KEY = 0x78E0E19AU;
constexpr uint32_t REGISTRY_COMMIT_KEY = 0xDB1FCB75U;

class LegacyNvsParticipantRegistryBackend : public ParticipantRegistryStorageBackend {
 public:
  bool initialize() {
    if (this->initialized_)
      return true;
    if (global_preferences == nullptr)
      return false;
    this->slot_preferences_[0] =
        global_preferences->make_preference<ParticipantRegistryStorageBlob>(
            REGISTRY_SLOT_0_KEY, true);
    this->slot_preferences_[1] =
        global_preferences->make_preference<ParticipantRegistryStorageBlob>(
            REGISTRY_SLOT_1_KEY, true);
    this->commit_preference_ =
        global_preferences->make_preference<ParticipantRegistryCommitBlob>(
            REGISTRY_COMMIT_KEY, true);
    this->initialized_ = true;
    return true;
  }

  bool load_participant_registry_slot(uint8_t slot,
                                      ParticipantRegistryStorageBlob *blob) override {
    return slot < 2 && this->slot_preferences_[slot].load(blob);
  }
  bool save_participant_registry_slot(
      uint8_t slot, const ParticipantRegistryStorageBlob &blob) override {
    return slot < 2 && this->slot_preferences_[slot].save(&blob);
  }
  bool load_participant_registry_commit(ParticipantRegistryCommitBlob *blob) override {
    return this->commit_preference_.load(blob);
  }
  bool save_participant_registry_commit(
      const ParticipantRegistryCommitBlob &blob) override {
    return this->commit_preference_.save(&blob);
  }
  bool sync() override { return global_preferences->sync(); }

 private:
  bool initialized_{false};
  ESPPreferenceObject slot_preferences_[2]{};
  ESPPreferenceObject commit_preference_{};
};

class RadarStorageParticipantRegistryBackend
    : public ParticipantRegistryStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return this->storage_ != nullptr && this->storage_->ensure_partition();
  }

  bool load_participant_registry_slot(
      uint8_t slot, ParticipantRegistryStorageBlob *blob) override {
    if (slot >= 2 || blob == nullptr)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_1;
    return this->storage_->read_platform_blob(
        target, blob->bytes.data(), blob->bytes.size());
  }
  bool save_participant_registry_slot(
      uint8_t slot, const ParticipantRegistryStorageBlob &blob) override {
    if (slot >= 2)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_REGISTRY_SLOT_1;
    return this->storage_->write_platform_blob(
        target, blob.bytes.data(), blob.bytes.size());
  }
  bool load_participant_registry_commit(
      ParticipantRegistryCommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob(
                                  RadarPlatformBlob::PARTICIPANT_REGISTRY_COMMIT,
                                  blob->bytes.data(), blob->bytes.size());
  }
  bool save_participant_registry_commit(
      const ParticipantRegistryCommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::PARTICIPANT_REGISTRY_COMMIT, blob.bytes.data(),
        blob.bytes.size());
  }
  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32ParticipantRegistryPersistence {
  RadarStorageParticipantRegistryBackend backend{};
  ParticipantRegistryStore *store{nullptr};
  bool store_allocation_attempted{false};

  bool ensure_store() {
    if (this->store != nullptr)
      return true;
    if (this->store_allocation_attempted)
      return false;
    this->store_allocation_attempted = true;

#ifdef USE_PSRAM
    void *memory =
        heap_caps_calloc(1, sizeof(ParticipantRegistryStore),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      ESP_LOGE("participant_registry",
               "Store allocation component=participant_registry region="
               "psram_failed size=%u internal_free=%u internal_largest=%u "
               "psram_free=%u psram_largest=%u",
               static_cast<unsigned>(sizeof(ParticipantRegistryStore)),
               static_cast<unsigned>(heap_caps_get_free_size(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
               static_cast<unsigned>(
                   heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
      return false;
    }
    this->store = new (memory) ParticipantRegistryStore();
    ESP_LOGI("participant_registry",
             "Store allocation component=participant_registry region=psram "
             "size=%u internal_free=%u psram_free=%u",
             static_cast<unsigned>(sizeof(ParticipantRegistryStore)),
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#else
    this->store = new (std::nothrow) ParticipantRegistryStore();
    if (this->store == nullptr) {
      ESP_LOGE("participant_registry",
               "Store allocation component=participant_registry region="
               "allocation_failed size=%u",
               static_cast<unsigned>(sizeof(ParticipantRegistryStore)));
      return false;
    }
    ESP_LOGW("participant_registry",
             "Store allocation component=participant_registry "
             "region=internal size=%u reason=psram_disabled",
             static_cast<unsigned>(sizeof(ParticipantRegistryStore)));
#endif
    return true;
  }
};

static_assert(sizeof(Esp32ParticipantRegistryPersistence) <= 32,
              "participant registry persistence shell must remain small");

Esp32ParticipantRegistryPersistence &participant_registry_persistence() {
  static Esp32ParticipantRegistryPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_participant_registry_persistence(
    ParticipantRegistryStorageBackend **backend, ParticipantRegistryStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = participant_registry_persistence();
  if (!persistence.ensure_store()) {
    *backend = nullptr;
    *store = nullptr;
    return false;
  }
  *backend = &persistence.backend;
  *store = persistence.store;
  return true;
}

ParticipantRegistryStoreResult initialize_esp32_participant_registry(
    RadarStorage *storage, ParticipantRegistrySnapshot *snapshot) {
  if (storage == nullptr || snapshot == nullptr)
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  auto &persistence = participant_registry_persistence();
  if (!persistence.backend.initialize(storage))
    return ParticipantRegistryStoreResult::STORAGE_FAILED;
  ParticipantRegistryStorageBackend *backend = nullptr;
  ParticipantRegistryStore *store = nullptr;
  if (!get_esp32_participant_registry_persistence(&backend, &store))
    return ParticipantRegistryStoreResult::STORAGE_FAILED;

  static LegacyNvsParticipantRegistryBackend legacy_backend;
  ParticipantRegistryStorageBackend *legacy = nullptr;
  if (global_preferences != nullptr && legacy_backend.initialize())
    legacy = &legacy_backend;
  const auto result = store->load_or_migrate(*backend, legacy, snapshot);
  if (result == ParticipantRegistryStoreResult::LOADED ||
      result == ParticipantRegistryStoreResult::CREATED ||
      result == ParticipantRegistryStoreResult::MIGRATED ||
      result == ParticipantRegistryStoreResult::COMMIT_REPAIRED) {
    const uint32_t keys[] = {REGISTRY_SLOT_0_KEY, REGISTRY_SLOT_1_KEY,
                             REGISTRY_COMMIT_KEY};
    erase_esp32_legacy_preference_keys(keys, 3);
  }
  return result;
}

bool erase_esp32_legacy_preference_keys(const uint32_t *keys,
                                        std::size_t count) {
  if (keys == nullptr || global_preferences == nullptr ||
      global_preferences->nvs_handle == 0)
    return false;
  bool changed = false;
  for (std::size_t index = 0; index < count; ++index) {
    char key[11];
    std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(keys[index]));
    size_t length = 0;
    const esp_err_t read_result =
        nvs_get_blob(global_preferences->nvs_handle, key, nullptr, &length);
    if (read_result == ESP_ERR_NVS_NOT_FOUND)
      continue;
    if (read_result != ESP_OK ||
        nvs_erase_key(global_preferences->nvs_handle, key) != ESP_OK)
      return false;
    changed = true;
  }
  return !changed || nvs_commit(global_preferences->nvs_handle) == ESP_OK;
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
