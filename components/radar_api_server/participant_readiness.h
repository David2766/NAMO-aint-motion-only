#pragma once

#include "participant_registry.h"
#include "spatial_transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class RadarStorage;

enum ParticipantCapability : uint16_t {
  PARTICIPANT_CAP_POINT_TRACKS = 1U << 0U,
  PARTICIPANT_CAP_VELOCITY = 1U << 1U,
  PARTICIPANT_CAP_POSITION_UNCERTAINTY = 1U << 2U,
  PARTICIPANT_CAP_RANGE = 1U << 3U,
  PARTICIPANT_CAP_PRESENCE = 1U << 4U,
  PARTICIPANT_CAP_MOTION = 1U << 5U,
  PARTICIPANT_CAP_ZONE_PRESENCE = 1U << 6U,
  PARTICIPANT_CAP_BRIDGE = 1U << 7U,
};

enum ParticipantReadinessMissing : uint8_t {
  PARTICIPANT_MISSING_NONE = 0,
  PARTICIPANT_MISSING_CAPABILITIES = 1U << 0U,
  PARTICIPANT_MISSING_ASSIGNMENT = 1U << 1U,
  PARTICIPANT_MISSING_POSE = 1U << 2U,
  PARTICIPANT_MISSING_CALIBRATION = 1U << 3U,
};

struct ParticipantReadinessConfig {
  static constexpr std::size_t MAX_ASSIGNMENT_ID_LENGTH = 40;

