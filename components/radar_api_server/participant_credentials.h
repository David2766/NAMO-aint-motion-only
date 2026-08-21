#pragma once

#include "participant_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class RadarStorage;

struct ParticipantCredentialRecord {
  bool occupied{false};
  bool enabled{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint64_t participant_token{0};
  uint32_t key_version{0};
  std::array<uint8_t, 32> key{};
};

enum class ParticipantCredentialResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  TOKEN_COLLISION,
  NOT_FOUND,
  FULL,
};

class ParticipantCredentialRegistry {
 public:
  ParticipantCredentialResult set(const char *participant_id,
                                  uint64_t participant_token,
                                  uint32_t key_version, const uint8_t *key,
                                  bool enabled = true);
  ParticipantCredentialResult set_enabled(const char *participant_id,
                                          bool enabled);
  ParticipantCredentialResult erase(const char *participant_id);
  void clear();

  const ParticipantCredentialRecord *find(const char *participant_id) const;
  const ParticipantCredentialRecord *resolve(uint64_t participant_token,
                                             uint32_t key_version) const;
  const ParticipantCredentialRecord *resolve_provisioned(
      uint64_t participant_token, uint32_t key_version) const;
  const ParticipantCredentialRecord *record_at(std::size_t slot) const;
  std::size_t size() const;

 private:
  std::array<ParticipantCredentialRecord,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS>
      entries_{};
};

struct ParticipantCredentialSnapshot {
  ParticipantCredentialRegistry registry{};
  uint32_t generation{0};
};

enum class ParticipantCredentialCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_REGISTRY,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class ParticipantCredentialCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t HEADER_SIZE = 12;
  static constexpr std::size_t RECORD_HEADER_SIZE = 48;
  static constexpr std::size_t CHECKSUM_SIZE = 4;
  static constexpr std::size_t MAX_ENCODED_SIZE =
      HEADER_SIZE + ParticipantRegistry::MAX_REMOTE_PARTICIPANTS *
                        (RECORD_HEADER_SIZE + ParticipantRecord::MAX_ID_LENGTH) +
      CHECKSUM_SIZE;

  static ParticipantCredentialCodecResult encode(
      const ParticipantCredentialSnapshot &snapshot, uint8_t *output,
      std::size_t output_capacity, std::size_t *output_size);
  static ParticipantCredentialCodecResult encode_registry(
      const ParticipantCredentialRegistry &registry, uint32_t generation,
      uint8_t *output, std::size_t output_capacity, std::size_t *output_size);
  static ParticipantCredentialCodecResult decode(
      const uint8_t *data, std::size_t size,
      ParticipantCredentialSnapshot *snapshot);
};

struct ParticipantCredentialStorageBlob {
  // The fixed footer stores the actual encoded length outside the checksummed
  // credential payload so erased flash and padded bytes are unambiguous.
  std::array<uint8_t, ParticipantCredentialCodec::MAX_ENCODED_SIZE + 4> bytes{};
};

struct ParticipantCredentialCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

enum class ParticipantCredentialStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_REGISTRY,
};

class ParticipantCredentialStorageBackend {
 public:
  virtual ~ParticipantCredentialStorageBackend() = default;
  virtual bool load_participant_credential_slot(
      uint8_t slot, ParticipantCredentialStorageBlob *blob) = 0;
  virtual bool save_participant_credential_slot(
      uint8_t slot, const ParticipantCredentialStorageBlob &blob) = 0;
  virtual bool load_participant_credential_commit(
      ParticipantCredentialCommitBlob *blob) = 0;
  virtual bool save_participant_credential_commit(
      const ParticipantCredentialCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class ParticipantCredentialStore {
 public:
  ParticipantCredentialStoreResult load(
      ParticipantCredentialStorageBackend &backend,
      ParticipantCredentialSnapshot *snapshot);
  ParticipantCredentialStoreResult create(
      ParticipantCredentialStorageBackend &backend,
      const ParticipantCredentialRegistry &registry,
      ParticipantCredentialSnapshot *snapshot = nullptr);
  ParticipantCredentialStoreResult update(
      ParticipantCredentialStorageBackend &backend,
      const ParticipantCredentialRegistry &registry,
      ParticipantCredentialSnapshot *snapshot = nullptr);

 private:
  ParticipantCredentialStorageBlob blob_{};

  ParticipantCredentialStoreResult write_(
      ParticipantCredentialStorageBackend &backend, uint8_t slot,
      const ParticipantCredentialRegistry &registry, uint32_t generation);
  ParticipantCredentialStoreResult load_slot_(
      ParticipantCredentialStorageBackend &backend, uint8_t slot,
      ParticipantCredentialSnapshot *snapshot);
};

const char *participant_credential_store_result_to_string(
    ParticipantCredentialStoreResult result);

#ifdef USE_ESP32
ParticipantCredentialStoreResult initialize_esp32_participant_credentials(
    RadarStorage *storage, ParticipantCredentialSnapshot *snapshot);
bool get_esp32_participant_credential_persistence(
    ParticipantCredentialStorageBackend **backend,
    ParticipantCredentialStore **store);
#endif

static_assert(ParticipantCredentialCodec::MAX_ENCODED_SIZE == 2144,
              "participant credential storage bound changed unexpectedly");
static_assert(std::is_trivially_copyable<ParticipantCredentialRecord>::value,
              "participant credentials must not own heap-backed state");
static_assert(sizeof(ParticipantCredentialSnapshot) <= 2560,
              "credential snapshot exceeded the embedded memory budget");
static_assert(sizeof(ParticipantCredentialStore) <= 2304,
              "credential store workspace exceeded the embedded memory budget");

}  // namespace radar_api_server
}  // namespace esphome
