#pragma once

#include "group_coordinator.h"
#include "site_configuration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class SiteGroupSummaryCache;

struct SiteDirectoryLimits {
  static constexpr std::size_t MAX_FLOORPLANS = 8;
  static constexpr std::size_t MAX_GROUPS = 8;
  static constexpr std::size_t MAX_GROUP_PARTICIPANTS =
      MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS;
  static constexpr std::size_t MAX_GROUP_FOLLOWERS =
      MAX_GROUP_PARTICIPANTS - 1;
  static constexpr std::size_t MAX_SITE_GROUP_PARTICIPANT_REFERENCES =
      MAX_GROUPS * MAX_GROUP_PARTICIPANTS;
};

struct FloorplanDirectoryEntry {
  bool occupied{false};
  bool committed{false};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char owner_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t owner_generation{0};
  uint32_t geometry_version{0};
  uint32_t config_hash{0};
  uint32_t image_hash{0};
};

struct GroupDirectoryEntry {
  bool occupied{false};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char floorplan_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  char leader_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t assignment_revision{0};
  uint8_t member_count{0};
};

struct SiteFloorplanOwnerReport {
  const char *participant_id{nullptr};
  const char *floorplan_id{nullptr};
  uint32_t owner_generation{0};
  uint32_t geometry_version{0};
  uint32_t config_hash{0};
  uint32_t image_hash{0};
  uint32_t lease_ms{0};
};

struct SiteFloorplanOwnerState {
  bool occupied{false};
  FloorplanDirectoryEntry floorplan{};
  uint32_t lease_expires_at_ms{0};
};

enum class SiteFloorplanOwnerResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  PARTICIPANT_INACTIVE,
  FLOORPLAN_CONFLICT,
  OWNER_CONFLICT,
  STALE_GENERATION,
  REVISION_CONFLICT,
  FULL,
};

class SiteFloorplanOwnerCache {
 public:
  static constexpr std::size_t MAX_FLOORPLANS =
      SiteDirectoryLimits::MAX_FLOORPLANS;

  SiteFloorplanOwnerResult accept(
      const SiteConfigurationSnapshot &configuration,
      const SiteFloorplanOwnerReport &report, uint32_t now_ms);
  void reconcile(const SiteConfigurationSnapshot *configuration,
                 bool local_authority, uint32_t now_ms);
  void clear();

  const SiteFloorplanOwnerState *find(const char *floorplan_id) const;
  const SiteFloorplanOwnerState *owner_at(std::size_t slot) const;
  std::size_t size() const;
  uint32_t generation() const { return this->generation_; }
  uint32_t rejected() const { return this->rejected_; }
  uint32_t expired() const { return this->expired_; }

 private:
  std::array<SiteFloorplanOwnerState, MAX_FLOORPLANS> owners_{};
  uint32_t generation_{0};
  uint32_t rejected_{0};
  uint32_t expired_{0};

  SiteFloorplanOwnerState *find_mutable_(const char *floorplan_id);
  SiteFloorplanOwnerState *ensure_(const char *floorplan_id);
};

struct SiteDirectorySnapshot {
  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  uint32_t generation{0};
  uint32_t topology_revision{0};
  bool committed{false};
  bool compatibility_mirror{false};
  bool legacy_single_floorplan_projection{false};
  std::array<FloorplanDirectoryEntry, SiteDirectoryLimits::MAX_FLOORPLANS>
      floorplans{};
  std::array<GroupDirectoryEntry, SiteDirectoryLimits::MAX_GROUPS> groups{};

  std::size_t floorplan_count() const;
  std::size_t group_count() const;
  const FloorplanDirectoryEntry *find_floorplan(
      const char *floorplan_id) const;
  const GroupDirectoryEntry *find_group(const char *group_id) const;
};

enum class SiteDirectoryResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_DIRECTORY,
  NOT_READY,
  FULL,
  CONFLICT,
};

const char *site_directory_result_to_string(SiteDirectoryResult result);
bool valid_site_directory(const SiteDirectorySnapshot &directory);
bool site_directory_matches_configuration_scope(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory);
bool site_directory_is_canonical_for_configuration(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &directory);
bool same_site_directory_payload(const SiteDirectorySnapshot &left,
                                 const SiteDirectorySnapshot &right);
uint32_t site_directory_payload_hash(
    const SiteDirectorySnapshot &directory);
uint32_t group_directory_assignment_hash(
    const GroupDirectoryEntry &group);
SiteDirectoryResult migrate_site_configuration_to_directory(
    const SiteConfigurationSnapshot &configuration,
    SiteDirectorySnapshot *directory);
