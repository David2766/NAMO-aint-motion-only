#include "participant_credentials.h"

#include <algorithm>
#include <cstring>

#ifdef USE_ESP32
#include "radar_storage.h"
#endif

namespace esphome {
namespace radar_api_server {

namespace {

constexpr uint32_t CREDENTIAL_MAGIC = 0x4E504352U;  // NPCR
constexpr uint32_t COMMIT_MAGIC = 0x4E504343U;      // NPCC

bool valid_id(const char *value) {
  if (value == nullptr)
    return false;
  const std::size_t length = std::strlen(value);
  if (length == 0 || length > ParticipantRecord::MAX_ID_LENGTH)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const char character = value[index];
    const bool alphanumeric =
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9');
    if ((!alphanumeric && character != '-' && character != '_' &&
         character != '.') ||
        (index == 0 && !alphanumeric))
      return false;
  }
  return true;
}

uint32_t checksum(const uint8_t *data, std::size_t size) {
  uint32_t value = 2166136261U;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= data[index];
    value *= 16777619U;
  }
  return value;
}

void write_u16(uint8_t *target, uint16_t value) {
  target[0] = static_cast<uint8_t>(value);
  target[1] = static_cast<uint8_t>(value >> 8U);
}

void write_u32(uint8_t *target, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index)
    target[index] = static_cast<uint8_t>(value >> (index * 8U));
}

void write_u64(uint8_t *target, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index)
    target[index] = static_cast<uint8_t>(value >> (index * 8U));
}

uint16_t read_u16(const uint8_t *source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(source[1] << 8U);
}

uint32_t read_u32(const uint8_t *source) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index)
    value |= static_cast<uint32_t>(source[index]) << (index * 8U);
  return value;
}

uint64_t read_u64(const uint8_t *source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index)
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  return value;
}

void encode_commit(uint8_t slot, uint32_t generation,
                   ParticipantCredentialCommitBlob *blob) {
  blob->bytes.fill(0);
  write_u32(blob->bytes.data(), COMMIT_MAGIC);
  write_u16(blob->bytes.data() + 4,
            ParticipantCredentialCodec::STORAGE_VERSION);
  blob->bytes[6] = slot;
  write_u32(blob->bytes.data() + 8, generation);
  write_u32(blob->bytes.data() + 12,
            checksum(blob->bytes.data(), 12));
}

bool decode_commit(const ParticipantCredentialCommitBlob &blob, uint8_t *slot,
                   uint32_t *generation) {
  if (slot == nullptr || generation == nullptr ||
      read_u32(blob.bytes.data()) != COMMIT_MAGIC ||
      read_u16(blob.bytes.data() + 4) !=
          ParticipantCredentialCodec::STORAGE_VERSION ||
      blob.bytes[6] > 1 || read_u32(blob.bytes.data() + 12) !=
                               checksum(blob.bytes.data(), 12))
    return false;
  *slot = blob.bytes[6];
  *generation = read_u32(blob.bytes.data() + 8);
  return *generation != 0;
}

}  // namespace

ParticipantCredentialResult ParticipantCredentialRegistry::set(
    const char *participant_id, uint64_t participant_token,
    uint32_t key_version, const uint8_t *key, bool enabled) {
  if (!valid_id(participant_id) || participant_token == 0 ||
      key_version == 0 || key == nullptr)
    return ParticipantCredentialResult::INVALID_ARGUMENT;

  ParticipantCredentialRecord *target = nullptr;
  for (auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0) {
      target = &entry;
      continue;
    }
    if (entry.occupied && entry.participant_token == participant_token)
      return ParticipantCredentialResult::TOKEN_COLLISION;
    if (!entry.occupied && target == nullptr)
      target = &entry;
  }
  if (target == nullptr)
    return ParticipantCredentialResult::FULL;

  *target = ParticipantCredentialRecord{};
  target->occupied = true;
  target->enabled = enabled;
  std::strncpy(target->participant_id, participant_id,
               ParticipantRecord::MAX_ID_LENGTH);
  target->participant_token = participant_token;
  target->key_version = key_version;
  std::memcpy(target->key.data(), key, target->key.size());
  return ParticipantCredentialResult::OK;
}

ParticipantCredentialResult ParticipantCredentialRegistry::set_enabled(
    const char *participant_id, bool enabled) {
  if (!valid_id(participant_id))
    return ParticipantCredentialResult::INVALID_ARGUMENT;
  for (auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0) {
      entry.enabled = enabled;
      return ParticipantCredentialResult::OK;
    }
  }
  return ParticipantCredentialResult::NOT_FOUND;
}

