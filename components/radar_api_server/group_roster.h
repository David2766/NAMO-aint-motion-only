#pragma once

#include "site_configuration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace esphome {
namespace radar_api_server {

class RadarStorage;

struct GroupRosterSnapshot {
  static constexpr std::size_t MAX_MEMBERS =
      MAX_AUTOMATIC_SPATIAL_GROUP_PARTICIPANTS;

  char site_id[PlatformIdentity::MAX_ID_LENGTH + 1]{};
  char group_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char space_id[ParticipantReadinessConfig::MAX_ASSIGNMENT_ID_LENGTH + 1]{};
  char leader_node_id[ParticipantRecord::MAX_ID_LENGTH + 1]{};
  uint32_t generation{0};
  uint32_t assignment_revision{0};
  bool committed{false};
  SiteFloorplanManifest floorplan{};
  std::array<SiteConfigurationMember, MAX_MEMBERS> members{};

  const SiteConfigurationMember *find(const char *participant_id) const;
  std::size_t member_count() const;
};

enum class GroupRosterResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_ROSTER,
  GROUP_NOT_FOUND,
  LEADER_NOT_FOUND,
  FULL,
};

bool valid_group_roster(const GroupRosterSnapshot &roster);
bool same_group_roster_payload(const GroupRosterSnapshot &left,
                               const GroupRosterSnapshot &right);
bool group_roster_update_allowed(const GroupRosterSnapshot &current,
                                 const GroupRosterSnapshot &candidate);
bool group_roster_matches_configuration_scope(
    const GroupRosterSnapshot &roster,
    const SiteConfigurationSnapshot &configuration, const char *group_id);
bool group_roster_current_for_local_node(
    const GroupRosterSnapshot &roster,
    const SiteConfigurationSnapshot &configuration,
    const char *local_participant_id);
const SiteConfigurationMember *select_group_roster_leader(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *site_leader_node_id, const char *directory_leader_node_id,
    const char *runtime_leader_node_id, const char *stored_leader_node_id);
GroupRosterResult prepare_group_roster_from_site_configuration(
    const SiteConfigurationSnapshot &configuration, const char *group_id,
    const char *leader_node_id, GroupRosterSnapshot *roster);
const char *group_roster_result_to_string(GroupRosterResult result);

enum class GroupRosterCodecResult : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  INVALID_ROSTER,
  EMPTY,
  CORRUPT,
  UNSUPPORTED_VERSION,
};

class GroupRosterCodec {
 public:
  static constexpr uint16_t STORAGE_VERSION = 1;
  static constexpr std::size_t MAX_ENCODED_SIZE = 4096;

  static GroupRosterCodecResult encode(const GroupRosterSnapshot &roster,
                                       uint8_t *output,
                                       std::size_t output_capacity,
                                       std::size_t *output_size,
                                       SiteConfigurationSnapshot *workspace);
  static GroupRosterCodecResult decode(const uint8_t *data, std::size_t size,
                                       GroupRosterSnapshot *roster,
                                       SiteConfigurationSnapshot *workspace);
};

struct GroupRosterStorageBlob {
  std::array<uint8_t, GroupRosterCodec::MAX_ENCODED_SIZE> bytes{};
};

struct GroupRosterCommitBlob {
  std::array<uint8_t, 16> bytes{};
};

class GroupRosterStorageBackend {
 public:
  virtual ~GroupRosterStorageBackend() = default;
  virtual bool load_group_roster_slot(uint8_t slot,
                                      GroupRosterStorageBlob *blob) = 0;
  virtual bool save_group_roster_slot(
      uint8_t slot, const GroupRosterStorageBlob &blob) = 0;
  virtual bool load_group_roster_commit(GroupRosterCommitBlob *blob) = 0;
  virtual bool save_group_roster_commit(
      const GroupRosterCommitBlob &blob) = 0;
  virtual bool sync() = 0;
};

enum class GroupRosterStoreResult : uint8_t {
  EMPTY = 0,
  LOADED,
  CREATED,
  MIGRATED,
  UPDATED,
  COMMIT_REPAIRED,
  CORRUPT,
  UNSUPPORTED_VERSION,
  STORAGE_FAILED,
  INVALID_ROSTER,
};

class GroupRosterStore {
 public:
  GroupRosterStoreResult load(GroupRosterStorageBackend &backend,
                              GroupRosterSnapshot *roster);
  GroupRosterStoreResult create(GroupRosterStorageBackend &backend,
                                const GroupRosterSnapshot &roster,
                                GroupRosterSnapshot *stored = nullptr);
  GroupRosterStoreResult update(GroupRosterStorageBackend &backend,
                                const GroupRosterSnapshot &roster,
                                GroupRosterSnapshot *stored = nullptr);
  GroupRosterStoreResult load_or_migrate(
      GroupRosterStorageBackend &backend,
      const SiteConfigurationSnapshot &configuration, const char *group_id,
      const char *leader_node_id, GroupRosterSnapshot *roster);

 private:
  GroupRosterStorageBlob blob_{};
  GroupRosterSnapshot scratch_{};
  std::array<GroupRosterSnapshot, 2> slot_scratch_{};
  SiteConfigurationSnapshot codec_scratch_{};

  GroupRosterStoreResult load_slot_(GroupRosterStorageBackend &backend,
                                    uint8_t slot,
                                    GroupRosterSnapshot *roster);
  GroupRosterStoreResult write_slot_(GroupRosterStorageBackend &backend,
                                     uint8_t slot,
                                     const GroupRosterSnapshot &roster);
};

const char *group_roster_store_result_to_string(GroupRosterStoreResult result);

#ifdef USE_ESP32
GroupRosterStoreResult initialize_esp32_group_roster(
    RadarStorage *storage, const SiteConfigurationSnapshot &configuration,
    const char *group_id, const char *leader_node_id,
    GroupRosterSnapshot *roster);
bool get_esp32_group_roster_persistence(GroupRosterStorageBackend **backend,
                                        GroupRosterStore **store);
#endif

static_assert(std::is_trivially_copyable<GroupRosterSnapshot>::value,
              "Group roster must not own heap-backed state");
static_assert(sizeof(GroupRosterSnapshot) <= 2048,
              "Group roster runtime state exceeded its PSRAM budget");
static_assert(sizeof(GroupRosterStore) <= 14 * 1024,
              "Group roster store workspace exceeded its PSRAM budget");

}  // namespace radar_api_server
}  // namespace esphome