SiteDirectoryResult prepare_site_directory_compatibility_mirror(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot *current, SiteDirectorySnapshot *candidate,
    bool *update_required);
SiteDirectoryResult prepare_legacy_single_floorplan_projection(
    const SiteConfigurationSnapshot &configuration,
    const GroupCoordinator &coordinator,
    const SiteDirectorySnapshot &current, SiteDirectorySnapshot *candidate,
    bool *update_required);
SiteDirectoryResult prepare_site_directory_from_advertisements(
    const SiteConfigurationSnapshot &configuration,
    const SiteDirectorySnapshot &current,
    const SiteFloorplanOwnerCache &floorplan_owners,
    const SiteGroupSummaryCache &group_summaries,
    SiteDirectorySnapshot *candidate, bool *update_required);

enum class SiteDirectoryCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_DIRECTORY,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class SiteDirectoryCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t MAX_ENCODED_SIZE = 4096;

  static SiteDirectoryCodecResult encode(
      const SiteDirectorySnapshot &directory, uint8_t *output,
      std::size_t output_capacity, std::size_t *output_size);
  static SiteDirectoryCodecResult decode(
      const uint8_t *data, std::size_t size,
      SiteDirectorySnapshot *directory);
};

struct SiteDirectoryStorageBlob {
  std::array<uint8_t, SiteDirectoryCodec::MAX_ENCODED_SIZE> bytes{};
};

struct SiteDirectoryCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

enum class SiteDirectoryStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_DIRECTORY,
};

class SiteDirectoryStorageBackend {
 public:
  virtual ~SiteDirectoryStorageBackend() = default;
  virtual bool load_site_directory_slot(
      uint8_t slot, SiteDirectoryStorageBlob *blob) = 0;
  virtual bool save_site_directory_slot(
      uint8_t slot, const SiteDirectoryStorageBlob &blob) = 0;
  virtual bool load_site_directory_commit(
      SiteDirectoryCommitBlob *blob) = 0;
  virtual bool save_site_directory_commit(
      const SiteDirectoryCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

class SiteDirectoryStore {
 public:
  SiteDirectoryStoreResult load(
      SiteDirectoryStorageBackend &backend,
      SiteDirectorySnapshot *directory);
  SiteDirectoryStoreResult create(
      SiteDirectoryStorageBackend &backend,
      const SiteDirectorySnapshot &directory,
      SiteDirectorySnapshot *stored = nullptr);
  SiteDirectoryStoreResult update(
      SiteDirectoryStorageBackend &backend,
      const SiteDirectorySnapshot &directory,
      SiteDirectorySnapshot *stored = nullptr);

 private:
  SiteDirectoryStorageBlob blob_{};
  SiteDirectorySnapshot scratch_{};
  std::array<SiteDirectorySnapshot, 2> slot_scratch_{};

  SiteDirectoryStoreResult load_slot_(
      SiteDirectoryStorageBackend &backend, uint8_t slot,
      SiteDirectorySnapshot *directory);
  SiteDirectoryStoreResult write_slot_(
      SiteDirectoryStorageBackend &backend, uint8_t slot,
      const SiteDirectorySnapshot &directory);
};

const char *site_directory_store_result_to_string(
    SiteDirectoryStoreResult result);

#ifdef USE_ESP32
SiteDirectoryStoreResult initialize_esp32_site_directory(
    RadarStorage *storage, const SiteConfigurationSnapshot &configuration,
    SiteDirectorySnapshot *directory);
bool get_esp32_site_directory_persistence(
    SiteDirectoryStorageBackend **backend, SiteDirectoryStore **store);
#endif

static_assert(
    SiteDirectoryLimits::MAX_SITE_GROUP_PARTICIPANT_REFERENCES == 56,
    "aggregate Group participant-reference bound changed unexpectedly");
static_assert(SiteDirectoryLimits::MAX_GROUP_PARTICIPANTS ==
                  GroupMembershipView::MAX_PARTICIPANTS,
              "Site directory and Group runtime limits must stay aligned");
static_assert(std::is_trivially_copyable<SiteDirectorySnapshot>::value,
              "Site directory must not own heap-backed state");
static_assert(sizeof(SiteDirectorySnapshot) <= 3072,
              "Site directory runtime state exceeded its embedded budget");
static_assert(sizeof(SiteDirectoryStore) <= 13 * 1024,
              "Site directory store workspace exceeded its PSRAM budget");
static_assert(std::is_trivially_copyable<SiteFloorplanOwnerCache>::value,
              "Floorplan owner advertisements must remain fixed-size");
static_assert(sizeof(SiteFloorplanOwnerCache) <= 1280,
              "Floorplan owner cache exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