ParticipantCredentialResult ParticipantCredentialRegistry::erase(
    const char *participant_id) {
  if (!valid_id(participant_id))
    return ParticipantCredentialResult::INVALID_ARGUMENT;
  for (auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0) {
      entry = ParticipantCredentialRecord{};
      return ParticipantCredentialResult::OK;
    }
  }
  return ParticipantCredentialResult::NOT_FOUND;
}

void ParticipantCredentialRegistry::clear() {
  for (auto &entry : this->entries_)
    entry = ParticipantCredentialRecord{};
}

const ParticipantCredentialRecord *ParticipantCredentialRegistry::find(
    const char *participant_id) const {
  if (!valid_id(participant_id))
    return nullptr;
  for (const auto &entry : this->entries_) {
    if (entry.occupied &&
        std::strcmp(entry.participant_id, participant_id) == 0)
      return &entry;
  }
  return nullptr;
}

const ParticipantCredentialRecord *ParticipantCredentialRegistry::resolve(
    uint64_t participant_token, uint32_t key_version) const {
  for (const auto &entry : this->entries_) {
    if (entry.occupied && entry.enabled &&
        entry.participant_token == participant_token &&
        entry.key_version == key_version)
      return &entry;
  }
  return nullptr;
}

const ParticipantCredentialRecord *
ParticipantCredentialRegistry::resolve_provisioned(
    uint64_t participant_token, uint32_t key_version) const {
  for (const auto &entry : this->entries_) {
    if (entry.occupied && entry.participant_token == participant_token &&
        entry.key_version == key_version)
      return &entry;
  }
  return nullptr;
}

const ParticipantCredentialRecord *ParticipantCredentialRegistry::record_at(
    std::size_t slot) const {
  if (slot >= this->entries_.size() || !this->entries_[slot].occupied)
    return nullptr;
  return &this->entries_[slot];
}

std::size_t ParticipantCredentialRegistry::size() const {
  std::size_t count = 0;
  for (const auto &entry : this->entries_) {
    if (entry.occupied)
      ++count;
  }
  return count;
}

ParticipantCredentialCodecResult ParticipantCredentialCodec::encode(
    const ParticipantCredentialSnapshot &snapshot, uint8_t *output,
    std::size_t output_capacity, std::size_t *output_size) {
  return encode_registry(snapshot.registry, snapshot.generation, output,
                         output_capacity, output_size);
}

ParticipantCredentialCodecResult ParticipantCredentialCodec::encode_registry(
    const ParticipantCredentialRegistry &registry, uint32_t generation,
    uint8_t *output, std::size_t output_capacity, std::size_t *output_size) {
  if (output == nullptr || output_size == nullptr || generation == 0 ||
      output_capacity < MAX_ENCODED_SIZE)
    return ParticipantCredentialCodecResult::INVALID_ARGUMENT;

  std::memset(output, 0, output_capacity);
  write_u32(output, CREDENTIAL_MAGIC);
  write_u16(output + 4, STORAGE_VERSION);
  output[6] = static_cast<uint8_t>(registry.size());
  write_u32(output + 8, generation);
  std::size_t cursor = HEADER_SIZE;
  for (std::size_t slot = 0;
       slot < ParticipantRegistry::MAX_REMOTE_PARTICIPANTS; ++slot) {
    const auto *record = registry.record_at(slot);
    if (record == nullptr)
      continue;
    const std::size_t id_length = std::strlen(record->participant_id);
    if (!valid_id(record->participant_id) || record->participant_token == 0 ||
        record->key_version == 0 || id_length > UINT8_MAX)
      return ParticipantCredentialCodecResult::INVALID_REGISTRY;
    output[cursor] = record->enabled ? 1U : 0U;
    output[cursor + 1] = static_cast<uint8_t>(id_length);
    write_u64(output + cursor + 4, record->participant_token);
    write_u32(output + cursor + 12, record->key_version);
    std::memcpy(output + cursor + 16, record->key.data(), record->key.size());
    std::memcpy(output + cursor + RECORD_HEADER_SIZE, record->participant_id,
                id_length);
    cursor += RECORD_HEADER_SIZE + id_length;
  }
  write_u32(output + cursor, checksum(output, cursor));
  *output_size = cursor + CHECKSUM_SIZE;
  return ParticipantCredentialCodecResult::OK;
}