  uint16_t capability_flags{0};
  uint8_t max_targets{0};
  char floorplan_id[MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char space_id[MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char group_id[MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  uint32_t geometry_version{0};
  uint32_t local_pose_version{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  SpatialTransform2D transform{};
};

struct ParticipantReadinessRecord {
  bool occupied{false};
  char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  ParticipantReadinessConfig config{};
};

enum class ParticipantReadinessResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_CAPABILITIES,
  INVALID_ASSIGNMENT,
  NOT_FOUND,
  FULL,
};

class ParticipantReadinessRegistry {
 public:
  ParticipantReadinessResult configure(const char *participant_id,
                                       const ParticipantReadinessConfig &config);
  ParticipantReadinessResult forget(const char *participant_id);
  void clear();

  const ParticipantReadinessRecord *find(const char *participant_id) const;
  const ParticipantReadinessRecord *record_at(std::size_t slot) const;
  std::size_t size() const;

  static bool valid_config(const ParticipantReadinessConfig &config);
  static uint8_t missing_requirements(const ParticipantReadinessRecord *record);
  static bool ready(const ParticipantReadinessRecord *record) {
    return missing_requirements(record) == PARTICIPANT_MISSING_NONE;
  }

 private:
  std::array<ParticipantReadinessRecord,
             ParticipantRegistry::MAX_REMOTE_PARTICIPANTS> entries_{};

  ParticipantReadinessRecord *find_mutable_(const char *participant_id);
  static bool valid_id_(const char *value, std::size_t max_length);
  static void copy_(char *target, std::size_t capacity, const char *source);
};

struct ParticipantReadinessSnapshot {
  ParticipantReadinessRegistry registry{};
  uint32_t generation{0};
};

enum class SpatialReadinessMismatch : uint8_t {
  NONE = 0,
  FLOORPLAN_GEOMETRY,
  LOCAL_POSE,
  REMOTE_POSE,
};

SpatialReadinessMismatch spatial_readiness_mismatch(
    const ParticipantReadinessRecord *record, bool floorplan_committed,
    const char *floorplan_id, uint32_t geometry_version,
    uint32_t local_pose_version, uint32_t remote_pose_version);
const char *spatial_readiness_mismatch_to_string(
    SpatialReadinessMismatch mismatch);

enum class ParticipantReadinessStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  MIGRATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_DATA,
};

class ParticipantReadinessStorageBackend {
 public:
  static constexpr std::size_t MAX_ENCODED_SIZE = 4096;
  using StorageBlob = std::array<uint8_t, MAX_ENCODED_SIZE>;
  using CommitBlob = std::array<uint8_t, 16>;

  virtual ~ParticipantReadinessStorageBackend() = default;
  virtual bool load_participant_readiness_slot(uint8_t slot, StorageBlob *blob) = 0;
  virtual bool save_participant_readiness_slot(uint8_t slot,
                                               const StorageBlob &blob) = 0;
  virtual bool load_participant_readiness_commit(CommitBlob *blob) = 0;
  virtual bool save_participant_readiness_commit(const CommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class ParticipantReadinessStore {
 public:
  ParticipantReadinessStoreResult load(ParticipantReadinessStorageBackend &backend,
                                       ParticipantReadinessSnapshot *snapshot);
  ParticipantReadinessStoreResult create(ParticipantReadinessStorageBackend &backend,
                                         const ParticipantReadinessRegistry &registry,
                                         ParticipantReadinessSnapshot *snapshot = nullptr);
  ParticipantReadinessStoreResult update(ParticipantReadinessStorageBackend &backend,
                                         const ParticipantReadinessRegistry &registry,
                                         ParticipantReadinessSnapshot *snapshot = nullptr);
  ParticipantReadinessStoreResult load_or_migrate(
      ParticipantReadinessStorageBackend &backend,
      ParticipantReadinessStorageBackend *legacy_backend,
      ParticipantReadinessSnapshot *snapshot);

 private:
  ParticipantReadinessStorageBackend::StorageBlob blob_{};
  ParticipantReadinessSnapshot scratch_{};

  ParticipantReadinessStoreResult write_slot_(ParticipantReadinessStorageBackend &backend,
                                              uint8_t slot);
  ParticipantReadinessStoreResult load_committed_(ParticipantReadinessStorageBackend &backend,
                                                  uint8_t *active_slot);
  ParticipantReadinessStoreResult decode_blob_();
};

enum class ObservationAdmissionResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  UNKNOWN_PARTICIPANT,
  PARTICIPANT_INACTIVE,
  TYPE_MISMATCH,
  SITE_MISMATCH,
  GROUP_MISMATCH,
  CAPABILITY_VIOLATION,
  CONFIG_VERSION_MISMATCH,
  EXPIRED,
  DUPLICATE_OR_REORDERED,
};

struct ParticipantObservationMetadata {
  const char *participant_id{nullptr};
  ParticipantType participant_type{ParticipantType::NAMO_NODE};
  const char *site_id{nullptr};
  const char *group_id{nullptr};
  const char *boot_id{nullptr};
  uint32_t sequence{0};
  uint32_t sample_age_ms{0};
  uint32_t ttl_ms{0};
  uint32_t pose_version{0};
  uint32_t calibration_version{0};
  uint8_t track_count{0};
  bool presence{false};
  bool motion{false};
  bool has_velocity{false};
  bool has_position_uncertainty{false};
  bool has_range{false};
  bool has_zone_presence{false};
};

class ObservationAdmission {
 public:
  ObservationAdmissionResult admit(const ParticipantRegistry &participants,
                                   const ParticipantReadinessRegistry &readiness,
                                   const char *site_id,
                                   const ParticipantObservationMetadata &observation,
                                   const char *expected_group_id = nullptr);
  ObservationAdmissionResult admit(
      const ParticipantRecord *participant,
      const ParticipantReadinessRecord *readiness, const char *site_id,
      const ParticipantObservationMetadata &observation,
      const char *expected_group_id = nullptr);
  void clear();

 private:
  static constexpr std::size_t MAX_BOOT_ID_LENGTH = 48;
  struct SourceState {
    bool occupied{false};
    char participant_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
    char boot_id[MAX_BOOT_ID_LENGTH + 1]{};
    uint32_t sequence{0};
  };

  std::array<SourceState, ParticipantRegistry::MAX_REMOTE_PARTICIPANTS> sources_{};

  SourceState *source_for_(const char *participant_id);
};

const char *participant_readiness_store_result_to_string(
    ParticipantReadinessStoreResult result);
const char *observation_admission_result_to_string(ObservationAdmissionResult result);

#ifdef USE_ESP32
ParticipantReadinessStoreResult initialize_esp32_participant_readiness(
    RadarStorage *storage, ParticipantReadinessSnapshot *snapshot);
bool get_esp32_participant_readiness_persistence(
    ParticipantReadinessStorageBackend **backend, ParticipantReadinessStore **store);
#endif

static_assert(std::is_trivially_copyable<ParticipantReadinessRecord>::value,
              "participant readiness records must not own heap-backed state");
static_assert(std::is_trivially_copyable<ParticipantReadinessRegistry>::value,
              "participant readiness registry must not own heap-backed state");
static_assert(sizeof(ParticipantReadinessRegistry) <= 4352,
              "participant readiness registry exceeded the embedded budget");
static_assert(std::is_trivially_destructible<ParticipantReadinessStore>::value,
              "readiness store workspace must remain placement-new safe");
static_assert(sizeof(ParticipantReadinessStore) <= 8448,
              "participant readiness store workspace exceeded the embedded budget");
static_assert(sizeof(ObservationAdmission) <= 2304,
              "observation admission state exceeded the embedded budget");

}  // namespace radar_api_server
}  // namespace esphome
