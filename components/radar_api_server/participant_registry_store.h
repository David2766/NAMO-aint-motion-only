#pragma once

#include "participant_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class RadarStorage;

struct ParticipantRegistrySnapshot {
  ParticipantRegistry registry{};
  uint32_t generation{0};
};

enum class ParticipantRegistryCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_REGISTRY,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class ParticipantRegistryCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t HEADER_SIZE = 12;
  static constexpr std::size_t RECORD_HEADER_SIZE = 5;
  static constexpr std::size_t CHECKSUM_SIZE = 4;
  static constexpr std::size_t MAX_ENCODED_SIZE =
      HEADER_SIZE + ParticipantRegistry::MAX_REMOTE_PARTICIPANTS *
                        (RECORD_HEADER_SIZE + ParticipantRecord::MAX_ID_LENGTH) +
      CHECKSUM_SIZE;

  static ParticipantRegistryCodecResult encode(const ParticipantRegistrySnapshot &snapshot,
                                                uint8_t *output,
                                                std::size_t output_capacity,
                                                std::size_t *output_size);
  static ParticipantRegistryCodecResult decode(const uint8_t *data, std::size_t size,
                                                ParticipantRegistrySnapshot *snapshot);
};

struct ParticipantRegistryStorageBlob {
  std::array<uint8_t, ParticipantRegistryCodec::MAX_ENCODED_SIZE> bytes{};
};

struct ParticipantRegistryCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

enum class ParticipantRegistryStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  MIGRATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_REGISTRY,
};

class ParticipantRegistryStorageBackend {
 public:
  virtual ~ParticipantRegistryStorageBackend() = default;
  virtual bool load_participant_registry_slot(uint8_t slot,
                                              ParticipantRegistryStorageBlob *blob) = 0;
  virtual bool save_participant_registry_slot(
      uint8_t slot, const ParticipantRegistryStorageBlob &blob) = 0;
  virtual bool load_participant_registry_commit(ParticipantRegistryCommitBlob *blob) = 0;
  virtual bool save_participant_registry_commit(
      const ParticipantRegistryCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class ParticipantRegistryStore {
 public:
  ParticipantRegistryStoreResult load(ParticipantRegistryStorageBackend &backend,
                                      ParticipantRegistrySnapshot *snapshot);
  ParticipantRegistryStoreResult create(ParticipantRegistryStorageBackend &backend,
                                        const ParticipantRegistry &registry,
                                        ParticipantRegistrySnapshot *snapshot = nullptr);
  ParticipantRegistryStoreResult update(ParticipantRegistryStorageBackend &backend,
                                        const ParticipantRegistry &registry,
                                        ParticipantRegistrySnapshot *snapshot = nullptr);
  ParticipantRegistryStoreResult load_or_migrate(
      ParticipantRegistryStorageBackend &backend,
      ParticipantRegistryStorageBackend *legacy_backend,
      ParticipantRegistrySnapshot *snapshot);

 private:
  ParticipantRegistryStorageBlob blob_{};
  ParticipantRegistrySnapshot scratch_{};

  ParticipantRegistryStoreResult write_slot_(ParticipantRegistryStorageBackend &backend,
                                              uint8_t slot);
  ParticipantRegistryStoreResult decode_slot_();
  ParticipantRegistryStoreResult load_committed_(ParticipantRegistryStorageBackend &backend,
                                                  uint8_t *active_slot);
};

const char *participant_registry_store_result_to_string(ParticipantRegistryStoreResult result);

#ifdef USE_ESP32
ParticipantRegistryStoreResult initialize_esp32_participant_registry(
    RadarStorage *storage, ParticipantRegistrySnapshot *snapshot);
bool get_esp32_participant_registry_persistence(
    ParticipantRegistryStorageBackend **backend, ParticipantRegistryStore **store);
bool erase_esp32_legacy_preference_keys(const uint32_t *keys, std::size_t count);
#endif

static_assert(ParticipantRegistryCodec::MAX_ENCODED_SIZE == 1327,
              "participant registry storage bound changed unexpectedly");
static_assert(std::is_trivially_copyable<ParticipantRegistrySnapshot>::value,
              "registry snapshot must not own heap-backed state");
static_assert(sizeof(ParticipantRegistrySnapshot) <= 2056,
              "registry snapshot runtime memory exceeded the embedded budget");
static_assert(sizeof(ParticipantRegistryStore) <= 3072,
              "registry store workspace exceeded the embedded memory budget");
static_assert(std::is_trivially_copyable<ParticipantRegistryStorageBlob>::value,
              "registry preference storage must remain trivially copyable");

}  // namespace radar_api_server
}  // namespace esphome