ParticipantCredentialCodecResult ParticipantCredentialCodec::decode(
    const uint8_t *data, std::size_t size,
    ParticipantCredentialSnapshot *snapshot) {
  if (data == nullptr || snapshot == nullptr)
    return ParticipantCredentialCodecResult::INVALID_ARGUMENT;
  if (size < HEADER_SIZE + CHECKSUM_SIZE)
    return ParticipantCredentialCodecResult::EMPTY;
  if (read_u32(data) != CREDENTIAL_MAGIC)
    return ParticipantCredentialCodecResult::CORRUPT;
  if (read_u16(data + 4) != STORAGE_VERSION)
    return ParticipantCredentialCodecResult::UNSUPPORTED_VERSION;
  const uint8_t count = data[6];
  const uint32_t generation = read_u32(data + 8);
  if (count > ParticipantRegistry::MAX_REMOTE_PARTICIPANTS || generation == 0)
    return ParticipantCredentialCodecResult::CORRUPT;

  snapshot->registry.clear();
  snapshot->generation = generation;
  std::size_t cursor = HEADER_SIZE;
  for (uint8_t index = 0; index < count; ++index) {
    if (cursor + RECORD_HEADER_SIZE + CHECKSUM_SIZE > size)
      return ParticipantCredentialCodecResult::CORRUPT;
    const bool enabled = data[cursor] == 1;
    const uint8_t id_length = data[cursor + 1];
    if (data[cursor] > 1 || id_length == 0 ||
        id_length > ParticipantRecord::MAX_ID_LENGTH ||
        cursor + RECORD_HEADER_SIZE + id_length + CHECKSUM_SIZE > size)
      return ParticipantCredentialCodecResult::CORRUPT;
    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    std::memcpy(participant_id, data + cursor + RECORD_HEADER_SIZE, id_length);
    const auto result = snapshot->registry.set(
        participant_id, read_u64(data + cursor + 4),
        read_u32(data + cursor + 12), data + cursor + 16, enabled);
    if (result != ParticipantCredentialResult::OK)
      return ParticipantCredentialCodecResult::INVALID_REGISTRY;
    cursor += RECORD_HEADER_SIZE + id_length;
  }
  if (cursor + CHECKSUM_SIZE != size ||
      read_u32(data + cursor) != checksum(data, cursor))
    return ParticipantCredentialCodecResult::CORRUPT;
  return ParticipantCredentialCodecResult::OK;
}

ParticipantCredentialStoreResult ParticipantCredentialStore::load_slot_(
    ParticipantCredentialStorageBackend &backend, uint8_t slot,
    ParticipantCredentialSnapshot *snapshot) {
  if (snapshot == nullptr || slot > 1 ||
      !backend.load_participant_credential_slot(slot, &this->blob_))
    return ParticipantCredentialStoreResult::EMPTY;
  const uint32_t encoded_size =
      read_u32(this->blob_.bytes.data() + this->blob_.bytes.size() - 4);
  if (encoded_size < ParticipantCredentialCodec::HEADER_SIZE +
                         ParticipantCredentialCodec::CHECKSUM_SIZE ||
      encoded_size > this->blob_.bytes.size() - 4)
    return ParticipantCredentialStoreResult::CORRUPT;
  const auto result = ParticipantCredentialCodec::decode(
      this->blob_.bytes.data(), encoded_size, snapshot);
  if (result == ParticipantCredentialCodecResult::OK)
    return ParticipantCredentialStoreResult::LOADED;
  snapshot->registry.clear();
  snapshot->generation = 0;
  if (result == ParticipantCredentialCodecResult::UNSUPPORTED_VERSION)
    return ParticipantCredentialStoreResult::UNSUPPORTED_VERSION;
  if (result == ParticipantCredentialCodecResult::INVALID_REGISTRY)
    return ParticipantCredentialStoreResult::INVALID_REGISTRY;
  return ParticipantCredentialStoreResult::CORRUPT;
}

ParticipantCredentialStoreResult ParticipantCredentialStore::write_(
    ParticipantCredentialStorageBackend &backend, uint8_t slot,
    const ParticipantCredentialRegistry &registry, uint32_t generation) {
  this->blob_.bytes.fill(0);
  std::size_t encoded_size = 0;
  const auto encode_result = ParticipantCredentialCodec::encode_registry(
      registry, generation, this->blob_.bytes.data(),
      this->blob_.bytes.size() - 4, &encoded_size);
  if (encode_result != ParticipantCredentialCodecResult::OK)
    return ParticipantCredentialStoreResult::INVALID_REGISTRY;
  write_u32(this->blob_.bytes.data() + this->blob_.bytes.size() - 4,
            static_cast<uint32_t>(encoded_size));
  if (!backend.save_participant_credential_slot(slot, this->blob_) ||
      !backend.sync())
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  ParticipantCredentialCommitBlob commit{};
  encode_commit(slot, generation, &commit);
  if (!backend.save_participant_credential_commit(commit) || !backend.sync())
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  return ParticipantCredentialStoreResult::UPDATED;
}

ParticipantCredentialStoreResult ParticipantCredentialStore::load(
    ParticipantCredentialStorageBackend &backend,
    ParticipantCredentialSnapshot *snapshot) {
  if (snapshot == nullptr)
    return ParticipantCredentialStoreResult::INVALID_REGISTRY;
  ParticipantCredentialCommitBlob commit{};
  uint8_t committed_slot = 0;
  uint32_t committed_generation = 0;
  if (backend.load_participant_credential_commit(&commit) &&
      decode_commit(commit, &committed_slot, &committed_generation)) {
    const auto result = this->load_slot_(backend, committed_slot, snapshot);
    if (result == ParticipantCredentialStoreResult::LOADED &&
        snapshot->generation == committed_generation)
      return result;
  }

  const auto first_result = this->load_slot_(backend, 0, snapshot);
  const bool first_valid = first_result == ParticipantCredentialStoreResult::LOADED;
  const uint32_t first_generation = first_valid ? snapshot->generation : 0;
  const auto second_result = this->load_slot_(backend, 1, snapshot);
  const bool second_valid = second_result == ParticipantCredentialStoreResult::LOADED;
  const uint32_t second_generation = second_valid ? snapshot->generation : 0;
  if (!first_valid && !second_valid)
    return first_result == ParticipantCredentialStoreResult::EMPTY &&
                   second_result == ParticipantCredentialStoreResult::EMPTY
               ? ParticipantCredentialStoreResult::EMPTY
               : ParticipantCredentialStoreResult::CORRUPT;
  const uint8_t repaired_slot =
      second_valid && (!first_valid || second_generation > first_generation) ? 1 : 0;
  if (repaired_slot == 0 &&
      this->load_slot_(backend, 0, snapshot) !=
          ParticipantCredentialStoreResult::LOADED)
    return ParticipantCredentialStoreResult::CORRUPT;
  encode_commit(repaired_slot, snapshot->generation, &commit);
  if (!backend.save_participant_credential_commit(commit) || !backend.sync())
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  return ParticipantCredentialStoreResult::COMMIT_REPAIRED;
}

ParticipantCredentialStoreResult ParticipantCredentialStore::create(
    ParticipantCredentialStorageBackend &backend,
    const ParticipantCredentialRegistry &registry,
    ParticipantCredentialSnapshot *snapshot) {
  const auto result = this->write_(backend, 0, registry, 1);
  if (result != ParticipantCredentialStoreResult::UPDATED)
    return result;
  if (snapshot != nullptr)
    snapshot->registry = registry;
  if (snapshot != nullptr)
    snapshot->generation = 1;
  return ParticipantCredentialStoreResult::CREATED;
}

ParticipantCredentialStoreResult ParticipantCredentialStore::update(
    ParticipantCredentialStorageBackend &backend,
    const ParticipantCredentialRegistry &registry,
    ParticipantCredentialSnapshot *snapshot) {
  if (snapshot == nullptr || snapshot->generation == 0)
    return ParticipantCredentialStoreResult::INVALID_REGISTRY;
  ParticipantCredentialCommitBlob commit{};
  uint8_t active_slot = 0;
  uint32_t generation = 0;
  if (!backend.load_participant_credential_commit(&commit) ||
      !decode_commit(commit, &active_slot, &generation) ||
      generation != snapshot->generation)
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  const uint32_t updated_generation = snapshot->generation + 1;
  if (updated_generation == 0)
    return ParticipantCredentialStoreResult::INVALID_REGISTRY;
  const auto result = this->write_(backend, active_slot == 0 ? 1 : 0,
                                   registry, updated_generation);
  if (result != ParticipantCredentialStoreResult::UPDATED)
    return result;
  snapshot->registry = registry;
  snapshot->generation = updated_generation;
  return ParticipantCredentialStoreResult::UPDATED;
}

const char *participant_credential_store_result_to_string(
    ParticipantCredentialStoreResult result) {
  switch (result) {
    case ParticipantCredentialStoreResult::EMPTY: return "empty";
    case ParticipantCredentialStoreResult::LOADED: return "loaded";
    case ParticipantCredentialStoreResult::CREATED: return "created";
    case ParticipantCredentialStoreResult::UPDATED: return "updated";
    case ParticipantCredentialStoreResult::COMMIT_REPAIRED:
      return "commit_repaired";
    case ParticipantCredentialStoreResult::CORRUPT: return "corrupt";
    case ParticipantCredentialStoreResult::UNSUPPORTED_VERSION:
      return "unsupported_version";
    case ParticipantCredentialStoreResult::STORAGE_FAILED:
      return "storage_failed";
    case ParticipantCredentialStoreResult::INVALID_REGISTRY:
      return "invalid_registry";
  }
  return "unknown";
}

#ifdef USE_ESP32

namespace {

class RadarStorageParticipantCredentialBackend
    : public ParticipantCredentialStorageBackend {
 public:
  bool initialize(RadarStorage *storage) {
    this->storage_ = storage;
    return this->storage_ != nullptr && this->storage_->ensure_partition();
  }

  bool load_participant_credential_slot(
      uint8_t slot, ParticipantCredentialStorageBlob *blob) override {
    if (slot > 1 || blob == nullptr)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_CREDENTIAL_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_CREDENTIAL_SLOT_1;
    return this->storage_->read_platform_blob(target, blob->bytes.data(),
                                              blob->bytes.size());
  }

  bool save_participant_credential_slot(
      uint8_t slot, const ParticipantCredentialStorageBlob &blob) override {
    if (slot > 1)
      return false;
    const auto target = slot == 0
                            ? RadarPlatformBlob::PARTICIPANT_CREDENTIAL_SLOT_0
                            : RadarPlatformBlob::PARTICIPANT_CREDENTIAL_SLOT_1;
    return this->storage_->write_platform_blob(target, blob.bytes.data(),
                                               blob.bytes.size());
  }

  bool load_participant_credential_commit(
      ParticipantCredentialCommitBlob *blob) override {
    return blob != nullptr && this->storage_->read_platform_blob(
                                  RadarPlatformBlob::PARTICIPANT_CREDENTIAL_COMMIT,
                                  blob->bytes.data(), blob->bytes.size());
  }

  bool save_participant_credential_commit(
      const ParticipantCredentialCommitBlob &blob) override {
    return this->storage_->write_platform_blob(
        RadarPlatformBlob::PARTICIPANT_CREDENTIAL_COMMIT, blob.bytes.data(),
        blob.bytes.size());
  }

  bool sync() override { return true; }

 private:
  RadarStorage *storage_{nullptr};
};

struct Esp32ParticipantCredentialPersistence {
  RadarStorageParticipantCredentialBackend backend{};
  ParticipantCredentialStore store{};
};

Esp32ParticipantCredentialPersistence &participant_credential_persistence() {
  static Esp32ParticipantCredentialPersistence persistence;
  return persistence;
}

}  // namespace

bool get_esp32_participant_credential_persistence(
    ParticipantCredentialStorageBackend **backend,
    ParticipantCredentialStore **store) {
  if (backend == nullptr || store == nullptr)
    return false;
  auto &persistence = participant_credential_persistence();
  *backend = &persistence.backend;
  *store = &persistence.store;
  return true;
}

ParticipantCredentialStoreResult initialize_esp32_participant_credentials(
    RadarStorage *storage, ParticipantCredentialSnapshot *snapshot) {
  if (storage == nullptr || snapshot == nullptr)
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  auto &persistence = participant_credential_persistence();
  if (!persistence.backend.initialize(storage))
    return ParticipantCredentialStoreResult::STORAGE_FAILED;
  const auto result = persistence.store.load(persistence.backend, snapshot);
  if (result != ParticipantCredentialStoreResult::EMPTY)
    return result;
  snapshot->registry.clear();
  snapshot->generation = 0;
  return persistence.store.create(persistence.backend, snapshot->registry,
                                  snapshot);
}

#endif

}  // namespace radar_api_server
}  // namespace esphome
